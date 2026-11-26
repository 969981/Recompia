// SPDX-License-Identifier: GPL-3.0-or-later
// 2D the game sizes in 16:9 fractions of its canvas (game::Render::square_quad_fs): on a canvas of any other shape it comes out stretched
// along the canvas's longer side by distortion = (canvas h / w) x (16 / 9) (taller than wide below 16:9, wider above). Two kinds:
//  * a square image on its own: its draw gets a viewport that scales its shorter side about the quad's centre to
//    its longer one: square on screen, never smaller (what was covered stays covered).
//  * a stencil clip group: a stencil mask quad (colour writes off) followed by the draws it clips, all
//    sized and placed the same way about the mask's centre. The mask and every draw with the stencil test on after it get a viewport that
//    scales the stretched axis back by the distortion about the mask's centre: the group as at 16:9, at the layout's own scale.
//  * a multi-quad picture: several such squares in one draw (one quad per square texture part), sized and placed in 16:9 fractions about the canvas's centre: drawn in the 16:9 part of the canvas centred on it, as the
//    game's layouts are.
// At 16:9 nothing is stretched and nothing changes. Render thread.
#include "nvn_replay_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nxrt/game.h"
#include "nxrt/game_render.h"

namespace nxrt::nvn::be::gpu {
namespace replay {

namespace {
constexpr int kFormatRgb32F = 34;     // the 2D vertex: x, y, z in canvas units
constexpr uint32_t kQuadIndices = 6;  // a quad: two triangles (0 1 2, 0 2 3)
constexpr uint32_t kMaxQuads = 16;    // a picture of more quads is text

bool is_square_fs(const ShaderInfo* fs) {
    if (g_cmd.fs_square < 0) {
        const game::Render* gr = game::info().render;
        g_cmd.fs_square = fs && gr && gr->square_quad_fs && fs->hash == gr->square_quad_fs ? 1 : 0;
    }
    return g_cmd.fs_square > 0;
}

// The draw's viewport with one axis (0 x, 1 y) scaled by k about the canvas point c on it; false when the device cannot take it. The canvas
// is drawn at one scale on both axes (P.scale), so canvas units are square on screen and the game's viewport spans the canvas.
bool scaled_viewport(int axis, float k, float c) {
    float x = 0.f, y = 0.f, vw = S.vp[0][2], vh = S.vp[0][3];
    if (axis == 0) { x = c - k * c; vw *= k; }
    else { y = c - k * c; vh *= k; }
    const VkViewport vp = vk_viewport_of(x, y, vw, vh, 0);
    const auto& lim = device().properties().limits;
    if (vp.width > static_cast<float>(lim.maxViewportDimensions[0]) || std::fabs(vp.height) > static_cast<float>(lim.maxViewportDimensions[1]) ||
        vp.x < lim.viewportBoundsRange[0] || vp.x + vp.width > lim.viewportBoundsRange[1] ||
        std::min(vp.y, vp.y + vp.height) < lim.viewportBoundsRange[0] || std::max(vp.y, vp.y + vp.height) > lim.viewportBoundsRange[1])
        return false;
    vkCmdSetViewportWithCount(F->cb, 1, &vp);
    return true;
}

// The stretched axis scaled back by the distortion about c (x, y): what 16:9 sizing would have drawn at the layout's scale.
bool unstretched_viewport(float distortion, const float* c) {
    return distortion > 1.f ? scaled_viewport(1, 1.f / distortion, c[1]) : scaled_viewport(0, distortion, c[0]);
}
}  // namespace

bool square_quad_viewport(const P_DrawElements& p) {
    if (!is_square_fs(S.stage[kFragment])) {
        g_cmd.square_clip = false;   // the clip group's content is drawn with the same shader, one draw after another
        return false;
    }
    // one viewport whose size is the canvas the quad's units are in (the game's 2D projection spans its viewport)
    if (S.nvp != 1 || S.vp[0][0] != 0.f || S.vp[0][1] != 0.f || !(S.vp[0][2] > 0.f) || !(S.vp[0][3] > 0.f)) return false;
    const float distortion = (S.vp[0][3] / S.vp[0][2]) * (16.0f / 9.0f);
    if (std::fabs(distortion - 1.f) <= 0.01f) return false;   // 16:9: drawn as the game sized it
    if (g_cmd.square_clip) {
        // content of the open clip group: whatever its quads' shapes (each is rounded to whole pixels, so not an exact distorted square),
        // placed about the mask's centre
        if (S.ds.stencil_test) return unstretched_viewport(distortion, g_cmd.square_clip_c);
        g_cmd.square_clip = false;
    }
    if (S.attr_count == 0 || !p.count || p.count % kQuadIndices || p.count > kMaxQuads * kQuadIndices) return false;
    const VertexAttribInfo& at = S.attr[0];
    const GState::Bind& vb = S.vtx[at.stream];
    const uint8_t* ix = host_ptr(p.index_address);
    if (at.format != kFormatRgb32F || !vb.addr || !ix) return false;
    const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
    const auto stride = S.stream[at.stream].stride;
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (uint32_t q = 0; q < p.count; q += kQuadIndices) {
        float qlo[2] = {1e30f, 1e30f}, qhi[2] = {-1e30f, -1e30f};
        for (uint32_t i = q; i < q + kQuadIndices; ++i) {
            uint32_t v = 0;
            if (isz == 1) v = ix[i];
            else if (isz == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; }
            else std::memcpy(&v, ix + i * 4, 4);
            float xy[2];
            const int64_t vi = static_cast<int64_t>(v) + p.base_vertex;
            const uint64_t off = static_cast<uint64_t>(vi) * stride + at.relative_offset;
            if (vi < 0 || (vb.size && off + sizeof xy > vb.size)) return false;   // an index past the bound vertex buffer: leave the draw as it is
            std::memcpy(xy, reinterpret_cast<const void*>(vb.addr + off), sizeof xy);
            for (int c = 0; c < 2; ++c) { qlo[c] = std::min(qlo[c], xy[c]); qhi[c] = std::max(qhi[c], xy[c]); }
        }
        // Only squares the 16:9 sizing distorted: each quad's height over width is the distortion. Other quads drawn with the same shader
        // (button labels, text: glyphs of shapes of their own) are left alone; squaring them stretched the text.
        const float qw = qhi[0] - qlo[0], qh = qhi[1] - qlo[1];
        if (!(qw > 0.f) || !(qh > 0.f) || std::fabs(qh / qw - distortion) > 0.01f * distortion) return false;
        for (int c = 0; c < 2; ++c) { lo[c] = std::min(lo[c], qlo[c]); hi[c] = std::max(hi[c], qhi[c]); }
    }
    if (p.count > kQuadIndices) {   // a picture of several squares, placed about the canvas's centre
        const float centre[2] = {S.vp[0][2] * 0.5f, S.vp[0][3] * 0.5f};
        return unstretched_viewport(distortion, centre);
    }
    const float w = hi[0] - lo[0], h = hi[1] - lo[1];
    const float c[2] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f};
    if (S.chmask.mask[0] == 0 && S.ds.stencil_test) {   // a clip mask: writes the stencil only
        if (!unstretched_viewport(distortion, c)) return false;
        g_cmd.square_clip = true;
        g_cmd.square_clip_c[0] = c[0];
        g_cmd.square_clip_c[1] = c[1];
        return true;
    }
    return h > w ? scaled_viewport(0, h / w, c[0]) : scaled_viewport(1, w / h, c[1]);
}

}  // namespace replay
}  // namespace nxrt::nvn::be::gpu
