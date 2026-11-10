// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: rows of the game's data tables ([main + layout::kTables], the data-table manager), looked up as the game does. One
// lookup for every hook file (the key guide, the inventory sort, the crafting search).
//
// The manager holds its tables as qwords at +0x10 + offset, and the u32 at layout::kTableCount is how many it holds: a table at `off` is
// there while that count is above off / 8. Every threshold the hook files had agrees: the item table (+0x638, slot 199) is read at +0 while
// the count is below 200 and the recipe table (+0x330, slot 0x66) below 0x67, as the game reads them, and the title's key guide lookups
// (+0x1050, slot 0x20a) needed a count above 0x20a. A table past the count is not read.
// The two calls take (this, arg): the sort, the title and the crafting search looked the key guide table up writing two, four and five
// argument registers (guest::call_n) and all three worked, so the registers past x1 are not read. Every lookup here writes five.
#pragma once
#include <cstdint>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

namespace nxrt::games::pkp::tables {

// The data-table manager, or 0 while it is not made (or its count is not readable).
inline uint64_t manager(uint64_t base) {
    const uint64_t mgr = guest::rd<uint64_t>(base + layout::kTables);
    return mgr && guest::ok(mgr + layout::kTableCount, 4) ? mgr : 0;
}

// The manager holds the table at `off`.
inline bool holds(uint64_t mgr, uint64_t off) { return guest::rd<uint32_t>(mgr + layout::kTableCount) > off / 8; }

// A row of the table at `off` (the table's vtable [0x38](0) -> rows, rows' vtable [0x30](id) -> row); 0 if the manager or the table is
// not there, or the table has no such row.
inline uint64_t row(::Ctx* c, uint64_t base, uint64_t off, uint32_t id) {
    const uint64_t mgr = manager(base);
    if (!mgr || !holds(mgr, off)) return 0;
    return guest::table_lookup<5>(c, guest::rd<uint64_t>(mgr + 0x10 + off), id);
}

// The same, but the table at +0 while the manager does not hold `off` yet (how the game reads the item and recipe tables).
inline uint64_t row_or_first(::Ctx* c, uint64_t base, uint64_t off, uint32_t id) {
    const uint64_t mgr = manager(base);
    if (!mgr) return 0;
    return guest::table_lookup<5>(c, guest::rd<uint64_t>(mgr + 0x10 + (holds(mgr, off) ? off : 0)), id);
}

}  // namespace nxrt::games::pkp::tables
