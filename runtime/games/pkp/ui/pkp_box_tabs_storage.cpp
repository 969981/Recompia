// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs: the slots behind a box and its tabs (pkp_box_tabs_storage.h).
#include "ui/pkp_box_tabs_storage.h"
#include "core/pkp_guest.h"

namespace nxrt::games::pkp::box_tabs {

using guest::ok;
using guest::rd;

BoxStorage box_storage(uint64_t base, uint8_t type) {
    const uint64_t root = rd<uint64_t>(base + layout::kSaveRoot);
    if (!root) return {};
    const uint64_t offset = type * sizeof(uint16_t);
    const uint32_t first_record = rd<uint16_t>(base + layout::kCabinetBases + offset);
    const uint32_t count = rd<uint16_t>(base + layout::kCabinetCounts + offset);
    const uint32_t slots = rd<uint16_t>(base + layout::kContainerSlotCounts + offset);
    const uint32_t first_slot = rd<uint16_t>(base + layout::kContainerSlotBases + offset);
    const uint64_t store = root + layout::kStageContainerStore + first_slot * items::kSlotSize;
    if (!count || !slots || !ok(store, uint64_t{count} * slots * items::kSlotSize)) return {};
    return {root + layout::kCabinetRecords, store, first_record, count, slots};
}

BoxStorage::Room BoxStorage::room(uint32_t id, uint32_t packed, uint32_t max_stack) const {
    Room r;
    for (uint32_t slot = 0; slot < slots; ++slot) {
        const uint64_t p = rd<uint64_t>(payload(id, slot));
        if (!items::id_of(p)) {
            r.total += max_stack;
        } else if (((static_cast<uint32_t>(p) ^ packed) & 0x7fffffff) == 0 && items::count_of(p) < max_stack) {
            r.merge += max_stack - items::count_of(p);
        }
    }
    r.total += r.merge;
    return r;
}

uint32_t BoxStorage::count_of(const Tabs& tabs, uint32_t item_id) const {
    uint32_t n = 0;
    for (int i = 0; i < tabs.count; ++i)
        for (uint32_t slot = 0; slot < slots; ++slot) {
            const uint64_t p = rd<uint64_t>(payload(tabs.ids[i], slot));
            if (items::id_of(p) == item_id) n += items::count_of(p);
        }
    return n;
}

}  // namespace nxrt::games::pkp::box_tabs
