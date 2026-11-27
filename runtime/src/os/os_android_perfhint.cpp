// SPDX-License-Identifier: GPL-3.0-or-later
// os_android_perfhint.cpp: phone CPU placement (nxrt/perf_hint.h). Android only (the CMake filter on os_android_*.cpp).
// A monitor thread reads every thread's CPU time from /proc/self/task once a second; a thread that used at least kBusyShare of a core
// in that second is busy. Busy threads join the hint session (APerformanceHint_setThreads, API 34; on API 33 the session is made again
// when the set changes). The presenting thread reports each frame's interval against
// the target (1 / the phone's 30 fps limit): the platform's controller raises the session's clocks and core class until frames meet the target
// and lowers them when they beat it. Every APerformanceHint_* call is looked up at run time because the app's floor is API 30.
#if defined(__ANDROID__)
#include "nxrt/perf_hint.h"

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::perfhint {
namespace {

constexpr const char* kTag = "perfhint";
constexpr double kBusyShare = 0.05;
constexpr int64_t kTargetNs = 33333333;   // the phone's 30 fps limit
constexpr size_t kMaxThreads = 32;

struct Api {
    void* (*get_manager)() = nullptr;
    void* (*create)(void*, const int32_t*, size_t, int64_t) = nullptr;
    int (*report)(void*, int64_t) = nullptr;
    void (*close)(void*) = nullptr;
    int (*set_threads)(void*, const int32_t*, size_t) = nullptr;   // API 34
    bool load() {
        get_manager = reinterpret_cast<decltype(get_manager)>(dlsym(RTLD_DEFAULT, "APerformanceHint_getManager"));
        create = reinterpret_cast<decltype(create)>(dlsym(RTLD_DEFAULT, "APerformanceHint_createSession"));
        report = reinterpret_cast<decltype(report)>(dlsym(RTLD_DEFAULT, "APerformanceHint_reportActualWorkDuration"));
        close = reinterpret_cast<decltype(close)>(dlsym(RTLD_DEFAULT, "APerformanceHint_closeSession"));
        set_threads = reinterpret_cast<decltype(set_threads)>(dlsym(RTLD_DEFAULT, "APerformanceHint_setThreads"));
        return get_manager && create && report && close;
    }
};

Api g_api;
std::mutex g_mu;            // the session: made and changed by the monitor, reported to by the presenting thread
void* g_session = nullptr;

// utime + stime (clock ticks) of every thread of this process.
std::unordered_map<int32_t, long> thread_times() {
    std::unordered_map<int32_t, long> out;
    DIR* d = opendir("/proc/self/task");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        const int32_t tid = static_cast<int32_t>(atoi(e->d_name));
        if (tid <= 0) continue;
        char p[64], buf[512];
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
        FILE* f = fopen(p, "r");
        if (!f) continue;
        const size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        const char* s = strrchr(buf, ')');   // comm may hold spaces; fields after it are fixed: state is field 3, utime 14, stime 15
        if (!s) continue;
        unsigned long ut = 0, st = 0;
        if (sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2) out[tid] = static_cast<long>(ut + st);
    }
    closedir(d);
    return out;
}

void monitor() {
    if (!config_get_bool("sched", "phone_hint", true)) {
        NXLOG_I(kTag, "hint session off ([sched] phone_hint = false)");
        return;
    }
    void* const mgr = g_api.load() ? g_api.get_manager() : nullptr;
    NXLOG_I(kTag, "hint session %s (setThreads %s)", mgr ? "on" : "not on this Android version", g_api.set_threads ? "yes" : "no");
    if (!mgr) return;
    const long hz = sysconf(_SC_CLK_TCK);
    std::unordered_map<int32_t, long> last = thread_times();
    std::vector<int32_t> in_session;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::unordered_map<int32_t, long> now = thread_times();
        std::vector<int32_t> busy;
        for (const auto& [tid, t] : now) {
            const auto it = last.find(tid);
            if (it != last.end() && static_cast<double>(t - it->second) >= kBusyShare * static_cast<double>(hz)) busy.push_back(tid);
        }
        last = std::move(now);
        std::sort(busy.begin(), busy.end());
        if (busy.size() > kMaxThreads) busy.resize(kMaxThreads);

        std::lock_guard<std::mutex> lk(g_mu);
        if (busy.empty()) {
            if (g_session) { g_api.close(g_session); g_session = nullptr; in_session.clear(); }
            continue;
        }
        if (g_session && busy == in_session) continue;
        if (g_session && g_api.set_threads && g_api.set_threads(g_session, busy.data(), busy.size()) == 0) { in_session = busy; continue; }
        if (g_session) g_api.close(g_session);
        g_session = g_api.create(mgr, busy.data(), busy.size(), kTargetNs);
        in_session = g_session ? busy : std::vector<int32_t>{};
        if (!g_session) NXLOG_W(kTag, "APerformanceHint_createSession failed for %zu threads", busy.size());
    }
}

}  // namespace

void frame(uint64_t interval_ns) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(monitor).detach(); });
    if (interval_ns == 0 || interval_ns > 250000000ull) return;   // the first frame, or a hitch: no frame rate
    std::unique_lock<std::mutex> lk(g_mu, std::try_to_lock);
    if (lk.owns_lock() && g_session) g_api.report(g_session, static_cast<int64_t>(interval_ns));
}

}  // namespace nxrt::perfhint
#endif  // __ANDROID__
