// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the job workers' idle spin governor (phones).
//
// A job worker of the game scans for work while its "work signalled" word has a bit of the classes it serves, and claims a queue
// entry with a compare-and-swap. On phones a class bit sometimes keeps coming back with nothing that worker can claim: the worker then
// re-checks it for the rest of the session without a single SVC, one whole core (about +30 % process CPU) in some long sessions on a
// Tensor G2 phone.
//
// Mid hooks at the scan's head, at a class's re-check and at the waiting-job bitmap's pass (kScanHead, kClassAgain, kBitmapAgain)
// only change the timing of a loop that is already stuck, as a preempting OS would: while the signal bits a worker serves stay the same
// (a claim clears one, new work sets one) from pass to pass for kStuckNs and kMinPasses, the thread yields at every pass, and after
// kSleepAfter yields it sleeps kSleepUs per pass until its bits change. A worker that blocked or left the scan starts over after a gap
// of kGapNs; the governor's own waits are not gaps. Guest state is never written.
#include "core/pkp_job_spin.h"
#include "core/pkp_guest.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "nxrt/log.h"
#include "nxrt/platform_facts.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp {

namespace {

using guest::rd;

constexpr uint64_t kScanHead = 0x1fef914;   // mid: the worker's scan head, x25 = the job system, w27 = the classes it serves
constexpr uint64_t kClassAgain = 0x1fefa20; // mid: a class's re-check after its queue was found empty (the same registers)
constexpr uint64_t kBitmapAgain = 0x1fefb4c; // mid: the bitmap class's pass, which loops back here while its bit is set again
constexpr uint64_t kSignal = 0x2c0;         // the job system's "work signalled" word
constexpr uint64_t kGapNs = 50000000, kStuckNs = 2000000;   // a pass after a 50 ms gap starts over; 2 ms of the same bits is stuck
constexpr uint32_t kMinPasses = 64, kSleepAfter = 256;
constexpr auto kSleepUs = std::chrono::microseconds(500);

// Per guest thread, keyed by its Ctx (stable for the thread's life). note: 64 fixed slots; a full table leaves a thread ungoverned.
struct Spin {
    std::atomic<uintptr_t> key{0};
    uint64_t last_ns = 0, since = 0;
    uint32_t last_signal = 0;
    uint32_t fast = 0, yields = 0;
    bool logged = false;
};
Spin g_spin[64];
std::atomic<uint64_t> g_sleeps{0};

Spin* slot_of(::Ctx* c) {
    const uintptr_t k = reinterpret_cast<uintptr_t>(c);
    const size_t h = (k >> 4) * 0x9E3779B97F4A7C15ull >> 58;
    for (size_t i = 0; i < 64; ++i) {
        Spin& s = g_spin[(h + i) & 63];
        uintptr_t cur = s.key.load(std::memory_order_acquire);
        if (cur == k) return &s;
        if (cur == 0 && s.key.compare_exchange_strong(cur, k, std::memory_order_acq_rel)) return &s;
        if (cur == k) return &s;
    }
    return nullptr;
}

int hook_scan(::Ctx* c, uint64_t, void*) {
    Spin* s = slot_of(c);
    if (!s) return 0;
    const uint64_t obj = c->x[25];
    // only the bits this worker serves: the other workers set and clear theirs in the same word all the time
    const uint32_t signal = (guest::ok(obj + kSignal, 4) ? rd<uint32_t>(obj + kSignal) : 0) & static_cast<uint32_t>(c->x[27]);
    const uint64_t now = qpc::now_ns();
    // A pass with the same bits set as the previous one, within kGapNs of it, continues the streak; a claim or new work changes the
    // bits, and a worker that blocked or left the scan comes back after a gap. How long one pass takes does not matter (a pass over
    // the waiting-job bitmap is long).
    const bool same = signal && signal == s->last_signal && now - s->last_ns < kGapNs;
    s->last_ns = now;
    s->last_signal = signal;
    if (!same) {
        s->since = now;
        s->fast = s->yields = 0;
        return 0;
    }
    if (++s->fast < kMinPasses || now - s->since < kStuckNs) return 0;
    if (s->yields < kSleepAfter) {
        ++s->yields;
        std::this_thread::yield();
    } else {
        if (!s->logged) {
            s->logged = true;
            NXLOG_I("pkp", "job spin: a worker rescans with nothing to claim (signal bits 0x%x): sleeping %lld us per pass until they change", signal,
                    static_cast<long long>(kSleepUs.count()));
        }
        g_sleeps.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(kSleepUs);
    }
    s->last_ns = qpc::now_ns();   // the governor's own wait is not a gap
    return 0;
}

void install(const FeatureInstall& in) {
    if constexpr (facts::kPhone) {
        in.hooks.add_mid(in.base + kScanHead, &hook_scan);
        in.hooks.add_mid(in.base + kClassAgain, &hook_scan);
        in.hooks.add_mid(in.base + kBitmapAgain, &hook_scan);
    }
}

void status_text(char* buf, size_t n) { std::snprintf(buf, n, " jobspin=%llu", static_cast<unsigned long long>(g_sleeps.load())); }

}  // namespace

const Feature kJobSpinFeature = {
    .name = "job_spin",
    .install = &install,
    .status_text = facts::kPhone ? &status_text : nullptr,
};

}  // namespace nxrt::games::pkp
