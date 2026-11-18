// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: a Save prompt in the pause menu's key guide (issue #354). [pkp] save_now, on by default.
//
// The pause menu (+) shows its actions in the game's key guide widget: A select, B close, (-) Save and Quit (the game's own, which asks
// first and then returns to the title). This module adds one more entry, "Save", which writes the save and leaves the player in the world.
// It is a key guide client (pkp_key_guide.h), the same mechanism as the title's Quit Game, the bag's Sort and the crafting screen's Search:
//   * the pause menu is known by its key guide set, not by the screen: every variant of it shows one of its own actions, Save and Quit
//     alone, Manage Connected Players / End Session / Go Back Home in a Link Play session (kPauseGuides; key guide set table dump: sets
//     33, 63, 64, 102, 108, 109, 171). The menu pointer's scan of the screen only follows the home's pause menu; over play outdoors it lets
//     go of the menu after a moment, so the prompt went away there.
//   * when a widget takes such a set, the Save guide (158, the crafting search's row; its icon and text are replaced) goes into the set's
//     first free slot, before the widget reads the set. Only pause sets get it, so it never has to be taken out again.
//   * the part is filled: the icon is the first of kButtons no other prompt of the set uses (Y, then R; click only when both are taken),
//     the text is "Save" in the game's language, and the part's layout goes to the menu pointer.
//   * a left click on the prompt, or that button on any pad, while the widget still shows the pause set, asks for the save; the game
//     thread then queues it with the game's own request, the one its Save and Quit dialog makes (0x241324, kind 2, pkp_layout.h;
//     pkp_story_guard.cpp uses the same call), and says so with a notice.
#include "ui/pkp_save_now.h"
#include "ui/pkp_key_guide.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"

#include <array>
#include <atomic>
#include <cstring>

#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
// The key guide row the prompt borrows: the crafting search's "Search" row (158), a row that exists. Its icon and text are ours; the
// crafting client lets go of its part when another set is taken (pkp_craft.cpp), so the two never fight over it.
constexpr uint16_t kSaveGuide = 158;
// The pause menu's own actions (key guide ids): 90 Save & Quit, 148 End Session, 149 and 191 Go Back Home, 150 Manage Connected Players.
// A set holding one of them is the pause menu; no other set of the table holds them.
constexpr uint16_t kPauseGuides[] = {90, 148, 149, 150, 191};

// "Save" in the game's languages (0..8, the order of its text banks: the key guide's own words checked)
constexpr const char* kLabel[9] = {
    "セーブ", "Save", "Sauvegarder", "Salva", "Speichern", "Guardar", "儲存", "저장", "保存",
};

// A prompt button: an action (input manager table) shown as that button, and the pad bit the prompt answers to.
struct Button {
    uint16_t act;
    uint8_t button;
    hid::NpadButtonBit pad;
    const char* name;
};
// The pause menu's set uses A, B and (-); Y and R are free there. Y first (the sort uses Y in the bag, so it is free in menus).
constexpr Button kButtons[] = {{0x97, 4, hid::kBtnY, "Y"}, {0x03, 0xb, hid::kBtnR, "R"}};

using guest::rd;
using guest::wr;

struct State {
    uint64_t base = 0;
    int client = -1;
    std::atomic<uint64_t> widget{0};    // the key guide widget that last took a pause set
    std::atomic<uint32_t> set{0};       // that pause set
    uint64_t row = 0;                   // key guide 158's table row
    std::atomic<bool> save{false};      // a Save was asked for (click or button)
    std::atomic<int> button{-1};        // the pad bit shown on the prompt, -1 = click only
    uint64_t prev_buttons = 0;          // main thread: the pads' buttons last frame
    bool logged_fill = false;
    bool logged_full = false;
    int tries = 0;                      // game frames spent waiting for the save manager
    int pending = 0;                    // game frames until the save manager is asked whether it finished
};

State g;

uint32_t language() {
    const uint64_t lp = rd<uint64_t>(g.base + layout::kLanguage);
    return lp && guest::ok(lp, 4) ? rd<uint32_t>(lp) : 1;
}

// The button of an action (input manager table, [main+0x3561718] +0xcd8), -1 if unknown.
int button_of(uint16_t act) {
    const uint64_t im = rd<uint64_t>(g.base + layout::kInputMgr);
    if (!im || !guest::ok(im + layout::kActionButtons, 16)) return -1;
    const uint64_t b = rd<uint64_t>(im + layout::kActionButtons), e = rd<uint64_t>(im + layout::kActionButtons + 8);
    if (!b || e < b || act >= (e - b) / 4 || !guest::ok(b + act * 4ull, 4)) return -1;
    return rd<uint8_t>(b + act * 4ull);
}

// The pause menu's key guide is up: the widget that took the pause set still shows it (any thread).
bool pause_shown() {
    return key_guide::shows(g.widget.load(std::memory_order_acquire), g.set.load(std::memory_order_acquire));
}

// A widget takes a set: a pause set gets the Save guide in its first free slot. The ids row is looked up again on every call: a scene
// change (the home) can move it. Guest thread, at the entry of the fill, so the widget reads the set with the guide in it.
void set_taken(::Ctx* c, uint64_t w, uint32_t set) {
    const uint64_t ids = key_guide::set_ids(c, set);
    if (!ids) return;
    bool pause = false;
    int at = -1, free_slot = -1;
    for (int i = 0; i < key_guide::kSetSlots; ++i) {
        const uint16_t v = rd<uint16_t>(ids + 2 * i);
        for (uint16_t p : kPauseGuides) pause |= v == p;
        if (v == kSaveGuide) at = i;
        if (v == 0 && free_slot < 0) free_slot = i;
    }
    if (!pause) return;
    if (!g.row) g.row = key_guide::guide_row(c, kSaveGuide);
    g.set.store(set, std::memory_order_release);
    g.widget.store(w, std::memory_order_release);
    if (at >= 0) return;
    if (free_slot >= 0) {
        wr<uint16_t>(ids + 2 * free_slot, kSaveGuide);
        NXLOG_I(kTag, "save now: key guide %u added to the pause menu's set %u (slot %d)", kSaveGuide, set, free_slot);
    } else if (!g.logged_full) {
        g.logged_full = true;
        NXLOG_W(kTag, "save now: the pause menu's key guide set %u has no free slot, no Save prompt", set);
    }
}

// The Save part is filled: its button icon, its text and its layout for the menu pointer.
void part_filled(::Ctx* c, const key_guide::Part& p) {
    if (!g.row || p.row != g.row) return;
    const uint64_t w = g.widget.load(std::memory_order_acquire);
    if (!w || p.widget != w) return;
    // The button: the first of kButtons that no other prompt of the set uses (the set is what the player sees).
    std::array<bool, 256> used{};
    key_guide::Entries e;
    if (key_guide::entries(w, &e)) {
        for (uint64_t i = 0; i < e.count; ++i) {
            const uint16_t id = e.id(i);
            if (id == kSaveGuide) continue;
            const uint64_t row = key_guide::guide_row(c, id);
            const uint64_t rec = row ? rd<uint64_t>(row + 8) : 0;
            if (!rec) continue;
            const int b = button_of(rd<uint16_t>(rec));
            if (b >= 0) used[b] = true;
        }
    }
    int pick = -1;
    for (int i = 0; i < static_cast<int>(std::size(kButtons)) && pick < 0; ++i)
        if (button_of(kButtons[i].act) == kButtons[i].button && !used[kButtons[i].button]) pick = i;
    g.button.store(pick >= 0 ? static_cast<int>(kButtons[pick].pad) : -1, std::memory_order_release);
    if (pick >= 0) key_guide::set_icon(c, p, kButtons[pick].act);
    const uint32_t lang = language();
    key_guide::set_text(c, p, kLabel[lang < 9 ? lang : 1]);
    key_guide::set_click_layout(g.client, key_guide::layout_of(c, p));
    if (!g.logged_fill) {
        g.logged_fill = true;
        NXLOG_I(kTag, "save now: Save prompt in the pause menu (part 0x%llx, language %u, button %s)",
                static_cast<unsigned long long>(p.part), lang, pick >= 0 ? kButtons[pick].name : "none (click only)");
    }
}

// Main thread: a left click on the Save prompt asks for the save (the menu pointer does not send A for it).
bool click(play::Act* act) {
    if (!pause_shown()) return false;
    g.save.store(true, std::memory_order_release);
    *act = play::Act::None;
    NXLOG_I(kTag, "save now: click on the pause menu's Save prompt");
    return true;
}

void install(const FeatureInstall& in) {
    if (!settings::get_bool(settings::Key::SaveNow)) {
        NXLOG_I(kTag, "save now: off ([pkp] save_now)");
        return;
    }
    g.base = in.base;
    g.client = key_guide::add({.name = "save now",
                               .set_taken = &set_taken,
                               .part_filled = &part_filled,
                               .click = &click,
                               .click_note = "the pause menu's Save prompt"});
    NXLOG_I(kTag, "save now: on (a Save prompt in the pause menu's key guide writes the save and stays in the world; [pkp] save_now)");
}

void remove() {
    g.base = 0;
    g.widget.store(0);
    g.set.store(0);
    g.row = 0;
    g.save.store(false);
    g.button.store(-1);
    g.prev_buttons = 0;
    g.logged_fill = false;
    g.logged_full = false;
    g.tries = 0;
    g.pending = 0;
}

// Main thread, once per frame: the prompt's button on any pad asks for the save.
void frame(bool, double, play::GameFrame*) {
    if (!g.base) return;
    const bool up = pause_shown();
    const uint64_t buttons = hid::sampled_buttons();
    const int b = g.button.load(std::memory_order_acquire);
    const uint64_t mask = b >= 0 ? 1ull << b : 0;
    if (up && mask && (buttons & mask) && !(g.prev_buttons & mask)) {
        g.save.store(true, std::memory_order_release);
        NXLOG_I(kTag, "save now: button on the pause menu's Save prompt");
    }
    g.prev_buttons = up ? buttons : ~uint64_t{0};   // a button held from before the menu opened does not count
}

// The game thread, once a frame: queue the game's own save request.
void game_frame(::Ctx* c) {
    if (!g.base) return;
    if (g.pending > 0 && --g.pending == 0) {
        const uint64_t manager = rd<uint64_t>(g.base + layout::kSaveManager);
        if (manager && (guest::call(c, g.base + layout::kSaveDone, manager, 0) & 1)) {
            notices::post("pkp.save_now", "Saved", 2.0);
            NXLOG_I(kTag, "save now: the save manager finished");
        } else {
            g.pending = 30;   // still writing: ask again in half a second
        }
    }
    if (!g.save.load(std::memory_order_acquire)) return;
    if (++g.tries > 600) {
        g.save.store(false, std::memory_order_release);
        g.tries = 0;
        NXLOG_W(kTag, "save now: the save manager stayed busy, the save was not requested");
        return;
    }
    const uint64_t game = rd<uint64_t>(g.base + layout::kGameState);
    const uint64_t manager = rd<uint64_t>(g.base + layout::kSaveManager);
    const uint64_t root = rd<uint64_t>(g.base + layout::kSaveRoot);
    if (!guest::ok(game, layout::kStateAutosave + 4) || !guest::ok(manager, 0x40) || !guest::ok(root, layout::kSaveRootHome + 1)) return;
    if (rd<uint32_t>(game + layout::kStateAutosave) - 1 < 4) return;   // the autosave is writing
    const uint64_t event = rd<uint64_t>(game + layout::kStateEvent);
    if (event && guest::ok(event, layout::kEventBusy + 1) && rd<uint8_t>(event + layout::kEventBusy)) return;   // the game is busy
    uint8_t request[12] = {};
    const uint32_t type = 5, kind = 2;   // kind 2: the player's own file, what Save and Quit writes (0x241324, pkp_layout.h)
    std::memcpy(request, &type, 4);
    request[5] = rd<uint8_t>(root + layout::kSaveRootHome);
    std::memcpy(request + 8, &kind, 4);
    if (!(guest::call(c, g.base + layout::kSaveRequest, manager, reinterpret_cast<uint64_t>(request), 0, 0) & 1)) return;
    g.save.store(false, std::memory_order_release);
    g.tries = 0;
    g.pending = 30;
    notices::post("pkp.save_now", "Saving...", 30.0);
    NXLOG_I(kTag, "save now: the game's own save request is queued (kind 2)");
}

}  // namespace

const Feature kSaveNowFeature = {.name = "save_now", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
