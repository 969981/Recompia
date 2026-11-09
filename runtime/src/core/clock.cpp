// SPDX-License-Identifier: GPL-3.0-or-later
// Guest time (include/nxrt/clock.h): the host counter and clock, or an installed virtual source.
#include "nxrt/clock.h"

#include <windows.h>

#include <atomic>

#include "nxrt/game.h"
#include "nxrt/qpc.h"

namespace nxrt::clock {
namespace {
std::atomic<const Source*> g_source{nullptr};
}  // namespace

uint64_t tick_hz() {
    static const uint64_t hz = game::info().platform == game::Platform::Switch2 ? kTickHzSwitch2 : kTickHzSwitch1;
    return hz;
}

uint64_t ticks() {
    if (const Source* s = g_source.load(std::memory_order_acquire)) return s->ticks();
    return qpc::scale(qpc::ticks(), tick_hz());
}

uint64_t ticks_to_ns(uint64_t t) {
    const uint64_t hz = tick_hz();
    return (t / hz) * 1000000000ull + (t % hz) * 1000000000ull / hz;
}

uint64_t ns_to_ticks(uint64_t ns) {
    const uint64_t hz = tick_hz();
    return (ns / 1000000000ull) * hz + (ns % 1000000000ull) * hz / 1000000000ull;
}

uint64_t wall_filetime() {
    if (const Source* s = g_source.load(std::memory_order_acquire)) return s->wall_filetime();
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void set_source(const Source* s) { g_source.store(s, std::memory_order_release); }

bool is_virtual() { return g_source.load(std::memory_order_acquire) != nullptr; }

uint64_t virtual_frame_step_ticks() {
    const Source* s = g_source.load(std::memory_order_acquire);
    return s ? s->frame_step_ticks() : 0;
}

bool virtual_sleep(int64_t ns) {
    const Source* s = g_source.load(std::memory_order_acquire);
    return s && s->sleep(ns);
}

}  // namespace nxrt::clock
