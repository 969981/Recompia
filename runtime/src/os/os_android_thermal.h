// SPDX-License-Identifier: GPL-3.0-or-later
// os_android_thermal.h: how hot the phone is, how far its CPU clock is held down and how busy this process keeps the CPU, for the
// frame rate counter ([fps] detail 2, Full). The GPU's load and clock cap come from diag::perf_gpu_load (src/diag/perf_system.h).
//
// status: Android's own thermal status (AThermal_getCurrentThermalStatus: 0 none, 1 light, 2 moderate, 3 severe, 4 critical,
// 5 emergency, 6 shutdown), the level that makes the system lower the clocks; not a temperature.
// headroom: AThermal_getThermalHeadroom(0) (API 31): the share of the thermal envelope in use, 1.0 = the SEVERE threshold; not a
// temperature either. skin_c / battery_c: real temperatures in degrees C where a sensor can be read: a thermal zone whose type names
// the skin sensor (usually blocked for apps by SELinux: then unknown), and the battery (power_supply sysfs, else the sticky
// ACTION_BATTERY_CHANGED intent's EXTRA_TEMPERATURE through JNI, which every app may read).
// cpu_cap_mhz: the fastest core's current clock ceiling (cpufreq scaling_max_freq). cpu_cores: this process's CPU time over the
// time since the previous read, in cores (2.5 = two and a half cores busy). Unknown: -1 (temperatures: NaN).
// Off Android read() is false (desktops have no such status: the counter shows nothing).
#pragma once

#include <cmath>

namespace nxrt::os::thermal {

struct Reading {
    int status = -1;
    float headroom = -1.f;
    float skin_c = NAN;
    float battery_c = NAN;
    int cpu_cap_mhz = -1;
    float cpu_cores = -1.f;
};

#if defined(__ANDROID__)
// One reading now: binder calls to the thermal service (and, when the battery sysfs file is not readable, to the activity manager
// through JNI) and a few small sysfs reads. Call it about once a second (the headroom answers NaN to faster callers). False when
// nothing at all is known.
bool read(Reading* out);
#else
inline bool read(Reading*) { return false; }
#endif

}  // namespace nxrt::os::thermal
