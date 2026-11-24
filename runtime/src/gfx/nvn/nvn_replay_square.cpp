// SPDX-License-Identifier: GPL-3.0-or-later
// A square image the game stretched with the screen (game::Render::square_quad_fs): the game sizes the quad in fractions of its canvas with a
// 16:9 ratio, so on a canvas of any other shape it comes out wider or taller than it is. The quad's draw gets a viewport that scales its
// shorter side about the quad's centre to its longer one: square on screen, never smaller (what was covered stays covered). At 16:9 the quad
// is square already and nothing changes. Render thread.
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
constexpr uint32_t kMaxIndices = 6;   // one quad

bool is_square_fs(const ShaderInfo* fs) {
    if (g_cmd.fs_square < 0) {
        const game::Render* gr = game::info().render;
        g_cmd.fs_square = fs && gr && gr->square_quad_fs && fs->hash == gr->square_quad_fs ? 1 : 0;
    }
    return g_cmd.fs_square > 0;
}
}  // namespace

bool square_quad_viewport(const P_DrawElements& p) {
    if (!is_square_fs(S.stage[kFragment])) return false;
    // one viewport whose size is the canvas the quad's units are in (the game's 2D projection spans its viewport)
    if (S.nvp != 1 || S.vp[0][0] != 0.f || S.vp[0][1] != 0.f || S.attr_count == 0 || !p.count || p.count > kMaxIndices) return false;
    const VertexAttribInfo& at = S.attr[0];
    const GState::Bind& vb = S.vtx[at.stream];
    const uint8_t* ix = host_ptr(p.index_address);
    if (at.format != kFormatRgb32F || !vb.addr || !ix) return false;
    const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
    const auto stride = S.stream[at.stream].stride;
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (uint32_t i = 0; i < p.count; ++i) {
        uint32_t v = 0;
        if (isz == 1) v = ix[i];
        else if (isz == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; }
        else std::memcpy(&v, ix + i * 4, 4);
        float xy[2];
        const int64_t vi = static_cast<int64_t>(v) + p.base_vertex;
        const uint64_t off = static_cast<uint64_t>(vi) * stride + at.relative_offset;
        if (vi < 0 || (vb.size && off + sizeof xy > vb.size)) return false;   // an index past the bound vertex buffer: leave the draw as it is
        std::memcpy(xy, reinterpret_cast<const void*>(vb.addr + off), sizeof xy);
        for (int c = 0; c < 2; ++c) { lo[c] = std::min(lo[c], xy[c]); hi[c] = std::max(hi[c], xy[c]); }
    }
    const float w = hi[0] - lo[0], h = hi[1] - lo[1];
    if (!(w > 0.f) || !(h > 0.f) || std::fabs(w - h) <= 0.01f * std::max(w, h)) return false;   // square already (16:9)
    // Only a square the 16:9 sizing distorted: its height over width is the canvas's shape over 16:9. Other quads drawn with the same
    // shader (button labels, text) have shapes of their own and are left alone; squaring them stretched the text.
    const float distortion = (S.vp[0][3] / S.vp[0][2]) * (16.0f / 9.0f);
    if (std::fabs(h / w - distortion) > 0.01f * distortion) return false;
    // the canvas is drawn at one scale on both axes (P.scale), so canvas units are square on screen
    float x = 0.f, y = 0.f, vw = S.vp[0][2], vh = S.vp[0][3];
    const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;
    if (h > w) { const float k = h / w; x = cx - k * cx; vw *= k; }
    else { const float k = w / h; y = cy - k * cy; vh *= k; }
    const VkViewport vp = vk_viewport_of(x, y, vw, vh, 0);
    const auto& lim = device().properties().limits;
    if (vp.width > static_cast<float>(lim.maxViewportDimensions[0]) || std::fabs(vp.height) > static_cast<float>(lim.maxViewportDimensions[1]) ||
        vp.x < lim.viewportBoundsRange[0] || vp.x + vp.width > lim.viewportBoundsRange[1] ||
        std::min(vp.y, vp.y + vp.height) < lim.viewportBoundsRange[0] || std::max(vp.y, vp.y + vp.height) > lim.viewportBoundsRange[1])
        return false;
    vkCmdSetViewportWithCount(F->cb, 1, &vp);
    return true;
}

}  // namespace replay
}  // namespace nxrt::nvn::be::gpu
