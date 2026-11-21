// SPDX-License-Identifier: GPL-3.0-or-later
// spirv_subgroup32 (declaration and contract in nvn_spirv_patch.h): the game's 32-lane warp operations on a GPU whose subgroups are not
// 32 wide (wider: Adreno 64 / 128; narrower: Mali 16, where each subgroup is a warp of its own width and only the quad swaps change).
//
// Every aligned slice of 32 lanes of the host subgroup plays one NVIDIA warp. The translated code reads the lane as
// SubgroupLocalInvocationId (the shuffle helpers compute a source lane from it and test it against NV's 0..31 bounds), takes word 0 of a
// ballot or of SubgroupLtMask, and votes over the whole subgroup. Rewritten: a lane load gives lane & 31; a shuffle reads lane
// id | (lane & ~31); ballot and mask words are the word (lane >> 5); Any / All are ballot(p) != 0 / ballot(p) == ballot(true) in that
// word. This is what Ryujinx's translator writes itself when QueryHostSubgroupSize() is 64 or 128, made width-agnostic (the word is picked
// at run time, so a driver that runs a stage 64 or 128 wide gets the same answer).
// In a fragment shader on a GPU with quad operations, a shuffle whose source is another lane of the same quad (the FSWZADD derivative code,
// lane ^ 1, ^ 2, ^ 3) takes OpGroupNonUniformQuadSwap's value: Vulkan defines quads as 4 consecutive lanes laid out as NVIDIA's (0 top left,
// 1 top right, 2 bottom left, 3 bottom right) and helper invocations take part in quad operations, which a plain shuffle does not promise.
#include "nvn_spirv_patch.h"

#include <cstring>
#include <iterator>
#include <map>
#include <set>

namespace nxrt::nvn::be::gpu {
namespace {

enum : uint32_t {
    OpSourceExtension = 4, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19,
    OpTypeBool = 20, OpTypeInt = 21, OpTypeVector = 23, OpTypePointer = 32, OpTypeForwardPointer = 39, OpConstantTrue = 41,
    OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71,
    OpVectorExtractDynamic = 77, OpCompositeExtract = 81, OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpShiftRightLogical = 194,
    OpBitwiseOr = 197, OpBitwiseXor = 198, OpBitwiseAnd = 199,
    OpGroupNonUniformAll = 334, OpGroupNonUniformAny = 335, OpGroupNonUniformBallot = 339, OpGroupNonUniformShuffle = 345,
    OpGroupNonUniformQuadSwap = 366,
    DecorationBuiltIn = 11, DecorationFlat = 14, StorageInput = 1, CapabilityGroupNonUniformBallot = 64, CapabilityGroupNonUniformQuad = 68, ExecutionModeLocalSize = 17,
    ModelFragment = 4, ModelGLCompute = 5,
    BuiltInSubgroupSize = 36, BuiltInSubgroupMaxSize = 37, BuiltInNumSubgroups = 38, BuiltInSubgroupId = 40, BuiltInLane = 41,
    BuiltInEqMask = 4416, BuiltInLtMask = 4420,
};

constexpr char kMarker[] = "nxrt.subgroup32";   // OpSourceExtension of a rewritten module: a second pass leaves it alone

struct Inst {
    size_t at;
    uint32_t op, len;
};

bool decode(const std::vector<uint32_t>& w, std::vector<Inst>& insts) {
    for (size_t i = 5; i < w.size();) {
        const uint32_t len = w[i] >> 16;
        if (!len || i + len > w.size()) return false;
        insts.push_back({i, w[i] & 0xffff, len});
        i += len;
    }
    return true;
}

bool is_type_or_constant(uint32_t op) { return (op >= OpTypeVoid && op <= OpTypeForwardPointer) || (op >= 41 && op <= 52); }

// Word k of a function-body instruction is a literal, not an id (the literals the translator's code carries)
bool is_literal(uint32_t op, uint32_t k) {
    switch (op) {
    case 8: return k >= 2;                                       // OpLine
    case 12: return k == 4;                                      // OpExtInst: the instruction number
    case OpLoad: return k >= 4;                                  // memory operands
    case 62: return k >= 3;                                      // OpStore
    case 79: return k >= 5;                                      // OpVectorShuffle components
    case OpCompositeExtract: return k >= 4;
    case 82: return k >= 5;                                      // OpCompositeInsert
    case 246: return k >= 3;                                     // OpLoopMerge
    case 247: return k == 2;                                     // OpSelectionMerge
    case 250: return k >= 4;                                     // OpBranchConditional weights
    case 251: return k >= 3 && (k & 1);                          // OpSwitch: literal, label pairs
    default: return false;
    }
}

// The translator's SHFL helpers (Ryujinx HelperFunctionManager: Shuffle, ShuffleXor, ...) inlined into their callers and removed.
// Adreno's compiler (driver 512.762.1) crashes on an OpGroupNonUniformShuffle in any function other than the entry point; in the entry
// point it compiles. Every helper is one block of value ops ending in OpReturnValue; its OpVariables move to the caller's first block.
// 0: nothing to inline; -1: a shuffle in a function of another shape (the module is left alone); otherwise the calls inlined.
int inline_shuffle_helpers(const std::vector<uint32_t>& in, const std::vector<Inst>& insts, std::vector<uint32_t>& out) {
    struct Fn {
        size_t begin, end;          // instruction indices: OpFunction .. OpFunctionEnd
        bool shuffle = false, simple = true;
        std::vector<uint32_t> params;
    };
    std::vector<Fn> fns;
    uint32_t entry_fn = 0;
    for (size_t i = 0; i < insts.size(); i++) {
        const Inst& n = insts[i];
        if (n.op == OpEntryPoint) entry_fn = in[n.at + 2];
        if (n.op == OpFunction) fns.push_back(Fn{i, i, false, true, {}});
        if (fns.empty() || fns.back().end != fns.back().begin) continue;
        Fn& f = fns.back();
        switch (n.op) {
        case 56: f.end = i; break;                                                    // OpFunctionEnd
        case 55: f.params.push_back(in[n.at + 2]); break;                             // OpFunctionParameter
        case OpGroupNonUniformShuffle: f.shuffle = true; break;
        case OpFunction: case 248: case OpVariable: case OpLoad: case 62: case 124: case 128: case 130: case OpSelect: case 170: case 171:
        case 172: case 173: case 174: case 175: case 176: case 177: case 178: case 194: case 195: case 196: case OpBitwiseOr:
        case OpBitwiseXor: case OpBitwiseAnd: case 200: break;
        case 254: f.simple &= insts[i + 1].op == 56; break;                           // OpReturnValue, last
        default: f.simple = false;
        }
    }
    std::map<uint32_t, const Fn*> helpers;
    for (const Fn& f : fns) {
        if (!f.shuffle) continue;
        const uint32_t id = in[insts[f.begin].at + 2];
        int labels = 0;
        for (size_t i = f.begin; i <= f.end; i++) labels += insts[i].op == 248;
        if (id == entry_fn || !f.simple || labels != 1 || insts[f.end - 1].op != 254) return -1;
        helpers[id] = &f;
    }
    if (helpers.empty()) return 0;
    std::set<uint32_t> gone;                   // ids of the removed functions: their names and decorations go too
    for (const auto& [id, f] : helpers) {
        gone.insert(id);
        for (size_t i = f->begin; i <= f->end; i++) {
            const Inst& n = insts[i];
            if (n.op != 62 && n.op != 254 && n.op != 56 && n.len >= 3) gone.insert(in[n.at + 2]);
        }
    }
    uint32_t bound = in[3];
    int calls = 0;
    out.assign(in.begin(), in.begin() + 5);
    const Fn* skip = nullptr;
    std::vector<uint32_t> body, vars;          // the current caller's code after its leading OpVariables, and the moved variables
    bool in_fn = false, leading = false;
    auto flush = [&] {
        out.insert(out.end(), vars.begin(), vars.end());
        out.insert(out.end(), body.begin(), body.end());
        vars.clear(), body.clear();
    };
    for (size_t i = 0; i < insts.size(); i++) {
        const Inst& n = insts[i];
        const uint32_t* w = &in[n.at];
        if (skip) {
            if (i == skip->end) skip = nullptr;
            continue;
        }
        if (n.op == OpFunction && helpers.count(w[2])) {
            skip = helpers[w[2]];
            continue;
        }
        if ((n.op == 5 || n.op == OpDecorate || n.op == 72) && gone.count(w[1])) continue;   // OpName, OpDecorate, OpMemberDecorate
        if (n.op == OpFunction) in_fn = true, leading = false;
        if (!in_fn) {
            out.insert(out.end(), w, w + n.len);
            continue;
        }
        if (n.op == 248 && !leading && body.empty()) {       // the first block's label: leading OpVariables follow
            out.insert(out.end(), w, w + n.len);
            leading = true;
            continue;
        }
        if (leading && n.op == OpVariable) {
            out.insert(out.end(), w, w + n.len);
            continue;
        }
        if (n.op != OpFunction && n.op != 55) leading = false;
        if (n.op == 57 && helpers.count(w[3])) {            // OpFunctionCall %type %result %helper args...
            const Fn& f = *helpers[w[3]];
            std::map<uint32_t, uint32_t> map;
            for (size_t k = 0; k < f.params.size() && 4 + k < n.len; k++) map[f.params[k]] = w[4 + k];
            for (size_t j = f.begin + 1; j < f.end; j++) {
                const Inst& m = insts[j];
                if (m.op != 62 && m.op != 254 && m.op != 55 && m.op != 248 && m.len >= 3) map[in[m.at + 2]] = bound++;
            }
            for (size_t j = f.begin + 1; j < f.end; j++) {
                const Inst& m = insts[j];
                const uint32_t* v = &in[m.at];
                if (m.op == 55 || m.op == 248) continue;
                std::vector<uint32_t>& dst = m.op == OpVariable ? vars : body;
                if (m.op == 254) {                           // the call's result
                    auto it = map.find(v[1]);
                    dst.insert(dst.end(), {(4u << 16) | 83u, w[1], w[2], it == map.end() ? v[1] : it->second});   // OpCopyObject
                    continue;
                }
                const size_t at = dst.size();
                dst.insert(dst.end(), v, v + m.len);
                for (uint32_t k = 1; k < m.len; k++) {
                    if (is_literal(m.op, k)) continue;
                    auto it = map.find(dst[at + k]);
                    if (it != map.end()) dst[at + k] = it->second;
                }
            }
            calls++;
            continue;
        }
        if (n.op == 56) {
            body.insert(body.end(), w, w + n.len);
            flush();
            in_fn = false;
            continue;
        }
        if (n.op == OpFunction || n.op == 55) {
            out.insert(out.end(), w, w + n.len);
            continue;
        }
        body.insert(body.end(), w, w + n.len);
    }
    out[3] = bound;
    return calls ? calls : -1;
}

bool rewrite(const std::vector<uint32_t>& in, bool (*runs_wide)(uint32_t, uint64_t), bool quad_ops, std::vector<uint32_t>& out,
             Subgroup32Rewrite& st);

}  // namespace

bool spirv_subgroup32(const std::vector<uint32_t>& in, bool (*runs_wide)(uint32_t execution_model, uint64_t invocations), bool quad_ops,
                      std::vector<uint32_t>& out, Subgroup32Rewrite* stats) {
    Subgroup32Rewrite st;
    if (stats) *stats = st;
    std::vector<Inst> insts;
    if (in.size() < 5 || !decode(in, insts)) return false;
    std::vector<uint32_t> inlined;
    const int calls = inline_shuffle_helpers(in, insts, inlined);
    if (calls < 0) return false;
    if (!rewrite(calls ? inlined : in, runs_wide, quad_ops, out, st)) return false;
    st.inlined = calls;
    if (stats) *stats = st;
    return true;
}

namespace {
bool rewrite(const std::vector<uint32_t>& in, bool (*runs_wide)(uint32_t, uint64_t), bool quad_ops, std::vector<uint32_t>& out,
             Subgroup32Rewrite& st) {
    std::vector<Inst> insts;
    if (!decode(in, insts)) return false;

    uint32_t model = ~0u, lane = 0, t_uint = 0, t_bool = 0, t_v4 = 0, p_in_uint = 0, c_true = 0;
    uint64_t invocations = 0;
    int entries = 0;
    size_t first_type = 0, first_function = 0, entry_at = 0;
    bool has_quad_cap = false, has_ballot_cap = false, sg_ops = false;
    std::set<uint32_t> masks;                    // SubgroupEq/Ge/Gt/Le/LtMask variables
    std::map<uint32_t, uint32_t> uconst;         // value -> id of a 32-bit unsigned OpConstant
    std::map<uint32_t, uint32_t> int_value;      // id -> value of any 32-bit integer OpConstant
    std::set<uint32_t> int_types, ballots;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!first_type && is_type_or_constant(n.op)) first_type = n.at;
        if (!first_function && n.op == OpFunction) first_function = n.at;
        switch (n.op) {
        case OpSourceExtension:
            if (n.len >= 2 && std::strncmp(reinterpret_cast<const char*>(w + 1), kMarker, (n.len - 1) * 4) == 0) return false;
            break;
        case OpCapability:
            has_quad_cap |= w[1] == CapabilityGroupNonUniformQuad;
            has_ballot_cap |= w[1] == CapabilityGroupNonUniformBallot;
            break;
        case OpEntryPoint: entries++, model = w[1], entry_at = n.at; break;
        case OpExecutionMode:
            if (n.len >= 6 && w[2] == ExecutionModeLocalSize) invocations = uint64_t{w[3]} * w[4] * w[5];
            break;
        case OpDecorate:
            if (n.len >= 4 && w[2] == DecorationBuiltIn) {
                const uint32_t b = w[3];
                if (b == BuiltInLane) lane = w[1];
                else if (b >= BuiltInEqMask && b <= BuiltInLtMask) masks.insert(w[1]);
                else if (b == BuiltInSubgroupSize || b == BuiltInSubgroupMaxSize || b == BuiltInNumSubgroups || b == BuiltInSubgroupId)
                    return false;   // reads the subgroup's shape itself: not the translator's code
            }
            break;
        case OpTypeBool: t_bool = w[1]; break;
        case OpTypeInt:
            if (n.len == 4 && w[2] == 32) {
                int_types.insert(w[1]);
                if (w[3] == 0) t_uint = w[1];
            }
            break;
        case OpTypeVector:
            if (n.len == 4 && w[2] == t_uint && t_uint && w[3] == 4) t_v4 = w[1];
            break;
        case OpTypePointer:
            if (n.len == 4 && w[2] == StorageInput && w[3] == t_uint && t_uint) p_in_uint = w[1];
            break;
        case OpConstantTrue:
            if (w[1] == t_bool) c_true = w[2];
            break;
        case OpConstant:
            if (n.len == 4 && int_types.count(w[1])) {
                int_value[w[2]] = w[3];
                if (w[1] == t_uint && !uconst.count(w[3])) uconst[w[3]] = w[2];
            }
            break;
        case OpGroupNonUniformBallot: ballots.insert(w[2]); [[fallthrough]];
        case OpGroupNonUniformAll:
        case OpGroupNonUniformAny:
        case OpGroupNonUniformShuffle: sg_ops = true; break;
        default:
            if (n.op >= 333 && n.op <= 366) return false;   // another subgroup operation: the translator writes none
        }
    }
    if (entries != 1 || !first_type || !first_function || (!sg_ops && !lane && masks.empty())) return false;
    if (!runs_wide(model, invocations)) return false;
    const bool frag_quads = quad_ops && model == ModelFragment;

    // Every use the rewrite does not understand leaves the module alone: a ballot read other than word 0, the lane or a mask variable
    // used other than by a load / an access chain to word 0. Only function bodies are checked (the entry point, names and decorations
    // name the variables too).
    std::set<size_t> extract_at, chain_at;
    for (const Inst& n : insts) {
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        for (uint32_t k = 1; k < n.len; k++) {
            if (is_literal(n.op, k)) continue;
            const uint32_t id = w[k];
            if (ballots.count(id)) {
                if (n.op == OpGroupNonUniformBallot && k == 2) continue;
                if (n.op == OpCompositeExtract && k == 3 && n.len == 5 && w[4] == 0) { extract_at.insert(n.at); continue; }
                return false;
            }
            if (id == lane && lane) {
                if (n.op == OpLoad && k == 3) continue;
                return false;
            }
            if (masks.count(id)) {
                if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && k == 3 && n.len == 5 && int_value.count(w[4]) &&
                    int_value[w[4]] == 0) {
                    chain_at.insert(n.at);
                    continue;
                }
                return false;
            }
        }
    }

    // declarations the rewrite needs, added before the first function
    uint32_t bound = in[3];
    std::vector<uint32_t> decls, decos;
    auto need_uint = [&] {
        if (!t_uint) t_uint = bound++, decls.insert(decls.end(), {(4u << 16) | OpTypeInt, t_uint, 32u, 0u});
        return t_uint;
    };
    auto uc = [&](uint32_t v) {
        auto it = uconst.find(v);
        if (it != uconst.end()) return it->second;
        const uint32_t t = need_uint(), id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpConstant, t, id, v});
        uconst[v] = id;
        return id;
    };
    auto need_bool = [&] {
        if (!t_bool) t_bool = bound++, decls.insert(decls.end(), {(2u << 16) | OpTypeBool, t_bool});
        return t_bool;
    };
    auto need_v4 = [&] {
        if (!t_v4) t_v4 = bound++, decls.insert(decls.end(), {(4u << 16) | OpTypeVector, t_v4, need_uint(), 4u});
        return t_v4;
    };
    auto need_true = [&] {
        if (!c_true) {
            const uint32_t b = need_bool();
            c_true = bound++, decls.insert(decls.end(), {(3u << 16) | OpConstantTrue, b, c_true});
        }
        return c_true;
    };
    bool new_lane = false;
    auto need_lane = [&] {
        if (!lane) {
            const uint32_t t = need_uint();
            if (!p_in_uint) p_in_uint = bound++, decls.insert(decls.end(), {(4u << 16) | OpTypePointer, p_in_uint, StorageInput, t});
            lane = bound++, new_lane = true;
            decls.insert(decls.end(), {(4u << 16) | OpVariable, p_in_uint, lane, StorageInput});
            decos.insert(decos.end(), {(4u << 16) | OpDecorate, lane, DecorationBuiltIn, BuiltInLane});
            if (model == ModelFragment) decos.insert(decos.end(), {(3u << 16) | OpDecorate, lane, DecorationFlat});
        }
        return lane;
    };
    // the raw lane and its 32-lane word (lane >> 5), emitted in front of the instruction being rewritten
    std::vector<uint32_t> body;
    auto emit = [&](std::initializer_list<uint32_t> words) { body.insert(body.end(), words); };
    auto raw_lane = [&] {
        const uint32_t id = bound++;
        emit({(4u << 16) | OpLoad, need_uint(), id, need_lane()});
        return id;
    };
    auto lane_word = [&] {
        const uint32_t raw = raw_lane(), id = bound++;
        emit({(5u << 16) | OpShiftRightLogical, t_uint, id, raw, uc(5)});
        return id;
    };
    auto my_word = [&](uint32_t v4) {   // word (lane >> 5) of a uvec4 subgroup mask
        const uint32_t wd = lane_word(), id = bound++;
        emit({(5u << 16) | OpVectorExtractDynamic, t_uint, id, v4, wd});
        return id;
    };
    auto ballot = [&](uint32_t scope, uint32_t pred) {
        const uint32_t id = bound++;
        emit({(5u << 16) | OpGroupNonUniformBallot, need_v4(), id, scope, pred});
        return id;
    };

    bool used_quad = false;
    for (const Inst& n : insts) {
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        if (n.op == OpLoad && w[3] == lane) {
            // the lane as NVIDIA numbers it: lane & 31
            const uint32_t raw = bound++;
            emit({(n.len << 16) | OpLoad, w[1], raw});
            body.insert(body.end(), w + 3, w + n.len);
            emit({(5u << 16) | OpBitwiseAnd, w[1], w[2], raw, uc(31)});
            st.lane_loads++;
        } else if (n.op == OpGroupNonUniformShuffle && n.len == 6) {
            // the source lane in this lane's slice of 32: id | (lane & ~31). The helpers' id is 0..31 whenever their `valid` holds (an id
            // past 31 reads an undefined value they throw away, as before). Not (id & 31): Adreno 512.762.1 fails to link some compute
            // shaders with that index ("Failed to link shaders").
            const uint32_t type = w[1], res = w[2], scope = w[3], value = w[4], src = w[5];
            const uint32_t raw = raw_lane(), hi = bound++, idx = bound++;
            emit({(5u << 16) | OpBitwiseAnd, t_uint, hi, raw, uc(~31u)});
            emit({(5u << 16) | OpBitwiseOr, t_uint, idx, src, hi});
            if (!frag_quads) {
                emit({(6u << 16) | OpGroupNonUniformShuffle, type, res, scope, value, idx});
            } else {
                // d = source ^ lane within the warp: 1, 2, 3 is the quad neighbour QuadSwap direction d - 1 reads
                const uint32_t l31 = bound++, d = bound++, any = bound++;
                emit({(5u << 16) | OpBitwiseAnd, t_uint, l31, raw, uc(31)});
                emit({(5u << 16) | OpBitwiseXor, t_uint, d, src, l31});
                emit({(6u << 16) | OpGroupNonUniformShuffle, type, any, scope, value, idx});
                uint32_t acc = any;
                for (uint32_t dir = 3; dir >= 1; dir--) {
                    const uint32_t q = bound++, eq = bound++, sel = dir == 1 ? res : bound++;
                    emit({(6u << 16) | OpGroupNonUniformQuadSwap, type, q, scope, value, uc(dir - 1)});
                    emit({(5u << 16) | OpIEqual, need_bool(), eq, d, uc(dir)});
                    emit({(6u << 16) | OpSelect, type, sel, eq, q, acc});
                    acc = sel;
                }
                used_quad = true;
                st.quad_shuffles++;
            }
            st.shuffles++;
        } else if (extract_at.count(n.at)) {
            const uint32_t wd = lane_word();
            emit({(5u << 16) | OpVectorExtractDynamic, w[1], w[2], w[3], wd});
            st.ballots++;
        } else if (chain_at.count(n.at)) {
            const uint32_t wd = lane_word();
            emit({(5u << 16) | n.op, w[1], w[2], w[3], wd});
            st.masks++;
        } else if ((n.op == OpGroupNonUniformAny || n.op == OpGroupNonUniformAll) && n.len == 5) {
            const uint32_t type = w[1], res = w[2], scope = w[3], pred = w[4];
            const uint32_t mine = my_word(ballot(scope, pred));
            if (n.op == OpGroupNonUniformAny) {
                emit({(5u << 16) | OpINotEqual, type, res, mine, uc(0)});
            } else {
                const uint32_t active = my_word(ballot(scope, need_true()));
                emit({(5u << 16) | OpIEqual, type, res, mine, active});
            }
            st.votes++;
        } else {
            body.insert(body.end(), w, w + n.len);
        }
    }
    if (!st.lane_loads && !st.shuffles && !st.ballots && !st.masks && !st.votes) return false;

    out.clear();
    out.reserve(first_function + body.size() + decls.size() + decos.size() + 16);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    if (used_quad && !has_quad_cap) out.insert(out.end(), {(2u << 16) | OpCapability, CapabilityGroupNonUniformQuad});
    if (st.votes && !has_ballot_cap) out.insert(out.end(), {(2u << 16) | OpCapability, CapabilityGroupNonUniformBallot});
    bool marked = false;
    for (const Inst& n : insts) {
        if (n.at >= first_function) break;
        // the marker opens the debug section: the first instruction after capabilities .. execution modes
        if (!marked && n.op != OpCapability && n.op != 10 && n.op != 11 && n.op != 14 && n.op != OpEntryPoint && n.op != OpExecutionMode &&
            n.op != 331) {
            uint32_t m[1 + (sizeof kMarker + 3) / 4] = {};
            m[0] = (static_cast<uint32_t>(sizeof m / 4) << 16) | OpSourceExtension;
            std::memcpy(&m[1], kMarker, sizeof kMarker);
            out.insert(out.end(), std::begin(m), std::end(m));
            marked = true;
        }
        if (n.at == first_type) out.insert(out.end(), decos.begin(), decos.end());
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.at == entry_at && new_lane) {   // the interface list is the tail of OpEntryPoint
            out.push_back(lane);
            out[out.size() - n.len - 1] += 1u << 16;
        }
    }
    out.insert(out.end(), decls.begin(), decls.end());
    out.insert(out.end(), body.begin(), body.end());
    out[3] = bound;
    return true;
}
}  // namespace

}  // namespace nxrt::nvn::be::gpu
