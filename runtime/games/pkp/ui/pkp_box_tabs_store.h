// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs (pkp_box_tabs.cpp): (+) Store All / Retrieve All over every tab of the box. The game's bulk transfer moves items
// between the bag and the one container the screen routes to (screen +0x148 type / +0x154 id), live, with no temp copies, and refreshes
// the panels when done; it runs after the player confirmed the dialog. The sweep runs that same transfer once per tab, the shown tab
// first and then the next ones in L / R order, with the screen and panel routed to each tab for its pass, puts the shown id back and
// leaves one result for the game's own message ("It's full", "Some items could not be stored", ...).
// Storing only what the box already holds takes two rounds: each tab first takes what it holds itself; what is left then goes into any
// tab with room as long as the box holds the item in some tab (the game's check looks at one container, see hook_count_matching).
#pragma once
#include <cstdint>

#include "nxrt/abi.h"
#include "ui/pkp_box_tabs_storage.h"

namespace nxrt::games::pkp::box_store {

struct Sweep {
    uint64_t screen = 0, panel = 0;
    const box_tabs::BoxStorage* storage = nullptr;
    const box_tabs::Tabs* tabs = nullptr;
    int shown = 0;                   // index of the shown tab
    uint8_t source = 0, target = 0;  // the transfer's panel kinds
    bool store = true;               // into the box (false: out of it)
    bool matching = false;           // the dialog's first choice: only items the destination already holds
    bool log = false;
};

// Runs the transfer over every tab, restores the shown id and refreshes. Returns the transfer's own result bit (something moved) and
// leaves the combined status where the game reads it.
bool sweep(::Ctx* c, uint64_t base, const Sweep& s);

// The sweep is calling the transfer: the hook lets the game's own body run.
bool running();

// Entry hook of the count behind "already stored here" (main 0xe70fb4): in the second matching round it counts over all tabs.
int hook_count_matching(::Ctx* c, uint64_t, void*);

}  // namespace nxrt::games::pkp::box_store
