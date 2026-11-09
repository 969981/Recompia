// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, title screen: a "Quit Game" key guide next to "(-) Settings" (issue #319). [pkp] title_quit, on by default.
//
// The title's prompts are the game's key guide widget (pkp_key_guide.h): 0x7100e60be4 (manager, player, set) gives it a key guide set, and
// the title uses set 152 = {233}: action 0x80 (the (-) button) and "Settings". A key guide client (pkp_key_guide.h):
//   * the widget takes set 152: the set's second id becomes key guide 267 (action 4, the (+) button, "Credits"; the game's only other
//     user is set 183 = {233, 267}, the title with a Credits prompt, which is left alone), so the widget builds a second prompt in the
//     game's own style, left of Settings. Written into the table row once.
//   * key guide 267's part is filled on the title widget: its text is replaced with "Quit Game" in the game's language, and its layout
//     goes to the menu pointer.
// The title itself takes no (+) (A, B, the d-pad and (-) only), so the button is free there: once a frame (main thread) while the title's
// widget shows set 152, a press of (+) on any pad as the hid service samples it (the keyboard's Escape by default) opens the runtime's
// "Close the game?" prompt. A click on the prompt sends (+) too (the key guide's click routing, pkp_menu.cpp).
#include "pkp_title.h"
#include "pkp_guest.h"
#include "pkp_key_guide.h"
#include "pkp_layout.h"

#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
using layout::kLanguage;
constexpr uint32_t kTitleSet = 152, kQuitGuide = 267;

// "Quit Game" in the game's languages (0..8, the order of its text banks: the key guide bank's own "Settings" / "Quit" words checked)
constexpr const char* kLabel[9] = {
    "ゲームをやめる", "Quit Game", "Quitter le jeu", "Esci dal gioco", "Spiel beenden", "Salir del juego", "結束遊戲", "게임 종료", "结束游戏",
};

struct State {
    uint64_t base = 0;
    play::GameHost* host = nullptr;
    int client = -1;            // the key guide client
    uint64_t widget = 0;        // the title's key guide widget (set 152 seen last on it)
    uint64_t row = 0;           // key guide 267's table row
    bool patched = false;
    uint64_t prev_buttons = 0;
} g;

using guest::rd;
using guest::wr;
using guest::ok;

void set_taken(::Ctx* c, uint64_t widget, uint32_t set) {
    if (set != kTitleSet) {
        if (widget == g.widget) g.widget = 0;
        return;
    }
    g.widget = widget;
    if (g.patched) return;
    g.patched = true;
    const uint64_t ids = key_guide::set_ids(c, kTitleSet);
    g.row = key_guide::guide_row(c, kQuitGuide);
    if (!ids || !g.row) {
        NXLOG_W(kTag, "title quit: the title's key guide set or key guide %u not found, no Quit Game prompt", kQuitGuide);
        return;
    }
    for (int i = 0; i < key_guide::kSetSlots; ++i) {
        const uint16_t v = rd<uint16_t>(ids + 2 * i);
        if (v == kQuitGuide) break;
        if (v == 0) {
            wr<uint16_t>(ids + 2 * i, static_cast<uint16_t>(kQuitGuide));
            NXLOG_I(kTag, "title quit: key guide %u added to the title's set %u (slot %d): (+) Quit Game", kQuitGuide, kTitleSet, i);
            break;
        }
    }
}

void part_filled(::Ctx* c, const key_guide::Part& p) {
    if (!g.widget || !g.row || p.row != g.row || p.widget != g.widget) return;
    const uint64_t lp = rd<uint64_t>(g.base + kLanguage);
    const uint32_t lang = lp && ok(lp, 4) ? rd<uint32_t>(lp) : 1;
    key_guide::set_text(c, p, kLabel[lang < 9 ? lang : 1]);
    const uint64_t lay = key_guide::layout_of(c, p);   // for the menu pointer: a click on it is (+)
    key_guide::set_click_layout(g.client, lay);
    NXLOG_I(kTag, "title quit: prompt text set (part 0x%llx, slot %u, language %u, layout 0x%llx)", static_cast<unsigned long long>(p.part), p.slot,
            lang, static_cast<unsigned long long>(lay));
}

// The title's key guides (set 152) are on screen.
bool title_up() { return key_guide::shows(g.widget, kTitleSet); }

// The title's Quit Game is a second instance of the same key guide layout: its own instance sends (+), only while the title's set is
// shown (the widget reuses its part instances for later sets).
bool click(play::Act* act) {
    if (!title_up()) return false;
    *act = play::Act::BtnPlus;
    return true;
}

void install(const FeatureInstall& in) {
    if (!config_get_bool("pkp", "title_quit", true)) {
        NXLOG_I(kTag, "title quit: off ([pkp] title_quit)");
        return;
    }
    g.base = in.base;
    g.host = in.host;
    g.client = key_guide::add({.name = "title quit", .set_taken = &set_taken, .part_filled = &part_filled, .click = &click,
                               .click_note = "the title's Quit Game key guide -> btn_plus"});
    NXLOG_I(kTag, "title quit: on (a (+) Quit Game prompt next to (-) Settings on the title)");
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

const Feature kTitleFeature = {.name = "title", .install = &install, .frame = &frame};

}  // namespace nxrt::games::pkp
