// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus (pkp_menu.h). Offsets in `main` of the 2.0.1 update, found in IDA (work/re/menus.md).
//
// The game's UI is KTGL screen layouts: a layout object holds its panes in a list, each pane has a name, a size and a world matrix in a
// 1920x1080 canvas (origin at the centre, y up). The game itself never hit-tests them for its menus (the engine's script call
// kids::placeable::screenlayout::DoPaneHitTest exists, 0x2611d00, and is where the pane geometry below comes from), and its menus move a
// cursor part (0000_parts_cursor_finger_s, 0250_parts_talk_cursor, 0000_parts_cursor_triangle ...) next to the selected entry when the
// d-pad moves the selection. So, once a frame while a menu is up (play mode's menu context: the camera's input is not running):
//   * every layout the UI manager ([0x35aebf8] +0x8d888: {entry**, count}; entry {placeable*, u32 id, u32 sub}; placeable +40 -> +16 =
//     the layout) is walked and each visible pane's screen rectangle computed;
//   * the entry under the pointer is the smallest visible "item" pane (a button, list cell, choice, tab ... by its name) containing it;
//   * the selected entry is the item pane nearest to the game's cursor pane (a pane named "...cursor...");
//   * when the pointer moved onto another entry, d-pad presses step the game's own selection toward it until the cursor sits on it
//     (one press, then a wait for the cursor to move; the other axis when a press changed nothing);
//   * a left click confirms (A) the entry under the pointer, after stepping to it if needed; with no cursor on screen (messages, prompts)
//     a click anywhere is A. Right click (B) and the wheel (d-pad up / down) are menu-context bindings (pkp_game.cpp).
// Menus whose selection shows no cursor pane (highlight only) get the click and the bindings but not the hover; the log says which
// layouts were on screen ([pkp] menu_log = 2 lists their panes) so they can be added.
#include "pkp_menu.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

// ---- offsets ----
constexpr uint64_t kUiMgr = 0x35aebf8;          // [ptr] the UI manager
constexpr uint64_t kMgrLayouts = 0x8d888;       // UI manager: {entry** data, u64 count} of the registered screen layouts (0x9125e0 / 0x912aa0 read it)
constexpr size_t kPlaceableObj = 40, kObjLayout = 16;     // placeable +40 -> object, +16 -> the layout (DoPaneHitTest 0x2611d00)
// layout
constexpr size_t kLayPaneCount = 146, kLayFirstNode = 32, kLaySentinel = 64;   // u16 pane count; pane list: node +8 next, +16 pane
// pane
constexpr size_t kPaneName = 176, kPaneTag = 184, kPaneParent = 208, kPaneWorld = 224, kPaneSize = 240, kPaneFlags = 260,
                 kPaneAnchor = 268, kPaneIndex = 270, kPaneNameLen = 272, kPaneAlpha = 275;
constexpr float kCanvasW = 1920.f, kCanvasH = 1080.f;
constexpr size_t kMaxLayouts = 256, kMaxPanes = 4096;

struct Pane {
    uint64_t ptr = 0;
    uint32_t lay_id = 0, lay_sub = 0;
    int lay = -1;                      // index into the frame's layouts
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // canvas pixels, y down
    float z = 0;
    uint8_t alpha = 0;
    uint32_t flags = 0;
    bool item = false, cursor = false, slider = false;
    char name[48] = {};
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
    float area() const { return (x1 - x0) * (y1 - y0); }
    bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    float dist(float x, float y) const {
        const float dx = std::max({x0 - x, 0.f, x - x1}), dy = std::max({y0 - y, 0.f, y - y1});
        return std::sqrt(dx * dx + dy * dy);
    }
};

struct Layout {
    uint64_t ptr = 0;
    uint32_t id = 0, sub = 0;
    int panes = 0, visible = 0;
};

struct State {
    uint64_t base = 0x7100000000ull;
    bool installed = false;
    bool enabled = true;
    int log_level = 1;
    // the frame's scan
    std::vector<Layout> layouts;
    std::vector<Pane> panes;
    int hovered = -1, focused = -1, cursor = -1, slider = -1;
    bool active = false;
    // navigation toward the entry under the pointer
    uint64_t target = 0;               // pane pointer
    int steps = 0, wait = 0, fails = 0;
    bool pending_confirm = false;
    bool prefer_other_axis = false;
    uint64_t focus_before = 0, last_focus = 0, last_hover = 0;
    int focus_stable = 0;
    int click_wait = 0;                // frames a pending click has waited for the selection
    // diagnostics
    bool was_up = false;
    uint64_t layout_sig = 0;
    std::unordered_set<uint64_t> dumped;    // layout (id, sub) whose panes were listed
    std::chrono::steady_clock::time_point last_log{};
    uint32_t log_lines = 0;
    std::atomic<uint32_t> n_layouts{0}, n_panes{0}, n_items{0}, n_steps{0}, n_clicks{0};
    char hover_name[48] = {}, focus_name[48] = {};
    // page readability cache (one VirtualQuery per page and scan)
    std::unordered_map<uint64_t, bool> pages;
};

State& st() {
    static State s;
    return s;
}

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }

// The range lies in guest memory that can be read (cached per 4 KiB page for one scan).
bool ok(uint64_t a, uint64_t n) {
    if (a < 0x1000000000ull || a > 0x7f00000000ull || n == 0 || n > 0x10000) return false;
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
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
bool pane_rect(uint64_t pane, Pane* out) {
    if (!ok(pane, kPaneAlpha + 1)) return false;
    const uint64_t mp = rd<uint64_t>(pane + kPaneWorld);
    if (!ok(mp, 64)) return false;
    float m[16];
    std::memcpy(m, reinterpret_cast<const void*>(mp), sizeof m);
    for (float v : m)
        if (!std::isfinite(v)) return false;
    const uint32_t size = rd<uint32_t>(pane + kPaneSize);
    const float w = static_cast<float>(size & 0xffff), h = static_cast<float>(size >> 16);
    if (w <= 0.f || h <= 0.f) return false;
    const uint8_t anchor = rd<uint8_t>(pane + kPaneAnchor);
    const int ah = anchor >> 4, av = anchor & 0xf;
    const float ox = ah == 0 ? w * 0.5f : (ah == 2 ? -w * 0.5f : 0.f);
    const float oy = av == 0 ? -h * 0.5f : (av == 2 ? h * 0.5f : 0.f);
    const float cx = m[12] + m[0] * ox + m[4] * oy, cy = m[13] + m[1] * ox + m[5] * oy;
    const float hx = (std::fabs(m[0]) * w + std::fabs(m[4]) * h) * 0.5f, hy = (std::fabs(m[1]) * w + std::fabs(m[5]) * h) * 0.5f;
    out->x0 = kCanvasW * 0.5f + cx - hx;
    out->x1 = kCanvasW * 0.5f + cx + hx;
    out->y0 = kCanvasH * 0.5f - cy - hy;
    out->y1 = kCanvasH * 0.5f - cy + hy;
    out->z = m[14];
    out->alpha = rd<uint8_t>(pane + kPaneAlpha);
    out->flags = rd<uint32_t>(pane + kPaneFlags);
    return true;
}

bool log_ok(bool force = false) {
    State& s = st();
    if (s.log_level <= 0 || s.log_lines > 4000) return false;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - s.last_log < std::chrono::milliseconds(200)) return false;
    s.last_log = now;
    ++s.log_lines;
    return true;
}

// Walks every registered layout and collects its visible panes.
void scan() {
    State& s = st();
    s.layouts.clear();
    s.panes.clear();
    if (s.pages.size() > 20000) s.pages.clear();
    const uint64_t mgr = rptr(s.base + kUiMgr);
    if (!mgr || !ok(mgr + kMgrLayouts, 16)) return;
    const uint64_t data = rd<uint64_t>(mgr + kMgrLayouts);
    const uint64_t count = rd<uint64_t>(mgr + kMgrLayouts + 8);
    if (!data || count == 0 || count > kMaxLayouts || !ok(data, count * 8)) return;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t e = rd<uint64_t>(data + 8 * i);
        if (!ok(e, 16)) continue;
        const uint64_t plc = rd<uint64_t>(e);
        const uint64_t obj = rptr(plc + kPlaceableObj);
        const uint64_t lay = rptr(obj + kObjLayout);
        if (!lay || !ok(lay, kLaySentinel + 8)) continue;
        Layout L;
        L.ptr = lay;
        L.id = rd<uint32_t>(e + 8);
        L.sub = rd<uint32_t>(e + 12);
        const int n = rd<uint16_t>(lay + kLayPaneCount);
        uint64_t node = rd<uint64_t>(lay + kLayFirstNode);
        const int li = static_cast<int>(s.layouts.size());
        for (int k = 0; k < n && k < static_cast<int>(kMaxPanes) && node && node != lay + kLaySentinel; ++k) {
            if (!ok(node, 24)) break;
            const uint64_t pane = rd<uint64_t>(node + 16);
            node = rd<uint64_t>(node + 8);
            ++L.panes;
            Pane p;
            if (!pane_rect(pane, &p)) continue;
            if (p.alpha == 0) continue;
            if (p.x1 < 0.f || p.y1 < 0.f || p.x0 > kCanvasW || p.y0 > kCanvasH) continue;     // off screen (or a world-space layout)
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
        s.layouts.push_back(L);
    }
}

int find_pane(uint64_t ptr) {
    const State& s = st();
    for (size_t i = 0; i < s.panes.size(); ++i)
        if (s.panes[i].ptr == ptr) return static_cast<int>(i);
    return -1;
}

// The entry under the pointer, the cursor and the entry it marks.
void pick(float px, float py, bool inside) {
    State& s = st();
    s.hovered = s.focused = s.cursor = s.slider = -1;
    const float big = kCanvasW * kCanvasH * 0.5f;
    if (inside) {
        float best = 1e30f;
        for (size_t i = 0; i < s.panes.size(); ++i) {
            const Pane& p = s.panes[i];
            if (!p.contains(px, py)) continue;
            if (p.slider && s.slider < 0) s.slider = static_cast<int>(i);
            if (!p.item || p.area() > big) continue;
            if (p.area() < best) { best = p.area(); s.hovered = static_cast<int>(i); }
        }
    }
    // the cursor: of the visible cursor panes, the one closest to an entry
    float best = 1e30f;
    for (size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big) continue;
        for (size_t i = 0; i < s.panes.size(); ++i) {
            const Pane& p = s.panes[i];
            if (!p.item || p.area() > big) continue;
            // prefer entries of the cursor's own layout: another layout's entries count from 40 px further away
            const float d = p.dist(cp.cx(), cp.cy()) + (p.lay == cp.lay ? 0.f : 40.f);
            if (d < best) { best = d; s.cursor = static_cast<int>(c); s.focused = static_cast<int>(i); }
        }
    }
    if (best > 160.f) s.focused = -1;
}

play::Act dir_toward(const Pane& from, const Pane& to, bool other_axis) {
    const float dx = to.cx() - from.cx(), dy = to.cy() - from.cy();
    bool horiz = std::fabs(dx) > std::fabs(dy);
    if (other_axis && std::fabs(dx) > 4.f && std::fabs(dy) > 4.f) horiz = !horiz;
    if (horiz) return dx > 0 ? play::Act::DpadRight : play::Act::DpadLeft;
    return dy > 0 ? play::Act::DpadDown : play::Act::DpadUp;
}

const char* act_name(play::Act a) {
    switch (a) {
        case play::Act::DpadUp: return "up";
        case play::Act::DpadDown: return "down";
        case play::Act::DpadLeft: return "left";
        case play::Act::DpadRight: return "right";
        case play::Act::BtnA: return "A";
        default: return "?";
    }
}

void clear_nav() {
    State& s = st();
    s.target = 0;
    s.steps = s.wait = s.fails = s.click_wait = 0;
    s.pending_confirm = false;
    s.prefer_other_axis = false;
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

}  // namespace

void menu_install(uint64_t base) {
    State& s = st();
    s.base = base;
    s.enabled = config_get_bool("pkp", "menu_mouse", true);
    s.log_level = static_cast<int>(config_get_int("pkp", "menu_log", 1));
    s.installed = true;
    NXLOG_I(kTag, "menu mouse %s (hover moves the game's cursor, click confirms; [pkp] menu_log %d)", s.enabled ? "on" : "off", s.log_level);
}

void menu_remove() {
    st().installed = false;
    clear_nav();
}

play::Act menu_frame(bool menu_up, const play::mm::Geometry& g) {
    State& s = st();
    play::mm::Pointer& ptr = play::mm::pointer();
    const play::mm::Snapshot snap = ptr.begin_frame(menu_up, g);
    s.active = false;
    if (!s.installed || !s.enabled || !snap.active) {
        if (s.was_up && log_ok(true)) NXLOG_I(kTag, "menu: closed");
        s.was_up = false;
        s.panes.clear();
        s.hovered = s.focused = s.cursor = s.slider = -1;
        clear_nav();
        return play::Act::None;
    }
    s.active = true;
    s.pages.clear();
    scan();
    const float px = (snap.gx + 1.f) * 0.5f * kCanvasW, py = (1.f - snap.gy) * 0.5f * kCanvasH;
    pick(px, py, snap.in_picture && snap.shown);
    log_layouts();
    s.was_up = true;

    const Pane* hov = s.hovered >= 0 ? &s.panes[s.hovered] : nullptr;
    const Pane* foc = s.focused >= 0 ? &s.panes[s.focused] : nullptr;
    const uint64_t hov_ptr = hov ? hov->ptr : 0, foc_ptr = foc ? foc->ptr : 0;
    if (hov_ptr != s.last_hover) {
        std::snprintf(s.hover_name, sizeof s.hover_name, "%s", hov ? hov->name : "");
        if (log_ok()) {
            if (hov) NXLOG_I(kTag, "menu: hover '%s' (layout %u/%u) at %.0f,%.0f", hov->name, hov->lay_id, hov->lay_sub, px, py);
            else NXLOG_I(kTag, "menu: hover nothing at %.0f,%.0f", px, py);
        }
        s.last_hover = hov_ptr;
    }
    if (foc_ptr != s.last_focus) {
        std::snprintf(s.focus_name, sizeof s.focus_name, "%s", foc ? foc->name : "");
        if (foc && log_ok()) NXLOG_I(kTag, "menu: selected '%s' (cursor '%s')", foc->name, s.cursor >= 0 ? s.panes[s.cursor].name : "");
        s.last_focus = foc_ptr;
        s.focus_stable = 0;
    } else {
        ++s.focus_stable;
    }

    // keyboard or pad used: the pointer lets go of the selection
    if (!snap.shown) clear_nav();
    // the pointer moved onto another entry: the selection follows it
    if (snap.hover && hov && hov_ptr != s.target && s.cursor >= 0) {
        clear_nav();
        s.target = hov_ptr;
    }
    // left click
    if (snap.click) {
        ptr.click_used();
        ++s.n_clicks;
        if (s.cursor < 0) {
            if (log_ok(true)) NXLOG_I(kTag, "menu: click -> A (no cursor on screen)");
            clear_nav();
            return play::Act::BtnA;
        }
        if (hov && foc_ptr == hov_ptr) {
            if (log_ok(true)) NXLOG_I(kTag, "menu: click on the selected '%s' -> A", hov->name);
            clear_nav();
            return play::Act::BtnA;
        }
        if (hov) {
            s.target = hov_ptr;
            s.pending_confirm = true;
            s.click_wait = 0;
            if (log_ok(true)) NXLOG_I(kTag, "menu: click on '%s', selecting it first", hov->name);
        } else if (log_ok(true)) {
            NXLOG_I(kTag, "menu: click on no entry (ignored)");
        }
    }

    if (!s.target) return play::Act::None;
    const int ti = find_pane(s.target);
    if (ti < 0 || !foc) {
        if (s.pending_confirm && ++s.click_wait > 90) clear_nav();
        else if (!s.pending_confirm) clear_nav();
        return play::Act::None;
    }
    if (foc_ptr == s.target) {
        if (s.pending_confirm) {
            if (s.focus_stable < 2) return play::Act::None;      // the cursor still moving
            if (log_ok(true)) NXLOG_I(kTag, "menu: '%s' selected -> A", foc->name);
            clear_nav();
            return play::Act::BtnA;
        }
        clear_nav();
        return play::Act::None;
    }
    if (s.wait > 0) {
        --s.wait;
        if (s.wait == 0 && foc_ptr == s.focus_before) {
            ++s.fails;                                  // the press moved nothing: try the other axis, then give up
            s.prefer_other_axis = !s.prefer_other_axis;
            if (s.fails >= 4) {
                if (log_ok()) NXLOG_I(kTag, "menu: cannot reach '%s' from '%s'", s.panes[ti].name, foc->name);
                clear_nav();
            }
        }
        return play::Act::None;
    }
    if (s.steps >= 40) { clear_nav(); return play::Act::None; }
    const play::Act a = dir_toward(*foc, s.panes[ti], s.prefer_other_axis);
    ++s.steps;
    ++s.n_steps;
    s.wait = 8;
    s.focus_before = foc_ptr;
    if (s.log_level >= 2 && log_ok()) NXLOG_I(kTag, "menu: step %s from '%s' toward '%s'", act_name(a), foc->name, s.panes[ti].name);
    return a;
}

bool menu_over_value() { return st().active && st().slider >= 0; }

bool menu_active() { return st().active; }

bool menu_open() { return st().active && st().cursor >= 0; }

void menu_status_text(char* buf, size_t n) {
    const State& s = st();
    std::snprintf(buf, n, " menu_layouts=%u menu_panes=%u menu_entries=%u hover='%s' selected='%s' steps=%u clicks=%u", s.n_layouts.load(),
                  s.n_panes.load(), s.n_items.load(), s.hover_name, s.focus_name, s.n_steps.load(), s.n_clicks.load());
}

}  // namespace nxrt::games::pkp
