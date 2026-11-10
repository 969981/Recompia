// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the engine's own temporal anti-aliasing (pkp_taa.h). Offsets in `main` of the 2.0.1 update (base 0x7100000000).
//
// What the engine has (KTGL): render passes TemporalAA (plTemporalAAConstants), TemporalAA2, TemporalAA3 (plTemporalAA3Constants, pass vtable
// 0x71033ecef0, slot names TAA3InputColor / InputDepth / OutputColor / Work1..2 / DepthWork1..2, pass parameter "TemporalAA3Enable") and KTSS
// (an accumulate pass with dilated velocity and depth), velocity passes (MakeVelocityMap, DecodeVelocityMap, GBufferVelocity*), and a
// projection jitter. The TAA shaders are in the romfs archives, but none of them is loaded in play: of the 6073 shaders a gameplay run
// used, none binds a velocity map, a history map or a TAA constant block.
//
// Camera switch: the camera parameter block's TAA enable. In the kids camera (describe 0x71009d3010) LensSimulationTemporalAAEnable is the int
// at +0x171c, LensSimulationTemporalAAType +0x1720, ...Jitter +0x1724, ...JitterPatternNum +0x172c; the block the renderer reads holds the same
// fields 0x300 higher (+0x1a1c enable, +0x1a20 type, +0x1a2c pattern count).
// The jitter (0x71008b6254, at 0x7102574a18): x9 = the per-view parameter block ([views + 8 i + 0x150], loaded at 0x7102571fd8, or a defaults
// copy on the stack when that is null). When its enable >= 1: index = frame % max(pattern count, 1) + 1, jitter = (2 Halton(index, 2) - 1) /
// width, (2 Halton(index, 3) - 1) / height (0x7101e585c0 is the radical inverse), added to the projection of the view object x25 (+0xc0..),
// stored in that view object at +0x1a24 / +0x1a28, and the view-projection rebuilt. The post parameter setter 0x7102581580 sets the shader flag
// TemporalAAEnable = (enable >= 1 && type == 0). With a pattern count of 0 or 1 the jitter is always 0.
//
// Pass switch: the TemporalAA3 pass has its own parameter "TemporalAA3Enable" (descriptor in the pass vtable data at 0x71033ecf60: type 6,
// default 0). Its render function 0x7101e5389c (vtable slot 0x71033ecf40) does nothing unless the byte at pass +0xc is set; its slot setup
// 0x7101e53748 (slot 0x71033ecf18) resolves the TAA3 targets by name (TAA3InputColor ... at pass +79*4 ..).
//
// NVN DLSS: the nvnDlss* procs are only fetched by the SDK's nvnLoadCProcs (0x71011456a0); the game never calls them.
//
// Run b4 (2026-11-02, game_taa = 1): the copy site 0x7102126a88 is never reached in play. The jitter site is (block 791a382270: the game's
// enable 0, type 0, pattern 16); the override makes the jitter branch run (the "jitter active" line counts frames with enable >= 1 as the
// game then reads it). The jitter values in that run's log were read from the parameter block, which holds none; they live in the view
// object x25, which the probe now reads. No TAA shader loaded: the pass switch above was not touched.
//
// Hooks (main.sites):
//   0x7102126a88  mid, in the camera block copy 0x7102123e80 (x20 = source block), before it reads +0x1a1c (not reached in play; kept as a probe).
//   0x7102574a18  mid, in the jitter step (x9 = the parameter block, x25 = the view object), before the game reads +0x1a1c.
//   0x7101e5389c  entry, TemporalAA3 render (x0 = the pass): logs once, counts, and sets the pass switch with game_taa 2.
// The log-only hooks (TemporalAA3, TemporalAA (v1) and KTSS slot setup) are in pkp_taa_probe.cpp, built with the test tools only.
// [pkp] game_taa = 1 (default 0: not proven) writes enable = 1 and, when the pattern count is below 2, 8 into the parameter block before the
// game reads it; 2 also sets the TemporalAA3 pass switch at each of its render calls (only meaningful when the probe shows the pass exists).
#include "render/pkp_taa.h"
#include "core/pkp_guest.h"
#include "core/pkp_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nxrt/extras.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kCopySite = 0x2126a88;     // camera block copy 0x7102123e80, x20 = source
constexpr uint64_t kJitterSite = 0x2574a18;   // jitter step in 0x71008b6254, x9 = the parameter block, x25 = the view object
constexpr uint64_t kTaa3Render = 0x1e5389c;   // entry: TemporalAA3 pass render (x0 = pass; nothing happens unless byte +0xc)
constexpr uint64_t kEnable = 0x1a1c, kType = 0x1a20, kPattern = 0x1a2c;   // parameter block
constexpr uint64_t kViewJitX = 0x1a24, kViewJitY = 0x1a28;                // view object
constexpr uint64_t kPassSwitch = 0xc, kPassSlots = 79 * 4, kPassSize = 0x168;
constexpr int32_t kForcedPattern = 8;

struct State {
    bool installed = false;
    int force = 0;   // [pkp] game_taa: 1 the camera TAA enable, 2 also the TemporalAA3 pass switch
    std::atomic<bool> copy_logged{false}, jitter_logged{false}, jitter_on_logged{false}, taa3_render_logged{false};
    std::atomic<uint64_t> copies{0}, jitter_calls{0}, jitter_on{0}, taa3_renders{0};
    std::atomic<int32_t> game_enable{-1}, game_type{-1}, game_pattern{-1};
};
State& st() {
    static State s;
    return s;
}

using guest::rd;
using guest::wr;

// Writes enable = 1 and a usable pattern count into a parameter block when [pkp] game_taa is on.
void force_on(uint64_t blk) {
    wr<int32_t>(blk + kEnable, 1);
    if (rd<int32_t>(blk + kPattern) < 2) wr<int32_t>(blk + kPattern, kForcedPattern);
}

// [pkp] game_taa only. A temporal upscaler does not switch it on: run b7 (enable forced, pattern 16) left the G-buffer camera unjittered, so
// the runtime jitters the scene camera itself (extras::temporal_jitter_block). The jitter step 0x7102574a18 writes (2 Halton - 1) / size into
// x25 +0x1a24 / +0x1a28 and into x25's projection (+0xc0..0xff, view-projection rebuilt at +0x100); x25 was the same object as the parameter
// block x9 in b7, and +0x1a24 is also the copied camera parameter "LensSimulationTemporalAAJitter" (+0x1724 in the kids camera), so the value
// read at the hook is the copy, not last frame's jitter. Where that projection goes was not traced.
bool forced() { return st().force != 0; }

void note_game_values(uint64_t blk) {
    State& s = st();
    s.game_enable.store(rd<int32_t>(blk + kEnable));
    s.game_type.store(rd<int32_t>(blk + kType));
    s.game_pattern.store(rd<int32_t>(blk + kPattern));
}

int hook_copy(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t src = c->x[20];
    if (!src || !mem::is_mapped(src, kPattern + 4)) return 0;
    s.copies.fetch_add(1, std::memory_order_relaxed);
    if (!s.copy_logged.exchange(true))
        NXLOG_I(kTag, "taa: camera block copy, game values: enable %d type %d jitter pattern %d", rd<int32_t>(src + kEnable), rd<int32_t>(src + kType),
                rd<int32_t>(src + kPattern));
    if (forced()) force_on(src);
    return 0;
}

int hook_jitter(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t blk = c->x[9];
    if (!blk || !mem::is_mapped(blk, kPattern + 4)) return 0;
    s.jitter_calls.fetch_add(1, std::memory_order_relaxed);
    if (!s.jitter_logged.exchange(true)) {
        note_game_values(blk);
        NXLOG_I(kTag, "taa: jitter step reached, parameter block %llx: game values enable %d type %d pattern %d%s", static_cast<unsigned long long>(blk),
                s.game_enable.load(), s.game_type.load(), s.game_pattern.load(), s.force ? " (enable overridden to 1 before the game reads it)" : "");
    }
    if (forced()) force_on(blk);
    if (rd<int32_t>(blk + kEnable) >= 1) {
        const uint64_t n = s.jitter_on.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t view = c->x[25];   // holds the previous frame's jitter (the game stores the new one after this point)
        if (view && mem::is_mapped(view, kViewJitY + 4)) extras::temporal_note_game_jitter(rd<float>(view + kViewJitX), rd<float>(view + kViewJitY));
        if (n == 16 && !s.jitter_on_logged.exchange(true) && view && mem::is_mapped(view, kViewJitY + 4))
            NXLOG_I(kTag, "taa: jitter active (16 frames), view %llx, last jitter %.6f %.6f", static_cast<unsigned long long>(view),
                    static_cast<double>(rd<float>(view + kViewJitX)), static_cast<double>(rd<float>(view + kViewJitY)));
    }
    return 0;
}

int hook_taa3_render(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t p = c->x[0];
    if (!p || !mem::is_mapped(p, kPassSize)) return 0;
    s.taa3_renders.fetch_add(1, std::memory_order_relaxed);
    if (!s.taa3_render_logged.exchange(true))
        NXLOG_I(kTag, "taa: TemporalAA3 render called (pass %llx): switch byte %u, colour / depth / output slot ids %d %d %d%s", static_cast<unsigned long long>(p),
                rd<uint8_t>(p + kPassSwitch), rd<int32_t>(p + kPassSlots), rd<int32_t>(p + kPassSlots + 4), rd<int32_t>(p + kPassSlots + 8),
                s.force >= 2 ? " (game_taa 2: switch forced on)" : "");
    if (s.force >= 2) wr<uint8_t>(p + kPassSwitch, 1);
    return 0;
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.installed) return;
    s.force = static_cast<int>(settings::get_int(settings::Key::GameTaa));
    in.hooks.add_mid(in.base + kCopySite, &hook_copy);
    in.hooks.add_mid(in.base + kJitterSite, &hook_jitter);
    in.hooks.add_entry(in.base + kTaa3Render, &hook_taa3_render);
    if (s.force) NXLOG_I(kTag, "taa: [pkp] game_taa = %d: the engine's temporal AA is switched on (experimental)", s.force);
    s.installed = true;
}

void remove() { st().installed = false; }   // the registry has removed the hooks

// (the leading space separates it from the fields before it in the status line)
void status_text(char* buf, size_t n) {
    State& s = st();
    std::snprintf(buf, n, " game TAA %d: game enable %d type %d pattern %d; copies %llu, jitter steps %llu (TAA on %llu), TAA3 renders %llu", s.force,
                  s.game_enable.load(), s.game_type.load(), s.game_pattern.load(), static_cast<unsigned long long>(s.copies.load()),
                  static_cast<unsigned long long>(s.jitter_calls.load()), static_cast<unsigned long long>(s.jitter_on.load()),
                  static_cast<unsigned long long>(s.taa3_renders.load()));
}

}  // namespace

const Feature kTaaFeature = {.name = "taa", .install = &install, .remove = &remove, .status_text = &status_text};

}  // namespace nxrt::games::pkp
