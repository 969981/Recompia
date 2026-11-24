// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer, play mode: a phone's dynamic resolution (pkp_play_internal.h dynres_pct). Under the game's own controller (KTGL's manager
// picks a lower level when its frame runs late) the scene size can change from one frame to the next, so no scene fraction was handed to
// the back end and SGSR enlarged the game's own bilinear stretch a second time. On a phone the percent is ours instead, set like a fixed
// size (its level and the scene fraction agree, SGSR enlarges the scene part in one step), from the GPU's own time per frame
// (gfx::Tuning::gpu_frame_us, the back end's timestamps):
//   target   90 % of the frame budget (the frame cap, else the screen's refresh rate);
//   lower    when the average GPU time stays above the target for 30 frames (a second at 30 fps): in proportion (GPU time taken to follow the pixels, so the
//            side scales with the square root), at least one 5 % step, to the 5 % grid, never below the learned floor;
//   judge    every step down, 15 frames after it settled: its elasticity is the GPU time it saved against what the pixels it dropped
//            would save in proportion. Under a third (or more than a quarter of the pixels for under a tenth of the time since the
//            player's Resolution) it is taken back and its upper level becomes the floor: lower would only cost sharpness. Measured on
//            a hot Tensor G2 (Mali-G710) phone: 45 ms a frame at 50 % and about 36 ms at 25 %, so it parked at 25 % for 2 to 3 fps;
//   probe    below a floor only after a back-off that doubles with each failed probe (60 s, 2, 4, 8 min, at most 10 min), and not at
//            all while the target is out of reach and the last two probes failed (on the Tensor G2 a 25 -> 20 probe came back every
//            50 s: a blip a minute). A probe that pays clears the floor; a new Resolution or turning it on again starts afresh;
//   raise    one step at a time when the next step is predicted to fit under 95 % of the target for 60 frames in a row (the time per
//            pixel now, scaled by the last measured elasticity): about 2 s at 30 fps; never within 30 s of a step down;
//   learn    what it settled at (the percent, the floor, the thermal cap) is saved at most once a minute in [pkp]
//            dynamic_resolution_state for this screen, mode, Resolution, upscaler and frame cap, and the next start begins there instead
//            of searching again (another screen or setting starts afresh);
//   settle   20 frames after each change before the next decision, with the average started again at the new size;
//   bounds   kPhoneResMin (15 % of the screen) up to the player's Resolution.
// Every decision is logged with the numbers it was made from (elasticity included), and the state every 300 frames.
//
// [pkp] dynamic_resolution_mode = thermal (live; developer A/B: [diag] ab_name = dr_thermal) adds a slow loop on the phone's thermal
// headroom (nxrt/thermal.h, 1 Hz, now and 10 s ahead) that sets a cap the loop above never exceeds, so the GPU keeps slack and its
// governor settles at a lower clock before the phone throttles:
//   target   H* = [pkp] dynamic_resolution_headroom (0.85), or the device's LIGHT threshold minus 0.03 when lower;
//   cap      every 2 polls: down when the forecast is over H* + 0.03, in proportion to the error past that (0.6 % per 0.01, at least
//            0.25, at most 5 % per 2 s; half as fast while the forecast is flat or falling), not below 60 % of the player's Resolution
//            unless the status is severe, and not below a level where 5 % of thermal steps saved under 10 % of the GPU time over 30 s (for
//            10 min), and only while the headroom rises (the forecast 0.01 over now, or now 0.01 over 30 s ago) unless it is 0.08 over
//            the target; up 1 % per 4 s when now and the forecast are both under H* - 0.03, or when the headroom has not risen for 30 s
//            and the GPU is under 75 % of its budget (a hot Snapdragon 8 Gen 3 phone held the cap at 15 % for 20 min at 19 of 41.7 ms and a steady headroom
//            of 0.87), then only 1 % per 12 s and never to within 3 % of a level that had to drop right after such a climb in the last
//            5 min (a Snapdragon 8 Gen 3 phone cycled 55 -> 35 -> 55 % every 2.5 to 3 min); either 10 s after the last decrease, below the
//            moderate status and while the loop above is not already lower than the cap; severe (or headroom 1.0): 15 % at once;
//            critical: the lowest resolution;
//   guard    the loop above with deadline thresholds: one step down after 2 frames over 98 % of the budget or the average over 92 % for
//            30 frames (each after the last step's verdict, so at least 35 frames apart); one step up after 2 s under 75 % when the next step is predicted under 85 %; nothing in between, which leaves
//            the governor room to lower the clock. Changes per minute are logged.
// No headroom on the device: the guard alone (the cap stays at the player's Resolution). Not implemented: lowering the frame cap at the
// lowest resolution.
#include "input/pkp_play_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/pkp_settings.h"
#include "nxrt/ab_switch.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"
#include "nxrt/thermal.h"
#include "render/pkp_phone_res.h"

namespace nxrt::games::pkp::play_detail {

std::atomic<int> g_dyn_fps_cap{30};

namespace {

constexpr int kStep = 5;
constexpr int kLowerAfter = 30, kRaiseAfter = 60, kSettle = 20, kJudge = 15, kStateEvery = 300;
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
    int lim = 0;                   // the cap the loop works under this frame: the player's Resolution, or the thermal cap below it
    // thermal mode
    bool thermal = false;
    double tcap = 0.0;             // the thermal cap, percent of the screen (moves by 1 %; the loop uses it on the 5 % grid)
    uint64_t polls_acted = 0;      // thermal::Headroom::polls at the last slow-loop action
    int last_status = -1;
    double prev_ms = 0.0;          // the previous frame's GPU ms (the guard's two-frame panic)
    int under = 0;                 // frames in a row under 75 % of the budget (the guard's raise)
    std::chrono::steady_clock::time_point last_down{}, last_up{}, minute{};
    int changes = 0;               // resolution changes since `minute`
    bool told_headroom = false;
    std::chrono::steady_clock::time_point last_drop{}, saved{};   // the loop's last step down; the last save of what it learned
    std::string sig;               // what the learned state belongs to (screen, mode, Resolution, upscaler, frame cap)
    std::string stored;            // the state as last written
    double gpu_long = 0.0;         // GPU ms per frame, slow average (0.02 a frame): what the thermal steps are judged by
    double tref_cap = 0.0, tref_ms = 0.0;   // the thermal cap and slow GPU time when the current run of thermal steps down began
    std::chrono::steady_clock::time_point tref_t{}, tfloor_until{};
    int tfloor = 0;                // a thermal cap its steps were found not to help below (until tfloor_until)
    int tfloor_now = 0;            // the thermal floor in force (60 % of the Resolution, or that level; the lowest when severe)
    bool climbed = false;          // the cap rose on a steady headroom since its last drop
    double unsustainable = 0.0;    // the cap a drop began at right after such a climb (until hold_until)
    std::chrono::steady_clock::time_point hold_until{};
    float h_hist[15] = {};         // the headroom at the last 15 slow-loop actions (about 30 s)
    int h_n = 0;
};
Dyn g_dyn;   // the game thread's (hook_copy_params)

double budget_ms() {
    int fps = g_dyn_fps_cap.load(std::memory_order_relaxed);
    if (fps <= 0) fps = static_cast<int>(std::lround(platform::display_refresh_hz()));
    return 1000.0 / std::clamp(fps, 10, 240);
}

void change(Dyn& d, int to, const char* why, double target) {
    ++d.changes;
    if (to < d.pct) d.last_drop = std::chrono::steady_clock::now();
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
    const bool poor_trade = d.cap_ms > 0.0 && pixels(d.pct, d.lim) < 0.75 && (d.cap_ms - d.avg) < 0.10 * d.cap_ms;
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
    const int back = poor_trade ? d.lim : d.from;
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

// The thermal cap (slow loop): acts on every second poll of the headroom.
void thermal_tick(Dyn& d, int cap, int lowest) {
    thermal::headroom_start();
    thermal::Headroom h;
    if (!thermal::headroom(&h)) {
        d.tcap = cap;
        if (!d.told_headroom && ++d.samples > 900) {   // the poller needs a few seconds; past that the device has none
            d.told_headroom = true;
            NXLOG_W(kTag, "dynamic resolution: no thermal headroom on this device: the deadline guard alone, under the player's Resolution");
        }
        return;
    }
    if (!d.told_headroom) {
        d.told_headroom = true;
        NXLOG_I(kTag, "dynamic resolution: thermal cap on (headroom %.2f now, %.2f in 10 s, LIGHT threshold %.2f)", h.now, h.forecast, h.light);
    }
    const auto now = std::chrono::steady_clock::now();
    const double target = std::min(settings::get_double(settings::Key::DynamicResolutionHeadroom),
                                   std::isnan(h.light) ? 1.0 : static_cast<double>(h.light) - 0.03);
    const double before = d.tcap;
    const char* why = nullptr;
    if (h.status != d.last_status) {   // the status overrides, at once
        if (h.status >= 4) { d.tcap = lowest; why = "critical status"; }
        else if (h.status >= 3 && d.last_status < 3) { d.tcap -= 15.0; why = "severe status"; }
        d.last_status = h.status;
    }
    if (!why && h.now >= 1.0f && d.tcap > lowest && h.polls >= d.polls_acted + 2 && before > 0 && now - d.last_down > std::chrono::seconds(10)) {
        d.tcap -= 15.0;
        why = "headroom at the severe status";
    }
    // the thermal loop alone never takes the cap below 60 % of the player's Resolution (nor below a level its own steps were found not to
    // help at), unless the status is severe: on a Snapdragon 8 Gen 3 phone only "warm" (headroom 0.79 against a target of 0.80) the noisy forecast drove the
    // cap down to 15 % and nothing raised it again
    const bool severe = h.status >= 3 || h.now >= 1.0f;
    const int no_gain = now < d.tfloor_until ? d.tfloor : 0;
    const double floor_t = severe ? lowest : std::max({static_cast<double>(lowest), std::ceil(0.6 * cap), static_cast<double>(no_gain)});
    d.tfloor_now = static_cast<int>(floor_t);
    // the headroom 30 s ago (one sample per slow-loop action): stable or falling means the cap is not what heats the phone further
    const bool act = h.polls >= d.polls_acted + 2;
    if (act) { d.h_hist[d.h_n++ % 15] = h.now; }
    const float h_30s = d.h_n >= 15 ? d.h_hist[d.h_n % 15] : NAN;
    const bool rising = h.forecast - h.now > 0.01f || (!std::isnan(h_30s) && h.now - h_30s > 0.01f);
    const bool slack = d.avg > 0.0 && d.avg < 0.75 * budget_ms();
    if (!why && act) {
        d.polls_acted = h.polls;
        if (h.forecast > target + 0.03 && d.tcap > floor_t && (rising || h.now > target + 0.08)) {
            // a drop right after a climb on a steady headroom: the level it climbed to was not sustainable; the climb stops 3 % under it
            // for 5 min (a Snapdragon 8 Gen 3 phone cycled 55 -> 35 -> 55 % every 2.5 to 3 min at a steady 0.84 to 0.88)
            if (d.climbed) {
                d.unsustainable = before;
                d.hold_until = now + std::chrono::minutes(5);
                d.climbed = false;
                NXLOG_I(kTag, "dynamic resolution cap: %.0f %% was not sustainable: the cap climbs no higher than %.0f %% for 5 min", before, before - 3.0);
            }
            // in proportion to the error past the deadband (0.01 of headroom: 0.6 %), half as fast while the forecast is flat or falling
            double step = 60.0 * (h.forecast - (target + 0.03));
            if (h.forecast - h.now < 0.005f) step *= 0.5;
            d.tcap -= std::clamp(step, 0.25, 5.0);
            why = "the forecast is over the target";
            if (d.tref_cap <= 0.0) { d.tref_cap = before; d.tref_ms = d.gpu_long; d.tref_t = now; }
        } else {
            // release: headroom to spare (1 % per 4 s), or a stable headroom (not rising for 30 s) with the GPU under 75 % of its budget,
            // which while over the target climbs 1 % per 12 s and not to a level found unsustainable in the last 5 min
            const bool spare = std::max(h.now, h.forecast) < target - 0.03;
            const bool steady = !rising && slack && !std::isnan(h_30s);
            const auto every = spare ? std::chrono::seconds(4) : std::chrono::seconds(12);
            const double top = now < d.hold_until ? d.unsustainable - 3.0 : static_cast<double>(cap);
            if ((spare || steady) && now - d.last_down > std::chrono::seconds(10) && now - d.last_up >= every && h.status < 2 &&
                d.pct >= std::min<int>(cap, d.lim) && d.tcap < cap && d.tcap + 1.0 <= top) {
                d.tcap += 1.0;
                d.last_up = now;
                if (!spare) d.climbed = true;
                why = spare ? "headroom to spare" : "the headroom is steady and the GPU has slack";
            }
        }
    }
    // the thermal steps judged like the loop's own: after 5 % or more down and 30 s, a GPU time that fell less than 10 % means the cap is
    // not what heats the phone (the scene was already under it, or the heat is elsewhere): no lower for 10 min
    if (d.tref_cap > 0.0 && d.tref_cap - d.tcap >= 5.0 && now - d.tref_t >= std::chrono::seconds(30)) {
        if (d.tref_ms > 0.0 && d.gpu_long > 0.9 * d.tref_ms) {
            d.tfloor = static_cast<int>(std::lround(d.tcap));
            d.tfloor_until = now + std::chrono::minutes(10);
            NXLOG_I(kTag, "dynamic resolution cap: %.0f -> %.0f %% gained little (GPU %.1f -> %.1f ms a frame): no lower on heat alone for 10 min",
                    d.tref_cap, d.tcap, d.tref_ms, d.gpu_long);
        }
        d.tref_cap = 0.0;
    }
    if (!severe && d.tcap < floor_t) d.tcap = floor_t;
    d.tcap = std::clamp(d.tcap, static_cast<double>(lowest), static_cast<double>(cap));
    if (why && static_cast<int>(d.tcap) != static_cast<int>(before)) {
        if (d.tcap < before) d.last_down = now;
        NXLOG_I(kTag, "dynamic resolution cap: %.0f -> %.0f %% (%s: headroom %.3f now, %.3f in 10 s, target %.3f, status %d)", before, d.tcap, why, h.now,
                h.forecast, target, h.status);
    }
}

// What the learned state belongs to: a new screen (the other side of a foldable), mode, Resolution, upscaler or frame cap starts learning again.
std::string signature(bool thermal, int cap) {
    char b[96];
    std::snprintf(b, sizeof b, "v1 %dx%d %s cap=%d up=%lld fps=%d", g_ph_win_w.load(std::memory_order_relaxed), g_ph_win_h.load(std::memory_order_relaxed),
                  thermal ? "thermal" : "gpu", cap, static_cast<long long>(settings::get_int(settings::Key::SceneUpscaler)),
                  g_dyn_fps_cap.load(std::memory_order_relaxed));
    return b;
}

// The learned state of the last session with the same signature: the percent it settled at, its floor and the thermal cap.
void resume(Dyn& d, int lowest) {
    const std::string st = settings::get_str(settings::Key::DynamicResolutionState);
    if (st.size() <= d.sig.size() || st.compare(0, d.sig.size(), d.sig) != 0) return;
    int pct = 0, floor_pct = 0;
    double tcap = 0.0;
    if (std::sscanf(st.c_str() + d.sig.size(), " pct=%d floor=%d tcap=%lf", &pct, &floor_pct, &tcap) != 3) return;
    d.pct = std::clamp(pct, lowest, d.cap);
    d.floor_pct = floor_pct > 0 ? std::clamp(floor_pct, lowest, d.cap) : 0;
    if (tcap > 0.0) d.tcap = std::clamp(tcap, static_cast<double>(lowest), static_cast<double>(d.cap));
    d.wait = d.backoff;   // a learned floor is not probed at once
    d.stored = st;
    NXLOG_I(kTag, "dynamic resolution: resuming what was learned for this screen and settings: %d %% (floor %d, thermal cap %.0f)", d.pct, d.floor_pct,
            d.tcap);
}

// Writes the learned state at most once a minute, and only when it changed.
void save(Dyn& d) {
    const auto now = std::chrono::steady_clock::now();
    if (now - d.saved < std::chrono::seconds(60)) return;
    d.saved = now;
    if (signature(d.thermal, d.cap) != d.sig) {   // the upscaler or the frame cap changed live: learn again, under the new signature
        d.cap = 0;
        return;
    }
    char b[160];
    std::snprintf(b, sizeof b, "%s pct=%d floor=%d tcap=%.0f", d.sig.c_str(), d.pct, d.floor_pct, d.thermal ? d.tcap : 0.0);
    if (d.stored == b) return;
    d.stored = b;
    settings::set_str(settings::Key::DynamicResolutionState, b);
    NXLOG_I(kTag, "dynamic resolution: learned state saved (%s)", b);
}

}  // namespace

int dynres_pct(int cap, bool on) {
    Dyn& d = g_dyn;
    gfx::Tuning& tu = gfx::tuning();
    if (!on) {
        if (d.pct) NXLOG_I(kTag, "dynamic resolution: off, %d %% of the screen", cap);
        d = Dyn();
        tu.res_dr_cap.store(0, std::memory_order_relaxed);
        return cap;
    }
    const int lowest = std::min(kPhoneResMin, cap);
    const bool thermal = diag::ab("dr_thermal", settings::get_str(settings::Key::DynamicResolutionMode) == "thermal");
    if (!d.pct || d.cap != cap || d.thermal != thermal) {   // on, a new Resolution or the other mode: start afresh at it
        const uint64_t seen = tu.gpu_frames.load(std::memory_order_relaxed);
        const bool first = !d.pct;
        d = Dyn();
        d.pct = d.cap = d.lim = cap;
        d.tcap = cap;
        d.thermal = thermal;
        d.seen = seen;
        d.settle = kSettle;
        d.minute = d.saved = std::chrono::steady_clock::now();
        NXLOG_I(kTag, "dynamic resolution: %s, %d to %d %% of the screen, %s (budget %.1f ms)", first ? "on" : "restarted", lowest, cap,
                thermal ? "under a thermal cap, the GPU time as a deadline guard" : "from the GPU's time per frame", budget_ms());
        d.sig = signature(thermal, cap);
        resume(d, lowest);
    }
    if (thermal) thermal_tick(d, cap, lowest);
    // the cap of this frame on the 5 % grid; the loop below never goes over it
    // the cap of this frame on the 5 % grid, rounded up to the thermal floor (33 % went to 30, under the floor)
    d.lim = thermal ? std::max(lowest, std::min(cap, std::max(static_cast<int>(d.tcap) / kStep * kStep, (d.tfloor_now + kStep - 1) / kStep * kStep))) : cap;
    tu.res_dr_cap.store(thermal ? d.lim : 0, std::memory_order_relaxed);
    if (d.pct > d.lim) change(d, d.lim, "the thermal cap", 0.9 * budget_ms());
    if (const auto now = std::chrono::steady_clock::now(); now - d.minute >= std::chrono::seconds(60)) {
        NXLOG_I(kTag, "dynamic resolution: %d changes in the last minute (%d %% of the screen, cap %d)", d.changes, d.pct, d.lim);
        d.changes = 0;
        d.minute = now;
    }
    if (d.settle == 0 && d.judge == 0) save(d);
    const uint64_t n = tu.gpu_frames.load(std::memory_order_relaxed);
    if (n == d.seen) {
        if (n == 0 && !d.told_none && ++d.samples > 600) {
            d.told_none = true;
            NXLOG_W(kTag, "dynamic resolution: no GPU timestamps on this device: the resolution follows only the thermal cap (%d %%)", d.pct);
        }
        return d.pct;
    }
    d.seen = n;
    const double budget = budget_ms();
    const double ms = tu.gpu_frame_us.load(std::memory_order_relaxed) / 1000.0;
    d.avg = d.avg > 0.0 ? d.avg + 0.15 * (ms - d.avg) : ms;
    d.gpu_long = d.gpu_long > 0.0 ? d.gpu_long + 0.02 * (ms - d.gpu_long) : ms;
    const double target = thermal ? 0.92 * budget : 0.9 * budget;
    const bool panic = thermal && ms > 0.98 * budget && d.prev_ms > 0.98 * budget;
    d.prev_ms = ms;
    if (d.wait > 0) --d.wait;
    if (d.failed && d.avg < 0.8 * target) d.failed = 0;   // the target is in reach again: probes may resume after their back-off
    if (++d.samples % kStateEvery == 0) {
        thermal::Headroom h;
        const bool hh = thermal && thermal::headroom(&h);
        NXLOG_I(kTag, "dynamic resolution: %d %% of the screen (cap %d, elasticity floor %d, thermal floor %d), GPU %.1f ms a frame on average, target %.1f ms, elasticity %.2f%s",
                d.pct, d.lim, d.floor_pct, thermal ? d.tfloor_now : 0, d.avg, target, d.elasticity,
                hh ? (", headroom " + std::to_string(h.now).substr(0, 5) + " now, " + std::to_string(h.forecast).substr(0, 5) + " in 10 s").c_str() : "");
    }
    // a panic still waits for the last step's verdict (settle and judge): stepping on before it chained a probe from 100 to 70 % in 2 s
    if (d.settle > 0) {
        --d.settle;
        return d.pct;
    }
    if (d.pct == d.lim) d.cap_ms = d.avg;
    if (d.judge > 0) {
        if (--d.judge == 0) judge_step(d, target);
        return d.pct;
    }
    d.over = d.avg > target ? d.over + 1 : 0;
    if ((panic || d.over >= kLowerAfter) && d.pct > lowest) {
        const double side = d.pct * std::sqrt(target / std::max(d.avg, target));
        int to = thermal ? d.pct - kStep : std::max(lowest, std::min(d.pct - kStep, static_cast<int>(side) / kStep * kStep));   // thermal: one step
        to = std::max(lowest, to);
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
        change(d, to, probe ? "a probe below the floor" : panic ? "two frames over 98 % of the budget" : "the GPU is late", target);
        d.from = from;
        d.from_ms = from_ms;
        d.judge = kJudge;
        d.probing = probe;
        return d.pct;
    }
    if (std::chrono::steady_clock::now() - d.last_drop < std::chrono::seconds(30)) { d.fits = d.under = 0; return d.pct; }   // no raise within 30 s of a drop
    const int up = std::min(d.lim, (d.pct / kStep + 1) * kStep);
    const double next = d.avg * (1.0 + d.elasticity * (pixels(up, d.pct) - 1.0));
    if (thermal) {   // the guard: 2 s under 75 % of the budget and the next step predicted under 85 %
        d.under = d.avg < 0.75 * budget ? d.under + 1 : 0;
        const int two_s = static_cast<int>(std::lround(2000.0 / budget));
        if (up > d.pct && d.under >= two_s && next < 0.85 * budget) change(d, up, "the next step fits under 85 % of the budget", target);
        return d.pct;
    }
    d.fits = up > d.pct && next < 0.95 * target ? d.fits + 1 : 0;
    if (d.fits >= kRaiseAfter) change(d, up, "the next step fits", target);
    return d.pct;
}

}  // namespace nxrt::games::pkp::play_detail
