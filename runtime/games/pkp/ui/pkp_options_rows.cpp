// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the rows of the PC settings tabs (pkp_options_internal.h): each binds to a field of the play-mode Settings behind
// controls.ini, the same store as the F1 page, except the Gameplay and Inventory tabs', which bind to the gameplay mods' [pkp] keys of the game
// ini.
// Nothing reaches the save file.
#include "ui/pkp_options_internal.h"
#include "core/pkp_game.h"
#include "nxrt/game_render.h"

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

// A phone's resolution rows. "Maximum Resolution" is [display] render_scale (30..100 % of 1920x1080, the size of the game's targets: next
// start). "Resolution" moves [upscale] scene_scale live, the game's own dynamic-resolution part of those targets, from the maximum the
// game started with down to the game's lowest correct level (game::Render::scene_scale_min); each step is labelled with the real percent.
int res_ceiling() {
    const float f = gfx::tuning().render_scale.load(std::memory_order_relaxed);
    return f > 0.0f ? std::clamp(static_cast<int>(std::lround(f * 100.0f)), 25, 100) : 100;
}
int res_steps() {   // real percents c, c - 5, ... down to the floor
    const int c = res_ceiling(), floor = (c * render_info().scene_scale_min + 99) / 100;
    return std::max(1, (c - floor) / 5 + 1);
}
int res_real(int pos) { return res_ceiling() - 5 * (res_steps() - 1 - pos); }
const char* res_text(int pos, char* out, size_t n) { std::snprintf(out, n, "%d%%", res_real(pos)); return out; }
int res_max() { return res_steps() - 1; }
int res_get(const S& s) {
    const int c = res_ceiling(), real = c * s.extras.upscale.scene_scale / 100;
    return std::clamp(res_steps() - 1 - static_cast<int>(std::lround((c - real) / 5.0)), 0, res_max());
}
void res_set(S& s, int v) {
    s.extras.upscale.scene_scale = std::clamp(static_cast<int>(std::lround(res_real(v) * 100.0 / res_ceiling())), render_info().scene_scale_min, 100);
}
const char* max_text(int pos, char* out, size_t n) { std::snprintf(out, n, "%d%%", 30 + 5 * pos); return out; }
int max_max() { return 14; }
int max_get(const S& s) { return std::clamp(((s.display.render_scale > 0 ? s.display.render_scale : res_ceiling()) - 30) / 5, 0, 14); }
void max_set(S& s, int v) { s.display.render_scale = 30 + 5 * v; play::quality_preset_edited(s.display, play::QualityField::RenderScale); }
constexpr Row live_row(Row r) { r.live = true; return r; }
// A phone's one Upscaler row: Off, SGSR, Bicubic ([pkp] scene_upscaler, the 3D alone, menus native) or FSR 1 ([upscale] mode, the whole
// picture at the present). Each choice clears the other key. Both apply at once.
int phone_upscaler_get(const S& s) {
    if (s.extras.upscale.mode == extras::UpscaleMode::Fsr1) return 3;
    return static_cast<int>(std::clamp<int64_t>(settings::get_int(settings::Key::SceneUpscaler), 0, 2));
}
void phone_upscaler_set(S& s, int v) {
    s.extras.upscale.mode = v == 3 ? extras::UpscaleMode::Fsr1 : extras::UpscaleMode::Off;
    settings::set_int(settings::Key::SceneUpscaler, v == 3 ? 0 : v);   // the game ini, like a key row
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
constexpr extras::UpscaleMode kUpscalers[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Nearest};

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

const Row kSound[] = {
    number("Master Volume", "The volume of everything in the game, on top of the sliders above.", 0, 100, "%.0f%%", 0, 1, ROW_PCT(audio.volume)),
    choice("Mute in Background", "Silence the game while its window is not in focus.", kOffOn, 2, ROW_BOOL(audio.mute_unfocused)),
    dynamic("Output Device", "The sound device the game plays through.", device_text, device_max,
            [](const S& s) { for (size_t i = 0; i < g_devices.size() && i < 250; ++i) if (g_devices[i] == s.audio.device) return static_cast<int>(i) + 1; return 0; },
            [](S& s, int v) { s.audio.device = v <= 0 || v > static_cast<int>(g_devices.size()) ? "default" : g_devices[static_cast<size_t>(v - 1)]; }, true),
};

// Window rows only where the window system has a choice of modes (a phone has one: nxrt/platform.h window_modes).
bool several_window_modes() { const int m = platform::window_modes(); return (m & (m - 1)) != 0; }

const Row kDisplay[] = {
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
    choice("VSync", "Wait for the display between frames: no tearing, a little more delay.", kOffOn, 2, ROW_BOOL(display.vsync)),
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
    when(dynamic("Maximum Resolution", "The highest resolution of the 3D picture (100% is 1920 x 1080). Takes effect at the next start.", max_text, max_max, max_get, max_set, true),
         nxrt::host_is_phone),
    when(live_row(dynamic("Resolution", "The 3D picture's resolution, from the maximum down; menus and text stay sharp. Changes at once.", res_text, res_max, res_get, res_set)),
         nxrt::host_is_phone),
    when(choice("Upscaler", "Sharpens the enlarged 3D picture: SGSR (Snapdragon Game Super Resolution) or bicubic, menus stay sharp; FSR 1 the whole picture (costs more).",
                {"Off", "SGSR", "Bicubic", "FSR 1"}, 4, phone_upscaler_get, phone_upscaler_set), nxrt::host_is_phone),
};

const Row kPerformance[] = {
    when(choice("Upscaler", "How the picture is enlarged from a lower resolution.", {"Off", "FSR 1", "Nearest"}, 3,
           [](const S& s) { for (int i = 0; i < 3; ++i) if (s.extras.upscale.mode == kUpscalers[i]) return i; return 0; },
           [](S& s, int v) { s.extras.upscale.mode = kUpscalers[v]; }), not_phone),
    number("Sharpness", "How much FSR 1 sharpens the picture.", 0, 20, "%.2f", 0, 0.05, ROW_STEP(extras.upscale.sharpness, 0.0, 0.05)),
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
    choice("Dynamic Resolution", "Let the game lower its resolution when the GPU falls behind.", kOffOn, 2, ROW_BOOL(display.dynamic_resolution)),
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

constinit const RowList kSoundRows = rows_of(kSound), kDisplayRows = rows_of(kDisplay), kPerformanceRows = rows_of(kPerformance),
                        kGraphicsRows = rows_of(kGraphics), kPictureRows = rows_of(kPicture), kMouseRows = rows_of(kMouse),
                        kControllerRows = rows_of(kController), kGameplayRows = rows_of(kGameplay),
                        kInventoryRows = rows_of(kInventory);

void refresh_audio_devices() {
    g_devices.clear();
    for (const audio::OutputDevice& d : audio::list_output_devices())
        if (!d.name.empty()) g_devices.push_back(d.name);
}

}  // namespace nxrt::games::pkp::options_detail
