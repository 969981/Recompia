// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: our entries in the game's key guide (pkp_key_guide.cpp). The title's Quit Game (pkp_title.cpp), the bag's Hold to
// Sort click (pkp_sort.cpp) and the crafting screen's Search (pkp_craft.cpp) are clients: each says what it wants done when the widget takes a set,
// lays its entries out and fills a part, and what a click on its part does. This module owns the widget's hooks, its layout facts, the
// writes into a part (icon, text) and the routing of a click on a client's part (pkp_menu.cpp).
//
// The widget (one per player, manager [0x3483fb8], ctor 0x7100ec4958): 0x7100ec5de0 (widget, set) gives it a key guide set, a row of the
// data table +0x1048 holding up to six key guide ids (u16, 1..399). Each id is a row of the table +0x1050: {u16 action, ff, 00, u8 text,
// 7f, ..} (the action picks the button icon, the text byte an entry of the key guide text bank). Its layout pass 0x7100ec54a0 (widget)
// places one instance of the key guide part (layout 881468973) per entry (+0xb0 entries of 0x360 bytes, +0xb8 count; six part slots) and
// fills each through 0x7100ec6800, whose end (inline 0x7100ec69b4) is where a client edits the part.
#pragma once
#include <cstdint>

#include "nxrt/abi.h"
#include "core/pkp_feature.h"
#include "play_game.h"

namespace nxrt::games::pkp::key_guide {

constexpr int kSetSlots = 6;   // key guide ids per set, entries the widget lays out

// A part the widget has just filled (x19 the part, w20 its text slot, x21 its key guide's table row; the widget is the layout pass's x19,
// saved at [x29 + 0x38]).
struct Part {
    uint64_t widget = 0;   // 0 when the frame cannot be read
    uint64_t part = 0;
    uint32_t slot = 0;
    uint64_t row = 0;
};

// What one feature shows in the key guide. The hooks run on the guest thread that drives the widget; a callback observes or edits the
// widget's data and never ends the game's function. Callbacks run in the order the clients were added (the registry's order).
struct Client {
    const char* name;
    void (*set_taken)(::Ctx* c, uint64_t widget, uint32_t set) = nullptr;   // entry 0x7100ec5de0
    void (*laying_out)(::Ctx* c, uint64_t widget) = nullptr;               // entry 0x7100ec54a0, before the parts are placed
    void (*part_filled)(::Ctx* c, const Part& p) = nullptr;                // a part has its icon and text
    // Main thread: a left click on the client's part (its layout as last given to set_click_layout). True when the click is taken;
    // *act is then the button it stands for (play::Act::None: the client acts itself).
    bool (*click)(play::Act* act) = nullptr;
    const char* click_note = "";   // the log's "menu: click on <note>"
};

// A client, at its feature's install (before the key guide feature's install in the registry). Returns its id.
int add(const Client& c);
// The layout of the client's part, for the menu pointer (any thread; 0 = none).
void set_click_layout(int client, uint64_t layout);
// Main thread (pkp_menu.cpp): a left click on a pane of `layout`. True when a client took it: *act as Client::click, *note its click_note.
bool click(uint64_t layout, play::Act* act, const char** note);

// ---- for the callbacks (guest thread, on the hooked call's context) ----
uint64_t guide_row(::Ctx* c, uint32_t guide);    // a key guide's table row (Part::row is one), 0 if none
uint64_t set_ids(::Ctx* c, uint32_t set);        // where a set's kSetSlots u16 key guide ids are, 0 if not readable
int32_t state(uint64_t widget);                  // the widget's state (1 or 2 while on screen), -1 if not readable
bool shown(uint64_t widget);                     // the widget is on screen (state 1 or 2)
bool shows(uint64_t widget, uint32_t set);       // ... and its set is `set`
void take_set(::Ctx* c, uint64_t widget, uint32_t set);   // the widget takes the set again (0x7100ec5de0; the hooks run for it)
void set_icon(::Ctx* c, const Part& p, uint16_t action);  // the part's button icon: the button of that action
void set_text(::Ctx* c, const Part& p, const char* utf8); // the part's text
uint64_t layout_of(::Ctx* c, const Part& p);              // the part's layout (what the menu pointer sees)

// The entries the widget lays out (the set's ids as it took them; a client may change an id before the layout pass places them).
struct Entries {
    uint64_t widget = 0, data = 0, count = 0;
    uint16_t id(uint64_t i) const;
    void set_id(uint64_t i, uint16_t guide) const;
    bool append(uint16_t guide) const;   // one more part at the end; false when all six are used
};
bool entries(uint64_t widget, Entries* out);   // false when they cannot be read

// A feature of the registry (pkp_game.cpp), after its clients: registers the hooks its clients need.
extern const Feature kKeyGuideFeature;

}  // namespace nxrt::games::pkp::key_guide
