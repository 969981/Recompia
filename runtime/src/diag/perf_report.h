// SPDX-License-Identifier: GPL-3.0-or-later
// Performance report (nxrt/perf_report.h): the hooks the hitch monitor (gauge.cpp) drives. Nothing here runs with [diag] perf_report = off.
#pragma once
#include <cstdint>

namespace nxrt::diag {

// Every presented frame, on the presenting thread (soak_note_frame), with its QueryPerformanceCounter tick: sorts the frame into free /
// capped / unfocused / long and adds it to the counters. A few relaxed loads and adds, no lock, no allocation.
void perf_frame(uint64_t qpc_ticks);
// The hitch monitor's summary tick (its own thread), period_s apart: the settings line when it changed, the period's line, the pass-sample
// trigger.
void perf_tick(double period_s);

}  // namespace nxrt::diag
