// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play mode: render targets the game creates at a literal size (#44; split out of pkp_play.cpp).
#include "input/pkp_play_internal.h"
#include "core/pkp_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>

#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/text.h"

namespace nxrt::games::pkp::play_detail {

// ---- render targets created at a literal size (#44) ----
// 0x1796810 (x0 the device, w1 width, w2 height, w3 1, w4 format: 0 RGBA8, 5 D24S8, 107 R11G11B10F; w5, w6; it packs them and calls
// 0x179e0a0) creates a KTGL 2D render target. The render-size targets come from the level getters above; a handful of call sites pass
// their size as immediates instead, authored for the console's 1920x1080 frame. When such a target is drawn pixel for pixel against a
// render-size one, the two disagree at any other size. The local-light buffer is the case seen (#44): the light volumes (vs a70f6d6e,
// fs 1bc278d7) are drawn into it and read the G-buffer's depth (t381) and normals (t382) with texelFetch at gl_FragCoord, and the
// deferred lighting (fs 0cd3176a, 37c9671c, f9b9b006) samples it at screen UV. On a W x H frame the light lit for pixel (x, y) showed
// at (x W / 1920, y H / 1080): lamps, the PC terminal and the Pokemon Center door lamps glowed a second time below (16:10) or below and
// right (1440p) of themselves, shaped like whatever lay there. The entry hook on the create gives each listed call site the size its
// class says; the viewports those passes record as 1920x1080 (or a fraction of it) then follow through fix_rect, like every
// render-size target.
//
// Every call site with a literal size was read (121 callers; the rest take a size from a variable). Return addresses (bl + 4):
//   screen    follow the render size: the light set-up 0x10f41a8 (its object +0xb0 the local-light buffer, its twin +0xb8; +0x940 /
//             +0x948 the per-view frame copies, filled by a blit whose rects come from both targets' own sizes, 0x10f57b8) and the
//             snapshot 0x110d220 +0x170 (blitted from the frame the same way, 0x110dec0, and drawn back over a screen-size viewport).
//   fraction  960x540 (+0x80 the half-size scene colour, +0x88 its depth: written by the downsamples c4664b38 / 2650d480 at UV, read by
//             the transparent passes 096c2fdc, c0a3b861, fe074eda, 124d19d5, c01ba9e1 at UV) and 480x270 (+0x90, read by the deferred
//             lighting at UV through c3[416]; +0x98, the depth written by e562487e at UV). Every reader and writer seen samples them at
//             screen UV, so they are right at either size: they stay as authored unless [pkp] target_fix = all scales them by W/1920,
//             H/1080 (sharper, untested in the world: a pass that set its viewport outside the three viewport hooks would cover a corner).
//   canvas    the dialogue text target (0x1fb35f8: 1400x1024 RGBA8, its size read from a global the game fills at start-up): the text boxes are
//             laid out on the UI canvas, which is the render size on a 16:9 render size above 1920x1080, and their glyphs grow with it; the
//             target stayed 1400x1024, so the text did not fit, the box's cleared area came out as 764x85 or 1x1, the old line stayed under
//             the new one and the composite drew the target twice (#267). It is made as large as the canvas needs (1.5x at 2880x1620).
//   fixed     left as authored: the photo pipeline (0x10fd8e0: 2 x 1920x1080 captures read back by the CPU with a 7680-byte pitch, see
//             0x10fe828 / 0x10fffd4; 25 x 480x270, 2 x 320x180, 40 x 640x360 album images), the capture scratch 0x10fe140 (it scales the
//             frame into a 1920x1080 target and draws into it with a viewport of 1920.0 x 1080.0 written straight into the command
//             stream, which no viewport hook sees), the 240x135 thumbnails (0xb3b60, 0x110d220 +0x178), the 80x45 terrain occlusion
//             and 30x16 targets (sampled at UV, small by design), and the square shadow / cube / LUT targets (not screen-shaped).
// A 1920x1080 request from any other caller is logged once per caller and kept. [pkp] target_fix = off leaves every target as authored.
constexpr uint64_t kTextTargetCfg = 0x3d0a6b0;   // pointer to the game's dialogue text target settings (+0x37c on, +0x380 width, +0x384 height)
enum class TargetClass : uint8_t { Screen, Fraction, Fixed, Canvas };
struct TargetSite {
    uint32_t ret;          // return address of the call, offset in main
    uint16_t w, h;         // the literal size it passes
    TargetClass cls;
    const char* what;
};
constexpr TargetSite kTargetSites[] = {
    {0x10f4408, 1920, 1080, TargetClass::Screen, "local-light buffer"},
    {0x10f4440, 1920, 1080, TargetClass::Screen, "local-light buffer twin"},
    {0x10f4518, 1920, 1080, TargetClass::Screen, "frame copy, view 1"},
    {0x10f4554, 1920, 1080, TargetClass::Screen, "frame copy, view 2"},
    {0x110d330, 1920, 1080, TargetClass::Screen, "frame snapshot"},
    {0x10f4204, 960, 540, TargetClass::Fraction, "half-size scene colour"},
    {0x10f4228, 960, 540, TargetClass::Fraction, "half-size scene depth"},
    {0x10f4320, 480, 270, TargetClass::Fraction, "quarter-size lighting input"},
    {0x10f4358, 480, 270, TargetClass::Fraction, "quarter-size depth"},
    {0x10fd930, 1920, 1080, TargetClass::Fixed, "photo capture"},
    {0x10fd988, 1920, 1080, TargetClass::Fixed, "photo capture"},
    {0x10fe210, 1920, 1080, TargetClass::Fixed, "capture scratch"},
    {0x1fb35f8, 1400, 1024, TargetClass::Canvas, "dialogue text target"},
};
constexpr size_t kTargetSiteCount = sizeof(kTargetSites) / sizeof(kTargetSites[0]);
static_assert(kTargetSiteCount <= 32, "one log bit per site");

std::atomic<uint8_t> g_target_fix{static_cast<uint8_t>(TargetFix::Screen)};
std::atomic<uint32_t> g_target_logged{0};
std::atomic<uint64_t> g_target_unlisted[128]{}; // every possible caller (121 total) can be remembered; an unlisted 1920x1080 create logs once

TargetFix target_fix_from_config() {
    std::string v = settings::get_str(settings::Key::TargetFix);
    v = nxrt::text::lower(std::move(v));
    if (v == "off" || v == "0" || v == "false" || v == "no") return TargetFix::Off;
    if (v == "all") return TargetFix::All;
    return TargetFix::Screen;                                        // screen, on, 1, true, and anything unknown
}

// true the first time `site` comes here; the table covers all 121 callers of the create function
bool first_unlisted(uint64_t site) {
    for (auto& slot : g_target_unlisted) {
        uint64_t seen = slot.load(std::memory_order_relaxed);
        if (seen == 0 && slot.compare_exchange_strong(seen, site + 1, std::memory_order_relaxed)) return true;
        if (seen == site + 1) return false;
    }
    return true;
}

int hook_create_target(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const int W = s.w.load(std::memory_order_relaxed), H = s.h.load(std::memory_order_relaxed);
    if (W == kTableW && H == kTableH) return 0;                      // the console's frame: every target as authored
    const auto mode = static_cast<TargetFix>(g_target_fix.load(std::memory_order_relaxed));
    const uint32_t w = static_cast<uint32_t>(c->x[1]), h = static_cast<uint32_t>(c->x[2]);
    const uint64_t site = c->x[30] - s.base;
    size_t k = 0;
    while (k < kTargetSiteCount && !(kTargetSites[k].ret == site && kTargetSites[k].w == w && kTargetSites[k].h == h)) ++k;
    if (k == kTargetSiteCount) {
        if (w == static_cast<uint32_t>(kTableW) && h == static_cast<uint32_t>(kTableH) && first_unlisted(site))
            NXLOG_I(kTag, "render target: 1920x1080 kept (format %u, caller 0x%llx, not in the table)", static_cast<uint32_t>(c->x[4]),
                    static_cast<unsigned long long>(site));
        return 0;
    }
    const TargetSite& t = kTargetSites[k];
    uint32_t nw = w, nh = h;
    if (mode != TargetFix::Off && t.cls == TargetClass::Screen) {
        nw = static_cast<uint32_t>(W);
        nh = static_cast<uint32_t>(H);
    } else if (mode != TargetFix::Off && t.cls == TargetClass::Canvas && W > kTableW && H > kTableH &&
               std::fabs(static_cast<double>(W) / H - static_cast<double>(kTableW) / kTableH) < 0.01) {
        nw = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(w) * H / kTableH)));
        nh = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(h) * H / kTableH)));
        // the game keeps this size in a global (flag +0x37c, width +0x380, height +0x384) and reads it again for the target's UVs,
        // viewport and the box's dirty rectangle: the new size goes in there too, or the content shrinks inside the larger target
        const uint64_t cfg = rd<uint64_t>(s.base + kTextTargetCfg);
        if (cfg && mem::is_mapped(cfg + 0x380, 8) && rd<uint32_t>(cfg + 0x380) == w && rd<uint32_t>(cfg + 0x384) == h) {
            wr<uint32_t>(cfg + 0x380, nw);
            wr<uint32_t>(cfg + 0x384, nh);
        }
    } else if (mode == TargetFix::All && t.cls == TargetClass::Fraction) {
        nw = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(w) * W / kTableW)));
        nh = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(h) * H / kTableH)));
    }
    c->x[1] = nw;
    c->x[2] = nh;
    if (!(g_target_logged.fetch_or(1u << k, std::memory_order_relaxed) & (1u << k))) {
        if (nw != w || nh != h)
            NXLOG_I(kTag, "render target: %ux%u made %ux%u (%s, format %u, caller 0x%llx)", w, h, nw, nh, t.what,
                    static_cast<uint32_t>(c->x[4]), static_cast<unsigned long long>(site));
        else
            NXLOG_I(kTag, "render target: %ux%u kept (%s, format %u, caller 0x%llx)", w, h, t.what, static_cast<uint32_t>(c->x[4]),
                    static_cast<unsigned long long>(site));
    }
    return 0;
}

}  // namespace nxrt::games::pkp::play_detail
