// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer, play mode: a phone's dynamic resolution (pkp_play_internal.h dynres_pct). Under the game's own controller (KTGL's manager
// picks a lower level when its frame runs late) the scene size can change from one frame to the next, so no scene fraction was handed to
// the back end and SGSR enlarged the game's own bilinear stretch a second time. On a phone the percent is ours instead, set like a fixed size (its level
// and the scene fraction agree, SGSR enlarges the scene part in one step), from the GPU's own time per frame (gfx::Tuning::gpu_frame_us,
// the back end's timestamps):
//   target   90 % of the frame budget (the frame cap, else the screen's refresh rate);
//   lower    when the average GPU time stays above the target for 8 frames: in proportion (GPU time follows the pixels, so the side scales
//            with the square root), at least one 5 % step, to the 5 % grid;
//   raise    one step at a time, only when the average predicts the next step fits under 95 % of the target (its pixels times the time
//            per pixel now) for 60 frames in a row: about 2 s at 30 fps, so it never pumps between two steps;
//   settle   20 frames after each change before the next decision, with the average started again at the new size;
//   no gain  15 frames after settling, a step down that did not take 15 % off the GPU time goes back up, and no step down follows
//            for 900 frames (30 s at 30 fps): the time was not the picture's (a load, uploads, the passes over the whole target), and
//            staying lower would only cost sharpness (measured on the desktop: a load's 68 ms took it from 62 to 15 %, where it stayed
//            at 35 ms);
//   bounds   kPhoneResMin (15 % of the screen) up to the player's Resolution.
// Every change is logged with the numbers it was made from, and the state every 300 frames.
#include "input/pkp_play_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "nxrt/gfx_tuning.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"
#include "render/pkp_phone_res.h"

namespace nxrt::games::pkp::play_detail {

std::atomic<int> g_dyn_fps_cap{30};

namespace {

struct Dyn {
    int pct = 0;           // percent of the screen in force (0: not running)
    double avg = 0.0;      // GPU ms per frame since the last change (exponential, 0.15 per frame)
    uint64_t seen = 0;     // gfx::Tuning::gpu_frames at the last sample
    int over = 0, fits = 0, settle = 0, samples = 0;
    int from = 0, judge = 0, calm = 0;   // the step down being judged: the percent before it, frames until the verdict; frames without steps down
    double from_ms = 0.0;                // the average before that step
    bool told_none = false;
};
Dyn g_dyn;   // the game thread's (hook_copy_params)

constexpr int kStep = 5;
constexpr int kLowerAfter = 8, kRaiseAfter = 60, kSettle = 20, kJudge = 15, kCalm = 900, kStateEvery = 300;

double budget_ms() {
    int fps = g_dyn_fps_cap.load(std::memory_order_relaxed);
    if (fps <= 0) fps = static_cast<int>(std::lround(platform::display_refresh_hz()));
    return 1000.0 / std::clamp(fps, 10, 240);
}

void change(Dyn& d, int to, const char* why, double ms, double target) {
    NXLOG_I(kTag, "dynamic resolution: %d -> %d %% of the screen (%s: GPU %.1f ms a frame on average, target %.1f ms)", d.pct, to, why, ms, target);
    d.pct = to;
    d.avg = 0.0;
    d.over = d.fits = 0;
    d.settle = kSettle;
    d.judge = 0;
}

}  // namespace

int dynres_pct(int cap, bool on) {
    Dyn& d = g_dyn;
    if (!on) {
        if (d.pct) NXLOG_I(kTag, "dynamic resolution: off, %d %% of the screen", cap);
        d = Dyn();
        return cap;
    }
    const int floor = std::min(kPhoneResMin, cap);
    if (!d.pct) {
        d.pct = cap;
        d.seen = gfx::tuning().gpu_frames.load(std::memory_order_relaxed);
        NXLOG_I(kTag, "dynamic resolution: on, %d to %d %% of the screen, from the GPU's time per frame (target 90 %% of %.1f ms)", floor, cap, budget_ms());
    }
    if (d.pct > cap) d.pct = cap;   // the player lowered the cap: at once
    const uint64_t n = gfx::tuning().gpu_frames.load(std::memory_order_relaxed);
    if (n == d.seen) {
        if (n == 0 && !d.told_none && ++d.samples > 600) {
            d.told_none = true;
            NXLOG_W(kTag, "dynamic resolution: no GPU timestamps on this device: the resolution stays at %d %%", d.pct);
        }
        return d.pct;
    }
    d.seen = n;
    const double ms = gfx::tuning().gpu_frame_us.load(std::memory_order_relaxed) / 1000.0;
    d.avg = d.avg > 0.0 ? d.avg + 0.15 * (ms - d.avg) : ms;
    const double target = 0.9 * budget_ms();
    if (++d.samples % kStateEvery == 0)
        NXLOG_I(kTag, "dynamic resolution: %d %% of the screen (cap %d), GPU %.1f ms a frame on average, target %.1f ms", d.pct, cap, d.avg, target);
    if (d.calm > 0) --d.calm;
    if (d.settle > 0) {
        --d.settle;
        return d.pct;
    }
    if (d.judge > 0 && --d.judge == 0 && d.avg > 0.85 * d.from_ms) {
        const int back = std::min(cap, d.from);
        change(d, back, "the step down gained nothing", d.avg, target);
        d.calm = kCalm;
        return d.pct;
    }
    d.over = d.avg > target ? d.over + 1 : 0;
    if (d.over >= kLowerAfter && d.pct > floor && d.calm == 0 && d.judge == 0) {
        const double side = d.pct * std::sqrt(target / d.avg);
        const int to = std::max(floor, std::min(d.pct - kStep, static_cast<int>(side) / kStep * kStep));
        const int from = d.pct;
        const double from_ms = d.avg;
        change(d, to, "the GPU is late", d.avg, target);
        d.from = from;
        d.from_ms = from_ms;
        d.judge = kJudge;
        return d.pct;
    }
    if (d.judge > 0) return d.pct;
    const int up = std::min(cap, (d.pct / kStep + 1) * kStep);
    const double next = d.avg * (static_cast<double>(up) * up) / (static_cast<double>(d.pct) * d.pct);
    d.fits = up > d.pct && next < 0.95 * target ? d.fits + 1 : 0;
    if (d.fits >= kRaiseAfter) change(d, up, "the next step fits", d.avg, target);
    return d.pct;
}

}  // namespace nxrt::games::pkp::play_detail
