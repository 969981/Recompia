// SPDX-License-Identifier: GPL-3.0-or-later
// Performance report: see nxrt/perf_report.h (what it logs) and perf_report.h (the hitch monitor's hooks).
//
// Frames. Every presented frame is one of: capped (a pacer held it back since the previous frame: note_pacer_wait moved), unfocused (the
// window had no focus), long (over kLongMs: a hitch, the hitch lines cover it) or free. Only free frames say what bounds the frame rate:
// a capped frame waits on purpose (the player's cap, or the game's own 60 fps ceiling in menus and events), an unfocused one may be
// throttled by the OS. Counters are cumulative relaxed atomics written by the presenting thread; the monitor subtracts its last snapshot,
// so nothing is ever reset under a writer.
//
// Per free frame: its interval (histogram of kBinMs bins for the median and the session percentiles), the render thread's replay time
// without its GPU slot waits (CPU work of the render thread), the render thread's GPU slot waits and swapchain acquire, and the game's
// presenting thread's waits (present back pressure, GPU sync waits, waiting system calls): the game thread's time is the interval minus
// those. GPU busy time (discrete GPUs, note_gpu_busy) arrives about three frames late, tagged with the frame it belongs to; a ring of the
// last frames' classes lets it count only for free frames.
//
// Verdict of a period, from the means over its free frames (F = interval), in this order: GPU busy, render-thread CPU, game-thread time,
// the first that reaches kBoundShare of F; without GPU times, render-thread GPU waits of kGpuWaitShare of F also say GPU-bound; anything
// else is "capped" (the display paces: vsync, a full present queue).
#include "perf_report.h"
#include "perf_system.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/platform_facts.h"
#include "nxrt/qpc.h"

namespace nxrt {
namespace {

constexpr const char* kTag = "perf";
constexpr double kBinMs = 0.25;
constexpr size_t kBins = 1000;              // 0 .. 250 ms
constexpr double kLongMs = 250.0;           // longer frames are hitches, not a frame rate
constexpr size_t kRing = 256;               // frames whose class GPU times can still look up (GPU times come ~3 frames late)
constexpr double kBoundShare = 0.80;
constexpr double kGpuWaitShare = 0.20;
constexpr double kLoadShare = 0.70;         // the kernel's GPU busy share (perf_gpu_load, no GPU timing): a sampled utilisation, not a per-frame time
constexpr double kSampleStreakS = 30.0;     // GPU-bound under kSampleFps this long before a pass sample
constexpr double kSampleFps = 55.0;
constexpr double kSampleGapS = 120.0;
constexpr int kSampleMax = 5;
constexpr size_t kSampleRows = 10;

enum Cls : uint64_t { kFree = 0, kCapped, kUnfocused, kLong, kClasses };
enum Verdict : int { kGpu = 0, kRender, kGame, kPaced, kVerdicts };
const char* const kVerdictName[kVerdicts] = {"GPU-bound", "CPU-bound (render thread)", "CPU-bound (game)", "capped"};

// Cumulative counters (presenting thread writes, monitor reads) and their snapshot (plain copy).
template <typename T>
struct CountersT {
    T frames[kClasses] = {};
    T free_ticks = 0, free_replay = 0, free_gpuwait = 0, free_acquire = 0, free_gamewait = 0;
    T gpu_free_ns = 0;                      // GPU busy time of free frames (render thread writes, at slot reuse)
};

struct Report {
    std::atomic<bool> focused{true};
    std::atomic<uint64_t> pacer_waits{0};
    std::atomic<uint64_t> frame_n{0};
    std::atomic<uint64_t> ring[kRing] = {};           // (frame << 2) | class
    std::atomic<bool> gpu_timing{false};
    std::atomic<bool> sample_wanted{false};
    std::atomic<std::string (*)()> settings_fn{nullptr};
    std::atomic<bool> system_logged{false}, ended{false};
    perf::GpuFacts gpu;                               // set once by set_gpu (before system_logged); read by the monitor
    std::atomic<bool> has_gpu{false};
    bool link_logged = false;                         // monitor: the PCIe link under load was logged
    CountersT<std::atomic<uint64_t>> c;
    std::atomic<uint32_t> hist[kBins] = {};

    // presenting thread only
    uint64_t prev_t = 0, prev_waits = 0;
    uint64_t prev_ph[4] = {};

    // monitor side, under mu
    std::mutex mu;
    CountersT<uint64_t> snap;
    uint32_t snap_hist[kBins] = {};
    std::string settings_text;
    uint64_t verdict_frames[kVerdicts] = {};
    double streak_s = 0, last_sample_s = -1e9;
    int samples = 0;
};
Report& rep() {
    static Report* r = new Report;    // never destroyed: the presenting thread may still tick while statics go
    return *r;
}

double ms_of(uint64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpc::freq()); }

CountersT<uint64_t> read(const CountersT<std::atomic<uint64_t>>& a) {
    CountersT<uint64_t> s;
    for (size_t i = 0; i < kClasses; i++) s.frames[i] = a.frames[i].load(std::memory_order_relaxed);
    s.free_ticks = a.free_ticks.load(std::memory_order_relaxed);
    s.free_replay = a.free_replay.load(std::memory_order_relaxed);
    s.free_gpuwait = a.free_gpuwait.load(std::memory_order_relaxed);
    s.free_acquire = a.free_acquire.load(std::memory_order_relaxed);
    s.free_gamewait = a.free_gamewait.load(std::memory_order_relaxed);
    s.gpu_free_ns = a.gpu_free_ns.load(std::memory_order_relaxed);
    return s;
}

CountersT<uint64_t> minus(const CountersT<uint64_t>& a, const CountersT<uint64_t>& b) {
    CountersT<uint64_t> d;
    for (size_t i = 0; i < kClasses; i++) d.frames[i] = a.frames[i] - b.frames[i];
    d.free_ticks = a.free_ticks - b.free_ticks;
    d.free_replay = a.free_replay - b.free_replay;
    d.free_gpuwait = a.free_gpuwait - b.free_gpuwait;
    d.free_acquire = a.free_acquire - b.free_acquire;
    d.free_gamewait = a.free_gamewait - b.free_gamewait;
    d.gpu_free_ns = a.gpu_free_ns - b.gpu_free_ns;
    return d;
}

// Frame time (ms) at quantile q of a histogram with n entries (the middle of the bin).
double quantile_ms(const uint32_t* h, uint64_t n, double q) {
    if (!n) return 0;
    const uint64_t want = std::max<uint64_t>(1, static_cast<uint64_t>(q * static_cast<double>(n) + 0.5));
    uint64_t acc = 0;
    for (size_t i = 0; i < kBins; i++) {
        acc += h[i];
        if (acc >= want) return (static_cast<double>(i) + 0.5) * kBinMs;
    }
    return kLongMs;
}

double fps_of(double ms) { return ms > 0 ? 1000.0 / ms : 0; }

struct Means {
    uint64_t free = 0;
    double frame_ms = 0, fps = 0, median_ms = 0;
    double gpu_ms = -1;                     // -1: no GPU times
    double render_ms = 0, game_ms = 0, gpuwait_ms = 0, acquire_ms = 0;
    Verdict verdict = kPaced;
};

Means means_of(const CountersT<uint64_t>& d, const uint32_t* hist, bool gpu, double load) {   // load: kernel GPU busy 0..1, < 0 unknown
    Means m;
    m.free = d.frames[kFree];
    if (!m.free) return m;
    const double n = static_cast<double>(m.free);
    m.frame_ms = ms_of(d.free_ticks) / n;
    m.fps = fps_of(m.frame_ms);
    m.median_ms = quantile_ms(hist, m.free, 0.5);
    const uint64_t replay_cpu = d.free_replay > d.free_gpuwait ? d.free_replay - d.free_gpuwait : 0;
    m.render_ms = ms_of(replay_cpu) / n;
    m.gpuwait_ms = ms_of(d.free_gpuwait) / n;
    m.acquire_ms = ms_of(d.free_acquire) / n;
    m.game_ms = std::max(0.0, m.frame_ms - ms_of(d.free_gamewait) / n);
    if (gpu) m.gpu_ms = static_cast<double>(d.gpu_free_ns) / 1e6 / n;
    // measured shares first: GPU busy and the render thread's CPU are timed directly; the game thread's time is what is left of the frame
    // after its timed waits, so any wait nothing times (a low-latency sleep) reads as game time and must not outrank a measured share
    if (gpu && m.gpu_ms >= kBoundShare * m.frame_ms) m.verdict = kGpu;
    else if (m.render_ms >= kBoundShare * m.frame_ms) m.verdict = kRender;
    else if (m.game_ms >= kBoundShare * m.frame_ms) m.verdict = kGame;
    else if (!gpu && load >= kLoadShare) m.verdict = kGpu;
    else if (!gpu && m.gpuwait_ms >= kGpuWaitShare * m.frame_ms) m.verdict = kGpu;
    else m.verdict = kPaced;
    return m;
}

// The settings line, when the play mode's description changed (monitor thread, under mu).
void log_settings(Report& r) {
    auto fn = r.settings_fn.load(std::memory_order_acquire);
    if (!fn) return;
    std::string text = fn();
    if (text.empty() || text == r.settings_text) return;
    r.settings_text = std::move(text);
    NXLOG_I(kTag, "settings: %s", r.settings_text.c_str());
}

}  // namespace

namespace perf {

bool enabled() {
    static const bool on = config_get_bool("diag", "perf_report", true);
    return on;
}

void note_focus(bool focused) { rep().focused.store(focused, std::memory_order_relaxed); }
void note_pacer_wait() { rep().pacer_waits.fetch_add(1, std::memory_order_relaxed); }

void set_gpu(const GpuFacts& gpu) {
    Report& r = rep();
    if (!enabled() || r.system_logged.exchange(true)) return;
    r.gpu = gpu;
    r.has_gpu.store(true, std::memory_order_release);
    diag::perf_log_system(gpu);
}

void set_settings_source(std::string (*fn)()) { rep().settings_fn.store(fn, std::memory_order_release); }
void set_gpu_timing(bool on) { rep().gpu_timing.store(on && enabled(), std::memory_order_relaxed); }
uint64_t frame_index() { return rep().frame_n.load(std::memory_order_relaxed); }

void note_gpu_busy(uint64_t tag, uint64_t ns) {
    Report& r = rep();
    const uint64_t frame = tag + 1;                  // recorded after frame `tag` was counted: presented as the next one
    const uint64_t e = r.ring[frame % kRing].load(std::memory_order_relaxed);
    if ((e >> 2) == frame && (e & 3) == kFree) r.c.gpu_free_ns.fetch_add(ns, std::memory_order_relaxed);
}

bool pass_sample_take() {
    Report& r = rep();
    return r.sample_wanted.load(std::memory_order_relaxed) && r.sample_wanted.exchange(false);
}

void pass_sample_done(std::vector<PassTime> passes, uint32_t submissions) {
    Report& r = rep();
    uint64_t total = 0, draws = 0;
    for (const PassTime& p : passes) {
        total += p.ns;
        draws += p.draws;
    }
    std::sort(passes.begin(), passes.end(), [](const PassTime& a, const PassTime& b) { return a.ns > b.ns; });
    int n;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        n = r.samples;
    }
    std::string rows;
    char b[256];
    for (size_t i = 0; i < passes.size() && i < kSampleRows; i++) {
        const PassTime& p = passes[i];
        std::snprintf(b, sizeof b, "%s%s %.1f%% (%.2f ms, %llu draws)", i ? "; " : "", p.label.c_str(), total ? 100.0 * static_cast<double>(p.ns) / static_cast<double>(total) : 0.0,
                      static_cast<double>(p.ns) / 1e6, static_cast<unsigned long long>(p.draws));
        rows += b;
    }
    NXLOG_I(kTag, "GPU pass sample %d of at most %d (one frame, %u submissions, %.2f ms GPU time in %zu passes, %llu draws), largest first: %s", n, kSampleMax,
            submissions, static_cast<double>(total) / 1e6, passes.size(), static_cast<unsigned long long>(draws), rows.empty() ? "no passes timed" : rows.c_str());
}

void session_end() {
    if (!enabled()) return;
    Report& r = rep();
    if (r.ended.exchange(true)) return;
    std::lock_guard<std::mutex> lk(r.mu);
    const CountersT<uint64_t> cur = read(r.c);
    // the periods the monitor did not judge yet (the last one): judged here
    uint32_t all[kBins];
    {
        uint32_t h[kBins];
        for (size_t i = 0; i < kBins; i++) {
            all[i] = r.hist[i].load(std::memory_order_relaxed);
            h[i] = all[i] - r.snap_hist[i];
        }
        const Means m = means_of(minus(cur, r.snap), h, r.gpu_timing.load(std::memory_order_relaxed), -1.0);   // session end: no load sample
        if (m.free) r.verdict_frames[m.verdict] += m.free;
    }
    const uint64_t nfree = cur.frames[kFree];
    uint64_t nall = 0;
    for (uint64_t f : cur.frames) nall += f;
    if (!nall) {
        NXLOG_I(kTag, "session summary: no frame was presented");
        return;
    }
    std::string shares;
    uint64_t judged = 0;
    for (uint64_t v : r.verdict_frames) judged += v;
    char b[160];
    for (int v = 0; v < kVerdicts; v++) {
        std::snprintf(b, sizeof b, "%s%s %.0f%%", v ? ", " : "", kVerdictName[v], judged ? 100.0 * static_cast<double>(r.verdict_frames[v]) / static_cast<double>(judged) : 0.0);
        shares += b;
    }
    const double free_ms = ms_of(cur.free_ticks);
    const double median = quantile_ms(all, nfree, 0.5), p99 = quantile_ms(all, nfree, 0.99), p999 = quantile_ms(all, nfree, 0.999);
    NXLOG_I(kTag, "session summary: %llu free frames in %.0f s (of %llu presented: %llu capped, %llu unfocused, %llu over %.0f ms): mean %.1f fps, median %.1f ms "
            "(%.1f fps), 1%% low %.1f fps, 0.1%% low %.1f fps; verdicts by free frames: %s; GPU pass samples %d",
            static_cast<unsigned long long>(nfree), free_ms / 1000.0, static_cast<unsigned long long>(nall), static_cast<unsigned long long>(cur.frames[kCapped]),
            static_cast<unsigned long long>(cur.frames[kUnfocused]), static_cast<unsigned long long>(cur.frames[kLong]), kLongMs,
            nfree && free_ms > 0 ? static_cast<double>(nfree) * 1000.0 / free_ms : 0.0, median, fps_of(median), fps_of(p99), fps_of(p999), shares.c_str(), r.samples);
    log_flush();
}

}  // namespace perf

namespace diag {

void perf_frame(uint64_t t) {
    if (!perf::enabled()) {
        // a phone still counts frames: the back end's GPU time per frame (the game's dynamic resolution) is summed by frame_index()
        if constexpr (facts::kPhone) rep().frame_n.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Report& r = rep();
    const uint64_t ph[4] = {hitch_phase_total(kHpRenderReplay), hitch_phase_total(kHpRenderGpuWait), hitch_phase_total(kHpRenderAcquire),
                            hitch_phase_total(kHpGameBackpressure) + hitch_phase_total(kHpGameSyncWait) + hitch_phase_total(kHpGameWait)};
    const uint64_t waits = r.pacer_waits.load(std::memory_order_relaxed);
    const uint64_t n = r.frame_n.load(std::memory_order_relaxed) + 1;
    Cls cls = kLong;      // the first frame has no interval: counted nowhere
    if (r.prev_t && t > r.prev_t) {
        const uint64_t dt = t - r.prev_t;
        const double ms = ms_of(dt);
        cls = waits != r.prev_waits ? kCapped : !r.focused.load(std::memory_order_relaxed) ? kUnfocused : ms > kLongMs ? kLong : kFree;
        r.c.frames[cls].fetch_add(1, std::memory_order_relaxed);
        if (cls == kFree) {
            r.hist[std::min(kBins - 1, static_cast<size_t>(ms / kBinMs))].fetch_add(1, std::memory_order_relaxed);
            r.c.free_ticks.fetch_add(dt, std::memory_order_relaxed);
            r.c.free_replay.fetch_add(ph[0] - r.prev_ph[0], std::memory_order_relaxed);
            r.c.free_gpuwait.fetch_add(ph[1] - r.prev_ph[1], std::memory_order_relaxed);
            r.c.free_acquire.fetch_add(ph[2] - r.prev_ph[2], std::memory_order_relaxed);
            r.c.free_gamewait.fetch_add(std::min(dt, ph[3] - r.prev_ph[3]), std::memory_order_relaxed);
        }
    }
    r.ring[n % kRing].store(n << 2 | cls, std::memory_order_relaxed);
    r.frame_n.store(n, std::memory_order_relaxed);
    r.prev_t = t;
    r.prev_waits = waits;
    for (int i = 0; i < 4; i++) r.prev_ph[i] = ph[i];
}

void perf_tick(double period_s) {
    if (!perf::enabled()) return;
    Report& r = rep();
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.ended.load()) return;
    log_settings(r);
    const CountersT<uint64_t> cur = read(r.c);
    const CountersT<uint64_t> d = minus(cur, r.snap);
    uint32_t h[kBins];
    for (size_t i = 0; i < kBins; i++) {
        const uint32_t v = r.hist[i].load(std::memory_order_relaxed);
        h[i] = v - r.snap_hist[i];
        r.snap_hist[i] = v;
    }
    r.snap = cur;
    const bool gpu = r.gpu_timing.load(std::memory_order_relaxed);
    diag::GpuLoad load;
    const bool have_load = !gpu && diag::perf_gpu_load(&load);
    const Means m = means_of(d, h, gpu, have_load ? load.busy : -1.0);
    uint64_t total = 0;
    for (uint64_t f : d.frames) total += f;
    if (!total) return;     // nothing presented: the hitch line says so
    char gpu_txt[48];
    if (gpu) std::snprintf(gpu_txt, sizeof gpu_txt, "%.1f ms", m.gpu_ms);
    else if (have_load) std::snprintf(gpu_txt, sizeof gpu_txt, "%.0f%% at %d of %d MHz (kernel)", load.busy * 100.0, load.mhz, load.max_mhz);
    else std::snprintf(gpu_txt, sizeof gpu_txt, "n/a");
    if (m.free) {
        r.verdict_frames[m.verdict] += m.free;
        NXLOG_I(kTag, "%.1f s: %llu free frames: %.1f fps, median %.1f ms; per frame: GPU busy %s, render thread %.1f ms (+ GPU waits %.1f, acquire %.1f), game thread %.1f ms -> %s; "
                "not judged: %llu capped, %llu unfocused, %llu over %.0f ms",
                period_s, static_cast<unsigned long long>(m.free), m.fps, m.median_ms, gpu_txt, m.render_ms, m.gpuwait_ms, m.acquire_ms, m.game_ms,
                kVerdictName[m.verdict], static_cast<unsigned long long>(d.frames[kCapped]), static_cast<unsigned long long>(d.frames[kUnfocused]),
                static_cast<unsigned long long>(d.frames[kLong]), kLongMs);
    } else {
        NXLOG_I(kTag, "%.1f s: no free frames (%llu capped, %llu unfocused, %llu over %.0f ms)", period_s, static_cast<unsigned long long>(d.frames[kCapped]),
                static_cast<unsigned long long>(d.frames[kUnfocused]), static_cast<unsigned long long>(d.frames[kLong]), kLongMs);
    }
    // a GPU at rest may lower its PCIe link: read it again once the first free frames kept it busy
    if (m.free && !r.link_logged && r.has_gpu.load(std::memory_order_acquire) && r.gpu.discrete) {
        r.link_logged = true;
        NXLOG_I(kTag, "PCIe link under load: %s", diag::perf_pcie_link(r.gpu).c_str());
    }
    // pass sample trigger (discrete GPUs: only they hand in GPU times): free frames GPU-bound under kSampleFps for kSampleStreakS
    if (gpu && m.free && m.verdict == kGpu && m.fps < kSampleFps) r.streak_s += period_s;
    else r.streak_s = 0;
    const double now = qpc::seconds();
    if (r.streak_s >= kSampleStreakS && r.samples < kSampleMax && now - r.last_sample_s >= kSampleGapS) {
        r.streak_s = 0;
        r.last_sample_s = now;
        r.samples++;
        r.sample_wanted.store(true);
    }
}

}  // namespace diag
}  // namespace nxrt
