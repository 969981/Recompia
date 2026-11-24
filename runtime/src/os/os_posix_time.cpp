// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_time.cpp: time for src/os/nx_os.h -- the performance counter, tick counts, system and file times, the
// timer-resolution calls (mmsystem.h) and the waitable timers, each with a worker thread on the shared wait rule.
#include "nx_os_internal.h"

// The SDK-shaped header whose entry points (timeBeginPeriod, timeGetDevCaps) live in this file.
#include "mmsystem.h"

#if !defined(_WIN32)

#include <pthread.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include <atomic>
#include <cstdint>

using namespace nxos;

// ==================================================================================================================
// time
// ==================================================================================================================
BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    if (!out) return FALSE;
#if defined(__APPLE__)
    out->QuadPart = static_cast<LONGLONG>(mach_absolute_time());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    out->QuadPart = static_cast<LONGLONG>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    if (!out) return FALSE;
#if defined(__APPLE__)
    // mach_absolute_time counts timebase ticks; report the frequency in the same unit so ns = ticks * 1e9 / freq.
    static const double ticks_per_ns = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return static_cast<double>(tb.denom) / static_cast<double>(tb.numer);
    }();
    out->QuadPart = static_cast<LONGLONG>(ticks_per_ns * 1e9);
#else
    out->QuadPart = 1000000000LL;
#endif
    return TRUE;
}

static uint64_t monotonic_ns() {
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    if (f.QuadPart <= 0) return 0;
    return static_cast<uint64_t>((static_cast<double>(c.QuadPart) * 1e9) / static_cast<double>(f.QuadPart));
}

DWORD GetTickCount(void) { return static_cast<DWORD>(GetTickCount64() & 0xFFFFFFFFu); }

ULONGLONG GetTickCount64(void) { return monotonic_ns() / 1000000ull; }

static void fill_systemtime(LPSYSTEMTIME st, const struct tm& t) {
    st->wYear = static_cast<WORD>(t.tm_year + 1900);
    st->wMonth = static_cast<WORD>(t.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(t.tm_wday);
    st->wDay = static_cast<WORD>(t.tm_mday);
    st->wHour = static_cast<WORD>(t.tm_hour);
    st->wMinute = static_cast<WORD>(t.tm_min);
    st->wSecond = static_cast<WORD>(t.tm_sec);
    st->wMilliseconds = 0;
}

void GetSystemTime(LPSYSTEMTIME st) {
    if (!st) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t{};
    gmtime_r(&ts.tv_sec, &t);
    fill_systemtime(st, t);
    st->wMilliseconds = static_cast<WORD>(ts.tv_nsec / 1000000);
}

void GetLocalTime(LPSYSTEMTIME st) {
    if (!st) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t{};
    localtime_r(&ts.tv_sec, &t);
    fill_systemtime(st, t);
    st->wMilliseconds = static_cast<WORD>(ts.tv_nsec / 1000000);
}

void GetSystemTimeAsFileTime(LPFILETIME ft) {
    if (!ft) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    set_filetime(ft, filetime_of(ts));
}

ULONGLONG GetSystemTimeAsUnixTime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<ULONGLONG>(ts.tv_sec);
}

BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft) {
    if (!st || !ft) return FALSE;
    struct tm t{};
    t.tm_year = st->wYear - 1900;
    t.tm_mon = st->wMonth - 1;
    t.tm_mday = st->wDay;
    t.tm_hour = st->wHour;
    t.tm_min = st->wMinute;
    t.tm_sec = st->wSecond;
    const time_t secs = timegm(&t);
    set_filetime(ft, 11644473600ull * 10000000ull + static_cast<uint64_t>(secs) * 10000000ull +
                         static_cast<uint64_t>(st->wMilliseconds) * 10000ull);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st) {
    if (!ft || !st) return FALSE;
    const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    const uint64_t unix_100ns = v - 11644473600ull * 10000000ull;
    time_t secs = static_cast<time_t>(unix_100ns / 10000000ull);
    struct tm t{};
    gmtime_r(&secs, &t);
    fill_systemtime(st, t);
    st->wMilliseconds = static_cast<WORD>((unix_100ns % 10000000ull) / 10000ull);
    return TRUE;
}

BOOL FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME local) {
    if (!ft || !local) return FALSE;
    const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    const time_t secs = static_cast<time_t>((v - 11644473600ull * 10000000ull) / 10000000ull);
    struct tm t{};
    localtime_r(&secs, &t);
    set_filetime(local, 11644473600ull * 10000000ull + static_cast<uint64_t>(timegm(&t)) * 10000000ull +
                            (v - 11644473600ull * 10000000ull) % 10000000ull);
    return TRUE;
}

BOOL LocalFileTimeToFileTime(const FILETIME* ft, LPFILETIME utc) {
    if (!ft || !utc) return FALSE;
    const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    const time_t secs = static_cast<time_t>((v - 11644473600ull * 10000000ull) / 10000000ull);
    struct tm t{};
    gmtime_r(&secs, &t);
    const time_t utc_secs = timegm(&t);
    struct tm local{};
    localtime_r(&utc_secs, &local);
    set_filetime(utc, 11644473600ull * 10000000ull + static_cast<uint64_t>(timegm(&local)) * 10000000ull +
                          (v - 11644473600ull * 10000000ull) % 10000000ull);
    return TRUE;
}

// macOS has no timer-resolution knob: the scheduler is not on a 15.6 ms tick, so these are no-ops that succeed.
DWORD timeBeginPeriod(UINT) { return 0; }
DWORD timeEndPeriod(UINT) { return 0; }

UINT timeGetDevCaps(TIMECAPS* caps, UINT size) {
    if (!caps || size < sizeof(TIMECAPS)) return 97;   // TIMERR_NOCANDO
    caps->wPeriodMin = 1;
    caps->wPeriodMax = 1000;
    return 0;   // TIMERR_NOERROR
}

void GetSystemTimePreciseAsFileTime(LPFILETIME ft) { GetSystemTimeAsFileTime(ft); }

// ---- waitable timers ----------------------------------------------------------------------------------------------
namespace {

void* timer_worker(void* p) {
    auto* t = static_cast<TimerObj*>(p);
    pthread_mutex_lock(&t->mu);
    while (!t->stop) {
        const uint64_t gen = t->generation;
        const uint64_t due = t->due_ns;
        if (due == 0) {
            pthread_cond_wait(&t->cv, &t->mu);
            continue;
        }
        const uint64_t now = monotonic_ns();
        if (due > now) {
            const uint64_t delta = due - now;
            struct timespec dl{};
            clock_gettime(CLOCK_REALTIME, &dl);   // see make_deadline: every condvar here is on the realtime clock
            dl.tv_sec += static_cast<time_t>(delta / 1000000000ull);
            dl.tv_nsec += static_cast<long>(delta % 1000000000ull);
            if (dl.tv_nsec >= 1000000000L) {
                dl.tv_sec += 1;
                dl.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&t->cv, &t->mu, &dl);
            continue;   // re-read the generation: a re-arm wins over the deadline
        }
        if (t->generation != gen) continue;
        t->signaled.store(true, std::memory_order_release);
        t->epoch.fetch_add(1);
        wait_note();
        if (t->period_ns) {
            t->due_ns = now + t->period_ns;
        } else {
            t->due_ns = 0;   // one-shot: disarmed until the next SetWaitableTimer
            pthread_cond_wait(&t->cv, &t->mu);
        }
    }
    pthread_mutex_unlock(&t->mu);
    return nullptr;
}

}  // namespace

HANDLE nxos_create_waitable_timer(const char* name, DWORD flags, DWORD) {
    (void)name;
    auto* t = new TimerObj();
    t->manual = (flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0;
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION asks for a timer that is not quantised to the scheduler tick. On macOS
    // pthread_cond_timedwait on CLOCK_MONOTONIC is already sub-millisecond, so the flag needs no separate path.
    // Joinable, not detached: ~TimerObj joins it so the mutex and the condition variable outlive the wait (see
    // nx_os_internal.h). A detached worker would be waiting on destroyed objects the moment CloseHandle ran.
    if (pthread_create(&t->worker, nullptr, timer_worker, t) != 0) {
        delete t;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    t->worker_started = true;
    return t;
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period, void*, void*, BOOL) {
    auto* t = as<TimerObj>(h, Kind::Timer);
    if (!t || !due) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const uint64_t now = monotonic_ns();
    // due->QuadPart is 100 ns units; negative means relative to now (frame_limiter.cpp:78, timestep.cpp:202).
    const int64_t q = due->QuadPart;
    const uint64_t target = q < 0 ? now + static_cast<uint64_t>(-q) * 100ull : static_cast<uint64_t>(q) * 100ull;

    pthread_mutex_lock(&t->mu);
    t->due_ns = target;
    t->period_ns = period > 0 ? static_cast<uint64_t>(period) * 1000000ull : 0;
    t->generation++;
    if (!t->manual) t->signaled.store(false, std::memory_order_release);
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return TRUE;
}

BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER* due, LONG period, void* routine, void* arg, void* wake_context,
                        ULONG flags) {
    (void)wake_context;   // no suspend/resume power management to attach a timer to
    (void)flags;
    return SetWaitableTimer(h, due, period, routine, arg, FALSE);
}

BOOL CancelWaitableTimer(HANDLE h) {
    auto* t = as<TimerObj>(h, Kind::Timer);
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&t->mu);
    t->due_ns = 0;
    t->period_ns = 0;
    t->generation++;
    t->signaled.store(false, std::memory_order_release);
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return TRUE;
}

#endif  // !_WIN32
