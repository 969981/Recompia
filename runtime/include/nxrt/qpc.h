// SPDX-License-Identifier: GPL-3.0-or-later
// The host's performance counter (QueryPerformanceCounter): ticks, the tick rate and overflow-safe conversions. Header-only; the rate is read
// once. This is the runtime's monotonic host clock; the console tick (kernel.h system_tick) is derived from it.
#pragma once
#include <cstdint>

#include <windows.h>

namespace nxrt::qpc {

// Ticks per second (QueryPerformanceFrequency), read once.
inline uint64_t freq() {
    static const uint64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart > 0 ? static_cast<uint64_t>(v.QuadPart) : uint64_t{1};
    }();
    return f;
}

// The counter now, in ticks.
inline uint64_t ticks() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

// `t` ticks scaled to `hz` units a second (1e9 = nanoseconds), split so the multiplication cannot overflow.
inline uint64_t scale(uint64_t t, uint64_t hz) {
    const uint64_t f = freq();
    return (t / f) * hz + ((t % f) * hz) / f;
}

inline uint64_t to_ns(uint64_t t) { return scale(t, 1000000000ull); }
inline uint64_t now_ns() { return to_ns(ticks()); }
inline double seconds() { return static_cast<double>(ticks()) / static_cast<double>(freq()); }

}  // namespace nxrt::qpc
