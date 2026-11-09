// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus: the drawn set, from mid hooks on the pane draws (split out of pkp_menu.cpp).
//
// ---- the layouts the game draws (needs the midhooks in recomp/games/pkp/main.sites) ----
// Every KTGL pane class's draw (vtable slot +0x2b0; e.g. the picture pane 0x7101f5b248) starts the same way: it ORs 0x20 into the pane's
// flags (+0x104 = +260), then returns early unless flags bit 0x4000 (forced) or bit 0x100 (visible) with no ancestor flagged 0x40 (hidden,
// 0x7101f72780) and a nonzero effective alpha (+0x113 = +275); the visible ones then emit their primitives through the draw context (x1).
// Pooled layouts are never handed to the draw, so "a pane of this layout went through a draw this frame" is the drawn test. The hooks sit
// on each draw's flag store (pane in x0, x21 for 0x7101f77fe0); pane +216 is its layout (the draw reads [pane+0xd8]+0x2e0, the layout's
// renderer, as 0x7101f56180 does). The scan then keeps only layouts drawn in the last kDrawnFrames frames.
#include "pkp_menu_internal.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nxrt::games::pkp::menu_detail {

constexpr size_t kPaneLayout = 216;
constexpr uint32_t kDrawnFrames = 3;
constexpr size_t kDrawnSlots = 4096;
std::atomic<uint64_t> g_drawn_key[kDrawnSlots];
std::atomic<uint32_t> g_drawn_frame[kDrawnSlots];
std::atomic<uint32_t> g_frame{1};             // play mode frames (menu_in_game runs once per frame)
std::atomic<uint64_t> g_draw_calls{0};
std::vector<uint32_t> g_draw_hooks;

size_t drawn_slot(uint64_t lay) { return static_cast<size_t>((lay >> 4) * 0x9e3779b97f4a7c15ull >> 52) & (kDrawnSlots - 1); }

void drawn_mark(uint64_t lay, uint32_t frame) {
    size_t h = drawn_slot(lay);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kDrawnSlots - 1)) {
        uint64_t key = g_drawn_key[h].load(std::memory_order_relaxed);
        if (key == 0 && g_drawn_key[h].compare_exchange_strong(key, lay, std::memory_order_relaxed)) key = lay;
        if (key == lay) { g_drawn_frame[h].store(frame, std::memory_order_relaxed); return; }
    }
}

// The layout went through a pane draw in the last kDrawnFrames frames.
bool drawn_recent(uint64_t lay) {
    const uint32_t now = g_frame.load(std::memory_order_relaxed);
    size_t h = drawn_slot(lay);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kDrawnSlots - 1)) {
        const uint64_t key = g_drawn_key[h].load(std::memory_order_relaxed);
        if (key == 0) return false;
        if (key == lay) return now - g_drawn_frame[h].load(std::memory_order_relaxed) <= kDrawnFrames;
    }
    return false;
}

bool draw_hooks_live() { return g_draw_calls.load(std::memory_order_relaxed) != 0; }

// Each pane that went through a draw: the frame, and the world matrix, size and anchor as the draw read them (the rect is the drawn one,
// whatever parent or animation moved it). Written by the drawing thread (the data, then the frame), read by the scan.
struct DrawnPane {
    std::atomic<uint64_t> key{0};
    std::atomic<uint32_t> frame{0};
    float m[16];
    uint32_t size = 0;
    uint8_t anchor = 0;
};
constexpr size_t kPaneSlots = 16384;
DrawnPane g_drawn_panes[kPaneSlots];
std::atomic<uint64_t> g_pane_table_full{0};      // marks dropped: 32 probes found no slot

size_t pane_slot(uint64_t pane) { return static_cast<size_t>((pane >> 3) * 0x9e3779b97f4a7c15ull >> 50) & (kPaneSlots - 1); }

bool pane_mark(uint64_t pane, uint32_t frame) {
    size_t h = pane_slot(pane);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kPaneSlots - 1)) {
        DrawnPane& d = g_drawn_panes[h];
        uint64_t key = d.key.load(std::memory_order_relaxed);
        if (key == 0 && d.key.compare_exchange_strong(key, pane, std::memory_order_relaxed)) key = pane;
        if (key != pane) continue;
        if (d.frame.load(std::memory_order_relaxed) == frame) return true;
        const uint64_t mp = *reinterpret_cast<const uint64_t*>(pane + kPaneWorld);   // the draw builds its corners from this matrix
        if (mp) std::memcpy(d.m, reinterpret_cast<const void*>(mp), sizeof d.m);
        d.size = *reinterpret_cast<const uint32_t*>(pane + kPaneSize);
        d.anchor = *reinterpret_cast<const uint8_t*>(pane + kPaneAnchor);
        d.frame.store(mp ? frame : 0, std::memory_order_release);
        return false;
    }
    g_pane_table_full.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// The pane's draw record: kNoRecord, kStale (drawn, but more than kDrawnFrames frames ago; *age set) or kFresh (*out = its drawn rectangle).
int pane_record(uint64_t pane, Pane* out, uint32_t* age) {
    const uint32_t now = g_frame.load(std::memory_order_relaxed);
    size_t h = pane_slot(pane);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kPaneSlots - 1)) {
        const DrawnPane& d = g_drawn_panes[h];
        const uint64_t key = d.key.load(std::memory_order_relaxed);
        if (key == 0) return kNoRecord;
        if (key != pane) continue;
        const uint32_t f = d.frame.load(std::memory_order_acquire);
        if (f == 0) return kNoRecord;
        *age = now - f;
        if (*age > kDrawnFrames) return kStale;
        float m[16];
        std::memcpy(m, d.m, sizeof m);
        for (float v : m)
            if (!std::isfinite(v)) return kStale;
        return rect_from(m, m[14], d.size, d.anchor, out) ? kFresh : kStale;
    }
    return kNoRecord;
}

// A drawn pane and its ancestors: entries and cursors are mostly Null panes and part instances ('CL_...') that hold the drawn pictures and
// texts but have no draw of their own (p5j: marking only the drawing panes left 2-4 visible panes on screens with dozens). An ancestor
// already marked this frame has its own ancestors marked too.
void mark_with_ancestors(uint64_t pane, uint32_t frame) {
    for (int depth = 0; pane && depth < 64; ++depth) {
        if (pane_mark(pane, frame)) return;
        pane = *reinterpret_cast<const uint64_t*>(pane + kPaneParent);
    }
}

int hook_pane_draw(::Ctx* c, uint64_t, void* user) {
    const uint64_t pane = c->x[reinterpret_cast<uintptr_t>(user)];
    const uint32_t frame = g_frame.load(std::memory_order_relaxed);
    thread_local uint64_t last_pane_lay = 0;
    thread_local uint32_t last_frame = 0;
    if (!pane) return 0;
    const uint64_t lay = *reinterpret_cast<const uint64_t*>(pane + kPaneLayout);    // the draw itself reads it next
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    if (!(*reinterpret_cast<const uint32_t*>(pane + kPaneFlags) & 0x4000)) {
        // the draw goes on only for panes it shows (0x100, no hidden ancestor, alpha); record those
        const uint32_t fl = *reinterpret_cast<const uint32_t*>(pane + kPaneFlags);
        bool shown = (fl & 0x100) && *reinterpret_cast<const uint8_t*>(pane + kPaneAlpha) != 0;
        for (uint64_t q = pane; shown && q; q = *reinterpret_cast<const uint64_t*>(q + kPaneParent))
            if (*reinterpret_cast<const uint32_t*>(q + kPaneFlags) & 0x40) shown = false;
        if (shown) mark_with_ancestors(pane, frame);
    } else {
        mark_with_ancestors(pane, frame);
    }
    if (!lay || (lay == last_pane_lay && frame == last_frame)) return 0;
    last_pane_lay = lay;
    last_frame = frame;
    drawn_mark(lay, frame);
    return 0;
}

// The game's own visibility rule for a pane (the draw's checks above, without the draw-list membership).
bool pane_shown(uint64_t pane, uint32_t flags) {
    if (flags & 0x4000) return true;
    if (!(flags & 0x100)) return false;
    uint64_t q = pane;
    for (int depth = 0; q && depth < 64; ++depth) {
        if (!ok(q + kPaneFlags, 4)) return false;
        if (rd<uint32_t>(q + kPaneFlags) & 0x40) return false;
        q = ok(q + kPaneParent, 8) ? rd<uint64_t>(q + kPaneParent) : 0;
    }
    return true;
}

}  // namespace nxrt::games::pkp::menu_detail
