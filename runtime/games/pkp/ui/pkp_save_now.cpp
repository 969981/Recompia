// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: a Save prompt in the pause menu (#354) and a quick-save key in play (#659). [pkp] save_now, on by default.
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
//   * a click or button on the prompt, or the Save binding in play, asks for the save. The game thread then saves the way the game's
//     own Save and Quit does: it asks the game's autosave to save now (pkp_layout.h). The autosave copies the game's state first and
//     writes the player's file, the island file and the screenshot; a visitor in Link Play writes no island file, as the game decides
//     for its autosave. A notice says when the save has been written.
#include "ui/pkp_save_now.h"
#include "ui/pkp_key_guide.h"
#include "core/pkp_guest.h"
#include "core/pkp_language.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"

#include <array>
#include <atomic>

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
    play::GameHost* host = nullptr;
    int client = -1;
    std::atomic<uint64_t> widget{0};    // the key guide widget that last took a pause set
    std::atomic<uint32_t> set{0};       // that pause set
    uint64_t row = 0;                   // key guide 158's table row
    std::atomic<bool> save{false};      // a Save was asked for (click, pad button or shortcut)
    std::atomic<int> button{-1};        // the pad bit shown on the prompt, -1 = click only
    uint64_t prev_buttons = 0;          // main thread: the pads' buttons last frame
    uint64_t save_activations = 0;      // main thread: the last Save activation seen
    bool logged_fill = false;
    bool logged_full = false;
    int tries = 0;                      // game frames spent waiting for the autosave to be idle and on
    int watch = 0;                      // game frames left to watch the asked-for save, 0 = none
    bool writing = false;               // the asked-for save reached the write step
};

State g;

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
    const uint32_t lang = language::index(g.base);
    key_guide::set_text(c, p, kLabel[lang]);
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
    g.host = in.host;
    g.client = key_guide::add({.name = "save now",
                               .set_taken = &set_taken,
                               .part_filled = &part_filled,
                               .click = &click,
                               .click_note = "the pause menu's Save prompt"});
    NXLOG_I(kTag, "save now: on (pause menu prompt and quick-save key; [pkp] save_now)");
}

void remove() {
    g.base = 0;
    g.host = nullptr;
    g.widget.store(0);
    g.set.store(0);
    g.row = 0;
    g.save.store(false);
    g.button.store(-1);
    g.prev_buttons = 0;
    g.save_activations = 0;
    g.logged_fill = false;
    g.logged_full = false;
    g.tries = 0;
    g.watch = 0;
    g.writing = false;
}

// Main thread, once per frame: the shortcut in play or the prompt's button on any pad asks for the same save.
void frame(bool in_game, double, play::GameFrame*) {
    if (!g.base) return;
    const bool up = pause_shown();
    const uint64_t activations = g.host ? g.host->action_activations(play::Act::Save) : 0;
    const bool shortcut = in_game && !up && activations != g.save_activations;
    g.save_activations = activations;
    const uint64_t buttons = hid::sampled_buttons();
    const int b = g.button.load(std::memory_order_acquire);
    const uint64_t mask = b >= 0 ? 1ull << b : 0;
    const bool prompt = up && mask && (buttons & mask) && !(g.prev_buttons & mask);
    if (shortcut || prompt) {
        g.save.store(true, std::memory_order_release);
        NXLOG_I(kTag, "save now: %s", shortcut ? "quick-save key" : "button on the pause menu's Save prompt");
    }
    g.prev_buttons = up ? buttons : ~uint64_t{0};   // a button held from before the menu opened does not count
}

// The game thread, once a frame: ask the game's autosave to save now and say when it has written the save.
void game_frame(::Ctx* c) {
    if (!g.base) return;
    const uint64_t game = rd<uint64_t>(g.base + layout::kGameState);
    if (!guest::ok(game, layout::kStateAutosaveFlags + 4)) return;
    const uint32_t step = rd<uint32_t>(game + layout::kStateAutosave);
    const uint32_t flags = rd<uint32_t>(game + layout::kStateAutosaveFlags);
    if (g.watch > 0) {
        g.writing |= step == 2;
        if (step == 0 && !(flags & 4)) {   // the asked-for save has run
            g.watch = 0;
            if (g.writing) {
                notices::post("pkp.save_now", "Saved", 2.0);
                NXLOG_I(kTag, "save now: the game's autosave has written the save");
            } else {
                notices::post("pkp.save_now", "Not saved", 3.0);
                NXLOG_W(kTag, "save now: the game's autosave stopped before writing (the game allows no save here)");
            }
        } else if (--g.watch == 0) {
            notices::post("pkp.save_now", "The game saves as soon as it can", 3.0);
            NXLOG_W(kTag, "save now: the game's autosave has not started after a minute; it saves when the game allows it");
        }
    }
    if (!g.save.load(std::memory_order_acquire)) return;
    if (step != 0 || !(flags & 1)) {   // the autosave is working, or the game has it off (title, loading)
        if (++g.tries > 600) {
            g.save.store(false, std::memory_order_release);
            g.tries = 0;
            notices::post("pkp.save_now", "Can't save right now", 3.0);
            NXLOG_W(kTag, "save now: the game's autosave stayed busy or off, the save was not asked for");
        }
        return;
    }
    g.save.store(false, std::memory_order_release);
    g.tries = 0;
    guest::call(c, g.base + layout::kAutosaveNow, game + layout::kStateAutosaveObj, 1, 0);
    if (!(rd<uint32_t>(game + layout::kStateAutosaveFlags) & 4)) {
        NXLOG_W(kTag, "save now: the game's autosave did not take the request");
        return;
    }
    g.watch = 3600;   // game frames: a minute at 60
    g.writing = false;
    notices::post("pkp.save_now", "Saving...", 30.0);
    NXLOG_I(kTag, "save now: the game's autosave was asked to save now, as its Save and Quit does");
}

}  // namespace

const Feature kSaveNowFeature = {.name = "save_now", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
