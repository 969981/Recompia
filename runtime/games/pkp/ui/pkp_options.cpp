// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options: the PC settings of the F1 page, as tabs and rows of the game's own Settings screen. [pkp] options_menu, on by default.
// Nothing reaches the save file: the rows read and write the Settings behind controls.ini, the same store as the F1 page.
//
// How the game's Options screen works (main 2.0.1a, static reading):
//  * Rows come from a data table of 51 records of 24 bytes (ids 0x25..0x32 are empty). A record names its tab, type, value count and text.
//  * The screen's value vector (ptr +0xa8, count +0xb0, 12-byte {id, cur, saved}) has one element per id and is filled when the screen opens.
//  * 0x7100e81b3c (player, id, mode, item) reads (mode 1) or applies (2, 3) the value at item + 0x358; unknown ids do nothing.
//  * 0x710100fce0 (widget, value count - 1, id) names the values from a table that ends at id 0x24: a new choice row crashes without a hook.
//  * Text ids above 800 are skipped by the game's guards; text 0xa8 is a bare "show this string" entry in every language.
//
// 14 free ids for about 60 rows, so the ids are slots: only one tab shows at a time, and a tab change rewrites the slot records and swaps
// the slot values in the vector. g.cur / g.orig hold every tab's values; they reach Settings only when the screen commits (0x10092a0).
// Long lists and decimals are sliders whose numeric text is replaced by the row's own (hook after 0x710100f0a0's text call).
// Hooks are the sites listed in recomp/games/pkp/main.sites.
#include "ui/pkp_options.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/dispatch.h"
#include "nxrt/extras.h"
#include "nxrt/host_device.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/platform.h"
#include "nxrt/services_api.h"
#include "play_game.h"
#include "play_types.h"
#include "pkp_time.h"

namespace nxrt::games::pkp {
namespace {

using S = play::Settings;

constexpr const char* kTag = "pkp";
constexpr uint64_t kOpen = 0x1006628;         // Options screen, open (vtable +0x38)
constexpr uint64_t kOption = 0xe81b3c;        // option get/set
constexpr uint64_t kValueNames = 0x100fce0;   // choice widget: value names
constexpr uint64_t kRowDrawn = 0x100b294;     // inline, after the row's draw called its value widget (x19 = the row)
constexpr uint64_t kSliderText = 0x100f1b4;   // inline, in the slider's update after its number was drawn (x19 = the row, w20 = the value)
constexpr uint64_t kTextById = 0xeba100;      // (widget, slot, text id): sets the description
constexpr uint64_t kStore = 0x100b920;        // (screen, id, value, flag): a row's value changed
constexpr uint64_t kCommit = 0x10092a0;       // (screen, commit): commit or revert the value vector
constexpr uint64_t kTabPrev = 0x100de9c, kTabNext = 0x100e084;   // the screen's tab change
constexpr uint64_t kRebuildList = 0x10070e0;  // (screen) row list for the current tab
constexpr uint64_t kRedraw = 0x1007380;       // (screen, 1, 1, 0) as the game's tab change calls it
constexpr uint64_t kCursor = 0x1008728;       // (screen, 1) as the game's tab change calls it
constexpr uint64_t kListCap = 0x1012a60;      // inline at the bl that sizes the tab list (w1 = capacity)
constexpr uint64_t kBarList = 0x10132a4;      // inline in the tab bar build: the three tabs are in the list (x0/x22 = list, x19 = parent, x21 = bar, w24 = mode)
constexpr uint64_t kBarState = 0x10132d0;     // inline at the bl that picks the bar's layout for its tab count (x21 = bar, x9 = the table, w1 = the entry)
constexpr uint64_t kBarIcons = 0x101334c;     // inline before the bar's icons are set (x21 = bar)
constexpr uint64_t kClockText = 0xead978;     // (hour, minute, out): the time of day as text, in the layout of the game language (out holds 0x1000 bytes)
constexpr uint64_t kBarTitle = 0xeb1b40;      // (bar, title byte): the title under the bar
using layout::kChildOf;
constexpr uint64_t kSetSprite = 0xa7c05c;     // (pane holder, slot, sprite, 0, 0)
using layout::kTables;
constexpr uint64_t kOptionTable = 0x10 + 0x828, kRowId = 0x420, kItemValue = 0x358;
constexpr uint64_t kVecPtr = 0xa8, kVecCount = 0xb0, kMode = 0x358, kCategory = 0x360, kGroup = 0x361, kTabIndex = 0x364, kDirty = 0x383;
constexpr uint32_t kCustomText = 801;         // past the game's guards (1..800): text id 801 + slot is that slot's description
constexpr uint32_t kFirstValueSlot = 7;       // text slots of the value names
constexpr uint32_t kSliderSlot = 0x14, kLabelSlot = 4, kTitleSlot = 0x19;
using layout::kLayoutOf;
constexpr uint64_t kPaneIndex = 0xa72644;     // the slot to pane lookup (with layout::kLayoutOf)
constexpr uint32_t kPaneWidth = 0xf0;         // int16, text pane
constexpr int16_t kSliderTextWidth = 420;     // room for the longest value name above the slider
constexpr uint8_t kFirstId = 0x25;
constexpr int kSlots = 14;
constexpr int kDirtySlot = kSlots - 1;        // never shown: its saved value differs from its cur while another tab holds changes (the game's own dirty scan sees only the vector)
constexpr uint8_t kNoGroup = 0xee;            // a slot not used by the current tab
constexpr uint8_t kFirstOrder = 40;           // after the game's own rows of the tab
constexpr uint8_t kCellClass = 0x49;
constexpr uint32_t kFirstCell = 0x62b;        // the bar's cell widgets, 16 of them
// ponytail: every new tab shows the Game tab's icon until there is art; add a sprite pair per tab when it exists.
constexpr uint32_t kIconOn = 0x88, kIconOff = 0x87;

// ---- rows ------------------------------------------------------------------------------------------------------------------------

struct Row {
    const char* label;
    const char* desc;
    const char* names[4];                 // a choice row: its values
    uint8_t choices;                      // 2..4; 0 = a slider
    uint8_t lo, hi;                       // a slider's range (positions, step 1)
    const char* fmt;                      // a slider's text: fmt of base + position * step ...
    double base, step;
    const char* const* list;              // ... or list[position - lo]
    const char* zero;                     // ... and this at position lo, when set
    const char* (*text)(int, char*, size_t);   // ... or this
    int (*max)();                         // a slider whose last position is known at run time
    int (*get)(const S&);
    void (*set)(S&, int);
    bool restart, live;
};

// clang-format off
#define ROW_BOOL(p) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = v != 0; }
#define ROW_ENUM(p, T) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = static_cast<T>(v); }
#define ROW_PCT(p) [](const S& s) { return static_cast<int>(std::lround(s.p * 100.0)); }, [](S& s, int v) { s.p = v / 100.0; }
#define ROW_INT(p) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = v; }
#define ROW_STEP(p, base, step) [](const S& s) { return static_cast<int>(std::lround((s.p - (base)) / (step))); }, [](S& s, int v) { s.p = (base) + v * (step); }

constexpr Row choice(const char* label, const char* desc, const char* const (&n)[4], uint8_t count, int (*get)(const S&), void (*set)(S&, int), bool restart = false) {
    return Row{label, desc, {n[0], n[1], n[2], n[3]}, count, 0, 0, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr, get, set, restart, false};
}
constexpr Row number(const char* label, const char* desc, uint8_t lo, uint8_t hi, const char* fmt, double base, double step, int (*get)(const S&), void (*set)(S&, int), bool live = false) {
    return Row{label, desc, {}, 0, lo, hi, fmt, base, step, nullptr, nullptr, nullptr, nullptr, get, set, false, live};
}
constexpr Row autonum(const char* label, const char* desc, uint8_t hi, const char* fmt, double base, double step, int (*get)(const S&), void (*set)(S&, int), bool restart = false) {
    return Row{label, desc, {}, 0, 0, hi, fmt, base, step, nullptr, "Auto", nullptr, nullptr, get, set, restart, false};
}
constexpr Row names(const char* label, const char* desc, const char* const* list, uint8_t count, int (*get)(const S&), void (*set)(S&, int), bool restart = false) {
    return Row{label, desc, {}, 0, 0, static_cast<uint8_t>(count - 1), nullptr, 0, 0, list, nullptr, nullptr, nullptr, get, set, restart, false};
}
constexpr Row dynamic(const char* label, const char* desc, const char* (*text)(int, char*, size_t), int (*max)(), int (*get)(const S&), void (*set)(S&, int), bool restart = false) {
    return Row{label, desc, {}, 0, 0, 0, nullptr, 0, 0, nullptr, nullptr, text, max, get, set, restart, false};
}

int index_of(const int* values, int n, int v, int otherwise) {
    for (int i = 0; i < n; ++i) if (values[i] == v) return i;
    return otherwise;
}

constexpr const char* kOffOn[4] = {"Off", "On"};

constexpr int kFpsValues[] = {-1, 30, 40, 45, 60, 90, 120, 144, 165, 240, 0};
constexpr int kFpsCount = static_cast<int>(std::size(kFpsValues));
constexpr const char* kFpsNames[kFpsCount + 1] = {"Match Display", "30", "40", "45", "60", "90", "120", "144", "165", "240", "Unlimited", "Custom"};
constexpr int kWidths[] = {1280, 1600, 1920, 2560, 3840}, kHeights[] = {720, 900, 1080, 1440, 2160};
constexpr int kSizeCount = static_cast<int>(std::size(kWidths));
constexpr const char* kSizeNames[kSizeCount + 1] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Custom"};
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

// Frame generation needs AMD's DLL: without it the row has no "On" and says what is missing.
const char* framegen_text(int v, char*, size_t) {
    if (v > 0) return "On";
    return extras::framegen_dll_found() ? "Off" : "Off (needs amd_fidelityfx_framegeneration_vk.dll)";
}
int framegen_max() { return extras::framegen_dll_found() ? 1 : 0; }

const char* monitor_text(int v, char* out, size_t n) {
    if (v <= 0) return "Automatic";
    if (!platform::display_name(v, out, static_cast<int>(n))) std::snprintf(out, n, "Display %d", v);
    return out;
}
int monitor_max() { return std::max(platform::display_count(), 1); }

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

const Row kDisplay[] = {
    choice("Window Mode", "Windowed, borderless fullscreen or exclusive fullscreen.", {"Windowed", "Borderless", "Fullscreen"}, 3,
           [](const S& s) { return key_index(kWindowKeys, 3, s.display.window_mode); }, [](S& s, int v) { s.display.window_mode = kWindowKeys[v]; }),
    dynamic("Monitor", "The display the window moves to. Fullscreen modes use this display.", monitor_text, monitor_max, ROW_INT(display.monitor)),
    names("Window Size", "The size of the window in windowed mode.", kSizeNames, kSizeCount + 1,
          [](const S& s) { for (int i = 0; i < kSizeCount; ++i) if (s.display.width == kWidths[i] && s.display.height == kHeights[i]) return i; return kSizeCount; },
          [](S& s, int v) { if (v < kSizeCount) { s.display.width = kWidths[v]; s.display.height = kHeights[v]; } }),
    choice("Keep 16:9 Picture", "Letterbox the picture instead of stretching it to the window.", kOffOn, 2, ROW_BOOL(display.keep_aspect)),
    choice("VSync", "Wait for the display between frames: no tearing, a little more delay.", kOffOn, 2, ROW_BOOL(display.vsync)),
    names("Frame Rate Limit", "The highest frame rate. Match Display follows the refresh rate.", kFpsNames, kFpsCount + 1,
          [](const S& s) { return index_of(kFpsValues, kFpsCount, s.display.fps_cap, kFpsCount); },
          [](S& s, int v) { if (v < kFpsCount) { s.display.fps_cap = kFpsValues[v]; play::quality_preset_edited(s.display, play::QualityField::FrameLimit); } }),
    choice("Even Frame Pacing", "With VSync, run at an even divisor of the refresh rate (Steam Deck: 30 instead of 40).", kOffOn, 2,
           [](const S& s) { return static_cast<int>(s.display.fps_snap == "true" || (s.display.fps_snap == "auto" && nxrt::host_is_steam_deck())); },
           [](S& s, int v) { s.display.fps_snap = v ? "true" : "false"; }),
    dynamic("Frame Generation", "AMD FSR 3.1: an extra frame between the game's, from the 3D picture only (not the HUD). Needs amd_fidelityfx_framegeneration_vk.dll "
            "in upscalers\\fsr3, and the picture of the window's shape.", framegen_text, framegen_max, ROW_BOOL(extras.framegen.enabled), true),
    autonum("Render Scale", "The game's resolution; 100% is 1920 x 1080. Auto follows the window.", 76, "%.0f%%", 20, 5,
            [](const S& s) { return s.display.render_scale <= 0 ? 0 : std::clamp((s.display.render_scale - 20) / 5, 1, 76); },
            [](S& s, int v) { s.display.render_scale = v <= 0 ? 0 : 20 + v * 5; play::quality_preset_edited(s.display, play::QualityField::RenderScale); }, true),
};

const Row kPerformance[] = {
    choice("Upscaler", "How the picture is enlarged from a lower resolution.", {"Off", "FSR 1", "Nearest"}, 3,
           [](const S& s) { for (int i = 0; i < 3; ++i) if (s.extras.upscale.mode == kUpscalers[i]) return i; return 0; },
           [](S& s, int v) { s.extras.upscale.mode = kUpscalers[v]; }),
    number("Sharpness", "How much FSR 1 sharpens the picture.", 0, 20, "%.2f", 0, 0.05, ROW_STEP(extras.upscale.sharpness, 0.0, 0.05)),
    number("Scene Resolution", "Renders the 3D scene smaller and keeps the menus sharp. 100% turns it off.", 25, 100, "%.0f%%", 0, 1, ROW_INT(extras.upscale.scene_scale)),
    names("Anti-Aliasing", "Smooths jagged edges of the 3D scene. The menus are not filtered.", kAaNames, 5,
          [](const S& s) { return static_cast<int>(s.extras.upscale.aa); },
          [](S& s, int v) { s.extras.upscale.aa = static_cast<extras::AaMode>(v); play::quality_preset_edited(s.display, play::QualityField::AntiAliasing); }),
    choice("Low Latency", "Reduces input delay on GPUs that support it.", {"Off", "Auto", "On", "Boost"}, 4, ROW_ENUM(extras.latency.mode, extras::LatencyMode), true),
    names("Frames Ahead", "How many frames may wait for the screen. Fewer is more responsive.", kAheadNames, 5,
          [](const S& s) { return s.extras.latency.frames_ahead + 1; }, [](S& s, int v) { s.extras.latency.frames_ahead = v - 1; }, true),
    choice("Frame Rate Counter", "Show the frame rate on screen.", kOffOn, 2, ROW_BOOL(fps.show)),
    choice("Counter Corner", "Where the frame rate counter sits.", {"Top Left", "Top Right", "Bottom Left", "Bottom Right"}, 4, ROW_ENUM(fps.corner, play::Corner)),
    choice("Counter Detail", "What the frame rate counter shows.", {"FPS", "FPS and Time", "Full"}, 3, ROW_INT(fps.detail)),
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
        [](const S& s) { extras::PictureSettings v; extras::picture_preset("vivid", &v); const auto& p = s.extras.picture; if (p.neutral()) return 0; return p.contrast == v.contrast && p.saturation == v.saturation && p.black == v.black && p.gamma == v.gamma ? 1 : 2; },
        [](S& s, int v) { if (v < 2) extras::picture_preset(v ? "vivid" : "neutral", &s.extras.picture); }, false, true},
    number("Contrast", "Spreads the tones away from mid grey. 1.00 is the game's own.", 0, 100, "%.2f", 0.5, 0.01, ROW_STEP(extras.picture.contrast, 0.5, 0.01), true),
    number("Saturation", "Color intensity. 1.00 is the game's own.", 0, 100, "%.2f", 0, 0.02, ROW_STEP(extras.picture.saturation, 0.0, 0.02), true),
    number("Black Level", "Below 0 deepens the blacks, above 0 lifts them.", 0, 150, "%+.3f", -0.5, 0.005, ROW_STEP(extras.picture.black, -0.5, 0.005), true),
    number("Gamma", "Mid-tone brightness. 1.00 is the game's own.", 0, 150, "%.2f", 0.5, 0.01, ROW_STEP(extras.picture.gamma, 0.5, 0.01), true),
};

// The Mouse mode key (the Joy-Con 2 mouse mode, pkp_play.cpp): its binding in play, any key, mouse button or wheel notch. The row shows the
// key; moving it either way starts a capture (play mode's rebind capture, GameHost::begin_capture) and the next input becomes the key, saved
// at once (Escape cancels). The slider sits in the middle of a wide range, so every left/right press or click moves it and starts a capture
// (with 0..1 it stuck at the end after one change and the next press did nothing). The new key replaces any other play binding of that input; an all-context binding of it still works elsewhere.
play::GameHost* g_host = nullptr;
bool g_mm_capture = false;     // waiting for the key
bool g_mm_redraw = false;      // the row's text changed outside a value change
std::string g_mm_shown;        // the key just picked, until play::apply_settings reaches current_settings (next frame)
constexpr int kMmMid = 100;

std::string mouse_mode_key_name(const S& s) {
    std::string out;
    for (const play::Binding& b : s.bindings)
        if (b.act == play::Act::MouseMode && (b.ctx == play::Ctx::Play || b.ctx == play::Ctx::All))
            out += (out.empty() ? "" : ", ") + play::input_name(b.in);
    return out.empty() ? "None" : out;
}
const char* mouse_mode_text(int, char* out, size_t n) {
    if (g_mm_capture) return "Press the new key (Esc: cancel)";
    std::snprintf(out, n, "%s", g_mm_shown.empty() ? mouse_mode_key_name(play::current_settings()).c_str() : g_mm_shown.c_str());
    return out;
}
[[maybe_unused]] int mouse_mode_max() { return 2 * kMmMid; }   // the parked Mouse Mode Key row
void set_mouse_mode_key(S& s, const play::InputId& in) {
    std::erase_if(s.bindings, [&](const play::Binding& b) { return b.act == play::Act::MouseMode || (b.in == in && b.ctx == play::Ctx::Play); });
    s.bindings.push_back(play::Binding{in, play::Ctx::Play, play::Act::MouseMode, play::Trig::Press, 0});
}

#define SENS(p) [](const S& s) { return static_cast<int>(std::lround(play::source_sens(s.mouse.p) * 10.0)); }, [](S& s, int v) { s.mouse.p = play::rad_per_count(v / 10.0); }
const Row kMouse[] = {
    number("Horizontal Sensitivity", "How fast the mouse turns the camera sideways.", 1, 200, "%.1f", 0, 0.1, SENS(yaw_rad)),
    number("Vertical Sensitivity", "How fast the mouse tilts the camera.", 1, 200, "%.1f", 0, 0.1, SENS(pitch_rad)),
    choice("Invert Vertical", "Moving the mouse up looks down.", kOffOn, 2, ROW_BOOL(mouse.invert_y)),
    choice("Invert Horizontal", "Moving the mouse left looks right.", kOffOn, 2, ROW_BOOL(mouse.invert_x)),
    choice("Mouse Pointer in Menus", "Use the mouse as a pointer: hover, click, right click, wheel.", kOffOn, 2, ROW_BOOL(mouse.menu_cursor)),
    names("Button Icons", "The button icons in prompts. Automatic follows the last device used.", kPromptNames, static_cast<uint8_t>(std::size(kPromptNames)), ROW_ENUM(prompts.style, play::PromptStyle)),
    // parked with the Joy-Con 2 mouse mode (pkp_mouse_mode.cpp kMouseModeParked): put the row back when the mode returns
#if 0
    dynamic("Mouse Mode Key", "Turns the Joy-Con 2 mouse mode on and off (point and click to place and break). To change it, press left or right on this row (or click it), then press the new key or mouse button. Esc cancels.",
            mouse_mode_text, mouse_mode_max, [](const S&) { return kMmMid; }, [](S&, int) {}),
#endif
};

const Row kController[] = {
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
// ---- Time and Events tabs ------------------------------------------------------------------------------------------------------
// Time: the game clock moved to a date and time (nxrt::services::timetravel, time.cpp). The date rows show the game's clock; "Time Travel"
// On sets the game to them when the screen commits, Off puts the clock back on the real time. Nothing reaches Settings or the save file.
// Events: one button row per line of [pkp] events_file (pokopia_events.tsv, read every time the screen opens); pressing one sets the game
// clock to noon on the day the event starts, at once, fills the Time tab with it and marks that row "Chosen" (only one at a time).
constexpr int kMaxEvents = 13;                 // a tab has kDirtySlot rows
constexpr int kEventHour = 12;                 // the hour of the day the clock is set to

struct Shown { int v[6]; };                    // year, month, day, hour, minute, second of the game's clock now
Shown shown_clock() {
    Shown s{};
    namespace tt = services::timetravel;
    if (!tt::local_calendar(tt::real_now() + tt::offset(), s.v)) s.v[0] = kFirstYear, s.v[1] = s.v[2] = 1;
    return s;
}
#define TIME_ROW(k, off, hi) [](const S&) { return clamp_pos(shown_clock().v[k] - (off), (hi) + 1); }, [](S&, int) {}

constexpr const char* kClockWords[3] = {"auto", "12h", "24h"};
constexpr const char* kClockNames[4] = {"Game", "12 hour", "24 hour"};
int clock_format_index(const std::string& f) { return f == "12h" ? 1 : f == "24h" ? 2 : 0; }

const Row kTime[] = {
    choice("Time Travel", "Off: the game follows the real date and time. On: the game is set to the date and time below when you leave this screen.",
           kOffOn, 2, [](const S&) { return services::timetravel::offset() != 0 ? 1 : 0; }, [](S&, int) {}),
    number("Year", "The year the game clock is set to. Time Travel must be On.", 0, kYearCount - 1, "%.0f", kFirstYear, 1, TIME_ROW(0, kFirstYear, kYearCount - 1)),
    number("Month", "The month the game clock is set to.", 0, 11, "%.0f", 1, 1, TIME_ROW(1, 1, 11)),
    number("Day", "The day of the month (a day past the end of the month becomes its last day).", 0, 30, "%.0f", 1, 1, TIME_ROW(2, 1, 30)),
    number("Hour", "The hour of the day, 0 to 23.", 0, 23, "%.0f", 0, 1, TIME_ROW(3, 0, 23)),
    number("Minute", "The minute.", 0, 59, "%.0f", 0, 1, TIME_ROW(4, 0, 59)),
    choice("Clock Notation", "How the game shows the time of day. Game: its language decides (English 12 hours with AM/PM, German 24 hours). A screen that is already open shows the new notation after you close it and open it again.", kClockNames, 3,
           [](const S& s) { return clock_format_index(s.clock_format); }, [](S& s, int v) { s.clock_format = kClockWords[std::clamp(v, 0, 2)]; }),
};

std::vector<TimeEvent> g_events;               // what the Events tab shows, in row order (the labels point into it)
std::string g_event_dates[kMaxEvents];         // each row's description
std::string g_none_desc;

// An event row is a two-state button: "Chosen" while the game clock is on the day the event starts (Time Travel on), else "Choose".
template <int I> int event_chosen(const S&) {
    namespace tt = services::timetravel;
    if (I >= static_cast<int>(g_events.size()) || tt::offset() == 0) return 0;
    const Shown c = shown_clock();
    return c.v[0] * 10000 + c.v[1] * 100 + c.v[2] == g_events[I].start ? 1 : 0;
}
template <int... I> constexpr std::array<int (*)(const S&), sizeof...(I)> event_getters(std::integer_sequence<int, I...>) {
    return {&event_chosen<I>...};
}
constexpr auto kEventChosen = event_getters(std::make_integer_sequence<int, kMaxEvents>());
constexpr const char* kChoose[4] = {"Choose", "Chosen"};
Row g_event_rows[kMaxEvents];

// clang-format on

struct Tab {
    const char* title;      // nullptr: the game's own title
    uint8_t group;          // record +0x12
    const Row* rows;
    int count;
};

template <size_t N>
constexpr Tab tab(const char* title, uint8_t group, const Row (&r)[N]) {
    static_assert(N <= kDirtySlot, "a tab has one slot per row and one slot is the dirty marker");
    return Tab{title, group, r, static_cast<int>(N)};
}

Tab kTabs[] = {
    {nullptr, 1, nullptr, 0},   // Game
    {nullptr, 2, nullptr, 0},   // Camera
    tab(nullptr, 3, kSound),
    tab("Display", 4, kDisplay),
    tab("Performance", 5, kPerformance),
    tab("Graphics", 6, kGraphics),
    tab("Picture", 7, kPicture),
    tab("Mouse", 8, kMouse),
    tab("Controller", 9, kController),
    tab("Time", 10, kTime),
    {"Events", 11, g_event_rows, 1},   // the count follows the event file (load_events)
};
constexpr int kTabCount = static_cast<int>(std::size(kTabs));
constexpr int kGameTabs = 3;
constexpr int kTimeTab = kTabCount - 2, kEventsTab = kTabCount - 1;

// ---- state -------------------------------------------------------------------------------------------------------------------------

struct State {
    uint64_t base = 0;
    uint64_t child = 0, parent = 0, bar = 0;
    bool active = false, bar_extended = false;
    int tab = 0;
    int cur[kTabCount][kSlots] = {}, orig[kTabCount][kSlots] = {};
    uint8_t* rec[kSlots] = {};
} g;

using guest::rd;
using guest::wr;
using guest::call;

// The string is copied below the guest stack pointer: the game reads it as guest memory.
void set_text(::Ctx* c, uint64_t widget, uint32_t slot, const char* s) { text::set_string(c, g.base, widget, slot, s); }

int row_slot(uint32_t id) { return id >= kFirstId && id < kFirstId + kDirtySlot ? static_cast<int>(id - kFirstId) : -1; }
const Row* row_at(int tab, int slot) { return slot >= 0 && slot < kTabs[tab].count ? &kTabs[tab].rows[slot] : nullptr; }
const Row* shown_row(uint32_t id) { return g.active ? row_at(g.tab, row_slot(id)) : nullptr; }

int row_hi(const Row& r) { return r.choices ? r.choices - 1 : r.max ? r.max() : r.hi; }

const char* value_text(const Row& r, int v, char* out, size_t n) {
    if (r.text) return r.text(v, out, n);
    if (r.list) return r.list[std::clamp(v - r.lo, 0, r.hi - r.lo)];
    if (r.zero && v == r.lo) return r.zero;
    std::snprintf(out, n, r.fmt, r.base + v * r.step);
    return out;
}

bool has_changes(int skip_tab) {
    for (int t = 0; t < kTabCount; ++t)
        if (t != skip_tab)
            for (int s = 0; s < kTabs[t].count; ++s)
                if (g.cur[t][s] != g.orig[t][s]) return true;
    return false;
}

void write_records(int tab) {
    static const S defaults = play::default_settings();
    for (int slot = 0; slot < kSlots; ++slot) {
        uint8_t* r = g.rec[slot];
        if (!r) continue;
        const Row* row = slot == kDirtySlot ? nullptr : row_at(tab, slot);
        const uint16_t text = static_cast<uint16_t>(kCustomText + slot);
        std::memcpy(r, &text, 2);
        std::memcpy(r + 2, &text, 2);
        std::memset(r + 8, 1, 10);                   // shown in every screen mode
        r[0x13] = 2;                                 // general options: a slot stays in the value vector whatever the tab
        r[0x16] = 3;                                 // as every other row
        r[0x15] = static_cast<uint8_t>(kFirstOrder + slot);   // a nonzero order is what makes the row exist
        r[0x12] = row ? kTabs[tab].group : kNoGroup;
        r[4] = row && !row->choices ? 2 : 1;
        r[6] = 1;                                    // the dirty scan skips rows with 0 here
        r[5] = r[7] = 0;
        if (!row) continue;
        r[5] = static_cast<uint8_t>(std::clamp(row->get(defaults), static_cast<int>(row->lo), row_hi(*row)));
        r[6] = static_cast<uint8_t>(row->choices ? row->choices - 1 : row_hi(*row));
        r[7] = row->lo;
    }
}

template <class F> void each_slot_element(F&& f) {
    const uint64_t ptr = rd<uint64_t>(g.child + kVecPtr), n = rd<uint64_t>(g.child + kVecCount);
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t e = ptr + i * 12;
        const uint8_t id = rd<uint8_t>(e);
        if (id >= kFirstId && id < kFirstId + kSlots) f(id - kFirstId, e);
    }
}

void vector_to_store() {
    each_slot_element([](int slot, uint64_t e) { if (slot != kDirtySlot) g.cur[g.tab][slot] = rd<int>(e + 4); });
}

void store_to_vector() {
    const bool other_tabs = has_changes(g.tab);
    each_slot_element([other_tabs](int slot, uint64_t e) {
        wr<int>(e + 4, slot == kDirtySlot ? 0 : g.cur[g.tab][slot]);
        wr<int>(e + 8, slot == kDirtySlot ? (other_tabs ? 1 : 0) : g.orig[g.tab][slot]);
    });
    if (other_tabs) wr<uint8_t>(g.child + kDirty, 1);
}

bool build_settings(S& s) {
    bool changed = false;
    for (int t = 0; t < kTabCount; ++t)
        for (int i = 0; i < kTabs[t].count; ++i)
            if (g.cur[t][i] != g.orig[t][i]) {
                kTabs[t].rows[i].set(s, g.cur[t][i]);
                changed = true;
            }
    return changed;
}

// ---- Time and Events: loading and applying -------------------------------------------------------------------------------------------

int real_today() {
    int v[6] = {kFirstYear, 1, 1, 0, 0, 0};
    services::timetravel::local_calendar(services::timetravel::real_now(), v);
    return v[0] * 10000 + v[1] * 100 + v[2];
}

// Reads the event file again (the launcher or a script may have changed it) and fills the Events tab's rows.
void load_events() {
    std::stringstream text;
    std::ifstream file(settings::get_str(settings::Key::EventsFile));
    if (file) text << file.rdbuf();
    const int today = real_today();
    g_events = parse_events(text.str(), today, kMaxEvents);
    for (size_t i = 0; i < g_events.size(); ++i) {
        g_event_dates[i] = event_dates(g_events[i], today) + ". Press to set the game clock to this day, at noon.";
        g_event_rows[i] = Row{g_events[i].name.c_str(), g_event_dates[i].c_str(), {kChoose[0], kChoose[1]}, 2, 0, 0, nullptr, 0, 0, nullptr, nullptr, nullptr,
                              nullptr, kEventChosen[i], [](S&, int) {}, false, false};
    }
    if (g_events.empty()) {
        g_none_desc = std::string("No events in ") + settings::get_str(settings::Key::EventsFile) +
                      ". The file has one event per line: start date, end date and name, separated by tabs (2026-11-16 2026-12-01 Name).";
        g_event_rows[0] = Row{"No events found", g_none_desc.c_str(), {kChoose[0], kChoose[1]}, 2, 0, 0, nullptr, 0, 0, nullptr, nullptr, nullptr,
                              nullptr, [](const S&) { return 0; }, [](S&, int) {}, false, false};
    }
    kTabs[kEventsTab].count = g_events.empty() ? 1 : static_cast<int>(g_events.size());
    NXLOG_I(kTag, "options: %zu events from %s", g_events.size(), settings::get_str(settings::Key::EventsFile).c_str());
}

// The Time tab's rows, as the game clock: Travel Off puts the real time back, On sets the date and time of the rows.
void apply_time_tab() {
    namespace tt = services::timetravel;
    const int* c = g.cur[kTimeTab];
    if (c[0] == 0) {
        tt::set_offset(0);
        return;
    }
    const int year = year_of(c[1]), month = 1 + c[2];
    int64_t at = 0;
    if (!tt::posix_of_local(year, month, std::min(1 + c[3], days_in_month(year, month)), c[4], c[5], &at)) {
        NXLOG_W(kTag, "options: %04d-%02d-%02d %02d:%02d does not exist in this time zone: the clock stays as it is", year, month, 1 + c[3], c[4], c[5]);
        return;
    }
    tt::set_offset(at - tt::real_now());
}

// An Events row was pressed: the game clock goes to noon on the event's first day now, and the Time tab shows it.
void go_to_event(int row) {
    namespace tt = services::timetravel;
    if (row < 0 || row >= static_cast<int>(g_events.size())) return;
    const int start = g_events[row].start, year = start / 10000, month = start / 100 % 100, day = start % 100;
    int64_t at = 0;
    if (!tt::posix_of_local(year, month, day, kEventHour, 0, &at)) {
        NXLOG_W(kTag, "options: %s starts on a date that does not exist in this time zone", g_events[row].name.c_str());
        return;
    }
    tt::set_offset(at - tt::real_now());
    const int rows[6] = {1, clamp_pos(year - kFirstYear, kYearCount), month - 1, day - 1, kEventHour, 0};
    for (int i = 0; i < 6; ++i) g.cur[kTimeTab][i] = g.orig[kTimeTab][i] = rows[i];
    NXLOG_I(kTag, "options: game clock set to %04d-%02d-%02d %02d:00 (%s)", year, month, day, kEventHour, g_events[row].name.c_str());
}

// ---- hooks -------------------------------------------------------------------------------------------------------------------------

bool find_records(::Ctx* c) {
    const uint64_t mgr = rd<uint64_t>(g.base + kTables);
    uint64_t t = mgr ? rd<uint64_t>(mgr + kOptionTable) : 0;
    if (t) t = call(c, rd<uint64_t>(rd<uint64_t>(t) + 0x38), t);
    if (!t) return false;
    for (int s = 0; s < kSlots; ++s) {
        const uint64_t entry = call(c, rd<uint64_t>(rd<uint64_t>(t) + 0x30), t, kFirstId + s);
        const uint64_t rec = entry ? rd<uint64_t>(entry + 8) : 0;
        if (!rec || !mem::is_mapped(rec, 24)) return false;
        g.rec[s] = reinterpret_cast<uint8_t*>(rec);
    }
    return true;
}

// The game's time-of-day text. [tweaks] clock_format: 12h or 24h write the text here; auto leaves the language's own layout to the game.
int hook_clock_text(::Ctx* c, uint64_t, void*) {
    const std::string& f = play::current_settings().clock_format;
    const int format = clock_format_index(f);
    if (format == 0) return 0;
    const int hour = static_cast<int>(c->x[0] & 0xffffffff) % 24, minute = static_cast<int>(c->x[1] & 0xffffffff) % 60;
    const uint64_t out = c->x[2];
    char text[40];
    if (format == 2) std::snprintf(text, sizeof text, "%02d:%02d", hour, minute);
    else std::snprintf(text, sizeof text, "%d:%02d<nbs2>%s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
    for (size_t i = 0; i <= std::strlen(text); ++i) wr<uint8_t>(out + i, static_cast<uint8_t>(text[i]));
    return 1;
}

int hook_open(::Ctx* c, uint64_t, void*) {
    const uint64_t child = c->x[0];
    if (!g.rec[0] && !find_records(c)) {
        static bool logged = false;
        if (!logged) NXLOG_W(kTag, "options: the option records were not found, the PC rows stay hidden");
        logged = true;
        return 0;
    }
    g.active = rd<uint8_t>(child + kCategory) == 2 && rd<int>(child + kMode) != 9;
    if (!g.active) {                                 // the second player's screen and the online screens keep the game's rows only
        for (uint8_t* r : g.rec) r[0x12] = kNoGroup;
        return 0;
    }
    const bool returning = rd<uint8_t>(child + 0x386) != 0;   // back from Button Assignments: the changes so far stay
    g.child = child;
    g.tab = std::clamp(rd<int>(child + kTabIndex), 0, kTabCount - 1);
    if (g.tab >= kGameTabs) wr<uint8_t>(child + kGroup, kTabs[g.tab].group);   // the game's table only knows its own three tabs
    if (!returning) {
        load_events();
        g_devices.clear();
        for (const audio::OutputDevice& d : audio::list_output_devices())
            if (!d.name.empty()) g_devices.push_back(d.name);
        const S s = play::current_settings();
        for (int t = 0; t < kTabCount; ++t)
            for (int i = 0; i < kTabs[t].count; ++i)
                g.cur[t][i] = g.orig[t][i] = std::clamp(kTabs[t].rows[i].get(s), static_cast<int>(kTabs[t].rows[i].lo), row_hi(kTabs[t].rows[i]));
    }
    write_records(g.tab);
    return 0;
}

int hook_option(::Ctx* c, uint64_t, void*) {
    const uint32_t id = c->x[1] & 0xff;
    if (!g.active || id < kFirstId || id >= kFirstId + kSlots) return 0;
    if (static_cast<uint32_t>(c->x[2]) == 1) wr<int>(c->x[3] + kItemValue, id == kFirstId + kDirtySlot ? 0 : g.cur[g.tab][id - kFirstId]);
    return 1;                                        // mode 2 and 3: hook_commit applies every tab at once
}

int hook_commit(::Ctx* c, uint64_t, void*) {
    if (!g.active || c->x[0] != g.child) return 0;
    vector_to_store();
    if (c->x[1] & 1) {
        if (std::memcmp(g.cur[kTimeTab], g.orig[kTimeTab], 6 * sizeof(int)) != 0) apply_time_tab();
        S s = play::current_settings();
        if (build_settings(s)) play::apply_settings(s);   // main thread, next frame; also writes controls.ini
        std::memcpy(g.orig, g.cur, sizeof g.orig);
    } else if (rd<int>(g.child + 0x354) != 0) {      // "discard" (the game's other call with 0 only redraws)
        std::memcpy(g.cur, g.orig, sizeof g.cur);
        extras::set_picture_live(play::current_settings().extras.picture);   // the colour rows preview live
    }
    store_to_vector();
    return 0;                                        // the game commits its own rows
}

int hook_store(::Ctx* c, uint64_t, void*) {
    const Row* r = c->x[0] == g.child ? shown_row(c->x[1] & 0xff) : nullptr;
    if (r && g.tab == kEventsTab) {                  // an event was picked: go there now, and keep the row where it was (nothing to save)
        const int slot = static_cast<int>(c->x[1] & 0xff) - kFirstId;
        go_to_event(slot);
        for (int i = 0; i < kSlots; ++i) g.cur[g.tab][i] = g.orig[g.tab][i] = i == slot ? 1 : 0;   // exactly one event is chosen
        each_slot_element([slot](int s, uint64_t e) { wr<int>(e + 4, s == slot ? 1 : 0); wr<int>(e + 8, s == slot ? 1 : 0); });
        return 1;
    }
    if (r && r->text == &mouse_mode_text) {          // the Mouse Mode Key row moved: wait for the new key
        if (g_host && !g_mm_capture) {
            g_host->begin_capture();
            g_mm_capture = true;
            g_mm_redraw = true;
            NXLOG_I(kTag, "options: Mouse Mode Key: waiting for a key");
        }
        return 0;
    }
    if (!r || !r->live) return 0;
    g.cur[g.tab][(c->x[1] & 0xff) - kFirstId] = static_cast<int>(c->x[2]);
    S s = play::current_settings();
    build_settings(s);
    extras::set_picture_live(s.extras.picture);
    return 0;
}

int change_tab(::Ctx* c, int dir) {
    const uint64_t child = c->x[0];
    if (!g.active || child != g.child) return 0;
    if (rd<uint32_t>(child + 0x90) - 1u > 1 || rd<int>(child + 0x350) != 0 || rd<uint8_t>(child + 0x385) != 0) {
        c->x[0] = 0;
        return 1;
    }
    vector_to_store();
    g.tab = (g.tab + dir + kTabCount) % kTabCount;
    wr<int>(child + kTabIndex, g.tab);
    wr<uint8_t>(child + kGroup, kTabs[g.tab].group);
    write_records(g.tab);
    store_to_vector();
    call(c, g.base + kRebuildList, child);
    call(c, g.base + kRedraw, child, 1, 1, 0);
    call(c, g.base + kCursor, child, 1);
    c->x[0] = 1;
    return 1;
}
int hook_tab_prev(::Ctx* c, uint64_t, void*) { return change_tab(c, -1); }
int hook_tab_next(::Ctx* c, uint64_t, void*) { return change_tab(c, 1); }

void hook_list_cap(::Ctx* c, uint64_t, void*) { c->x[1] = kTabCount; }

void hook_bar_list(::Ctx* c, uint64_t, void*) {
    g.bar_extended = static_cast<uint32_t>(c->x[24]) != 9;
    g.parent = c->x[19];
    g.bar = c->x[21];
    wr<int>(c->x[0] + 0x78, g.bar_extended ? kTabCount : kGameTabs);   // the list's columns: L and R only move within a row of the grid
    if (!g.bar_extended) return;
    const uint64_t list = rd<uint64_t>(c->x[0] + 8);
    if (!list) return;
    for (uint64_t i = kGameTabs; i < kTabCount; ++i) {   // the list's {ptr, size, capacity} of {u64 (index << 32 | 0x2f), u32 0}
        const uint64_t size = rd<uint64_t>(list + 8);
        if (size == rd<uint64_t>(list + 0x10)) break;
        const uint64_t e = rd<uint64_t>(list) + size * 12;
        wr<uint64_t>(e, i << 32 | 0x2f);
        wr<uint32_t>(e + 8, 0);
        wr<uint64_t>(list + 8, size + 1);
    }
}

void hook_bar_state(::Ctx* c, uint64_t, void*) {
    if (!g.bar_extended) return;
    wr<uint8_t>(c->x[21] + 0xa4, kTabCount);
    c->x[1] = rd<uint32_t>(c->x[9] + (kTabCount - 1) * 4);   // the game masks this index to its three tabs
}

void hook_bar_icons(::Ctx* c, uint64_t, void*) {
    if (!g.bar_extended) return;
    const uint64_t bar = c->x[21];
    const uint64_t cls = (c->x[31] - 0x20) & ~uint64_t{0xf};
    for (uint32_t i = kGameTabs; i < kTabCount; ++i) {
        wr<uint16_t>(cls, 0);
        const uint64_t cell = call(c, g.base + kChildOf, bar + 0x48, kFirstCell + i, cls, 0, 0, cls - 0x10);
        if (!cell || rd<uint16_t>(cls) != kCellClass) continue;
        call(c, g.base + kSetSprite, cell + 0x30, 5, kIconOn, 0, 0, cls - 0x10);
        call(c, g.base + kSetSprite, cell + 0x30, 6, kIconOff, 0, 0, cls - 0x10);
    }
}

int hook_bar_title(::Ctx* c, uint64_t, void*) {
    if (!g.bar_extended || c->x[0] != g.bar) return 0;
    const int tab = rd<int>(g.parent + 0xac);
    if (tab < kGameTabs || tab >= kTabCount) return 0;
    set_text(c, g.bar, kTitleSlot, kTabs[tab].title);
    return 1;
}

int hook_value_names(::Ctx* c, uint64_t, void*) {
    const Row* r = shown_row(c->x[2] & 0xff);
    if (!r) return 0;
    for (int i = 0; i < r->choices; ++i) set_text(c, c->x[0], kFirstValueSlot + i, r->names[i]);
    return 1;
}

uint64_t text_pane(::Ctx* c, uint64_t widget, uint32_t slot);

void hook_row_drawn(::Ctx* c, uint64_t, void*) {
    const Row* r = shown_row(rd<uint8_t>(c->x[19] + kRowId));
    if (!r) return;
    set_text(c, c->x[19], kLabelSlot, r->label);
    if (r->text == &mouse_mode_text && g_mm_redraw) {   // the capture started or ended: the key text, not the slider's number
        g_mm_redraw = false;
        if (const uint64_t pane = text_pane(c, c->x[19], kSliderSlot)) {
            if (rd<int16_t>(pane + kPaneWidth) < kSliderTextWidth) wr<int16_t>(pane + kPaneWidth, kSliderTextWidth);
        }
        char buf[128];
        set_text(c, c->x[19], kSliderSlot, mouse_mode_text(0, buf, sizeof buf));
    }
}

// The widget's text pane for a slot: the game's own walk (see 0xa7abe0). 0 if there is none.
uint64_t text_pane(::Ctx* c, uint64_t widget, uint32_t slot) {
    const uint64_t handler = rd<uint64_t>(widget + 0x68);
    const uint64_t source = handler ? rd<uint64_t>(handler + 8) : 0;
    const uint64_t layout = source ? call(c, g.base + kLayoutOf, source) : 0;
    if (!layout || rd<uint16_t>(layout + 0x92) <= slot) return 0;
    const uint32_t index = static_cast<uint32_t>(call(c, g.base + kPaneIndex, handler, slot));
    const uint32_t count = static_cast<uint32_t>(call(c, rd<uint64_t>(rd<uint64_t>(layout + 0x10) + 0x20), layout + 0x10));
    uint64_t node = index < count ? rd<uint64_t>(layout + 0x20) : 0;
    for (uint32_t i = 0; node && i < index; ++i) node = rd<uint64_t>(node + 8);
    const uint64_t pane = rd<uint64_t>(node ? node + 0x10 : layout + 0x28);
    return pane && rd<uint8_t>(pane + 0x112) == 2 ? pane : 0;
}

void hook_slider_text(::Ctx* c, uint64_t, void*) {
    const Row* r = shown_row(rd<uint8_t>(c->x[19] + kRowId));
    if (!r || r->choices) return;
    // The box is sized for a short number: "Match Display" came out at half size.
    if (const uint64_t pane = text_pane(c, c->x[19], kSliderSlot)) {
        if (rd<int16_t>(pane + kPaneWidth) < kSliderTextWidth) wr<int16_t>(pane + kPaneWidth, kSliderTextWidth);
    }
    char buf[128];
    set_text(c, c->x[19], kSliderSlot, value_text(*r, static_cast<int>(c->x[20]), buf, sizeof buf));
}

int hook_text_by_id(::Ctx* c, uint64_t, void*) {
    const uint32_t id = c->x[2] & 0xffff;
    if (id < kCustomText || id >= kCustomText + kDirtySlot) return 0;
    const Row* r = g.active ? row_at(g.tab, static_cast<int>(id - kCustomText)) : nullptr;
    if (!r) return 1;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s%s", r->desc, r->restart ? " Applies after a restart." : "");
    set_text(c, c->x[0], static_cast<uint32_t>(c->x[1]), buf);
    return 1;
}

// frame boundary, main thread: the Mouse Mode Key row's key capture
void frame(bool, double, play::GameFrame*) {
    if (!g_mm_shown.empty() && !g_mm_capture && mouse_mode_key_name(play::current_settings()) == g_mm_shown) g_mm_shown.clear();
    if (!g_mm_capture || !g_host) return;
    play::InputId in;
    if (g_host->capture_result(&in)) {
        if (g_host->reserved_key(in)) {              // F1 opens the settings page: keep the old key
            NXLOG_I(kTag, "options: Mouse Mode Key: %s opens the settings page, the key stays", play::input_name(in).c_str());
            g_mm_capture = false;
            g_mm_redraw = true;
            return;
        }
        S s = play::current_settings();
        set_mouse_mode_key(s, in);
        g_mm_shown = play::input_name(in);
        play::apply_settings(s);                     // main thread, next frame; also writes controls.ini
        NXLOG_I(kTag, "options: Mouse Mode Key = %s", play::input_name(in).c_str());
    } else if (g_host->capturing()) {
        if (g.active) return;
        g_host->cancel_capture();                    // the screen closed while waiting
    }
    g_mm_capture = false;
    g_mm_redraw = true;
}

void install(const FeatureInstall& in) {
    in.hooks.add_entry(in.base + kClockText, &hook_clock_text);   // clock_format works without the Settings tabs too
    if (!settings::get_bool(settings::Key::OptionsMenu)) return;
    const uint64_t base = in.base;
    g.base = base;
    g_host = in.host;
    in.hooks.add_entry(base + kOpen, &hook_open);
    in.hooks.add_entry(base + kOption, &hook_option);
    in.hooks.add_entry(base + kValueNames, &hook_value_names);
    in.hooks.add_inline(base + kRowDrawn, &hook_row_drawn);
    in.hooks.add_inline(base + kSliderText, &hook_slider_text);
    in.hooks.add_entry(base + kTextById, &hook_text_by_id);
    in.hooks.add_entry(base + kCommit, &hook_commit);
    in.hooks.add_entry(base + kStore, &hook_store);
    in.hooks.add_entry(base + kTabPrev, &hook_tab_prev);
    in.hooks.add_entry(base + kTabNext, &hook_tab_next);
    in.hooks.add_inline(base + kListCap, &hook_list_cap);
    in.hooks.add_inline(base + kBarList, &hook_bar_list);
    in.hooks.add_inline(base + kBarState, &hook_bar_state);
    in.hooks.add_inline(base + kBarIcons, &hook_bar_icons);
    in.hooks.add_entry(base + kBarTitle, &hook_bar_title);
    NXLOG_I(kTag, "options: PC settings in the game's menu are on (%d tabs)", kTabCount);
}

}  // namespace

const Feature kOptionsFeature = {.name = "options", .install = &install, .frame = &frame};

}  // namespace nxrt::games::pkp
