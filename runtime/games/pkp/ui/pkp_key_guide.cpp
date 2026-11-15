// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: our entries in the game's key guide (pkp_key_guide.h). Offsets in main of the 2.0.1a update.
#include "ui/pkp_key_guide.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_tables.h"
#include "core/pkp_text.h"

#include <atomic>

#include "nxrt/memory.h"

namespace nxrt::games::pkp::key_guide {

namespace {

constexpr uint64_t kFillSet = 0xec5de0;          // entry: (widget, set id): the widget takes a key guide set
constexpr uint64_t kLayoutGuides = 0xec54a0;     // entry: (widget): places one part per entry
constexpr uint64_t kPartFilled = 0xec69b4;       // inline: a part has its icon and text (x19 part, x21 row, w20 text slot, [x29 + 0x38] widget)
constexpr uint64_t kSetIcon = 0xa7dbcc;          // (part + 0x30, slot, action, 0, 0): a part's button icon
constexpr uint64_t kSetTable = 0x1048;           // data table: key guide sets, up to six key guide ids (u16) per row (record at [row + 8])
constexpr uint64_t kGuideTable = 0x1050;         // data table: key guides, {u16 action, ff, 00, u8 text, 7f, ..} per row
constexpr uint64_t kWidgetState = 0x90;          // i32: 1 or 2 while shown
constexpr uint64_t kWidgetSet = 0xf0;            // u8: the set it took last
constexpr uint64_t kWidgetEntries = 0xb0, kWidgetCount = 0xb8, kEntrySize = 0x360;
constexpr uint32_t kIconSlot = 3;
constexpr int kMaxClients = 4;

using guest::ok;
using guest::rd;
using guest::wr;

struct Slot {
    Client client;
    std::atomic<uint64_t> layout{0};
};
struct State {
    uint64_t base = 0;
    Slot slots[kMaxClients];
    std::atomic<int> count{0};
};
State g;

// Below the guest stack pointer, for the calls made from a hook.
uint64_t call_sp(const ::Ctx* c) { return (c->x[31] - 0x100) & ~uint64_t{0xf}; }

int hook_fill_set(::Ctx* c, uint64_t, void*) {
    const uint64_t widget = c->x[0];
    const uint32_t set = static_cast<uint32_t>(c->x[1] & 0xff);
    const int n = g.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g.slots[i].client.set_taken) g.slots[i].client.set_taken(c, widget, set);
    return 0;
}

int hook_layout_guides(::Ctx* c, uint64_t, void*) {
    const int n = g.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g.slots[i].client.laying_out) g.slots[i].client.laying_out(c, c->x[0]);
    return 0;
}

void hook_part_filled(::Ctx* c, uint64_t, void*) {
    Part p;
    const uint64_t fp = c->x[29];
    p.widget = ok(fp + 0x38, 8) ? rd<uint64_t>(fp + 0x38) : 0;   // the layout pass's (0x7100ec54a0) x19: this part's widget
    p.part = c->x[19];
    p.slot = static_cast<uint32_t>(c->x[20]);
    p.row = c->x[21];
    const int n = g.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g.slots[i].client.part_filled) g.slots[i].client.part_filled(c, p);
}

void install(const FeatureInstall& in) {
    g.base = in.base;
    bool sets = false, layouts = false, parts = false;
    const int n = g.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        sets |= g.slots[i].client.set_taken != nullptr;
        layouts |= g.slots[i].client.laying_out != nullptr;
        parts |= g.slots[i].client.part_filled != nullptr;
    }
    if (sets) in.hooks.add_entry(in.base + kFillSet, &hook_fill_set);
    if (layouts) in.hooks.add_entry(in.base + kLayoutGuides, &hook_layout_guides);
    if (parts) in.hooks.add_inline(in.base + kPartFilled, &hook_part_filled);
}

void remove() {   // the registry has removed the hooks; the clients add themselves again at the next install
    for (Slot& s : g.slots) s.layout.store(0, std::memory_order_relaxed);
    g.count.store(0, std::memory_order_release);
}

}  // namespace

int add(const Client& c) {
    const int i = g.count.load(std::memory_order_relaxed);
    if (i >= kMaxClients) return -1;
    g.slots[i].client = c;
    g.slots[i].layout.store(0, std::memory_order_relaxed);
    g.count.store(i + 1, std::memory_order_release);
    return i;
}

void set_click_layout(int client, uint64_t layout) {
    if (client >= 0 && client < kMaxClients) g.slots[client].layout.store(layout, std::memory_order_relaxed);
}

bool click(uint64_t layout, play::Act* act, const char** note) {
    if (!layout) return false;
    const int n = g.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const Slot& s = g.slots[i];
        if (s.layout.load(std::memory_order_relaxed) != layout || !s.client.click) continue;
        play::Act a = play::Act::None;
        if (!s.client.click(&a)) continue;
        *act = a;
        *note = s.client.click_note;
        return true;
    }
    return false;
}

uint64_t guide_row(::Ctx* c, uint32_t guide) { return tables::row(c, g.base, kGuideTable, guide); }

uint64_t set_ids(::Ctx* c, uint32_t set) {
    const uint64_t srow = tables::row(c, g.base, kSetTable, set);
    const uint64_t ids = srow ? rd<uint64_t>(srow + 8) : 0;
    return ids && ok(ids, 2 * kSetSlots) ? ids : 0;
}

int32_t state(uint64_t widget) { return widget && ok(widget + kWidgetState, 4) ? rd<int32_t>(widget + kWidgetState) : -1; }

bool shown(uint64_t widget) { return state(widget) - 1u < 2u; }

bool shows(uint64_t widget, uint32_t set) {
    if (!widget || !ok(widget + kWidgetSet, 8)) return false;
    return rd<uint8_t>(widget + kWidgetSet) == set && rd<int32_t>(widget + kWidgetState) - 1u < 2u;
}

void take_set(::Ctx* c, uint64_t widget, uint32_t set) { guest::call(c, g.base + kFillSet, widget, set); }

void set_icon(::Ctx* c, const Part& p, uint16_t action) {
    guest::call(c, g.base + kSetIcon, p.part + 0x30, kIconSlot, action, 0, 0, call_sp(c));
}

void set_text(::Ctx* c, const Part& p, const char* utf8) { text::set_string(c, g.base, p.part, p.slot, utf8); }

uint64_t layout_of(::Ctx* c, const Part& p) {
    const uint64_t src = rd<uint64_t>(p.part + 0x38);
    return src ? guest::call(c, g.base + layout::kLayoutOf, src, 0, 0, 0, 0, call_sp(c)) : 0;
}

uint16_t Entries::id(uint64_t i) const { return rd<uint16_t>(data + i * kEntrySize); }
void Entries::set_id(uint64_t i, uint16_t guide) const { wr<uint16_t>(data + i * kEntrySize, guide); }
bool Entries::append(uint16_t guide) const {
    if (!widget || count >= kSetSlots) return false;
    wr<uint16_t>(data + count * kEntrySize, guide);
    wr<uint64_t>(widget + kWidgetCount, count + 1);
    return true;
}

bool entries(uint64_t widget, Entries* out) {
    if (!widget || !mem::is_mapped(widget + kWidgetEntries, 0x10)) return false;
    const uint64_t data = rd<uint64_t>(widget + kWidgetEntries), n = rd<uint64_t>(widget + kWidgetCount);
    if (!data || n == 0 || n > kSetSlots || !mem::is_mapped(data, n * kEntrySize)) return false;
    out->widget = widget;
    out->data = data;
    out->count = n;
    return true;
}

const Feature kKeyGuideFeature = {.name = "key_guide", .install = &install, .remove = &remove};

}  // namespace nxrt::games::pkp::key_guide
