// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs: the game's sort over the whole box (pkp_box_tabs_sort.h). Main 2.0.1a.
#include "ui/pkp_box_tabs_sort.h"
#include "core/pkp_guest.h"

#include <array>
#include <cstring>
#include <map>

#include "nxrt/log.h"

namespace nxrt::games::pkp::box_sort {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kMerge = 0x5b3420;   // (-, Slot* first, count): tops equal stacks up from later ones
constexpr uint64_t kSort = 0x5b25e0;    // (1 container, Slot* base, first, count <= 600): the game's order, empty slots last
constexpr uint32_t kMaxSlots = 600;

using guest::call;
using guest::rd;
using guest::wr;

// How many of each item (packed bits 0..30) a run of slots holds.
std::map<uint32_t, uint32_t> totals(const uint8_t* slots, uint32_t count) {
    std::map<uint32_t, uint32_t> t;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t p;
        std::memcpy(&p, slots + i * items::kSlotSize + items::kSlotPayload, sizeof(p));
        if (items::id_of(p)) t[static_cast<uint32_t>(p) & 0x7fffffff] += items::count_of(p);
    }
    return t;
}

}  // namespace

bool sort(::Ctx* c, uint64_t base, const box_tabs::BoxStorage& storage, const box_tabs::Tabs& tabs) {
    const uint32_t n = static_cast<uint32_t>(tabs.count) * storage.slots;
    if (!n || n > kMaxSlots) return false;
    alignas(16) static std::array<uint8_t, kMaxSlots * items::kSlotSize> run;   // game thread; guest addresses are host addresses
    const auto slot = [&](uint32_t i) { return storage.payload(tabs.ids[i / storage.slots], i % storage.slots); };
    for (uint32_t i = 0; i < n; ++i)
        std::memcpy(run.data() + i * items::kSlotSize, reinterpret_cast<const void*>(slot(i) - items::kSlotPayload), items::kSlotSize);
    const auto before = totals(run.data(), n);
    const uint64_t at = reinterpret_cast<uint64_t>(run.data());
    call(c, base + kMerge, 1, at, n);
    call(c, base + kSort, 1, at, 0, n);
    // The sort clears an item it does not know and clamps a stack over the maximum: then every slot stays as it is.
    if (totals(run.data(), n) != before) {
        NXLOG_W(kTag, "storage box tabs: not sorted over the tabs: the game's sort would not keep every item");
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t p;
        std::memcpy(&p, run.data() + i * items::kSlotSize + items::kSlotPayload, sizeof(p));
        wr<uint64_t>(slot(i), items::moved_into(rd<uint64_t>(slot(i)), p));
    }
    return true;
}

}  // namespace nxrt::games::pkp::box_sort
