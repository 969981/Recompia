// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: seamless co-op's one-off request outcomes (pkp_coop_internal.h). Every member can hand a shared unique request in
// and keeps that hand-in (its own record, its own rewards, pkp_coop_requests.cpp); what the hand-in adds to the world is applied once
// per request for the whole group, in the authority's world.
//
// The game, 2.0.1: the completion step 0x618304(MU, slot, w3) of unique request id (slot = MU + id * 16, +2 u8 the id, +0 u16 the
// bound Pokémon's actor handle h) runs, after the record write (5) and its re-sort:
//   #6  0x618b00(CM)            CM = *(main + 0x3565170): +1 on every active "complete requests" town goal of the current area (it
//                               never reads the id: any completion counts). Accumulates: one +1 per run.
//   #8  0x617d98(&h, 7)         row +0x5d == 7: ends the escort (a member's run sends RequestPartyNPC; the authority's own run is
//                               the owner). Not replayed: the member's run already moved the party, a repeat changes nothing.
//   #9  0x6191e8(&h, +0x40, f)  row +0x5d == 0x14: the Pokémon moves in (guarded by 0x616dbc: a no-op once it lives in a matching
//                               house). On a member: its mirror only.
//   #10 0x11752c(+0x2a)         row +0x2a in 1..0x1f4: clears the pop flags of the placed objects tied to +0x2a, unless 0x117320
//                               finds a Pokémon whose request component holds an id in state 3/4 with the same +0x2a. Idempotent.
//   #11 0x619410(&h)            always: friendship (friend points, friend level u16 +0x2e of the per-chara 0x180 record in CMNDAT).
//                               Accumulates. On a member the level store returns early and the points land in its mirror.
//   #12 0x13a9b0(&h)            id 73 only: a hint bit and one effect-command slot. Idempotent.
//   #13 the general entry       row +0x5d == 1 or 0x39: chara = request component +0xc; +0x22 = row +0x40, +0x24 = 0, +0x26 = the
//                               date, in the entry the 3-way select picks (0x77ecb0: R + 0xc81488, else 0x376c4: R + 0x16f2af8, else
//                               R + 0x22fef0; + chara * 0x30 while chara < 0x41a). Idempotent but the date.
// (Listing work/requests-re/l_618304.txt; RE notes modding-docs/coop-v2/REQUESTS-RE.md U11.)
//
// The two accumulating ones (#6, #11) are what "once for the group" is about. Two hook sites (pkp_coop.cpp) steer them in the game's
// own step, one decision per run:
//   midhook 0x61858c  `cbz x0, 0x618594` right before `bl 0x618b00`: x0 = CM, x19 = the slot. x0 = 0 skips #6 (x0 is not read after
//                     0x618594: 0x618dd0 loads its own). Every run that reaches #11 passes here first (0x618520..0x61858c is straight
//                     line), so the decision for the run is made here and kept for #11.
//   entry 0x619410    only when x30 is main + 0x618710 or + 0x618758 (its two calls in 0x618304; 0x118bc4 and 0x621e54 also call
//                     it and are left alone): x19 is still the caller's slot at entry. Returning 1 returns at once (void; nothing
//                     else to restore). A midhook cannot skip one call (nonzero returns the whole function, nxrt/hooks.h), hence the
//                     entry hook.
// Rules:
//   - a member (visit role, in the group's world) skips #6 and #11 on every run and reports the id to the authority (ReqOutcome).
//     Its world effects would land in its mirrors (or, #6, likely be forwarded by the game: skipped so a forward never counts twice);
//   - the authority's own run applies #6 and #11 the first time for the group (the ledger bit is clear) and sets the bit; a later
//     run (any player) skips them. Its idempotent pieces run every time, as the game does;
//   - a member's report on the authority, the bit clear: the authority replays the world part in its own world once, at a safe
//     moment (it can walk, talks to no one, the Pokémon is bound and loaded here), in the step's order: #6, #9, #10, #11, #12, #13,
//     then sets the bit. Its own record and request component are left as they are (option B: the host keeps its own hand-in);
//     for #10 the component's id is cleared for that one call and put back, or 0x117320 sees this Pokémon's open request and
//     clears nothing. Not replayed: #7 (the gift: personal), #8 (above), the play report, the UI trigger and the follow-up offer;
//   - a report of an id whose bit is set is dropped; a non-story id whose record is 5 on the authority (its own hand-in, maybe
//     from before this ledger) only sets the bit. A story id's 5 there proves nothing (the share closes story ids with a raw 5);
//   - the authority's own run while a replay waits does the world part itself: the replay is dropped;
//   - ids the game binds to one player (requests::own_request) are not touched: the game's own step runs as it does.
// The ledger lives in the authority's save, so a later session does not apply the world part again: the unused elements of the
// record array (256 of 4 bytes, ids 1..200 used; every reader checks id - 1 <= 0xc7, U1, and the file is the flat CMNDAT image,
// elements 201..255 seen at their default 01 01 00 00 in a real save): element 201 holds kLedgerMagic, 202..208 hold one bit per
// id (bit (id - 1) & 7 of byte (id - 1) / 8). Members never write it.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_tables.h"

#include <cstring>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::outcomes {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint32_t kIdMax = layout::kUniqueRequestCount;     // ids 1..200
constexpr uint8_t kDone = 5;
constexpr uint32_t kRecordCount = 256;                        // elements of the record array (4 bytes each)
constexpr uint32_t kLedgerMagicAt = 201, kLedgerBitsAt = 202;   // elements: the ledger's mark, then its bits (25 bytes)
constexpr uint32_t kLedgerMagic = 0x706f6f63;                 // "coop"; the default element is 01 01 00 00
constexpr size_t kLedgerBytes = 7 * 4;                         // elements 202..208: ids 1..200, one bit each

// The completion step's two calls of 0x619410 return here (main +).
constexpr uint64_t kFriendshipFrom[] = {0x618710, 0x618758};
constexpr uint64_t kSlotId = 2;   // slot + : u8 the id

// The world pieces of the step (main +), called as 0x618304 calls them.
constexpr uint64_t kChallengeMgr = 0x3565170;   // main + : CM
constexpr uint64_t kGoalCount = 0x618b00;       // (CM)
constexpr uint64_t kMoveIn = 0x6191e8;          // (&h, u16 row +0x40, &f[6])
constexpr uint64_t kPopFlags = 0x11752c;        // (u16 row +0x2a)
constexpr uint64_t kFriendship = 0x619410;      // (&h)
constexpr uint64_t kBulbasaur = 0x13a9b0;       // (&h), id 73
constexpr uint32_t kBulbasaurId = 0x49;
constexpr uint64_t kStage11 = 0x77ecb0, kOnline = layout::kOnline;   // () -> bit 0: the general entry's select
// The request row (#1557): the data-table manager's table at 0x1160 (slot 0x22c, the table at +0 while the manager holds fewer),
// row object +8 -> the row, or the default row *(main + 0x34845e8) when that is 0 (0x6184e8..0x618500).
constexpr uint64_t kRowTable = 0x1160;
constexpr uint64_t kDefaultRow = 0x34845e8;
constexpr uint64_t kRowGoal = 0x2a, kRowKind = 0x40, kRowType = 0x5d;   // u16 pop-flag key, u16 kind, u8 type
constexpr uint64_t kRowHouse[6] = {0xc, 0x10, 0x14, 0x0, 0x4, 0x8};     // f[0..5] = (u16) row + these (0x618654..0x618674)
constexpr uint8_t kTypeMoveIn = 0x14, kTypeEntryA = 1, kTypeEntryB = 0x39;
// The general entry (#13).
constexpr uint64_t kEntryStage11 = 0xc81488, kEntryMirror = 0x16f2af8, kEntryOwn = 0x22fef0;   // save root +
constexpr uint32_t kEntryCharaMax = 0x41a;
constexpr uint64_t kEntrySize = 0x30, kEntryItem = 0x22, kEntryFlag = 0x24, kEntryDate = 0x26;
constexpr uint64_t kDate = 0x13d3640;   // save root + : u32 the date the entry keeps
constexpr uint64_t kCompChara = 0xc;    // request component + : u32 the Pokémon's chara
constexpr uint64_t kCompId = 0x10;      // request component + : u8 the id it holds
// Logged only: the friend level (per-chara 0x180 record, CMNDAT) and the goals' pending count.
constexpr uint64_t kFriendRecords = 0x120, kFriendSize = 0x180, kFriendLevel = 0x2e;   // save root +
constexpr uint64_t kGoalPending = 0x18030;                                             // CM + : u64

enum Wait : uint8_t {
    kReport = 1,   // a member: this id's completion is still to be sent to the authority
    kReplay = 2,   // the authority: a member finished this id first; the world part waits for a safe moment
};

struct State {
    uint8_t wait[kIdMax + 1] = {};     // per id: Wait bits
    uint8_t origin[kIdMax + 1] = {};   // per id: the node whose report waits for the replay
    bool logged[kIdMax + 1] = {};      // per id: the replay's wait was logged
    uint32_t run_id = 0;               // the completion step that passed the midhook last (0: none) and its decision
    bool run_skip = false;
    bool replaying = false;            // our own calls of the pieces: the hooks pass
    // The pieces' guest-readable arguments (guest addresses are host addresses).
    alignas(8) uint16_t handle_arg = 0;
    alignas(8) uint16_t house_arg[6] = {};
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }

uint64_t records() {
    const uint64_t r = save_root();
    return r && guest::ok(r + layout::kSaveUniqueRequests, kRecordCount * 4) ? r + layout::kSaveUniqueRequests : 0;
}

bool member_in_world() { return role() == CoopRole::Visit && (session_bits() & layout::kSessionVisitor); }

bool done(uint32_t id) {
    const uint64_t recs = records();
    if (!recs || guest::rd<uint32_t>(recs + kLedgerMagicAt * 4) != kLedgerMagic) return false;
    return guest::rd<uint8_t>(recs + kLedgerBitsAt * 4 + (id - 1) / 8) >> ((id - 1) & 7) & 1;
}

void set_done(uint32_t id) {
    const uint64_t recs = records();
    if (!recs) return;
    if (guest::rd<uint32_t>(recs + kLedgerMagicAt * 4) != kLedgerMagic) {   // first use in this save: an empty ledger
        std::memset(reinterpret_cast<void*>(recs + kLedgerBitsAt * 4), 0, kLedgerBytes);
        guest::wr<uint32_t>(recs + kLedgerMagicAt * 4, kLedgerMagic);
    }
    const uint64_t at = recs + kLedgerBitsAt * 4 + (id - 1) / 8;
    guest::wr<uint8_t>(at, static_cast<uint8_t>(guest::rd<uint8_t>(at) | 1u << ((id - 1) & 7)));
}

// The id of the slot in x19 (both sites), or 0 when it is not a shared unique request.
uint32_t slot_id(::Ctx* c) {
    const uint64_t slot = c->x[19];
    if (!guest::ok(slot, kSlotId + 1)) return 0;
    const uint32_t id = guest::rd<uint8_t>(slot + kSlotId);
    return id >= 1 && id <= kIdMax && !requests::own_request(id) ? id : 0;
}

// Decides one completion step's #6 and #11 (true: skip both) and does the bookkeeping that goes with the decision.
bool decide(uint32_t id) {
    State& s = st();
    if (member_in_world()) {
        s.wait[id] |= kReport;
        NXLOG_I(kTag, "Co-op outcomes: request %u finished here; its town goal and friendship are the authority's, reported", id);
        return true;
    }
    if (role() != CoopRole::Host) return false;
    if (done(id)) {
        NXLOG_I(kTag, "Co-op outcomes: request %u finished here; the group's town goal and friendship were applied before, skipped", id);
        return true;
    }
    set_done(id);
    if (s.wait[id] & kReplay) NXLOG_I(kTag, "Co-op outcomes: request %u finished here first; node %u's replay is dropped", id, s.origin[id]);
    s.wait[id] &= ~kReplay;
    NXLOG_I(kTag, "Co-op outcomes: request %u finished here; town goal and friendship applied for the group", id);
    return false;
}

uint16_t friend_level(uint64_t comp) {
    const uint64_t r = save_root();
    const uint32_t chara = comp ? guest::rd<uint32_t>(comp + kCompChara) : 0;
    const uint64_t at = r + kFriendRecords + chara * kFriendSize + kFriendLevel;
    return chara && chara < kEntryCharaMax && guest::ok(at, 2) ? guest::rd<uint16_t>(at) : 0;
}

uint64_t goal_pending(uint64_t cm) { return cm && guest::ok(cm + kGoalPending, 8) ? guest::rd<uint64_t>(cm + kGoalPending) : 0; }

// #13: the three general-entry stores, with the entry the game's select picks on this process.
void general_entry(::Ctx* c, uint64_t comp, uint16_t kind) {
    const uint64_t r = save_root();
    uint64_t entry = r + kEntryOwn;
    if (guest::call(c, base() + kStage11, 0) & 1) entry = r + kEntryStage11;
    else if (guest::call(c, base() + kOnline, 0) & 1) entry = r + kEntryMirror;
    const uint32_t chara = guest::rd<uint32_t>(comp + kCompChara);
    if (chara < kEntryCharaMax) entry += chara * kEntrySize;
    if (!guest::ok(entry + kEntryDate, 4) || !guest::ok(r + kDate, 4)) return;
    guest::wr<uint16_t>(entry + kEntryItem, kind);
    guest::wr<uint8_t>(entry + kEntryFlag, 0);
    guest::wr<uint32_t>(entry + kEntryDate, guest::rd<uint32_t>(r + kDate));
}

// The world part of request id's completion on the authority (the rules above); false: not now (kept).
bool replay(::Ctx* c, uint32_t id) {
    State& s = st();
    const uint64_t slot = requests::request_slot(id);
    const uint64_t comp = slot ? requests::request_component(c, slot) : 0;
    if (!comp) return false;   // the Pokémon is not bound and loaded here (yet)
    const uint64_t obj = tables::row_or_first(c, base(), kRowTable, id);
    if (!obj || !guest::ok(obj + 8, 8)) return false;   // no table or row: the step itself returns before its world part (0x6184e4)
    uint64_t row = guest::rd<uint64_t>(obj + 8);
    if (!row) row = guest::rd<uint64_t>(base() + kDefaultRow);
    if (!guest::ok(row, kRowType + 1)) return false;
    const uint8_t type = guest::rd<uint8_t>(row + kRowType);
    const uint16_t goal = guest::rd<uint16_t>(row + kRowGoal), kind = guest::rd<uint16_t>(row + kRowKind);
    const uint64_t cm = guest::rd<uint64_t>(base() + kChallengeMgr);
    const uint16_t level = friend_level(comp);
    const uint64_t pending = goal_pending(cm);
    s.handle_arg = guest::rd<uint16_t>(slot);
    const uint64_t h = reinterpret_cast<uint64_t>(&s.handle_arg);
    s.replaying = true;
    if (cm) guest::call(c, base() + kGoalCount, cm);
    if (type == kTypeMoveIn) {
        for (size_t k = 0; k < 6; ++k) s.house_arg[k] = guest::rd<uint16_t>(row + kRowHouse[k]);
        guest::call(c, base() + kMoveIn, h, kind, reinterpret_cast<uint64_t>(s.house_arg));
    }
    if (goal >= 1 && goal <= 0x1f4) {
        // 0x117320 would find this Pokémon's own open request (the host keeps it, option B) and clear nothing: its id is let go
        // for this one call, as the step has let it go by then (0x6183b0), and put back.
        const uint8_t held = guest::rd<uint8_t>(comp + kCompId);
        guest::wr<uint8_t>(comp + kCompId, 0);
        guest::call(c, base() + kPopFlags, goal);
        guest::wr<uint8_t>(comp + kCompId, held);
    }
    s.handle_arg = guest::rd<uint16_t>(slot);
    guest::call(c, base() + kFriendship, h);
    if (id == kBulbasaurId) {
        s.handle_arg = guest::rd<uint16_t>(slot);
        guest::call(c, base() + kBulbasaur, h);
    }
    if (type == kTypeEntryA || type == kTypeEntryB) general_entry(c, comp, kind);
    s.replaying = false;
    set_done(id);
    NXLOG_I(kTag, "Co-op outcomes: request %u (type 0x%x) finished first on node %u: its world part ran here; friend level %u -> %u, "
            "town goal pending %llu -> %llu", id, type, s.origin[id], level, friend_level(comp),
            static_cast<unsigned long long>(pending), static_cast<unsigned long long>(goal_pending(cm)));
    return true;
}

bool safe_now(bool in_game) { return session::can_walk(in_game) && !talk::talking(); }

}  // namespace

int on_goal_count(::Ctx* c) {
    State& s = st();
    s.run_id = 0;
    if (s.replaying) return 0;
    const uint32_t id = slot_id(c);
    if (!id) return 0;
    s.run_id = id;
    s.run_skip = decide(id);
    if (s.run_skip) c->x[0] = 0;
    return 0;
}

int on_friendship(::Ctx* c) {
    State& s = st();
    if (s.replaying) return 0;
    const uint64_t from = c->x[30] - base();
    if (from != kFriendshipFrom[0] && from != kFriendshipFrom[1]) return 0;
    const uint32_t id = slot_id(c);
    const bool skip = id && id == s.run_id && s.run_skip;
    s.run_id = 0;
    return skip ? 1 : 0;
}

void on_outcome_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t id = r.u8();
    if (!r.ok() || role() != CoopRole::Host || id < 1 || id > kIdMax || requests::own_request(id)) return;
    const uint64_t recs = records();
    if (!recs) return;
    if (done(id)) {
        NXLOG_I(kTag, "Co-op outcomes: node %u finished request %u; the group's world part was applied before", origin, id);
        return;
    }
    if (!requests::story_request(id) && guest::rd<uint8_t>(recs + id * 4) == kDone) {
        set_done(id);
        NXLOG_I(kTag, "Co-op outcomes: node %u finished request %u; this player finished it before, nothing to apply", origin, id);
        return;
    }
    s.wait[id] |= kReplay;
    s.origin[id] = origin;
    s.logged[id] = false;
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c || !records()) return;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        if ((s.wait[id] & kReport) && link::to_authority(wire::Type::ReqOutcome, wire::Writer(link::self()).u8(static_cast<uint8_t>(id)).take()))
            s.wait[id] &= ~kReport;
    }
    if (role() != CoopRole::Host || !safe_now(in_game)) return;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        if (!(s.wait[id] & kReplay)) continue;
        if (replay(c, id)) {
            s.wait[id] &= ~kReplay;
            return;   // one a frame
        }
        if (!s.logged[id])
            NXLOG_I(kTag, "Co-op outcomes: request %u finished on node %u waits for its Pokémon to be loaded here", id, s.origin[id]);
        s.logged[id] = true;
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::outcomes
