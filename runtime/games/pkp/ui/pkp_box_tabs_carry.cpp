// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs: carrying a held item to another tab (pkp_box_tabs_carry.h). Main 2.0.1a; the calls are the game's own staged item
// operations and its write-back and snapshot, with the screen's player (placing and cancelling after a carry checked in a game).
#include "ui/pkp_box_tabs_carry.h"
#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "nxrt/log.h"

namespace nxrt::games::pkp::box_carry {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kPlayerOf = 0xa6fd58;     // (screen + 0x30) -> player index
constexpr uint64_t kSnapshot = 0xff359c;     // (screen) -> bit 0: the temp copies taken by the screen's type / id, flag and +0x1a0 bit 5 set
constexpr uint64_t kWriteBack = 0xff3be8;    // (screen, 1 write back / 0 drop) -> bit 0; clears the flag and +0x1a0 bit 5
constexpr uint64_t kEndHold = 0xff3708;      // (screen, 0): hides the held item, clears it, writes back (a no-op with no copies), state 1
constexpr uint64_t kStagedSet = 0xe6df5c;    // (player, item*, kind, slot, type, id): puts the item into a slot of a temp copy
constexpr uint64_t kStagedTake = 0xe6e828;   // (player, item*, kind, slot, type, id): takes the item's count out of a slot of a temp copy
constexpr uint64_t kBagCapacity = 0x2e8dc4;  // () -> u8: the bag slots unlocked
constexpr uint64_t kPlayerHelper = 0x30, kType = 0x148, kId = 0x154, kState = 0x160, kFlags = 0x1a0;
constexpr uint32_t kOwnsCopies = 1u << 5;    // +0x1a0: the screen took the temp copies
constexpr uint8_t kHolding = 3, kHoldingPart = 4;
constexpr uint8_t kBagKind = 0;
constexpr uint32_t kMaxItemId = 0x11f7;

using guest::call;
using guest::call_n;
using guest::ok;
using guest::rd;
using guest::wr;

uint8_t player_of(::Ctx* c, uint64_t base, uint64_t screen) {
    return static_cast<uint8_t>(call(c, base + kPlayerOf, screen + kPlayerHelper));
}

uint64_t held(uint64_t base, uint8_t player) { return base + layout::kHeldItem + player * layout::kHeldItemSize; }
uint64_t staged_bag(uint64_t base, uint8_t player) { return base + layout::kStagedBag + player * layout::kStagedBagSize; }

// A slot of the bag copy: its address, or 0 when the copy has no such slot.
uint64_t bag_slot(uint64_t base, uint8_t player, uint32_t slot) {
    const uint64_t v = staged_bag(base, player);
    const uint64_t data = rd<uint64_t>(v);
    const uint64_t len = rd<uint64_t>(v + 8);
    return slot < len && ok(data + slot * items::kSlotSize, items::kSlotSize) ? data + slot * items::kSlotSize : 0;
}

// The item as a slot record below the guest stack (the game's staged operations read it there).
uint64_t item_buffer(::Ctx* c, uint64_t base, uint32_t packed, uint8_t count) {
    const uint64_t buf = (c->x[31] - 0x100) & ~uint64_t{0xf};
    std::memset(reinterpret_cast<void*>(buf), 0, 0x40);
    wr<uint64_t>(buf, base + layout::kItemSlotVtable);
    wr<uint32_t>(buf + items::kSlotPayload, packed);
    wr<uint8_t>(buf + items::kSlotPayload + 4, count);
    return buf;
}

uint64_t staged_op(::Ctx* c, uint64_t base, uint64_t fn, uint64_t screen, uint8_t player, uint64_t item, uint32_t slot) {
    const std::array<uint64_t, 6> args = {player, item, kBagKind, slot, rd<uint8_t>(screen + kType), rd<uint16_t>(screen + kId)};
    return call_n<6>(c, base + fn, args, item - 0x10);
}

}  // namespace

bool holding(::Ctx* c, uint64_t base, uint64_t screen) {
    const uint8_t state = rd<uint8_t>(screen + kState);
    if (state != kHolding && state != kHoldingPart) return false;
    const uint8_t player = player_of(c, base, screen);
    return player < 2 && rd<uint8_t>(base + layout::kItemTempFlag + player) && (rd<uint32_t>(screen + kFlags) & kOwnsCopies);
}

Parked park(::Ctx* c, uint64_t base, uint64_t screen) {
    Parked p;
    if (!holding(c, base, screen)) return p;
    p.player = player_of(c, base, screen);
    const uint64_t h = held(base, p.player);
    const uint16_t count = rd<uint16_t>(h + 0x14);
    p.packed = rd<uint32_t>(h + 8) & 0x7fffffff;
    const uint32_t id = items::id_of(p.packed);
    if (id - 1 >= kMaxItemId || count == 0 || count > 0xff) return p;
    p.count = static_cast<uint8_t>(count);
    // A free slot the player has unlocked, in the bag copy (the bag panel's own slot numbers).
    const uint32_t usable = std::min<uint32_t>(call(c, base + kBagCapacity, 0) & 0xff, layout::kBagSlots);
    bool found = false;
    for (uint32_t s = 0; s < usable && !found; ++s) {
        const uint64_t slot = bag_slot(base, p.player, s);
        if (slot && !items::id_of(rd<uint32_t>(slot + items::kSlotPayload))) {
            p.slot = static_cast<uint16_t>(s);
            found = true;
        }
    }
    if (!found) return p;
    const uint64_t slot = bag_slot(base, p.player, p.slot);
    const uint64_t item = item_buffer(c, base, p.packed, p.count);
    staged_op(c, base, kStagedSet, screen, p.player, item, p.slot);
    if (items::id_of(rd<uint32_t>(slot + items::kSlotPayload)) != id || rd<uint8_t>(slot + items::kSlotPayload + 4) != p.count) return p;
    if (!(call(c, base + kWriteBack, screen, 1) & 1)) {
        // Not written back: undo the park in the copy, so the hold goes on as before.
        if (rd<uint8_t>(base + layout::kItemTempFlag + p.player))
            staged_op(c, base, kStagedTake, screen, p.player, item_buffer(c, base, p.packed, p.count), p.slot);
        NXLOG_W(kTag, "storage box tabs: the held item stays (the temp copies were not written back)");
        return p;
    }
    p.ok = true;
    return p;
}

bool resume(::Ctx* c, uint64_t base, uint64_t screen, const Parked& p) {
    if (!(call(c, base + kSnapshot, screen) & 1)) {
        call(c, base + kEndHold, screen, 0);   // no copies: the item stays where it is parked, in the live bag
        NXLOG_W(kTag, "storage box tabs: the held item went into the bag (slot %u): the temp copies could not be taken again", p.slot);
        return false;
    }
    const uint64_t slot = bag_slot(base, p.player, p.slot);
    if (slot) staged_op(c, base, kStagedTake, screen, p.player, item_buffer(c, base, p.packed, p.count), p.slot);
    if (!slot || items::id_of(rd<uint32_t>(slot + items::kSlotPayload))) {
        call(c, base + kWriteBack, screen, 0);   // drop the copies: the live bag keeps the parked item
        call(c, base + kEndHold, screen, 0);
        NXLOG_W(kTag, "storage box tabs: the held item went into the bag (slot %u): it could not be taken out of the bag copy", p.slot);
        return false;
    }
    // The way back is the bag slot it is parked in (both the cancel target and the last cell).
    const uint64_t h = held(base, p.player);
    wr<uint16_t>(h + 0x10, p.slot);
    wr<uint16_t>(h + 0x12, p.slot);
    wr<uint8_t>(h + 0x16, kBagKind);
    wr<uint8_t>(h + 0x17, kBagKind);
    return true;
}

}  // namespace nxrt::games::pkp::box_carry
