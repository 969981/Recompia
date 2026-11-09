// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus: the game's cursor, the entry it marks and the entry under the pointer (split out of
// pkp_menu.cpp).
#include "pkp_menu_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/text.h"

namespace nxrt::games::pkp::menu_detail {

// The entry under the pointer, the cursor and the entry it marks.
bool advance_mark(const Pane& p) {
    // a message box's "next" arrow is a cursor pane with no choice behind it (p5d: 'CL_0000_parts_cursor_triangle' over '0000_btn_common_1')
    std::string n = p.name;
    n = nxrt::text::lower(std::move(n));
    return has(n, "triangle");
}

// Menus outside play (title, settings, language, save and yes / no dialogs) build their entries from parts, and in KTGL each part instance is
// a layout of its own in the pool (the bag's triangle 4148535219/43 is one too). Pane names come from the layout data only: main holds no
// pane-name string at all (no "cursor", "Win_", "btn_" bytes in the image), the screens are driven by their scripts, and no selection field
// could be read from code, so the selection is still read off the cursor panes. Two shapes the cursor rule above misses (from the
// settings / language layouts listed by p5d / p5e, menu_log 2):
//   * settings: the list 434523034 holds 'CL_0020_parts_setting_menu_btn_0..3'; each button is a part layout (2408049848) with its own
//     'Win_cursor' highlight and entries ('Null_type_1tab', 'CL_0000_parts_btn_choices' ...). The cursor rule picks an entry of that part,
//     and hover then stays inside the one button. Rule 1 (promotion): when the chosen entry's layout is small (at most 4 entries) and the
//     entry sits inside an entry of another list layout, the selection is that list entry (the smallest holding the entry's centre), so the
//     hover covers the whole list. Also, an entry chosen by a non-triangle cursor that has a smaller entry of its layout under the cursor's
//     centre gives way to it (a list frame 'posi_list' around the buttons).
//   * yes / no, language and save choices: each choice is an instance of one button layout ('Null_btn', '0000_btn_common_N', and the
//     language's '0030_prt_language_1'), the selected one showing 'CL_0000_parts_cursor_triangle' at its left; the cursor rule takes that
//     triangle for a message box's advance arrow (under 3 entries in its layout). Rule 2 (siblings): when no cursor was found, a cursor
//     whose layout has at least one other drawn instance (same layout id) is the selection; the entries are each instance's largest entry
//     (they must not all sit on one spot), and hover / steps move between them. Draw hooks only (pooled templates all look visible).
// The in-game menus seen so far (bag: 41 entries, chat choices: 8) never reach either rule, so their path is unchanged.
bool ignored_layout(const Pane& p) {
    const State& s = st();
    return !s.ig_ignore.empty() && s.ig_ignore.count(static_cast<uint64_t>(p.lay_id) << 32 | p.lay_sub);
}

void pick_extend(const std::vector<int>& items, float big) {
    State& s = st();
    s.mode = 0;
    s.group.clear();
    const int nl = static_cast<int>(items.size());
    if (s.focused >= 0 && s.cursor >= 0) {
        const Pane& cp = s.panes[s.cursor];
        // a frame entry around several entries ('posi_list'): the entry of that layout under the cursor's centre
        if (!advance_mark(cp)) {
            const Pane& f = s.panes[s.focused];
            int inner = 0, under = -1;
            for (size_t i = 0; i < s.panes.size(); ++i) {
                const Pane& p = s.panes[i];
                if (static_cast<int>(i) == s.focused || !p.item || p.lay != f.lay || p.area() >= f.area()) continue;
                if (!f.contains(p.cx(), p.cy())) continue;
                ++inner;
                if (p.contains(cp.cx(), cp.cy()) && (under < 0 || p.area() < s.panes[under].area())) under = static_cast<int>(i);
            }
            if (inner >= 2 && under >= 0) s.focused = under;
        }
        // rule 1: the chosen entry belongs to a part (a small layout) placed in an entry of a list layout
        const Pane& f = s.panes[s.focused];
        if (f.lay >= 0 && f.lay < nl && items[f.lay] <= 4) {
            int up = -1;
            for (size_t i = 0; i < s.panes.size(); ++i) {
                const Pane& p = s.panes[i];
                if (!p.item || p.area() > big || p.lay < 0 || p.lay >= nl || p.lay == f.lay || items[p.lay] < 2) continue;
                if (!p.contains(f.cx(), f.cy()) || f.area() > p.area() * 1.5f) continue;
                if (up < 0 || p.area() < s.panes[up].area()) up = static_cast<int>(i);
            }
            if (up >= 0) { s.focused = up; s.mode = 1; }
        }
        return;
    }
    // rule 2: sibling instances of one button layout, the cursor in one of them
    if (!draw_hooks_live()) return;
    std::vector<int> rep(s.layouts.size(), -1);       // each layout's largest entry
    for (size_t i = 0; i < s.panes.size(); ++i) {
        const Pane& p = s.panes[i];
        if (!p.item || p.area() > big || p.lay < 0 || p.lay >= static_cast<int>(rep.size())) continue;
        if (rep[p.lay] < 0 || p.area() > s.panes[rep[p.lay]].area()) rep[p.lay] = static_cast<int>(i);
    }
    float best = 160.f;
    for (size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big || cp.lay < 0 || cp.lay >= static_cast<int>(rep.size()) || ignored_layout(cp)) continue;
        const int mine = rep[cp.lay];
        if (mine < 0) continue;
        const uint32_t id = s.layouts[cp.lay].id;
        std::vector<int> g;
        int apart = 0;
        for (size_t li = 0; li < s.layouts.size(); ++li) {
            if (s.layouts[li].id != id || rep[li] < 0) continue;
            g.push_back(rep[li]);
            const Pane& q = s.panes[rep[li]];
            if (std::fabs(q.cx() - s.panes[mine].cx()) > 8.f || std::fabs(q.cy() - s.panes[mine].cy()) > 8.f) ++apart;
        }
        if (g.size() < 2 || apart == 0) continue;
        const float d = s.panes[mine].dist(cp.cx(), cp.cy());
        if (d > best) continue;
        best = d;
        s.cursor = static_cast<int>(c);
        s.focused = mine;
        s.group = std::move(g);
        s.mode = 2;
    }
}

// The game's cursor: a visible cursor pane (not a message box's advance arrow, not in an ignored layout) whose own layout has at least two
// entries, one of them within 160 px; of those, the closest. The entry under the pointer only counts in the cursor's layout (pooled layouts
// that are not drawn still look visible: their panes keep alpha 255 and a rectangle).
void pick(float px, float py, bool inside) {
    State& s = st();
    s.hovered = s.focused = s.cursor = s.slider = -1;
    const float big = kCanvasW * kCanvasH * 0.5f;
    std::vector<int> items(s.layouts.size(), 0);
    for (const Pane& p : s.panes)
        if (p.item && p.area() <= big && p.lay >= 0 && p.lay < static_cast<int>(items.size())) ++items[p.lay];
    float best = 1e30f;
    int rejected_arrow = 0, rejected_lone = 0;
    for (size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big) continue;
        if (!s.ig_ignore.empty() && s.ig_ignore.count(static_cast<uint64_t>(cp.lay_id) << 32 | cp.lay_sub)) continue;
        if (cp.lay < 0 || cp.lay >= static_cast<int>(items.size())) { ++rejected_lone; continue; }
        // The cursor's own layout holds the entries, or (only with the draw hooks live, so every layout here is on screen) the cursor is a
        // layout of its own over another drawn layout's list: the bag's triangle (615988817/11) sits on slot 0 of the inventory 4084873301/154.
        const bool own = items[cp.lay] >= 2;
        if (!own && !draw_hooks_live()) { ++rejected_lone; continue; }
        // the cursor's name rules, once per cursor (they read only cp.name; the entry loop below changes no pane)
        const bool arrow = advance_mark(cp);
        const bool down = arrow && has(std::string(cp.name), "180");
        for (size_t i = 0; i < s.panes.size(); ++i) {
            const Pane& p = s.panes[i];
            if (!p.item || p.area() > big || p.lay < 0) continue;
            if (own ? p.lay != cp.lay : items[p.lay] < 2) continue;
            // a triangle is a message box's advance arrow unless it marks an entry of a real list (3+ entries)
            if (arrow && items[p.lay] < 3) continue;
            // a triangle pointing down ('..._180_..', the bag) sits above its entry: its tip, a quarter of its height below the bottom, is
            // where it points (`down`); ties between overlapping slots go to the nearer centre
            const float qx = cp.cx(), qy = down ? cp.y1 + (cp.y1 - cp.y0) * 0.25f : cp.cy();
            const float d = p.dist(qx, qy) + 0.001f * std::hypot(p.cx() - qx, p.cy() - qy);
            // on a tie (the cursor over a row container and its slot) the smaller entry wins
            if (d < best || (d == best && s.focused >= 0 && p.area() < s.panes[s.focused].area())) {
                best = d;
                s.cursor = static_cast<int>(c);
                s.focused = static_cast<int>(i);
            }
        }
        if (arrow && (s.cursor != static_cast<int>(c))) ++rejected_arrow;
    }
    if (best > 160.f) s.cursor = s.focused = -1;
    pick_extend(items, big);
    if (inside && s.mode == 2) {
        float hb = 1e30f;
        for (int i : s.group) {
            const Pane& p = s.panes[i];
            if (p.contains(px, py) && p.area() < hb) { hb = p.area(); s.hovered = i; }
        }
    } else if (inside) {
        float hb = 1e30f;
        for (size_t i = 0; i < s.panes.size(); ++i) {
            const Pane& p = s.panes[i];
            if (!p.contains(px, py)) continue;
            if (s.focused >= 0 && p.lay != s.panes[s.focused].lay) continue;   // the entries' layout (may differ from the cursor's)
            if (p.slider && s.slider < 0) s.slider = static_cast<int>(i);
            if (!p.item || p.area() > big) continue;
            if (p.area() < hb) { hb = p.area(); s.hovered = static_cast<int>(i); }
        }
    }
    if (g_trace_pick) {
        g_trace_pick = false;
        if (s.cursor >= 0) {
            const Pane& c = s.panes[s.cursor];
            NXLOG_I(kTag, "menu chain: chosen layout %u/%u (cursor '%s' on '%s', %d entries in it); skipped %d advance arrows, %d cursors in layouts "
                    "with under 2 entries", c.lay_id, c.lay_sub, c.name, s.panes[s.focused].name, items[c.lay], rejected_arrow, rejected_lone);
        } else {
            NXLOG_I(kTag, "menu chain: no cursor chosen; skipped %d advance arrows, %d cursors in layouts with under 2 entries", rejected_arrow,
                    rejected_lone);
        }
    }
}

}  // namespace nxrt::games::pkp::menu_detail
