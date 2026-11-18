// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus: the game's menus over play (menu_in_game; split out of pkp_menu.cpp).
#include "ui/pkp_menu.h"
#include "ui/pkp_menu_internal.h"
#include "input/pkp_play.h"

#include <cstdio>

#include "nxrt/log.h"

namespace nxrt::games::pkp {

using namespace menu_detail;

// While the game is in play its menus (bag, crafting, shops, choice boxes) are KTGL layouts over the world and the game keeps calling its
// input function, so play mode cannot tell them from play. A layout with the game's selection cursor on an entry (pick's rule: a visible
// "...cursor..." pane within 160 px of an "item" pane) is such a menu. Closed: one scan every kPollFrames frames; up: menu_frame scans
// every frame and its result is reused. Debounced: up after kOpenHits positive polls in a row, down after kCloseFrames negative frames.
// A drawn key guide layout from [pkp] menu_key_guides (the title's "(-) Settings"): a screen with no selection cursor that still takes clicks.
// Gameplay's HUD draws the same key guide layout (Run / Bag / Focus), so it counts only while the follow camera is not running (the title).
bool key_guide_drawn(const State& s) {
    if (s.key_guides.empty() || play_follow_live()) return false;
    for (const Layout& L : s.layouts)
        if (L.visible > 0 && s.key_guides.count(L.id)) return true;
    return false;
}

bool menu_in_game(bool in_game) {
    State& s = st();
    constexpr int kPollFrames = 6, kOpenHits = 2, kCloseFrames = 10;
    g_frame.fetch_add(1, std::memory_order_relaxed);     // the drawn set's clock (once per play-mode frame)
    const bool scanned = s.frame_scanned, scanned_present = s.frame_present;
    s.frame_scanned = false;
    if (s.log_level >= 3) {   // once a second: why the menu code does or does not look at this frame
        static uint32_t n = 0;
        if (++n % 600 == 0) NXLOG_I(kTag, "menu: poll in_game %d installed %d enabled %d in_game_menus %d up %d layouts %zu", in_game ? 1 : 0, s.installed ? 1 : 0, s.enabled ? 1 : 0, s.ig_enabled ? 1 : 0, s.ig_up ? 1 : 0, s.layouts.size());
    }
    if (!in_game || !s.installed || !s.enabled || !s.ig_enabled) {
        if (s.ig_up && log_ok(true)) NXLOG_I(kTag, "menu: in-game menu closed (layout %u/%u, the game left play)", s.ig_lay_id, s.ig_lay_sub);
        s.ig_up = false;
        s.ig_hits = s.ig_miss = s.ig_tick = 0;
        return false;
    }
    // Photo mode's own screen (style tabs, zoom bar) has cursors but is not a menu while the photo camera takes look input
    // (pkp_photo_camera.cpp); a dialog over it stops that input, and play mode then leaves play on its own.
    if (play_photo_live()) {
        if (s.ig_up && log_ok(true)) NXLOG_I(kTag, "menu: in-game menu closed (layout %u/%u, photo mode)", s.ig_lay_id, s.ig_lay_sub);
        s.ig_up = false;
        s.ig_hits = s.ig_miss = s.ig_tick = 0;
        return false;
    }
    bool present;
    if (s.ig_up && scanned) {
        present = scanned_present;
    } else {
        if (!s.ig_up && ++s.ig_tick < kPollFrames) return false;
        s.ig_tick = 0;
        s.pages.clear();
        scan();
        pick(-1.f, -1.f, false);
        if (s.log_level >= 3) log_layouts();   // what play-time screens (the title too) draw
        present = (s.cursor >= 0 && s.focused >= 0) || key_guide_drawn(s);
    }
    if (!s.ig_up) {
        s.ig_hits = present ? s.ig_hits + 1 : 0;
        if (s.ig_hits >= kOpenHits) {
            s.ig_up = true;
            s.ig_miss = 0;
            ++s.n_ig_opens;
            if (s.cursor < 0 || s.focused < 0) {   // a key guide screen (title): no cursor
                s.ig_lay_id = s.ig_lay_sub = 0;
                std::snprintf(s.ig_cursor, sizeof s.ig_cursor, "%s", "(key guide)");
                if (log_ok(true)) NXLOG_I(kTag, "menu: in-game menu up: a key guide screen with no cursor (%zu layouts); clicks go to the key guides, else A", s.layouts.size());
                return s.ig_up;
            }
            const Pane& c = s.panes[s.cursor];
            const Pane& f = s.panes[s.focused];
            s.ig_lay_id = c.lay_id;
            s.ig_lay_sub = c.lay_sub;
            std::snprintf(s.ig_cursor, sizeof s.ig_cursor, "%s", c.name);
            if (log_ok(true))
                NXLOG_I(kTag,
                        "menu: in-game menu up: layout %u/%u (cursor '%s' on '%s', entry layout %u/%u), %zu layouts, %zu visible panes; "
                        "the mouse is the menu pointer ([pkp] menu_in_game_ignore = %u/%u if this is not a menu)",
                        c.lay_id, c.lay_sub, c.name, f.name, f.lay_id, f.lay_sub, s.layouts.size(), s.panes.size(), c.lay_id, c.lay_sub);
        }
    } else {
        s.ig_miss = present ? 0 : s.ig_miss + 1;
        if (s.ig_miss >= kCloseFrames) {
            s.ig_up = false;
            s.ig_hits = 0;
            if (log_ok(true)) NXLOG_I(kTag, "menu: in-game menu closed (layout %u/%u, cursor '%s')", s.ig_lay_id, s.ig_lay_sub, s.ig_cursor);
        }
    }
    return s.ig_up;
}

}  // namespace nxrt::games::pkp
