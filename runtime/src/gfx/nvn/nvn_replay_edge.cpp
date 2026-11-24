// SPDX-License-Identifier: GPL-3.0-or-later
// [gfx] edge_guard (phones, default on): the edge of a scene part rendered below its targets' size is repeated into a band past it.
//
// With the scene drawn into the top-left part of larger targets (extras::scene_fraction: a phone's Resolution or its dynamic
// resolution), some of the game's passes read past the part: the deferred lighting, for one, samples the quarter-size lighting input
// (480x270, filtered) by screen UV and draws with a viewport one column short of the part (479 of 480 at 25 % of 1920), and its reads
// at the part's edge reach texels that hold whatever that part of the target held before. Enlarged to the screen that is a soft band
// along the right edge of the picture: blue on a phone where the area was never drawn, a stale picture after the camera moved (traced on the desktop at 25 %: the lit scene t446 had its last
// column unlit and a gradient about 8 columns wide; 15 px on a near-square phone screen at Resolution 25 right after a walk). After a pass whose viewport is
// exactly that part of its targets (its top-left fraction, 2.5 px for rounding), the last column and row of every colour target are
// copied into the next kBand columns and rows, so a read past the part finds the part's own edge, as a clamp to the part would give.
// Costs two small copies and two barriers per such pass. Measured on the desktop at 25 % (a near-square window, start below 100 %): the right 8 px
// stood 66 to 75 above the columns 64 to 128 px in (blue minus the mean of red and green), 4 to 6 with the guard. Phones only: elsewhere no pass reaches here (facts::kPhone at the callers).
#include "nvn_replay_internal.h"

#include "nxrt/config.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
namespace {

constexpr uint32_t kBand = 8;   // image pixels past the part: a filtered read of the quarter-size buffer at 4x reaches 1 texel = 4 px
struct Edge { float w = 0.0f, h = 0.0f; bool other = false; };
Edge g_edge;                     // the current pass's viewports (render thread)

bool edge_guard_on() {
    static const bool on = config_get_bool("gfx", "edge_guard", true);
    return on;
}

}  // namespace

void edge_guard_note(float x, float y, float w, float h, const VkRect2D& scissor) {
    if (x != 0.0f || y != 0.0f || scissor.offset.x != 0 || scissor.offset.y != 0) { g_edge.other = true; return; }
    g_edge.w = std::max(g_edge.w, std::min(w, static_cast<float>(scissor.extent.width)));
    g_edge.h = std::max(g_edge.h, std::min(std::fabs(h), static_cast<float>(scissor.extent.height)));
}

// The scene part as this frame drew it, per colour target: the area the last pass into that target covered (its top-left viewport,
// within its scissor). Under dynamic resolution the scene changes size between frames, and the fraction the game layer publishes belongs
// to the frame it is building, which can be a frame ahead of the one replayed: one frame per step enlarged the wrong part.
namespace {
struct FramePart { Ref<Texture> tex; uint32_t w = 0, h = 0, pw = 0, ph = 0; };   // the reference keeps the texture alive
FramePart g_parts[64];   // a frame draws into some 20 to 40 colour targets: fewer slots dropped the stretch input's entry before the stretch
uint32_t g_parts_next = 0;
}  // namespace

void frame_parts_reset() {   // before the textures and the device go (replay_shutdown)
    for (FramePart& f : g_parts) f = FramePart();
    g_parts_next = 0;
}

bool frame_scene_part(const Texture* t, uint32_t w, uint32_t h, uint32_t* pw, uint32_t* ph) {
    for (const FramePart& f : g_parts)
        if (f.tex && f.w == w && f.h == h && f.pw && shares_image(f.tex.get(), t)) { *pw = f.pw; *ph = f.ph; return true; }
    return false;
}

void edge_guard_after_pass() {
    const Edge e = g_edge;
    g_edge = Edge();
    if (P.draws > 0 && !e.other && e.w > 0.0f && e.h > 0.0f && P.ncolor > 0 && P.color[0].tex) {   // the frame's scene part per target
        const uint32_t W = P.extent.width, H = P.extent.height;
        const uint32_t pw = std::min(W, static_cast<uint32_t>(std::lround(e.w))), ph = std::min(H, static_cast<uint32_t>(std::lround(e.h)));
        FramePart* slot = nullptr;
        for (FramePart& f : g_parts) if (f.tex.get() == P.color[0].tex) slot = &f;
        if (!slot) slot = &g_parts[g_parts_next++ % 64];
        *slot = {Ref<Texture>::share(P.color[0].tex), W, H, pw, ph};
    }
    if (!edge_guard_on() || P.draws == 0 || e.other || P.ncolor == 0) return;
    // the published fraction, and the one before it: the frame being replayed can still be one drawn at the previous size
    static float cur_x = 1.0f, cur_y = 1.0f, prev_x = 1.0f, prev_y = 1.0f;
    float fx = 1.0f, fy = 1.0f;
    extras::scene_fraction(&fx, &fy);
    if (fx != cur_x || fy != cur_y) { prev_x = cur_x; prev_y = cur_y; cur_x = fx; cur_y = fy; }
    if (fx >= 1.0f && fy >= 1.0f && prev_x >= 1.0f && prev_y >= 1.0f) return;
    const uint32_t W = P.extent.width, H = P.extent.height;
    const uint32_t vw = static_cast<uint32_t>(std::ceil(e.w - 0.01f)), vh = static_cast<uint32_t>(std::ceil(e.h - 0.01f));
    if (vw < 2 || vh < 2 || vw >= W || vh >= H) return;
    const auto near = [&](float x, float y) { return std::fabs(e.w - x * static_cast<float>(W)) <= 2.5f && std::fabs(e.h - y * static_cast<float>(H)) <= 2.5f; };
    if (!near(fx, fy) && !near(prev_x, prev_y)) return;
    static bool told = false;
    if (!told) {
        told = true;
        NXLOG_I(kTag, "edge guard: the scene part's last column and row repeated %u px past it (first: %ux%u of a %ux%u target; [gfx] edge_guard)",
                kBand, vw, vh, W, H);
    }
    memory_barrier(F->cb);
    g_bar.pending.clear();
    for (uint32_t i = 0; i < P.ncolor && i < 8; ++i) {
        const Target& tg = P.color[i];
        Texture* t = tg.tex;
        if (!t || !tex_gpu(t) || tex_is_depth(t)) continue;
        tex_transition(t, *F, VK_IMAGE_LAYOUT_GENERAL);
        const VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, tg.level, tg.layer, tg.layers};
        VkImageCopy r[2 * kBand];
        uint32_t n = 0;
        for (uint32_t k = 0; k < kBand && vw + k < W; ++k)   // the last column into the columns right of the part
            r[n++] = {sub, {static_cast<int32_t>(vw - 1), 0, 0}, sub, {static_cast<int32_t>(vw + k), 0, 0}, {1, vh, 1}};
        for (uint32_t k = 0; k < kBand && vh + k < H; ++k)   // the last row into the rows below it
            r[n++] = {sub, {0, static_cast<int32_t>(vh - 1), 0}, sub, {0, static_cast<int32_t>(vh + k), 0}, {vw, 1, 1}};
        if (n) vkCmdCopyImage(F->cb, tex_image(t), VK_IMAGE_LAYOUT_GENERAL, tex_image(t), VK_IMAGE_LAYOUT_GENERAL, n, r);
    }
    memory_barrier(F->cb);
}

}  // namespace replay
}  // namespace nxrt::nvn::be::gpu
