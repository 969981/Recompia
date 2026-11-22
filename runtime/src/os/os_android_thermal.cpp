// SPDX-License-Identifier: GPL-3.0-or-later
// os_android_thermal.cpp: thermal status, temperatures, CPU clock ceiling and CPU use (os_android_thermal.h). Android only (the CMake
// filter in runtime/CMakeLists.txt, and the guard below). The thermal API is in libandroid (status: API 30; headroom: API 31, looked up
// at run time because the app's floor is 30) and needs no permission; cpufreq is world-readable sysfs. The thermal zones and the
// battery's power_supply files may be closed to apps by SELinux depending on the vendor: each is tried once and dropped when it fails.
#if defined(__ANDROID__)
#include "os_android_thermal.h"

#include <android/thermal.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/times.h>
#include <unistd.h>

#include <SDL3/SDL_system.h>   // SDL_GetAndroidJNIEnv, SDL_GetAndroidActivity: SDL owns the JVM attachment of each thread

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "nxrt/log.h"

namespace nxrt::os::thermal {
namespace {

constexpr const char* kTag = "thermal";

long read_long(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "re");
    if (!f) return LONG_MIN;
    long v = LONG_MIN;
    if (std::fscanf(f, "%ld", &v) != 1) v = LONG_MIN;
    std::fclose(f);
    return v;
}

std::string read_word(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "re");
    if (!f) return {};
    char b[64] = {};
    if (!std::fgets(b, sizeof b, f)) b[0] = 0;
    std::fclose(f);
    std::string s = b;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool plausible(float c) { return c > -20.f && c < 120.f; }

// The fastest core's ceiling file (the core with the highest cpuinfo_max_freq), found once.
const std::string& fastest_ceiling() {
    static const std::string path = [] {
        std::string best;
        long best_khz = 0;
        for (int i = 0; i < 32; ++i) {
            const std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
            const long m = read_long(dir + "cpuinfo_max_freq");
            if (m > best_khz) { best_khz = m; best = dir + "scaling_max_freq"; }
        }
        return best;
    }();
    return path;
}

// The skin sensor's thermal zone, found once: the zone whose type is a known skin sensor name (Pixel VIRTUAL-SKIN, Qualcomm
// skin-therm / sdm-skin-therm, Samsung skin_therm), else the first type containing "skin". Board sensors such as xo-therm are left
// out: they are not the skin. Empty when no type file is readable (SELinux) or no zone is the skin.
const std::string& skin_zone() {
    static const std::string path = [] {
        static constexpr const char* kNames[] = {"virtual-skin", "skin-therm", "skin_therm", "sdm-skin-therm", "skin"};
        std::string best, best_type;
        int best_rank = 99;
        for (int i = 0; i < 256; ++i) {
            const std::string dir = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/";
            std::string type = read_word(dir + "type");
            if (type.empty()) continue;
            std::string low = type;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            int rank = 99;
            for (int k = 0; k < static_cast<int>(std::size(kNames)); ++k)
                if (low == kNames[k]) { rank = k; break; }
            if (rank == 99 && low.find("skin") != std::string::npos) rank = static_cast<int>(std::size(kNames));
            if (rank < best_rank) { best_rank = rank; best = dir + "temp"; best_type = type; }
        }
        if (best.empty()) NXLOG_I(kTag, "skin temperature: no readable thermal zone names the skin sensor");
        else NXLOG_I(kTag, "skin temperature: %s (type %s)", best.c_str(), best_type.c_str());
        return best;
    }();
    return path;
}

float skin_c() {
    static bool dead = false;
    if (dead || skin_zone().empty()) return NAN;
    const long v = read_long(skin_zone());
    if (v == LONG_MIN) { dead = true; NXLOG_I(kTag, "skin temperature: %s not readable, dropped", skin_zone().c_str()); return NAN; }
    const float c = std::labs(v) >= 1000 ? static_cast<float>(v) / 1000.f : static_cast<float>(v);   // millidegrees on every known kernel
    return plausible(c) ? c : NAN;
}

// The sticky ACTION_BATTERY_CHANGED intent: Context.registerReceiver(null, IntentFilter(...)) returns it without registering anything
// (no permission, no receiver flag needed for this system broadcast). EXTRA_TEMPERATURE is in tenths of a degree C. Every JNI call
// is checked for a pending exception before the next one (CheckJNI aborts otherwise).
float battery_c_jni() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env || env->PushLocalFrame(16) != 0) { if (env) env->ExceptionClear(); return NAN; }
    float out = NAN;
    auto ok = [env] { if (!env->ExceptionCheck()) return true; env->ExceptionClear(); return false; };
    do {
        auto* act = static_cast<jobject>(SDL_GetAndroidActivity());
        if (!ok() || !act) break;
        jclass filter_cls = env->FindClass("android/content/IntentFilter");
        if (!ok() || !filter_cls) break;
        jmethodID filter_new = env->GetMethodID(filter_cls, "<init>", "(Ljava/lang/String;)V");
        if (!ok() || !filter_new) break;
        jstring action = env->NewStringUTF("android.intent.action.BATTERY_CHANGED");
        if (!ok()) break;
        jobject filter = env->NewObject(filter_cls, filter_new, action);
        if (!ok() || !filter) break;
        jmethodID reg = env->GetMethodID(env->GetObjectClass(act), "registerReceiver",
                                         "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;");
        if (!ok() || !reg) break;
        jobject intent = env->CallObjectMethod(act, reg, nullptr, filter);
        if (!ok() || !intent) break;
        jmethodID get = env->GetMethodID(env->GetObjectClass(intent), "getIntExtra", "(Ljava/lang/String;I)I");
        if (!ok() || !get) break;
        jstring key = env->NewStringUTF("temperature");
        if (!ok()) break;
        const jint t = env->CallIntMethod(intent, get, key, INT_MIN);
        if (!ok() || t == INT_MIN) break;
        out = static_cast<float>(t) / 10.f;
    } while (false);
    env->PopLocalFrame(nullptr);
    return plausible(out) ? out : NAN;
}

// The battery: power_supply sysfs (tenths of a degree C) while it is readable, else the intent above. The source is logged once.
float battery_c() {
    static int source = 0;   // 0 not tried, 1 sysfs, 2 JNI
    if (source <= 1) {
        const long v = read_long("/sys/class/power_supply/battery/temp");
        if (v != LONG_MIN && plausible(static_cast<float>(v) / 10.f)) {
            if (source == 0) NXLOG_I(kTag, "battery temperature: /sys/class/power_supply/battery/temp");
            source = 1;
            return static_cast<float>(v) / 10.f;
        }
        NXLOG_I(kTag, "battery temperature: power_supply sysfs not readable, using the ACTION_BATTERY_CHANGED intent");
        source = 2;
    }
    return battery_c_jni();
}

// This process's CPU time (all threads) since the previous call, in cores. times() counts in clock ticks for both.
float cpu_cores() {
    static clock_t last_wall = 0, last_cpu = 0;
    tms t{};
    const clock_t wall = times(&t);
    const clock_t cpu = t.tms_utime + t.tms_stime;
    float cores = -1.f;
    if (last_wall && wall > last_wall) cores = static_cast<float>(cpu - last_cpu) / static_cast<float>(wall - last_wall);
    last_wall = wall;
    last_cpu = cpu;
    return cores;
}

}  // namespace

bool read(Reading* out) {
    static AThermalManager* const mgr = AThermal_acquireManager();   // kept for the life of the process
    using HeadroomFn = float (*)(AThermalManager*, int);
    static const auto headroom = reinterpret_cast<HeadroomFn>(dlsym(RTLD_DEFAULT, "AThermal_getThermalHeadroom"));   // API 31+
    static float last_headroom = -1.f;   // NaN answers (called a little early, or warming up) keep the last value
    *out = Reading{};
    if (mgr) {
        const AThermalStatus s = AThermal_getCurrentThermalStatus(mgr);
        if (s >= ATHERMAL_STATUS_NONE) out->status = static_cast<int>(s);
        if (headroom) {
            const float h = headroom(mgr, 0);
            if (!std::isnan(h)) last_headroom = h;
            out->headroom = last_headroom;
        }
    }
    out->skin_c = skin_c();
    out->battery_c = battery_c();
    if (!fastest_ceiling().empty()) {
        const long khz = read_long(fastest_ceiling());
        if (khz > 0) out->cpu_cap_mhz = static_cast<int>(khz / 1000);
    }
    out->cpu_cores = cpu_cores();
    return out->status >= 0 || out->cpu_cap_mhz >= 0 || !std::isnan(out->battery_c);
}

}  // namespace nxrt::os::thermal
#endif
