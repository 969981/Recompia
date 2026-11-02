// SPDX-License-Identifier: GPL-3.0-or-later
// HDR patch of the tone-map fragment shader (hdr_tonemap_patch.h).
#include "extras/hdr_tonemap_patch.h"

#include <cstring>
#include <unordered_map>

namespace nxrt::extras {
namespace {

enum : uint32_t {
    kOpExtInstImport = 11, kOpExtInst = 12, kOpEntryPoint = 15, kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypeImage = 25, kOpTypeSampledImage = 27,
    kOpConstant = 43, kOpFunction = 54, kOpLoad = 61, kOpStore = 62, kOpVectorShuffle = 79, kOpCompositeConstruct = 80, kOpImageSampleImplicitLod = 87,
    kOpFMul = 133,
    kGlslFma = 50, kGlslFMax = 40,
    kExecFragment = 4, kDim3D = 2,
};

struct Inst {
    size_t pos;       // index of the first word
    uint32_t op, len;
};

uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

}  // namespace

bool spirv_hdr_tonemap(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 8 || in[0] != 0x07230203u) return false;
    std::vector<Inst> insts;
    std::unordered_map<uint32_t, size_t> def;   // result id -> index into insts
    for (size_t i = 5; i < in.size();) {
        const uint32_t op = in[i] & 0xffff, len = in[i] >> 16;
        if (len == 0 || i + len > in.size()) return false;
        insts.push_back({i, op, len});
        // result id position: types and constants have it at word 1; most value instructions at word 2
        auto result_at = [&](uint32_t idx) { if (idx < len) def[in[i + idx]] = insts.size() - 1; };
        switch (op) {
            case kOpExtInstImport: case kOpTypeFloat: case kOpTypeVector: case kOpTypeImage: case kOpTypeSampledImage: result_at(1); break;
            case kOpConstant: case kOpLoad: case kOpVectorShuffle: case kOpCompositeConstruct: case kOpImageSampleImplicitLod: case kOpExtInst: case kOpFMul: result_at(2); break;
            default: break;
        }
        i += len;
    }
    auto get = [&](uint32_t id) -> const Inst* {
        auto it = def.find(id);
        return it == def.end() ? nullptr : &insts[it->second];
    };

    // fragment entry point
    bool fragment = false;
    uint32_t glsl = 0;
    for (const Inst& n : insts) {
        if (n.op == kOpEntryPoint && in[n.pos + 1] == kExecFragment) fragment = true;
        if (n.op == kOpExtInstImport && std::memcmp(&in[n.pos + 2], "GLSL.std.450", 12) == 0) glsl = in[n.pos + 1];
    }
    if (!fragment || !glsl) return false;

    // types: 32-bit float, vec3 of it, the constant 1.0
    uint32_t t_float = 0, t_v3 = 0, c_one = 0;
    for (const Inst& n : insts) if (n.op == kOpTypeFloat && in[n.pos + 2] == 32) t_float = in[n.pos + 1];
    if (!t_float) return false;
    for (const Inst& n : insts) if (n.op == kOpTypeVector && in[n.pos + 2] == t_float && in[n.pos + 3] == 3) t_v3 = in[n.pos + 1];
    for (const Inst& n : insts) if (n.op == kOpConstant && in[n.pos + 1] == t_float && in[n.pos + 3] == float_bits(1.0f)) c_one = in[n.pos + 2];
    if (!t_v3) return false;

    // the 3D samples in program order
    struct Sample { size_t idx; uint32_t result, coord; };
    std::vector<Sample> s3;
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        if (n.op != kOpImageSampleImplicitLod) continue;
        const Inst* si = get(in[n.pos + 3]);                 // the sampled image value: an OpLoad of a variable
        if (!si || si->op != kOpLoad) continue;
        const Inst* sty = get(in[si->pos + 1]);              // its type: OpTypeSampledImage
        if (!sty || sty->op != kOpTypeSampledImage) continue;
        const Inst* img = get(in[sty->pos + 2]);             // OpTypeImage: result, sampled type, dim
        if (!img || img->op != kOpTypeImage || in[img->pos + 3] != kDim3D) continue;
        s3.push_back({k, in[n.pos + 2], in[n.pos + 4]});
    }
    if (s3.size() != 2) return false;

    // the curve outputs: the coordinate of the first sample is (Fma(y_r, s, b), Fma(y_g, s, b), Fma(y_b, s, b))
    const Inst* cc = get(s3[0].coord);
    if (!cc || cc->op != kOpCompositeConstruct || cc->len != 6) return false;
    uint32_t y[3];
    for (int c = 0; c < 3; ++c) {
        const Inst* f = get(in[cc->pos + 3 + c]);
        if (!f || f->op != kOpExtInst || in[f->pos + 3] != glsl || in[f->pos + 4] != kGlslFma || f->len != 8) return false;
        y[c] = in[f->pos + 5];
    }

    // the graded colour: Shuffle(sample B) -> vec3 -> Store
    const uint32_t b_res = s3[1].result;
    const Inst* shuf = nullptr;
    for (const Inst& n : insts)
        if (n.op == kOpVectorShuffle && n.len == 8 && in[n.pos + 3] == b_res && in[n.pos + 4] == b_res && in[n.pos + 5] == 0 && in[n.pos + 6] == 1 && in[n.pos + 7] == 2 && in[n.pos + 1] == t_v3) shuf = &n;
    if (!shuf) return false;
    const uint32_t v3_id = in[shuf->pos + 2];
    const Inst* store = nullptr;
    for (size_t k = 0; k < insts.size(); ++k)
        if (insts[k].op == kOpStore && insts[k].pos > shuf->pos && in[insts[k].pos + 2] == v3_id) { store = &insts[k]; break; }
    if (!store) return false;

    // build the new module
    out.clear();
    out.reserve(in.size() + 40);
    uint32_t bound = in[3];
    const uint32_t id_one = c_one ? c_one : bound++;
    uint32_t m[3] = {bound++, bound++, bound++};
    const uint32_t id_vec = bound++, id_mul = bound++;
    out.insert(out.end(), in.begin(), in.begin() + 5);
    out[3] = bound;
    bool const_added = c_one != 0;
    for (const Inst& n : insts) {
        if (!const_added && n.op == kOpFunction) {       // the 1.0 constant goes at the end of the global section, before the first function
            out.push_back((4u << 16) | kOpConstant);
            out.push_back(t_float);
            out.push_back(id_one);
            out.push_back(float_bits(1.0f));
            const_added = true;
        }
        if (&n == store) {
            for (int c = 0; c < 3; ++c) {                // m_c = max(y_c, 1)
                out.push_back((7u << 16) | kOpExtInst);
                out.push_back(t_float);
                out.push_back(m[c]);
                out.push_back(glsl);
                out.push_back(kGlslFMax);
                out.push_back(y[c]);
                out.push_back(id_one);
            }
            out.push_back((6u << 16) | kOpCompositeConstruct);
            out.push_back(t_v3);
            out.push_back(id_vec);
            out.push_back(m[0]);
            out.push_back(m[1]);
            out.push_back(m[2]);
            out.push_back((5u << 16) | kOpFMul);
            out.push_back(t_v3);
            out.push_back(id_mul);
            out.push_back(v3_id);
            out.push_back(id_vec);
            out.push_back(in[n.pos]);                    // the store itself, with the new value
            out.push_back(in[n.pos + 1]);
            out.push_back(id_mul);
            for (uint32_t w = 3; w < n.len; ++w) out.push_back(in[n.pos + w]);
            continue;
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.pos), in.begin() + static_cast<std::ptrdiff_t>(n.pos + n.len));
    }
    return const_added;
}

}  // namespace nxrt::extras
