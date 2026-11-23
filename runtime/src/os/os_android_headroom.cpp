// SPDX-License-Identifier: GPL-3.0-or-later
// os_android_headroom.cpp: the thermal headroom poller (nxrt/thermal.h). Android only (the CMake filter on os_android_*.cpp).
// One detached thread asks AThermal_getThermalHeadroom for now and 10 s ahead once a second. AOSP samples the skin sensor every second
// while a client asked within the last 10 s and forecasts by a linear regression over ~30 samples; it stops and forgets them after 10 s
// without a caller, so the poll never pauses. Older releases and some OEM builds answer NaN to fast callers: a NaN backs the poll off to
// every 10 s for a minute. A device whose first three answers are all NaN or 0 has no headroom. The status comes from a listener
// (AThermal_registerThermalStatusListener, API 30: a change arrives at once) and is read again on every poll (some devices never
// report a change); the LIGHT threshold from AThermal_getThermalHeadroomThresholds (API 35), looked up at run time like the headroom
// (API 31), because the app's floor is API 30.
#if defined(__ANDROID__)
#include "nxrt/thermal.h"

#include <android/thermal.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "nxrt/log.h"

namespace nxrt::thermal {
namespace {

constexpr const char* kTag = "thermal";

struct ThresholdEntry {   // AThermalHeadroomThreshold (API 35)
    float headroom;
    AThermalStatus thermalStatus;
};

std::mutex g_mu;
Headroom g_last;
bool g_dead = false;                 // the device answers no headroom
std::atomic<int> g_status{-1};

void on_status(void*, AThermalStatus s) {
    g_status.store(static_cast<int>(s), std::memory_order_relaxed);
    NXLOG_I(kTag, "thermal status %d (listener)", static_cast<int>(s));
}

void run() {
    AThermalManager* const mgr = AThermal_acquireManager();   // kept for the life of the process
    if (!mgr) { g_dead = true; return; }
    using HeadroomFn = float (*)(AThermalManager*, int);
    using ThresholdsFn = int (*)(AThermalManager*, const ThresholdEntry**, size_t*);
    const auto headroom_fn = reinterpret_cast<HeadroomFn>(dlsym(RTLD_DEFAULT, "AThermal_getThermalHeadroom"));                     // API 31
    const auto thresholds_fn = reinterpret_cast<ThresholdsFn>(dlsym(RTLD_DEFAULT, "AThermal_getThermalHeadroomThresholds"));      // API 35
    if (!headroom_fn) { g_dead = true; NXLOG_I(kTag, "headroom: not on this Android version"); return; }
    AThermal_registerThermalStatusListener(mgr, on_status, nullptr);
    float light = NAN;
    if (thresholds_fn) {
        const ThresholdEntry* t = nullptr;
        size_t n = 0;
        if (thresholds_fn(mgr, &t, &n) == 0 && t) {
            for (size_t i = 0; i < n; ++i)
                if (t[i].thermalStatus == ATHERMAL_STATUS_LIGHT && !std::isnan(t[i].headroom)) light = t[i].headroom;
            std::free(const_cast<ThresholdEntry*>(t));
        }
    }
    NXLOG_I(kTag, "headroom poller: once a second, now and 10 s ahead (median of 5); LIGHT threshold %s", std::isnan(light) ? "unknown" : std::to_string(light).c_str());
    float fc[5] = {NAN, NAN, NAN, NAN, NAN};
    int fc_n = 0, empty = 0, slow = 0;
    uint64_t polls = 0;
    for (;;) {
        const float h0 = headroom_fn(mgr, 0);
        const float hf = headroom_fn(mgr, 10);
        const int st = static_cast<int>(AThermal_getCurrentThermalStatus(mgr));
        if (st >= 0) g_status.store(st, std::memory_order_relaxed);
        const bool number = !std::isnan(h0) && !std::isnan(hf);
        if (polls == 0 && (!number || h0 <= 0.0f)) {
            if (++empty >= 3) {
                g_dead = true;
                NXLOG_I(kTag, "headroom: the device answers none (NaN or 0 three times): no heat-aware control");
                return;
            }
        } else if (!number) {
            if (slow == 0) NXLOG_W(kTag, "headroom: NaN: polling every 10 s for a minute");
            slow = 6;
        } else {
            fc[fc_n++ % 5] = hf;
            float s[5];
            const int k = std::min(fc_n, 5);
            std::copy(fc, fc + k, s);
            std::sort(s, s + k);
            ++polls;
            std::lock_guard<std::mutex> lk(g_mu);
            g_last.now = h0;
            g_last.forecast = s[k / 2];
            g_last.light = light;
            g_last.polls = polls;
        }
        if (slow > 0) {
            --slow;
            std::this_thread::sleep_for(std::chrono::seconds(10));
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

}  // namespace

void headroom_start() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(run).detach(); });
}

bool headroom(Headroom* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    *out = g_last;
    out->status = g_status.load(std::memory_order_relaxed);
    return !g_dead && g_last.polls > 0;
}

}  // namespace nxrt::thermal
#endif
