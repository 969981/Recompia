// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus (pkp_menu.h). Offsets in `main` of the 2.0.1 update, found in IDA (work/re/menus.md).
//
// The game's UI is KTGL screen layouts: a layout object holds its panes in a list, each pane has a name, a size and a world matrix in a
// 1920x1080 canvas (origin at the centre, y up). The game itself never hit-tests them for its menus (the engine's script call
// kids::placeable::screenlayout::DoPaneHitTest exists, 0x2611d00, and is where the pane geometry below comes from), and its menus move a
// cursor part (0000_parts_cursor_finger_s, 0250_parts_talk_cursor, 0000_parts_cursor_triangle ...) next to the selected entry when the
// d-pad moves the selection. So, once a frame while a menu is up (play mode's menu context: the camera's input is not running):
//   * every layout in use in the UI manager's pools ([0x35aebf8] +0x84808 / +0x8c858, see kPoolA; placeable +40 -> +16 =
//     the layout) is walked and each visible pane's screen rectangle computed;
//   * the entry under the pointer is the smallest visible "item" pane (a button, list cell, choice, tab ... by its name) containing it;
//   * the selected entry is the item pane nearest to the game's cursor pane (a pane named "...cursor...");
//   * the pointer does not move the selection (that guess is not reliable enough to steer by, see menu_frame): a left click is A on the
//     game's own selection, or the button of a key guide under it. Right click (B) and the wheel (d-pad up / down) are menu-context
//     bindings (pkp_game.cpp).
// The hover and the selection still feed the log, the status line, the wheel over a slider and the in-game menu detection;
// [pkp] menu_log = 2 lists the panes of the layouts on screen.
//
// Files: pkp_menu_scan.cpp (the layouts and their visible panes), pkp_menu_draw.cpp (the drawn set, from the pane draws), pkp_menu_pick.cpp
// (the cursor and the selection), pkp_menu_in_game.cpp (menus over play), on the state of pkp_menu_internal.h. This file installs the hooks,
// runs the frame and routes the clicks.
#include "ui/pkp_menu.h"
#include "ui/pkp_menu_internal.h"
#include "ui/pkp_key_guide.h"
#include "core/pkp_settings.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "nxrt/hooks.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::menu_detail {

State& st() {
    static State s;
    return s;
}

}  // namespace nxrt::games::pkp::menu_detail

namespace nxrt::games::pkp {

using namespace menu_detail;

namespace {

void install(const FeatureInstall& in) {
    State& s = st();
    s.base = in.base;
    s.enabled = settings::get_bool(settings::Key::MenuMouse);
    s.log_level = static_cast<int>(settings::get_int(settings::Key::MenuLog));
    s.ig_enabled = settings::get_bool(settings::Key::MenuInGame);     // on: drawn-layout detection confirmed with the bag (p5m)
    s.ig_ignore.clear();
    {   // "id/sub id/sub ..." (the ids the "in-game menu up" log line prints)
        const std::string list = settings::get_str(settings::Key::MenuInGameIgnore);
        unsigned id = 0, sub = 0;
        int used = 0;
        for (const char* q = list.c_str(); *q;) {
            if (std::sscanf(q, " %u/%u%n", &id, &sub, &used) == 2 && used > 0) {
                s.ig_ignore.insert(static_cast<uint64_t>(id) << 32 | sub);
                q += used;
            } else {
                ++q;
            }
        }
    }
    s.key_guides.clear();
    {   // "id=act ..." (act names as in the [switch] section: btn_minus, btn_plus, btn_x ...). 881468973 = the title's "(-) Settings" guide
        // (p5m: 'Win_000_btn_key_guide_0' at 1722,1003..1920,1075; gameplay's HUD guides use the layout too, see key_guide_drawn); "" turns them off
        const std::string list = settings::get_str(settings::Key::MenuKeyGuides);
        char name[32];
        unsigned id = 0;
        int used = 0;
        for (const char* q = list.c_str(); *q;) {
            play::Act a;
            if (std::sscanf(q, " %u=%31[A-Za-z0-9_]%n", &id, name, &used) == 2 && used > 0) {
                if (play::parse_act(name, &a)) s.key_guides[id] = a;
                else NXLOG_W(kTag, "menu_key_guides: unknown action '%s'", name);
                q += used;
            } else {
                ++q;
            }
        }
    }
    for (const DrawSite& d : kDrawSites)
        in.hooks.add_mid(in.base + d.off, &hook_pane_draw, reinterpret_cast<void*>(static_cast<uintptr_t>(d.reg)));
    s.installed = true;
    NXLOG_I(kTag, "menu mouse %s (the wheel and keys move the selection, a left click confirms the selected entry, the pointer does not pick; [pkp] menu_log %d; 3 = the layout chain once a second)", s.enabled ? "on" : "off", s.log_level);
}

void remove() {   // the registry has removed the pane draw hooks
    st().installed = false;
    st().ig_up = false;
}

void status_text(char* buf, size_t n) {
    const State& s = st();
    std::snprintf(buf, n, " menu_layouts=%u menu_panes=%u menu_entries=%u hover='%s' selected='%s' clicks=%u in_game_menu=%d (%u/%u) in_game_opens=%u",
                  s.n_layouts.load(), s.n_panes.load(), s.n_items.load(), s.hover_name, s.focus_name, s.n_clicks.load(),
                  s.ig_up ? 1 : 0, s.ig_lay_id, s.ig_lay_sub, s.n_ig_opens.load());
}

}  // namespace

const Feature kMenuFeature = {.name = "menu", .install = &install, .remove = &remove, .status_text = &status_text};

play::Act menu_frame(bool menu_up, const play::mm::Geometry& g) {
    State& s = st();
    play::mm::Pointer& ptr = play::mm::pointer();
    const play::mm::Snapshot snap = ptr.begin_frame(menu_up, g);
    s.active = false;
    if (s.log_level >= 3) {
        static uint32_t n = 0;
        if (++n % 600 == 0) NXLOG_I(kTag, "menu: frame menu_up %d active %d inside %d shown %d", menu_up ? 1 : 0, snap.active ? 1 : 0, snap.inside ? 1 : 0, snap.shown ? 1 : 0);
    }
    if (!s.installed || !s.enabled || !snap.active) {
        if (s.was_up && log_ok(true)) NXLOG_I(kTag, "menu: closed");
        s.was_up = false;
        // The pointer can be off while the pause menu is still up (the mouse outside the picture, a controller only): the last scan's
        // answer stands until play mode leaves the menu context. Outside it nothing can be up.
        if (!menu_up) s.pause_up.store(false, std::memory_order_relaxed);
        s.panes.clear();
        s.hovered = s.focused = s.cursor = s.slider = -1;
        return play::Act::None;
    }
    s.active = true;
    s.pages.clear();
    scan();
    // the UI plane: Pokopia keeps its menus and HUD 16:9 and centred on wider shapes (p5l at 21:9: the whole-picture mapping hit one slot left)
    const float px = (snap.nx + 1.f) * 0.5f * kCanvasW, py = (1.f - snap.ny) * 0.5f * kCanvasH;
    pick(px, py, snap.inside && snap.shown);
    s.frame_scanned = true;
    s.frame_present = (s.cursor >= 0 && s.focused >= 0) || key_guide_drawn(s);
    // The pause menu's main view (its 9 buttons) is up. Published here as well as in menu_in_game: the home opens the menu where play
    // mode's in-game path does not run, so the Save prompt (pkp_save_now.cpp) has to follow this scan.
    s.pause_up.store(s.focused >= 0 && std::strncmp(s.panes[s.focused].name, "CL_0300_parts_btn_mainmenu_", 27) == 0,
                     std::memory_order_relaxed);
    log_layouts();
    s.was_up = true;
    {   // which rule found the selection (pick_extend), once per change: the proof for menus outside play
        const Pane* c = s.cursor >= 0 ? &s.panes[s.cursor] : nullptr;
        const Pane* f = s.focused >= 0 ? &s.panes[s.focused] : nullptr;
        const uint64_t sig = c && f ? (static_cast<uint64_t>(s.mode) << 62) ^ (static_cast<uint64_t>(c->lay_id) << 32) ^ f->lay_id ^
                                          (static_cast<uint64_t>(s.group.size()) << 48)
                                    : 0;
        if (sig != s.mode_sig && c && f && log_ok(true)) {
            static const char* const kMode[] = {"cursor", "list entry holding the cursor's part", "sibling button layouts"};
            NXLOG_I(kTag, "menu: selection by %s: cursor '%s' (layout %u/%u) on '%s' (layout %u/%u), %zu sibling entries", kMode[s.mode], c->name,
                    c->lay_id, c->lay_sub, f->name, f->lay_id, f->lay_sub, s.group.size());
        }
        s.mode_sig = sig;
    }

    const Pane* hov = s.hovered >= 0 ? &s.panes[s.hovered] : nullptr;
    const Pane* foc = s.focused >= 0 ? &s.panes[s.focused] : nullptr;
    const uint64_t hov_ptr = hov ? hov->ptr : 0, foc_ptr = foc ? foc->ptr : 0;
    if (hov_ptr != s.last_hover) {
        std::snprintf(s.hover_name, sizeof s.hover_name, "%s", hov ? hov->name : "");
        if (log_ok()) {
            if (hov) NXLOG_I(kTag, "menu: hover '%s' (layout %u/%u, drawn rect %.0f,%.0f..%.0f,%.0f) at %.0f,%.0f; selected '%s'", hov->name, hov->lay_id,
                             hov->lay_sub, hov->x0, hov->y0, hov->x1, hov->y1, px, py, foc ? foc->name : "");
            else NXLOG_I(kTag, "menu: hover nothing at %.0f,%.0f", px, py);
        }
        s.last_hover = hov_ptr;
    }
    if (foc_ptr != s.last_focus) {
        std::snprintf(s.focus_name, sizeof s.focus_name, "%s", foc ? foc->name : "");
        if (foc && log_ok()) NXLOG_I(kTag, "menu: selected '%s' (cursor '%s')", foc->name, s.cursor >= 0 ? s.panes[s.cursor].name : "");
        s.last_focus = foc_ptr;
    }

    // Left click: a key guide under the pointer ("(-) Settings" on the title) sends its button, any other click is A on the game's own
    // selection. The pointer never moves the selection: the selected entry is only guessed from where the cursor part is drawn, and in
    // grids with a bobbing cursor (the main menu's triangle over CL_0300_parts_btn_mainmenu_0000..0008) the guess jumped between entries,
    // so d-pad steps toward the hovered entry started from the wrong one and sent the selection around the menu (fork issue #7).
    // totk-pc-recomp keeps the mouse out of its game's menus the same way. The selection moves with the wheel and the keys.
    if (!snap.click) return play::Act::None;
    ptr.click_used();
    ++s.n_clicks;
    if (snap.inside && snap.shown && !s.key_guides.empty()) {
        for (const Pane& p : s.panes) {
            if (!p.item || !p.contains(px, py)) continue;
            // our own key guide entries (the title's Quit Game, the bag's Hold to Sort, the crafting screen's Search) are other instances of the
            // same key guide layout: the instance a client last filled takes the click while the client still shows it (pkp_key_guide.h)
            if (p.lay >= 0 && p.lay < static_cast<int>(s.layouts.size())) {
                play::Act act = play::Act::None;
                const char* note = "";
                if (key_guide::click(s.layouts[p.lay].ptr, &act, &note)) {
                    if (log_ok(true)) NXLOG_I(kTag, "menu: click on %s", note);
                    return act;
                }
            }
            const auto it = s.key_guides.find(p.lay_id);
            if (it == s.key_guides.end()) continue;
            if (log_ok(true)) NXLOG_I(kTag, "menu: click on key guide '%s' (layout %u/%u) -> %s", p.name, p.lay_id, p.lay_sub, play::act_info(it->second).name);
            return it->second;
        }
    }
    if (log_ok(true)) NXLOG_I(kTag, "menu: click -> A on '%s'", foc ? foc->name : "");
    return play::Act::BtnA;
}

bool menu_over_value() { return st().active && st().slider >= 0; }

bool menu_active() { return st().active; }



}  // namespace nxrt::games::pkp
