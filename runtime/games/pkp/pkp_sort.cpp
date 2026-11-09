// SPDX-License-Identifier: GPL-3.0-or-later
// Inventory sort (#323): in the bag screen (X) and the container screens, Y (keyboard F through the default btn_y binding) sorts the
// items of the panel under the cursor by the item table's kind byte, then by item id; empty slots go last. [pkp] inventory_sort
// (on by default: the button only acts when pressed), [pkp] inventory_autosort (off by default: sorts the bag once each time the
// screen opens). Both do nothing during Link Play.
//
// How the game keeps the bag (main 2.0.1a, measured with page traps and snapshot diffs in scripted runs):
//  * A slot is 16 bytes: a vtable qword (main+0x322c1d8), a packed u32 (bits 0..12 item id, 0 = empty; 13..20, 21..25, 26..30 and
//    bit 31 per-slot data) and a u8 count at +0xc. Only the 8 bytes from +8 move; every slot keeps its vtable.
//  * Inventory kind 0 is the player's bag: 120 slots, then kind 4 (10 slots at bag + 0x780, the separate sub-area) that is never
//    touched. Offline the bag is save common + 0x830 (m_useItemInventoryOffline; save common = [main+0x3561930] + 0x13d4920). The
//    other kinds are containers: count slots from slot `first` of a store (save common + 0x1050, or the stage's store when the
//    container type's table entry main+0x3484c48 + type * 12 is nonzero); 0x7100e6dc2c (player, kind, type, id, use_temp) returns
//    the bag or the store, 0x71002ee4bc (type, id, &first, &count) the container's first slot and slot count.
//  * The screen works on a temp copy only while an item is held (per-player flag main+0x36bbfd8; copies at 0x3684340 / 0x36853b0)
//    and copies it back on placement / close (0x7100ff3be8 -> 0x7100e707c0). Sorting is done only while nothing is held (screen
//    state 1, flag 0), so it writes the live array and no copy is pending.
//  * The bag / container screen is UI class 0x8d (vtable main+0x333ba60, the object holds +0x10; size 0x1a8). Its per-frame virtual 0x7100feee78 (x0 =
//    the screen) is the entry hook (needs `entryhook 0x7100feee78` in recomp/games/pkp/main.sites). Fields: +0x30 player helper
//    (0x7100a6fd58 -> player index), +0x148 container type (u8), +0x154 container id (u16), +0x160 state (1 browsing, 3 holding),
//    +0x168 the screen's container kind (0 = bag only), +0x169 the kind of the panel under the cursor. 0x7100ff12e8 (screen) makes
//    the container and bag panels re-read their arrays (the game calls it after its own commit), so the sorted order shows at once.
//  * In browsing state Y has no action in either screen (X = pick up / hold to rearrange, A = take out, B = back, - = info,
//    + = drop), so it is free for the sort.
//  * m_currentUseItemSlot (save common + 0xc070, u16) holds a bag slot index: after a bag sort it is moved to the new slot of the
//    same item.
//  * Link Play: online the bag is m_useItemInventoryOnline and container moves go through the game's network requests
//    (0x7100e6e4fc), so nothing is sorted while 0x71000376c4 (online) is true.
#include "pkp_sort.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kScreenFrame = 0xfeee78;      // bag / container screen virtual +0x48 (per frame), x0 = the screen
constexpr uint64_t kScreenVtable = 0x333ba70;     // the vtable pointer the object holds (vtable 0x333ba60 + 0x10)
constexpr uint64_t kRefresh = 0xff12e8;          // (screen): panels re-read their arrays
constexpr uint64_t kPlayerOf = 0xa6fd58;         // (screen + 0x30) -> player index
constexpr uint64_t kInventory = 0xe6dc2c;        // (player, kind, type, id, use_temp) -> slot array
constexpr uint64_t kContainerSlots = 0x2ee4bc;   // (type, id, u32* first, u32* count) -> valid
constexpr uint64_t kOnline = 0x376c4;            // () -> bit 0: Link Play session
constexpr uint64_t kTempFlag = 0x36bbfd8;        // u8 per player: the screen works on a temp copy (an item is held)
constexpr uint64_t kSaveRoot = 0x3561930, kCommon = 0x13d4920, kBagOffline = 0x830, kUseSlot = 0xc070;
constexpr uint64_t kTables = 0x35618f0;          // data-table manager; item table at +0x10 + 0x638 (+0 while it holds < 200 tables)
constexpr uint64_t kTableCount = 0x2b70, kItemTable = 0x638, kItemKind = 0x3a;
constexpr uint64_t kPlayerHelper = 0x30, kType = 0x148, kId = 0x154, kState = 0x160, kScreenKind = 0x168, kCursorKind = 0x169;
constexpr uint32_t kBagSlots = 120, kMaxSlots = 0x800, kStoreSlots = 0x77a9;   // the stage store is the larger one
constexpr uint32_t kIdMask = 0x1fff;
constexpr uint64_t kSortButton = hid::btn(hid::kBtnY);
constexpr auto kReopenGap = std::chrono::milliseconds(300);   // no screen frame for this long = the screen was closed

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }

struct State {
    uint64_t base = 0;
    bool button = true, autosort = false, log_lists = false;
    uint64_t screen = 0, prev_buttons = 0;
    std::chrono::steady_clock::time_point last_frame{};
    bool auto_pending = false;
    std::array<uint8_t, kIdMask + 1> kind_of{};   // item kind + 1, 0 = not looked up yet
    uint64_t sorts = 0;
};
State g;

uint64_t call(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t sp = 0) {
    const ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    c->x[4] = a4;
    if (sp) c->x[31] = sp;
    dispatch_call(c, g.base + fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}
uint64_t call_ptr(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0) {
    const ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

// The item table's kind byte (record +0x3a), 0xff when the table cannot be read. Looked up once per id.
uint8_t item_kind(::Ctx* c, uint32_t id) {
    id &= kIdMask;
    if (g.kind_of[id]) return static_cast<uint8_t>(g.kind_of[id] - 1);
    uint8_t kind = 0xff;
    const uint64_t mgr = rd<uint64_t>(g.base + kTables);
    if (mgr) {
        const uint64_t off = rd<uint32_t>(mgr + kTableCount) < 200 ? 0 : kItemTable;
        const uint64_t t = rd<uint64_t>(mgr + off + 0x10);
        const uint64_t rows = t ? call_ptr(c, rd<uint64_t>(rd<uint64_t>(t) + 0x38), t, 0) : 0;
        const uint64_t entry = rows ? call_ptr(c, rd<uint64_t>(rd<uint64_t>(rows) + 0x30), rows, id) : 0;
        const uint64_t rec = entry ? rd<uint64_t>(entry + 8) : 0;
        if (rec && mem::is_mapped(rec + kItemKind, 1)) kind = rd<uint8_t>(rec + kItemKind);
    }
    if (kind != 0xff) g.kind_of[id] = static_cast<uint8_t>(kind + 1);
    return kind;
}

struct Slot {
    uint64_t payload;   // packed u32 + count byte + 3 bytes, as stored at slot + 8
    uint32_t key;       // kind << 16 | id (empty slots are not keyed)
    uint32_t from;
};

void log_list(const char* what, uint64_t arr, uint32_t n) {
    char line[1024];
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 16 < sizeof line; ++i) {
        const uint32_t v = rd<uint32_t>(arr + i * 16 + 8);
        if (!(v & kIdMask)) continue;
        k += static_cast<size_t>(std::snprintf(line + k, sizeof line - k, " %u:%x*%u", i, v & kIdMask, rd<uint8_t>(arr + i * 16 + 0xc)));
    }
    line[k] = 0;
    NXLOG_I(kTag, "inventory sort: %s%s", what, line);
}

// Sorts the n slots at arr. Returns false when nothing could be read.
bool sort_slots(::Ctx* c, uint64_t arr, uint32_t n, bool bag) {
    if (!arr || !n || n > kMaxSlots || !mem::is_mapped(arr, uint64_t{n} * 16)) return false;
    const uint64_t vt = rd<uint64_t>(arr);
    std::vector<Slot> items, empty;
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (rd<uint64_t>(arr + i * 16) != vt) return false;   // not one array of slots
        const uint64_t p = rd<uint64_t>(arr + i * 16 + 8);
        const uint32_t id = static_cast<uint32_t>(p) & kIdMask;
        if (!id) { empty.push_back({p, 0, i}); continue; }
        items.push_back({p, static_cast<uint32_t>(item_kind(c, id)) << 16 | id, i});
        total += (p >> 32) & 0xff;
    }
    if (g.log_lists) log_list("before", arr, n);
    std::stable_sort(items.begin(), items.end(), [](const Slot& a, const Slot& b) { return a.key < b.key; });
    std::vector<uint32_t> moved_to(n, ~0u);
    uint32_t at = 0;
    for (const Slot& s : items) { moved_to[s.from] = at; std::memcpy(reinterpret_cast<void*>(arr + at++ * 16 + 8), &s.payload, 8); }
    for (const Slot& s : empty) std::memcpy(reinterpret_cast<void*>(arr + at++ * 16 + 8), &s.payload, 8);
    if (bag) {   // the in-use slot index follows its item
        const uint64_t save = rd<uint64_t>(g.base + kSaveRoot);
        if (save && arr == save + kCommon + kBagOffline) {
            const uint64_t a = save + kCommon + kUseSlot;
            const uint16_t old = rd<uint16_t>(a);
            if (old < n && moved_to[old] != ~0u) {
                const uint16_t now = static_cast<uint16_t>(moved_to[old]);
                std::memcpy(reinterpret_cast<void*>(a), &now, 2);
            }
        }
    }
    uint64_t after = 0;
    for (uint32_t i = 0; i < n; ++i) after += rd<uint8_t>(arr + i * 16 + 0xc) * ((rd<uint32_t>(arr + i * 16 + 8) & kIdMask) != 0);
    if (g.log_lists) log_list("after ", arr, n);
    if (++g.sorts <= 64)
        NXLOG_I(kTag, "inventory sort: %s, %u slots, %zu stacks, %llu items (%llu after)", bag ? "bag" : "container", n, items.size(),
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(after));
    return true;
}

// Sorts the array of kind `kind` shown by the screen and refreshes its panels.
void sort_screen(::Ctx* c, uint64_t screen, uint8_t player, uint8_t kind) {
    if (kind == 4 || kind == 9) return;   // the bag's sub-area and the special kind 9 are left alone
    const uint8_t type = rd<uint8_t>(screen + kType);
    const uint16_t id = rd<uint16_t>(screen + kId);
    uint64_t arr = call(c, kInventory, player, kind, type, id, 0);
    uint32_t n = kBagSlots;
    if (kind != 0) {   // containers: the call returns the whole store; the container is count slots from its first one
        const uint64_t buf = (c->x[31] - 0x80) & ~uint64_t{0xf};
        std::memset(reinterpret_cast<void*>(buf), 0, 16);
        if (!(call(c, kContainerSlots, type, id, buf, buf + 4, 0, buf - 0x40) & 1)) return;
        const uint32_t first = rd<uint32_t>(buf);
        n = rd<uint32_t>(buf + 4);
        if (!arr || first + uint64_t{n} > kStoreSlots) return;
        arr += uint64_t{first} * 16;
    }
    if (sort_slots(c, arr, n, kind == 0)) call(c, kRefresh, screen);
}

int hook_screen_frame(::Ctx* c, uint64_t, void*) {
    const uint64_t screen = c->x[0];
    if (!screen || !mem::is_mapped(screen, 0x1a8) || rd<uint64_t>(screen) != g.base + kScreenVtable) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (screen != g.screen || now - g.last_frame > kReopenGap) {   // the screen (re)opened
        g.screen = screen;
        g.auto_pending = g.autosort;
        g.prev_buttons = kSortButton;   // a Y still held from before the screen opened does not count
        if (g.log_lists)
            NXLOG_I(kTag, "inventory sort: screen %llx open, state %u, kinds %u/%u, type %u id %u", static_cast<unsigned long long>(screen),
                    rd<uint8_t>(screen + kState), rd<uint8_t>(screen + kScreenKind), rd<uint8_t>(screen + kCursorKind),
                    rd<uint8_t>(screen + kType), rd<uint16_t>(screen + kId));
    }
    g.last_frame = now;

    uint64_t buttons = 0;
    if (g.button) {
        if (auto src = hid::input_source()) {
            hid::InputSnapshot snap;
            src->poll(snap);
            buttons = (snap.npad[0].buttons | snap.npad[hid::kNpadSlotHandheld].buttons) & kSortButton;
        }
    }
    const bool pressed = buttons && !g.prev_buttons;
    g.prev_buttons = buttons;
    if (!pressed && !g.auto_pending) return 0;

    const uint8_t state = rd<uint8_t>(screen + kState);
    const uint8_t player = static_cast<uint8_t>(call(c, kPlayerOf, screen + kPlayerHelper));
    const bool online = call(c, kOnline, 0) & 1;
    if (pressed && g.log_lists)
        NXLOG_I(kTag, "inventory sort: Y, state %u, player %u, temp %u, online %u, cursor kind %u", state, player,
                player < 2 ? rd<uint8_t>(g.base + kTempFlag + player) : 0xff, online, rd<uint8_t>(screen + kCursorKind));
    if (state != 1) return 0;   // browsing only, never while an item is held
    if (player > 1 || rd<uint8_t>(g.base + kTempFlag + player)) return 0;
    if (online) { g.auto_pending = false; return 0; }   // Link Play: off
    if (g.auto_pending) {
        g.auto_pending = false;
        sort_screen(c, screen, player, 0);
    }
    if (pressed) {
        const uint8_t kind = rd<uint8_t>(screen + kCursorKind);
        if (kind == 0 || kind == rd<uint8_t>(screen + kScreenKind)) sort_screen(c, screen, player, kind);
    }
    return 0;
}

}  // namespace

void sort_install(uint64_t base) {
    g.base = base;
    g.button = config_get_bool("pkp", "inventory_sort", true);
    g.autosort = config_get_bool("pkp", "inventory_autosort", false);
    g.log_lists = config_get_int("pkp", "menu_log", 0) > 0;
    if (!g.button && !g.autosort) return;
    hook_add_entry(base + kScreenFrame, &hook_screen_frame);
    NXLOG_I(kTag, "inventory sort: button %s, autosort %s (hook main+0x%llx)", g.button ? "on" : "off", g.autosort ? "on" : "off",
            static_cast<unsigned long long>(kScreenFrame));
}

}  // namespace nxrt::games::pkp
