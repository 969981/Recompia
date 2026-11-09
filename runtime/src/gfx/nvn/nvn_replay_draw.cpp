// SPDX-License-Identifier: GPL-3.0-or-later
// Draws of the replay: pipeline keys, dynamic state and its shadow, begin_draw, primitive conversion, direct, indirect and feedback draws.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
DynShadow g_dsh;
VbShadow g_vbsh[16];
IbShadow g_ibsh;
// vkCmdBindIndexBuffer unless the same (buffer, offset, type) is already bound in this command buffer.
static void bind_index_buffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type) {
    if (bind_coalesce_on()) {
        if (g_ibsh.ok && g_ibsh.buf == buf && g_ibsh.off == off && g_ibsh.type == type) { ft::split_count(ft::kCntSkipped); return; }
        g_ibsh = {buf, off, type, true};
    }
    vkCmdBindIndexBuffer(cb, buf, off, type);
}
const ProgLayout* g_pl = nullptr;
VkPipeline g_pipe = VK_NULL_HANDLE;
VkPrimitiveTopology g_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
VkPrimitiveTopology g_pipe_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
static bool g_pipe_fbl = false;        // g_pipe was made for a pass with a feedback loop attachment (GraphicsKey::fb_loop)
bool g_restart = false, g_restart_valid = false;
static uint32_t g_key_attr_count = 0;
static uint8_t g_key_binding[16] = {};

uint8_t g_key_dummy_binding = 0;      // 1 + the binding that feeds the defaults of unset vertex attributes (0: none)
bool g_want_depth_ro = false;         // some texture this draw samples is the current depth attachment
bool g_vb_bound_in_cb = false;        // vkCmdBindVertexBuffers2 was called in this command buffer (dynamic strides need one)

// [nvn] trace_frame = N (NXRT_NVN_TRACE_FRAME): every draw, clear, copy and dispatch of the frame that is presented as number N is logged
// at Info ("trace ..." lines): programs, render targets, sampled textures, blend and depth state. For finding out what a frame does.
bool g_xfb_active = false;
static bool g_pipe_xfb = false;             // the bound pipeline is a stream-out variant
bool g_xfb_began = false;            // vkCmdBeginTransformFeedbackEXT is in effect in the open pass
bool g_xfb_have_counter = false;     // a counter buffer holds the bytes written so far: the next begin continues after them
gfx::Buffer g_xfb_counter;
std::unordered_map<uint64_t, uint32_t> g_xfb_slot_of;
uint32_t g_xfb_slot_next = 0, g_xfb_cur = 0;
bool g_xfb_slot_written[kXfbSlots] = {};

// ---- helpers ----------------------------------------------------------------------------------------------------------------
static VkFormat scaled_for(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: return VK_FORMAT_R8_USCALED;
        case VK_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_USCALED;
        case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_USCALED;
        case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED;
        case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_USCALED;
        case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED;
        case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16_USCALED;
        case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_SSCALED;
        case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SSCALED;
        case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SSCALED;
        case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
        case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SSCALED;
        case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
        case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SSCALED;
        case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        default: return f;
    }
}

static bool is_int_format(VkFormat f) { return scaled_for(f) != f; }

// ---- viewport conversion -------------------------------------------------------------------------------------------------
VkRect2D scissor_rect(const int32_t* r) {
    int32_t x = r[0], y = r[1], w = r[2], h = r[3];
    if (P.scale != 1.0f) {     // the edges scale (a rectangle covering the target still covers it)
        const float sc = P.scale;
        const int32_t x1 = static_cast<int32_t>(std::ceil((static_cast<float>(x) + static_cast<float>(w)) * sc - 0.001f));
        const int32_t y1 = static_cast<int32_t>(std::ceil((static_cast<float>(y) + static_cast<float>(h)) * sc - 0.001f));
        x = static_cast<int32_t>(std::floor(static_cast<float>(x) * sc + 0.001f));
        y = static_cast<int32_t>(std::floor(static_cast<float>(y) * sc + 0.001f));
        w = x1 - x;
        h = y1 - y;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    w = std::max(0, std::min<int32_t>(w, static_cast<int32_t>(P.extent.width) - x));
    h = std::max(0, std::min<int32_t>(h, static_cast<int32_t>(P.extent.height) - y));
    return {{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
}

static void set_viewports_and_scissors() {
    VkCommandBuffer cb = F->cb;
    VkViewport vp[16];
    VkRect2D sc[16];
    const uint32_t nvp = S.nvp ? S.nvp : 1;
    for (uint32_t i = 0; i < nvp; ++i) {
        float x = S.vp[i][0], y = S.vp[i][1], w = S.vp[i][2], h = S.vp[i][3];
        if (!S.nvp) { x = 0; y = 0; w = static_cast<float>(P.extent.width); h = static_cast<float>(P.extent.height); }
        else if (P.scale != 1.0f) { x *= P.scale; y *= P.scale; w *= P.scale; h *= P.scale; }
        // Upper-left origin: window y is the image row from the top and NDC +y is the top edge of the rectangle (negative height).
        // Lower-left origin (the GL convention): window y 0 is memory row 0 and NDC -y is the first row, so no flip.
        const bool lower_left = S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
        vp[i] = lower_left ? VkViewport{x, y, w, h, S.dr[i][0], S.dr[i][1]}
                           : VkViewport{x, y + h, w, -h, S.dr[i][0], S.dr[i][1]};
        if (i < S.nsc) sc[i] = scissor_rect(S.sc[i]);
        else sc[i] = {{0, 0}, P.extent};
    }
    if (bind_coalesce_on()) {
        // compare the whole arrays (the unused tail is zero) and the count
        std::array<VkViewport, 16> av = {};
        std::array<VkRect2D, 16> as = {};
        std::memcpy(av.data(), vp, sizeof(VkViewport) * nvp);
        std::memcpy(as.data(), sc, sizeof(VkRect2D) * nvp);
        const bool cv = g_dsh.vp.changed(av) || g_dsh.nvp != nvp;
        const bool cs = g_dsh.sc.changed(as) || g_dsh.nsc != nvp;
        g_dsh.nvp = g_dsh.nsc = nvp;
        if (cv) vkCmdSetViewportWithCount(cb, nvp, vp); else ft::split_count(ft::kCntSkipped);
        if (cs) vkCmdSetScissorWithCount(cb, nvp, sc); else ft::split_count(ft::kCntSkipped);
        return;
    }
    vkCmdSetViewportWithCount(cb, nvp, vp);
    vkCmdSetScissorWithCount(cb, nvp, sc);
}

// ---- draws -------------------------------------------------------------------------------------------------------------------
static const ShaderInfo* tcs_now();   // the program's control shader, or the generated pass-through one (below)
static bool build_key(GraphicsKey& key, VkPrimitiveTopology topo) {
    std::memset(&key, 0, sizeof key);
    key.stage[0] = S.stage[kVertex];
    key.stage[1] = S.stage[kFragment];
    key.stage[2] = S.stage[kGeometry];
    if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {      // begin_draw checked: both tessellation stages, patches
        key.tess[0] = tcs_now();
        key.tess[1] = S.stage[kTessEval];
        key.patch_control_points = static_cast<uint8_t>(std::clamp<uint32_t>(S.patch_size, 1, 32));
    }
    const ShaderInfo* vs = key.stage[0];
    // attributes: those the vertex shader reads. An input array takes one location per element. A location the game did not set up
    // (no attribute, or a disabled one) still has to exist in the pipeline; it reads (0, 0, 0, 1) from a dummy binding.
    struct Missing { uint32_t location; ScalarType type; };
    Missing missing[32];
    uint32_t nmissing = 0;
    for (const ShaderIo& in : vs->refl.inputs) {
        const uint32_t elems = in.array ? in.array : 1;
        for (uint32_t e = 0; e < elems; ++e) {
            const uint32_t loc = in.location + e;
            if (loc >= 32) break;
            VkFormat f = VK_FORMAT_UNDEFINED;
            const VertexAttribInfo* a = nullptr;
            if (loc < S.attr_count && loc < 16) {
                a = &S.attr[loc];
                f = vertex_format(a->format);
            }
            if (f == VK_FORMAT_UNDEFINED) {
                if (nmissing < 32) missing[nmissing++] = {loc, in.type};
                continue;
            }
            // a float input fed by an integer attribute gets the raw bits: the integer format stays and the pipeline patches the shader
            // (spirv_int_attributes); stream-out draws use the shader's stream-out variant, which is not patched: scaled floats there
            if (in.type == ScalarType::Float && is_int_format(f) && g_xfb_active) f = scaled_for(f);
            else if ((in.type == ScalarType::Uint || in.type == ScalarType::Sint) && !is_int_format(f))
                log_once(kOnceAttrTypeMismatch, "a vertex attribute with a float format feeds an integer shader input (needs a SPIR-V variant)");
            if (key.attr_count >= 16) break;
            KeyAttr& k = key.attr[key.attr_count++];
            k.location = static_cast<uint8_t>(loc);
            k.binding = static_cast<uint8_t>(a->stream & 15);
            k.offset = static_cast<uint16_t>(a->relative_offset);
            k.format = f;
            key.binding_instance[k.binding] = S.stream[k.binding].divisor > 0 ? 1 : 0;
        }
    }
    if (nmissing) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < key.attr_count; ++i) used |= 1u << key.attr[i].binding;
        uint32_t free_binding = 0;
        while (free_binding < 16 && (used >> free_binding & 1)) ++free_binding;
        if (free_binding < 16) {
            key.dummy_binding = static_cast<uint8_t>(free_binding + 1);
            key.binding_instance[free_binding] = 0;
            for (uint32_t i = 0; i < nmissing && key.attr_count < 16; ++i) {
                KeyAttr& k = key.attr[key.attr_count++];
                k.location = static_cast<uint8_t>(missing[i].location);
                k.binding = static_cast<uint8_t>(free_binding);
                const bool is_float = missing[i].type != ScalarType::Uint && missing[i].type != ScalarType::Sint;
                k.offset = is_float ? 0 : 16;
                k.format = is_float ? VK_FORMAT_R32G32B32A32_SFLOAT : (missing[i].type == ScalarType::Uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SINT);
            }
        }
    }
    key.color_count = P.ncolor;
    if (!P.ncolor && S.stage[kFragment] && !S.stage[kFragment]->refl.outputs.empty()) key.no_out = 1;   // a depth-only pass: no outputs
    if (!key.no_out && device().features().dynamic_rendering_unused_attachments) {
        // the fragment shader may write outputs the pass has no attachment for: they are listed as unused (format UNDEFINED)
        if (const ShaderInfo* fs = S.stage[kFragment])
            for (const ShaderIo& o : fs->refl.outputs)
                for (uint32_t e = 0; e < (o.array ? o.array : 1); ++e)
                    if (o.location + e < 8 && o.location + e >= key.color_count) key.color_count = o.location + e + 1;
    }
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) {
        if (i >= P.ncolor) {
            key.color_format[i] = VK_FORMAT_UNDEFINED;
            key.write_mask[i] = 0;
            continue;
        }
        key.color_format[i] = P.color_fmt[i];
        key.blend_enable[i] = (S.color.blend_enable_mask >> i) & 1;
        const BlendInfo& b = S.blend[i];
        key.blend_src_rgb[i] = b.src_rgb ? vk_blend_factor(b.src_rgb) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_rgb[i] = b.dst_rgb ? vk_blend_factor(b.dst_rgb) : VK_BLEND_FACTOR_ZERO;
        key.blend_src_a[i] = b.src_a ? vk_blend_factor(b.src_a) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_a[i] = b.dst_a ? vk_blend_factor(b.dst_a) : VK_BLEND_FACTOR_ZERO;
        key.blend_op_rgb[i] = b.eq_rgb ? vk_blend_op(b.eq_rgb) : VK_BLEND_OP_ADD;
        key.blend_op_a[i] = b.eq_a ? vk_blend_op(b.eq_a) : VK_BLEND_OP_ADD;
        key.write_mask[i] = S.chmask.mask[i] & 0xf;
        if (P.color_fmt[i] == VK_FORMAT_UNDEFINED) key.write_mask[i] = 0;
        auto uses_src1 = [](VkBlendFactor f) {
            return f == VK_BLEND_FACTOR_SRC1_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR || f == VK_BLEND_FACTOR_SRC1_ALPHA ||
                   f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        };
        if (key.blend_enable[i] && (uses_src1(static_cast<VkBlendFactor>(key.blend_src_rgb[i])) || uses_src1(static_cast<VkBlendFactor>(key.blend_dst_rgb[i])) ||
                                    uses_src1(static_cast<VkBlendFactor>(key.blend_src_a[i])) || uses_src1(static_cast<VkBlendFactor>(key.blend_dst_a[i]))))
            key.dual_src = 1;
    }
    key.xfb = g_xfb_active && device().features().transform_feedback && S.stage[kVertex] && xfb_stride(S.stage[kVertex]) ? 1 : 0;
    key.depth_format = P.depth_fmt;
    key.stencil_format = P.has_stencil ? P.depth_fmt : VK_FORMAT_UNDEFINED;
    key.samples = P.samples;
    key.topology = static_cast<uint8_t>(topo);
    key.polygon_mode = static_cast<uint8_t>(vk_polygon_mode(S.poly.polygon_mode));
    if (key.polygon_mode != VK_POLYGON_MODE_FILL && !device().features().fill_mode_non_solid) key.polygon_mode = VK_POLYGON_MODE_FILL;
    key.depth_clamp = S.depth_clamp && device().features().depth_clamp;
    key.alpha_to_coverage = S.ms.alpha_to_coverage && S.ms.enable ? 1 : 0;
    key.depth_neg_one = S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W ? 1 : 0;
    if (S.color.logic_op != 0 && S.color.logic_op != NVN_LOGIC_OP_COPY && device().features().logic_op) {
        key.logic_op_enable = 1;
        key.logic_op = static_cast<uint8_t>(vk_logic_op(S.color.logic_op));
        for (uint32_t i = 0; i < 8; ++i) key.blend_enable[i] = 0;
    }
    if (S.color.alpha_test != 0 && S.color.alpha_test != NVN_ALPHA_FUNC_ALWAYS) log_once(kOnceAlphaTest, "alpha test is enabled by the game: not implemented (needs a shader variant)");
    return true;
}

static void set_dynamic_state(uint32_t dirty, VkPrimitiveTopology topo, bool indexed, int index_type) {
    VkCommandBuffer cb = F->cb;
    const bool co = bind_coalesce_on();
    // true: send the value (always when the key is off)
    auto ch = [co](auto& sh, const auto& v) {
        if (!co || sh.changed(v)) return true;
        ft::split_count(ft::kCntSkipped);
        return false;
    };
    if (dirty & kDynVp) set_viewports_and_scissors();
    if (dirty & kDynRaster) {
        const VkCullModeFlags cull = vk_cull_mode(S.poly.cull_face);
        if (ch(g_dsh.cull, static_cast<uint32_t>(cull))) vkCmdSetCullMode(cb, cull);
        VkFrontFace ff = vk_front_face(S.poly.front_face);
        if (S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) ff = ff == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // unflipped viewport
        if (ch(g_dsh.front, static_cast<uint32_t>(ff))) vkCmdSetFrontFace(cb, ff);
        if (ch(g_dsh.discard, static_cast<uint32_t>(S.rasterizer_discard ? 1 : 0))) vkCmdSetRasterizerDiscardEnable(cb, S.rasterizer_discard);
        const float lw = device().features().wide_lines ? std::max(1.0f, S.line_width) : 1.0f;
        if (ch(g_dsh.line_width, lw)) vkCmdSetLineWidth(cb, lw);
    }
    if (topo != g_topo) {
        vkCmdSetPrimitiveTopology(cb, topo);
        g_topo = topo;
    }
    const bool restart = S.prim_restart && indexed &&
                         ((index_type == NVN_INDEX_TYPE_UNSIGNED_INT && S.restart_index == 0xffffffffu) || (index_type == NVN_INDEX_TYPE_UNSIGNED_SHORT && S.restart_index == 0xffffu) ||
                          (index_type == NVN_INDEX_TYPE_UNSIGNED_BYTE && S.restart_index == 0xffu));
    if (!g_restart_valid || restart != g_restart || (!co && (dirty & kDynRaster))) {
        vkCmdSetPrimitiveRestartEnable(cb, restart);
        g_restart = restart;
        g_restart_valid = true;
    }
    const bool have_depth = P.depth_fmt != VK_FORMAT_UNDEFINED;
    if (dirty & kDynDepth) {
        const bool dt = have_depth && S.ds.depth_test, dw = have_depth && !P.depth_ro && S.ds.depth_test && S.ds.depth_write;
        const VkCompareOp dc = vk_compare_op(S.ds.depth_func ? S.ds.depth_func : NVN_DEPTH_FUNC_LESS);
        const bool dbt = have_depth && S.depth_bounds && device().features().depth_bounds;
        if (ch(g_dsh.depth_test, static_cast<uint32_t>(dt))) vkCmdSetDepthTestEnable(cb, dt);
        if (ch(g_dsh.depth_write, static_cast<uint32_t>(dw))) vkCmdSetDepthWriteEnable(cb, dw);
        if (ch(g_dsh.depth_cmp, static_cast<uint32_t>(dc))) vkCmdSetDepthCompareOp(cb, dc);
        if (ch(g_dsh.depth_bounds_test, static_cast<uint32_t>(dbt))) vkCmdSetDepthBoundsTestEnable(cb, dbt);
        if (S.depth_bounds && device().features().depth_bounds && ch(g_dsh.depth_bounds, std::array<float, 2>{S.db_near, S.db_far})) vkCmdSetDepthBounds(cb, S.db_near, S.db_far);
    }
    if (dirty & kDynStencil) {
        const bool ste = P.has_stencil && S.ds.stencil_test;
        if (ch(g_dsh.stencil_test, static_cast<uint32_t>(ste))) vkCmdSetStencilTestEnable(cb, ste);
        for (int f = 0; f < 2; ++f) {
            const VkStencilFaceFlags face = f == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT;
            const std::array<uint32_t, 5> op = {static_cast<uint32_t>(face), static_cast<uint32_t>(vk_stencil_op(S.ds.stencil_fail[f])), static_cast<uint32_t>(vk_stencil_op(S.ds.depth_pass[f])),
                                                static_cast<uint32_t>(vk_stencil_op(S.ds.depth_fail[f])), static_cast<uint32_t>(vk_compare_op(S.ds.stencil_func[f] ? S.ds.stencil_func[f] : NVN_STENCIL_FUNC_ALWAYS))};
            if (ch(g_dsh.stencil_op[f], op))
                vkCmdSetStencilOp(cb, face, static_cast<VkStencilOp>(op[1]), static_cast<VkStencilOp>(op[2]), static_cast<VkStencilOp>(op[3]), static_cast<VkCompareOp>(op[4]));
            if (ch(g_dsh.stencil_ref[f], static_cast<uint32_t>(S.stencil_ref[f]))) vkCmdSetStencilReference(cb, face, S.stencil_ref[f]);
            if (ch(g_dsh.stencil_cmask[f], static_cast<uint32_t>(S.stencil_vmask[f]))) vkCmdSetStencilCompareMask(cb, face, S.stencil_vmask[f]);
            const uint32_t wm = P.depth_ro ? 0u : static_cast<uint32_t>(S.stencil_wmask[f]);
            if (ch(g_dsh.stencil_wmask[f], wm)) vkCmdSetStencilWriteMask(cb, face, wm);
        }
    }
    if (dirty & kDynBias) {
        const int off = S.poly.offset_enables;
        const int mode = S.poly.polygon_mode;
        const bool bias = (mode == NVN_POLYGON_MODE_FILL && (off & NVN_POLYGON_OFFSET_ENABLE_FILL)) ||
                          (mode == NVN_POLYGON_MODE_LINE && (off & NVN_POLYGON_OFFSET_ENABLE_LINE)) ||
                          (mode == NVN_POLYGON_MODE_POINT && (off & NVN_POLYGON_OFFSET_ENABLE_POINT));
        if (ch(g_dsh.bias_enable, static_cast<uint32_t>(bias))) vkCmdSetDepthBiasEnable(cb, bias);
        const float bclamp = device().features().depth_bias_clamp ? S.bias_clamp : 0.0f;
        if (ch(g_dsh.bias, std::array<float, 3>{S.bias_units, bclamp, S.bias_factor})) vkCmdSetDepthBias(cb, S.bias_units, bclamp, S.bias_factor);
    }
    if (dirty & kDynBlend) {
        const std::array<float, 4> bc = {S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]};
        if (ch(g_dsh.blend, bc)) vkCmdSetBlendConstants(cb, S.blend_color);
    }
}

// Evaluation-only programs (NVN tessellates with the fixed SetInner/OuterTessellationLevels): a generated pass-through control shader
// (nvn_tess_gen.h), one per (vertex shader, evaluation shader, patch size). It copies each per-vertex input of the evaluation shader from the
// vertex shader output at the same location (zero where the vertex shader writes none) and gl_Position, zeroes the per-patch inputs, and reads
// the levels from push constants (pushed before each such draw, so level changes need no new pipeline). Null when an interface variable has a
// type the generator does not handle.
static const ShaderInfo* g_gen_tcs = nullptr;   // the generated control shader of the current draw (null: the program has its own, or none)
static const ShaderInfo* passthrough_tcs_for(const ShaderInfo* vs, const ShaderInfo* tes) {
    struct Key {
        const ShaderInfo* vs; const ShaderInfo* tes; uint32_t n;
        bool operator<(const Key& o) const { return std::tie(vs, tes, n) < std::tie(o.vs, o.tes, o.n); }
    };
    static std::map<Key, std::unique_ptr<ShaderInfo>> cache;
    const uint32_t n = std::clamp<uint32_t>(S.patch_size, 1, 32);
    const Key key{vs, tes, n};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    auto to_io = [](const ShaderIo& v, tessgen::Io& o) {
        if (v.components < 1 || v.components > 4) return false;
        if (v.type == ScalarType::Float) o.kind = tessgen::Kind::Float;
        else if (v.type == ScalarType::Sint) o.kind = tessgen::Kind::Sint;
        else if (v.type == ScalarType::Uint) o.kind = tessgen::Kind::Uint;
        else return false;
        o.location = v.location;
        o.component = v.component;
        o.components = v.components;
        return true;
    };
    std::vector<tessgen::Io> vs_out, per_vertex, per_patch;
    bool ok = true;
    for (const ShaderIo& o : vs->refl.outputs) {   // an output array (several locations) is not matched: its readers get zero
        tessgen::Io io;
        if (o.array <= 1 && to_io(o, io)) vs_out.push_back(io);
    }
    for (const ShaderIo& in : tes->refl.inputs) {
        tessgen::Io io;
        if (!to_io(in, io)) { ok = false; break; }
        (in.array > 1 ? per_vertex : per_patch).push_back(io);   // in an evaluation shader a non-array input is per patch
    }
    std::unique_ptr<ShaderInfo> si;
    if (ok) {
        const std::vector<tessgen::Copy> copies = tessgen::match(vs_out, per_vertex);
        size_t copied = 0;
        for (const tessgen::Copy& c : copies) copied += c.has_src;
        si = std::make_unique<ShaderInfo>();
        si->hash = "tcs-pass-" + vs->hash + "-" + tes->hash + "-" + std::to_string(n);
        si->stage = ShaderStage::TessControl;
        // gl_ClipDistance / gl_CullDistance: copied when the vertex shader writes them and the evaluation shader reads gl_in[]'s
        auto uses = [](const std::vector<uint32_t>& v, uint32_t b) { return std::find(v.begin(), v.end(), b) != v.end(); };
        constexpr uint32_t kBuiltInClip = 3, kBuiltInCull = 4;
        const bool clip = uses(vs->refl.builtins_used_out, kBuiltInClip) && uses(tes->refl.builtins_used_in, kBuiltInClip);
        const bool cull = uses(vs->refl.builtins_used_out, kBuiltInCull) && uses(tes->refl.builtins_used_in, kBuiltInCull);
        si->spirv = tessgen::passthrough_tcs(copies, per_patch, n, 7, 1, clip, cull);
        si->push_bytes = tessgen::kPushBytes;
        std::string err;
        if (!reflect_spirv(si->spirv.data(), si->spirv.size(), si->refl, &err)) {
            NXLOG_W(kTag, "generated control shader for %s does not reflect: %s", tes->hash.c_str(), err.c_str());
            si.reset();
        } else {
            NXLOG_I(kTag, "evaluation shader %s (vs %s): pass-through control shader, %u vertices, %zu per-vertex locations (%zu from the vertex shader, %zu zero), %zu per-patch%s%s",
                    tes->hash.c_str(), vs->hash.c_str(), n, copies.size(), copied, copies.size() - copied, per_patch.size(), clip ? ", clip distances" : "",
                    cull ? ", cull distances" : "");
        }
    } else {
        NXLOG_W(kTag, "evaluation shader %s: an input is not a 1..4 component float/int, no pass-through control shader", tes->hash.c_str());
    }
    const ShaderInfo* out = si.get();
    cache.emplace(key, std::move(si));
    return out;
}
static const ShaderInfo* tcs_now() { return S.stage[kTessCtl] ? S.stage[kTessCtl] : g_gen_tcs; }

// Prepares everything a draw needs and binds it. False: skip the draw.
static bool begin_draw(int mode, bool indexed, int index_type, VkPrimitiveTopology* topo_out) {
    RpScope rp_all(kRpDraw);
    PoolReasonScope pr_(kReasonVertex);
    ResolveScope rs_;
    const ShaderInfo* vs = S.stage[kVertex];
    if (!vs) { log_once(kOnceDrawNoVs, "draw without a vertex shader skipped"); return false; }
    if (!S.render_enable) return false;
    ++g_draw_idx;
    if (key_check_on()) key_check_draw(counters().presents.load(std::memory_order_relaxed));
    if (S.stage[kFragment] && skip_fs_draw(S.stage[kFragment]->hash)) return false;
    // Tessellation: Vulkan runs it only with both shaders and a patch list. NVN also allows an evaluation shader alone (fixed levels from
    // SetInner/OuterTessellationLevels): those draws get a generated pass-through control shader (passthrough_tcs_for).
    if (S.stage[kTessCtl] || S.stage[kTessEval] || mode == NVN_DRAW_PRIMITIVE_PATCHES) {
        if (mode != NVN_DRAW_PRIMITIVE_PATCHES) { note_unsupported("tessellation draw: tessellation program drawn without patches"); return false; }
        if (!S.stage[kTessEval]) { note_unsupported("tessellation draw: patches without a tessellation evaluation shader"); return false; }
        if (!S.stage[kTessCtl]) {
            const ShaderInfo* gen = passthrough_tcs_for(vs, S.stage[kTessEval]);
            if (!gen) { note_unsupported("tessellation draw: evaluation shader without a control shader, no pass-through could be made"); return false; }
            if (gen != g_gen_tcs) g_dirty |= kPipe;
            g_gen_tcs = gen;
        }
    } else {
        g_gen_tcs = nullptr;
    }
    // The layout depends on the stages only: they change with BindProgram (kPipe) and the generated control shader (kPipe above);
    // begin_submit and the full resets set kPipe too. kDesc alone (a new binding) keeps the same layout.
    if (g_dirty & kPipe) {
        const ShaderInfo* stages[6] = {vs, S.stage[kFragment], S.stage[kGeometry], tcs_now(), S.stage[kTessEval], nullptr};
        g_pl = prog_layout(stages);
    }
    if (!g_trigger_fs.empty() && !g_trigger_done && trigger_time_ok() && S.stage[kFragment] && S.stage[kFragment]->hash.compare(0, g_trigger_fs.size(), g_trigger_fs) == 0) {
        g_trigger_seen = true;
        g_trigger_present = static_cast<int64_t>(counters().presents.load());
    }
    const ProgLayout* pl = g_pl;
    if (!pl) return false;
    if (pl->c0_graphics && g_draw_base_instance != g_bank_base_instance) g_dirty |= kDesc;   // c[0] + 0x644 changed: new sets
    rs_.arm(pl);
    if (extras::temporal_wanted()) temporal_gbuffer_note();
    if (g_dirty & (kDesc | kPipe)) {
        RpScope rp_(kRpPrep);
        g_prog_writes = false;
        for (const BindingDesc& d : pl->bindings) g_prog_writes |= d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        attach_targets(false);
        g_want_depth_ro = samples_depth_target(*pl);
        prepare_textures(*pl);
        if (g_trace) {   // [nvn] trace_dump_tex: also the block-compressed level 0 as the GPU holds it, to compare with the prepared data
            static const std::string dump_dir = nxrt::config_get_str("nvn", "trace_dump_tex", "");
            static std::unordered_map<uint32_t, bool> raw_done;
            if (!dump_dir.empty()) {
                for (const BindingDesc& d : pl->bindings) {
                    if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
                    const TexResolved r = resolve_texture(d);
                    if (!r.tex || !raw_done.emplace(r.tex->id, true).second) continue;
                    end_pass();
                    const int ri = snapshot_record_raw(r.tex, *F);
                    if (ri >= 0) g_trace_snaps.push_back({ri, g_trace_n, dump_dir + "/gpu_t" + std::to_string(r.tex->id) + ".bin", "raw"});
                }
            }
        }
        scene_upscale_for_draw(*pl);
    }
    if (g_fb_seen || (g_dirty & (kDesc | kPipe))) feedback_prepare(*pl);
    bool pass_ok;
    { RpScope rp_(kRpPass); pass_ok = ensure_pass(g_want_depth_ro, g_fbl_want); }
    if (!pass_ok) { log_once(kOnceDrawNoTarget, "draw without a usable render target skipped"); return false; }
    if (g_prog_writes || g_xfb_active) g_pass_unsafe = true;
    const VkPrimitiveTopology topo = vk_topology(mode);
    if ((g_dirty & kPipe) || topo != g_pipe_topo || !g_pipe || g_pipe_fbl != (P.fbl != 0)) {
        RpScope rp_(kRpPipe);
        GraphicsKey key;
        build_key(key, topo);
        key.fb_loop = P.fbl != 0 ? 1 : 0;   // the pass has an attachment in the feedback loop layout
        g_pipe_fbl = key.fb_loop != 0;
        g_pipe = graphics_pipeline(key, *pl);
        g_pipe_xfb = key.xfb != 0;
        g_pipe_topo = topo;
        g_key_attr_count = key.attr_count;
        g_key_dummy_binding = key.dummy_binding;
        for (uint32_t i = 0; i < key.attr_count; ++i) g_key_binding[i] = key.attr[i].binding;
    }
    if (!g_pipe) return false;
    // a draw inside a stream-out section that has no stream-out variant and nothing to render to has no effect
    if (g_xfb_active && !g_pipe_xfb && !P.ncolor && !S.depth_tex) return false;
    if (g_trace) {
        char a[96], b[96], c[96];
        std::string line;
        auto remember = [](Texture* t) {
            if (!t) return;
            for (const Ref<Texture>& r : g_trace_targets) if (r.get() == t) return;
            g_trace_targets.push_back(Ref<Texture>::share(t));
        };
        for (uint32_t i = 0; i < P.ncolor; ++i) remember(S.color_tex[i].get());
        remember(S.depth_tex.get());
        for (uint32_t i = 0; i < P.ncolor; ++i) { line += tex_desc(S.color_tex[i].get(), a, sizeof a); line += " | "; }
        TRACE("draw #%d vs %s fs %s gs %s mode %d idx %d | rt %s ds %s%s | blend %u mask %x depth t%d w%d f%d cull %d | ubo0 %llx/%llu",
              ++g_trace_n, S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-",
              S.stage[kGeometry] ? S.stage[kGeometry]->hash.c_str() : "-", mode, indexed ? 1 : 0, line.c_str(), tex_desc(S.depth_tex.get(), b, sizeof b),
              P.depth_ro ? " (ro)" : "", S.color.blend_enable_mask, S.chmask.mask[0], S.ds.depth_test ? 1 : 0, S.ds.depth_write ? 1 : 0, S.ds.depth_func,
              S.poly.cull_face, static_cast<unsigned long long>(S.ubo[kVertex][0].addr), static_cast<unsigned long long>(S.ubo[kVertex][0].size));
        TRACE("    poly mode %d offset %x factor %g units %g clamp %g | depth mode %d origin %d clamp %d", S.poly.polygon_mode, S.poly.offset_enables, S.bias_factor, S.bias_units,
              S.bias_clamp, S.depth_mode, S.window_origin, S.depth_clamp ? 1 : 0);
        if (S.color.blend_enable_mask)
            TRACE("    blend rt0 rgb %d,%d,%d a %d,%d,%d (nvn enums src,dst,eq)", S.blend[0].src_rgb, S.blend[0].dst_rgb, S.blend[0].eq_rgb, S.blend[0].src_a, S.blend[0].dst_a, S.blend[0].eq_a);
        // [nvn] trace_ubo_bytes = N (NXRT_NVN_TRACE_UBO_BYTES, default 768, at most 65536): how many leading bytes of each uniform block the trace prints
        static const uint64_t ubo_bytes = static_cast<uint64_t>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "trace_ubo_bytes", 768), 16, 65536));
        for (int stg = kVertex; stg <= kFragment; ++stg)
        for (uint32_t slot = 0; slot < 16; ++slot) {    // the first words of every uniform block bound to the vertex and the fragment stage
            const GState::Bind& ub = S.ubo[stg][slot];
            if (!ub.addr) continue;
            BufRef ur = resolve_buffer(ub.addr, std::min<uint64_t>(ub.size, ubo_bytes));
            std::string words;
            if (ur.ok) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(ub.addr);
                for (uint32_t i = 0; i * 4 < ubo_bytes && i * 4 < ub.size; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
            }
            TRACE("    %s ubo%u %llx/%llu%s:%s", stg == kVertex ? "vs" : "fs", slot, static_cast<unsigned long long>(ub.addr), static_cast<unsigned long long>(ub.size), ur.ok ? "" : " (outside every pool)", words.c_str());
        }
        TRACE("    viewport %u: %g %g %g %g depth %g..%g scissor %u: %d %d %d %d", S.nvp, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0][0], S.dr[0][1], S.nsc,
              S.sc[0][0], S.sc[0][1], S.sc[0][2], S.sc[0][3]);
        for (uint32_t i = 0; i < P.ncolor; ++i)
            if (Texture* ct = S.color_tex[i].get()) {
                const FormatInfo* vf = (S.color_has_view[i] && S.color_view[i].has_format) ? format_info(S.color_view[i].format) : nullptr;
                TRACE("    rt%u t%u mem %p storage %llu cpu-dirty %d view level %u layer %u layers %u format %s", i, ct->id, static_cast<void*>(ct->mem),
                      static_cast<unsigned long long>(ct->storage_size), ct->cpu_dirty.load() ? 1 : 0, S.color_level[i], S.color_layer[i], S.color_layers[i],
                      vf && vf->name ? vf->name : "-");
            }
        g_trace_sampled.clear();
        for (const BindingDesc& d : pl->bindings) {   // texel buffers (light lists and other BUFFER-target textures): the view the draw gets
            if (d.type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) continue;
            const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            const TexResolved r = storage ? resolve_image(d) : resolve_texture(d);
            if (!r.tex) { TRACE("    texel set%u b%u -> (no texture)", d.set, d.binding); continue; }
            uint64_t addr = 0, bytes = 0;
            const VkBufferView v = tex_buffer_view(r.tex, r.has_view ? &r.view : nullptr, 0, VK_FORMAT_UNDEFINED, storage, &addr, &bytes);
            std::string words;
            if (r.tex->mem) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(r.tex->mem);
                const size_t nw = std::min<size_t>(32, static_cast<size_t>(r.tex->b.width) * (r.tex->fmt ? r.tex->fmt->bytes : 4) / 4);
                char wb[12];
                for (size_t i = 0; i < nw; ++i) { std::snprintf(wb, sizeof wb, " %08x", w[i]); words += wb; }
                const size_t all = static_cast<size_t>(r.tex->b.width) * (r.tex->fmt ? r.tex->fmt->bytes : 4) / 4;
                size_t nz = 0;
                for (size_t i = 0; i < all; ++i) nz += w[i] != 0;
                std::snprintf(wb, sizeof wb, " |nz %zu", nz);
                words = std::string(wb) + "/" + std::to_string(all) + words;
            }
            TRACE("    texel set%u b%u -> t%u %s width %d @%p view %s addr %llx bytes %llu view-format %s:%s", d.set, d.binding, r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width,
                  static_cast<void*>(r.tex->mem), v ? "ok" : "NONE (dummy)", static_cast<unsigned long long>(addr), static_cast<unsigned long long>(bytes),
                  (r.has_view && r.view.has_format) ? "set" : "-", words.c_str());
        }
        for (const BindingDesc& d : pl->bindings) {
            if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
            const TexResolved r = d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? resolve_image(d) : resolve_texture(d);
            if (r.tex) g_trace_sampled.push_back(r.tex);
            if (r.tex) {
                static const std::string dump_dir = nxrt::config_get_str("nvn", "trace_dump_tex", "");
                if (!dump_dir.empty()) trace_dump_prepared(r.tex, dump_dir);
            }
            char smp[200] = "", vw[160] = "";
            if (r.has_sampler)
                std::snprintf(smp, sizeof smp, " | sampler min %d mag %d wrap %d,%d,%d lod %g..%g bias %g aniso %g cmp %d func %d", r.smp.min_filter, r.smp.mag_filter, r.smp.wrap_s, r.smp.wrap_t,
                              r.smp.wrap_r, r.smp.lod_min, r.smp.lod_max, r.smp.lod_bias, r.smp.max_anisotropy, r.smp.compare_mode, r.smp.compare_func);
            if (r.has_view) {
                // the view's own format by name (a view naming the sRGB partner of its texture's format, or the reverse, changes the decode)
                const FormatInfo* vf = r.view.has_format ? format_info(r.view.format) : nullptr;
                char vfn[48] = "";
                if (r.view.has_format) std::snprintf(vfn, sizeof vfn, " format %s", vf && vf->name ? vf->name : "?");
                std::snprintf(vw, sizeof vw, " | view levels %d+%d layers %d+%d%s", r.view.has_levels ? r.view.min_level : -1, r.view.has_levels ? r.view.num_levels : -1,
                              r.view.has_layers ? r.view.min_layer : -1, r.view.has_layers ? r.view.num_layers : -1, vfn);
            }
            if (r.tex) {
                const int* sw = (r.has_view && r.view.has_swizzle) ? r.view.swizzle : r.tex->b.swizzle;
                const size_t n = std::strlen(vw);
                std::snprintf(vw + n, sizeof vw - n, " swz %d%d%d%d", sw[0], sw[1], sw[2], sw[3]);
            }
            TRACE("    %s set%u b%u %s -> %s @%p%s mem %d%%%s%s%s", d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "image" : "tex", d.set, d.binding,
                  d.shader->refl.bindings[d.index].name.c_str(), tex_desc(r.tex, c, sizeof c), r.tex ? static_cast<void*>(r.tex->mem) : nullptr, r.has_sampler ? "" : " (no sampler)", storage_nonzero_pct(r.tex),
                  r.tex && r.tex->cpu_dirty.load() ? " cpu-dirty" : "", smp, vw);
        }
    }
    if (g_xfb_active) {
        static std::unordered_map<std::string, bool> seen;
        if (seen.emplace(S.stage[kVertex]->hash, true).second)
            NXLOG_I("nvn.xfb", "draw under transform feedback: vs %s mode %d attrs %u xfb0 %llx/%llu", S.stage[kVertex]->hash.c_str(), mode, S.attr_count,
                    static_cast<unsigned long long>(S.xfb[0].addr), static_cast<unsigned long long>(S.xfb[0].size));
    }
    if (g_pipe != g_bound_pipeline) {
        xfb_end_in_pass();     // the pipeline cannot change while feedback is active; the byte counter carries the position over
        vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
        g_gauge_pipeline_binds.add(1);
        g_bound_pipeline = g_pipe;
    }
    if (g_xfb_active && g_pipe_xfb && !g_xfb_began && device().features().transform_feedback && S.xfb[0].addr) {
        BufRef xb = resolve_buffer(S.xfb[0].addr, S.xfb[0].size);
        if (xb.ok) {
            gpu_wrote(S.xfb[0].addr, S.xfb[0].size ? S.xfb[0].size : xb.avail, kWriteXfb);
            if (!g_xfb_counter) {
                if (device().create_buffer(kXfbSlots * kXfbSlotBytes, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Upload,
                                           &g_xfb_counter, "nvn xfb counter") != VK_SUCCESS)
                    g_xfb_counter = gfx::Buffer();
            }
            const VkDeviceSize size = std::min<VkDeviceSize>(S.xfb[0].size ? S.xfb[0].size : xb.avail, xb.avail) & ~static_cast<VkDeviceSize>(3);
            vkCmdBindTransformFeedbackBuffersEXT(F->cb, 0, 1, &xb.buffer, &xb.offset, &size);
            VkBuffer cbuf = g_xfb_counter.buffer;
            const VkDeviceSize coff = static_cast<VkDeviceSize>(g_xfb_cur) * kXfbSlotBytes;
            if (g_xfb_have_counter && cbuf) vkCmdBeginTransformFeedbackEXT(F->cb, 0, 1, &cbuf, &coff);
            else vkCmdBeginTransformFeedbackEXT(F->cb, 0, 0, nullptr, nullptr);
            g_xfb_began = true;
        } else {
            note_unsupported("transform feedback into an address outside every pool");
        }
    }
    { RpScope rp_(kRpDyn); set_dynamic_state(g_dirty, topo, indexed, index_type); }
    if (g_dirty & kDesc) {
        RpScope rp_(kRpSets);
        support_for_draw(*pl, false);
        bind_sets(*pl, VK_PIPELINE_BIND_POINT_GRAPHICS);
        g_bank_base_instance = g_draw_base_instance;
    }
    if (pl->push_bytes && g_gen_tcs) {
        // the pass-through control shader's tessellation levels (NVN's fixed SetInner/OuterTessellationLevels), every draw: cheap, and
        // the layout may have changed since the last push
        tessgen::TessPush push = {};
        std::memcpy(push.outer, S.tess_outer, sizeof push.outer);
        std::memcpy(push.inner, S.tess_inner, sizeof push.inner);
        vkCmdPushConstants(F->cb, pl->layout, pl->push_stages, 0, std::min<uint32_t>(pl->push_bytes, sizeof push), &push);
        static std::set<const ShaderInfo*> logged;
        if (logged.insert(g_gen_tcs).second)
            NXLOG_I(kTag, "tessellation draw: evaluation shader %s drawn with pass-through control shader %s, patch size %u, levels %g %g %g %g / %g %g",
                    S.stage[kTessEval]->hash.c_str(), g_gen_tcs->hash.c_str(), static_cast<unsigned>(std::clamp<uint32_t>(S.patch_size, 1, 32)),
                    S.tess_outer[0], S.tess_outer[1], S.tess_outer[2], S.tess_outer[3], S.tess_inner[0], S.tess_inner[1]);
    }
    if (g_dirty & (kVb | kPipe)) {
        RpScope rp_(kRpVb);
        const bool co = bind_coalesce_on();
        // pending run of bindings to bind with one call
        VkBuffer rb[16];
        VkDeviceSize ro[16], rs[16], rst[16];
        uint32_t rfirst = 0, rn = 0;
        auto flush = [&] {
            if (!rn) return;
            vkCmdBindVertexBuffers2(F->cb, rfirst, rn, rb, ro, rs, rst);
            if (rn > 1) ft::split_count(ft::kCntMerged, rn - 1);
            g_vb_bound_in_cb = true;
            rn = 0;
        };
        auto add = [&](uint32_t binding, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, VkDeviceSize stride) {
            if (co && binding < 16) {
                VbShadow& sh = g_vbsh[binding];
                if (sh.ok && sh.buf == buf && sh.off == off && sh.size == size && sh.stride == stride) { ft::split_count(ft::kCntSkipped); flush(); return; }
                sh = {buf, off, size, stride, true};
                if (rn && rfirst + rn == binding) {
                    rb[rn] = buf; ro[rn] = off; rs[rn] = size; rst[rn] = stride; ++rn;
                    return;
                }
                flush();
                rfirst = binding;
                rb[0] = buf; ro[0] = off; rs[0] = size; rst[0] = stride;
                rn = 1;
                return;
            }
            vkCmdBindVertexBuffers2(F->cb, binding, 1, &buf, &off, &size, &stride);
            g_vb_bound_in_cb = true;
        };
        // bindings in ascending order so that neighbours merge
        uint32_t order[16], norder = 0;
        for (uint32_t i = 0; i < g_key_attr_count; ++i) {
            const uint32_t binding = g_key_binding[i];
            bool done = false;
            for (uint32_t j = 0; j < norder; ++j) if (order[j] == binding) done = true;
            if (!done && norder < 16) order[norder++] = binding;
        }
        if (co) std::sort(order, order + norder);
        for (uint32_t k = 0; k < norder; ++k) {
            const uint32_t binding = order[k];
            if (g_key_dummy_binding && binding == static_cast<uint32_t>(g_key_dummy_binding - 1)) {
                add(binding, dummy_buffer(), kDummyAttrFloatOffset, 32, 0);
                continue;
            }
            const GState::Bind& vb = S.vtx[binding];
            BufRef br = vb.addr ? resolve_read(vb.addr, vb.size, kReadVertex) : BufRef{dummy_buffer(), 0, 65536, false};
            VkDeviceSize size = vb.size ? std::min<VkDeviceSize>(vb.size, br.avail) : br.avail;
            VkDeviceSize stride = static_cast<VkDeviceSize>(S.stream[binding].stride);
            add(binding, br.buffer, br.offset, size, stride);
        }
        flush();
        if (!g_vb_bound_in_cb) {
            // the pipeline's vertex binding strides are dynamic: a command buffer that never bound a vertex buffer must still bind one
            VkBuffer dummy = dummy_buffer();
            VkDeviceSize off = 0, size = 65536, stride = 0;
            vkCmdBindVertexBuffers2(F->cb, 0, 1, &dummy, &off, &size, &stride);
            g_vb_bound_in_cb = true;
            if (co) g_vbsh[0] = {dummy, off, size, stride, true};
        }
    }
    g_dirty = g_mirror_stale ? (kVb | kDesc) : 0;     // a storage buffer of this draw made copied pages stale: the next draw binds again
    g_mirror_stale = false;
    *topo_out = topo;
    return true;
}

// Counts a draw for the primitive counters, before the replay decides whether to skip it: the console generates the primitives of every
// draw it runs, and a draw of a feedback section also writes them (14). A draw with rendering disabled is not run by the console: not counted.
static void count_primitives(uint32_t mode, uint32_t count, uint32_t instances) {
    if (!S.render_enable) return;
    const uint64_t per = mode == NVN_DRAW_PRIMITIVE_PATCHES ? count / std::max<uint32_t>(1, S.patch_size) : primitives_for(static_cast<int>(mode), count);   // patches: input patches
    const uint64_t prims = per * std::max<uint32_t>(1, instances);
    g_prims_generated += prims;
    if (g_xfb_active) g_prims_xfb += prims;
}

// Quads, quad strips, polygons and line loops, which Vulkan has no topology for, as the triangle (or line strip) index list of the same vertices.
// `in` holds the vertex indices of the draw in order (for DrawArrays: first, first + 1, ...). Shared by the direct and the indirect draws.
static std::vector<uint32_t> convert_prim_indices(PrimConversion conv, const std::vector<uint32_t>& in) {
    std::vector<uint32_t> idx;
    const size_t n = in.size();
    switch (conv) {
        case PrimConversion::Quads:
            for (size_t q = 0; q + 3 < n; q += 4) idx.insert(idx.end(), {in[q], in[q + 1], in[q + 2], in[q], in[q + 2], in[q + 3]});
            break;
        case PrimConversion::QuadStrip:
            for (size_t q = 0; q + 3 < n; q += 2) idx.insert(idx.end(), {in[q], in[q + 1], in[q + 2], in[q + 2], in[q + 1], in[q + 3]});
            break;
        case PrimConversion::Polygon:
            for (size_t k = 1; k + 1 < n; ++k) idx.insert(idx.end(), {in[0], in[k], in[k + 1]});
            break;
        case PrimConversion::LineLoop:
            idx = in;
            if (!in.empty()) idx.push_back(in[0]);
            break;
        default: idx = in; break;
    }
    return idx;
}

// Binds an index list of 32-bit indices from the upload buffer. False: no room.
static bool bind_u32_indices(const std::vector<uint32_t>& idx) {
    UploadAlloc u = F->upload(idx.size() * 4, 4);
    if (!u) return false;
    std::memcpy(u.ptr, idx.data(), idx.size() * 4);
    bind_index_buffer(F->cb, u.buffer, u.offset, VK_INDEX_TYPE_UINT32);
    return true;
}

// [gfx] gpu_checkpoints = all: a checkpoint before the draw names it (vertex and fragment shader, counts) if the device is lost while it runs.
// One compare when off or without NVIDIA's extension. Indirect: a, b = the guest address of the arguments, c = the draw count limit.
static void checkpoint_draw(uint32_t a, uint32_t b, uint32_t c, bool indirect) {
    gfx::Device& d = device();
    if (!d.checkpoints_on(gfx::Device::Checkpoint::Draw)) return;
    d.checkpoint(F->cb, gfx::Device::Checkpoint::Draw, S.stage[kVertex] ? &S.stage[kVertex]->hash : nullptr,
                 S.stage[kFragment] ? &S.stage[kFragment]->hash : nullptr, a, b, c, indirect);
}

void do_draw_arrays(const P_DrawArrays& p) {
    count_primitives(p.mode, p.count, p.instance_count);
    g_draw_base_instance = p.base_instance;
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    checkpoint_draw(p.count, std::max<uint32_t>(1, p.instance_count), 0, false);
    if (conv == PrimConversion::None) {
        TRACE("    vertices %u instances %u first %u", p.count, p.instance_count, p.first);
        vkCmdDraw(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), p.first, p.base_instance);
    } else {
        // QUADS / QUAD_STRIP / POLYGON / LINE_LOOP: generate an index buffer
        std::vector<uint32_t> in(p.count);
        for (uint32_t k = 0; k < p.count; ++k) in[k] = p.first + k;
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty()) return;
        if (!bind_u32_indices(idx)) return;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), std::max<uint32_t>(1, p.instance_count), 0, 0, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    g_pass_draws++;
    g_pass_verts += static_cast<uint64_t>(p.count) * std::max<uint32_t>(1, p.instance_count);
    trace_snap_after_draw();
}

void do_draw_elements(const P_DrawElements& p) {
    PoolReasonScope pr_(kReasonIndex);
    count_primitives(p.mode, p.count, p.instance_count);
    g_draw_base_instance = p.base_instance;
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), true, static_cast<int>(p.type), &topo)) { counters().draws_skipped.fetch_add(1); return; }
    if (g_trace) {   // the indices and the first attributes of the first vertices
        const uint32_t isz0 = index_size_bytes(static_cast<int>(p.type));
        const uint8_t* ix = host_ptr(p.index_address);
        TRACE("    indexed: count %u type %d instances %u base_vertex %d base_instance %u index address %llx", p.count, static_cast<int>(p.type), p.instance_count, static_cast<int>(p.base_vertex),
              p.base_instance, static_cast<unsigned long long>(p.index_address));
        for (uint32_t i = 0; ix && i < std::min<uint32_t>(p.count, 12); ++i) {
            uint32_t v = 0;
            if (isz0 == 1) v = ix[i]; else if (isz0 == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; } else std::memcpy(&v, ix + i * 4, 4);
            std::string a;
            for (uint32_t k = 0; k < S.attr_count && k < 3; ++k) {
                const VertexAttribInfo& at = S.attr[k];
                const auto& st = S.stream[at.stream];
                const GState::Bind& vb = S.vtx[at.stream];
                if (!vb.addr) continue;
                const float* f = reinterpret_cast<const float*>(vb.addr + static_cast<uint64_t>(static_cast<int64_t>(v) + p.base_vertex) * st.stride + at.relative_offset);
                char t[96]; std::snprintf(t, sizeof t, " a%u(fmt %d)=(%g %g %g)", k, at.format, f[0], f[1], f[2]);
                a += t;
            }
            TRACE("      index %u:%s", v, a.c_str());
        }
    }
    if (g_trace) {   // bounds of the first attributes over all indices of the draw (half and float formats decoded), to see what part of a texture a mesh maps
        const uint32_t isz1 = index_size_bytes(static_cast<int>(p.type));
        const uint8_t* ix = host_ptr(p.index_address);
        if (ix && p.count) {
            std::string a;
            for (uint32_t k = 0; k < S.attr_count && k < 3; ++k) {
                const VertexAttribInfo& at = S.attr[k];
                const auto& st = S.stream[at.stream];
                const GState::Bind& vb = S.vtx[at.stream];
                if (!vb.addr) continue;
                float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
                int nc = at.format == 41 ? 4 : at.format == 34 ? 3 : at.format == 17 ? 2 : at.format == 22 ? 2 : 0;
                for (uint32_t i = 0; nc && i < p.count && i < 20000; ++i) {
                    uint32_t v = 0;
                    if (isz1 == 1) v = ix[i]; else if (isz1 == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; } else std::memcpy(&v, ix + i * 4, 4);
                    const uint8_t* ve = reinterpret_cast<const uint8_t*>(vb.addr + static_cast<uint64_t>(static_cast<int64_t>(v) + p.base_vertex) * st.stride + at.relative_offset);
                    for (int c = 0; c < nc; ++c) {
                        float f;
                        if (at.format == 41 || at.format == 17) {
                            uint16_t h; std::memcpy(&h, ve + c * 2, 2);
                            const int e = (h >> 10) & 31, m = h & 1023; const float sg = (h & 0x8000) ? -1.f : 1.f;
                            f = e == 0 ? sg * m * (1.f / 16777216.f) : e == 31 ? sg * 65504.f : sg * (1.f + m / 1024.f) * std::ldexp(1.f, e - 15);
                        } else std::memcpy(&f, ve + c * 4, 4);
                        lo[c] = std::min(lo[c], f); hi[c] = std::max(hi[c], f);
                    }
                }
                char t[200];
                if (nc) std::snprintf(t, sizeof t, " a%u[%g..%g, %g..%g, %g..%g, %g..%g]", k, lo[0], hi[0], lo[1], hi[1], nc > 2 ? lo[2] : 0.f, nc > 2 ? hi[2] : 0.f, nc > 3 ? lo[3] : 0.f, nc > 3 ? hi[3] : 0.f);
                else std::snprintf(t, sizeof t, " a%u(fmt %d: not decoded)", k, at.format);
                a += t;
            }
            TRACE("    bounds:%s", a.c_str());
        }
    }
    const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    const uint8_t* src = host_ptr(p.index_address);
    checkpoint_draw(p.count, std::max<uint32_t>(1, p.instance_count), 0, false);
    if (conv != PrimConversion::None || isz == 1) {
        // widen to 32 bit and/or convert the primitive
        std::vector<uint32_t> in(p.count);
        for (uint32_t i = 0; i < p.count; ++i) {
            uint32_t v = 0;
            if (isz == 1) v = src[i];
            else if (isz == 2) { uint16_t t; std::memcpy(&t, src + i * 2, 2); v = t; }
            else std::memcpy(&v, src + i * 4, 4);
            in[i] = v;
        }
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty()) return;
        if (!bind_u32_indices(idx)) return;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    } else {
        BufRef br = resolve_read(p.index_address, static_cast<uint64_t>(p.count) * isz, kReadIndex);
        bind_index_buffer(F->cb, br.buffer, br.offset, vk_index_type(static_cast<int>(p.type)));
        vkCmdDrawIndexed(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    g_pass_draws++;
    g_pass_verts += static_cast<uint64_t>(p.count) * std::max<uint32_t>(1, p.instance_count);
    trace_snap_after_draw();
}

// The bytes at a GPU address as the CPU sees them, when they are current: inside a pool, and not a range a submission that has not completed
// may write (gw_settled). Null otherwise. For decisions that need data the GPU would otherwise hold (indirect arguments, draw counts).
// Compute results held in the device copy ([nvn] compute_out_lazy) are newer than the pool: they are fetched back first (out_fetch: a
// completed submission's result, or one whose copy-back is recorded but has not run); one the submission being recorded writes cannot be
// read (null). Without the fetch an unguarded held result would be read stale, and a guarded one through a fault.
const uint8_t* cpu_view(uint64_t addr, uint64_t size) {
    if (!addr || !size) return nullptr;
    const PoolHit hit = pool_lookup(addr);
    if (!hit.pool || hit.pool->is_virtual || !hit.pool->base || hit.offset + size > hit.pool->size) return nullptr;
    if (!gw_settled(addr, size)) return nullptr;
    if (out_any()) {
        out_fetch(addr, size, false);
        if (out_overlaps(addr, size)) return nullptr;
    }
    return host_ptr(addr);
}

// ---- indirect quads on the GPU ([nvn] indirect_quads_gpu, default on) ----------------------------------------------------------
// Based on pokopia-native (MIT) windows/src/nxrt/vk/vk_replay.c + shaders/quad_args.comp @27a6df0 (quad_args).
// A single non-indexed indirect quad draw whose arguments a command earlier in the same submission writes (particle effects: a compute
// pass counts the live quads) cannot be converted by the CPU (cpu_view: null) and was skipped (#120). A one-thread dispatch
// (nvn_quad_args_shader.inc) turns the arguments into those of an indexed indirect draw over a shared quad index list (0,1,2, 0,2,3, ...
// for 65536 quads, made once), outside the pass: begin_draw reopens it, loading what the targets hold. Counts are clamped to 65536 quads
// and 65536 instances, so a count the GPU has not written yet cannot keep it busy past the OS timeout.
#include "nvn_quad_args_shader.inc"
constexpr uint32_t kQuadMax = 65536;
QuadArgs g_qa;

static bool indirect_quads_gpu_on() {
    static const bool on = nxrt::config_get_bool("nvn", "indirect_quads_gpu", true);
    return on;
}

static bool quad_args_init() {
    QuadArgs& q = g_qa;
    if (q.tried) return q.ok;
    q.tried = true;
    VkDevice d = vkdev();
    q.ssbo_align = std::max<VkDeviceSize>(16, device().properties().limits.minStorageBufferOffsetAlignment);
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 2;
    sli.pBindings = b;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &q.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kQuadArgsComp;
    mi.pCode = kQuadArgsComp;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(d, &sli, nullptr, &q.set_layout) != VK_SUCCESS || vkCreatePipelineLayout(d, &pli, nullptr, &q.layout) != VK_SUCCESS ||
        vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) {
        NXLOG_W("nvn.replay", "indirect quads: the argument shader could not be made: those draws take the CPU path");
        return false;
    }
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = q.layout;
    const VkResult pr = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &q.pipe);
    vkDestroyShaderModule(d, m, nullptr);
    if (pr != VK_SUCCESS ||
        device().create_buffer(static_cast<VkDeviceSize>(kQuadMax) * 6 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               gfx::MemoryUse::GpuOnly, &q.index, "nvn quad indices") != VK_SUCCESS) {
        q.pipe = pr == VK_SUCCESS ? q.pipe : VK_NULL_HANDLE;
        NXLOG_W("nvn.replay", "indirect quads: the argument pipeline or the quad index buffer could not be made: those draws take the CPU path");
        return false;
    }
    q.ok = true;
    NXLOG_I("nvn.replay", "indirect quads whose arguments the GPU writes: made into an indexed indirect draw on the GPU ([nvn] indirect_quads_gpu)");
    return true;
}

// Records the dispatch that makes the indexed draw's 20 bytes of arguments from the 16 at p.indirect_address (ending the pass); *out = where
// they are. False: nothing recorded (the draw takes the CPU path).
static bool quad_args_gpu(const P_DrawIndirect& p, BufRef* out) {
    if (!quad_args_init()) return false;
    QuadArgs& q = g_qa;
    const BufRef src = held_or_pool(p.indirect_address, 16, kOutIndirect, false);   // a held compute result: the copy (out_before_op)
    if (!src.ok || src.offset % 4) return false;
    const VkDeviceSize base = src.offset / q.ssbo_align * q.ssbo_align;
    const UploadAlloc dst = F->upload(20, q.ssbo_align);
    if (!dst) return false;
    const VkDescriptorSet set = F->alloc_set(q.set_layout);
    if (!set) return false;
    const VkDescriptorBufferInfo bi[2] = {{src.buffer, base, src.offset - base + 16}, {dst.buffer, dst.offset, 20}};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = static_cast<uint32_t>(i);
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(vkdev(), 2, w, 0, nullptr);
    end_pass();
    if (!q.index_filled) {   // the shared quad index list, once (copied from this submission's staging)
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(kQuadMax) * 6 * 4;
        const UploadAlloc st = F->staging(bytes);
        if (!st) return false;
        uint32_t* ix = reinterpret_cast<uint32_t*>(st.ptr);
        for (uint32_t k = 0; k < kQuadMax; ++k) {
            const uint32_t v = k * 4;
            uint32_t* t = ix + static_cast<size_t>(k) * 6;
            t[0] = v; t[1] = v + 1; t[2] = v + 2; t[3] = v; t[4] = v + 2; t[5] = v + 3;
        }
        const VkBufferCopy r = {st.offset, 0, bytes};
        vkCmdCopyBuffer(F->cb, st.buffer, q.index.buffer, 1, &r);
        q.index_filled = true;
    }
    memory_barrier(F->cb);   // the GPU's own write of the arguments earlier in this submission (and the index copy) before the dispatch
    vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, q.pipe);
    vkCmdBindDescriptorSets(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, q.layout, 0, 1, &set, 0, nullptr);
    const uint32_t at = static_cast<uint32_t>((src.offset - base) / 4);
    vkCmdPushConstants(F->cb, q.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &at);
    vkCmdDispatch(F->cb, 1, 1, 1);
    memory_barrier(F->cb);   // the dispatch's write before the indirect read
    g_bound_pipeline = VK_NULL_HANDLE;
    out->buffer = dst.buffer;
    out->offset = dst.offset;
    out->ok = true;
    return true;
}

// An indirect draw Vulkan cannot take as it is (quads, quad strips, polygons and line loops have no topology; 8-bit indices need
// VK_EXT_index_type_uint8): each draw's arguments (and its indices) are read by the CPU and the draw is made as a direct indexed draw of
// the converted 32-bit index list, like DrawArrays / DrawElements. That needs the arguments in memory the CPU can read: arguments a compute
// shader or a copy wrote and no completed submission has handed back are not (skipped, logged). A multi-draw reads its count the same way.
static void draw_indirect_cpu(const P_DrawIndirect& p, PrimConversion conv, uint32_t isz, bool indexed) {
    static bool noted = false;
    if (!noted) {
        noted = true;
        NXLOG_I("nvn.replay", "replay: indirect draw by CPU index conversion (first): mode %u, %s, index size %u, %s", p.mode, indexed ? "indexed" : "arrays", isz,
                p.multi ? "multi-draw" : "single");
    }
    const uint64_t cmd_size = indexed ? 20 : 16;
    const uint64_t stride = p.stride > 0 ? static_cast<uint64_t>(p.stride) : cmd_size;
    uint32_t n = 1;
    if (p.multi && p.count_address) {
        const uint8_t* c = cpu_view(p.count_address, 4);
        if (!c) { note_unsupported("indirect draw conversion: the draw count is not readable by the CPU (written by the GPU)"); counters().draws_skipped.fetch_add(1); return; }
        std::memcpy(&n, c, 4);
        n = std::min(n, p.max_count);
    }
    bool drew = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* a = cpu_view(p.indirect_address + static_cast<uint64_t>(i) * stride, cmd_size);
        if (!a) { note_unsupported("indirect draw conversion: the arguments are not readable by the CPU (written by the GPU)"); break; }
        uint32_t v[5] = {};
        std::memcpy(v, a, cmd_size);
        const uint32_t count = v[0], instances = v[1];
        if (!count || !instances) continue;
        if (!indirect_args_plausible(count, instances)) { note_unsupported("indirect draw conversion: implausible arguments (stale guest memory?)"); continue; }
        std::vector<uint32_t> in(count);
        int32_t base_vertex = 0;
        uint32_t base_instance = v[3];
        if (indexed) {
            base_vertex = static_cast<int32_t>(v[3]);
            base_instance = v[4];
            const uint8_t* src = cpu_view(p.index_address + static_cast<uint64_t>(v[2]) * isz, static_cast<uint64_t>(count) * isz);
            if (!src) { note_unsupported("indirect draw conversion: the indices are not readable by the CPU (written by the GPU)"); break; }
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t x = 0;
                if (isz == 1) x = src[k];
                else if (isz == 2) { uint16_t t; std::memcpy(&t, src + k * 2, 2); x = t; }
                else std::memcpy(&x, src + k * 4, 4);
                in[k] = x;
            }
        } else {
            for (uint32_t k = 0; k < count; ++k) in[k] = v[2] + k;
        }
        count_primitives(p.mode, count, instances);
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty() || !bind_u32_indices(idx)) continue;
        checkpoint_draw(count, instances, 0, false);   // the CPU read the arguments: a direct draw
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), instances, 0, base_vertex, base_instance);
        drew = true;
    }
    if (drew) {
        counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
        g_pass_draws++;
    } else {
        counters().draws_skipped.fetch_add(1);
    }
    trace_snap_after_draw();
}

void do_draw_indirect(const P_DrawIndirect& p) {
    PoolReasonScope pr_(kReasonIndirect);
    const bool indexed = p.type != 0xffffffffu;
    // the base instance is in the arguments, which the GPU may write and a multi-draw holds one of per draw: the vertex shaders read it
    // as Vulkan's BaseInstance (spirv_base_instance_from_c0), so the c[0] word is not needed and stays 0
    g_draw_base_instance = 0;
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    // indirect quads whose arguments the CPU cannot read (the GPU writes them): the indexed arguments made on the GPU, before the pass opens
    BufRef qa{};
    if (conv == PrimConversion::Quads && !indexed && !p.multi && indirect_quads_gpu_on() && S.render_enable && S.stage[kVertex] &&
        !cpu_view(p.indirect_address, 16))
        quad_args_gpu(p, &qa);
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), indexed, indexed ? static_cast<int>(p.type) : 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    if (qa.ok) {
        bind_index_buffer(F->cb, g_qa.index.buffer, 0, VK_INDEX_TYPE_UINT32);
        checkpoint_draw(static_cast<uint32_t>(p.indirect_address), static_cast<uint32_t>(p.indirect_address >> 32), 1, true);
        vkCmdDrawIndexedIndirect(F->cb, qa.buffer, qa.offset, 1, 20);
        const uint64_t n = ++g_qa.draws;
        if ((n & (n - 1)) == 0)   // 1, 2, 4, 8, ...
            NXLOG_I("nvn.replay", "indirect quads drawn with GPU-made arguments: %llu so far", static_cast<unsigned long long>(n));
        counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
        g_pass_draws++;
        trace_snap_after_draw();
        return;
    }
    const uint32_t isz = indexed ? index_size_bytes(static_cast<int>(p.type)) : 0;
    if (conv != PrimConversion::None || (indexed && isz == 1 && !device().features().index_type_uint8)) {
        draw_indirect_cpu(p, conv, isz, indexed);
        return;
    }
    // GPU path. The primitives of these draws are not known to the CPU: the primitive counters see them through a query window (ResetCounter).
    static bool noted_indirect = false, noted_u8 = false;
    if (!noted_indirect) { noted_indirect = true; NXLOG_I("nvn.replay", "replay: indirect draw (first): mode %u, %s%s", p.mode, indexed ? "indexed" : "arrays", p.multi ? ", multi-draw" : ""); }
    if (indexed && isz == 1 && !noted_u8) { noted_u8 = true; NXLOG_I("nvn.replay", "replay: indirect draw with 8-bit indices (first): VK_EXT_index_type_uint8"); }
    const uint64_t cmd_size = indexed ? 20 : 16;
    const uint64_t n_cmds = p.multi && p.count_address ? std::max<uint64_t>(1, p.max_count) : 1;
    BufRef ib = held_or_pool(p.indirect_address, (p.stride ? static_cast<uint64_t>(p.stride) : cmd_size) * (n_cmds - 1) + cmd_size, kOutIndirect, false);
    if (indexed) {
        BufRef xb = resolve_buffer(p.index_address, 4);
        if (pool_copy_on()) pool_copy_touch(p.index_address, indirect_index_span(p));   // [nvn] pool_copy: every index the draw may read
        bind_index_buffer(F->cb, xb.buffer, xb.offset, vk_index_type(static_cast<int>(p.type)));
    }
    checkpoint_draw(static_cast<uint32_t>(p.indirect_address), static_cast<uint32_t>(p.indirect_address >> 32), static_cast<uint32_t>(n_cmds), true);
    if (p.multi && p.count_address) {
        BufRef cb = held_or_pool(p.count_address, 4, kOutIndirect, false);
        const uint32_t stride = static_cast<uint32_t>(p.stride ? p.stride : cmd_size);
        if (indexed) vkCmdDrawIndexedIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
        else vkCmdDrawIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
    } else if (indexed) {
        vkCmdDrawIndexedIndirect(F->cb, ib.buffer, ib.offset, 1, 20);
    } else {
        vkCmdDrawIndirect(F->cb, ib.buffer, ib.offset, 1, 16);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    g_pass_draws++;
    trace_snap_after_draw();
}

// DrawTransformFeedback(mode, control): draws what the feedback section begun with this control buffer wrote, as many vertices as it wrote
// (vkCmdDrawIndirectByteCountEXT over the section's byte counter, divided by the stride of the vertex stream that reads the feedback buffer). The game
// binds the feedback buffer as an ordinary vertex buffer. The feedback writes are ordered before the vertex fetch by ending the pass first.
void do_draw_xfb(const P_DrawXfb& p) {
    static bool noted = false;
    if (!noted) { noted = true; NXLOG_I("nvn.xfb", "replay: transform feedback draw (first): mode %u control %llx", p.mode, static_cast<unsigned long long>(p.control)); }
    if (!S.render_enable) return;
    if (!device().features().transform_feedback) { note_unsupported("DrawTransformFeedback (the device has no VK_EXT_transform_feedback)"); counters().draws_skipped.fetch_add(1); return; }
    const auto it = g_xfb_slot_of.find(p.control);
    if (!g_xfb_counter || it == g_xfb_slot_of.end() || !g_xfb_slot_written[it->second]) {
        TRACE("DrawTransformFeedback control %llx: the section wrote nothing", static_cast<unsigned long long>(p.control));   // zero bytes: zero vertices
        counters().draws_skipped.fetch_add(1);
        return;
    }
    if (prim_conversion(static_cast<int>(p.mode)) != PrimConversion::None) { note_unsupported("DrawTransformFeedback of a quad/polygon/line-loop primitive"); return; }
    const uint32_t slot = it->second;
    end_pass();
    memory_barrier(F->cb);   // the section's writes (feedback buffer, counter) before the vertex fetch and the indirect read
    g_draw_base_instance = 0;   // vkCmdDrawIndirectByteCountEXT draws from instance 0
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    uint32_t stride = g_key_attr_count ? S.stream[g_key_binding[0]].stride : 0;
    for (uint32_t b = 0; !stride && b < 16; ++b) stride = S.stream[b].stride;
    if (!stride) { note_unsupported("DrawTransformFeedback with no vertex stream stride"); counters().draws_skipped.fetch_add(1); return; }
    checkpoint_draw(static_cast<uint32_t>(p.control), static_cast<uint32_t>(p.control >> 32), 1, true);   // the count is the section's byte counter
    vkCmdDrawIndirectByteCountEXT(F->cb, 1, 0, g_xfb_counter.buffer, static_cast<VkDeviceSize>(slot) * kXfbSlotBytes, 0, stride);
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    g_pass_draws++;
    trace_snap_after_draw();
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
