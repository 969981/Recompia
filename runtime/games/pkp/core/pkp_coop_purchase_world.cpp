// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: what the shared shop purchases (pkp_coop_purchases.cpp) need to know about the world for a purchase the game does not put
// in the bag but places in the world next to the buyer (the place class: the five rebuilding kits, shop entries 525, 734, 1029, 1219,
// 1828 and the relocation rows of kit 326).
//   kIsPlaced(item)  0x787b80: true for a category 2 item whose object belongs to a building definition of kind 3; the grant then calls
//                    0xe88ec8 -> 0x78b740, which places the object next to the LOCAL player (no stage test) on the process that holds
//                    the world, and does nothing on a Link Play visitor. 0x78b740 answers true whatever the block write then did.
//   The delivery proof: placing a kit registers a cabinet record (save root + kCabinetRecords: 0x610 records of 0x10 bytes, the
//   occupied flag in byte +0xe). A kit is "there" when a record that was free before the grant is occupied after it. Only the 164 kit
//   (type 3, from a player's log) is known to register one; the other four are assumed to.
//   The shop row (LINKDATA #70, registry slot +0x230, 0x24 byte rows): u8 +0x22 is the area the row belongs to, which is the stage the
//   kit's building stands in.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_tables.h"

namespace nxrt::games::pkp::coop::purchases::world {

namespace {

constexpr uint64_t kIsPlaced = 0x787b80;        // (item) -> 1: a world-placed purchase
constexpr uint8_t kCategoryBuilding = 2;        // item record category of the kits (the first test of kIsPlaced)
constexpr uint64_t kShopTable = 0x230;          // the data-table manager's slot of the shop rows
constexpr uint64_t kShopRowArea = 0x22;         // shop row: u8 the area (stage) of the row

}  // namespace

bool is_place_class(::Ctx* c, uint32_t item, uint8_t category) {
    return item && category == kCategoryBuilding && (guest::call(c, base() + kIsPlaced, item) & 1);
}

// The shop row's area, or 0 when the row cannot be read (the caller then does not place).
uint8_t shop_area(::Ctx* c, uint16_t entry) {
    const uint64_t row = tables::row(c, base(), kShopTable, entry);
    return row && guest::ok(row + kShopRowArea, 1) ? guest::rd<uint8_t>(row + kShopRowArea) : 0;
}

bool in_stage(uint8_t area) { return area && session::area_stage() == area && local_player(); }

Snapshot snapshot() {
    Snapshot s;
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t recs = r + layout::kCabinetRecords;
    if (!r || !guest::ok(recs, layout::kCabinetRecordCount * layout::kCabinetRecordSize)) return s;
    s.valid = true;
    for (uint32_t i = 0; i < layout::kCabinetRecordCount; ++i)
        s.occupied[i] = guest::rd<uint8_t>(recs + i * layout::kCabinetRecordSize + layout::kCabinetFlags) & layout::kCabinetOccupied;
    return s;
}

bool added_since(const Snapshot& before) {
    if (!before.valid) return false;
    const Snapshot now = snapshot();
    return now.valid && (now.occupied & ~before.occupied).any();
}

}  // namespace nxrt::games::pkp::coop::purchases::world
