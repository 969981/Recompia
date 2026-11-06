// SPDX-License-Identifier: GPL-3.0-or-later
// System tick and high-resolution sleep. Pattern from smo-recomp runtime/os/os_threads.c sleep_ns and SuperMarioWonderRecomp kernel.cpp system_tick.
#include <windows.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/tt.h"
#include "kernel_internal.h"

namespace nxrt::kernel {
namespace {
// [time] tick = wall (default) | qpc. wall: the console tick (CNTPCT_EL0, nn::os::GetSystemTick) counts the host wall clock since a base
// kept in tick_base.txt beside the ini (unix ns). QueryPerformanceCounter stops while the host sleeps and starts over at boot, and a game
// that moves its own clock by the tick between saves (Pokopia) lost every sleep: 41 min behind after a handheld slept with it open, and
// still behind at every launch after. The wall tick never loses a tick against the host clock, through sleep, launches and reboots. The
// base is written once, as host time minus the counter at that moment, so the count goes on from where the old counter was (no jump at
// the switch); [time] tick_shift = <seconds> adds that much on that first write, a one-time move of the game's clock (to give back the
// minutes lost before the switch). The same file on another machine gives the same tick for a shared save. The tick never runs
// backwards: a host clock step back under 2 s holds it, a larger one lowers the base by the step (logged) so the count goes on.
// qpc: the old counter.
constexpr const char* kTickBaseFile = "tick_base.txt";
constexpr int64_t kHoldNs = 2000000000;          // a step back of the host clock under this holds the tick instead of moving the base

int64_t qpc_ns() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(c.QuadPart / f.QuadPart * 1000000000 + c.QuadPart % f.QuadPart * 1000000000 / f.QuadPart);
}
int64_t wall_unix_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;   // 100 ns units since 1601-01-01
    return static_cast<int64_t>(t - 116444736000000000ull) * 100;
}

struct WallTick {
    std::mutex m;
    std::atomic<bool> ready{false};
    bool on = false;
    std::atomic<int64_t> base_ns{0};
    std::atomic<int64_t> last_ns{0};
};
WallTick& wall_tick() {
    static WallTick w;
    return w;
}

void wall_tick_init(WallTick& w) {
    std::lock_guard<std::mutex> lk(w.m);
    if (w.ready.load()) return;
    std::string mode = config_get_str("time", "tick", "wall");
    for (char& ch : mode)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    w.on = mode != "qpc";
    if (w.on) {
        int64_t base = 0;
        {
            std::ifstream in(kTickBaseFile);
            if (!(in >> base)) base = 0;
        }
        if (base == 0) {
            const int64_t shift = config_get_int("time", "tick_shift", 0);
            base = wall_unix_ns() - qpc_ns() - shift * 1000000000ll;
            std::ofstream out(kTickBaseFile, std::ios::trunc);
            out << base << '\n';
            NXLOG_I("time", "console tick: base written to %s (continues the counter, shift %lld s)", kTickBaseFile, static_cast<long long>(shift));
        }
        w.base_ns.store(base);
        NXLOG_I("time", "console tick: host wall clock since base %lld (unix ns): %lld s at start-up", static_cast<long long>(base),
                static_cast<long long>((wall_unix_ns() - base) / 1000000000));
    } else {
        NXLOG_I("time", "console tick: QueryPerformanceCounter");
    }
    w.ready.store(true);
}

// The wall tick in ns, or false when [time] tick = qpc.
bool wall_tick_ns(int64_t* out) {
    WallTick& w = wall_tick();
    if (!w.ready.load(std::memory_order_acquire)) wall_tick_init(w);
    if (!w.on) return false;
    for (;;) {
        int64_t v = wall_unix_ns() - w.base_ns.load(std::memory_order_relaxed);
        if (v < 0) v = 0;
        int64_t last = w.last_ns.load(std::memory_order_relaxed);
        if (v >= last) {
            if (w.last_ns.compare_exchange_weak(last, v, std::memory_order_relaxed)) {
                *out = v;
                return true;
            }
            continue;
        }
        if (last - v < kHoldNs) {
            *out = last;
            return true;
        }
        std::lock_guard<std::mutex> lk(w.m);
        v = wall_unix_ns() - w.base_ns.load();
        last = w.last_ns.load();
        if (v >= last || last - v < kHoldNs) continue;   // another thread moved the base already
        const int64_t step = last - v;
        w.base_ns.store(w.base_ns.load() - step);
        std::ofstream out(kTickBaseFile, std::ios::trunc);
        out << w.base_ns.load() << '\n';
        NXLOG_W("time", "console tick: the host clock stepped back %lld ms; base lowered to %lld so the tick goes on from where it was",
                static_cast<long long>(step / 1000000), static_cast<long long>(w.base_ns.load()));
    }
}
}  // namespace

uint64_t tick_hz() {
    static const uint64_t hz = game::info().platform == game::Platform::Switch2 ? kTickHzSwitch2 : kTickHzSwitch1;
    return hz;
}

uint64_t system_tick() {
    if (uint64_t vt; tt::virtual_tick(&vt)) return vt;      // deterministic mode: the virtual clock (test tooling)
    if (int64_t ns; wall_tick_ns(&ns)) {                     // [time] tick = wall: the host wall clock in console ticks
        const uint64_t n = static_cast<uint64_t>(ns), hz = tick_hz();
        return (n / 1000000000ull) * hz + (n % 1000000000ull) * hz / 1000000000ull;
    }
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t v = static_cast<uint64_t>(c.QuadPart);
    const uint64_t hz = tick_hz();
    return (v / freq) * hz + (v % freq) * hz / freq;     // split so the multiplication cannot overflow
}

// SleepThread takes nanoseconds and the console timer is good to a few microseconds. Sleep() counts whole milliseconds on a
// 1 ms or 15.6 ms grid, so the wait runs on a high-resolution waitable timer (100 ns units); Sleep() is only the fallback.
// A zero or negative span yields, as on the console.
namespace {
// The calling thread's high-resolution waitable timer for ns (> 0); Sleep() in whole milliseconds without one.
void timer_wait(int64_t ns) {
    static thread_local HANDLE timer = nullptr;
    static thread_local bool no_timer = false;
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) no_timer = true;
    }
    LARGE_INTEGER due;
    due.QuadPart = -((ns + 99) / 100);
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
    else
        WaitForSingleObject(timer, INFINITE);
}

void sleep_once(int64_t ns) {
    if (ns <= 0) {
        yield_host();
        return;
    }
    // Short sleeps (polls such as Pokopia's GPU-fence wait, 100 us a round) overshoot to about half a millisecond on the timer: yield until the
    // deadline instead. Longer ones keep the timer (no busy waiting while a thread idles).
    if (ns <= 200000) {
        LARGE_INTEGER f, s, n;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&s);
        const int64_t ticks = ns * f.QuadPart / 1000000000;
        do {
            yield_host();
            QueryPerformanceCounter(&n);
        } while (n.QuadPart - s.QuadPart < ticks);
        return;
    }
    // [sched] precise_sleep: the timer is asked for less by its learned lateness (precise_wait.cpp). Pokopia's main thread waits out each
    // frame in about 900 SleepThread(1 ms) a second; uncorrected, each takes about 1.4-1.5 ms (pokopia-native m4-title: title 59.3 Hz).
    if (precise_sleep_enabled()) {
        precise_wait_ns(ns, nullptr);
        return;
    }
    timer_wait(ns);
}
}  // namespace

void detail::sleep_timer_ns(int64_t ns) {
    if (ns <= 0 || tt::virtual_sleep(ns)) return;
    timer_wait(ns);
}

// On the thread set_blocked_hook names, a sleep longer than 10 ms is cut into 10 ms slices with the hook run between them, as in the kernel
// waits: Pokopia's main thread sleeps in SleepThread while the software keyboard applet is open (the freeze report's main stack).
void sleep_ns(int64_t ns) {
    if (tt::virtual_sleep(ns)) return;                      // deterministic mode: time advances, the thread only yields
    constexpr int64_t kSlice = 10000000;
    void (*fn)() = ns > kSlice ? detail::blocked_hook_here() : nullptr;
    if (!fn) return sleep_once(ns);
    const auto qpc_ns = [] {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<int64_t>(c.QuadPart / f.QuadPart * 1000000000 + c.QuadPart % f.QuadPart * 1000000000 / f.QuadPart);
    };
    const int64_t until = qpc_ns() + ns;
    for (int64_t rem = ns; rem > 0; rem = until - qpc_ns()) {
        if (rem <= kSlice) return sleep_once(rem);
        sleep_once(kSlice);
        fn();
    }
}

}  // namespace nxrt::kernel
