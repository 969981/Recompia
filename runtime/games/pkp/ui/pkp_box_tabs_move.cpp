// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs: the rest of one stack into the other tabs (pkp_box_tabs_move.h). Main 2.0.1a; the mover is the one the game's own
// Add / Add Stack / Store All reach, on live data.
#include "ui/pkp_box_tabs_move.h"
#include "core/pkp_guest.h"

#include <algorithm>
#include <array>

namespace nxrt::games::pkp::box_move {
namespace {

// (player, Slot* with the requested count, source slot, to kind, from kind, type, id, result*, [sp] s32 page -1 = all, [sp+8] u32 page
// size 0, [sp+0x10] u8 receive flag 0) -> bit 0: something moved. Takes what was accepted out of the source slot.
constexpr uint64_t kMove = 0xe6ad0c;
constexpr uint8_t kBoxKind = 1;

using guest::call_n;
using guest::rd;
using guest::wr;

}  // namespace

uint32_t left(const Stack& s) {
    const uint64_t p = rd<uint64_t>(s.item + items::kSlotPayload);
    return items::id_of(p) && (static_cast<uint32_t>(p) & 0x7fffffff) == s.packed ? items::count_of(p) : 0;
}

uint32_t spread(::Ctx* c, uint64_t base, const Stack& s, uint32_t units, const box_tabs::BoxStorage& storage, const box_tabs::Tabs& tabs,
                int shown) {
    uint32_t moved = 0;
    for (int n = 1; n < tabs.count && moved < units; ++n) {
        const uint16_t id = tabs.ids[(shown + n) % tabs.count];
        const uint32_t have = left(s);
        const uint32_t ask = std::min({units - moved, have, storage.room(id, s.packed, s.max_stack).total});
        if (!ask) continue;
        alignas(16) uint8_t item[0x20] = {}, result[0x20] = {};   // guest addresses are host addresses
        const uint64_t at = reinterpret_cast<uint64_t>(item);
        wr<uint64_t>(at, base + layout::kItemSlotVtable);
        wr<uint32_t>(at + items::kSlotPayload, s.packed);
        wr<uint8_t>(at + items::kSlotPayload + 4, static_cast<uint8_t>(ask));
        const uint64_t sp = (c->x[31] - 0x100) & ~uint64_t{0xf};
        wr<int64_t>(sp, -1);
        wr<uint64_t>(sp + 8, 0);
        wr<uint64_t>(sp + 0x10, 0);
        const std::array<uint64_t, 8> args = {s.player, at, s.slot, kBoxKind, s.kind, s.type, id, reinterpret_cast<uint64_t>(result)};
        call_n<8>(c, base + kMove, args, sp);
        moved += have - left(s);
    }
    return moved;
}

}  // namespace nxrt::games::pkp::box_move
