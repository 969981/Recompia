// SPDX-License-Identifier: GPL-3.0-or-later
// The device's thermal headroom (Android ADPF, src/os/os_android_headroom.cpp): one thread polls AThermal_getThermalHeadroom once a
// second, now and 10 s ahead, so every reader (the frame rate counter, a game's dynamic resolution) shares one caller, as the platform
// asks. headroom: the share of the thermal envelope in use, 1.0 = the SEVERE status (AOSP: about 0.033 per degree C of skin temperature,
// linear on (0, 1]); not a temperature. The forecast is AOSP's linear extrapolation of the last ~30 s, so it is noisy: the median of the
// last 5 is kept. Off Android, or where the API answers nothing, headroom() is false.
#pragma once

#include <cmath>
#include <cstdint>

namespace nxrt::thermal {

struct Headroom {
    float now = NAN;          // AThermal_getThermalHeadroom(0), the last answer that was a number
    float forecast = NAN;     // median of the last 5 answers of AThermal_getThermalHeadroom(10)
    float light = NAN;        // the headroom at which the LIGHT status starts on this device (API 35 thresholds), NaN when unknown
    int status = -1;          // AThermal status: 0 none, 1 light, 2 moderate, 3 severe, 4 critical, 5 emergency, 6 shutdown; -1 unknown
    uint64_t polls = 0;       // polls that gave a number (a reader acts on new ones)
};

#if defined(__ANDROID__)
void headroom_start();                // starts the poller (once; later calls do nothing)
bool headroom(Headroom* out);         // the latest values; false while nothing usable has arrived (or the device has no headroom)
#else
inline void headroom_start() {}
inline bool headroom(Headroom*) { return false; }
#endif

}  // namespace nxrt::thermal
