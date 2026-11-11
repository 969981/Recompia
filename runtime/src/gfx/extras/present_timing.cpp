// SPDX-License-Identifier: GPL-3.0-or-later
// Presentation timing and display-locked pacing (include/nxrt/present_timing.h).
#include "nxrt/present_timing.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "pacing";
constexpr uint64_t kRing = 256;                 // frames kept (a record is complete about two frames after its present call)
constexpr uint64_t kLogNs = 10000000000ull;     // one summary every 10 s
constexpr uint64_t kHoldAfterVblankNs = 1000000; // a held present goes this long after the vblank before its slot
constexpr int kBudgetFrames = 128;              // vblank_lock: release -> GPU done kept for the budget
constexpr double kMaxShiftNs = 500000.0;        // vblank_lock: the grid moves at most this much a frame
constexpr double kShiftGain = 0.2;              // vblank_lock: share of the error corrected each frame (the error shows 2-3 frames later)
constexpr double kPi = 3.14159265358979323846;

struct Cfg {
    bool stats = false, lock = false;
};
const Cfg& cfg() {
    static const Cfg c = [] {
        Cfg v;
        v.stats = config_get_bool("latency", "present_stats", false);
        v.lock = config_get_bool("latency", "vblank_lock", false);
        return v;
    }();
    return c;
}

struct Rec {
    std::atomic<uint64_t> frame{0};             // the frame the fields belong to (0 while it is being reset)
    std::atomic<uint64_t> call{0}, release{0}, replay{0}, held{0}, q0{0}, q1{0}, gpu{0}, shown{0};
};
Rec g_ring[kRing];
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_pending_release{0};     // boundary release not yet taken by a present call
std::atomic<FrameLimiter*> g_lim{nullptr};
std::atomic<bool> g_waiter{false};
std::atomic<uint64_t> g_waits[3];               // game frames with 0, 1, 2+ boundary waits (cumulative, always counted)
thread_local uint64_t t_render_frame = 0;

// the last frame known to be on the screen, for the hold
std::mutex g_shown_mu;
uint64_t g_shown_frame = 0, g_shown_ns = 0;

Rec* rec(uint64_t f) {
    if (!f) return nullptr;
    Rec& r = g_ring[f % kRing];
    return r.frame.load(std::memory_order_acquire) == f ? &r : nullptr;
}

double ms(uint64_t ns) { return static_cast<double>(ns) * 1e-6; }
double ms_between(uint64_t a, uint64_t b) { return (static_cast<double>(b) - static_cast<double>(a)) * 1e-6; }

// p in [0, 1] of v (sorts v); NaN when empty
double pct(std::vector<float>& v, double p) {
    if (v.empty()) return std::nan("");
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}

// Everything below runs on the thread that completes a frame (the present waiter, or the submit thread without it), under g_mu.
std::mutex g_mu;
struct Agg {
    // the last two completed frames (a frame's time on screen is known when the next one is shown)
    uint64_t prev_f = 0, prev_s = 0, pprev_f = 0, pprev_s = 0;
    // window
    uint64_t t0 = 0, frames = 0, shown_n[4] = {}, exact = 0, late = 0, held = 0;
    uint64_t waits_at_t0[3] = {};
    double held_ms = 0, phase_c = 0, phase_s = 0;
    uint64_t phase_n = 0;
    std::vector<float> dur, rel_call, call_rep, rep_q, qblock, rel_gpu, gpu_shown, rel_shown, margin;
    // vblank_lock
    float budget[kBudgetFrames] = {};
    int budget_n = 0, budget_pos = 0;
    double guard_ns = 1000000.0, shift_pos = 0, shift_neg = 0, last_budget = 0, last_target = 0;
    uint64_t lock_frames = 0, lock_late = 0, lock_late_window = 0;
    bool lock_active = false;
    int lock_lines = 0;
};
Agg g_agg;

void log_window(Agg& a, uint64_t now, double hz, int k) {
    const double secs = static_cast<double>(now - a.t0) * 1e-9;
    uint64_t w[3];
    for (int i = 0; i < 3; ++i) {
        const uint64_t c = g_waits[i].load(std::memory_order_relaxed);
        w[i] = c - a.waits_at_t0[i];
        a.waits_at_t0[i] = c;
    }
    const char* src = g_waiter.load() ? "present wait" : "vkQueuePresentKHR calls, no VK_KHR_present_wait: queue intervals, not the screen";
    if (hz > 0) {
        NXLOG_I(kTag, "present timing: %.1f s, %llu frames (%s, %.2f Hz, %d refreshes a frame): shown for 1/2/3/4+ refreshes %llu/%llu/%llu/%llu, exactly %d: %.1f %%; boundary waits per game frame 0/1/2+ %llu/%llu/%llu",
                secs, static_cast<unsigned long long>(a.frames), src, hz, k, static_cast<unsigned long long>(a.shown_n[0]), static_cast<unsigned long long>(a.shown_n[1]),
                static_cast<unsigned long long>(a.shown_n[2]), static_cast<unsigned long long>(a.shown_n[3]), k, a.frames ? 100.0 * static_cast<double>(a.exact) / static_cast<double>(a.frames) : 0.0,
                static_cast<unsigned long long>(w[0]), static_cast<unsigned long long>(w[1]), static_cast<unsigned long long>(w[2]));
    } else {
        const double p50 = pct(a.dur, 0.5), p99 = pct(a.dur, 0.99), mx = a.dur.empty() ? 0.0 : a.dur.back();
        NXLOG_I(kTag, "present timing: %.1f s, %llu frames (%s, refresh rate not known: no fixed-refresh panel with vsync): frame on screen p50 %.2f p99 %.2f max %.2f ms; boundary waits per game frame 0/1/2+ %llu/%llu/%llu",
                secs, static_cast<unsigned long long>(a.frames), src, p50, p99, mx, static_cast<unsigned long long>(w[0]), static_cast<unsigned long long>(w[1]),
                static_cast<unsigned long long>(w[2]));
    }
    char phase[96] = "n/a";
    if (a.phase_n && hz > 0) {
        const double refresh = 1000.0 / hz;
        const double c = a.phase_c / static_cast<double>(a.phase_n), s = a.phase_s / static_cast<double>(a.phase_n);
        double ang = std::atan2(s, c);
        if (ang < 0) ang += 2 * kPi;
        const double rbar = std::min(1.0, std::sqrt(c * c + s * s));
        const double spread = rbar > 1e-9 ? refresh / (2 * kPi) * std::sqrt(-2.0 * std::log(rbar)) : refresh;
        std::snprintf(phase, sizeof phase, "mean %.2f spread %.2f ms", ang / (2 * kPi) * refresh, spread);
    }
    const double qmax = a.qblock.empty() ? 0.0 : (std::sort(a.qblock.begin(), a.qblock.end()), static_cast<double>(a.qblock.back()));
    char lock[200] = "";
    if (a.lock_active)
        std::snprintf(lock, sizeof lock, "; vblank lock: budget %.2f + guard %.2f ms, release->shown target %.2f ms, grid moved %+.2f / %+.2f ms, %llu frames late",
                      a.last_budget * 1e-6, a.guard_ns * 1e-6, a.last_target * 1e-6, a.shift_pos * 1e-6, a.shift_neg * 1e-6, static_cast<unsigned long long>(a.lock_late_window));
    NXLOG_I(kTag, "present timing ms p50/p99: release->call %.2f/%.2f, call->replay done %.2f/%.2f, replay->queue present %.2f/%.2f, queue present blocked %.2f/%.2f (max %.2f), "
                  "release->GPU done %.2f/%.2f, GPU done->shown %.2f/%.2f, release->shown %.2f/%.2f; release after vblank %s; GPU done before its target vblank p1 %.2f p50 %.2f (%llu frames late); "
                  "held %llu frames, %.2f ms avg%s",
            pct(a.rel_call, 0.5), pct(a.rel_call, 0.99), pct(a.call_rep, 0.5), pct(a.call_rep, 0.99), pct(a.rep_q, 0.5), pct(a.rep_q, 0.99), pct(a.qblock, 0.5),
            pct(a.qblock, 0.99), qmax, pct(a.rel_gpu, 0.5), pct(a.rel_gpu, 0.99), pct(a.gpu_shown, 0.5), pct(a.gpu_shown, 0.99), pct(a.rel_shown, 0.5),
            pct(a.rel_shown, 0.99), phase, pct(a.margin, 0.01), pct(a.margin, 0.5), static_cast<unsigned long long>(a.late), static_cast<unsigned long long>(a.held),
            a.held ? a.held_ms / static_cast<double>(a.held) : 0.0, lock);
}

void reset_window(Agg& a, uint64_t now) {
    a.t0 = now;
    a.frames = a.exact = a.late = a.held = a.phase_n = a.lock_late_window = 0;
    for (uint64_t& n : a.shown_n) n = 0;
    a.held_ms = a.phase_c = a.phase_s = a.shift_pos = a.shift_neg = 0;
    for (auto* v : {&a.dur, &a.rel_call, &a.call_rep, &a.rep_q, &a.qblock, &a.rel_gpu, &a.gpu_shown, &a.rel_shown, &a.margin}) v->clear();
}

// vblank_lock: one frame p shown at s_p (the previous frame at s_prev, 0 = unknown) moves the limiter's grid toward release -> shown = budget.
void lock_frame(Agg& a, FrameLimiter* lim, const Rec& r, uint64_t p, uint64_t s_p, uint64_t s_prev, int k, double refresh_ns) {
    const uint64_t rel = r.release.load(std::memory_order_relaxed), gpu = r.gpu.load(std::memory_order_relaxed);
    if (!rel || !gpu || gpu < rel || s_p < rel) return;
    a.budget[a.budget_pos] = static_cast<float>(gpu - rel);
    a.budget_pos = (a.budget_pos + 1) % kBudgetFrames;
    a.budget_n = std::min(a.budget_n + 1, kBudgetFrames);
    // late: the frame before stayed k + 1 refreshes or more although this frame's release came on time
    if (s_prev) {
        const Rec* rp = rec(p - 1);
        const uint64_t rel_prev = rp ? rp->release.load(std::memory_order_relaxed) : 0;
        const double slot = (static_cast<double>(k) + 0.5) * refresh_ns;
        if (rel_prev && rel > rel_prev && static_cast<double>(s_p - s_prev) >= slot && static_cast<double>(rel - rel_prev) < slot) {
            ++a.lock_late;
            ++a.lock_late_window;
        }
    }
    if (++a.lock_frames % kBudgetFrames == 0) {
        const double rate = static_cast<double>(a.lock_late) / kBudgetFrames;
        if (rate > 0.015) a.guard_ns = std::min(a.guard_ns + 1000000.0, refresh_ns);
        else if (a.lock_late == 0) a.guard_ns = std::max(a.guard_ns - 250000.0, 0.0);
        a.lock_late = 0;
    }
    if (a.budget_n < 32) return;
    std::vector<float> v(a.budget, a.budget + a.budget_n);
    a.last_budget = pct(v, 0.99);
    a.last_target = a.last_budget + a.guard_ns;
    const double e = static_cast<double>(s_p - rel) - a.last_target;     // > 0: the release can come later
    const double step = std::clamp(e * kShiftGain, -kMaxShiftNs, kMaxShiftNs);
    lim->shift_grid(step);
    (step > 0 ? a.shift_pos : a.shift_neg) += step;
}

// Frame p was shown at s_p and the next one at s_next.
void frame_done(Agg& a, uint64_t p, uint64_t s_p, uint64_t s_next, uint64_t s_prev, uint64_t now) {
    FrameLimiter* lim = g_lim.load(std::memory_order_acquire);
    const double hz = lim ? lim->refresh_hz() : 0.0;
    const int k = lim ? lim->grid_refreshes() : 0;
    const double refresh_ns = hz > 0 ? 1e9 / hz : 0.0;
    if (!a.t0) {
        a.t0 = now;
        for (int i = 0; i < 3; ++i) a.waits_at_t0[i] = g_waits[i].load(std::memory_order_relaxed);
    }
    ++a.frames;
    const double dur = static_cast<double>(s_next - s_p);
    if (refresh_ns > 0) {
        const long n = std::lround(dur / refresh_ns);
        ++a.shown_n[n <= 1 ? 0 : n >= 4 ? 3 : n - 1];
        if (n == k) ++a.exact;
    }
    a.dur.push_back(static_cast<float>(dur * 1e-6));
    if (const Rec* r = rec(p)) {
        const uint64_t rel = r->release.load(std::memory_order_relaxed), call = r->call.load(std::memory_order_relaxed),
                       rep = r->replay.load(std::memory_order_relaxed), q0 = r->q0.load(std::memory_order_relaxed), q1 = r->q1.load(std::memory_order_relaxed),
                       gpu = r->gpu.load(std::memory_order_relaxed), held = r->held.load(std::memory_order_relaxed);
        if (rel && call >= rel) a.rel_call.push_back(static_cast<float>(ms_between(rel, call)));
        if (call && rep >= call) a.call_rep.push_back(static_cast<float>(ms_between(call, rep)));
        if (rep && q0 >= rep) a.rep_q.push_back(static_cast<float>(ms_between(rep, q0)));
        if (q0 && q1 >= q0) a.qblock.push_back(static_cast<float>(ms_between(q0, q1)));
        if (rel && gpu >= rel) a.rel_gpu.push_back(static_cast<float>(ms_between(rel, gpu)));
        if (gpu && s_p >= gpu) a.gpu_shown.push_back(static_cast<float>(ms_between(gpu, s_p)));
        if (rel && s_p >= rel) a.rel_shown.push_back(static_cast<float>(ms_between(rel, s_p)));
        if (held) { ++a.held; a.held_ms += ms(held); }
        if (rel && refresh_ns > 0) {      // the release's phase after the vblank the frame was shown on (a vblank of the measured grid)
            const double x = (static_cast<double>(rel) - static_cast<double>(s_p)) / refresh_ns;
            const double ang = 2 * kPi * (x - std::floor(x));
            a.phase_c += std::cos(ang);
            a.phase_s += std::sin(ang);
            ++a.phase_n;
        }
        if (gpu && s_prev && refresh_ns > 0) {    // the vblank the frame was due on: the previous frame's + k refreshes (1 without a cap)
            const double target = static_cast<double>(s_prev) + std::max(k, 1) * refresh_ns;
            const double m = (target - static_cast<double>(gpu)) * 1e-6;
            a.margin.push_back(static_cast<float>(m));
            if (static_cast<double>(s_p) > target + 0.5 * refresh_ns) ++a.late;
        }
        const bool lock = cfg().lock && lim && g_waiter.load() && k >= 2 && refresh_ns > 0 && !frame_limiter_unpaced();
        if (lock != a.lock_active && a.lock_lines < 16) {
            ++a.lock_lines;
            if (lock) NXLOG_I(kTag, "vblank lock: on (%.2f Hz, a frame every %d refreshes): presents held to their slot, the limiter's grid follows the vblank", hz, k);
            else NXLOG_I(kTag, "vblank lock: off (%s)", !g_waiter.load() ? "no present wait" : k < 2 ? "no frame cap of refresh / k on a fixed-refresh panel" : "unpaced frames");
        }
        a.lock_active = lock;
        if (lock) lock_frame(a, lim, *r, p, s_p, s_prev, k, refresh_ns);
    }
    if (now - a.t0 >= kLogNs) {     // (vblank_lock alone keeps no log, only the window's samples are dropped)
        if (cfg().stats) log_window(a, now, hz, k);
        reset_window(a, now);
    }
}

// Frame f reached the screen (or, without the present waiter, the queue) at s.
void complete(uint64_t f, uint64_t s) {
    std::lock_guard<std::mutex> lk(g_mu);
    Agg& a = g_agg;
    if (a.prev_f && f == a.prev_f + 1 && s >= a.prev_s)
        frame_done(a, a.prev_f, a.prev_s, s, a.pprev_f && a.pprev_f + 1 == a.prev_f ? a.pprev_s : 0, FrameLimiter::now_ns());
    a.pprev_f = a.prev_f;
    a.pprev_s = a.prev_s;
    a.prev_f = f;
    a.prev_s = s;
}
}  // namespace

bool present_timing_on() { return cfg().stats || cfg().lock; }
bool present_wait_wanted() { return present_timing_on(); }
void present_timing_set_limiter(FrameLimiter* lim) { g_lim.store(lim, std::memory_order_release); }

void present_timing_set_waiter(bool runs) {
    static bool logged = false;
    g_waiter.store(runs);
    if (!present_timing_on() || logged) return;
    logged = true;
    NXLOG_I(kTag, "present timing: %s%s", runs ? "on, the screen times come from VK_KHR_present_wait" : "on, VK_KHR_present_wait not available: frame intervals at vkQueuePresentKHR only",
            cfg().lock ? (runs ? "; [latency] vblank_lock wanted" : "; [latency] vblank_lock needs it: off") : "");
}

uint64_t present_timing_game_present(uint32_t boundary_waits) {
    g_waits[std::min<uint32_t>(boundary_waits, 2)].fetch_add(1, std::memory_order_relaxed);
    if (!present_timing_on()) return 0;
    const uint64_t f = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    Rec& r = g_ring[f % kRing];
    r.frame.store(0, std::memory_order_relaxed);
    r.call.store(FrameLimiter::now_ns(), std::memory_order_relaxed);
    r.release.store(g_pending_release.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    for (auto* x : {&r.replay, &r.held, &r.q0, &r.q1, &r.gpu, &r.shown}) x->store(0, std::memory_order_relaxed);
    r.frame.store(f, std::memory_order_release);
    return f;
}

void present_timing_release(uint64_t t_ns) {
    if (present_timing_on()) g_pending_release.store(t_ns, std::memory_order_relaxed);
}

void present_timing_render_begin(uint64_t frame) {
    t_render_frame = frame;
    if (Rec* r = rec(frame)) r->replay.store(FrameLimiter::now_ns(), std::memory_order_relaxed);
}

uint64_t present_timing_render_frame() { return t_render_frame; }

void present_timing_present_release(uint64_t t_ns) {
    if (Rec* r = rec(t_render_frame)) r->release.store(t_ns, std::memory_order_relaxed);
}

void present_timing_queue_present(uint64_t frame, uint64_t held_ns, uint64_t t0, uint64_t t1) {
    Rec* r = rec(frame);
    if (!r) return;
    r->held.store(held_ns, std::memory_order_relaxed);
    r->q0.store(t0, std::memory_order_relaxed);
    r->q1.store(t1, std::memory_order_relaxed);
    if (!g_waiter.load(std::memory_order_relaxed)) complete(frame, t1);
}

void present_timing_gpu_done(uint64_t frame, uint64_t t_ns) {
    if (Rec* r = rec(frame)) r->gpu.store(t_ns, std::memory_order_relaxed);
}

void present_timing_shown(uint64_t frame, uint64_t t_ns) {
    Rec* r = rec(frame);
    if (!r) return;
    r->shown.store(t_ns, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_shown_mu);
        if (frame > g_shown_frame) {
            g_shown_frame = frame;
            g_shown_ns = t_ns;
        }
    }
    complete(frame, t_ns);
}

uint64_t present_timing_hold_until(uint64_t frame, bool fifo) {
    if (!cfg().lock || !fifo || !frame || !g_waiter.load(std::memory_order_relaxed) || frame_limiter_unpaced()) return 0;
    FrameLimiter* lim = g_lim.load(std::memory_order_acquire);
    if (!lim) return 0;
    const int k = lim->grid_refreshes();
    const double hz = lim->refresh_hz();
    if (k < 2 || !(hz > 0)) return 0;
    uint64_t j, s_j;
    {
        std::lock_guard<std::mutex> lk(g_shown_mu);
        j = g_shown_frame;
        s_j = g_shown_ns;
    }
    if (!j || frame <= j || frame - j > 4) return 0;      // nothing recent on the screen to count from
    const double refresh_ns = 1e9 / hz;
    // frame j went out on the vblank at s_j; frame i is due k refreshes after frame i-1: presented after the vblank before that, FIFO cannot show it earlier
    const double t = static_cast<double>(s_j) + (static_cast<double>((frame - j) * static_cast<uint64_t>(k)) - 1.0) * refresh_ns + kHoldAfterVblankNs;
    const double now = static_cast<double>(FrameLimiter::now_ns());
    if (t <= now || t - now > k * refresh_ns) return 0;
    return static_cast<uint64_t>(t);
}

}  // namespace nxrt::extras
