// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs (pkp_box_tabs.cpp): the rest of one stack from the bag into the box's other tabs, after the game's own move filled
// the shown one (Y Add Stack, ZL half, the amount picker).
#pragma once
#include <cstdint>

#include "nxrt/abi.h"
#include "ui/pkp_box_tabs_storage.h"

namespace nxrt::games::pkp::box_move {

struct Stack {
    uint8_t player = 0, kind = 0;   // the bag panel's kind
    uint16_t slot = 0;              // its slot there
    uint64_t item = 0;              // the live slot record
    uint32_t packed = 0;            // bits 0..30
    uint8_t type = 0;               // the box
    uint32_t max_stack = 0;
};

// The units of the stack still in its bag slot.
uint32_t left(const Stack& s);

// Moves up to `units` of the stack into the tabs after `shown` (in L / R order, round the end). Returns how many went.
uint32_t spread(::Ctx* c, uint64_t base, const Stack& s, uint32_t units, const box_tabs::BoxStorage& storage, const box_tabs::Tabs& tabs,
                int shown);

}  // namespace nxrt::games::pkp::box_move
