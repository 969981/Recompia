// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs (pkp_box_tabs.cpp): (+) Store All over every tab of the box. The game's bulk transfer moves bag items into the one
// container the screen routes to (screen +0x148 type / +0x154 id), live, with no temp copies, and refreshes the panels when done; it runs
// after the player confirmed the dialog. The sweep runs that same transfer once per tab, the shown tab first and then the next ones in
// L / R order, with the screen and panel routed to each tab for its pass, stops as soon as nothing is left to store, puts the shown
// id back and leaves one result for the game's own message ("It's full", "Some items could not be stored", ...).
#pragma once
#include <cstdint>

#include "nxrt/abi.h"

namespace nxrt::games::pkp::box_store {

struct Sweep {
    uint64_t screen = 0, panel = 0;
    const uint16_t* ids = nullptr;   // the box's tabs in L / R order
    int count = 0;
    int shown = 0;                   // index of the shown tab in ids
    uint8_t source = 0, target = 0;  // the transfer's bag kind and container kind
    bool matching = false;           // the dialog's first choice: only items the container already holds
    bool log = false;
};

// Runs the transfer over every tab, restores the shown id and refreshes. Returns the transfer's own result bit (something moved) and
// leaves the combined status where the game reads it.
bool sweep(::Ctx* c, uint64_t base, const Sweep& s);

// The sweep is calling the transfer: the hook lets the game's own body run.
bool running();

}  // namespace nxrt::games::pkp::box_store
