// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, title screen: a "Quit Game" key guide next to "(-) Settings" (issue #319). [pkp] title_quit, on by default.
//
// The title's prompts are the game's key guide widget (one per player, manager [0x3483fb8], ctor 0x7100ec4958): 0x7100e60be4 (manager,
// player, set) gives it a key guide set, and 0x7100ec5de0 (widget, set) fills it: a set is a row of the data table at [0x35618f0] +0x1048
// holding up to six key guide ids (u16, 1..399). Each id is a row of the table at +0x1050: {u16 action, ff, 00, u8 text, 7f, ..} (the
// action picks the button icon, the text byte is an entry of the key guide text bank). The widget lays out one instance of the key guide
// part (layout 881468973: icon '000_key_1', texts 'TextBox_0000/0001') per id and the area 'CL_0000_area_btn_keyguide' of the title layout
// places them from the right. The title uses set 152 = {233}: action 0x80 (the (-) button) and "Settings".
//   * entry 0x7100ec5de0, set 152: the set's second id becomes key guide 267 (action 4, the (+) button, "Credits"; the game's only other
//     user is set 183 = {233, 267}, the title with a Credits prompt, which is left alone), so the widget builds a second prompt in the
//     game's own style, left of Settings. Written into the table row once.
//   * inline 0x7100ec69b4, the end of the part's fill (0x7100ec6800, a vtable call from 0x7100ec54a0 whose x19 is the widget: saved at
//     [x29 + 0x38]): for that key guide on the title widget its text is replaced with "Quit Game" through text 0xa8 ("<string;0(lstr1)>",
//     0x7100f35f20 (part, text slot w20, 0xa8, utf8)). x21 = the key guide's table row, x19 = the part.
// The title itself takes no (+) (A, B, the d-pad and (-) only), so the button is free there: once a frame (main thread) while the title's
// widget shows set 152 (+0xf0 = the set, +0x90 = state 1 or 2 while shown), a press of (+) on any pad as the hid service samples it
// (the keyboard's Escape by default) opens the runtime's "Close the game?" prompt. A click on the prompt sends (+) too (pkp_menu.cpp,
// menu_set_quit_guide).
#include "pkp_title.h"
#include "pkp_guest.h"
#include "pkp_layout.h"
#include "pkp_tables.h"

#include <cstring>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "pkp_menu.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
using layout::kFillSet;
using layout::kPartFilled;
using layout::kTextString;
using layout::kLayoutOf;
using layout::kLanguage;
using layout::kSetTable;
using layout::kGuideTable;
constexpr uint64_t kWidgetSet = 0xf0;
using layout::kWidgetState;
using layout::kStringText;
constexpr uint32_t kTitleSet = 152, kQuitGuide = 267;
constexpr int kSetSlots = 6;

// "Quit Game" in the game's languages (0..8, the order of its text banks: the key guide bank's own "Settings" / "Quit" words checked)
constexpr const char* kLabel[9] = {
    "ゲームをやめる", "Quit Game", "Quitter le jeu", "Esci dal gioco", "Spiel beenden", "Salir del juego", "結束遊戲", "게임 종료", "结束游戏",
};

struct State {
    uint64_t base = 0;
    play::GameHost* host = nullptr;
    uint64_t widget = 0;        // the title's key guide widget (set 152 seen last on it)
    uint64_t row = 0;           // key guide 267's table row
    bool patched = false;
    uint64_t prev_buttons = 0;
} g;

using guest::rd;
using guest::wr;
using guest::ok;

uint64_t table_row(::Ctx* c, uint64_t table_off, uint32_t id) { return tables::row(c, g.base, table_off, id); }

int hook_fill_set(::Ctx* c, uint64_t, void*) {
    const uint64_t widget = c->x[0];
    const uint32_t set = static_cast<uint32_t>(c->x[1] & 0xff);
    if (set != kTitleSet) {
        if (widget == g.widget) g.widget = 0;
        return 0;
    }
    g.widget = widget;
    if (g.patched) return 0;
    g.patched = true;
    const uint64_t srow = table_row(c, kSetTable, kTitleSet);
    const uint64_t ids = srow ? rd<uint64_t>(srow + 8) : 0;
    g.row = table_row(c, kGuideTable, kQuitGuide);
    if (!ids || !ok(ids, 2 * kSetSlots) || !g.row) {
        NXLOG_W(kTag, "title quit: the title's key guide set or key guide %u not found, no Quit Game prompt", kQuitGuide);
        return 0;
    }
    for (int i = 0; i < kSetSlots; ++i) {
        const uint16_t v = rd<uint16_t>(ids + 2 * i);
        if (v == kQuitGuide) break;
        if (v == 0) {
            wr<uint16_t>(ids + 2 * i, static_cast<uint16_t>(kQuitGuide));
            NXLOG_I(kTag, "title quit: key guide %u added to the title's set %u (slot %d): (+) Quit Game", kQuitGuide, kTitleSet, i);
            break;
        }
    }
    return 0;
}

void hook_part_filled(::Ctx* c, uint64_t, void*) {
    if (!g.widget || !g.row || c->x[21] != g.row) return;
    const uint64_t fp = c->x[29];
    if (!ok(fp + 0x38, 8) || rd<uint64_t>(fp + 0x38) != g.widget) return;     // the caller's (0x7100ec54a0) x19: this part's widget
    const uint64_t part = c->x[19];
    const uint32_t slot = static_cast<uint32_t>(c->x[20]);
    const uint64_t lp = rd<uint64_t>(g.base + kLanguage);
    const uint32_t lang = lp && ok(lp, 4) ? rd<uint32_t>(lp) : 1;
    const char* s = kLabel[lang < 9 ? lang : 1];
    const size_t n = std::strlen(s);
    const uint64_t buf = (c->x[31] - (n + 0x40)) & ~uint64_t{0xf};     // below the guest stack pointer: the game reads it as guest memory
    std::memcpy(reinterpret_cast<void*>(buf), s, n + 1);
    guest::call_n<4>(c, g.base + kTextString, {part, slot, kStringText, buf}, buf - 0x10);
    // the part's layout, for the menu pointer: a click on it is (+)
    const uint64_t src = rd<uint64_t>(part + 0x38);
    const uint64_t lay = src ? guest::call_n<4>(c, g.base + kLayoutOf, {src}, buf - 0x10) : 0;
    menu_set_quit_guide(lay);
    NXLOG_I(kTag, "title quit: prompt text set (part 0x%llx, slot %u, language %u, layout 0x%llx)", static_cast<unsigned long long>(part), slot, lang,
            static_cast<unsigned long long>(lay));
}

void install(const FeatureInstall& in) {
    if (!config_get_bool("pkp", "title_quit", true)) {
        NXLOG_I(kTag, "title quit: off ([pkp] title_quit)");
        return;
    }
    const uint64_t base = in.base;
    g.base = base;
    g.host = in.host;
    in.hooks.add_entry(base + kFillSet, &hook_fill_set);
    in.hooks.add_inline(base + kPartFilled, &hook_part_filled);
    NXLOG_I(kTag, "title quit: on (a (+) Quit Game prompt next to (-) Settings on the title; hooks main+0x%llx, +0x%llx)",
            static_cast<unsigned long long>(kFillSet), static_cast<unsigned long long>(kPartFilled));
}

// main thread, once per frame: (+) on the title opens the quit prompt
void frame(bool, double, play::GameFrame*) {
    if (!g.host) return;
    const uint64_t buttons = hid::sampled_buttons();
    const uint64_t plus = hid::btn(hid::kBtnPlus);
    const bool press = (buttons & plus) && !(g.prev_buttons & plus);
    g.prev_buttons = buttons;
    if (!press || !title_up()) return;
    NXLOG_I(kTag, "title quit: (+) on the title, the Close the game? prompt opens");
    g.host->request_quit_prompt();
}

}  // namespace

bool title_up() {
    const uint64_t w = g.widget;
    if (!w || !ok(w + kWidgetSet, 8)) return false;
    return rd<uint8_t>(w + kWidgetSet) == kTitleSet && rd<int32_t>(w + kWidgetState) - 1u < 2u;
}

const Feature kTitleFeature = {.name = "title", .install = &install, .frame = &frame};

}  // namespace nxrt::games::pkp
