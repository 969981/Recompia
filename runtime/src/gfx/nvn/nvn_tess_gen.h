// SPDX-License-Identifier: GPL-3.0-or-later
// Pass-through tessellation control shader for NVN programs that have an evaluation shader only. NVN then tessellates with the fixed
// levels from nvnCommandBufferSetInner/OuterTessellationLevels; Vulkan always needs a control shader. The generated shader copies every
// per-vertex input location the evaluation shader reads (vec4, the translator's attribute type) and gl_Position from the vertex shader,
// writes zero to the evaluation shader's per-patch inputs, and sets the levels as constants. Header-only, no dependencies, so a
// standalone test can validate the words (spirv-val).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace nxrt::nvn::be::tessgen {

// vertex_locations: per-vertex vec4 locations to copy; patch_locations: per-patch vec4 outputs (written as zero);
// vertices: output patch size (1..32); outer[4] / inner[2]: tessellation levels. The gl_PerVertex block matches the translator's
// ({vec4 Position, float PointSize, float ClipDistance[clip_count], float CullDistance[cull_count]}).
inline std::vector<uint32_t> passthrough_tcs(const std::vector<uint32_t>& vertex_locations, const std::vector<uint32_t>& patch_locations,
                                             uint32_t vertices, const float outer[4], const float inner[2], uint32_t clip_count = 7,
                                             uint32_t cull_count = 1) {
    std::vector<uint32_t> w;
    uint32_t next = 1;
    auto id = [&] { return next++; };
    auto op = [&](uint32_t code, std::initializer_list<uint32_t> args) {
        w.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | code);
        w.insert(w.end(), args.begin(), args.end());
    };
    auto fbits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    enum : uint32_t {
        kCapability = 17, kMemoryModel = 14, kEntryPoint = 15, kExecutionMode = 16, kDecorate = 71, kMemberDecorate = 72, kTypeVoid = 19,
        kTypeFunction = 33, kTypeFloat = 22, kTypeInt = 21, kTypeVector = 23, kTypeArray = 28, kTypeStruct = 30, kTypePointer = 32,
        kConstant = 43, kConstantComposite = 44, kVariable = 59, kFunction = 54, kLabel = 248, kLoad = 61, kStore = 62, kAccessChain = 65,
        kReturn = 253, kFunctionEnd = 56,
        kInput = 1, kOutput = 3,
        kBlock = 2, kBuiltIn = 11, kPatch = 15, kLocation = 30,
        kPosition = 0, kPointSize = 1, kClipDistance = 3, kCullDistance = 4, kInvocationId = 8, kTessLevelOuter = 11, kTessLevelInner = 12,
    };
    if (vertices < 1) vertices = 1;
    if (vertices > 32) vertices = 32;

    // ids
    const uint32_t t_void = id(), t_fn = id(), t_float = id(), t_int = id(), t_uint = id(), t_v4 = id();
    const uint32_t c_u32 = id(), c_un = id(), c_u4 = id(), c_u2 = id(), c_uclip = id(), c_ucull = id();
    const uint32_t t_arr_v4_32 = id(), t_arr_v4_n = id(), t_arr_f4 = id(), t_arr_f2 = id(), t_arr_fclip = id(), t_arr_fcull = id();
    const uint32_t t_pv_in = id(), t_pv_out = id(), t_arr_pv_32 = id(), t_arr_pv_n = id();
    const uint32_t p_in_arr_v4 = id(), p_out_arr_v4 = id(), p_out_v4 = id(), p_in_v4 = id(), p_in_arr_pv = id(), p_out_arr_pv = id();
    const uint32_t p_in_int = id(), p_out_f4 = id(), p_out_f2 = id(), p_out_float = id();
    const uint32_t c_i0 = id(), c_f0 = id(), c_zero_v4 = id();
    uint32_t c_outer[4], c_inner[2], c_idx[4];
    for (uint32_t& c : c_outer) c = id();
    for (uint32_t& c : c_inner) c = id();
    for (uint32_t& c : c_idx) c = id();
    const uint32_t v_invocation = id(), v_gl_in = id(), v_gl_out = id(), v_outer = id(), v_inner = id();
    std::vector<uint32_t> v_in(vertex_locations.size()), v_out(vertex_locations.size()), v_patch(patch_locations.size());
    for (uint32_t& v : v_in) v = id();
    for (uint32_t& v : v_out) v = id();
    for (uint32_t& v : v_patch) v = id();
    const uint32_t f_main = id(), l_entry = id();

    w = {0x07230203u, 0x00010300u, 0u, 0u /* bound, set at the end */, 0u};
    op(kCapability, {1});   // Shader
    op(kCapability, {3});   // Tessellation
    op(kMemoryModel, {0, 1});
    {   // OpEntryPoint TessellationControl %main "main" <interface>
        std::vector<uint32_t> args = {1, f_main, 0x6e69616du, 0u};   // "main\0"
        args.push_back(v_invocation); args.push_back(v_gl_in); args.push_back(v_gl_out); args.push_back(v_outer); args.push_back(v_inner);
        for (uint32_t v : v_in) args.push_back(v);
        for (uint32_t v : v_out) args.push_back(v);
        for (uint32_t v : v_patch) args.push_back(v);
        w.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | kEntryPoint);
        w.insert(w.end(), args.begin(), args.end());
    }
    op(kExecutionMode, {f_main, 26, vertices});   // OutputVertices

    op(kDecorate, {v_invocation, kBuiltIn, kInvocationId});
    op(kDecorate, {v_outer, kBuiltIn, kTessLevelOuter});
    op(kDecorate, {v_outer, kPatch});
    op(kDecorate, {v_inner, kBuiltIn, kTessLevelInner});
    op(kDecorate, {v_inner, kPatch});
    for (uint32_t s : {t_pv_in, t_pv_out}) {
        op(kMemberDecorate, {s, 0, kBuiltIn, kPosition});
        op(kMemberDecorate, {s, 1, kBuiltIn, kPointSize});
        op(kMemberDecorate, {s, 2, kBuiltIn, kClipDistance});
        op(kMemberDecorate, {s, 3, kBuiltIn, kCullDistance});
        op(kDecorate, {s, kBlock});
    }
    for (size_t i = 0; i < vertex_locations.size(); ++i) {
        op(kDecorate, {v_in[i], kLocation, vertex_locations[i]});
        op(kDecorate, {v_out[i], kLocation, vertex_locations[i]});
    }
    for (size_t i = 0; i < patch_locations.size(); ++i) {
        op(kDecorate, {v_patch[i], kPatch});
        op(kDecorate, {v_patch[i], kLocation, patch_locations[i]});
    }

    op(kTypeVoid, {t_void});
    op(kTypeFunction, {t_fn, t_void});
    op(kTypeFloat, {t_float, 32});
    op(kTypeInt, {t_int, 32, 1});
    op(kTypeInt, {t_uint, 32, 0});
    op(kTypeVector, {t_v4, t_float, 4});
    op(kConstant, {t_uint, c_u32, 32});
    op(kConstant, {t_uint, c_un, vertices});
    op(kConstant, {t_uint, c_u4, 4});
    op(kConstant, {t_uint, c_u2, 2});
    op(kConstant, {t_uint, c_uclip, clip_count ? clip_count : 1});
    op(kConstant, {t_uint, c_ucull, cull_count ? cull_count : 1});
    op(kTypeArray, {t_arr_v4_32, t_v4, c_u32});
    op(kTypeArray, {t_arr_v4_n, t_v4, c_un});
    op(kTypeArray, {t_arr_f4, t_float, c_u4});
    op(kTypeArray, {t_arr_f2, t_float, c_u2});
    op(kTypeArray, {t_arr_fclip, t_float, c_uclip});
    op(kTypeArray, {t_arr_fcull, t_float, c_ucull});
    op(kTypeStruct, {t_pv_in, t_v4, t_float, t_arr_fclip, t_arr_fcull});
    op(kTypeStruct, {t_pv_out, t_v4, t_float, t_arr_fclip, t_arr_fcull});
    op(kTypeArray, {t_arr_pv_32, t_pv_in, c_u32});
    op(kTypeArray, {t_arr_pv_n, t_pv_out, c_un});
    op(kTypePointer, {p_in_arr_v4, kInput, t_arr_v4_32});
    op(kTypePointer, {p_out_arr_v4, kOutput, t_arr_v4_n});
    op(kTypePointer, {p_out_v4, kOutput, t_v4});
    op(kTypePointer, {p_in_v4, kInput, t_v4});
    op(kTypePointer, {p_in_arr_pv, kInput, t_arr_pv_32});
    op(kTypePointer, {p_out_arr_pv, kOutput, t_arr_pv_n});
    op(kTypePointer, {p_in_int, kInput, t_int});
    op(kTypePointer, {p_out_f4, kOutput, t_arr_f4});
    op(kTypePointer, {p_out_f2, kOutput, t_arr_f2});
    op(kTypePointer, {p_out_float, kOutput, t_float});
    op(kConstant, {t_int, c_i0, 0});
    op(kConstant, {t_float, c_f0, 0});
    op(kConstantComposite, {t_v4, c_zero_v4, c_f0, c_f0, c_f0, c_f0});
    for (int i = 0; i < 4; ++i) op(kConstant, {t_float, c_outer[i], fbits(outer[i])});
    for (int i = 0; i < 2; ++i) op(kConstant, {t_float, c_inner[i], fbits(inner[i])});
    for (int i = 0; i < 4; ++i) op(kConstant, {t_int, c_idx[i], static_cast<uint32_t>(i)});

    op(kVariable, {p_in_int, v_invocation, kInput});
    op(kVariable, {p_in_arr_pv, v_gl_in, kInput});
    op(kVariable, {p_out_arr_pv, v_gl_out, kOutput});
    op(kVariable, {p_out_f4, v_outer, kOutput});
    op(kVariable, {p_out_f2, v_inner, kOutput});
    for (uint32_t v : v_in) op(kVariable, {p_in_arr_v4, v, kInput});
    for (uint32_t v : v_out) op(kVariable, {p_out_arr_v4, v, kOutput});
    for (uint32_t v : v_patch) op(kVariable, {p_out_v4, v, kOutput});

    op(kFunction, {t_void, f_main, 0, t_fn});
    op(kLabel, {l_entry});
    const uint32_t inv = id();
    op(kLoad, {t_int, inv, v_invocation});
    for (size_t i = 0; i < v_in.size(); ++i) {   // out[id] = in[id]
        const uint32_t src = id(), val = id(), dst = id();
        op(kAccessChain, {p_in_v4, src, v_in[i], inv});
        op(kLoad, {t_v4, val, src});
        op(kAccessChain, {p_out_v4, dst, v_out[i], inv});
        op(kStore, {dst, val});
    }
    {   // gl_out[id].gl_Position = gl_in[id].gl_Position
        const uint32_t src = id(), val = id(), dst = id();
        op(kAccessChain, {p_in_v4, src, v_gl_in, inv, c_i0});
        op(kLoad, {t_v4, val, src});
        op(kAccessChain, {p_out_v4, dst, v_gl_out, inv, c_i0});
        op(kStore, {dst, val});
    }
    // per-patch values: every invocation writes the same constants (allowed: identical values)
    for (int i = 0; i < 4; ++i) {
        const uint32_t dst = id();
        op(kAccessChain, {p_out_float, dst, v_outer, c_idx[i]});
        op(kStore, {dst, c_outer[i]});
    }
    for (int i = 0; i < 2; ++i) {
        const uint32_t dst = id();
        op(kAccessChain, {p_out_float, dst, v_inner, c_idx[i]});
        op(kStore, {dst, c_inner[i]});
    }
    for (uint32_t v : v_patch) op(kStore, {v, c_zero_v4});
    op(kReturn, {});
    op(kFunctionEnd, {});
    w[3] = next;
    return w;
}

}  // namespace nxrt::nvn::be::tessgen
