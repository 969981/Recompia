// SPDX-License-Identifier: GPL-3.0-or-later
// Display options of play mode: see play_display.h.
#include "play_display.h"

#include <algorithm>
#include <cmath>

#include "nxrt/gfx_tuning.h"
#include "nxrt/host_device.h"

namespace nxrt::play {

// ---- arithmetic -----------------------------------------------------------------------------------------------------------------
namespace {

int clamp_dim(double v) {
    const long r = std::lround(v);
    const long even = r & ~1L;
    return static_cast<int>(std::clamp<long>(even, kMinRenderDim, kMaxRenderDim));
}

}  // namespace

void internal_size(const DisplaySettings& d, int window_w, int window_h, int* out_w, int* out_h) {
    double w, h;
    if (d.render_width > 0 && d.render_height > 0) {
        w = d.render_width;
        h = d.render_height;
    } else if (d.render_scale <= 0) {
        // auto: [display] render_mode decides. "console" (the default) renders at the resolution the console would, 1920x1080 docked
        // and 1280x720 handheld, and the present pass upscales to the window, which is what a console does; rendering at the window's
        // size instead was 1.78x the pixels in a 1440p window and measured 42 fps against 60.0 at 1080p in the same world scene on an
        // M3, where the GPU is fill-rate bound. "window" is the behaviour every build had before this key: the render follows the
        // window. render_scale and render_width/render_height override both.
        if (d.render_mode == "window") {
            w = std::max(window_w, 1);
            h = std::max(window_h, 1);
        } else {
            const bool handheld = host_is_handheld();
            w = handheld ? 1280 : 1920;
            h = handheld ? 720 : 1080;
            if (window_w > 0 && window_h > 0) {
                // a smaller window than the console's mode renders at the window's size (no point upscaling a small window)
                const double f = std::min(1.0, std::min(static_cast<double>(window_w) / w, static_cast<double>(window_h) / h));
                w *= f;
                h *= f;
            }
        }
    } else {
        const double s = std::clamp(d.render_scale, 25, 400) / 100.0;
        w = std::max(window_w, 1) * s;
        h = std::max(window_h, 1) * s;
    }
    constexpr double kMaxArea = 8192.0 * 8192.0 * 2.0;
    if (w * h > kMaxArea) {
        const double f = std::sqrt(kMaxArea / (w * h));
        w *= f;
        h *= f;
    }
    *out_w = clamp_dim(w);
    *out_h = clamp_dim(h);
}

double display_aspect(const DisplaySettings& d, int w, int h) {
    if (d.aspect > 0) return std::clamp(d.aspect, kMinAspect, kMaxAspect);
    if (w <= 0 || h <= 0) return kHudAspect;
    return std::clamp(static_cast<double>(w) / h, kMinAspect, kMaxAspect);
}

namespace {
bool near_ratio(double a, double b) { return std::fabs(a - b) < 0.02; }
}  // namespace

bool hud_ortho(float* left, float* right, float* bottom, float* top, double display_aspect) {
    if (!left || !right || !bottom || !top || !(display_aspect > 0)) return false;
    // The retail ball HUD stores left = 8 and right = -8 (reversed): the width is the absolute difference and the order is kept.
    const double span = static_cast<double>(*right) - *left;
    const double w = std::fabs(span);
    const double h = std::fabs(static_cast<double>(*top) - *bottom);
    if (!(w > 0) || !(h > 0)) return false;
    const double own = w / h;
    if (!near_ratio(own, kHudAspect) && !near_ratio(own, display_aspect)) return false;
    const double cx = (static_cast<double>(*left) + *right) * 0.5;
    const double dir = span >= 0 ? 1.0 : -1.0;
    if (display_aspect < kHudAspect - 0.02) {
        // narrower than 16:9: keep the width the frame was drawn for and grow the height around its centre, so nothing is cut off at the sides
        const double cy = (static_cast<double>(*top) + *bottom) * 0.5;
        const double nh = w / display_aspect;
        const double vdir = (static_cast<double>(*top) - *bottom) >= 0 ? 1.0 : -1.0;
        *bottom = static_cast<float>(cy - vdir * nh * 0.5);
        *top = static_cast<float>(cy + vdir * nh * 0.5);
        return true;
    }
    const double nw = h * display_aspect;
    *left = static_cast<float>(cx - dir * nw * 0.5);
    *right = static_cast<float>(cx + dir * nw * 0.5);
    return true;
}

// ---- renderer tuning (every game: the NVN back end's sampler settings) ----------------------------------------------------------
void display_tuning(const DisplaySettings& d) {
    gfx::tuning().anisotropy.store(d.anisotropy);
    gfx::tuning().lod_bias.store(static_cast<float>(std::clamp(d.lod_bias, -2.0, 2.0)));
    gfx::tuning().shadow_scale.store(gfx::shadow_scale_of(d.shadow_quality));
}

}  // namespace nxrt::play
