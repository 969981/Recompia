// SPDX-License-Identifier: GPL-3.0-or-later
// Precise waits (src/kernel/precise_wait.cpp, [sched] precise_sleep / precise_waits): the timer request and lateness estimate (pure
// functions), and that no precise sleep or timed kernel wait ever ends before its deadline. Timing rule: lower bounds are exact (the
// runtime and steady_clock both read QueryPerformanceCounter; 2 us slack for the console-tick conversion), upper bounds are only
// asserted where a missed wake would take a whole timeout, and are generous for a loaded machine.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "nxrt/kernel.h"
#include "test_framework.h"

NXTEST_GROUP(kernel_precise)

namespace ks = nxrt::kernel;

namespace {
using Clock = std::chrono::steady_clock;
constexpr int64_t kUs = 1000;
constexpr int64_t kSlackNs = 2000;

int64_t ns_since(Clock::time_point t0) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count(); }

struct PreciseOn {   // turns both keys on for one test and back to the config afterwards
    PreciseOn() { ks::set_precise_timing(1, 1); }
    ~PreciseOn() { ks::set_precise_timing(-2, -2); }
};
}  // namespace

NXTEST(timer_request_leaves_the_lateness_out) {
    CHECK_EQ(ks::precise_timer_request(ks::kPreciseSpinMaxNs, 450 * kUs), 0);        // short remainder: yielded, no timer
    CHECK_EQ(ks::precise_timer_request(50 * kUs, 450 * kUs), 0);
    CHECK_EQ(ks::precise_timer_request(1000 * kUs, 450 * kUs), 550 * kUs);           // 1 ms: ask for 0.55 ms, wakes about on time
    CHECK_EQ(ks::precise_timer_request(1000 * kUs, 0), 1000 * kUs);
    CHECK_EQ(ks::precise_timer_request(1000 * kUs, -5), 1000 * kUs);                  // a negative estimate counts as none
    CHECK_EQ(ks::precise_timer_request(300 * kUs, 450 * kUs), 100);                   // less left than the lateness: the shortest timer
    CHECK(ks::precise_timer_request(ks::kPreciseSpinMaxNs + 1, 0) > 0);
}

NXTEST(late_update_is_a_bounded_moving_average) {
    // A timer that is always 450 us late keeps a 450 us estimate.
    CHECK_EQ(ks::precise_late_update(450 * kUs, 550 * kUs, 1000 * kUs), 450 * kUs);
    // Moves 1/16 of the way to the sample.
    CHECK_EQ(ks::precise_late_update(450 * kUs, 550 * kUs, 550 * kUs + 610 * kUs), 450 * kUs + 10 * kUs);
    CHECK_EQ(ks::precise_late_update(450 * kUs, 550 * kUs, 550 * kUs + 290 * kUs), 450 * kUs - 10 * kUs);
    // Early wakes and preempted waits (2 ms late or more) are not samples.
    CHECK_EQ(ks::precise_late_update(450 * kUs, 550 * kUs, 500 * kUs), 450 * kUs);
    CHECK_EQ(ks::precise_late_update(450 * kUs, 550 * kUs, 550 * kUs + 2000 * kUs), 450 * kUs);
    CHECK_EQ(ks::precise_late_update(450 * kUs, 550 * kUs, 550 * kUs + 9000 * kUs), 450 * kUs);
    // Converges: from the start value to a 120 us timer, and to a 1.5 ms one (a coarse timer).
    int64_t late = 450 * kUs;
    for (int i = 0; i < 400; i++) late = ks::precise_late_update(late, ks::precise_timer_request(1000 * kUs, late), ks::precise_timer_request(1000 * kUs, late) + 120 * kUs);
    CHECK(late > 110 * kUs && late < 130 * kUs);
    for (int i = 0; i < 400; i++) late = ks::precise_late_update(late, 100, 100 + 1500 * kUs);
    CHECK(late > 1450 * kUs && late <= 1500 * kUs);
    // Clamped to 0 .. 2 ms.
    CHECK_EQ(ks::precise_late_update(2500 * kUs, 100, 100 + 1999 * kUs), 2000 * kUs);
    CHECK_EQ(ks::precise_late_update(0, 100, 100), 0);
}

NXTEST(precise_wait_never_returns_early) {
    const int64_t spans[] = {201 * kUs, 300 * kUs, 500 * kUs, 1000 * kUs, 1500 * kUs, 2300 * kUs, 4000 * kUs};
    int64_t total_late = 0, n = 0;
    for (int64_t ns : spans)
        for (int r = 0; r < 20; r++) {
            const auto t0 = Clock::now();
            CHECK(!ks::precise_wait_ns(ns, nullptr));
            const int64_t el = ns_since(t0);
            CHECK_MSG(el >= ns, "a precise wait ended before its deadline");
            total_late += el - ns;
            n++;
        }
    std::printf("    precise_wait: %lld waits, mean %lld us late, estimate %lld us\n", static_cast<long long>(n),
                static_cast<long long>(total_late / n / kUs), static_cast<long long>(ks::precise_late_ns() / kUs));
}

NXTEST(precise_wait_ends_on_its_event) {
    const HANDLE ev = CreateEventW(nullptr, FALSE, TRUE, nullptr);   // already set: returns at once
    REQUIRE(ev);
    auto t0 = Clock::now();
    CHECK(ks::precise_wait_ns(500000 * kUs, ev));
    CHECK_MSG(ns_since(t0) < 1000000 * kUs / 10, "a set event did not end a 500 s wait");
    std::thread setter([ev] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        SetEvent(ev);
    });
    t0 = Clock::now();
    CHECK(ks::precise_wait_ns(500000 * kUs, ev));     // set while the thread sleeps on the timer
    CHECK_MSG(ns_since(t0) < 10000000 * kUs, "the event did not wake the timer wait");
    setter.join();
    CloseHandle(ev);
}

NXTEST(precise_sleep_never_returns_early) {
    {   // reference: the default path, for the printed comparison only
        ks::set_precise_timing(0, -1);
        int64_t total = 0;
        for (int r = 0; r < 100; r++) {
            const auto t0 = Clock::now();
            ks::sleep_ns(1000 * kUs);
            total += ns_since(t0);
        }
        std::printf("    default sleep_ns(1 ms): mean %lld us\n", static_cast<long long>(total / 100 / kUs));
    }
    PreciseOn on;
    CHECK(ks::precise_sleep_enabled());
    int64_t total = 0;
    for (int r = 0; r < 100; r++) {
        const auto t0 = Clock::now();
        ks::sleep_ns(1000 * kUs);
        const int64_t el = ns_since(t0);
        CHECK_MSG(el >= 1000 * kUs, "SleepThread(1 ms) ended early");
        total += el;
    }
    std::printf("    precise sleep_ns(1 ms): mean %lld us\n", static_cast<long long>(total / 100 / kUs));
    for (int64_t ns : {150 * kUs, 250 * kUs, 3000 * kUs, 12000 * kUs}) {
        const auto t0 = Clock::now();
        ks::sleep_ns(ns);
        CHECK_MSG(ns_since(t0) >= ns, "a precise sleep ended early");
    }
}

NXTEST(precise_kernel_waits_never_return_early) {
    ks::kernel_reset();
    PreciseOn on;
    CHECK(ks::precise_waits_enabled());
    alignas(4) uint32_t w = 4;
    const uint64_t a = reinterpret_cast<uint64_t>(&w);
    for (int64_t ns : {100 * kUs, 300 * kUs, 1000 * kUs, 2200 * kUs, 2600 * kUs, 7300 * kUs})
        for (int r = 0; r < 5; r++) {
            const auto t0 = Clock::now();
            CHECK_EQ(ks::wait_for_address(a, 2, 4, ns), ks::kTimedOut);   // WaitIfEqual, met
            const int64_t el = ns_since(t0);
            CHECK_MSG(el + kSlackNs >= ns, "a precise WaitForAddress timed out before its deadline");
            CHECK_MSG(el < ns + 1000000 * kUs, "a precise WaitForAddress took far longer than its timeout");
        }
    // WaitSynchronization: same, with an event that stays clear.
    auto e = std::make_shared<ks::KEvent>();
    const uint32_t h = ks::handle_add(e);
    REQUIRE(h != 0);
    for (int64_t ns : {150 * kUs, 1200 * kUs, 3100 * kUs}) {
        int32_t idx = -1;
        const auto t0 = Clock::now();
        CHECK_EQ(ks::wait_synchronization(&h, 1, ns, &idx), ks::kTimedOut);
        CHECK_MSG(ns_since(t0) + kSlackNs >= ns, "a precise WaitSynchronization timed out before its deadline");
    }
    ks::kernel_reset();
}

// A signal in the last stretch (on the timer, not the condition variable) still wakes the waiter: the event path of kick().
NXTEST(precise_kernel_wait_woken_in_last_stretch) {
    ks::kernel_reset();
    PreciseOn on;
    int woken = 0, tried = 0;
    for (int r = 0; r < 10; r++) {
        auto e = std::make_shared<ks::KEvent>();
        const uint32_t h = ks::handle_add(e);
        REQUIRE(h != 0);
        constexpr int64_t kTimeout = 2400 * kUs;   // all of it is the last stretch
        std::atomic<bool> go{false};
        std::atomic<int64_t> signal_at{0};
        const auto t0 = Clock::now();
        std::thread signaller([&] {
            while (!go.load()) SwitchToThread();
            const auto s0 = Clock::now();
            while (Clock::now() - s0 < std::chrono::microseconds(600)) SwitchToThread();
            e->state->signaled = true;
            ks::notify_waitable_changed();
            signal_at = ns_since(t0);
        });
        int32_t idx = -1;
        go = true;
        const uint32_t rc = ks::wait_synchronization(&h, 1, kTimeout, &idx);
        const int64_t el = ns_since(t0);
        signaller.join();
        ks::handle_close(h);
        if (signal_at.load() > kTimeout - 1000 * kUs) continue;   // the signaller was descheduled near the timeout: no verdict
        tried++;
        CHECK_EQ(rc, ks::kOk);
        // The wait loop re-checks the event after a timeout too, so kOk alone proves nothing: it must end well before the timeout.
        if (rc == ks::kOk && el < kTimeout - 300 * kUs) woken++;
    }
    CHECK_MSG(tried == 0 || woken * 2 >= tried, "signals in the last stretch did not end the timer wait");
    std::printf("    last-stretch wakes: %d of %d\n", woken, tried);
    ks::kernel_reset();
}
