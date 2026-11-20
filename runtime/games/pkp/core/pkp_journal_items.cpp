// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's items watcher (pkp_journal_watch.h): the bags (j.item), items lying in the world, the item gives co-op
// shares, and chests and storage boxes (j.box; the bag screen's open and close are j.item).
//
// Bags (save root R): the online bag R+0x13d4930 and the offline bag R+0x13d5150 (holder R+0x13d4920 +0x10 / +0x830), each 130 slots of
// 16 bytes (pkp_items.h: 120 bag slots, then the 10 of kind 4). Which one the local player uses is the selector's rule (0x2ecc04, read
// in main 2.0.1a: 0x2ecc88..0x2eccc8, same as 0x2e70e0): the online bag when the stage byte GM+0x18 is 0x11 (0x99df10), or when the
// session byte GM+0x90 has bit 0 (0x207b0) and bit 2 (0x376c4, the visitor) and GM+0x11 has not bit 0; else the offline bag. It is
// worked out here from those raw bytes (printed with it), the selector is never called. Polled at 1 Hz: one line per item id whose
// total changed; a new save look (the save root changed, or its play time R+0x4a5418 went back or jumped) lists both bags instead.
// World items (coop/PICKUPS-RE.md, verified static; 0x9cb680's reads re-read this pass): pickup 0x9cdba0(mgr, entry, player, w3 auto,
// w4 forced), removal 0x9cb680(entry, quiet). A drop entry (0xd0 bytes) holds its save element at +0, serial u32 +0xa0, index u16
// +0xc4, list u8 +0xc6 (2 important, 1 normal, 3 third), claim s8 +0xc7 (player index, -1 free); the element holds the item word u32
// at +0x10 (bits 0..12 id, 13..20 variant), the count u8 at +0x14 and the position (3 floats) at +0x20. A forced pickup replays the
// effects of one the host decided (0x222a20 on the host, 0x22a544 on the visitor).
// Co-op's item sites (read only; the journal's hook runs before co-op's): the reward list's give 0x3bcbc0(comp, &u16 item, &u16
// count), into this process's bag (its talker is not an argument: on the host it may be a member's reward, PR #835), event-script
// command 0xbea 0x12c734(cmd: u16 item +4, u32 count +8), and the host's join bag piece 0x5727c0 at 0x5728b4 (w20 member, u8 found
// flag at x29+0x1c, x22 the stored entry or 0; coop-v2/REJOIN-BAG-RE.md), read before co-op clears the flag.
//
// The open screen: the bag / container screen (UI class 0x8d, vtable main+0x333ba70) runs its frame 0xfeee78 every frame while it is
// up, as the storage box tabs module finds it (ui/pkp_box_tabs.cpp). Its fields (modding-docs chest/TABS-RE.md, runtime-checked in
// CHEST-RE.md 6): u8 type +0x148, u16 id +0x154 (box tabs rewrites it while it shows a tab), u8 state +0x160 (1 browsing, 3 holding),
// u8 container panel kind +0x168 (0: no container, the bag alone; 1 storage boxes; 0xa the passcode box, its cell at +0x140/+0x142/
// +0x144; 11 portal pods), and the open descriptor's physical payload +0xc8 (id bits 0..15, type bits 32..39). The hook only compares
// those with the view it has; a change opens a view (one line and the contents), no frame for 0.3 s closes it (the line and the diff
// against the open snapshot: held items live in temp copies until placed, so the store has settled by then).
// A container's slice, as 0x2ee4bc computes it (read this pass, main 2.0.1a 0x2ee50c..0x2ee538): count = u16 main+0x280baf0[type],
// first = u16 main+0x280bb2e[type] + count * id, without the id term for types 27 and 28 (the portal pods share one slice); 1 <= type
// <= 30, type 4 is the bag's kind 4. The store (0x5b1c84, CHEST-RE.md 1): u32 main+0x2808bcc + type * 12, 0 the common store
// R+0x13d5970 (1509 slots), else the stage store R+0x13ea9a8 (30633), 0xffffffff none (types 0 and 4).
// Where it stands: the placed container's cabinet record (m_gimmickSaveData R+0xa0ee28, 16 bytes each: u16 cell x/y/z +8/+a/+c, u8
// flags +0xe bit 0 occupied, u8 type +0xf; record = u16 main+0x280bbaa[type] + id), which the open path matches by the entity's cell to
// find the id (chest/TABS.md 1: 0x7b5620 / 0x7b6da0). Written with the player's position (character +0x20c0) at open time.
// Polls every 2 s: each slice's totals (one line per slice that changed, the open view's slice left to its close line) and the cabinet
// records (a container placed or picked up). A stage load swaps the stage store: after a stage or save root change, or while play
// mode is not in game, the polls copy without lines for kSettlePolls and then write one baseline line; more than kReloadChanges
// slices or records at once also take a new baseline.
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "core/pkp_journal.h"
#include "core/pkp_journal_watch.h"
#include "core/pkp_layout.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::journal {

namespace {

using guest::ok;
using guest::rd;

// A main address as an offset from main (an address outside it unchanged), for "%llx".
unsigned long long rel(uint64_t a) {
    const uint64_t b = base();
    return static_cast<unsigned long long>(a >= b ? a - b : a);
}

constexpr uint64_t kBagOnline = layout::kSaveCommon + 0x10;   // m_useItemInventoryOnline (coop/SAVE-BAG-RE.md)
constexpr uint64_t kBagOffline = layout::kSaveCommon + layout::kBagOffline;
constexpr uint32_t kBagAllSlots = 130;                        // 120 + kind 4's 10 (0x5727c0 sends 130, the staging copy holds 130)
constexpr uint64_t kGmSelector = 0x11;                        // game state: u8, bit 0 keeps the offline bag in a session (0x2eccb4)
constexpr uint8_t kSharedTown = 0x11;                         // stage 0x11 (Cloud Island): everyone uses the online bag (0x99df10)
constexpr double kBagPoll = 1.0;
constexpr double kNewLookJump = 30.0;   // play time seconds between two 1 Hz polls past which the save is taken as another one
constexpr size_t kChangesPerLine = 8;   // more item ids changed in one poll than this: one summary line and a list

// World items
constexpr uint64_t kDropSerial = 0xa0, kDropIndex = 0xc4, kDropList = 0xc6, kDropClaim = 0xc7, kDropSize = 0xd0;
constexpr uint64_t kElemWord = 0x10, kElemCount = 0x14, kElemPos = 0x20, kElemSize = 0x30;
constexpr int kRemovalBurst = 20;   // removals written per second; a stage unload removes many: the rest are counted

// Join bag piece (0x5727c0's frame)
constexpr uint64_t kJoinFound = 0x1c;

struct Bag {
    std::vector<uint64_t> slots;   // payloads; empty when not readable
};

struct State {
    double next_bag = 0.0;
    uint64_t root = 0;
    double play_time = 0.0;
    bool online_in_use = false;
    Bag online, offline;
    double removal_window = 0.0;
    int removals = 0, removals_dropped = 0;
};
State g;

Bag read_bag(uint64_t at) {
    Bag b;
    if (!ok(at, kBagAllSlots * items::kSlotSize) || rd<uint64_t>(at) != base() + layout::kItemSlotVtable) return b;
    b.slots.resize(kBagAllSlots);
    for (uint32_t i = 0; i < kBagAllSlots; ++i) b.slots[i] = rd<uint64_t>(at + i * items::kSlotSize + items::kSlotPayload);
    return b;
}

std::map<uint32_t, uint32_t> totals(const Bag& b) {
    std::map<uint32_t, uint32_t> t;
    for (uint64_t p : b.slots)
        if (const uint32_t id = items::id_of(p)) t[id] += items::count_of(p);
    return t;
}

std::vector<std::string> slot_list(const Bag& b) {
    std::vector<std::string> out;
    for (uint32_t i = 0; i < b.slots.size(); ++i)
        if (const uint32_t id = items::id_of(b.slots[i])) out.push_back(fmt("s%u:%u x%u", i, id, items::count_of(b.slots[i])));
    return out;
}

void list_bag(const char* name, const Bag& b) {
    if (b.slots.empty()) return;
    list(Area::Item, name, slot_list(b));
}

// Which bag the local player uses, from the selector's raw bytes; *why gets them.
bool online_in_use(std::string* why) {
    const uint64_t gm = guest::deref(base() + layout::kGameState);
    if (!ok(gm, layout::kStateSession + 1)) {
        if (why) *why = "no game state";
        return false;
    }
    const uint8_t stage = rd<uint8_t>(gm + layout::kStateStage), sel = rd<uint8_t>(gm + kGmSelector),
                  session = rd<uint8_t>(gm + layout::kStateSession);
    const bool online = stage == kSharedTown ||
                        ((session & layout::kSessionOnline) && (session & layout::kSessionVisitor) && !(sel & 1));
    if (why) *why = fmt("stage=0x%02x gm+0x11=0x%02x gm+0x90=0x%02x", stage, sel, session);
    return online;
}

const char* bag_name(bool online, bool in_use) {
    if (online) return in_use ? "online (in use)" : "online (not in use)";
    return in_use ? "offline (in use)" : "offline (not in use)";
}

void diff_bag(const char* name, const Bag& before, const Bag& after) {
    if (before.slots.empty() || after.slots.empty() || before.slots == after.slots) return;
    const auto a = totals(before), b = totals(after);
    std::vector<std::string> changes;
    for (const auto& [id, n] : a) {
        const auto it = b.find(id);
        const uint32_t m = it == b.end() ? 0 : it->second;
        if (m != n) changes.push_back(fmt("%u: %u -> %u (%+d)", id, n, m, static_cast<int>(m) - static_cast<int>(n)));
    }
    for (const auto& [id, m] : b)
        if (!a.count(id)) changes.push_back(fmt("%u: 0 -> %u (+%u)", id, m, m));
    if (changes.empty()) return;   // slots moved (a sort, a stack merge), no total changed
    if (changes.size() <= kChangesPerLine) {
        for (const std::string& ch : changes) Rec(Area::Item, "bag").in("bag=%s", name).out("item %s", ch.c_str());
        return;
    }
    Rec(Area::Item, "bag").in("bag=%s", name).out("%zu item ids changed at once (a join, a load, a script)", changes.size());
    list(Area::Item, fmt("bag %s changes", name).c_str(), changes);
}

void poll_bags(double now) {
    if (now < g.next_bag) return;
    g.next_bag = now + kBagPoll;
    const uint64_t root = guest::deref(base() + layout::kSaveRoot);
    if (!ok(root, kBagOffline + kBagAllSlots * items::kSlotSize)) {
        g.root = 0;
        return;
    }
    Bag on = read_bag(root + kBagOnline), off = read_bag(root + kBagOffline);
    if (on.slots.empty() && off.slots.empty()) return;
    const double t = rd<double>(root + layout::kSavePlayTime);
    std::string why;
    const bool online = online_in_use(&why);
    const bool new_look = root != g.root || t + 1.0 < g.play_time || t > g.play_time + kNewLookJump;
    if (new_look) {
        const auto ton = totals(on), toff = totals(off);
        Rec(Area::Item, "bag first look")
            .in("save play time %.0f s; %s", t, why.c_str())
            .out("in use: %s bag; online %zu ids in %zu slots, offline %zu ids in %zu slots", online ? "online" : "offline", ton.size(),
                 slot_list(on).size(), toff.size(), slot_list(off).size());
        list_bag("bag online", on);
        list_bag("bag offline", off);
    } else {
        if (online != g.online_in_use)
            Rec(Area::Item, "bag in use").in("%s", why.c_str()).out("%s -> %s", g.online_in_use ? "online" : "offline",
                                                                     online ? "online" : "offline");
        diff_bag(bag_name(true, online), g.online, on);
        diff_bag(bag_name(false, !online), g.offline, off);
    }
    g.root = root;
    g.play_time = t;
    g.online_in_use = online;
    g.online = std::move(on);
    g.offline = std::move(off);
}

// The world item a drop entry holds, as "item=.. count=.. at (..) list=.. index=.. serial=.. claim=.."; empty when unreadable.
std::string drop_text(uint64_t entry) {
    if (!ok(entry, kDropSize)) return {};
    const uint64_t elem = rd<uint64_t>(entry);
    if (!ok(elem, kElemSize)) return fmt("entry index=%u (no element)", rd<uint16_t>(entry + kDropIndex));
    const uint32_t word = rd<uint32_t>(elem + kElemWord);
    return fmt("item=%u variant=%u count=%u at (%.1f, %.1f, %.1f) list=%u index=%u serial=%u claim=%d", word & items::kIdMask,
               (word >> 13) & 0xff, rd<uint8_t>(elem + kElemCount), rd<float>(elem + kElemPos), rd<float>(elem + kElemPos + 4),
               rd<float>(elem + kElemPos + 8), rd<uint8_t>(entry + kDropList), rd<uint16_t>(entry + kDropIndex),
               rd<uint32_t>(entry + kDropSerial), rd<int8_t>(entry + kDropClaim));
}

const char* player_name(uint64_t player) {
    const uint64_t pm = guest::deref(base() + layout::kPlayerManager);
    if (!ok(pm, layout::kManagerPlayer + 16) || !player) return "?";
    if (player == rd<uint64_t>(pm + layout::kManagerPlayer)) return "local";
    if (player == rd<uint64_t>(pm + layout::kManagerPlayer + 8)) return "second (local P2)";
    return "other";
}

void on_pickup(::Ctx* c) {
    const std::string drop = drop_text(c->x[1]);
    Rec(Area::Item, "world pickup")
        .in("%s player=%s auto=%u forced=%u", drop.empty() ? "entry unreadable" : drop.c_str(), player_name(c->x[2]),
            static_cast<unsigned>(c->x[3] & 1), static_cast<unsigned>(c->x[4] & 1))
        .note("from 0x%llx%s", rel(c->x[30]), (c->x[4] & 1) ? "; forced: effects of a pickup the host decided" : "");
}

// The removals a burst left out, once its second is over (from the next removal or the game frame).
void flush_removals(double now) {
    if (now - g.removal_window < 1.0) return;
    if (g.removals_dropped)
        Rec(Area::Item, "world removal").out("%d more removals in one second not written (a stage unload)", g.removals_dropped);
    g.removal_window = now;
    g.removals = g.removals_dropped = 0;
}

void on_removal(::Ctx* c) {
    flush_removals(qpc::seconds());
    if (++g.removals > kRemovalBurst) {
        ++g.removals_dropped;
        return;
    }
    const std::string drop = drop_text(c->x[0]);
    Rec(Area::Item, "world removal")
        .in("%s quiet=%u", drop.empty() ? "entry unreadable" : drop.c_str(), static_cast<unsigned>(c->x[1] & 1))
        .note("from 0x%llx", rel(c->x[30]));
}

void on_reward_give(::Ctx* c) {
    const bool okp = ok(c->x[1], 2) && ok(c->x[2], 2);
    Rec(Area::Item, "give reward")
        .in("item=%u count=%u (reward list)", okp ? rd<uint16_t>(c->x[1]) : 0u, okp ? rd<uint16_t>(c->x[2]) : 0u)
        .out("into this process's bag (%s)", bag_name(online_in_use(nullptr), true))
        .note("from 0x%llx; the talker is not an argument here", rel(c->x[30]));
}

void on_script_give(::Ctx* c) {
    const bool okp = ok(c->x[0], 0xc);
    Rec(Area::Item, "give script")
        .in("item=%u count=%u (event-script command 0xbea)", okp ? rd<uint16_t>(c->x[0] + 4) : 0u, okp ? rd<uint32_t>(c->x[0] + 8) : 0u)
        .out("into this process's bag (%s)", bag_name(online_in_use(nullptr), true))
        .note("from 0x%llx", rel(c->x[30]));
}

void on_join_bag(::Ctx* c) {
    const uint64_t flag = c->x[29] + kJoinFound;
    const int found = ok(flag, 1) ? rd<uint8_t>(flag) : -1;
    // The stored entry is a bag's 130 slots (the writer loop 0x572b60 reads u32 item word at entry + 8 + i * 0x10, u8 count at +0xc).
    const uint64_t entry = c->x[22];
    std::vector<std::string> stored;
    uint32_t total = 0;
    if (found == 1 && ok(entry, kBagAllSlots * items::kSlotSize))
        for (uint32_t i = 0; i < kBagAllSlots; ++i) {
            const uint64_t p = rd<uint64_t>(entry + i * items::kSlotSize + items::kSlotPayload);
            if (!items::id_of(p)) continue;
            stored.push_back(fmt("s%u:%u x%u", i, items::id_of(p), items::count_of(p)));
            total += items::count_of(p);
        }
    Rec(Area::Item, "join bag piece")
        .in("member=%u found=%d stored-entry=%s entry-items=%zu slots, %u items", static_cast<unsigned>(c->x[20] & 0xff), found,
            entry ? "yes" : "no", stored.size(), total)
        .out("%s", found == 1 ? "the game sends this save's stored copy (co-op clears the flag after this line)"
                              : "no stored copy: nothing sent, the member keeps its bag")
        .expect("the member keeps its own bag");
    if (!stored.empty()) list(Area::Item, fmt("join bag stored copy member=%u", static_cast<unsigned>(c->x[20] & 0xff)).c_str(), stored);
}

// journal::list of the bag the local player uses now, under "<what>: bag online|offline".
void list_bag_in_use(const char* what) {
    std::string why;
    const bool online = online_in_use(&why);
    const uint64_t root = guest::deref(base() + layout::kSaveRoot);
    const Bag b = ok(root, 8) ? read_bag(root + (online ? kBagOnline : kBagOffline)) : Bag{};
    const std::string name = fmt("%s: bag %s", what, online ? "online" : "offline");
    if (b.slots.empty()) Rec(Area::Item, name.c_str()).out("not readable").note("%s", why.c_str());
    else list(Area::Item, name.c_str(), slot_list(b));
}

// ---- chests and storage boxes ----

// The screen
constexpr uint64_t kScreenVtable = 0x333ba70;   // ui/pkp_box_tabs.cpp
constexpr uint64_t kScreenSize = 0x1a8;
constexpr uint64_t kType = 0x148, kId = 0x154, kState = 0x160, kKind = 0x168, kPhysical = 0xc8, kScreenCell = 0x140;
constexpr uint8_t kNoContainer = 0, kPasscodeKind = 0xa;
constexpr double kCloseGap = 0.3;   // as box tabs' reopen gap

// Slices and stores
constexpr uint64_t kStoreKinds = 0x2808bcc, kStoreKindStride = 12;
constexpr uint32_t kNoStore = 0xffffffffu;
constexpr uint64_t kCommonStore = layout::kSaveCommon + 0x1050;   // R+0x13d5970
constexpr uint32_t kCommonSlots = 1509, kStageSlots = 30633;
constexpr uint8_t kMaxType = 30, kBagKind4 = 4, kPodTown = 27, kPodCloud = 28;

constexpr double kPollEvery = 2.0;
constexpr int kSettlePolls = 2;   // polls copied silently after a trip sign (stage or root change, play mode not in game)
constexpr size_t kReloadChanges = 16;
constexpr size_t kNetPerLine = 6;   // item ids written in a slice line's note

struct Slice {
    uint64_t at = 0;   // the first slot
    uint32_t first = 0, count = 0;
    bool common = false;
};

struct View {
    bool active = false;
    uint64_t screen = 0;
    uint8_t type = 0, kind = 0;
    uint16_t id = 0, physical = 0;
    Slice slice;
    std::vector<uint64_t> snap;
    double opened = 0.0, last_seen = 0.0;
};

struct SliceKey {
    uint8_t type;
    uint16_t id;
    uint32_t first, count;
    bool common;
};

struct BoxState {
    View view;
    double next_poll = 0.0;
    uint64_t root = 0;
    int stage = -1;
    int settle = 0;                                   // polls left to copy without lines; the last one writes the baseline
    std::vector<SliceKey> keys;                       // every slice, built once per save root
    std::vector<uint64_t> common, stage_store;        // payloads at the last poll
    std::vector<uint64_t> cabinets;                   // each record's bytes +8..+0xf at the last poll
};
BoxState gb;

uint64_t root() { return guest::deref(base() + layout::kSaveRoot); }

uint16_t main_u16(uint64_t table, uint8_t type) { return rd<uint16_t>(base() + table + type * 2u); }

bool resolve(uint64_t r, uint8_t type, uint16_t id, Slice& out) {
    if (type < 1 || type > kMaxType || type == kBagKind4) return false;
    const uint32_t kind = rd<uint32_t>(base() + kStoreKinds + type * kStoreKindStride);
    if (kind == kNoStore) return false;
    const uint32_t count = main_u16(layout::kContainerSlotCounts, type);
    const uint32_t first = main_u16(layout::kContainerSlotBases, type) + (type == kPodTown || type == kPodCloud ? 0 : count * id);
    const bool common = kind == 0;
    if (!count || first + count > (common ? kCommonSlots : kStageSlots)) return false;
    out = {r + (common ? kCommonStore : layout::kStageContainerStore) + uint64_t{first} * items::kSlotSize, first, count, common};
    return ok(out.at, uint64_t{count} * items::kSlotSize);
}

std::vector<uint64_t> payloads(uint64_t at, uint32_t n) {
    std::vector<uint64_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = rd<uint64_t>(at + i * items::kSlotSize + items::kSlotPayload);
    return v;
}

// "cell (x, y, z)" of the container's cabinet record, or why there is none.
std::string cell_text(uint64_t r, uint8_t type, uint16_t id) {
    if (type < 1 || type > kMaxType) return "no cabinet record";
    if (id >= main_u16(layout::kCabinetCounts, type)) return fmt("no cabinet record (id %u of %u)", id, main_u16(layout::kCabinetCounts, type));
    const uint32_t index = main_u16(layout::kCabinetBases, type) + id;
    const uint64_t rec = r + layout::kCabinetRecords + uint64_t{index} * layout::kCabinetRecordSize;
    if (index >= layout::kCabinetRecordCount || !ok(rec, layout::kCabinetRecordSize)) return fmt("no cabinet record (record %u)", index);
    const uint8_t flags = rd<uint8_t>(rec + layout::kCabinetFlags), rtype = rd<uint8_t>(rec + layout::kCabinetType);
    const std::string cell = fmt("(%u, %u, %u)", rd<uint16_t>(rec + layout::kCabinetCellX), rd<uint16_t>(rec + layout::kCabinetCellY),
                                 rd<uint16_t>(rec + layout::kCabinetCellZ));
    if ((flags & layout::kCabinetOccupied) && rtype == type) return fmt("cell %s (record %u)", cell.c_str(), index);
    return fmt("record %u not this container's (flags 0x%02x type %u cell %s)", index, flags, rtype, cell.c_str());
}

std::string player_text() {
    const uint64_t pm = guest::deref(base() + layout::kPlayerManager);
    const uint64_t me = ok(pm, layout::kManagerPlayer + 8) ? rd<uint64_t>(pm + layout::kManagerPlayer) : 0;
    if (!ok(me, layout::kPosition + 12)) return "player at ?";
    return fmt("player at (%.1f, %.1f, %.1f)", rd<float>(me + layout::kPosition), rd<float>(me + layout::kPosition + 4),
               rd<float>(me + layout::kPosition + 8));
}

std::string slot_text(uint32_t slot, uint64_t p) {
    return items::id_of(p) ? fmt("s%u:%u x%u", slot, items::id_of(p), items::count_of(p)) : fmt("s%u:empty", slot);
}

std::vector<std::string> contents(const std::vector<uint64_t>& v, uint32_t* used, uint32_t* total) {
    std::vector<std::string> out;
    *used = *total = 0;
    for (uint32_t i = 0; i < v.size(); ++i)
        if (items::id_of(v[i])) {
            out.push_back(slot_text(i, v[i]));
            ++*used;
            *total += items::count_of(v[i]);
        }
    return out;
}

const char* store_name(const Slice& s) { return s.common ? "common" : "stage"; }

// ---- the open view ----

void open_view(View& v, const char* how) {
    const uint64_t r = root();
    if (v.kind == kNoContainer) {
        Rec(Area::Item, "bag screen open").in("type=%u id=%u kind=0 (no container)", v.type, v.id).note("%s", how);
        list_bag_in_use("bag screen open");
        return;
    }
    std::string where = cell_text(r, v.type, v.physical);
    if (v.kind == kPasscodeKind && ok(v.screen + kScreenCell, 6))
        where += fmt("; screen cell (%u, %u, %u)", rd<uint16_t>(v.screen + kScreenCell), rd<uint16_t>(v.screen + kScreenCell + 2),
                     rd<uint16_t>(v.screen + kScreenCell + 4));
    const std::string me = player_text();
    if (!ok(r, 8) || !resolve(r, v.type, v.id, v.slice)) {
        v.snap.clear();
        Rec(Area::Box, "open").in("type=%u id=%u physical=%u kind=%u", v.type, v.id, v.physical, v.kind)
            .out("no slice for this type (contents not read)").note("%s; %s; %s", where.c_str(), me.c_str(), how);
        return;
    }
    v.snap = payloads(v.slice.at, v.slice.count);
    uint32_t used, total;
    const std::vector<std::string> items_in = contents(v.snap, &used, &total);
    Rec(Area::Box, "open")
        .in("type=%u id=%u physical=%u kind=%u", v.type, v.id, v.physical, v.kind)
        .out("%u of %u slots used, %u items (%s store slots %u..%u)", used, v.slice.count, total, store_name(v.slice), v.slice.first,
             v.slice.first + v.slice.count - 1)
        .note("%s; %s; %s", where.c_str(), me.c_str(), how);
    if (!items_in.empty()) list(Area::Box, fmt("box type=%u id=%u contents", v.type, v.id).c_str(), items_in);
}

void refresh_baseline(const Slice& s);

void close_view(View& v, const char* how) {
    const double secs = qpc::seconds() - v.opened;
    if (v.kind == kNoContainer) {
        Rec(Area::Item, "bag screen close").out("open %.1f s", secs).note("%s", how);
        v.active = false;
        return;
    }
    if (v.snap.empty() || !ok(v.slice.at, uint64_t{v.slice.count} * items::kSlotSize)) {
        Rec(Area::Box, "close").in("type=%u id=%u physical=%u", v.type, v.id, v.physical).out("open %.1f s, contents not read", secs)
            .note("%s", how);
        v.active = false;
        return;
    }
    const std::vector<uint64_t> now = payloads(v.slice.at, v.slice.count);
    std::vector<std::string> changes;
    std::map<uint32_t, int> net;
    for (uint32_t i = 0; i < now.size(); ++i) {
        if (now[i] == v.snap[i]) continue;
        changes.push_back(slot_text(i, v.snap[i]) + " -> " +
                          (items::id_of(now[i]) ? fmt("%u x%u", items::id_of(now[i]), items::count_of(now[i])) : std::string("empty")));
        if (items::id_of(v.snap[i])) net[items::id_of(v.snap[i])] -= static_cast<int>(items::count_of(v.snap[i]));
        if (items::id_of(now[i])) net[items::id_of(now[i])] += static_cast<int>(items::count_of(now[i]));
    }
    std::string in_out;
    for (const auto& [id, d] : net)
        if (d) in_out += fmt("%s%u %+d", in_out.empty() ? "" : ", ", id, d);
    uint32_t used, total;
    contents(now, &used, &total);
    Rec(Area::Box, "close")
        .in("type=%u id=%u physical=%u", v.type, v.id, v.physical)
        .out("open %.1f s, %zu slots changed, items %s; now %u of %u slots used, %u items", secs, changes.size(),
             in_out.empty() ? "unchanged" : in_out.c_str(), used, v.slice.count, total)
        .note("%s", how);
    if (!changes.empty()) list(Area::Box, fmt("box type=%u id=%u changes", v.type, v.id).c_str(), changes);
    refresh_baseline(v.slice);
    v.active = false;
}

// ---- the 2 s polls ----

void build_keys() {
    gb.keys.clear();
    for (uint8_t type = 1; type <= kMaxType; ++type) {
        if (type == kBagKind4 || rd<uint32_t>(base() + kStoreKinds + type * kStoreKindStride) == kNoStore) continue;
        const uint32_t ids = type == kPodTown || type == kPodCloud ? 1 : main_u16(layout::kCabinetCounts, type);
        const uint32_t count = main_u16(layout::kContainerSlotCounts, type);
        for (uint32_t id = 0; id < ids; ++id) {
            const uint32_t first = main_u16(layout::kContainerSlotBases, type) + (ids == 1 ? 0 : count * id);
            const bool common = rd<uint32_t>(base() + kStoreKinds + type * kStoreKindStride) == 0;
            if (count && first + count <= (common ? kCommonSlots : kStageSlots))
                gb.keys.push_back({type, static_cast<uint16_t>(id), first, count, common});
        }
    }
}

void refresh_baseline(const Slice& s) {
    std::vector<uint64_t>& store = s.common ? gb.common : gb.stage_store;
    if (store.size() < s.first + s.count) return;
    const std::vector<uint64_t> now = payloads(s.at, s.count);
    std::copy(now.begin(), now.end(), store.begin() + s.first);
}

void slice_totals(const std::vector<uint64_t>& store, const SliceKey& k, uint32_t* used, uint32_t* total, std::map<uint32_t, int>* net,
                  int sign) {
    *used = *total = 0;
    for (uint32_t i = k.first; i < k.first + k.count; ++i)
        if (const uint32_t id = items::id_of(store[i])) {
            ++*used;
            *total += items::count_of(store[i]);
            if (net) (*net)[id] += sign * static_cast<int>(items::count_of(store[i]));
        }
}

bool view_shows(const SliceKey& k) {
    const View& v = gb.view;
    return v.active && v.kind != kNoContainer && !v.snap.empty() && v.slice.common == k.common && v.slice.first == k.first;
}

enum class Poll { Quiet, Baseline, Diff };

void poll_slices(uint64_t r, Poll mode) {
    std::vector<uint64_t> common = payloads(r + kCommonStore, kCommonSlots), stage = payloads(r + layout::kStageContainerStore, kStageSlots);
    if (mode != Poll::Diff || gb.common.size() != kCommonSlots || gb.stage_store.size() != kStageSlots) {
        size_t holding = 0;
        for (const SliceKey& k : gb.keys) {
            uint32_t used, total;
            slice_totals(k.common ? common : stage, k, &used, &total, nullptr, 0);
            if (used) ++holding;
        }
        if (mode != Poll::Quiet)
            Rec(Area::Box, "slices baseline").in("stage=%d", gb.stage).out("%zu of %zu container slices hold items", holding, gb.keys.size());
        gb.common = std::move(common);
        gb.stage_store = std::move(stage);
        return;
    }
    std::vector<const SliceKey*> changed;
    for (const SliceKey& k : gb.keys) {
        const std::vector<uint64_t>& a = k.common ? gb.common : gb.stage_store;
        const std::vector<uint64_t>& b = k.common ? common : stage;
        if (!std::equal(a.begin() + k.first, a.begin() + k.first + k.count, b.begin() + k.first) && !view_shows(k)) changed.push_back(&k);
    }
    if (changed.size() > kReloadChanges) {
        Rec(Area::Box, "slices reloaded").in("stage=%d", gb.stage).out("%zu slices changed at once: new baseline", changed.size());
    } else {
        for (const SliceKey* k : changed) {
            std::map<uint32_t, int> net;
            uint32_t used0, total0, used1, total1;
            slice_totals(k->common ? gb.common : gb.stage_store, *k, &used0, &total0, &net, -1);
            slice_totals(k->common ? common : stage, *k, &used1, &total1, &net, 1);
            std::string items_net;
            size_t n = 0;
            for (const auto& [id, d] : net)
                if (d && n++ < kNetPerLine) items_net += fmt("%s%u %+d", items_net.empty() ? "" : ", ", id, d);
            if (n > kNetPerLine) items_net += fmt(", +%zu more", n - kNetPerLine);
            Rec(Area::Box, "slice")
                .in("type=%u id=%u (%s store slots %u..%u)", k->type, k->id, k->common ? "common" : "stage", k->first, k->first + k->count - 1)
                .out("items %u -> %u, slots used %u -> %u; %s", total0, total1, used0, used1, items_net.empty() ? "moved only" : items_net.c_str())
                .note("%s", cell_text(r, k->type, k->id).c_str());
        }
    }
    gb.common = std::move(common);
    gb.stage_store = std::move(stage);
}

void poll_cabinets(uint64_t r, Poll mode) {
    const uint64_t recs = r + layout::kCabinetRecords;
    std::vector<uint64_t> now(layout::kCabinetRecordCount);
    for (uint32_t i = 0; i < layout::kCabinetRecordCount; ++i) now[i] = rd<uint64_t>(recs + i * layout::kCabinetRecordSize + 8);
    auto placed = [](uint64_t v) { return (v >> 48) & layout::kCabinetOccupied; };
    if (mode != Poll::Diff || gb.cabinets.size() != now.size()) {
        size_t n = 0;
        for (uint64_t v : now) n += placed(v);
        if (mode != Poll::Quiet) Rec(Area::Box, "cabinets baseline").in("stage=%d", gb.stage).out("%zu of %u cabinet records placed", n, layout::kCabinetRecordCount);
        gb.cabinets = std::move(now);
        return;
    }
    std::vector<uint32_t> changed;
    for (uint32_t i = 0; i < now.size(); ++i)
        if (now[i] != gb.cabinets[i]) changed.push_back(i);
    if (changed.size() > kReloadChanges) {
        Rec(Area::Box, "cabinets reloaded").in("stage=%d", gb.stage).out("%zu records changed at once: new baseline", changed.size());
    } else {
        for (uint32_t i : changed) {
            const uint64_t a = gb.cabinets[i], b = now[i];
            const uint8_t type = static_cast<uint8_t>((b >> 56) ? b >> 56 : a >> 56);   // a freed record may lose its type
            const bool was = placed(a), is = placed(b);
            const uint32_t first = type >= 1 && type <= kMaxType ? main_u16(layout::kCabinetBases, type) : 0;
            Rec(Area::Box, is && !was ? "placed" : !is && was ? "picked up" : "cabinet changed")
                .in("record %u type=%u id=%d", i, type, type >= 1 && type <= kMaxType && i >= first ? static_cast<int>(i - first) : -1)
                .out("cell (%u, %u, %u) flags 0x%02x (was cell (%u, %u, %u) flags 0x%02x type %u)", static_cast<unsigned>(b & 0xffff),
                     static_cast<unsigned>((b >> 16) & 0xffff), static_cast<unsigned>((b >> 32) & 0xffff),
                     static_cast<unsigned>((b >> 48) & 0xff), static_cast<unsigned>(a & 0xffff), static_cast<unsigned>((a >> 16) & 0xffff),
                     static_cast<unsigned>((a >> 32) & 0xffff), static_cast<unsigned>((a >> 48) & 0xff), static_cast<unsigned>(a >> 56));
        }
    }
    gb.cabinets = std::move(now);
}

void on_screen_frame(::Ctx* c) {
    const uint64_t screen = c->x[0];
    if (!ok(screen, kScreenSize) || rd<uint64_t>(screen) != base() + kScreenVtable) return;
    const double now = qpc::seconds();
    const uint8_t type = rd<uint8_t>(screen + kType), kind = rd<uint8_t>(screen + kKind);
    const uint16_t id = rd<uint16_t>(screen + kId);
    const uint64_t payload = rd<uint64_t>(screen + kPhysical);
    const uint16_t physical = ((payload >> 32) & 0xff) == type ? static_cast<uint16_t>(payload) : id;
    View& v = gb.view;
    if (v.active && (screen != v.screen || kind != v.kind || type != v.type || physical != v.physical || now - v.last_seen > kCloseGap))
        close_view(v, "reopened");
    if (v.active && id != v.id) {
        close_view(v, fmt("tab: shown id %u -> %u", v.id, id).c_str());
        v = {.active = true, .screen = screen, .type = type, .kind = kind, .id = id, .physical = physical, .opened = now, .last_seen = now};
        open_view(v, fmt("tab of physical id %u (state %u)", physical, rd<uint8_t>(screen + kState)).c_str());
    } else if (!v.active) {
        v = {.active = true, .screen = screen, .type = type, .kind = kind, .id = id, .physical = physical, .opened = now, .last_seen = now};
        open_view(v, fmt("state %u", rd<uint8_t>(screen + kState)).c_str());
    }
    v.last_seen = now;
}

void on_cabinet_alloc(::Ctx* c) {
    Rec(Area::Box, "place ask")
        .in("type=%u cell (%u, %u, %u)", static_cast<unsigned>(c->x[0] & 0xff), static_cast<unsigned>(c->x[1] & 0xffff),
            static_cast<unsigned>(c->x[2] & 0xffff), static_cast<unsigned>(c->x[3] & 0xffff))
        .note("from 0x%llx; the record it gets follows in a \"placed\" line", rel(c->x[30]));
}

void poll_boxes(double now, bool in_game) {
    if (gb.view.active && now - gb.view.last_seen > kCloseGap) close_view(gb.view, "closed");
    if (now < gb.next_poll) return;
    gb.next_poll = now + kPollEvery;
    const uint64_t r = root();
    if (!ok(r + kCommonStore, uint64_t{kCommonSlots} * items::kSlotSize) ||
        !ok(r + layout::kStageContainerStore, uint64_t{kStageSlots} * items::kSlotSize) ||
        !ok(r + layout::kCabinetRecords, uint64_t{layout::kCabinetRecordCount} * layout::kCabinetRecordSize)) {
        gb.root = 0;
        return;
    }
    const uint64_t gm = guest::deref(base() + layout::kGameState);
    const int stage = ok(gm, layout::kStateStage + 1) ? rd<uint8_t>(gm + layout::kStateStage) : -1;
    // A trip: the stage byte and the stage store do not change in the same poll, and a load takes several seconds. Copy without
    // lines while play mode is not in game and for kSettlePolls after any sign, then write the new baseline.
    if (r != gb.root || stage != gb.stage || !in_game) gb.settle = kSettlePolls + 1;
    if (r != gb.root) build_keys();
    gb.root = r;
    gb.stage = stage;
    const Poll mode = gb.settle > 1 ? Poll::Quiet : gb.settle == 1 ? Poll::Baseline : Poll::Diff;
    if (gb.settle) --gb.settle;
    poll_slices(r, mode);
    poll_cabinets(r, mode);
}

void reset_boxes() { gb = {}; }

constexpr WatchSite kSites[] = {
    {{0xfeee78, kPacibsp, true}, &on_screen_frame, "bag/container screen frame"},
    {{0x776540, kPacibsp, true}, &on_cabinet_alloc, "cabinet allocator"},
    {{0x9cdba0, 0xb40045e1u, true}, &on_pickup, "world item pickup"},   // cbz x1 (before its pacibsp)
    {{0x9cb680, kPacibsp, true}, &on_removal, "world item removal"},
    {{0x3bcbc0, 0xaa0203e8u, true}, &on_reward_give, "reward list give"},   // mov x8, x2
    {{0x12c734, 0x79400808u, true}, &on_script_give, "script give 0xbea"},   // ldrh w8, [x0, #4]
    {{0x5728b4, 0xf9400660u, false}, &on_join_bag, "join bag piece"},       // ldr x0, [x19, #8]
};

void game_frame(::Ctx*, bool in_game) {
    const double now = qpc::seconds();
    poll_bags(now);
    if (g.removals) flush_removals(now);
    poll_boxes(now, in_game);
}

void reset() {
    g = {};
    reset_boxes();
}

}  // namespace

const Watcher kItemsWatcher = {"items", kSites, sizeof kSites / sizeof kSites[0], &game_frame, &reset};

}  // namespace nxrt::games::pkp::journal
