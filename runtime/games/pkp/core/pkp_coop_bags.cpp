// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's bag records (pkp_coop_internal.h; RE: modding-docs coop-v2 BAG-RECORDS-PLAN.md, P0-RE.md P0RE-11,
// REJOIN-BAG-2-RE.md). Vanilla Link Play treats a visitor's bag as the host's to keep and hand back, and a copy that drifts from the
// real bag came back at a rejoin, a drop or a crash: an empty bag, or another session's. In co-op the bag is ours instead (PROJECT.md
// 6.7): one record per member, which is the truth, written into whichever bag the game uses; the game's own writers that fight it are
// switched off (pkp_coop_bag.cpp).
//
// The game, 2.0.1a:
//   The bag is 130 slots of 16 bytes (layout::kBagWholeSlots): +0 the slot vtable (main + layout::kItemSlotVtable, a pointer into this
//   process's main, never copied), +8 the payload (pkp_items.h: packed item word, count; only items::kMoved of it is copied, as the
//   game's own copies do, so each slot keeps its bit 31 and padding). There are two: the offline bag (save common
//   + layout::kBagOffline) and the online one (+ layout::kBagOnline). The selectors 0x2ecc04 / 0x2e70e0 take the online bag on stage
//   0x11 (Cloud Island), or when IsOnline (0x207b0), not GM+0x11 bit 0 (layout::kStateOwnBag) and the visitor test (0x376c4); the
//   offline bag otherwise. (Their third case, the local second player, is a split-screen window of its own here: its own process.)
//   While an item is held, the bag screen works on a temp copy (layout::kItemTempFlag) and copies it back over the bag at the end;
//   while it is open at all, its panels show the array as they last read it. After a whole-bag write the game refreshes the HUD's
//   item bar (layout::kItemBarRefresh); so does every write here.
//   The host also keeps a working copy of each visitor's bag (save root + layout::kBagMirror, by member table index) that its request
//   checks read and change (station crafts, pickups). At a join (0x575644, join state 3 -> 4) the game fills it from its per-user
//   table and sets its valid flag, which later stores it back there.
// The record:
//   - per member key: a sequence number and the 130 payloads. A file per key, <save dir>/bags/<member key>.bin ([fs] save_dir is
//     the co-op folder, launcher coop_save.rs): "CBAG", u8 format, u64 sequence, 130 x 16 bytes (the first 8 of each 0), written
//     whole (temp file + rename) at most every kWriteEvery while it changed, and at session end. Every process keeps its own player's
//     record there; the authority also keeps every member's (a later host, the newest copy, has at least its own).
//   - when two copies meet, the higher sequence wins.
// Every process, the authority's own bag too, in a session:
//   - apply: the record is written into the bag the selector picks now, at session up, when the authority's copy is newer, and when
//     the selection may change (the visitor test, GM+0x11 bit 0, the stage, the player's character made again at a load's end). The
//     game's own loaded bag is never taken over a record. No record anywhere (a first co-op session): the bag as it is becomes the
//     first record, once the selection has held for kGrace. A member does neither before its visitor test is up (until then the
//     selection is its own island's bag, not the one it plays the session with). An apply waits while the bag / container screen
//     is open, so no panel shows a bag that is no longer there.
//   - take: once a frame the selected bag is compared with the record; a change becomes the record, sequence + 1, and a member sends
//     it to the authority (BagRecord, at most every kSendEvery).
//   - safeguard: within kGrace of an apply or a selection change (a load, an area or session change: where the game's own wipes
//     ran, and any not mapped would), a change that loses anything is not taken: the record is written back into the bag, and the
//     refusal logged (an emptied bag and more than kMaxKindsGone item kinds gone are named in the line). In play every change is
//     taken, a store-all too.
//     Losses are counted per item id over the whole bag, so a sort or a stack merge (slots emptied, nothing lost) is taken.
//   - a member sends its record (sequence 0 when it has none) at session up, again every kHelloEvery until the authority answered;
//     the authority keeps the newer copy and answers the first record of each member's session (and any older than its copy) with a
//     BagRestore. Until that answer a member takes nothing, so a stale bag never outbids the authority's copy.
// The authority's mirror of each visitor:
//   - a record carries its player's user id, which names its row in the member table, so its mirror index. The game's fill at the
//     join is replaced: the mirror gets the record (when there is none yet, the mirror stays as the game cleared it until the first
//     arrives), the valid flag stays clear (nothing stores the mirror back), and the game's fill from its table does not run (it
//     also pushed what did not fit into the host's PC). Every newer record of that member is fed into its mirror again.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "core/pkp_layout.h"
#include "core/pkp_save_files.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "nxrt/file_write.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::bags {

namespace {

constexpr const char* kTag = "pkp";

constexpr size_t kSlots = layout::kBagWholeSlots;
static_assert(kSlots == wire::kBagRecordSlots && items::kSlotSize == wire::kBagSlotBytes, "the record is the game's whole bag");
constexpr uint64_t kBagBytes = kSlots * items::kSlotSize;

constexpr double kWriteEvery = 2.0;   // seconds between two writes of one record's file
constexpr double kSendEvery = 1.0;    // seconds between two BagRecords of a member
constexpr double kHelloEvery = 5.0;   // seconds between two first BagRecords while the authority has not answered
constexpr double kGrace = 3.0;        // seconds after an apply or a selection change in which no loss is taken
constexpr uint32_t kMaxKindsGone = 8;
constexpr double kRefuseLogEvery = 1.0;
constexpr double kNotReadyLog = 15.0; // seconds in a session with the player in the world but the bag never ready: logged once
constexpr double kScreenGap = 0.3;    // seconds without a bag / container screen frame: the screen is closed

constexpr char kFileMagic[4] = {'C', 'B', 'A', 'G'};
constexpr uint8_t kFormat = 1;
constexpr size_t kFileHead = sizeof kFileMagic + 1 + 8;

using Key = std::array<uint8_t, kMemberKeyBytes>;
using UserId = std::array<uint8_t, layout::kUserIdSize>;

struct Record {
    uint64_t seq = 0;   // 0: no record
    std::array<uint64_t, kSlots> slot{};   // each slot's payload (+8)
};

// A record with its file.
struct Kept {
    Record rec;
    bool dirty = false;
    double written_at = -1e9;
    UserId user{};   // the authority: the member's user id, from its last BagRecord (all 0: not known yet)
};

// What the bag selector reads, as last seen.
struct Selection {
    uint64_t bag = 0;
    uint8_t stage = 0, own_bag = 0;
    bool visitor = false, online = false;
    bool player = false;   // the local player's character exists (it is made again at the end of a load)
    bool operator==(const Selection&) const = default;
};

struct State {
    bool loaded = false;      // own's file was read
    Kept own;                 // this process's player
    std::map<Key, Kept> members;   // the authority: every other member's, by key (read from its file on first use)
    bool active = false;      // in a session at the last tick
    bool answered = false;    // a member: the authority answered this session's first record (the authority: always)
    double hello_at = -1e9;
    bool send = false;        // a member: the record changed since the last BagRecord
    double sent_at = -1e9;
    bool apply = false;       // the record waits to be written into the selected bag
    double grace_until = 0.0;
    double refused_log_at = -1e9;
    Selection seen;
    bool looked = false;      // seen holds this session's first look
    double up_at = 0.0;       // the session came up
    bool was_ready = false;   // the bag was ready once in this session (or its absence was logged)
    uint32_t greeted = 0;     // the authority: bit per node whose first record of its session was answered
    double screen_at = -1e9;  // the bag / container screen's last frame (its hook runs on the game thread, as the tick)
};
State& st() {
    static State s;
    return s;
}

Key own_key() {
    Key k;
    std::memcpy(k.data(), member_key_bytes(), k.size());
    return k;
}

std::string hex(const Key& k) {
    std::string out;
    char b[3];
    for (uint8_t v : k) {
        std::snprintf(b, sizeof b, "%02x", v);
        out += b;
    }
    return out;
}

// Slots in use and items in them.
std::pair<uint32_t, uint32_t> totals(const Record& r) {
    uint32_t slots = 0, count = 0;
    for (uint64_t p : r.slot)
        if (items::id_of(p)) ++slots, count += items::count_of(p);
    return {slots, count};
}

// ---- the files ----

std::filesystem::path file_of(const Key& k) { return save_files::save_dir() / "bags" / (hex(k) + ".bin"); }

bool read_file(const Key& k, Record* out) {
    std::string b;
    if (std::ifstream in{file_of(k), std::ios::binary}) b.assign(std::istreambuf_iterator<char>(in), {});
    if (b.empty()) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(b.data());
    if (b.size() != kFileHead + kBagBytes || std::memcmp(p, kFileMagic, sizeof kFileMagic) != 0 || p[4] != kFormat) {
        NXLOG_W(kTag, "Co-op bags: %s is not a bag record of this build; it is not used", save_files::u8(file_of(k)).c_str());
        return false;
    }
    std::memcpy(&out->seq, p + 5, 8);
    for (size_t i = 0; i < kSlots; ++i) {
        std::memcpy(&out->slot[i], p + kFileHead + i * items::kSlotSize + items::kSlotPayload, 8);
        out->slot[i] = items::moved_of(out->slot[i]);
    }
    return true;
}

void write_file(const Key& k, Kept& kept, double now) {
    std::string b(kFileHead + kBagBytes, '\0');
    std::memcpy(b.data(), kFileMagic, sizeof kFileMagic);
    b[4] = static_cast<char>(kFormat);
    std::memcpy(b.data() + 5, &kept.rec.seq, 8);
    for (size_t i = 0; i < kSlots; ++i) std::memcpy(b.data() + kFileHead + i * items::kSlotSize + items::kSlotPayload, &kept.rec.slot[i], 8);
    std::string err;
    kept.written_at = now;
    if (!write_file_atomic(save_files::u8(file_of(k)), b, &err)) {
        NXLOG_W(kTag, "Co-op bags: the record of %s is not written: %s", hex(k).c_str(), err.c_str());
        return;
    }
    kept.dirty = false;
    const auto [slots, count] = totals(kept.rec);
    NXLOG_I(kTag, "Co-op bags: kept %s record %llu (%u slots, %u items)", k == own_key() ? "this player's" : hex(k).c_str(),
            static_cast<unsigned long long>(kept.rec.seq), slots, count);
}

// Writes every changed record whose last write is kWriteEvery old (all of them with force).
void flush(bool force) {
    State& s = st();
    const double now = qpc::seconds();
    if (s.own.dirty && (force || now - s.own.written_at >= kWriteEvery)) write_file(own_key(), s.own, now);
    for (auto& [k, kept] : s.members)
        if (kept.dirty && (force || now - kept.written_at >= kWriteEvery)) write_file(k, kept, now);
}

Kept& own() {
    State& s = st();
    if (!s.loaded) {
        s.loaded = true;
        if (read_file(own_key(), &s.own.rec))
            NXLOG_I(kTag, "Co-op bags: this player's record %llu read", static_cast<unsigned long long>(s.own.rec.seq));
    }
    return s.own;
}

Kept& member(const Key& k) {
    auto [it, added] = st().members.try_emplace(k);
    if (added) read_file(k, &it->second.rec);
    return it->second;
}

// ---- the bag ----

// The bag the game's selector picks now (0 while there is none), with what it read.
Selection select(::Ctx* c) {
    Selection sel;
    sel.player = local_player() != 0;
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(r + layout::kSaveCommon, layout::kBagOffline + kBagBytes) || !guest::ok(gm, layout::kStateSession + 1)) return sel;
    sel.stage = guest::rd<uint8_t>(gm + layout::kStateStage);
    sel.own_bag = guest::rd<uint8_t>(gm + layout::kStateOwnBag) & 1;
    sel.visitor = guest::call(c, base() + layout::kOnline, 0) & 1;
    sel.online = sel.stage == layout::kStageCloudIsland ||
                 ((guest::call(c, base() + layout::kIsOnline, 0) & 1) && !sel.own_bag && sel.visitor);
    sel.bag = r + layout::kSaveCommon + (sel.online ? layout::kBagOnline : layout::kBagOffline);
    return sel;
}

// The bag may be read and written now: the player's character exists, no item is held in the bag screen (its temp copy would be
// copied back over ours), and every slot holds the slot vtable (the bag is made; *bad_slot the first that does not). Play mode's
// in_game is not asked: it is also false while a menu is open (no ProcessInput), and a craft or a box move there must be taken.
bool ready(const Selection& sel, size_t* bad_slot) {
    *bad_slot = kSlots;
    if (!sel.bag || !sel.player || guest::rd<uint8_t>(base() + layout::kItemTempFlag)) return false;
    for (size_t i = 0; i < kSlots; ++i)
        if (guest::rd<uint64_t>(sel.bag + i * items::kSlotSize) != base() + layout::kItemSlotVtable) return *bad_slot = i, false;
    return true;
}

// A record holds what a copy moves (items::kMoved) only, so a bag and its record compare equal right after a write.
void read_bag(uint64_t bag, Record* out) {
    for (size_t i = 0; i < kSlots; ++i)
        out->slot[i] = items::moved_of(guest::rd<uint64_t>(bag + i * items::kSlotSize + items::kSlotPayload));
}

void write_bag(uint64_t bag, const Record& r) {
    for (size_t i = 0; i < kSlots; ++i) {
        const uint64_t at = bag + i * items::kSlotSize + items::kSlotPayload;
        guest::wr<uint64_t>(at, items::moved_into(guest::rd<uint64_t>(at), r.slot[i]));
    }
}

// The record into this player's bag, and the HUD's item bar told (slot 0: a co-op process plays one player).
void put(::Ctx* c, uint64_t bag, const Record& r) {
    write_bag(bag, r);
    guest::call(c, base() + layout::kItemBarRefresh, 0, 0, 0);
}

bool screen_open() { return qpc::seconds() - st().screen_at < kScreenGap; }

// The authority: the mirror index of the member with this user id (its row in the member table), or -1.
int mirror_index(const UserId& user) {
    if (user == UserId{}) return -1;
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t own = guest::deref(guest::deref(base() + layout::kOwnMemberIndexRef));
    if (!guest::ok(r + layout::kBagMirror, layout::kMemberCount * kBagBytes) || !guest::ok(own + layout::kOwnMirrorIndex, 1)) return -1;
    const uint8_t own_idx = guest::rd<uint8_t>(own + layout::kOwnMirrorIndex);
    for (uint32_t i = 0; i < layout::kMemberCount; ++i) {
        UserId row;
        const uint64_t at = r + layout::kMemberTable + i * layout::kMemberSize + layout::kMemberUserId;
        if (i != own_idx && guest::read(at, row.data(), row.size()) && row == user) return static_cast<int>(i);
    }
    return -1;
}

// The authority: a member's record into its mirror (each slot's vtable and unmoved bits kept). The valid flag is left as it is
// (clear: the game's fill that sets it does not run in co-op).
void feed_mirror(int idx, const Record& r) {
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    write_bag(root + layout::kBagMirror + static_cast<uint64_t>(idx) * kBagBytes, r);
}

// What a change from `was` to `now` loses: whether any item's total fell, and how many item kinds have nothing left.
struct Loss {
    bool any = false;
    uint32_t kinds_gone = 0;
};
Loss loss_of(const Record& was, const Record& now) {
    std::map<uint32_t, int64_t> have;   // item id -> count before minus count after
    for (uint64_t p : was.slot)
        if (const uint32_t id = items::id_of(p)) have[id] += items::count_of(p);
    std::map<uint32_t, int64_t> before = have;
    for (uint64_t p : now.slot)
        if (const uint32_t id = items::id_of(p)) have[id] -= items::count_of(p);
    Loss l;
    for (const auto& [id, left] : have) {
        if (left <= 0) continue;
        l.any = true;
        if (left == before[id]) ++l.kinds_gone;
    }
    return l;
}

// ---- the messages ----

std::vector<uint8_t> slots_payload(wire::Writer& w, const Record& r) {
    const uint8_t zero[items::kSlotPayload] = {};
    for (uint64_t p : r.slot) w.bytes(zero, sizeof zero).u64(p);
    return w.take();
}

bool read_slots(wire::Reader& r, Record* out) {
    for (uint64_t& p : out->slot) {
        r.u64();   // the vtable's place: 0 on the wire
        p = items::moved_of(r.u64());
    }
    return r.ok();
}

void send_record() {
    State& s = st();
    wire::Writer w(link::self());
    UserId user{};
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (guest::ok(r + layout::kOwnUserId, user.size())) guest::read(r + layout::kOwnUserId, user.data(), user.size());
    w.bytes(member_key_bytes(), kMemberKeyBytes).u64(s.own.rec.seq).bytes(user.data(), user.size());
    if (link::to_authority(wire::Type::BagRecord, slots_payload(w, s.own.rec))) {
        s.send = false;
        s.sent_at = qpc::seconds();
    }
}

void send_restore(int node, const Key& k, const Record& r) {
    wire::Writer w(link::self());
    w.bytes(k.data(), k.size()).u8(r.seq ? 1 : 0).u64(r.seq);
    link::to_node(node, wire::Type::BagRestore, slots_payload(w, r));
}

// The authority: a member's record changed (or its user id became known): its mirror, if the member is in the member table, gets it.
// Latest wins: the host's own request checks change the mirror in between, and the member's next record carries the same change.
void on_member_record(const Key& k, const Kept& kept, int node) {
    const int idx = kept.rec.seq ? mirror_index(kept.user) : -1;
    if (idx >= 0) feed_mirror(idx, kept.rec);
    NXLOG_D(kTag, "Co-op bags: node %d's record %llu stored (%s), mirror %d", node, static_cast<unsigned long long>(kept.rec.seq),
            hex(k).c_str(), idx);
}

// ---- the tick's steps ----

void session_up() {
    State& s = st();
    Kept& o = own();
    s.active = true;
    s.answered = link::is_authority();
    s.hello_at = -1e9;
    s.send = false;
    s.apply = o.rec.seq != 0;
    s.grace_until = qpc::seconds() + kGrace;
    s.looked = false;
    s.up_at = qpc::seconds();
    s.was_ready = false;
    NXLOG_I(kTag, "Co-op bags: in a session as %s; this player's record %llu", s.answered ? "the authority" : "a member",
            static_cast<unsigned long long>(o.rec.seq));
}

void session_down() {
    State& s = st();
    flush(true);
    s.active = false;
    s.greeted = 0;
    NXLOG_I(kTag, "Co-op bags: out of the session; the records are written");
}

void take_or_refuse(::Ctx* c, const Selection& sel, double now) {
    State& s = st();
    Kept& o = s.own;
    Record bag;
    read_bag(sel.bag, &bag);
    if (o.rec.seq == 0) {   // no record anywhere: the bag as it is becomes the first one, once the selection has settled
        if (now < s.grace_until) return;
        bag.seq = 1;
        o.rec = bag;
        o.dirty = true;
        s.send = !link::is_authority();
        const auto [slots, count] = totals(o.rec);
        NXLOG_I(kTag, "Co-op bags: no record of this player anywhere; its bag becomes the first one (%u slots, %u items)", slots, count);
        return;
    }
    if (bag.slot == o.rec.slot) return;
    const Loss loss = loss_of(o.rec, bag);
    const auto [was_slots, was_count] = totals(o.rec);
    const auto [now_slots, now_count] = totals(bag);
    // Only inside the grace window: the game's whole-bag wipes (switched off in pkp_coop_bag.cpp; this catches one not mapped) run
    // at a load, an area or session change or a selection change, and the apply right before it puts the record back. In play, a
    // bulk loss is the player's own (a store-all into a box), and writing the record back then would copy the items.
    const char* why = nullptr;
    if (now >= s.grace_until) {
    } else if (was_slots && !now_slots) why = "it empties the bag right after a load, an area change or a session change";
    else if (loss.kinds_gone > kMaxKindsGone) why = "too many item kinds are gone right after a load, an area change or a session change";
    else if (loss.any) why = "it loses items right after a load, an area change or a session change";
    if (why) {
        put(c, sel.bag, o.rec);
        if (now - s.refused_log_at >= kRefuseLogEvery) {
            s.refused_log_at = now;
            NXLOG_W(kTag, "Co-op bags: a change of the bag is refused (%s): %u slots / %u items before, %u / %u after, %u kinds gone; "
                          "the record is written back", why, was_slots, was_count, now_slots, now_count, loss.kinds_gone);
        }
        return;
    }
    bag.seq = o.rec.seq + 1;
    o.rec = bag;
    o.dirty = true;
    s.send = !link::is_authority();
}

}  // namespace

void tick(::Ctx* c) {
    State& s = st();
    const bool in = c && link::in_session();
    if (in != s.active) {
        if (in) session_up();
        else session_down();
    }
    if (!in) return;
    const double now = qpc::seconds();
    const Selection sel = select(c);
    if (!s.looked || !(sel == s.seen)) {
        if (s.looked) {   // the first look of a session is session_up's
            s.apply = s.own.rec.seq != 0;
            s.grace_until = now + kGrace;
        }
        s.looked = true;
        s.seen = sel;
    }
    if (!s.answered && now - s.hello_at >= kHelloEvery) {
        s.hello_at = now;
        send_record();
    }
    // A member's bag is the online one once the host's world has loaded (the visitor test rises 10 to 20 s after the join,
    // pkp_coop_session.cpp); before that its selection is its own island's, which is neither applied to nor taken from.
    const bool settled = link::is_authority() || sel.visitor;
    size_t bad_slot = kSlots;
    const bool can = settled && ready(sel, &bad_slot);
    if (can) s.was_ready = true;
    else if (!s.was_ready && bad_slot < kSlots && now - s.up_at > kNotReadyLog) {
        s.was_ready = true;
        NXLOG_W(kTag, "Co-op bags: the %s bag is not usable (slot %zu holds no slot vtable); its record is neither applied nor taken",
                sel.online ? "online" : "offline", bad_slot);
    }
    // An apply waits while the bag / container screen is open, and the take with it (a change made meanwhile is the apply's to
    // overwrite: the record it brings is the newer one).
    if (can && s.apply && !screen_open()) {
        put(c, sel.bag, s.own.rec);
        s.apply = false;
        s.grace_until = now + kGrace;
        const auto [slots, count] = totals(s.own.rec);
        NXLOG_I(kTag, "Co-op bags: record %llu written into the %s bag (%u slots, %u items)",
                static_cast<unsigned long long>(s.own.rec.seq), sel.online ? "online" : "offline", slots, count);
    } else if (can && !s.apply && s.answered) {
        take_or_refuse(c, sel, now);
    }
    if (s.send && s.answered && !link::is_authority() && now - s.sent_at >= kSendEvery) send_record();
    flush(false);
}

void on_member_joined(int node) {
    if (node >= 0 && node < 32) st().greeted &= ~(1u << node);
}

int on_mirror_fill(::Ctx* c) {
    if (role() != CoopRole::Host) return 0;
    const int idx = static_cast<int>(c->x[0] & 0xff);
    for (const auto& [k, kept] : st().members) {
        if (!kept.rec.seq || mirror_index(kept.user) != idx) continue;
        feed_mirror(idx, kept.rec);
        const auto [slots, count] = totals(kept.rec);
        NXLOG_I(kTag, "Co-op bags: member %d joins; the host's copy of its bag is its record %llu (%u slots, %u items)", idx,
                static_cast<unsigned long long>(kept.rec.seq), slots, count);
        return 1;
    }
    NXLOG_I(kTag, "Co-op bags: member %d joins before its record is here; the host's copy of its bag waits for it", idx);
    return 1;   // the game's fill from its per-user table does not run in co-op
}

int on_bag_screen(::Ctx* c) {
    const uint64_t screen = c->x[0];
    if (guest::ok(screen, 8) && guest::rd<uint64_t>(screen) == base() + layout::kBagScreenVtable) st().screen_at = qpc::seconds();
    return 0;
}

void on_record_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    Key k;
    r.bytes(k.data(), k.size());
    Record got;
    got.seq = r.u64();
    UserId user;
    r.bytes(user.data(), user.size());
    if (!read_slots(r, &got) || !link::is_authority() || from < 0 || from >= 32) return;
    if (k == own_key()) {
        NXLOG_W(kTag, "Co-op bags: node %d sent a record under this player's own key; it is not kept", from);
        return;
    }
    State& s = st();
    Kept& m = member(k);
    const bool first = !(s.greeted & (1u << from));
    const bool newer = got.seq > m.rec.seq, new_user = user != m.user;
    m.user = user;
    if (newer) {
        m.rec = got;
        m.dirty = true;
    }
    if (newer || new_user) on_member_record(k, m, from);
    if (first || got.seq < m.rec.seq) {
        s.greeted |= 1u << from;
        send_restore(from, k, m.rec);
        if (first) {
            const auto [slots, count] = totals(m.rec);
            NXLOG_I(kTag, "Co-op bags: node %d joined with record %llu; it gets record %llu (%u slots, %u items)", from,
                    static_cast<unsigned long long>(got.seq), static_cast<unsigned long long>(m.rec.seq), slots, count);
        }
    }
}

void on_restore_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    Key k;
    r.bytes(k.data(), k.size());
    const uint8_t have = r.u8();
    Record got;
    got.seq = r.u64();
    if (!read_slots(r, &got) || link::is_authority() || k != own_key()) return;
    State& s = st();
    Kept& o = own();
    s.answered = true;
    if (have && got.seq > o.rec.seq) {
        o.rec = got;
        o.dirty = true;
        s.apply = true;
        NXLOG_I(kTag, "Co-op bags: the authority's copy of this player's bag is newer (record %llu); it is written into the bag",
                static_cast<unsigned long long>(got.seq));
    } else if (o.rec.seq > got.seq) {
        s.send = true;   // the authority's copy is older: ours goes to it
    }
}

void reset() {
    if (st().loaded) flush(true);
    st() = State{};
}

}  // namespace nxrt::games::pkp::coop::bags
