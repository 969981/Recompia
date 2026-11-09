// SPDX-License-Identifier: GPL-3.0-or-later
// Guest time: the console's counter (CNTFRQ_EL0 / CNTVCT_EL0, svcGetSystemTick, the kernel's timed waits) and the wall clock the time
// services report. By default both follow the host: QueryPerformanceCounter scaled to the console's rate, and the host's UTC. A virtual
// source (the test tooling's deterministic mode) replaces them through set_source; the kernel and the services ask only here, so they do
// not depend on who provides the time.
#pragma once
#include <cstdint>

namespace nxrt::clock {

constexpr uint64_t kTickHzSwitch1 = 19200000;   // Switch 1 system tick (CNTFRQ_EL0)
constexpr uint64_t kTickHzSwitch2 = 31250000;   // Switch 2 system tick (SDK 20.5 for Switch 2 uses 31.25 MHz)

// The guest counter's frequency: the registered game's platform rate (game.h Info::platform), read once. Everything that converts guest
// ticks takes the rate from here.
uint64_t tick_hz();
uint64_t ticks();                                // the guest counter now (the source's, or the host counter scaled to tick_hz())
uint64_t ticks_to_ns(uint64_t ticks);            // at tick_hz(), split so the multiplication cannot overflow
uint64_t ns_to_ticks(uint64_t ns);
inline uint64_t now_ns() { return ticks_to_ns(ticks()); }
uint64_t wall_filetime();                        // UTC in 100 ns units since 1601-01-01

// A replacement for the host clock. Every member must be set.
struct Source {
    uint64_t (*ticks)();                         // the guest counter, at tick_hz()
    bool (*sleep)(int64_t ns);                   // takes a sleep of ns: true = handled (time moved as the source decides), the caller does not wait
    uint64_t (*wall_filetime)();                 // UTC in 100 ns units since 1601-01-01
};
// Installs `s` (null: the host clock again). `s` must stay valid while installed. Install before the guest runs: a thread that is
// already reading sees the change at its next read.
void set_source(const Source* s);
// A source is installed: guest time is not host time. Precise host waits and the yield back-off are then off (they measure host time).
bool is_virtual();
// The source's sleep of ns; false without a source (the caller waits on the host).
bool virtual_sleep(int64_t ns);

}  // namespace nxrt::clock
