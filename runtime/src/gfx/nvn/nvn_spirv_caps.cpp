// SPDX-License-Identifier: GPL-3.0-or-later
// spirv_drop_clip_cull, spirv_readonly_vertex_buffers (declarations and contracts in nvn_spirv_patch.h): modules made to fit a GPU that lacks
// a feature of the vertex pipeline (Mali-G710: shaderClipDistance, shaderCullDistance and vertexPipelineStoresAndAtomics are all 0).
//
// The translator declares gl_PerVertex { Position, PointSize, ClipDistance[], CullDistance[] } in every vertex, tessellation and geometry
// module, without the ClipDistance / CullDistance capability (a declared member nobody writes needs none). A module may write
// ClipDistance (user clip planes from a constant buffer); the patch assumes no module writes CullDistance or reads either as an input.
// Storage buffers in those stages are declared like any other (Uniform + BufferBlock), without NonWritable; the ones that are only read
// need it when vertexPipelineStoresAndAtomics is off (VUID-RuntimeSpirv-NonWritable-06341).
#include "nvn_spirv_patch.h"

#include <map>
#include <set>

namespace nxrt::nvn::be::gpu {
namespace {

enum : uint32_t {
    OpEntryPoint = 15, OpTypeVoid = 19, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32,
    OpTypeForwardPointer = 39, OpConstant = 43, OpFunction = 54, OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyMemory = 63,
    OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72, OpAtomicLoad = 227, OpAtomicXor = 242,
    DecorationBufferBlock = 3, DecorationBuiltIn = 11, DecorationNonWritable = 24,
    BuiltInClipDistance = 3, BuiltInCullDistance = 4,
    StorageUniform = 2, StorageOutput = 3, StorageStorageBuffer = 12,
    ModelVertex = 0, ModelGeometry = 3,
};

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

// A vertex, tessellation control / evaluation or geometry module (one entry point, as the translator writes them).
bool vertex_pipeline(const std::vector<uint32_t>& w, const std::vector<Inst>& insts) {
    int entries = 0;
    bool ok = false;
    for (const Inst& n : insts)
        if (n.op == OpEntryPoint) ++entries, ok = w[n.at + 1] >= ModelVertex && w[n.at + 1] <= ModelGeometry;
    return entries == 1 && ok;
}

// The words of `in` without the instructions at `drop` (offsets of instructions).
void copy_without(const std::vector<uint32_t>& in, const std::vector<Inst>& insts, const std::set<size_t>& drop, std::vector<uint32_t>& out) {
    out.assign(in.begin(), in.begin() + 5);
    out.reserve(in.size());
    for (const Inst& n : insts)
        if (!drop.count(n.at)) out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
}

}  // namespace

bool spirv_drop_clip_cull(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, int* stores) {
    std::vector<Inst> insts;
    if (in.size() < 5 || !decode(in, insts) || !vertex_pipeline(in, insts)) return false;
    std::map<uint32_t, std::set<uint32_t>> members;   // struct -> members decorated BuiltIn ClipDistance / CullDistance
    std::map<uint32_t, uint32_t> consts, ptr_pointee, var_type;
    std::set<uint32_t> plain;                          // variables decorated BuiltIn ClipDistance / CullDistance themselves
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpMemberDecorate && n.len >= 5 && w[3] == DecorationBuiltIn && (w[4] == BuiltInClipDistance || w[4] == BuiltInCullDistance))
            members[w[1]].insert(w[2]);
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && (w[3] == BuiltInClipDistance || w[3] == BuiltInCullDistance))
            plain.insert(w[1]);
        else if (n.op == OpConstant && n.len == 4) consts[w[2]] = w[3];
        else if (n.op == OpTypePointer && n.len == 4) ptr_pointee[w[1]] = w[3];
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageOutput) var_type[w[2]] = w[1];
    }
    if (members.empty() && plain.empty()) return false;
    // pointers into a clip / cull distance: the variable itself (plain), or an access chain into a gl_PerVertex output at such a member
    std::set<uint32_t> into;
    for (const auto& [v, t] : var_type)
        if (plain.count(v)) into.insert(v);
    std::set<size_t> drop;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 5) {
            if (into.count(w[3])) { into.insert(w[2]); drop.insert(n.at); continue; }
            const auto vt = var_type.find(w[3]);
            if (vt == var_type.end()) continue;
            const auto st = members.find(ptr_pointee[vt->second]);
            if (st == members.end()) continue;
            const auto k = consts.find(w[4]);
            if (k == consts.end()) return false;   // a computed member index: not the translator's
            if (st->second.count(k->second)) into.insert(w[2]), drop.insert(n.at);
        } else if (n.op == OpStore && n.len >= 3 && into.count(w[1])) {
            drop.insert(n.at);
        } else if ((n.op == OpLoad && n.len >= 4 && into.count(w[3])) || (n.op == OpCopyMemory && n.len >= 3 && (into.count(w[1]) || into.count(w[2])))) {
            return false;   // a read or copy of a clip / cull distance is not what the translator writes
        } else if (n.op == OpFunctionCall) {
            for (uint32_t k = 4; k < n.len; ++k)
                if (into.count(w[k])) return false;
        }
    }
    int n_stores = 0;
    for (const Inst& n : insts) n_stores += n.op == OpStore && drop.count(n.at);
    if (!n_stores) return false;   // only access chains, nothing stored: leave the module as it is
    copy_without(in, insts, drop, out);
    if (stores) *stores = n_stores;
    return true;
}

bool spirv_readonly_vertex_buffers(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, int* buffers) {
    std::vector<Inst> insts;
    if (in.size() < 5 || !decode(in, insts) || !vertex_pipeline(in, insts)) return false;
    std::set<uint32_t> buffer_block;
    std::map<uint32_t, uint32_t> ptr_pointee, elem, var_type, var_class;
    std::map<uint32_t, uint32_t> struct_members;
    std::set<std::pair<uint32_t, uint32_t>> nonwritable;
    size_t first_type = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!first_type && is_type_or_constant(n.op)) first_type = n.at;
        if (n.op == OpDecorate && n.len >= 3 && w[2] == DecorationBufferBlock) buffer_block.insert(w[1]);
        else if (n.op == OpMemberDecorate && n.len >= 4 && w[3] == DecorationNonWritable) nonwritable.insert({w[1], w[2]});
        else if (n.op == OpTypePointer && n.len == 4) ptr_pointee[w[1]] = w[3];
        else if ((n.op == OpTypeArray || n.op == OpTypeRuntimeArray) && n.len >= 3) elem[w[1]] = w[2];
        else if (n.op == OpTypeStruct) struct_members[w[1]] = n.len - 2;
        else if (n.op == OpVariable && n.len >= 4) var_type[w[2]] = w[1], var_class[w[2]] = w[3];
    }
    // the storage buffer variables and their block structs
    std::map<uint32_t, uint32_t> buf_struct;   // variable -> struct
    for (const auto& [v, t] : var_type) {
        uint32_t s = ptr_pointee[t];
        while (elem.count(s)) s = elem[s];
        const uint32_t c = var_class[v];
        if (c == StorageStorageBuffer || (c == StorageUniform && buffer_block.count(s))) buf_struct[v] = s;
    }
    if (buf_struct.empty() || !first_type) return false;
    // a store or atomic through any of them: the module really writes (leave it to the caller)
    std::map<uint32_t, uint32_t> base;   // access chain -> variable
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 4) {
            const auto b = base.find(w[3]);
            base[w[2]] = b != base.end() ? b->second : w[3];
        }
        uint32_t ptr = 0;
        if (n.op == OpStore && n.len >= 3) ptr = w[1];
        else if (n.op == OpCopyMemory && n.len >= 3) ptr = w[1];
        else if (n.op >= OpAtomicLoad && n.op <= OpAtomicXor && n.op != OpAtomicLoad && n.len >= 4) ptr = w[3];
        if (!ptr) continue;
        const auto b = base.find(ptr);
        if (buf_struct.count(b != base.end() ? b->second : ptr)) return false;
    }
    std::vector<uint32_t> deco;
    std::set<uint32_t> done;
    for (const auto& [v, s] : buf_struct) {
        if (!done.insert(s).second) continue;
        for (uint32_t m = 0; m < struct_members[s]; ++m)
            if (!nonwritable.count({s, m})) deco.insert(deco.end(), {(4u << 16) | OpMemberDecorate, s, m, DecorationNonWritable});
    }
    if (deco.empty()) return false;
    // annotations come before the first type: the new decorations go right there
    out.assign(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(first_type));
    out.insert(out.end(), deco.begin(), deco.end());
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(first_type), in.end());
    if (buffers) *buffers = static_cast<int>(done.size());
    return true;
}

}  // namespace nxrt::nvn::be::gpu
