// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play mode: native render resolution (pkp_play.h; split out of pkp_play.cpp). Offsets in `main` of the 2.0.1 update.
//
// Render resolution. KTGL's dynamic-resolution manager (the object at [0x35618d0]) sizes everything from a level table of 34 sizes
// (0x2849658 widths, 0x28496e0 heights: 960x540 + 32x18 per level, then 2560x1440, 3840x2160, 7680x4320), read only through two getters
// (0x1129990 width, 0x11299d0 height). At start it is set up with level 30 (1920x1080) as the target size (the swap chain, the scene and UI
// targets: manager +16/+20, copied to +72/+76 which the window set-up and every pass read) and as the largest scene size (+24/+28). Each frame
// it picks a lower level when the GPU is late (docked tables top out at level 30) and scales by a quality factor, giving the scene's viewport
// (+32/+36). The hooks: the two getters answer the configured size for level 30 and scale the levels below it in proportion (so the
// game's own dynamic resolution still steps down smoothly from the new top), the fallbacks 0x1129980 / 0x11299c0 (1920 / 1080 while the
// manager does not exist yet) answer the same size, and the per-frame copy of the parameters (0x22d1e0) gets the full size as the viewport
// while [display] dynamic_resolution is off. The scene passes keep viewports authored for 1920x1080: the command context's SetViewport(s)
// (0x17b509c / 0x17b5240, which also derive the 2/w, -2/h screen constants from the rect) get the target's size, and the render thread's
// replay (0x17b6c98) stretches what still arrives sized for 1920x1080. The internal size is fixed at start (the targets are created once).
// The terrain occlusion pass's copy into its fixed 80x45 targets takes its source rect from a viewport recorded as 1920x1080;
// hook_record_viewports below gives that record the target's size (without the right source rect, a hard-edged dark patch showed on the
// ground). [pkp] render_override (default 1 since the
// 2026-11-02 run confirmed that fix; 0 = the game's 1080p layout) switches all of it on.
#include "input/pkp_play_internal.h"
#include "render/pkp_gfx_heap.h"
#include "core/pkp_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "nxrt/extras.h"
#include "nxrt/hooks.h"
#include "nxrt/host_device.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "play_display.h"

namespace nxrt::games::pkp::play_detail {

// ---- render resolution ----
uint32_t level_size(int level, bool width) {
    const int full = width ? st().w.load(std::memory_order_relaxed) : st().h.load(std::memory_order_relaxed);
    if (level == kFullLevel) return static_cast<uint32_t>(full);
    // the game's own table: levels 0..29 are 960x540 + 32x18 per level (scaled with the new top), 31..33 are 1440p, 4K, 8K (kept)
    static constexpr int kHighW[3] = {2560, 3840, 7680}, kHighH[3] = {1440, 2160, 4320};
    if (level > 33) return static_cast<uint32_t>(full);              // the game's own out-of-range answer is level 30's
    if (level > kFullLevel) return static_cast<uint32_t>(width ? kHighW[level - 31] : kHighH[level - 31]);
    const int table = width ? 960 + 32 * level : 540 + 18 * level;
    const double ref = width ? kTableW : kTableH;
    return static_cast<uint32_t>(std::lround(table * full / ref)) & ~1u;
}

int hook_level_w(::Ctx* c, uint64_t, void*) {
    c->x[0] = level_size(static_cast<int>(c->x[0] & 0xff), true);
    st().res_calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}
int hook_level_h(::Ctx* c, uint64_t, void*) {
    c->x[0] = level_size(static_cast<int>(c->x[0] & 0xff), false);
    st().res_calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}
int hook_full_w(::Ctx* c, uint64_t, void*) { c->x[0] = static_cast<uint64_t>(st().w.load()); return 1; }
int hook_full_h(::Ctx* c, uint64_t, void*) { c->x[0] = static_cast<uint64_t>(st().h.load()); return 1; }

// [upscale] scene_scale (#116; extras::upscale_scene_scale: percent per side, or the temporal quality preset's) shrinks the scene viewport
// (+32 / +36) the way the game's own dynamic resolution does: the scene passes render into the top-left of the full-size targets and the
// composite pass (the tone-map draw, KTGL cbComposite: scene UV = uv * c5[11].zw + c5[11].xy) stretches that part over the frame with one
// bilinear tap; the HUD and the post chain stay at full size. With dynamic resolution on, the game's own pick is capped at that size.
// extras::scene_fraction tells the back end's scene upscalers (FSR 1 / NIS / FSR 3 / DLSS at the tone-map draw) which part to upscale. It is
// published only with dynamic resolution off and one view: a size that changes every frame or a split screen keeps the game's own stretch.
std::atomic<int> g_scene_pct{-1};   // the percent the last copy applied (logged when it changes)
uint32_t scene_side(uint32_t full, int pct) { return std::max<uint32_t>(2, static_cast<uint32_t>((static_cast<uint64_t>(full) * pct + 50) / 100) & ~1u); }
// The scene's part of its targets as the game will use it this frame (view 0's scene size / target size, per side; 1 = all of it), for the
// viewport hooks below (#163). Unlike extras::scene_fraction it is also kept with dynamic resolution on; a split screen keeps 1.
std::atomic<float> g_vp_kx{1.0f}, g_vp_ky{1.0f};

int hook_copy_params(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const bool dyn = s.dynres.load(std::memory_order_relaxed);
    const int pct = std::clamp(extras::upscale_scene_scale(), 25, 100);
    if (g_scene_pct.exchange(pct, std::memory_order_relaxed) != pct)
        NXLOG_I(kTag, "scene resolution %d%%: %ux%u of the %dx%d targets ([upscale] scene_scale%s)", pct, scene_side(s.w.load(), pct),
                scene_side(s.h.load(), pct), s.w.load(), s.h.load(), dyn ? "; the cap of the game's dynamic resolution, upscaled by the game" : "");
    const uint64_t mgr = c->x[0];
    if (!mgr || !mem::is_mapped(mgr, 16 + 2 * kViewStride)) {
        extras::scene_fraction_set(1.0f, 1.0f);
        g_vp_kx.store(1.0f, std::memory_order_relaxed);
        g_vp_ky.store(1.0f, std::memory_order_relaxed);
        return 0;
    }
    int views = 1;
    const uint64_t vo = rd<uint64_t>(s.base + kViewsGlobal);
    if (vo && mem::is_mapped(vo + 562, 1) && rd<uint8_t>(vo + 562)) views = 2;
    // the viewport hooks' fraction: what view 0 renders after this copy (the game's own pick with dynamic resolution, capped below)
    const auto publish_vp_fraction = [&] {
        float kx = 1.0f, ky = 1.0f;
        const uint32_t tw = rd<uint32_t>(mgr + kTgtW), th = rd<uint32_t>(mgr + kTgtH), cw = rd<uint32_t>(mgr + kCurW), ch = rd<uint32_t>(mgr + kCurH);
        if (views == 1 && tw > 0 && th > 0 && cw > 0 && ch > 0 && cw <= tw && ch <= th) {
            kx = static_cast<float>(cw) / static_cast<float>(tw);
            ky = static_cast<float>(ch) / static_cast<float>(th);
        }
        g_vp_kx.store(kx, std::memory_order_relaxed);
        g_vp_ky.store(ky, std::memory_order_relaxed);
    };
    if (dyn && pct >= 100) {
        extras::scene_fraction_set(1.0f, 1.0f);
        publish_vp_fraction();
        return 0;
    }
    float fx = 1.0f, fy = 1.0f;
    for (int i = 0; i < views; ++i) {
        const uint64_t v = mgr + i * kViewStride;
        const uint32_t mw = rd<uint32_t>(v + kMaxW), mh = rd<uint32_t>(v + kMaxH);
        const uint32_t sw = pct >= 100 ? mw : scene_side(mw, pct), sh = pct >= 100 ? mh : scene_side(mh, pct);
        if (dyn) {
            wr<uint32_t>(v + kCurW, std::min(rd<uint32_t>(v + kCurW), sw));
            wr<uint32_t>(v + kCurH, std::min(rd<uint32_t>(v + kCurH), sh));
            continue;
        }
        wr<uint32_t>(v + kCurW, sw);
        wr<uint32_t>(v + kCurH, sh);
        wr<uint8_t>(v + kLvlCur, rd<uint8_t>(v + kLvlMax));
        const uint32_t tw = rd<uint32_t>(v + kTgtW), th = rd<uint32_t>(v + kTgtH);   // the targets the scene is drawn into
        if (i == 0 && tw > 0 && th > 0) {
            fx = static_cast<float>(sw) / static_cast<float>(tw);
            fy = static_cast<float>(sh) / static_cast<float>(th);
        }
    }
    if (dyn || views > 1) fx = fy = 1.0f;
    extras::scene_fraction_set(fx, fy);
    publish_vp_fraction();
    s.copy_calls.fetch_add(1, std::memory_order_relaxed);
    return 0;                                       // the game's copy runs with the chosen size in place
}

// The scene passes store their viewports as authored for 1920x1080 (the render thread replays them; the targets themselves follow the new
// size). Right before the replay hands them to nvnCommandBufferSetViewports (sp+216: count x {x, y, w, h} floats; sp+24: the scissors as ints,
// which default to the target's size): a viewport at the origin whose size is the target's size scaled by 1920x1080 / the internal size is
// stretched to the whole target. Smaller fractional targets (half size and so on) get the same treatment; anything else is left alone.
// The command context's SetViewport(s) (0x17b509c / 0x17b5240) record the pass's viewport and, from its size, the screen constants the
// shaders read (2/w, -2/h in vs ubo0). Rects are {w, h, x, y, min, max} (ints, then floats; stride 24). The pass state in use
// (ctx [+0x18] + 616 * ctx [+0x24]) holds the bound target at +0x50, its size at +0x58 / +0x5c. A rect at the origin sized for a
// 1920x1080 frame on a target of the render size is made the target's size here, so the constants follow it too.
// The other viewport setter, 0x17c0420 (64 callers: the G-buffer, the deferred passes and most scene passes; x0 = ctx + 0x10, w1 count,
// x2 rects), only records the rects into the pass state (+0x60 viewports, +0x58 scissors). The screen constants are derived later from
// that record (0x17b3000, 0x17b45c0, 0x17b9700, 0x17bac00: 2 / pass[+0x60].w, -2 / h), so a rect kept at 1920x1080 there gave the
// render-size passes 2/1920, -2/1080 while the replay stretched the viewport itself: a quad in pixel units (vs 821538a4) then covered
// 1.333x the target from the top-left, and its UVs with it. At 2560x1440 the G-buffer AO composite (fs 90107dd3, t428 into the
// albedo's alpha) showed the AO of the characters as dark blue silhouettes at 1.333x their screen position and moved the ground's
// AO the same way (the mottled, world-fixed haze of upstream #17). The entry hook there fixes the rects the same way, before they are
// recorded, so the constants, the replayed viewport and the terrain occlusion copy's source rect all follow.
// A scene rendered below its targets' size ([upscale] scene_scale, a temporal upscaler's preset, dynamic resolution: g_vp_kx / ky) has
// its viewports authored as that fraction of the 1920x1080 frame: 1440x810 at 75% (#163). On a 16:9 render size of 1920x1080 that is
// the right rect; on any other render size it must become the same fraction of the target, 1440x900 on 1920x1200. Kept at 1440x810 on a
// 16:10 target, the scene passes drew into a 16:9 part (an empty band below it in the composite) and the screen constants the
// characters and objects are placed with disagreed with the terrain's: players sank into the ground, the #73 symptoms. So a rect at the
// origin of the full authored size or of the scene's fraction of it is matched, nothing in between.
// [pkp] scene_viewport_fix (default true) turns this on; false keeps the full-frame match only (the old behaviour).
std::atomic<uint64_t> g_set_vp_fixes{0}, g_rec_vp_fixes{0}, g_frac_vp_fixes{0};
std::atomic<bool> g_frac_vp_on{true};
// [pkp] target_fix = all rounds a fractional target to whole pixels. Matching its authored viewport through the inverse ratio needs the
// same rounding allowance (for example 480x270 -> 168x95 at 672x378 maps back to 480x271.43), in addition to the original 1 px tolerance.
double viewport_tolerance(int table, int render) { return 1.0 + 0.5 * table / std::max(1, render); }
void fix_rect(::Ctx* c, uint64_t rect, uint64_t ctx = 0) {
    State& s = st();
    const int W = s.w.load(std::memory_order_relaxed), H = s.h.load(std::memory_order_relaxed);
    if (!ctx) ctx = c->x[0];
    const int32_t idx = rd<int32_t>(ctx + 0x24);
    const uint64_t pass = idx < 0 ? 0 : rd<uint64_t>(ctx + 0x18) + 616ull * static_cast<uint32_t>(idx);
    if (!pass) return;
    const uint64_t tgt = rd<uint64_t>(pass + 0x50);
    if (!tgt || !mem::is_mapped(tgt + 0x58, 8)) return;
    const int32_t tw = rd<int32_t>(tgt + 0x58), th = rd<int32_t>(tgt + 0x5c);
    int32_t* r = reinterpret_cast<int32_t*>(rect);
    if (r[2] != 0 || r[3] != 0 || tw <= 0 || th <= 0 || (r[0] == tw && r[1] == th)) return;
    const double tolw = viewport_tolerance(kTableW, W), tolh = viewport_tolerance(kTableH, H);
    const auto matches = [&](double kx, double ky) {
        const double ew = static_cast<double>(tw) * kx * kTableW / W, eh = static_cast<double>(th) * ky * kTableH / H;
        return std::fabs(r[0] - ew) <= tolw && std::fabs(r[1] - eh) <= tolh;
    };
    if (matches(1.0, 1.0)) {
        r[0] = tw;
        r[1] = th;
        g_set_vp_fixes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const double kx = g_vp_kx.load(std::memory_order_relaxed), ky = g_vp_ky.load(std::memory_order_relaxed);
    if ((kx >= 1.0 && ky >= 1.0) || !g_frac_vp_on.load(std::memory_order_relaxed)) return;
    const int32_t nw = static_cast<int32_t>(std::lround(tw * kx)), nh = static_cast<int32_t>(std::lround(th * ky));
    if ((r[0] == nw && r[1] == nh) || !matches(kx, ky)) return;
    if (g_frac_vp_fixes.fetch_add(1, std::memory_order_relaxed) == 0)
        NXLOG_I(kTag, "scene viewport: %dx%d (%.3f x %.3f of the 1920x1080 frame) made %dx%d of the %dx%d target", r[0], r[1], kx, ky, nw, nh, tw, th);
    r[0] = nw;
    r[1] = nh;
    g_set_vp_fixes.fetch_add(1, std::memory_order_relaxed);
}
int hook_set_viewport(::Ctx* c, uint64_t, void*) {
    const State& s = st();
    if (s.w.load(std::memory_order_relaxed) == kTableW && s.h.load(std::memory_order_relaxed) == kTableH) return 0;
    fix_rect(c, c->x[1]);
    return 0;
}
int hook_set_viewports(::Ctx* c, uint64_t, void*) {
    const State& s = st();
    if (s.w.load(std::memory_order_relaxed) == kTableW && s.h.load(std::memory_order_relaxed) == kTableH) return 0;
    const uint32_t n = static_cast<uint32_t>(c->x[1]);
    for (uint32_t i = 0; i < n && i < 16; ++i) fix_rect(c, c->x[2] + 24ull * i);
    return 0;
}
int hook_record_viewports(::Ctx* c, uint64_t, void*) {
    const State& s = st();
    if (s.w.load(std::memory_order_relaxed) == kTableW && s.h.load(std::memory_order_relaxed) == kTableH) return 0;
    const uint64_t ctx = c->x[0] - 0x10, rects = c->x[2];
    const uint32_t n = static_cast<uint32_t>(c->x[1]);
    if (!rects || n == 0 || !mem::is_mapped(ctx + 0x18, 16) || !mem::is_mapped(rects, 24ull * std::min<uint32_t>(n, 16))) return 0;
    const uint64_t before = g_set_vp_fixes.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < 16; ++i) fix_rect(c, rects + 24ull * i, ctx);
    if (g_set_vp_fixes.load(std::memory_order_relaxed) != before && g_rec_vp_fixes.fetch_add(1, std::memory_order_relaxed) == 0)
        NXLOG_I(kTag, "recorded viewport: 1920x1080-sized rect made the target's size (0x17c0420, screen constants follow)");
    return 0;
}

int hook_replay_viewports(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const int W = s.w.load(std::memory_order_relaxed), H = s.h.load(std::memory_order_relaxed);
    if (W == kTableW && H == kTableH) return 0;
    const uint32_t n = static_cast<uint32_t>(c->x[23]);
    if (n == 0 || n > 16) return 0;
    const uint64_t sp = c->x[31];
    for (uint32_t i = 0; i < n; ++i) {
        float* vp = reinterpret_cast<float*>(sp + 216 + 16 * i);
        const int32_t* sc = reinterpret_cast<const int32_t*>(sp + 24 + 16 * i);
        if (vp[0] != 0.f || vp[1] != 0.f || sc[0] != 0 || sc[1] != 0 || sc[2] <= 0 || sc[3] <= 0) continue;
        const double ew = static_cast<double>(sc[2]) * kTableW / W, eh = static_cast<double>(sc[3]) * kTableH / H;
        if (std::fabs(vp[2] - ew) <= viewport_tolerance(kTableW, W) && std::fabs(vp[3] - eh) <= viewport_tolerance(kTableH, H)) {
            const uint64_t key = (static_cast<uint64_t>(sc[2]) << 32) | static_cast<uint32_t>(sc[3]);
            bool seen = false;
            const uint32_t nk = std::min<uint32_t>(s.vp_shape_n.load(std::memory_order_relaxed), 16);
            for (uint32_t k = 0; k < nk && !seen; ++k) seen = s.vp_shapes[k].load(std::memory_order_relaxed) == key;
            if (!seen) {
                const uint32_t slot = s.vp_shape_n.fetch_add(1, std::memory_order_relaxed);
                if (slot < 16) {
                    s.vp_shapes[slot].store(key, std::memory_order_relaxed);
                    NXLOG_I(kTag, "viewport stretch: %.0fx%.0f -> %dx%d (viewport %u of %u)", vp[2], vp[3], sc[2], sc[3], i, n);
                }
            }
            vp[2] = static_cast<float>(sc[2]);
            vp[3] = static_cast<float>(sc[3]);
            s.vp_fixes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return 0;
}

// The 16:9 internal size: the base rule (play_display.h internal_size), kept at 16:9 (the game's cameras and UI are authored for it) and,
// unless a smaller size was asked for explicitly, at least the console's 1920x1080. On a handheld (nxrt/host_device.h) auto is the
// window's size: a Steam Deck held ~40-50 fps in the world at 1920x1080 and shows 1280x800 anyway (Game Mode: a 1280x720 window).
void pick_size(const play::DisplaySettings& d, int window_w, int window_h, int* out_w, int* out_h) {
    int w = kTableW, h = kTableH;
    play::internal_size(d, window_w, window_h, &w, &h);
    const bool fixed = d.render_width > 0 && d.render_height > 0;
    h = std::min(h, static_cast<int>(static_cast<int64_t>(w) * 9 / 16));
    if (!fixed && ((d.render_scale <= 0 && !host_is_handheld()) || d.render_scale >= 100)) h = std::max(h, kTableH);
    h = std::clamp(h / 18 * 18, 360, 4320);                          // 16:9 in whole pixels
    *out_w = h * 16 / 9;
    *out_h = h;
}

// Game targets above 1920x1080 need larger graphics pools than the game's fixed ones (pkp_gfx_heap.h): grows them (nothing is hooked at
// 1920x1080 and below) and clamps a size whose pools would not fit, before the targets' size is installed. Only the paths where the game
// itself builds its targets at the new size come here; with render_override off the game keeps 1920x1080 targets (the back end scales them).
void game_target_memory(uint64_t base, int* w, int* h) {
    const GfxHeapPlan plan = gfx_heap_plan(*w, *h);
    if (plan.clamped)
        NXLOG_W(kTag, "render resolution %dx%d needs more graphics memory than the game can load the world with (pools under 4 GiB, at most 3840x2160 pixels): using %dx%d", *w, *h,
                plan.w, plan.h);
    *w = plan.w;
    *h = plan.h;
    gfx_heap_install(base, plan, *st().hooks);
}

// The hooks that make the game's screen-relative targets and their viewports follow the render size (both size paths below).
// The terrain occlusion pass's 80x45 copy (0x73d73c calling 0x10f48d8) needs no hook of its own: it takes its source rect from the
// G-buffer pass's recorded viewport (pass +96) times the view's scale, and hook_record_viewports makes that record the target's size.
// Its former mid hook (0x73d824) never changed a rect in eight 2560x1440 runs with the recorder hook in place, though the copy ran
// every frame (the 80x45 targets were drawn 140 times per traced frame).
void install_render_size_hooks(uint64_t base) {
    State& s = st();
    const struct { uint64_t off; SiteHookFn fn; } table[] = {
        {kLevelWidth, hook_level_w}, {kLevelHeight, hook_level_h}, {kFullWidth, hook_full_w}, {kFullHeight, hook_full_h},
        {kCopyParams, hook_copy_params},
    };
    for (const auto& t : table) s.hooks->add_entry(base + t.off, t.fn);
    s.hooks->add_mid(base + kSetViewport, &hook_set_viewport);
    s.hooks->add_mid(base + kSetViewports, &hook_set_viewports);
    s.hooks->add_entry(base + kRecordViewports, &hook_record_viewports);
    s.hooks->add_mid(base + kReplayViewports, &hook_replay_viewports);
    g_frac_vp_on.store(settings::get_bool(settings::Key::SceneViewportFix), std::memory_order_relaxed);
    const TargetFix mode = target_fix_from_config();
    g_target_fix.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
    s.hooks->add_entry(base + kCreateTarget, &hook_create_target);
    if (s.w.load() != kTableW || s.h.load() != kTableH)
        NXLOG_I(kTag, "render targets with a literal size: %s ([pkp] target_fix)",
                mode == TargetFix::Off   ? "all kept as authored"
                : mode == TargetFix::All ? "screen-size and fractional ones follow the render size"
                                         : "screen-size ones follow the render size");
}

}  // namespace nxrt::games::pkp::play_detail
