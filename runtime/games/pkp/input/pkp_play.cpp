// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the PC features of play mode (pkp_play.h): native render resolution, frame rates above 60 at the right game speed and the
// raw mouse camera. All addresses are offsets in `main` of the 2.0.1 update (build 76a489f3...), found in IDA (work/re/pc_features.md);
// the sites are in recomp/games/pkp/main.sites.
//
// The subsystems live in their own files, on the state of pkp_play_internal.h: pkp_render_size.cpp (render resolution and viewports),
// pkp_render_targets.cpp (targets of a literal size), pkp_aspect.cpp (aspects other than 16:9), pkp_wide_text.cpp (wide text spacing),
// pkp_frame_rate.cpp (frame rate), pkp_mouse_camera.cpp (mouse camera), pkp_mouse_mode.cpp (Joy-Con 2 mouse mode). This file installs them
// and answers play mode's GamePlay table.
#include "input/pkp_play.h"
#include "input/pkp_play_internal.h"
#include "core/pkp_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

#include "nxrt/extras.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/hooks.h"
#include "nxrt/host_device.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp::play_detail {

State& st() {
    static State s;
    return s;
}

}  // namespace nxrt::games::pkp::play_detail

namespace nxrt::games::pkp {

using namespace play_detail;

namespace {

void install_render(uint64_t base, const play::DisplaySettings& d, int window_w, int window_h, HookSet& hooks) {
    State& s = st();
    if (s.render_installed) return;
    s.base = base;
    s.hooks = &hooks;
    int w = kTableW, h = kTableH;
    pick_size(d, window_w, window_h, &w, &h);
    s.dynres.store(d.dynamic_resolution);
    {
        // [hdr]: the host's HDR scene pipeline (src/gfx/extras): float post targets, and the composite pass's LUT tone map (cbComposite,
        // g_tHdrLut: a log shaper into a 32^3 grade that clips flat at 1 from scene 0.85-1.1) runs as an HDR variant whose highlights climb
        // above paper white to the display's peak (extras spirv_hdr_tonemap_lut; the shader is matched by structure from the render facts in game::Info::render, render/pkp_render_info.cpp; no game hook). The game's own "true HDR" constants
        // (fKIDSTrueHDR, fMaxLum, SIMHDR_PARAMS set at 0x2580198; HdrTvNit / BaseTvNit at 0x2581580) are looked up by name and no shader
        // in the game's data declares them, so driving them would change nothing. Pokopia never asks whether the
        // TV is HDR (no settings / vi / oe / NVN capability query in main) and never calls the NVN2 window HDR procs; what it does declare are the
        // nn::vi layer content luminance ranges (standard 0..300, extended 0..400 nits), which [hdr] mode = native uses (paper white, content max).
        const extras::HdrState hs = extras::hdr_state();
        NXLOG_I(kTag, "HDR: scene %s, output %s, paper white %.0f nits, peak %.0f nits (peak ratio %.2f: %s)", hs.scene_hdr ? "HDR" : "SDR",
                extras::output_kind_name(hs.output), static_cast<double>(hs.paper_white_nits), static_cast<double>(hs.peak_nits),
                static_cast<double>(hs.peak_ratio),
                hs.scene_hdr && hs.peak_ratio > 1.001f ? "the composite LUT tone map's highlights reach the peak" : "highlights stay at paper white");
    }
    // The display's aspect ([display] aspect: auto = the window's, or the fixed render size's when one is set). Anything but 16:9 gives the
    // game targets of that shape (1080 high when wider, 1920 wide when narrower; the back end's scale on top) and the camera / UI hooks above.
    const bool fixed_size = d.render_width > 0 && d.render_height > 0;
    double aspect = fixed_size ? play::display_aspect(d, d.render_width, d.render_height) : play::display_aspect(d, window_w, window_h);
    // Taller than 16:9 (16:10 handhelds): by default (taller_fov on) the taller view is made through the camera's FOV and aspect (taller_fields),
    // so every draw agrees on the projection (#73). With [pkp] taller_fov = false the view is matrix-only (fix_camera leaves the camera's FOV
    // and aspect at 16:9), the draws that rebuild their projection from those fields (objects, characters) sit at the wrong height, and
    // auto falls back to 16:9 with bars (the 0.4.1 letterbox). A forced [display] aspect then still gets the matrix-only taller view.
    wide().taller_fov = settings::get_bool(settings::Key::TallerFov);
    const bool taller_auto = !wide().taller_fov && !(d.aspect > 0) && aspect < kAspect169 && !close_to(aspect, kAspect169);
    if (taller_auto) {
        NXLOG_I(kTag, "aspect auto: the window (%dx%d) is taller than 16:9, so the picture stays 16:9 with bars (#73; [display] aspect forces a "
                "shape)", fixed_size ? d.render_width : window_w, fixed_size ? d.render_height : window_h);
        aspect = kAspect169;
    }
    Wide& wd = wide();
    wd.aspect = close_to(aspect, kAspect169) ? kAspect169 : aspect;
    const bool override_size = settings::get_bool(settings::Key::RenderOverride);
    if (wide_on()) {   // a non-16:9 shape always takes the wide path (render_override only picks the size for 16:9)
        int gw = kTableW, gh = kTableH;
        if (wd.aspect > kAspect169) gw = static_cast<int>(std::lround(kTableH * wd.aspect)) & ~1;
        else gh = static_cast<int>(std::lround(kTableW / wd.aspect)) & ~1;
        gw = std::clamp(gw, 640, 8192);
        gh = std::clamp(gh, 360, 8192);
        game_target_memory(base, &gw, &gh);
        s.w.store(gw);
        s.h.store(gh);
        install_render_size_hooks(base);
        s.hooks->add_mid(base + kCamSubmit, &hook_cam_submit);
        s.hooks->add_mid(base + kProjBuild, &hook_proj_build);
        wd.ui_size = mem::alloc_kernel(16);
        if (wd.ui_size) {
            wr<int32_t>(wd.ui_size, gw);
            wr<int32_t>(wd.ui_size + 4, gh);
            s.hooks->add_mid(base + kUiCamera, &hook_ui_camera);
        }
        if (wd.aspect > kAspect169) {
            s.hooks->add_mid(base + kTextSpacing, &hook_text_spacing);
            s.hooks->add_mid(base + kTextWriterStyle, &hook_text_writer_style);
        }
        float scale = 1.0f;
        if (fixed_size) scale = static_cast<float>(d.render_height) / gh;
        else if (d.render_scale <= 0) {
            // The console's resolution, not the window's: the game's targets stay the size the game picks (1080p docked, 720p
            // handheld) and the present pass upscales to the window. Rendering them at the window's height was 1.78x the pixels
            // in a 1440p window and measured 42 fps against 60.0 at 1080p in the same world scene on an M3 (the GPU is fill-rate
            // bound there); the sharper render stays available with [display] render_scale or render_width/render_height.
            scale = host_is_handheld() ? std::min(1.0f, std::min(static_cast<float>(window_h) / gh, static_cast<float>(window_w) / gw)) : 1.0f;
        }
        else scale = static_cast<float>(std::clamp(d.render_scale, 25, 400)) / 100.0f;
        gfx::tuning().render_scale.store(scale);
        NXLOG_I(kTag, "aspect %.4f (%s, window %dx%d): %dx%d game targets at scale %.3f, 3D view %s, UI kept 16:9 and centred, dynamic resolution %s",
                wd.aspect, d.aspect > 0 ? "[display] aspect" : "auto", window_w, window_h, gw, gh, scale,
                wd.aspect > kAspect169 ? "Hor+ (same vertical FOV, wider horizontal)" : wd.taller_fov ? "16:9 width kept, taller (camera FOV and aspect, [pkp] taller_fov)" : "16:9 width kept, taller", d.dynamic_resolution ? "on" : "off");
        s.render_installed = true;
        return;
    }
    // Below the console's 1920x1080 the game keeps its own size, whatever [pkp] render_override says. The game scales its layouts to its
    // targets in whole pixels, pane by pane, so the parts of a layout stop adding up: the tiles of the terminal menu are two 368 px
    // halves with a 0 px piece between them, at 832x468 they came out as 159 + 1 + 159 and the 1 px piece drew a column of the half's
    // texture, a line down the tile (#490). The back end renders the game's 1920x1080 targets at h / 1080 instead: the same number of
    // pixels, laid out as the game authored it. Sizes from 1920x1080 up stay the game's own (its text is then drawn at that size).
    const bool below_console = override_size && h < kTableH;
    if (!override_size || below_console) {
        // The game's own size (the back end's [gfx] render_scale enlarges it). Its dynamic resolution still drops the scene viewport below
        // 1920x1080 when frames run late (1664x936 seen at 1440p: the scene then covers part of the targets, a hard-edged band): with
        // [display] dynamic_resolution off, the per-frame copy of the parameters keeps the top level ([upscale] scene_scale below it).
        s.hooks->add_entry(base + kCopyParams, hook_copy_params);
        // [display] render_scale for the NVN back end, which renders the game's targets at that factor: a fixed size by its height, auto
        // by the window's height (at least 1), a percentage of the console's 1920x1080
        float scale = 1.0f;
        if (below_console) scale = static_cast<float>(h) / kTableH;   // the size pick_size chose
        else if (d.render_width > 0 && d.render_height > 0) scale = static_cast<float>(d.render_height) / kTableH;
        // the console's resolution (see above): the game's targets are not enlarged to the window, the present pass upscales
        else if (d.render_scale <= 0) scale = host_is_handheld() ? std::min(1.0f, static_cast<float>(window_h) / kTableH) : 1.0f;
        else scale = static_cast<float>(std::clamp(d.render_scale, 25, 400)) / 100.0f;
        gfx::tuning().render_scale.store(scale);
        NXLOG_I(kTag, "aspect 16:9 (%s, window %dx%d): letterboxed on other window shapes", d.aspect > 0 ? "[display] aspect" : "auto", window_w, window_h);
        NXLOG_I(kTag, "render resolution: 1920x1080 targets at scale %.3f (window %dx%d, [display] render_scale %s), dynamic resolution %s", scale,
                window_w, window_h, d.render_scale <= 0 ? "auto" : std::to_string(d.render_scale).c_str(), d.dynamic_resolution ? "on" : "off");
        s.render_installed = true;
        return;
    }
    game_target_memory(base, &w, &h);
    s.w.store(w);
    s.h.store(h);
    install_render_size_hooks(base);
    s.render_installed = true;
    NXLOG_I(kTag, "render resolution %dx%d (window %dx%d, scale %d%%%s%s), dynamic resolution %s", w, h, window_w, window_h, d.render_scale,
            d.render_width > 0 && d.render_height > 0 ? ", fixed size" : "", d.render_scale <= 0 && host_is_handheld() ? ", handheld: the window's size" : "",
            d.dynamic_resolution ? "on" : "off");
}

void settings_changed(const play::Settings& set) { st().dynres.store(set.display.dynamic_resolution); }

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.installed) return;
    const uint64_t base = in.base;
    const play::Settings& set = in.settings;
    s.base = base;
    s.host = in.host;
    s.hooks = &in.hooks;
    s.fps_fix.store(settings::get_bool(settings::Key::FrameTimeFix));
    s.hold_on.store(settings::get_bool(settings::Key::EventHold));
    s.hold_ns = static_cast<uint64_t>(settings::get_int(settings::Key::EventHoldMs)) * 1000000ull;
    s.mouse_cam.store(settings::get_bool(settings::Key::MouseCamera));
    s.game_limit.store(settings::get_bool(settings::Key::GameFrameLimit));
    s.cam_every_frame.store(settings::get_bool(settings::Key::CameraEveryFrame));
    s.grace_ns = static_cast<uint64_t>(settings::get_int(settings::Key::CameraGraceMs)) * 1000000ull;
    s.hooks->add_mid(base + kFrameMultiplier, &hook_frame_multiplier);
    for (const uint64_t site : kTweenStep) s.hooks->add_mid(base + site, &hook_tween_step);
    s.hooks->add_mid(base + kSyncPeriod, &hook_sync_period);
    s.hooks->add_mid(base + kFixedStepRate, &hook_fixed_step_rate);
    s.hooks->add_mid(base + kCamField, &hook_cam_field);
    s.hooks->add_mid(base + kCamStick, &hook_cam_stick);
    s.hooks->add_mid(base + kFollowGate, &hook_follow_gate);
    s.hooks->add_mid(base + kPhotoStickX, &hook_photo_stick_x);
    s.hooks->add_mid(base + kPhotoStickY, &hook_photo_stick_y);
    s.hooks->add_mid(base + kMmStyle, &hook_mm_style);
    s.hooks->add_mid(base + kMmOption, &hook_mm_option);
    s.hooks->add_mid(base + kMmStart, &hook_mm_start);
    s.installed = true;
    settings_changed(set);
    NXLOG_I(kTag, "frame-time fix %s (event hold %s, %llu ms), game's 60 Hz wait %s (fps limit %d, vsync %s), mouse camera %s, camera placement %s, grace %llu ms",
            s.fps_fix.load() ? "on" : "off", s.hold_on.load() ? "on" : "off", static_cast<unsigned long long>(s.hold_ns / 1000000ull),
            s.game_limit.load() ? "kept" : "off", set.display.fps_cap, set.display.vsync ? "on" : "off",
            s.mouse_cam.load() ? "on" : "off", s.cam_every_frame.load() ? "every frame" : "game's 120 Hz tick",
            static_cast<unsigned long long>(s.grace_ns / 1000000ull));
}

void remove() {   // the registry has removed play's hooks
    State& s = st();
    if (s.mm_want) hid::jc2_mouse_request(false);
    s.mm_want = false;
    s.mm_gate.store(false);
    s.hooks = nullptr;
    s.installed = s.render_installed = false;
    s.host = nullptr;
}

void status_text(char* buf, size_t n) {
    const State& s = st();
    const int k = std::snprintf(buf, n, " render=%dx%d res_hooks=%llu copies=%llu set_vp=%llu vp_fixes=%llu mult=%.3f short_frames=%llu sync=%llu fixed_step=%llu cam=%llu cam_moves=%llu cam_field=%llu follow_forced=%llu cam_dropped=%llu mouse_mode=%d mm_dropped=%llu photo=%llu photo_moves=%llu", s.w.load(), s.h.load(),
                  static_cast<unsigned long long>(s.res_calls.load()), static_cast<unsigned long long>(s.copy_calls.load()), static_cast<unsigned long long>(g_set_vp_fixes.load()), static_cast<unsigned long long>(s.vp_fixes.load()),
                  static_cast<double>(s.last_mult.load()), static_cast<unsigned long long>(s.fps_calls.load()),
                  static_cast<unsigned long long>(s.sync_calls.load()), static_cast<unsigned long long>(s.fixed_calls.load()),
                  static_cast<unsigned long long>(s.cam_calls.load()), static_cast<unsigned long long>(s.cam_moves.load()),
                  static_cast<unsigned long long>(s.cam_field.load()), static_cast<unsigned long long>(s.follow_forced.load()),
                  static_cast<unsigned long long>(s.cam_dropped.load()), s.mm_want ? 1 : 0, static_cast<unsigned long long>(s.mm_dropped.load()),
                  static_cast<unsigned long long>(s.photo_calls.load()), static_cast<unsigned long long>(s.photo_moves.load()));
    if (k > 0 && static_cast<size_t>(k) < n && wide_on()) {
        const Wide& w = wide();
        std::snprintf(buf + k, n - k, " aspect=%.3f cam_fixes=%llu aspect_puts=%llu fov_puts=%llu ui_fixes=%llu", w.aspect,
                      static_cast<unsigned long long>(w.cam_fixes.load()), static_cast<unsigned long long>(w.aspect_puts.load()),
                      static_cast<unsigned long long>(w.fov_puts.load()), static_cast<unsigned long long>(w.ui_fixes.load()));
    }
}

}  // namespace

const Feature kPlayFeature = {
    .name = "play",
    .install_render = &install_render,
    .install = &install,
    .remove = &remove,
    .frame = &play_frame,
    .settings_changed = &settings_changed,
    .status_text = &status_text,
};

play::DisplayLive play_display_live() {
    play::DisplayLive l;
    l.render_w = st().w.load();
    l.render_h = st().h.load();
    if (wide_on()) l.aspect = wide().aspect;
    l.dynamic_resolution = st().dynres.load();
    return l;
}

int play_ui_canvas_height() {
    const uint64_t size = wide().ui_size;
    return size ? rd<int32_t>(size + sizeof(int32_t)) : kTableH;
}

uint32_t play_display_calls() { return static_cast<uint32_t>(st().res_calls.load() + st().copy_calls.load()); }

bool play_follow_live() {
    const uint64_t t = g_follow_ns.load(std::memory_order_relaxed);
    return t != 0 && mono_ns() - t < 1000000000ull;
}

bool play_photo_live() {
    const uint64_t t = g_photo_ns.load(std::memory_order_relaxed);
    return t != 0 && mono_ns() - t < st().grace_ns;
}

}  // namespace nxrt::games::pkp
