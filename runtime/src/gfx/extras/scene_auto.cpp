// SPDX-License-Identifier: GPL-3.0-or-later
// [upscale] scene_auto: automatic scene resolution. The scene percent (upscale_scene_scale, the game's own dynamic-resolution path: the scene
// renders into the top-left part of its targets and the stretch enlarges it, per frame, no reallocation) follows the GPU: lowered in steps of
// 5 when the frames run late while the GPU is busy, raised again when the frames are on time with the GPU clearly idle. The player's
// scene_scale stays the ceiling, [upscale] scene_auto_min the floor. Needs the kernel's GPU busy figure (diag::perf_gpu_load: Adreno's kgsl);
// without it the percent stays the player's. Render thread only (scene_auto_tick at each present).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/game.h"
#include "nxrt/game_render.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"
#include "nxrt/platform_facts.h"
#include "../../diag/perf_system.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "scene_auto";
std::atomic<int> g_auto_pct{0};   // 0 = off or not known: upscale_scene_scale gives the player's value
std::atomic<int> g_watch_pct{0};  // [upscale] scene_scale_watch: the percent last read from the file (0 = none)
std::atomic<int> g_watch_up{-1};  // [gfx] scene_upscale_watch: 0 off, 1 sgsr, 2 bicubic from the file (-1 = none: the key's value)

struct Ctl {
    bool on = false, have_load = true;
    int floor = 50;
    double target_ms = 1000.0 / 30.0;
    std::chrono::steady_clock::time_point last, window_start;
    double sum_ms = 0;
    int frames = 0, calm = 0;
    std::string watch;                                  // [upscale] scene_scale_watch: a file holding a percent, read twice a second
    std::string up_watch;                               // [gfx] scene_upscale_watch: a file holding off | sgsr | bicubic, read twice a second
    std::chrono::steady_clock::time_point watch_last;
};
Ctl& ctl() {
    static Ctl c = [] {
        Ctl k;
        k.on = config_get_bool("upscale", "scene_auto", false);
        const game::Render* gr = game::info().render;
        const int lowest = gr ? std::clamp(gr->scene_scale_min, 25, 100) : 25;   // the game's lowest correct percent
        k.floor = static_cast<int>(std::clamp<int64_t>(config_get_int("upscale", "scene_auto_min", 50), lowest, 100));
        const int64_t fps = config_get_int("upscale", "scene_auto_fps", 30);
        k.target_ms = 1000.0 / static_cast<double>(std::clamp<int64_t>(fps, 10, 240));
        // a developer key for test harnesses (not a player setting, no settings row): the file is read only, a number in it is the
        // live percent; the players' control is [upscale] scene_scale (the in-game Resolution row on a phone)
        k.watch = config_get_str("upscale", "scene_scale_watch", "");
        k.up_watch = config_get_str("gfx", "scene_upscale_watch", "");   // the same kind of developer key, for the scene upscaler
        if (!k.up_watch.empty()) NXLOG_I(kTag, "live scene upscaler from %s ([gfx] scene_upscale_watch; write off, sgsr or bicubic into it)", k.up_watch.c_str());
        if (!k.watch.empty()) NXLOG_I(kTag, "live scene percent from %s ([upscale] scene_scale_watch; write 25..100 into it)", k.watch.c_str());
        if (k.on) NXLOG_I(kTag, "automatic scene resolution: on, %d%% .. the player's scene_scale, target %.1f ms ([upscale] scene_auto)", k.floor, k.target_ms);
        return k;
    }();
    return c;
}
}  // namespace

int scene_scale_auto() { return g_auto_pct.load(std::memory_order_relaxed); }
int scene_scale_watch() { return g_watch_pct.load(std::memory_order_relaxed); }
int scene_upscale_watch() { return g_watch_up.load(std::memory_order_relaxed); }

// Testing aid: a percent written into the watched file applies at the next half second, in the running game (a test harness on a phone
// writes it with adb; a desktop tester with echo). It replaces the player's percent as the ceiling until the file is removed.
static void watch_tick(Ctl& c, std::chrono::steady_clock::time_point now) {
    if ((c.watch.empty() && c.up_watch.empty()) || std::chrono::duration<double>(now - c.watch_last).count() < 0.5) return;
    c.watch_last = now;
    if (!c.up_watch.empty()) {
        char w[16] = {};
        int u = -1;
        if (FILE* f = std::fopen(c.up_watch.c_str(), "r")) {
            if (std::fscanf(f, "%15s", w) == 1) u = !std::strcmp(w, "sgsr") ? 1 : !std::strcmp(w, "bicubic") ? 2 : 0;
            std::fclose(f);
        }
        if (g_watch_up.exchange(u, std::memory_order_relaxed) != u) {
            NXLOG_I(kTag, "live scene upscaler: %s", u < 0 ? "file gone: the key's again" : u == 1 ? "sgsr" : u == 2 ? "bicubic" : "off");
            if (u >= 0) notices::post("scene_upscale", u == 1 ? "Scene upscaler: SGSR" : u == 2 ? "Scene upscaler: bicubic" : "Scene upscaler: off", 2.0);
        }
    }
    if (c.watch.empty()) return;
    int v = 0;
    if (FILE* f = std::fopen(c.watch.c_str(), "r")) {
        if (std::fscanf(f, "%d", &v) != 1) v = 0;
        std::fclose(f);
    }
    v = v > 0 ? std::clamp(v, facts::kPhone ? 1 : 25, 100) : 0;   // a phone: its percent of the screen, which goes lower (the game layer clamps it)
    if (g_watch_pct.exchange(v, std::memory_order_relaxed) != v) NXLOG_I(kTag, "live scene percent %d%% (%s)", v, v ? "from the watched file" : "file gone: the player's again");
}

void scene_auto_tick(int player_pct) {
    Ctl& c = ctl();
    const auto now = std::chrono::steady_clock::now();
    watch_tick(c, now);
    if (const int w = g_watch_pct.load(std::memory_order_relaxed); w > 0) player_pct = w;
    if (!c.on || !c.have_load) return;
    if (c.frames == 0 && c.sum_ms == 0) c.window_start = now;
    if (c.last.time_since_epoch().count()) {
        const double ms = std::chrono::duration<double, std::milli>(now - c.last).count();
        if (ms < 500) { c.sum_ms += ms; ++c.frames; }   // a load screen or a hidden app is not a slow frame
    }
    c.last = now;
    if (std::chrono::duration<double>(now - c.window_start).count() < 0.5 || c.frames < 5) return;
    const double mean = c.sum_ms / c.frames;
    c.sum_ms = 0;
    c.frames = 0;
    c.window_start = now;
    diag::GpuLoad g;
    if (!diag::perf_gpu_load(&g)) {
        c.have_load = false;
        NXLOG_I(kTag, "no GPU busy figure from the kernel here: the scene percent stays the player's");
        return;
    }
    const int ceil = std::clamp(player_pct, 25, 100), floor = std::min(c.floor, ceil);
    int pct = g_auto_pct.load(std::memory_order_relaxed);
    if (pct == 0) pct = ceil;
    const int before = pct;
    // late and GPU-bound: down at once; on time with room to spare for 3 windows (1.5 s): up one step (hysteresis between the two)
    if (mean > c.target_ms * 1.04 && g.busy > 0.85) {
        pct -= 5;
        c.calm = 0;
    } else if (mean <= c.target_ms * 1.01 && g.busy < 0.70) {
        if (++c.calm >= 3) { pct += 5; c.calm = 0; }
    } else {
        c.calm = 0;
    }
    pct = std::clamp(pct, floor, ceil);
    if (pct != before)
        NXLOG_I(kTag, "scene %d%% -> %d%% (frames %.1f ms, GPU %.0f%% at %d of %d MHz)", before, pct, mean, g.busy * 100.0, g.mhz, g.max_mhz);
    g_auto_pct.store(pct, std::memory_order_relaxed);
}

}  // namespace nxrt::extras
