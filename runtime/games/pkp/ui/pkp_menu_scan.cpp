// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus: the scan of the layouts the UI manager's pools hold and their visible panes (split out of
// pkp_menu.cpp).
#include "ui/pkp_menu_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/text.h"

namespace nxrt::games::pkp::menu_detail {

// The range lies in guest memory that can be read (cached per 4 KiB page for one scan).
bool ok(uint64_t a, uint64_t n) {
    // guest space: the heap and stacks (0x78........) and main's image at 0x7100000000 (the UI manager pointer lives there)
    if (a < 0x1000000000ull || a + n > 0x8000000000ull || n == 0 || n > 0x10000) return false;
    State& s = st();
    for (uint64_t p = a & ~0xfffull; p < a + n; p += 0x1000) {
        auto it = s.pages.find(p);
        bool m;
        if (it != s.pages.end()) m = it->second;
        else {
            m = mem::is_mapped(p, 0x1000);
            s.pages.emplace(p, m);
        }
        if (!m) return false;
    }
    return true;
}

uint64_t rptr(uint64_t a) { return ok(a, 8) ? rd<uint64_t>(a) : 0; }

void read_str(uint64_t p, size_t len_hint, char* out, size_t n) {
    out[0] = 0;
    if (!p) return;
    size_t len = len_hint ? std::min(len_hint, n - 1) : n - 1;
    if (!ok(p, len)) {
        len = 0;
        while (len < n - 1 && ok(p + len, 1) && rd<char>(p + len)) ++len;
    }
    size_t i = 0;
    for (; i < len; ++i) {
        const char c = rd<char>(p + i);
        if (!c) break;
        out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    out[i] = 0;
}

bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

void classify(Pane& p, const char* tag) {
    std::string n = p.name;
    n += '|';
    n += tag;
    n = nxrt::text::lower(std::move(n));
    p.cursor = has(n, "cursor") && !has(n, "mouse");
    p.slider = has(n, "slider") || has(n, "slide_bar");
    static const char* const kItem[] = {"btn", "button", "item", "cell", "choice", "list", "tab", "slot", "chip", "select", "menu"};
    static const char* const kNot[] = {"cursor", "txt", "text", "icon", "shadow", "_bg", "bg_", "frame", "base", "line", "effect", "eff_"};
    bool item = false;
    for (const char* k : kItem) item = item || has(n, k);
    for (const char* k : kNot) item = item && !has(n, k);
    p.item = item;
}

// One pane's rectangle on the canvas (the anchor rule of DoPaneHitTest's box, 0x2645000): the pane's origin is its left / centre / right
// edge by the anchor's high nibble (0 / 1 / 2) and its top / middle / bottom by the low nibble; x and y axes from the world matrix.
// The rectangle from the world matrix's X axis (m0, m1), Y axis (m4, m5) and translation (m12, m13), the size and the anchor: the corners the
// pane draws build (0x7101f53010: the same anchor offsets, then the matrix, then the UI camera).
bool rect_from(const float* m, float m14, uint32_t size, uint8_t anchor, Pane* out) {
    const float w = static_cast<float>(size & 0xffff), h = static_cast<float>(size >> 16);
    if (w <= 0.f || h <= 0.f) return false;
    const int ah = anchor >> 4, av = anchor & 0xf;
    const float ox = ah == 0 ? w * 0.5f : (ah == 2 ? -w * 0.5f : 0.f);
    const float oy = av == 0 ? -h * 0.5f : (av == 2 ? h * 0.5f : 0.f);
    const float cx = m[12] + m[0] * ox + m[4] * oy, cy = m[13] + m[1] * ox + m[5] * oy;
    const float hx = (std::fabs(m[0]) * w + std::fabs(m[4]) * h) * 0.5f, hy = (std::fabs(m[1]) * w + std::fabs(m[5]) * h) * 0.5f;
    out->x0 = kCanvasW * 0.5f + cx - hx;
    out->x1 = kCanvasW * 0.5f + cx + hx;
    out->y0 = kCanvasH * 0.5f - cy - hy;
    out->y1 = kCanvasH * 0.5f - cy + hy;
    out->z = m14;
    return true;
}

bool pane_rect(uint64_t pane, Pane* out) {
    if (!ok(pane, kPaneAlpha + 1)) return false;
    const uint64_t mp = rd<uint64_t>(pane + kPaneWorld);
    if (!ok(mp, 64)) return false;
    float m[16];
    std::memcpy(m, reinterpret_cast<const void*>(mp), sizeof m);
    for (float v : m)
        if (!std::isfinite(v)) return false;
    if (!rect_from(m, m[14], rd<uint32_t>(pane + kPaneSize), rd<uint8_t>(pane + kPaneAnchor), out)) return false;
    out->alpha = rd<uint8_t>(pane + kPaneAlpha);
    out->flags = rd<uint32_t>(pane + kPaneFlags);
    return true;
}

bool log_ok(bool force) {
    State& s = st();
    if (s.log_level <= 0 || s.log_lines > 4000) return false;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - s.last_log < std::chrono::milliseconds(200)) return false;
    s.last_log = now;
    ++s.log_lines;
    return true;
}

// [pkp] menu_log = 3: the chain from the UI manager global down to the panes, once a second while scanning.
bool g_trace = false;
bool g_trace_pick = false;      // the scan traced: pick() logs its choice once
#define TRACE(...) do { if (g_trace) NXLOG_I(kTag, "menu chain: " __VA_ARGS__); } while (0)

// One layout's visible panes (layout = placeable +40 -> +16).
void add_layout(uint64_t plc, uint32_t id, uint32_t sub, int trace_n) {
    State& s = st();
    const uint64_t obj = rptr(plc + kPlaceableObj);
    const uint64_t lay = rptr(obj + kObjLayout);
    if (!lay || !ok(lay, kLaySentinel + 8)) {
        if (trace_n < 4) TRACE("    placeable 0x%llx -> obj 0x%llx -> layout 0x%llx: stop (layout not readable)", (unsigned long long)plc,
                               (unsigned long long)obj, (unsigned long long)lay);
        return;
    }
    for (const Layout& L : s.layouts)
        if (L.ptr == lay) return;
    if (draw_hooks_live() && !drawn_recent(lay)) { ++s.n_not_drawn; return; }
    Layout L;
    L.ptr = lay;
    L.id = id;
    L.sub = sub;
    const int n = rd<uint16_t>(lay + kLayPaneCount);
    uint64_t node = rd<uint64_t>(lay + kLayFirstNode);
    const uint64_t first = node;
    const int li = static_cast<int>(s.layouts.size());
    int unreadable = 0, alpha0 = 0, offscreen = 0;
    // the candidates: the panes the draw's own rule shows (flag 0x100 / 0x4000, no hidden ancestor, alpha above 0), each with its draw record
    struct Cand { uint64_t pane; Pane p; int rec; };
    std::vector<Cand> cands;
    const bool live = draw_hooks_live();
    int fresh = 0;
    for (int k = 0; k < n && k < static_cast<int>(kMaxPanes) && node && node != lay + kLaySentinel; ++k) {
        if (!ok(node, 24)) break;
        const uint64_t pane = rd<uint64_t>(node + 16);
        node = rd<uint64_t>(node + 8);
        ++L.panes;
        Pane p;
        if (!pane_rect(pane, &p)) { ++unreadable; continue; }
        if (p.alpha == 0 || !pane_shown(pane, p.flags)) { ++alpha0; continue; }
        if (!live && !(p.flags & 0x20)) { ++alpha0; continue; }     // no hooks (not re-lifted): never went through a draw
        Cand cd{pane, p, kNoRecord};
        if (live) {
            ++s.rec_panes;
            Pane r;
            uint32_t age = 0;
            cd.rec = pane_record(pane, &r, &age);
            if (cd.rec != kNoRecord) ++s.rec_with;
            if (cd.rec == kStale) s.rec_ages.push_back(age);
            if (cd.rec == kFresh) {
                ++fresh;
                cd.p.x0 = r.x0; cd.p.y0 = r.y0; cd.p.x1 = r.x1; cd.p.y1 = r.y1; cd.p.z = r.z;     // the rect it was drawn at
            }
        }
        cands.push_back(cd);
    }
    // With fresh records in this layout, its panes without one (or with an old one) were not drawn: dropped. Without any (a pane class the
    // hooks do not cover, a draw on another cadence), the layout-level filter alone decides, as before the per-pane records.
    // Only when most of the layout's shown panes have records: some pane classes draw without going through the hooked draws (the bag's
    // empty slots: p5k kept 4 of ~70 inventory panes), and dropping their panes would lose the menu.
    const bool refine = live && fresh > 0 && fresh * 2 >= static_cast<int>(cands.size());
    if (live && !refine && !cands.empty()) ++s.rec_fallback_layouts;
    for (Cand& cd : cands) {
        if (refine && cd.rec != kFresh) { ++alpha0; continue; }
        const uint64_t pane = cd.pane;
        Pane& p = cd.p;
        if (p.x1 < 0.f || p.y1 < 0.f || p.x0 > kCanvasW || p.y0 > kCanvasH) { ++offscreen; continue; }     // off screen (or a world-space layout)
        p.ptr = pane;
        p.lay = li;
        p.lay_id = L.id;
        p.lay_sub = L.sub;
        read_str(rd<uint64_t>(pane + kPaneName), rd<uint16_t>(pane + kPaneNameLen), p.name, sizeof p.name);
        char tag[48];
        read_str(rd<uint64_t>(pane + kPaneTag), 0, tag, sizeof tag);
        classify(p, tag);
        ++L.visible;
        s.panes.push_back(p);
    }
    // candidate "drawn" fields, logged so one run with a menu open shows which differs (all inferred, none confirmed): the placeable's
    // atomic flag word (+488, tested by 0x7101fdc1e0), the layout's counters at +744..+756 (+748 = pending textures, 0x7101f55880;
    // +756 feeds pane flag 0x200 in 0x7101f56060), the root pane's flags (+260).
    if (trace_n < 6 && L.visible > 0) {
        const uint64_t pflags = ok(plc + 488, 8) ? rd<uint64_t>(plc + 488) : 0;
        uint32_t lf[4] = {};
        if (ok(lay + 744, 16)) std::memcpy(lf, reinterpret_cast<const void*>(lay + 744), sizeof lf);
        const uint64_t root = ok(first, 24) ? rd<uint64_t>(first + 16) : 0;
        const uint32_t rflags = ok(root + kPaneFlags, 4) ? rd<uint32_t>(root + kPaneFlags) : 0;
        TRACE("    layout %u/%u 0x%llx (placeable 0x%llx, obj 0x%llx): panes %d, visible %d (unreadable %d, alpha 0 %d, off screen %d); "
              "placeable+488 0x%llx, layout+744 %u +748 %u +752 %u +756 %u, root pane flags 0x%x",
              id, sub, (unsigned long long)lay, (unsigned long long)plc, (unsigned long long)obj, L.panes, L.visible, unreadable, alpha0, offscreen,
              (unsigned long long)pflags, lf[0], lf[1], lf[2], lf[3], rflags);
    }
    s.layouts.push_back(L);
}

// A pool's entries {entry** data, u64 count}; false (and the reason in the trace) when the list cannot be read.
bool pool_entries(uint64_t mgr, uint64_t pool, const char* name, uint64_t* data, uint64_t* count) {
    const uint64_t a = mgr + pool + kPoolEntries;
    if (!ok(a, 16)) { TRACE("  pool %s at 0x%llx: stop (entry list header 0x%llx not readable)", name, (unsigned long long)(mgr + pool), (unsigned long long)a); return false; }
    *data = rd<uint64_t>(a);
    *count = rd<uint64_t>(a + 8);
    TRACE("  pool %s at 0x%llx: entries data 0x%llx count %llu", name, (unsigned long long)(mgr + pool), (unsigned long long)*data,
          (unsigned long long)*count);
    if (!*data || *count == 0) { TRACE("  pool %s: stop (empty)", name); return false; }
    if (*count > kMaxPoolEntries || !ok(*data, *count * 8)) { TRACE("  pool %s: stop (count over %llu or data not readable)", name, (unsigned long long)kMaxPoolEntries); return false; }
    return true;
}

// Walks every screen layout the UI manager's pools hold and collects the visible panes.
void scan() {
    State& s = st();
    s.layouts.clear();
    s.panes.clear();
    if (s.pages.size() > 20000) s.pages.clear();
    {
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        g_trace = s.log_level >= 3 && s.log_lines < 4000 && now - last >= std::chrono::seconds(1);
        if (g_trace) { last = now; s.log_lines += 24; }
        g_trace_pick = g_trace;
    }
    s.n_not_drawn = 0;
    s.rec_panes = s.rec_with = s.rec_fallback_layouts = 0;
    s.rec_ages.clear();
    TRACE("drawn set: hooks %s, %llu pane draws so far, frame %u", draw_hooks_live() ? "live" : "silent (not re-lifted? panes need flag 0x20 instead)",
          (unsigned long long)g_draw_calls.load(), g_frame.load());
    const uint64_t gaddr = s.base + kUiMgr;
    const uint64_t mgr = rptr(gaddr);
    TRACE("global 0x%llx (main+0x%llx) readable %d -> UI manager 0x%llx", (unsigned long long)gaddr, (unsigned long long)kUiMgr, ok(gaddr, 8) ? 1 : 0,
          (unsigned long long)mgr);
    if (!mgr) { TRACE("stop (no UI manager)"); g_trace = false; return; }
    uint64_t data = 0, count = 0;
    // pool A: per layout resource, the instances in use (all minus the free ring)
    if (pool_entries(mgr, kPoolA, "A", &data, &count)) {
        int traced = 0;
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t e = rd<uint64_t>(data + 8 * i);
            if (!ok(e, 48)) { if (i < 4) TRACE("   A[%llu] entry 0x%llx: stop (not readable)", (unsigned long long)i, (unsigned long long)e); continue; }
            const uint32_t id = rd<uint32_t>(e + 8), sub = rd<uint32_t>(e + 12);
            const uint64_t vec = rd<uint64_t>(e + kEntInstances), ring = rd<uint64_t>(e + kEntFree);
            uint64_t vdata = 0, vcount = 0;
            if (ok(vec, 16)) { vdata = rd<uint64_t>(vec); vcount = rd<uint64_t>(vec + 8); }
            uint64_t free_ptrs[kMaxInstances];
            size_t nfree = 0;
            uint64_t rcount = 0;
            if (ok(ring, 40)) {
                const uint64_t rdata = rd<uint64_t>(ring), rsize = rd<uint64_t>(ring + 8), rcap = rd<uint64_t>(ring + 16), rhead = rd<uint64_t>(ring + 24);
                rcount = rd<uint64_t>(ring + 32);
                if (rdata && rsize && rcap && rcount <= kMaxInstances && rsize <= 4096 && ok(rdata, rsize * 8))
                    for (uint64_t k = 0; k < rcount; ++k) {
                        const uint64_t idx = std::min((rhead + k) % rcap, rsize - 1);      // 0x710087da00's indexing
                        free_ptrs[nfree++] = rd<uint64_t>(rdata + 8 * idx);
                    }
            }
            if (i < 4)
                TRACE("   A[%llu] entry 0x%llx id %u sub %u: instances 0x%llx {data 0x%llx count %llu}, free ring 0x%llx count %llu", (unsigned long long)i,
                      (unsigned long long)e, id, sub, (unsigned long long)vec, (unsigned long long)vdata, (unsigned long long)vcount,
                      (unsigned long long)ring, (unsigned long long)rcount);
            if (!vdata || vcount == 0 || vcount > kMaxInstances || !ok(vdata, vcount * 8)) continue;
            for (uint64_t k = 0; k < vcount; ++k) {
                const uint64_t plc = rd<uint64_t>(vdata + 8 * k);
                if (!plc || std::find(free_ptrs, free_ptrs + nfree, plc) != free_ptrs + nfree) continue;     // free: not on screen
                add_layout(plc, id, sub, traced);
                if (!s.layouts.empty() && s.layouts.back().ptr && s.layouts.back().visible > 0) ++traced;
            }
        }
    }
    // pool B: entries are placeables
    if (pool_entries(mgr, kPoolB, "B", &data, &count)) {
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t e = rd<uint64_t>(data + 8 * i);
            if (!ok(e, 16)) continue;
            add_layout(rd<uint64_t>(e), rd<uint32_t>(e + 8), rd<uint32_t>(e + 12), static_cast<int>(i));
        }
    }
    TRACE("result: %zu layouts drawn (%d pooled ones not drawn skipped), %zu visible panes", s.layouts.size(), s.n_not_drawn, s.panes.size());
    if (g_trace) {
        std::vector<uint32_t> a = s.rec_ages;
        std::sort(a.begin(), a.end());
        TRACE("pane records: %d shown panes in drawn layouts, %d with a record, %d fresh, %zu too old (age min %u median %u max %u frames), "
              "%d layouts on the layout-level fallback, %llu marks dropped (table full), frame %u",
              s.rec_panes, s.rec_with, s.rec_with - static_cast<int>(a.size()), a.size(), a.empty() ? 0u : a.front(), a.empty() ? 0u : a[a.size() / 2],
              a.empty() ? 0u : a.back(), s.rec_fallback_layouts, (unsigned long long)g_pane_table_full.load(), g_frame.load());
    }
    if (g_trace) {
        char ids[900];
        size_t k = 0;
        ids[0] = 0;
        for (const Layout& L : s.layouts) {
            if (k + 24 >= sizeof ids) break;
            const int m = std::snprintf(ids + k, sizeof ids - k, " %u/%u(%d)", L.id, L.sub, L.visible);
            if (m > 0) k += static_cast<size_t>(m);
        }
        TRACE("drawn layouts id/sub(visible panes):%s", ids);
        // every visible pane of the drawn layouts, once each time their number changes (names for the cursor / entry rules)
        static size_t dumped = static_cast<size_t>(-1);
        if (s.panes.size() != dumped && s.panes.size() <= 400) {
            dumped = s.panes.size();
            for (const Pane& p : s.panes)
                TRACE("  pane %u/%u '%s'%s%s rect %.0f,%.0f..%.0f,%.0f", p.lay_id, p.lay_sub, p.name, p.item ? " [item]" : "", p.cursor ? " [cursor]" : "",
                      p.x0, p.y0, p.x1, p.y1);
        }
    }
    g_trace = false;
}

void log_layouts() {
    State& s = st();
    uint64_t sig = 0;
    int vis = 0, items = 0, cursors = 0;
    for (const Layout& L : s.layouts)
        if (L.visible) sig = sig * 1000003ull + (static_cast<uint64_t>(L.id) << 32 | L.sub) + 1;
    for (const Pane& p : s.panes) { ++vis; items += p.item; cursors += p.cursor; }
    s.n_layouts.store(static_cast<uint32_t>(s.layouts.size()), std::memory_order_relaxed);
    s.n_panes.store(static_cast<uint32_t>(vis), std::memory_order_relaxed);
    s.n_items.store(static_cast<uint32_t>(items), std::memory_order_relaxed);
    const bool opened = !s.was_up;
    if ((opened || sig != s.layout_sig) && log_ok(opened)) {
        NXLOG_I(kTag, "menu: %s, %zu layouts (%zu with visible panes), %d visible panes, %d entries, %d cursors", opened ? "open" : "changed",
                s.layouts.size(), static_cast<size_t>(std::count_if(s.layouts.begin(), s.layouts.end(), [](const Layout& L) { return L.visible > 0; })),
                vis, items, cursors);
    }
    s.layout_sig = sig;
    if (s.log_level < 2) return;
    // [pkp] menu_log = 2: each layout's visible panes, once per layout
    for (size_t li = 0; li < s.layouts.size(); ++li) {
        const Layout& L = s.layouts[li];
        const uint64_t key = static_cast<uint64_t>(L.id) << 32 | L.sub;
        if (!L.visible || s.dumped.count(key) || s.dumped.size() > 200 || s.log_lines > 4000) continue;
        s.dumped.insert(key);
        NXLOG_I(kTag, "menu: layout id=%u sub=%u ptr=0x%llx panes=%d visible=%d", L.id, L.sub, static_cast<unsigned long long>(L.ptr), L.panes,
                L.visible);
        int shown = 0;
        for (const Pane& p : s.panes) {
            if (p.lay != static_cast<int>(li) || shown >= 60) continue;
            ++shown;
            ++s.log_lines;
            NXLOG_I(kTag, "menu:   '%s'%s%s%s rect %.0f,%.0f..%.0f,%.0f z=%.1f a=%u flags=0x%x", p.name, p.item ? " [entry]" : "",
                    p.cursor ? " [cursor]" : "", p.slider ? " [slider]" : "", p.x0, p.y0, p.x1, p.y1, p.z, p.alpha, p.flags);
        }
    }
}

}  // namespace nxrt::games::pkp::menu_detail
