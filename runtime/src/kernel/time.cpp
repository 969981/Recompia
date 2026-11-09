// SPDX-License-Identifier: GPL-3.0-or-later
// High-resolution sleep (the system tick is in core/clock.cpp). Pattern from smo-recomp runtime/os/os_threads.c sleep_ns.
#include <windows.h>

#include <string>

#include "nxrt/clock.h"
#include "nxrt/config.h"
#include "nxrt/host_device.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "kernel_internal.h"

namespace nxrt::kernel {

// SleepThread takes nanoseconds and the console timer is good to a few microseconds. Sleep() counts whole milliseconds on a
// 1 ms or 15.6 ms grid, so the wait runs on a high-resolution waitable timer (100 ns units); Sleep() is only the fallback.
// A zero or negative span yields, as on the console.
namespace {
// Short sleeps as real sleeps: under Wine, NtDelayExecution (not alertable) is a nanosleep on the host, good to tens of microseconds,
// so a 100 us poll costs no processor time, but wakes ~50 us late (Linux timer slack). On Windows it rounds up to the timer's grid
// (about half a millisecond or more). [kernel] short_sleep = auto | yield | sleep (sleep only under Wine; Windows always yields).
// auto: sleep on a handheld (host_is_handheld), yield elsewhere. Measured in game: on the PC, uncapped, yielding 231 fps on 4.1 cores and
// sleeping 198 fps on 3.8 cores; on a Steam Deck LCD, where the frame waits on the GPU anyway and CPU and GPU share one power budget,
// sleeping 54 fps (1% lows ~30) against 52 yielding (lows ~27).
// Pokopia polls its GPU fences with ~80 us SleepThread calls on two threads, thousands a second: yielded through, each keeps a
// processor core busy for as long as the GPU takes.
using NtDelayExecutionFn = LONG(WINAPI*)(BOOLEAN, PLARGE_INTEGER);
NtDelayExecutionFn short_sleep_fn() {
    static const NtDelayExecutionFn fn = [] () -> NtDelayExecutionFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const bool wine = host_is_wine();
        const std::string m = config_get_str("kernel", "short_sleep", "auto");
        const bool sleep = wine && (m == "sleep" || (m == "auto" && host_is_handheld()));
        auto f = sleep && ntdll ? reinterpret_cast<NtDelayExecutionFn>(GetProcAddress(ntdll, "NtDelayExecution")) : nullptr;
        NXLOG_I("kernel", "short sleeps (up to 200 us): %s ([kernel] short_sleep = %s%s)", f ? "slept (NtDelayExecution)" : "yielded through",
                m.c_str(), !wine ? ", Windows" : m == "auto" ? (host_is_handheld() ? ", a handheld" : ", not a handheld") : "");
        return f;
    }();
    return fn;
}

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
        if (NtDelayExecutionFn delay = short_sleep_fn()) {
            LARGE_INTEGER span;
            span.QuadPart = -((ns + 99) / 100);             // relative, in 100 ns units
            delay(FALSE, &span);
            return;
        }
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
    if (ns <= 0 || clock::virtual_sleep(ns)) return;
    timer_wait(ns);
}

// On the thread set_blocked_hook names, a sleep longer than 10 ms is cut into 10 ms slices with the hook run between them, as in the kernel
// waits: Pokopia's main thread sleeps in SleepThread while the software keyboard applet is open (the freeze report's main stack).
void sleep_ns(int64_t ns) {
    if (clock::virtual_sleep(ns)) return;                   // a virtual clock (deterministic test mode): time advances, the thread only yields
    constexpr int64_t kSlice = 10000000;
    void (*fn)() = ns > kSlice ? detail::blocked_hook_here() : nullptr;
    if (!fn) return sleep_once(ns);
    const auto qpc_ns = [] { return static_cast<int64_t>(qpc::now_ns()); };
    const int64_t until = qpc_ns() + ns;
    for (int64_t rem = ns; rem > 0; rem = until - qpc_ns()) {
        if (rem <= kSlice) return sleep_once(rem);
        sleep_once(kSlice);
        fn();
    }
}

}  // namespace nxrt::kernel
