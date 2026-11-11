// SPDX-License-Identifier: GPL-3.0-or-later
// spirv_limit_count (declared in nvn_spirv_patch.h). Ported from pokopia-native (MIT) windows/src/nxrt/vk/vk_shader.c vkspv_limit_count
// @9cb7f07, texel buffer tables only (every table this translator writes is an OpTypeImage Dim Buffer behind a sampled image).
#include "nvn_spirv_patch.h"

#include <cstring>

namespace nxrt::nvn::be::gpu {
namespace {

enum : uint32_t {
    OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpCapability = 17, OpTypeInt = 21, OpTypeImage = 25, OpTypeSampledImage = 27,
    OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpImage = 100,
    OpImageQuerySize = 104, OpIAdd = 128, OpSDiv = 135, OpBitcast = 124, OpCmpFirst = 170, OpCmpLast = 181,
    CapImageQuery = 50, DimBuffer = 5, GlslUMin = 38, GlslSMin = 39,
};

bool name_is(const std::vector<uint32_t>& w, size_t at, uint32_t len, const char* s) {
    const size_t bytes = (len - 2) * 4, n = std::strlen(s);
    return n < bytes && std::memcmp(&w[at + 2], s, n + 1) == 0;
}

}  // namespace

int spirv_limit_count(const std::vector<uint32_t>& in, const SpvCountLimit& lim, bool by_table, std::vector<uint32_t>& out) {
    const auto& w = in;
    if (w.size() < 5 || w[0] != 0x07230203u || !lim.count_var || !lim.stride) return 0;
    const bool table = by_table && lim.table_var;
    uint32_t bound = w[3], cv = 0, tv = 0, glsl = 0, ti = 0, tu = 0, query = 0;
    const uint32_t cval[3] = {16384, lim.stride - 1, lim.stride};   // the cap, the round-up, the stride
    uint32_t c[3] = {};
    const uint32_t at[3] = {0, lim.word / 4, lim.word % 4};   // the count's access chain: member 0 (the vec4 array), the vec4, the component
    size_t first_fn = 0;
    std::vector<uint32_t> def(bound), kval(bound);
    std::vector<uint8_t> isk(bound), i32(bound), ac(bound), ld(bound), bc(bound);   // bc: 1 a signed count read, 2 an unsigned one
    for (size_t i = 5; i < w.size();) {
        const uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > w.size()) return 0;
        if (op == OpName && len >= 3 && w[i + 1] < bound) {
            if (name_is(w, i, len, lim.count_var)) cv = w[i + 1];
            else if (lim.table_var && name_is(w, i, len, lim.table_var)) tv = w[i + 1];
        }
        if (op == OpExtInstImport && len >= 3 && name_is(w, i, len, "GLSL.std.450")) glsl = w[i + 1];
        if (op == OpCapability && len == 2 && w[i + 1] == CapImageQuery) query = 1;
        if ((op == OpTypePointer || op == OpTypeSampledImage || op == OpTypeImage) && len >= 3 && w[i + 1] < bound) def[w[i + 1]] = uint32_t(i);
        if (op == OpVariable && len >= 4 && w[i + 2] < bound) def[w[i + 2]] = uint32_t(i);
        if (op == OpTypeInt && len == 4 && w[i + 2] == 32 && w[i + 1] < bound) {
            i32[w[i + 1]] = 1;
            if (w[i + 3] == 1 && !ti) ti = w[i + 1];
            if (w[i + 3] == 0 && !tu) tu = w[i + 1];
        }
        if (op == OpConstant && len == 4 && w[i + 1] < bound && w[i + 2] < bound && i32[w[i + 1]]) {
            isk[w[i + 2]] = 1;
            kval[w[i + 2]] = w[i + 3];
            for (int k = 0; k < 3; k++)
                if (ti && w[i + 1] == ti && w[i + 3] == cval[k] && !c[k]) c[k] = w[i + 2];
        }
        if (op == OpFunction && !first_fn) first_fn = i;
        if (op == OpAccessChain && len == 7 && cv && w[i + 3] == cv && w[i + 2] < bound) {
            bool m = true;
            for (int k = 0; k < 3; k++) m = m && w[i + 4 + k] < bound && isk[w[i + 4 + k]] && kval[w[i + 4 + k]] == at[k];
            if (m) ac[w[i + 2]] = 1;
        }
        if (op == OpLoad && len >= 4 && w[i + 2] < bound && w[i + 3] < bound && ac[w[i + 3]]) ld[w[i + 2]] = 1;
        if (op == OpBitcast && len == 4 && w[i + 1] && (w[i + 1] == ti || w[i + 1] == tu) && w[i + 2] < bound && w[i + 3] < bound &&
            ld[w[i + 3]])
            bc[w[i + 2]] = w[i + 1] == ti ? 1 : 2;
        i += len;
    }
    // the table: a UniformConstant pointer to a sampled image of a Dim Buffer image
    uint32_t sit = 0, it = 0;
    if (table && tv && def[tv]) {
        const uint32_t pt = w[def[tv] + 1];
        if (pt < bound && def[pt] && (w[def[pt]] & 0xffff) == OpTypePointer) sit = w[def[pt] + 3];
        if (sit < bound && def[sit] && (w[def[sit]] & 0xffff) == OpTypeSampledImage) it = w[def[sit] + 2];
        if (!(it < bound && def[it] && (w[def[it]] & 0xffff) == OpTypeImage && (w[def[it]] >> 16) >= 9 && w[def[it] + 3] == DimBuffer)) it = 0;
    }
    if (!glsl || !ti || !first_fn || (table && !it)) return 0;
    bool any = false;
    for (uint32_t k = 0; k < bound; k++) any = any || bc[k];
    if (!any) return 0;

    out.clear();
    out.reserve(w.size() + 64);
    out.insert(out.end(), w.begin(), w.begin() + 5);
    if (table && !query) out.insert(out.end(), {2u << 16 | OpCapability, CapImageQuery});
    auto put = [&](std::initializer_list<uint32_t> x) { out.insert(out.end(), x); };
    std::vector<uint32_t> nid(bound);   // each count read's limited value
    int limited = 0;
    for (size_t i = 5; i < w.size();) {
        const uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (i == first_fn)   // the constants the limit needs and the module lacks
            for (int k = 0; k < 3; k++)
                if (!c[k]) {
                    c[k] = bound++;
                    put({4u << 16 | OpConstant, ti, c[k], cval[k]});
                }
        if (op >= OpCmpFirst && op <= OpCmpLast && len == 5 && ((w[i + 3] < bound && nid[w[i + 3]]) || (w[i + 4] < bound && nid[w[i + 4]]))) {
            uint32_t x[5];
            std::memcpy(x, &w[i], sizeof x);
            for (int a = 3; a < 5; a++)
                if (x[a] < bound && nid[x[a]]) x[a] = nid[x[a]];
            out.insert(out.end(), x, x + 5);
            limited++;
            i += len;
            continue;
        }
        out.insert(out.end(), w.begin() + i, w.begin() + i + len);
        if (op == OpBitcast && len == 4 && w[i + 2] < bound && bc[w[i + 2]]) {
            // the count read: min(count, (texels + stride - 1) / stride), or min(count, 16384)
            uint32_t cap = c[0];
            if (table) {
                const uint32_t tex = bound++, img = bound++, sz = bound++, tr = bound++;
                cap = bound++;
                put({4u << 16 | OpLoad, sit, tex, tv, 4u << 16 | OpImage, it, img, tex, 4u << 16 | OpImageQuerySize, ti, sz, img,
                     5u << 16 | OpIAdd, ti, tr, sz, c[1], 5u << 16 | OpSDiv, ti, cap, tr, c[2]});
            }
            const uint32_t r = bound++;
            if (bc[w[i + 2]] == 1) {
                put({7u << 16 | OpExtInst, ti, r, glsl, GlslSMin, w[i + 2], cap});
            } else {   // compared unsigned: the unsigned minimum (a garbage count of 0xffffffff is no negative number)
                const uint32_t cu = bound++;
                put({4u << 16 | OpBitcast, tu, cu, cap, 7u << 16 | OpExtInst, tu, r, glsl, GlslUMin, w[i + 2], cu});
            }
            nid[w[i + 2]] = r;
        }
        i += len;
    }
    if (!limited) {
        out.clear();
        return 0;
    }
    out[3] = bound;
    return limited;
}

}  // namespace nxrt::nvn::be::gpu
