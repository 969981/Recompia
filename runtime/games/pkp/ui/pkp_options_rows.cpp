// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the rows of the PC settings tabs (pkp_options_internal.h): each binds to a field of the play-mode Settings behind
// controls.ini, the same store as the F1 page, except the Gameplay and Inventory tabs', which bind to the gameplay mods' [pkp] keys of the game
// ini.
// Nothing reaches the save file.
#include "ui/pkp_options_internal.h"
#include "core/pkp_game.h"
#include "nxrt/game_render.h"
#include "render/pkp_phone_res.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/extras.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/host_device.h"
#include "nxrt/input.h"
#include "nxrt/platform.h"
#include "play_game.h"

namespace nxrt::games::pkp::options_detail {
namespace {

// clang-format off
#define ROW_BOOL(p) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = v != 0; }
#define ROW_ENUM(p, T) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = static_cast<T>(v); }
#define ROW_PCT(p) [](const S& s) { return static_cast<int>(std::lround(s.p * 100.0)); }, [](S& s, int v) { s.p = v / 100.0; }
#define ROW_INT(p) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = v; }
#define ROW_STEP(p, base, step) [](const S& s) { return static_cast<int>(std::lround((s.p - (base)) / (step))); }, [](S& s, int v) { s.p = (base) + v * (step); }

int index_of(const int* values, int n, int v, int otherwise) {
    for (int i = 0; i < n; ++i) if (values[i] == v) return i;
    return otherwise;
}
int index_of(const double* values, int n, double v, int otherwise) {
    for (int i = 0; i < n; ++i) if (std::fabs(values[i] - v) < 1e-6) return i;
    return otherwise;
}

constexpr int kFpsValues[] = {-1, 30, 40, 45, 60, 90, 120, 144, 165, 240, 0};
constexpr int kFpsCount = static_cast<int>(std::size(kFpsValues));
constexpr const char* kFpsNames[kFpsCount + 1] = {"Match Display", "30", "40", "45", "60", "90", "120", "144", "165", "240", "Unlimited", "Custom"};
// A phone's frame limits come from its panel: Match Display, every exact divisor of the refresh rate down to 18 fps (120 Hz: 120 60 40
// 30 24 20), Unlimited. Each is stored as the whole number the limiter snaps to that divisor (fps_snap, frame_limiter snap_frame_cap: 90 / 4
// = 22.5 is stored as 23) and labelled with its real value, rounded down. A panel of unknown rate offers the 60 Hz list.
float fps_panel_hz() { const float hz = platform::display_refresh_hz(); return hz > 0.0f ? hz : 60.0f; }
int fps_divisors() { int n = 0; while (fps_panel_hz() / static_cast<float>(n + 1) >= 18.0f && n < 12) ++n; return n; }
int fps_value(int pos) {   // pos 0: Match Display, 1..n: hz / pos, n + 1: Unlimited
    const int n = fps_divisors();
    return pos <= 0 ? -1 : pos > n ? 0 : static_cast<int>(std::ceil(fps_panel_hz() / static_cast<float>(pos) - 1e-3f));
}
const char* fps_text(int pos, char* out, size_t n) {
    const int d = fps_divisors();
    if (pos <= 0) return "Match Display";
    if (pos > d) return "Unlimited";
    std::snprintf(out, n, "%d", static_cast<int>(fps_panel_hz() / static_cast<float>(pos)));
    return out;
}
int fps_max() { return fps_divisors() + 1; }
// Any stored cap selects the entry the limiter runs it at (the snap's divisor k = ceil(hz / (cap * 1.005)), frame_limiter
// snap_frame_cap): a 25 from an older list or another app shows as 24 on a 120 Hz panel, not as Match Display.
int fps_get(const S& s) {
    const int cap = s.display.fps_cap, n = fps_divisors();
    if (cap < 0) return 0;
    if (cap == 0) return n + 1;
    const int k = static_cast<int>(std::ceil(fps_panel_hz() / (static_cast<float>(cap) * 1.005f)));
    return std::clamp(k, 1, n);
}
void fps_set(S& s, int v) { s.display.fps_cap = fps_value(v); play::quality_preset_edited(s.display, play::QualityField::FrameLimit); }
bool not_phone() { return !nxrt::host_is_phone(); }

// A phone's Resolution row: [pkp] resolution (render/pkp_phone_res.h), percent of the screen per side in 5 % steps from kPhoneResMin up to
// the screen (or its presented share); a value between the steps (worked out from the keys before it) is an entry of its own. Each entry
// shows the 3D picture's size in pixels. Up to the targets of this start (the whole screen, or on the low-memory profile the start
// percent) it changes at once; above them it applies at the next start.
std::vector<int> res_values() {
    std::vector<int> v;
    const int top = phone_res_max(), cur = phone_res_setting();
    for (int p = kPhoneResMin; p < top; p += 5) v.push_back(p);
    v.push_back(top);
    if (std::find(v.begin(), v.end(), cur) == v.end()) v.insert(std::upper_bound(v.begin(), v.end(), cur), cur);
    return v;
}
const char* res_text(int pos, char* out, size_t n) {
    const std::vector<int> v = res_values();
    const int pct = v[static_cast<size_t>(std::clamp(pos, 0, static_cast<int>(v.size()) - 1))];
    int w = 0, h = 0;
    phone_res_pixels(pct, &w, &h);
    std::snprintf(out, n, pct > phone_res_ceiling() ? "%d%% (%d x %d), next start" : "%d%% (%d x %d)", pct, w, h);
    return out;
}
int res_max() { return static_cast<int>(res_values().size()) - 1; }
int res_get(const S&) {
    const std::vector<int> v = res_values();
    return static_cast<int>(std::find(v.begin(), v.end(), phone_res_setting()) - v.begin());
}
void res_set(S&, int pos) {   // the game ini at once: the scene follows from the next frame (pkp_render_size.cpp phone_scene_pct)
    const std::vector<int> v = res_values();
    settings::set_int(settings::Key::Resolution, v[static_cast<size_t>(std::clamp(pos, 0, static_cast<int>(v.size()) - 1))]);
    // the frame rate counter's line too, at once: it is otherwise published by the per-frame copy of the game's size parameters,
    // which may not run while the game's menu is open (a step back up to 50 % on a Tensor G2 phone showed 45 % there)
    gfx::tuning().res_pct.store(phone_res_value(), std::memory_order_relaxed);
}
constexpr Row live_row(Row r) { r.live = true; return r; }
// A phone's one Upscaler row: Off, SGSR ([pkp] scene_upscaler) or SGSR 2 ([upscale] mode sgsr2, temporal): the 3D picture alone, enlarged
// in one step to the screen; menus native. Each choice clears the other key (and FSR 1 of older builds). Applies at once.
int phone_upscaler_get(const S& s) {
    if (s.extras.upscale.mode == extras::UpscaleMode::Sgsr2) return 2;
    return settings::get_int(settings::Key::SceneUpscaler) != 0 ? 1 : 0;
}
void phone_upscaler_set(S& s, int v) {
    s.extras.upscale.mode = v == 2 ? extras::UpscaleMode::Sgsr2 : extras::UpscaleMode::Off;
    settings::set_int(settings::Key::SceneUpscaler, v == 1 ? 1 : 0);   // the game ini, like a key row
}

constexpr int kWidths[] = {1280, 1600, 1920, 2560, 3840}, kHeights[] = {720, 900, 1080, 1440, 2160};
constexpr int kSizeCount = static_cast<int>(std::size(kWidths));
constexpr const char* kSizeNames[kSizeCount + 1] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Custom"};
// [display] aspect: 0 is auto (the window's shape); read once at start (pkp_play.cpp install_render), so the row applies after a restart.
constexpr double kAspectValues[] = {0.0, 16.0 / 9.0, 16.0 / 10.0, 21.0 / 9.0, 32.0 / 9.0};
constexpr int kAspectCount = static_cast<int>(std::size(kAspectValues));
constexpr const char* kAspectNames[kAspectCount + 1] = {"Auto", "16:9", "16:10", "21:9", "32:9", "Custom"};
constexpr int kAniso[] = {0, 2, 4, 8, 16};
constexpr const char* kAnisoNames[] = {"Game Default", "2x", "4x", "8x", "16x"};
// In play::PromptStyle order (the row stores the enum's number).
constexpr const char* kPromptNames[] = {"Switch",      "Xbox",        "PlayStation", "Steam Controller", "Steam Deck", "Nintendo Switch", "Switch 2",
                                        "Wii U",       "Steam Frame", "Valve Index", "Touch",            "Keyboard",   "Automatic"};
static_assert(std::size(kPromptNames) == play::kPromptStyleCount, "one name per PromptStyle");
constexpr const char* kAheadNames[] = {"No Limit", "0", "1", "2", "3"};
constexpr const char* kAaNames[] = {"Off", "SMAA Low", "SMAA Medium", "SMAA High", "SMAA Ultra"};      // indexed by extras::AaMode
constexpr const char* kShadowNames[] = {"Game Default", "High (2x)", "Very High (3x)", "Ultra (4x)", "Extreme (8x)"};      // indexed by extras::ShadowQuality
constexpr const char* kAstcKeys[] = {"fast", "high", "uncompressed"};
constexpr const char* kUniformSnapshotKeys[] = {"auto", "on", "off"};
constexpr const char* kWindowKeys[] = {"windowed", "borderless", "exclusive"};

std::vector<std::string> g_devices;   // audio output names, refreshed when the screen opens

int key_index(const char* const* keys, int n, const std::string& v) {
    for (int i = 0; i < n; ++i) if (v == keys[i]) return i;
    return 0;
}

const char* preset_text(int v, char*, size_t) {
    int n = 0;
    const play::QualityPresetInfo* q = play::quality_presets(&n);
    return q[std::clamp(v, 0, n - 1)].name;
}
int preset_max() { int n = 0; play::quality_presets(&n); return n - 1; }

const char* monitor_text(int v, char* out, size_t n) {
    if (v <= 0) return "Automatic";
    if (!platform::display_name(v, out, static_cast<int>(n))) std::snprintf(out, n, "Display %d", v);
    return out;
}
int monitor_max() { return std::max(platform::display_count(), 1); }

// [pad] controller: 0 = every pad, n = the pad SDL numbers player n (GamepadInfo::player + 1)
const char* controller_text(int v, char* out, size_t n) {
    if (v <= 0) return "All Controllers";
    input::GamepadInfo pads[8];
    const size_t count = input::gamepad_list(pads, 8);
    for (size_t i = 0; i < count; ++i) {
        if (pads[i].player + 1 == v) {
            std::snprintf(out, n, "%d: %s", v, pads[i].name);
            return out;
        }
    }
    std::snprintf(out, n, "%d: Not Connected", v);
    return out;
}
int controller_max() { return 8; }

const char* device_text(int v, char*, size_t) {
    return v <= 0 || v > static_cast<int>(g_devices.size()) ? "System Default" : g_devices[static_cast<size_t>(v - 1)].c_str();
}
int device_max() { return std::clamp(static_cast<int>(g_devices.size()), 1, 250); }

// The desktop Upscaler row's entries, in this order (the stored [upscale] mode values do not depend on it); the temporal ones only where
// this machine can run them (extras::upscale_mode_available), asked once.
const std::vector<extras::UpscaleMode>& upscalers() {
    static const std::vector<extras::UpscaleMode> v = [] {
        std::vector<extras::UpscaleMode> r;
        for (extras::UpscaleMode m : {extras::UpscaleMode::Off, extras::UpscaleMode::Nearest, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Fsr3,
                                      extras::UpscaleMode::Dlss, extras::UpscaleMode::Dlaa, extras::UpscaleMode::Sgsr2})
            if (extras::upscale_mode_available(m)) r.push_back(m);
        return r;
    }();
    return v;
}
int upscaler_max() { return static_cast<int>(upscalers().size()) - 1; }
const char* upscaler_text(int v, char*, size_t) {
    switch (upscalers()[static_cast<size_t>(std::clamp(v, 0, upscaler_max()))]) {
        case extras::UpscaleMode::Nearest: return "Nearest";
        case extras::UpscaleMode::Fsr1: return "FSR 1";
        case extras::UpscaleMode::Fsr3: return "FSR 3";
        case extras::UpscaleMode::Dlss: return "DLSS";
        case extras::UpscaleMode::Dlaa: return "DLAA";
        case extras::UpscaleMode::Sgsr2: return "SGSR 2";
        default: return "Off";
    }
}
int upscaler_get(const S& s) {
    const auto& v = upscalers();
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == s.extras.upscale.mode) return static_cast<int>(i);
    return 0;
}
void upscaler_set(S& s, int v) { s.extras.upscale.mode = upscalers()[static_cast<size_t>(std::clamp(v, 0, upscaler_max()))]; }

const Row kSound[] = {
    number("Master Volume", "The volume of everything in the game, on top of the sliders above.", 0, 100, "%.0f%%", 0, 1, ROW_PCT(audio.volume)),
    choice("Mute in Background", "Silence the game while its window is not in focus.", kOffOn, 2, ROW_BOOL(audio.mute_unfocused)),
    dynamic("Output Device", "The sound device the game plays through.", device_text, device_max,
            [](const S& s) { for (size_t i = 0; i < g_devices.size() && i < 250; ++i) if (g_devices[i] == s.audio.device) return static_cast<int>(i) + 1; return 0; },
            [](S& s, int v) { s.audio.device = v <= 0 || v > static_cast<int>(g_devices.size()) ? "default" : g_devices[static_cast<size_t>(v - 1)]; }, true),
};

// Window rows only where the window system has a choice of modes (a phone has one: nxrt/platform.h window_modes).
bool several_window_modes() { const int m = platform::window_modes(); return (m & (m - 1)) != 0; }

constexpr Row kDisplay[] = {
    when(choice("Window Mode", "Windowed, borderless fullscreen or exclusive fullscreen.", {"Windowed", "Borderless", "Fullscreen"}, 3,
           [](const S& s) { return key_index(kWindowKeys, 3, s.display.window_mode); }, [](S& s, int v) { s.display.window_mode = kWindowKeys[v]; }), several_window_modes),
    when(dynamic("Monitor", "The display the window moves to. Fullscreen modes use this display.", monitor_text, monitor_max, ROW_INT(display.monitor)), several_window_modes),
    when(names("Window Size", "The size of the window in windowed mode.", kSizeNames, kSizeCount + 1,
          [](const S& s) { for (int i = 0; i < kSizeCount; ++i) if (s.display.width == kWidths[i] && s.display.height == kHeights[i]) return i; return kSizeCount; },
          [](S& s, int v) { if (v < kSizeCount) { s.display.width = kWidths[v]; s.display.height = kHeights[v]; } }), several_window_modes),
    names("Aspect Ratio", "The shape of the picture. Auto follows the window; 16:9 is the game's own.", kAspectNames, kAspectCount + 1,
          [](const S& s) { return index_of(kAspectValues, kAspectCount, s.display.aspect, kAspectCount); },
          [](S& s, int v) { if (v < kAspectCount) s.display.aspect = kAspectValues[v]; }, true),
    choice("Letterbox", "Black bars keep the picture's shape when it does not fit the window, instead of stretching it.", kOffOn, 2, ROW_BOOL(display.keep_aspect)),
    when(choice("VSync", "Wait for the display between frames: no tearing, a little more delay.", kOffOn, 2, ROW_BOOL(display.vsync)), not_phone),
    when(names("Frame Rate Limit", "The highest frame rate. Match Display follows the refresh rate.", kFpsNames, kFpsCount + 1,
          [](const S& s) { return index_of(kFpsValues, kFpsCount, s.display.fps_cap, kFpsCount); },
          [](S& s, int v) { if (v < kFpsCount) { s.display.fps_cap = kFpsValues[v]; play::quality_preset_edited(s.display, play::QualityField::FrameLimit); } }), not_phone),
    when(dynamic("Frame Rate Limit", "The highest frame rate, each a whole number of the screen's refreshes (even frames). Lower keeps a hot phone running evenly.",
                 fps_text, fps_max, fps_get, fps_set), nxrt::host_is_phone),
    choice("Even Frame Pacing", "With VSync, run at an even divisor of the refresh rate (Steam Deck: 30 instead of 40).", kOffOn, 2,
           [](const S& s) { return static_cast<int>(s.display.fps_snap == "true" || (s.display.fps_snap == "auto" && (nxrt::host_is_steam_deck() || nxrt::host_is_phone()))); },
           [](S& s, int v) { s.display.fps_snap = v ? "true" : "false"; }),
    when(autonum("Render Scale", "The game's resolution; 100% is 1920 x 1080. Auto follows the window.", 76, "%.0f%%", 20, 5,
            [](const S& s) { return s.display.render_scale <= 0 ? 0 : std::clamp((s.display.render_scale - 20) / 5, 1, 76); },
            [](S& s, int v) { s.display.render_scale = v <= 0 ? 0 : 20 + v * 5; play::quality_preset_edited(s.display, play::QualityField::RenderScale); }, true), not_phone),
    when(live_row(dynamic("Resolution", "The 3D picture's resolution, in percent of the screen; menus and text stay sharp. Changes at once (an entry marked next start: after a restart).", res_text, res_max,
                          res_get, res_set)), nxrt::host_is_phone),
    // a phone's Dynamic Resolution sits with its Resolution row (the Graphics tab's copy is left out there)
    when(choice("Dynamic Resolution", "Lowers the resolution below the one above while the GPU falls behind, and raises it back up to it.", kOffOn, 2,
                ROW_BOOL(display.dynamic_resolution)), nxrt::host_is_phone),
    when(choice("Dynamic Resolution Mode", "GPU Time: lower the resolution when the GPU falls behind. Heat: also hold it below what keeps the phone from throttling, so the GPU can run cooler.",
                {"GPU Time", "Heat"}, 2, [](const S&) { return settings::get_str(settings::Key::DynamicResolutionMode) == "thermal" ? 1 : 0; },
                [](S&, int v) { settings::set_str(settings::Key::DynamicResolutionMode, v ? "thermal" : "gpu"); }), nxrt::host_is_phone),
};

const Row kPerformance[] = {
    when(dynamic("Upscaler", "How the picture is enlarged from a lower resolution. FSR 3, DLSS and SGSR 2 build a sharper picture from several frames (DLSS and DLAA apply after a restart).",
                 upscaler_text, upscaler_max, upscaler_get, upscaler_set), not_phone),
    when(number("Sharpness", "How much FSR 1, FSR 3 or SGSR 2 sharpens the picture.", 0, 20, "%.2f", 0, 0.05, ROW_STEP(extras.upscale.sharpness, 0.0, 0.05)), not_phone),
    // a phone's Upscaler and its Sharpness sit here too, at the same place as the desktop's (the Display tab keeps Resolution)
    when(choice("Upscaler", "Sharpens the enlarged 3D picture: SGSR (Snapdragon Game Super Resolution), or SGSR 2, which builds detail over several "
                "frames; menus stay sharp.", {"Off", "SGSR", "SGSR 2"}, 3, phone_upscaler_get, phone_upscaler_set), nxrt::host_is_phone),
    when(live_row(number("Sharpness", "How much SGSR 2 sharpens its picture; SGSR does not use it. Higher is crisper, and fine detail can shimmer more.", 0,
                         20, "%.2f", 0, 0.05, ROW_STEP(extras.upscale.sharpness, 0.0, 0.05))), nxrt::host_is_phone),
    when(number("Scene Resolution", "Renders the 3D scene smaller and keeps the menus sharp. 100% turns it off.", 25, 100, "%.0f%%", 0, 1, ROW_INT(extras.upscale.scene_scale)), not_phone),
    names("Anti-Aliasing", "Smooths jagged edges of the 3D scene. The menus are not filtered.", kAaNames, 5,
          [](const S& s) { return static_cast<int>(s.extras.upscale.aa); },
          [](S& s, int v) { s.extras.upscale.aa = static_cast<extras::AaMode>(v); play::quality_preset_edited(s.display, play::QualityField::AntiAliasing); }),
    choice("Low Latency", "Reduces input delay on GPUs that support it.", {"Off", "Auto", "On", "Boost"}, 4, ROW_ENUM(extras.latency.mode, extras::LatencyMode), true),
    names("Frames Ahead", "How many frames may wait for the screen. Fewer is more responsive.", kAheadNames, 5,
          [](const S& s) { return s.extras.latency.frames_ahead + 1; }, [](S& s, int v) { s.extras.latency.frames_ahead = v - 1; }, true),
    choice("Frame Rate Counter", "Show the frame rate on screen.", kOffOn, 2, ROW_BOOL(fps.show)),
    choice("Counter Corner", "Where the frame rate counter sits.", {"Top Left", "Top Right", "Bottom Left", "Bottom Right"}, 4, ROW_ENUM(fps.corner, play::Corner)),
    when(choice("Counter Detail", "What the frame rate counter shows.", {"FPS", "FPS and Time", "Full"}, 3, ROW_INT(fps.detail)), not_phone),
    when(choice("Counter Detail", "What the frame rate counter shows. Full adds the 1% low, the worst frame, how hot the phone is and how busy the GPU and CPU are against the clock limits the heat sets.",
                {"FPS", "FPS and Time", "Full"}, 3, ROW_INT(fps.detail)), nxrt::host_is_phone),
    key_row("Camera aim: show the ray", "Draws camera aim's ray over the picture: the blocks it tested, the block it hit and the block it selected, with the reason in a panel at the top left. For finding aiming bugs.", settings::Key::CameraAimDebug),
};

const Row kGraphics[] = {
    dynamic("Quality Preset", "Sets the texture options together.", preset_text, preset_max,
            [](const S& s) { int n = 0; const play::QualityPresetInfo* q = play::quality_presets(&n); for (int i = 0; i < n; ++i) if (s.display.quality_preset == q[i].id) return i; return n - 1; },
            [](S& s, int v) { int n = 0; const play::QualityPresetInfo* q = play::quality_presets(&n); const play::GamePlay* gp = play::game_play(); if (v < n - 1 && gp && gp->quality_preset) gp->quality_preset(q[v].id, s); }),
    when(choice("Dynamic Resolution", "Let the game lower its resolution when the GPU falls behind.", kOffOn, 2, ROW_BOOL(display.dynamic_resolution)), not_phone),
    names("Texture Filtering", "Sharper textures at an angle.", kAnisoNames, 5,
          [](const S& s) { return index_of(kAniso, 5, s.display.anisotropy, 0); },
          [](S& s, int v) { s.display.anisotropy = kAniso[v]; play::quality_preset_edited(s.display, play::QualityField::Anisotropy); }),
    names("Shadow Quality", "Makes the game's shadow maps 2x, 3x, 4x or 8x sharper per side. Ultra and Extreme cost a lot of video memory.", kShadowNames, 5,
          [](const S& s) { return static_cast<int>(s.display.shadow_quality); },
          [](S& s, int v) { s.display.shadow_quality = static_cast<gfx::ShadowQuality>(v); play::quality_preset_edited(s.display, play::QualityField::Shadows); }),
    number("Texture Sharpness", "Texture level of detail. Negative is sharper.", 0, 80, "%+.2f", -2, 0.05,
           [](const S& s) { return static_cast<int>(std::lround((s.display.lod_bias + 2.0) / 0.05)); },
           [](S& s, int v) { s.display.lod_bias = -2.0 + v * 0.05; play::quality_preset_edited(s.display, play::QualityField::LodBias); }),
    choice("Texture Compression", "Fast loads quickest; Uncompressed uses a lot of video memory.", {"Fast", "High", "Uncompressed"}, 3,
           [](const S& s) { return key_index(kAstcKeys, 3, s.display.astc_quality); }, [](S& s, int v) { s.display.astc_quality = kAstcKeys[v]; }, true),
    choice("Faster GPU Memory", "Uses more video memory for smoother drawing.", kOffOn, 2, ROW_BOOL(display.fast_gpu_memory), true),
    choice("Constants in Video Memory", "Auto helps graphics cards under 8 GB. Off: read them from system memory as before.", {"Auto", "On", "Off"}, 3,
           [](const S& s) { return key_index(kUniformSnapshotKeys, 3, s.display.uniform_snapshot); },
           [](S& s, int v) { s.display.uniform_snapshot = kUniformSnapshotKeys[v]; }, true),
    choice("Fewer GPU Waits", "Skips pipeline waits that are not needed.", kOffOn, 2, ROW_BOOL(display.fewer_gpu_waits), true),
    choice("Prepare Shaders", "Builds known shaders early to reduce stutter.", kOffOn, 2, ROW_BOOL(display.shader_prewarm), true),
};

const Row kPicture[] = {
    choice("HDR", "High dynamic range output on a display that supports it.", {"Off", "Auto", "On", "Native"}, 4, ROW_ENUM(extras.hdr.mode, extras::HdrMode), true),
    choice("HDR Format", "The HDR signal sent to the display.", {"Auto", "HDR10", "scRGB"}, 3, ROW_ENUM(extras.hdr.format, extras::HdrFormat), true),
    autonum("Peak Brightness", "The brightest the display can go in HDR.", 197, "%.0f nits", 150, 50,
            [](const S& s) { return s.extras.hdr.peak_nits <= 0 ? 0 : std::clamp(static_cast<int>(std::lround((s.extras.hdr.peak_nits - 150) / 50.0)), 1, 197); },
            [](S& s, int v) { s.extras.hdr.peak_nits = v <= 0 ? 0 : 150 + v * 50; }),
    autonum("Paper White", "The brightness of white in HDR.", 43, "%.0f nits", 70, 10,
            [](const S& s) { return s.extras.hdr.paper_white_nits <= 0 ? 0 : std::clamp(static_cast<int>(std::lround((s.extras.hdr.paper_white_nits - 70) / 10.0)), 1, 43); },
            [](S& s, int v) { s.extras.hdr.paper_white_nits = v <= 0 ? 0 : 70 + v * 10; }),
    number("Highlight Boost", "How far highlights reach toward peak brightness.", 0, 40, "%.2f", 0, 0.05, ROW_STEP(extras.hdr.highlight_boost, 0.0, 0.05)),
    Row{"Color Preset", "Neutral is the game's own picture.", {"Neutral", "Vivid", "Custom"}, 3, 0, 0, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr,
        [](const S& s) { extras::PictureSettings v; extras::picture_preset("vivid", &v); const auto& p = s.extras.picture; if (p.neutral()) return 0; return p.contrast == v.contrast && p.saturation == v.saturation && p.black == v.black && p.gamma == v.gamma && p.brightness == v.brightness ? 1 : 2; },
        [](S& s, int v) { if (v < 2) extras::picture_preset(v ? "vivid" : "neutral", &s.extras.picture); }, false, true},
    number("Contrast", "Spreads the tones away from mid grey. 1.00 is the game's own.", 0, 100, "%.2f", 0.5, 0.01, ROW_STEP(extras.picture.contrast, 0.5, 0.01), true),
    number("Saturation", "Color intensity. 1.00 is the game's own.", 0, 100, "%.2f", 0, 0.02, ROW_STEP(extras.picture.saturation, 0.0, 0.02), true),
    number("Black Level", "Below 0 deepens the blacks, above 0 lifts them.", 0, 150, "%+.3f", -0.5, 0.005, ROW_STEP(extras.picture.black, -0.5, 0.005), true),
    number("Gamma", "Mid-tone brightness. 1.00 is the game's own.", 0, 150, "%.2f", 0.5, 0.01, ROW_STEP(extras.picture.gamma, 0.5, 0.01), true),
    number("Brightness", "Dims the whole picture, white included. 1.00 is the game's own.", 0, 75, "%.2f", 0.25, 0.01, ROW_STEP(extras.picture.brightness, 0.25, 0.01), true),
};

// Game actions bound in play. A new key replaces that input's play binding; an all-context binding still works elsewhere.
class PlayBindingKey final : public CaptureClient {
public:
    PlayBindingKey(const char* label, play::Act action, bool keyboard_only = false)
        : label_(label), action_(action), keyboard_only_(keyboard_only) {}

    const char* name() const override { return label_; }
    std::string bound() const override {
        const S s = play::current_settings();
        std::string out;
        for (const play::Binding& b : s.bindings) {
            if (b.act != action_ || (b.ctx != play::Ctx::Play && b.ctx != play::Ctx::All)) continue;
            if (b.ctx == play::Ctx::All && std::any_of(s.bindings.begin(), s.bindings.end(), [&](const play::Binding& other) {
                    return other.ctx == play::Ctx::Play && other.in == b.in;
                })) continue;
            out += (out.empty() ? "" : ", ") + play::input_name(b.in);
        }
        return out.empty() ? "None" : out;
    }
    bool take(const play::InputId& in) override {
        S s = play::current_settings();
        if (keyboard_only_) {
            play::InputId persisted;
            if (in.kind != play::InKind::Key || in == s.fps.key || in == s.quit.key ||
                !play::parse_input(play::input_name(in), &persisted) || persisted != in) return false;
        }
        std::erase_if(s.bindings, [&](const play::Binding& b) { return b.act == action_ || (b.in == in && b.ctx == play::Ctx::Play); });
        s.bindings.push_back(play::Binding{in, play::Ctx::Play, action_, play::Trig::Press, 0});
        play::apply_settings(s);   // main thread, next frame; also writes controls.ini
        return true;
    }

private:
    const char* label_;
    play::Act action_;
    bool keyboard_only_;
};

PlayBindingKey g_mouse_mode_key{"Mouse Mode Key", play::Act::MouseMode};
PlayBindingKey g_save_key{"Quick Save Key", play::Act::Save, true};

#define SENS(p) [](const S& s) { return static_cast<int>(std::lround(play::source_sens(s.mouse.p) * 10.0)); }, [](S& s, int v) { s.mouse.p = play::rad_per_count(v / 10.0); }
const Row kMouse[] = {
    number("Horizontal Sensitivity", "How fast the mouse turns the camera sideways.", 1, 200, "%.1f", 0, 0.1, SENS(yaw_rad)),
    number("Vertical Sensitivity", "How fast the mouse tilts the camera.", 1, 200, "%.1f", 0, 0.1, SENS(pitch_rad)),
    choice("Invert Vertical", "Moving the mouse up looks down.", kOffOn, 2, ROW_BOOL(mouse.invert_y)),
    choice("Invert Horizontal", "Moving the mouse left looks right.", kOffOn, 2, ROW_BOOL(mouse.invert_x)),
    choice("Mouse Pointer in Menus", "Use the mouse as a pointer: hover, click, right click, wheel.", kOffOn, 2, ROW_BOOL(mouse.menu_cursor)),
    names("Button Icons", "The button icons in prompts. Automatic follows the last device used.", kPromptNames, static_cast<uint8_t>(std::size(kPromptNames)), ROW_ENUM(prompts.style, play::PromptStyle)),
    capture_row("Mouse Mode Key", "Turns the Joy-Con 2 mouse mode on and off (point and click to place and break). To change it, press left or right on this row (or click it), then press the new key or mouse button. Esc cancels.",
                g_mouse_mode_key),
    capture_row("Quick Save Key", "Saves while you play. Press left or right on this row (or click it), then press a keyboard key. It replaces that key's action in play. Esc cancels.", g_save_key),
};

const Row kController[] = {
    dynamic("Controller", "The controller that plays. All lets every connected controller play; pick one when other controllers on this PC belong to someone else.",
            controller_text, controller_max, ROW_INT(pad.controller)),
    choice("Face Buttons", "Switch keeps the buttons by position, Xbox by their letters.", {"Switch", "Xbox"}, 2, ROW_BOOL(pad.label_abxy)),
    number("Left Stick Dead Zone", "Ignores small movements of the left stick.", 0, 50, "%.0f%%", 0, 1, ROW_PCT(pad.stick_deadzone)),
    number("Right Stick Dead Zone", "Ignores small movements of the right stick.", 0, 50, "%.0f%%", 0, 1, ROW_PCT(pad.right_deadzone)),
    number("Left Stick Range", "How much of the stick's travel counts as full. Lower it for a worn stick.", 50, 100, "%.0f%%", 0, 1, ROW_PCT(pad.stick_max)),
    number("Right Stick Range", "How much of the stick's travel counts as full. Lower it for a worn stick.", 50, 100, "%.0f%%", 0, 1, ROW_PCT(pad.right_stick_max)),
    choice("Invert Look", "Stick up looks down. Adds to the game's own invert option.", kOffOn, 2, ROW_BOOL(pad.invert_y)),
    number("Look Sensitivity", "Multiplies the right stick while playing.", 0, 55, "%.2fx", 0.25, 0.05, ROW_STEP(pad.look_sens, 0.25, 0.05)),
    number("Trigger Threshold", "How far a trigger must be pressed to count.", 5, 100, "%.0f%%", 0, 1, ROW_PCT(pad.trigger_threshold)),
    choice("Motion Sensor", "Pass the controller's gyro to the game.", kOffOn, 2, ROW_BOOL(pad.gyro)),
    number("Motion Strength", "Scales the gyro.", 0, 78, "%.2fx", 0.1, 0.05, ROW_STEP(pad.gyro_scale, 0.1, 0.05)),
    number("Rumble Strength", "Scales the vibration.", 0, 200, "%.0f%%", 0, 1, ROW_PCT(pad.rumble_scale)),
};

// The gameplay mods (the gameplay() rows of pkp_settings_table.h), labelled as in the launcher's Settings > Gameplay (settings-schema.js
// GAMEPLAY). title_quit has no row (hidden by the repo owner); the inventory mods have their own tab (kInventory). Every mod reads its keys
// when it installs, so these rows apply after a restart;
// the shoulder camera's two distances are live keys (no switch: 0 / 0 is the game's own camera), in force when the screen commits. A tab has
// kDirtySlot (13) rows; camera aim's debug view moved to Performance (an on-screen diagnostic) to keep this tab within that.
using settings::Key;
constexpr Row kGameplay[] = {
    key_row("Running jump keeps speed", "A running jump keeps the running speed in the air.", Key::JumpMomentum),
    key_row("Running jump speed kept", "With the running jump on: how much of the extra running speed is kept.", Key::JumpMomentumStrength, 0.01, "%.0f%%", 100),
    key_row("Camera aim", "Abilities and tools aim where the camera looks instead of where your character faces, and your character turns to the target when the ability starts.", Key::CameraAim),
    key_row("Extra reach in blocks", "With camera aim: extra blocks of reach. Off is the game's own range.", Key::CameraAimReach, 1, "%.0f", 1, "Off"),
    key_row("Search in the crafting screen", "A Search button in the key guide of the crafting screen.", Key::CraftSearch),
    key_row("Save button in the pause menu", "Enables the Save prompt and the Quick Save Key. Both save your progress and keep you in the world.", Key::SaveNow),
    key_row("Item editor (experimental)", "Adds an Items tab to this screen that changes how many of an item you hold. Not during Link Play.", Key::ItemEditor),
    key_row("More creatures in town", "Experimental: more creatures can live in your town than the game's 25.", Key::PokemonCap),
    key_row("Creatures in town", "With More creatures in town: how many can live there, 25 to 50. Visitors and spawns share the game's 50 drawn creatures, so a high number leaves them less room.", Key::PokemonCapLimit, 1, "%.0f"),
    key_row("Shoulder camera: side", "Moves the camera to the side so your character no longer blocks the view, in blocks. Right is positive, left negative; 0 is the game's own camera.", Key::CameraShoulderSide, 0.1, "%+.1f"),
    key_row("Shoulder camera: height", "Moves the camera up, in blocks. 0 is the game's own camera.", Key::CameraShoulderHeight, 0.1, "%+.1f"),
};
// The inventory mods (bag, storage boxes), gameplay() rows like kGameplay's and labelled as in the launcher, in a tab of their own so the
// Gameplay tab keeps room within its kDirtySlot (13) rows.
constexpr Row kInventory[] = {
    key_row("Sort button in the bag and containers", "Click Hold to Sort in the bag and storage boxes to sort at once.", Key::InventorySort),
    key_row("Sort each time the bag opens", "Sorts the bag each time the bag or a container screen opens.", Key::InventoryAutosort),
    key_row("Storage box tabs", "Extra pages in storage boxes; L or R switches them. About the first 22 boxes (big boxes: 42) get every page. Off in Link Play.", Key::BoxTabs),
    key_row("Pages per storage box", "Pages per storage box, 2 to 12 (20 slots each). A page that holds items stays shown if you lower this.", Key::BoxTabPages, 1, "%.0f"),
    key_row("Tabs per big storage box", "Tabs of 60 slots per big storage box, 1 to 6 (6: 360 slots). A tab that holds items stays shown if you lower this.", Key::BigBoxTabs, 1, "%.0f"),
};
// clang-format on

}  // namespace

// Display holds more rows than a tab's slots, split by machine: a phone leaves out the not_phone rows, any other machine the phone rows.
template <size_t N>
constexpr size_t most_shown(const Row (&r)[N]) {
    size_t phone = 0, other = 0;
    for (const Row& x : r) {
        phone += x.shown != &not_phone;
        other += x.shown != &nxrt::host_is_phone;
    }
    return std::max(phone, other);
}
// GCC 11 (the Linux release runners, Ubuntu 22.04) cannot compare two different functions' addresses in a constant expression; GCC 12+,
// clang and MSVC can, so every other build still checks this.
#if defined(__clang__) || !defined(__GNUC__) || __GNUC__ >= 12
static_assert(most_shown(kDisplay) <= static_cast<size_t>(kDirtySlot), "no machine shows more Display rows than a tab has slots");
#endif

constinit const RowList kSoundRows = rows_of(kSound), kDisplayRows{kDisplay, static_cast<int>(std::size(kDisplay))}, kPerformanceRows = rows_of(kPerformance),
                        kGraphicsRows = rows_of(kGraphics), kPictureRows = rows_of(kPicture), kMouseRows = rows_of(kMouse),
                        kControllerRows = rows_of(kController), kGameplayRows = rows_of(kGameplay),
                        kInventoryRows = rows_of(kInventory);

void refresh_audio_devices() {
    g_devices.clear();
    for (const audio::OutputDevice& d : audio::list_output_devices())
        if (!d.name.empty()) g_devices.push_back(d.name);
}

}  // namespace nxrt::games::pkp::options_detail
