// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs (pkp_box_tabs.cpp): the save's slots behind a box and its tabs. Every tab is a container ID of the box's type, and
// every ID owns one slice of `slots` item slots in the stage store.
#pragma once
#include <array>
#include <cstdint>

#include "core/pkp_items.h"
#include "core/pkp_layout.h"

namespace nxrt::games::pkp::box_tabs {

constexpr int kMaxPages = 12, kMaxBigTabs = 6;   // fixed for good: they set where every tab's items live (available_tabs)

struct Tabs {
    std::array<uint16_t, kMaxPages> ids{};   // the box itself first, then in L / R order
    int count = 1;
    int taken = 0;   // requested units whose ID is unavailable

    int index_of(uint16_t id) const {
        for (int i = 0; i < count; ++i)
            if (ids[i] == id) return i;
        return -1;
    }
};

struct BoxStorage {
    uint64_t records = 0, store = 0;
    uint32_t first_record = 0, count = 0, slots = 0;

    uint64_t record(uint32_t id) const {
        const uint32_t index = first_record + id;
        // Native indices outside the cabinet pool use record zero, but the item slice uses the logical ID.
        return records + (index < layout::kCabinetRecordCount ? index : 0) * layout::kCabinetRecordSize;
    }

    // The address of a slot's payload (packed word, count).
    uint64_t payload(uint32_t id, uint32_t slot) const {
        return store + (uint64_t{id} * slots + slot) * items::kSlotSize + items::kSlotPayload;
    }

    bool slice_has_items(uint32_t id) const {
        for (uint32_t slot = 0; slot < slots; ++slot)
            if (items::id_of(guest::rd<uint32_t>(payload(id, slot)))) return true;
        return false;
    }

    // How many of an item (packed word) one ID still takes: all of it, and on top of stacks it already holds.
    struct Room { uint32_t total = 0, merge = 0; };
    Room room(uint32_t id, uint32_t packed, uint32_t max_stack) const;

    // How many of an item id (any variant) the tabs hold together.
    uint32_t count_of(const Tabs& tabs, uint32_t item_id) const;
};

// The slots of box type `type`; count 0 with no readable save.
BoxStorage box_storage(uint64_t base, uint8_t type);

}  // namespace nxrt::games::pkp::box_tabs
