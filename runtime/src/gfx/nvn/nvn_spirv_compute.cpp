// SPDX-License-Identifier: GPL-3.0-or-later
// SPIR-V rewrites of single compute programs, chosen by module hash at pipeline creation ([nvn] cs_vote, [nvn] cs_retile; nvn_pipeline.cpp). Each
// one checks the structure it relies on and returns false (no variant) when the module is not what it expects. The shader cache is
// not touched: the variant exists only as the module of a separate pipeline.
#include "nvn_spirv_patch.h"

#include <map>
#include <cstddef>

namespace nxrt::nvn::be::gpu {
namespace {
enum : uint32_t {
    OpExecutionMode = 16, OpTypeBool = 20, OpTypeInt = 21, OpConstantFalse = 42, OpConstant = 43, OpVariable = 59, OpLoad = 61,
    OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpCopyObject = 83, OpIAdd = 128, OpIMul = 132, OpBitcast = 124,
    OpControlBarrier = 224, OpMemoryBarrier = 225, OpNop = 0, OpGroupNonUniformAll = 334,
    ModeLocalSize = 17, DecBuiltIn = 11, BuiltInWorkgroupId = 26, BuiltInLocalInvocationId = 27, StorageWorkgroup = 4,
};
struct Ins { size_t at; uint32_t op, len; };
bool decode(const std::vector<uint32_t>& w, std::vector<Ins>& out) {
    if (w.size() < 5) return false;
    for (size_t i = 5; i < w.size();) {
        const uint32_t len = w[i] >> 16;
        if (!len || i + len > w.size()) return false;
        out.push_back({i, w[i] & 0xffff, len});
        i += len;
    }
    return true;
}
// Does instruction `op` have a result id at word 2 (type at word 1)? Enough for the instructions these rewrites look at.
bool has_type_and_result(uint32_t op) {
    return op == OpConstant || op == OpLoad || op == OpAccessChain || op == OpInBoundsAccessChain || op == OpBitcast || op == OpIAdd ||
           op == OpIMul || op == OpCopyObject || op == OpVariable || op == OpConstantFalse;
}
}  // namespace

bool spirv_retile(const std::vector<uint32_t>& in, uint32_t tx, uint32_t ty, std::vector<uint32_t>& out, uint32_t* fx, uint32_t* fy) {
    std::vector<Ins> ins;
    if (!tx || !ty || !decode(in, ins)) return false;
    size_t mode_at = 0;
    uint32_t sx = 0, sy = 0, wid = 0, lid = 0;
    std::map<uint32_t, uint32_t> konst;     // OpConstant id -> value (32-bit)
    std::map<uint32_t, size_t> konst_at;    // OpConstant id -> word index
    std::map<uint32_t, uint32_t> konst_type;
    for (const Ins& n : ins) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpExecutionMode && n.len == 6 && w[2] == ModeLocalSize) {
            if (mode_at) return false;
            mode_at = n.at;
            sx = w[3]; sy = w[4];
            if (w[5] != 1) return false;
        } else if (n.op == OpDecorate && n.len == 4 && w[2] == DecBuiltIn) {
            if (w[3] == BuiltInWorkgroupId) wid = w[1];
            else if (w[3] == BuiltInLocalInvocationId) lid = w[1];
            else return false;      // any other built-in (invocation index, group count, subgroup ids) could see the shape
        } else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageWorkgroup) return false;   // shared memory
        else if (n.op == OpControlBarrier || n.op == OpMemoryBarrier || (n.op >= 333 && n.op <= 366)) return false;   // barriers, subgroups
        else if (n.op == OpConstant && n.len == 4) { konst[w[2]] = w[3]; konst_at[w[2]] = n.at; konst_type[w[2]] = w[1]; }
    }
    if (!mode_at || !wid || !lid || sx % tx || sy % ty || (sx == tx && sy == ty)) return false;
    // uses of every id in the function bodies (instruction index). Literal operands can look like ids; such a false use only makes the
    // checks below fail closed (no variant). The built-ins' declarations (entry point, decorations, names) come before the bodies.
    std::map<uint32_t, std::vector<size_t>> users;
    bool body = false;
    for (size_t k = 0; k < ins.size(); ++k) {
        const Ins& n = ins[k];
        body = body || n.op == 54;   // OpFunction
        if (!body) continue;
        const size_t first = has_type_and_result(n.op) ? 3 : 1;
        for (size_t j = first; j < n.len; ++j) {
            // operands that are literals, not ids: OpExtInst's instruction number, extract / shuffle indices, merge masks
            if ((n.op == 12 && j == 4) || (n.op == 81 && j >= 4) || (n.op == 79 && j >= 5) || ((n.op == 246 || n.op == 247) && j >= 2)) continue;
            users[in[n.at + j]].push_back(k);
        }
    }
    auto only_user = [&](uint32_t id) -> const Ins* { const auto it = users.find(id); return it != users.end() && it->second.size() == 1 ? &ins[it->second[0]] : nullptr; };
    // component c of a built-in: access chain -> load -> (bitcast) -> one user
    auto component_value = [&](const Ins& chain, uint32_t* comp, uint32_t* val) {
        const uint32_t* w = &in[chain.at];
        if ((chain.op != OpAccessChain && chain.op != OpInBoundsAccessChain) || chain.len != 5 || !konst.count(w[4])) return false;
        *comp = konst[w[4]];
        const Ins* ld = only_user(w[2]);
        if (!ld || ld->op != OpLoad) return false;
        uint32_t v = in[ld->at + 2];
        const Ins* bc = only_user(v);
        if (bc && bc->op == OpBitcast) v = in[bc->at + 2];
        *val = v;
        return true;
    };
    // every use of the two built-ins is a component access; WorkgroupId.c * size.c + LocalInvocationId.c, nothing else
    std::map<uint32_t, size_t> mul_at;            // component -> word index of the OpIMul
    std::map<uint32_t, uint32_t> mul_result, mul_const, lid_value;
    for (uint32_t var : {wid, lid}) {
        for (size_t k : users[var]) {
            const Ins& n = ins[k];
            uint32_t c = 0, v = 0;
            if (!component_value(n, &c, &v) || c > 1) return false;
            const Ins* u = only_user(v);
            if (!u) return false;
            const uint32_t* uw = &in[u->at];
            if (var == wid) {
                if (u->op != OpIMul || u->len != 5) return false;
                const uint32_t other = uw[3] == v ? uw[4] : uw[3];
                if (!konst.count(other) || konst[other] != (c == 0 ? sx : sy) || mul_at.count(c)) return false;
                mul_at[c] = u->at;
                mul_result[c] = uw[2];
                mul_const[c] = other;
            } else {
                if (u->op != OpIAdd || u->len != 5 || lid_value.count(c)) return false;
                lid_value[c] = v;
                // the add's other operand must be the matching workgroup product
                const uint32_t other = uw[3] == v ? uw[4] : uw[3];
                mul_result[10 + c] = other;
            }
        }
    }
    for (uint32_t c = 0; c < 2; ++c)
        if (!mul_at.count(c) || !lid_value.count(c) || mul_result[c] != mul_result[10 + c] || !only_user(mul_result[c])) return false;
    // the multiplier constants may be used elsewhere (a logical stride): new constants for the new sizes
    out = in;
    uint32_t bound = out[3];
    const uint32_t cx = bound++, cy = bound++;
    out[3] = bound;
    out[mode_at + 3] = tx;
    out[mode_at + 4] = ty;
    out[mul_at[0] + (in[mul_at[0] + 3] == mul_const[0] ? 3 : 4)] = cx;
    out[mul_at[1] + (in[mul_at[1] + 3] == mul_const[1] ? 3 : 4)] = cy;
    const size_t after = konst_at[mul_const[0]] + 4;   // the new constants right after the old one (types and constants section)
    const uint32_t type = konst_type[mul_const[0]];
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(after), {(4u << 16) | OpConstant, type, cx, tx, (4u << 16) | OpConstant, type, cy, ty});
    *fx = sx / tx;
    *fy = sy / ty;
    return true;
}

bool spirv_vote_all_false(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    std::vector<Ins> ins;
    if (!decode(in, ins)) return false;
    size_t vote = 0;
    uint32_t bool_type = 0, false_id = 0;
    for (const Ins& n : ins) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpGroupNonUniformAll && n.len == 5) {
            if (vote) return false;                    // exactly one vote
            vote = n.at;
        } else if (n.op == OpTypeBool && n.len == 2) bool_type = w[1];
        else if (n.op == OpConstantFalse && n.len == 3) false_id = w[2];
    }
    if (!vote || !bool_type || in[vote + 1] != bool_type) return false;
    out = in;
    // OpGroupNonUniformAll %bool %scope %pred (5 words) -> OpCopyObject %bool %false (4 words) + OpNop
    size_t bool_at = 0;
    for (const Ins& n : ins) if (n.op == OpTypeBool && n.len == 2) bool_at = n.at;
    const bool add_false = !false_id;
    if (add_false) false_id = out[3]++;
    out[vote] = (4u << 16) | OpCopyObject;
    out[vote + 3] = false_id;
    out[vote + 4] = (1u << 16) | OpNop;
    if (add_false)   // new: right after the bool type
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(bool_at + 2), {(3u << 16) | OpConstantFalse, bool_type, false_id});
    return true;
}

}  // namespace nxrt::nvn::be::gpu
