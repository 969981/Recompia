// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the performance report's settings line (nxrt/perf_report.h): what the player set that changes the frame rate, with the sizes
// the NVN back end presented last. Called on the hitch monitor's thread every summary; it copies the display settings under st.mu and
// reads the rest from atomics (gfx::tuning(), the limiter, the extras' live upscale fields).
#include "play_state.h"

#include <string>

#include "nxrt/gfx_tuning.h"
#include "nxrt/perf_report.h"

namespace nxrt::play {
namespace detail {
namespace {

const char* upscale_name(extras::UpscaleMode m) {
    switch (m) {
        case extras::UpscaleMode::Off: return "off";
        case extras::UpscaleMode::Fsr1: return "fsr1";
        case extras::UpscaleMode::Fsr3: return "fsr3";
        case extras::UpscaleMode::Dlss: return "dlss";
        case extras::UpscaleMode::Xess: return "xess";
        case extras::UpscaleMode::Nis: return "nis";
        case extras::UpscaleMode::Dlaa: return "dlaa";
        case extras::UpscaleMode::Nearest: return "nearest";
        case extras::UpscaleMode::Sgsr2: return "sgsr2";
    }
    return "?";
}

const char* aa_name(extras::AaMode m) {
    switch (m) {
        case extras::AaMode::Off: return "off";
        case extras::AaMode::SmaaLow: return "smaa low";
        case extras::AaMode::SmaaMedium: return "smaa medium";
        case extras::AaMode::SmaaHigh: return "smaa high";
        case extras::AaMode::SmaaUltra: return "smaa ultra";
    }
    return "?";
}

std::string describe() {
    G& st = g();
    DisplaySettings d;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        d = st.settings.display;
    }
    const gfx::Tuning& tu = gfx::tuning();
    std::string render_set;
    if (d.render_width > 0 && d.render_height > 0) render_set = "fixed " + std::to_string(d.render_width) + "x" + std::to_string(d.render_height);
    else if (d.render_scale > 0) render_set = "render_scale " + std::to_string(d.render_scale) + "%";
    else render_set = "auto, render_mode " + d.render_mode;
    std::string cap = d.fps_cap == 0 ? "unlimited" : d.fps_cap < 0 ? "auto (refresh rate)" : std::to_string(d.fps_cap);
    char b[768];
    std::snprintf(b, sizeof b,
                  "window %s, output %dx%d, render %dx%d (%s), scene scale %d%%, dynamic resolution %s, shadow scale %.0fx; frame cap %s (limiter %.2f fps, game "
                  "ceiling %.0f fps), vsync %s, present mode asked %s (the swapchain line says what the driver gave); upscaler %s, aa %s",
                  d.window_mode.c_str(), tu.present_out_w.load(std::memory_order_relaxed), tu.present_out_h.load(std::memory_order_relaxed),
                  tu.present_src_w.load(std::memory_order_relaxed), tu.present_src_h.load(std::memory_order_relaxed), render_set.c_str(),
                  extras::upscale_scene_scale(), d.dynamic_resolution ? "on" : "off", static_cast<double>(tu.shadow_scale.load(std::memory_order_relaxed)),
                  cap.c_str(), st.limiter.target_fps(), st.limiter.max_fps(), d.vsync ? "on" : "off", gfx::present_mode_name(present_mode_for(d)),
                  upscale_name(extras::upscale_mode()), aa_name(extras::aa_mode()));
    return b;
}

}  // namespace

void perf_settings_register() { perf::set_settings_source(&describe); }

}  // namespace detail
}  // namespace nxrt::play
