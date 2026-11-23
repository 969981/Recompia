// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer, play mode: a phone's dynamic resolution (pkp_play_internal.h dynres_pct). Under the game's own controller (KTGL's manager
// picks a lower level when its frame runs late) the scene size can change from one frame to the next, so no scene fraction was handed to
// the back end and SGSR enlarged the game's own bilinear stretch a second time. On a phone the percent is ours instead, set like a fixed
// size (its level and the scene fraction agree, SGSR enlarges the scene part in one step), from the GPU's own time per frame
// (gfx::Tuning::gpu_frame_us, the back end's timestamps):
//   target   90 % of the frame budget (the frame cap, else the screen's refresh rate);
//   lower    when the average GPU time stays above the target for 8 frames: in proportion (GPU time taken to follow the pixels, so the
//            side scales with the square root), at least one 5 % step, to the 5 % grid, never below the learned floor;
//   judge    every step down, 15 frames after it settled: its elasticity is the GPU time it saved against what the pixels it dropped
//            would save in proportion. Under a third (or more than a quarter of the pixels for under a tenth of the time since the
//            player's Resolution) it is taken back and its upper level becomes the floor: lower would only cost sharpness. Measured on
//            a hot Tensor G2 (Mali-G710) phone: 45 ms a frame at 50 % and about 36 ms at 25 %, so it parked at 25 % for 2 to 3 fps;
//   probe    below a floor only after a back-off that doubles with each failed probe (60 s, 2, 4, 8 min, at most 10 min), and not at
//            all while the target is out of reach and the last two probes failed (on the Tensor G2 a 25 -> 20 probe came back every
//            50 s: a blip a minute). A probe that pays clears the floor; a new Resolution or turning it on again starts afresh;
//   raise    one step at a time when the next step is predicted to fit under 95 % of the target for 60 frames in a row (the time per
//            pixel now, scaled by the last measured elasticity): about 2 s at 30 fps;
//   settle   20 frames after each change before the next decision, with the average started again at the new size;
//   bounds   kPhoneResMin (15 % of the screen) up to the player's Resolution.
// Every decision is logged with the numbers it was made from (elasticity included), and the state every 300 frames.
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

constexpr int kStep = 5;
constexpr int kLowerAfter = 8, kRaiseAfter = 60, kSettle = 20, kJudge = 15, kStateEvery = 300;
constexpr int kProbeFirst = 1800, kProbeMax = 18000;   // frames before a probe below a floor (60 s, doubling, at most 10 min at 30 fps)
constexpr double kMinElasticity = 1.0 / 3.0;

struct Dyn {
    int pct = 0, cap = 0;          // percent of the screen in force (0: not running), the player's Resolution it started from
    double avg = 0.0;              // GPU ms per frame since the last change (exponential, 0.15 per frame)
    uint64_t seen = 0;             // gfx::Tuning::gpu_frames at the last sample
    int over = 0, fits = 0, settle = 0, samples = 0;
    int from = 0, judge = 0;       // the step down being judged: the percent before it, frames until the verdict
    double from_ms = 0.0;          // the average before that step
    double cap_ms = 0.0;           // the average last measured at the player's Resolution
    double elasticity = 1.0;       // the last step's measured share of the proportional saving (0.3 .. 1, for the raise prediction)
    int floor_pct = 0;             // the lowest percent worth rendering (learned; 0: none)
    int wait = 0, backoff = kProbeFirst, failed = 0;   // frames until a probe below the floor, the next back-off, probes failed in a row
    bool probing = false;          // the step being judged went below the floor
    bool told_none = false;
};
Dyn g_dyn;   // the game thread's (hook_copy_params)

double budget_ms() {
    int fps = g_dyn_fps_cap.load(std::memory_order_relaxed);
    if (fps <= 0) fps = static_cast<int>(std::lround(platform::display_refresh_hz()));
    return 1000.0 / std::clamp(fps, 10, 240);
}

void change(Dyn& d, int to, const char* why, double target) {
    NXLOG_I(kTag, "dynamic resolution: %d -> %d %% of the screen (%s: GPU %.1f ms a frame on average, target %.1f ms)", d.pct, to, why, d.avg, target);
    d.pct = to;
    d.avg = 0.0;
    d.over = d.fits = 0;
    d.settle = kSettle;
    d.judge = 0;
}

double pixels(int a, int b) { return (static_cast<double>(a) * a) / (static_cast<double>(b) * b); }   // a's pixels as a share of b's

// The verdict on the step down from d.from to d.pct, made from d.avg against d.from_ms.
void judge_step(Dyn& d, double target) {
    const double expected = d.from_ms * (1.0 - pixels(d.pct, d.from));   // the saving in proportion to the pixels dropped
    const double saved = d.from_ms - d.avg;
    const double e = expected > 0.0 ? saved / expected : 0.0;
    // more than a quarter of the player's pixels for under a tenth of the GPU time measured there: stay high
    const bool poor_trade = d.cap_ms > 0.0 && pixels(d.pct, d.cap) < 0.75 && (d.cap_ms - d.avg) < 0.10 * d.cap_ms;
    if (e >= kMinElasticity && !poor_trade) {
        NXLOG_I(kTag, "dynamic resolution: %d -> %d %% kept: it saved %.1f of %.1f ms in proportion (elasticity %.2f)", d.from, d.pct, saved,
                expected, e);
        d.elasticity = std::clamp(e, 0.3, 1.0);
        if (d.probing) {
            d.floor_pct = 0;          // a probe that paid: the floor is gone
            d.failed = 0;
            d.backoff = kProbeFirst;
        }
        d.probing = false;
        return;
    }
    const int back = poor_trade ? d.cap : d.from;
    char why[96];
    std::snprintf(why, sizeof why, "%s, elasticity %.2f: %.1f of %.1f ms saved", poor_trade ? "a poor trade for the pixels" : "the step gained little", e,
                  saved, expected);
    d.elasticity = std::clamp(e, 0.3, 1.0);
    d.floor_pct = back;
    if (d.probing) {
        ++d.failed;
        d.wait = d.backoff;
        d.backoff = std::min(d.backoff * 2, kProbeMax);
    } else {
        d.wait = d.backoff;
    }
    d.probing = false;
    change(d, back, why, target);
    NXLOG_I(kTag, "dynamic resolution: floor %d %%, next probe below it in %d frames%s", d.floor_pct, d.wait,
            d.failed >= 2 ? " (none while the target is out of reach: the last two probes failed)" : "");
}

}  // namespace

int dynres_pct(int cap, bool on) {
    Dyn& d = g_dyn;
    if (!on) {
        if (d.pct) NXLOG_I(kTag, "dynamic resolution: off, %d %% of the screen", cap);
        d = Dyn();
        return cap;
    }
    const int lowest = std::min(kPhoneResMin, cap);
    if (!d.pct || d.cap != cap) {   // on, or a new Resolution: start afresh at it
        const uint64_t seen = gfx::tuning().gpu_frames.load(std::memory_order_relaxed);
        const bool first = !d.pct;
        d = Dyn();
        d.pct = d.cap = cap;
        d.seen = seen;
        d.settle = kSettle;
        if (first)
            NXLOG_I(kTag, "dynamic resolution: on, %d to %d %% of the screen, from the GPU's time per frame (target 90 %% of %.1f ms)", lowest, cap, budget_ms());
    }
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
    if (d.wait > 0) --d.wait;
    if (d.failed && d.avg < 0.8 * target) d.failed = 0;   // the target is in reach again: probes may resume after their back-off
    if (++d.samples % kStateEvery == 0)
        NXLOG_I(kTag, "dynamic resolution: %d %% of the screen (cap %d, floor %d), GPU %.1f ms a frame on average, target %.1f ms, elasticity %.2f",
                d.pct, cap, d.floor_pct, d.avg, target, d.elasticity);
    if (d.settle > 0) {
        --d.settle;
        return d.pct;
    }
    if (d.pct == cap) d.cap_ms = d.avg;
    if (d.judge > 0) {
        if (--d.judge == 0) judge_step(d, target);
        return d.pct;
    }
    d.over = d.avg > target ? d.over + 1 : 0;
    if (d.over >= kLowerAfter && d.pct > lowest) {
        const double side = d.pct * std::sqrt(target / d.avg);
        int to = std::max(lowest, std::min(d.pct - kStep, static_cast<int>(side) / kStep * kStep));
        bool probe = false;
        if (d.floor_pct && to < d.floor_pct) {
            // below the floor only as a probe: after the back-off, and not while out of reach after two failed probes
            if (d.pct > d.floor_pct) to = d.floor_pct;
            else if (d.wait == 0 && d.failed < 2) { to = std::max(lowest, d.pct - kStep); probe = true; }
            else return d.pct;
        }
        if (to >= d.pct) return d.pct;
        const int from = d.pct;
        const double from_ms = d.avg;
        change(d, to, probe ? "a probe below the floor" : "the GPU is late", target);
        d.from = from;
        d.from_ms = from_ms;
        d.judge = kJudge;
        d.probing = probe;
        return d.pct;
    }
    const int up = std::min(cap, (d.pct / kStep + 1) * kStep);
    const double next = d.avg * (1.0 + d.elasticity * (pixels(up, d.pct) - 1.0));
    d.fits = up > d.pct && next < 0.95 * target ? d.fits + 1 : 0;
    if (d.fits >= kRaiseAfter) change(d, up, "the next step fits", target);
    return d.pct;
}

}  // namespace nxrt::games::pkp::play_detail
