// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs (pkp_box_tabs.cpp): an item held in a storage box screen carried to another tab. While an item is held the screen
// works on temp copies of the bag and of the shown container and writes them back by the container id shown at that moment; the item
// itself lives only in the held record, and the live data still holds it where it came from (a cancel or a close drops the copies).
// Before the tab switch the held item is parked in a free slot of the bag copy and the copies are written back, so the old tab loses
// the item and the live bag holds it; after the switch the copies are taken again (bag and new tab), the parked item is taken out of
// the bag copy only, and the held item's way back points at that bag slot. Placing it writes both copies as the game always does; a
// cancel puts it back in that bag slot; a close that drops the copies leaves it in the live bag. Nothing is lost on any of these.
#pragma once
#include <cstdint>

#include "nxrt/abi.h"

namespace nxrt::games::pkp::box_carry {

struct Parked {
    bool ok = false;
    uint8_t player = 0;
    uint16_t slot = 0;      // bag slot the item is parked in
    uint32_t packed = 0;    // the item (bits 0..30 of the packed word)
    uint8_t count = 0;
};

// The screen holds an item that can be parked: parks it in a free slot of the bag copy and writes both copies back by the screen's
// current type / id. ok false: nothing changed (no free bag slot, or the write-back failed and the park was undone).
Parked park(::Ctx* c, uint64_t base, uint64_t screen);

// After the screen's (and its panel's) container id was switched: takes the copies again, takes the parked item out of the bag copy and
// points the held item's way back at its bag slot. false: the hold was ended instead (the item stays in the live bag).
bool resume(::Ctx* c, uint64_t base, uint64_t screen, const Parked& parked);

// The screen is holding an item (an item picked up, the temp copies in use).
bool holding(::Ctx* c, uint64_t base, uint64_t screen);

}  // namespace nxrt::games::pkp::box_carry
