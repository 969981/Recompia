// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's one-off request outcomes (pkp_coop_internal.h). A shared unique request is handed in once for the
// group (the repo owner's call, 2026-11-20): the first hand-in closes it for every member (pkp_coop_requests.cpp), and what the
// hand-in adds to the world is applied once per request for the whole group, in the authority's world.
//
// The game, 2.0.1: the completion step 0x618304(MU, slot, w3) of unique request id (slot = MU + id * 16, +2 u8 the id, +0 u16 the
// bound creature's actor handle h) runs, after the record write (5) and its re-sort:
//   #6  0x618b00(CM)            CM = *(main + 0x3565170): +1 on every active "complete requests" town goal of the current area (it
//                               never reads the id: any completion counts). Accumulates: one +1 per run. On a member the +1
//                               lands in its own CM and the game sends it to the authority the next frame (CM frame 0x631020 ->
//                               0x633150 on a visitor: RequestChallengeProgress {goal id, amount}; the authority's handler
//                               0x54fbc0 adds it with 0x54fd60, no area test there).
//   #8  0x617d98(&h, 7)         row +0x5d == 7: ends the escort (a member's run sends RequestPartyNPC; the authority's own run is
//                               the owner). Not replayed: the member's run already moved the party, a repeat changes nothing.
//   #9  0x6191e8(&h, +0x40, f)  row +0x5d == 0x14: the creature moves in (guarded by 0x616dbc: a no-op once it lives in a matching
//                               house). On a member: its mirror only.
//   #10 0x11752c(+0x2a)         row +0x2a in 1..0x1f4: clears the pop flags of the placed objects tied to +0x2a, unless 0x117320
//                               finds a creature whose request component holds an id in state 3/4 with the same +0x2a. Idempotent.
//   #11 0x619410(&h)            always: friendship (friend points, friend level u16 +0x2e of the per-chara 0x180 record in CMNDAT).
//                               Accumulates. On a member the level store returns early and the points land in its mirror.
//   #12 0x13a9b0(&h)            id 73 only: a hint bit and one effect-command slot. Idempotent.
//   #13 the general entry       row +0x5d == 1 or 0x39: chara = request component +0xc; +0x22 = row +0x40, +0x24 = 0, +0x26 = the
//                               date, in the entry the 3-way select picks (0x77ecb0: R + 0xc81488, else 0x376c4: R + 0x16f2af8, else
//                               R + 0x22fef0; + chara * 0x30 while chara < 0x41a). Idempotent but the date.
//
// The two accumulating ones (#6, #11) are what the rules below steer: #11 once for the group, #6 on the authority once for the
// group and on each member's own hand-in (the game forwards it). Two hook sites (pkp_coop.cpp) steer them in the game's own step,
// one decision per run:
//   midhook 0x61858c  `cbz x0, 0x618594` right before `bl 0x618b00`: x0 = CM, x19 = the slot. x0 = 0 skips #6 (x0 is not read after
//                     0x618594: 0x618dd0 loads its own). Every run that reaches #11 passes here first (0x618520..0x61858c is straight
//                     line), so the decision for the run is made here and kept for #11.
//   entry 0x619410    only when x30 is main + 0x618710 or + 0x618758 (its two calls in 0x618304; 0x118bc4 and 0x621e54 also call
//                     it and are left alone): x19 is still the caller's slot at entry. Returning 1 returns at once (void; nothing
//                     else to restore). A midhook cannot skip one call (nonzero returns the whole function, nxrt/hooks.h), hence the
//                     entry hook.
// Rules:
//   - a member (visit role, in the group's world) runs #6 itself: the game's own forward (above) counts it on the authority at
//     once, for the goal of the area the member is in. It skips #11 on every run (on a visitor the level store returns early and
//     the points land in its mirror) and reports the id to the authority (ReqOutcome);
//   - the authority's own run applies #6 and #11 the first time for the group (the ledger bit is clear) and sets the bit; a later
//     run (any player) skips them. Its idempotent pieces run every time, as the game does;
//   - a member's report on the authority: the request is closed here (requests::close: the record 5, the creature's component let
//     go, its bubble off), which every member then closes too. The bit clear: the authority replays the world part in its own
//     world once, at a safe moment (it can walk, talks to no one, the creature is bound and loaded here), in the step's order: #9,
//     #10, #11, #12, #13 (not #6: the member's run counted it), then sets the bit;
//     for #10 the component's id is cleared for that one call and put back, or 0x117320 sees this creature's open request and
//     clears nothing. Not replayed: #7 (the gift: personal), #8 (above), the play report, the UI trigger and the follow-up offer;
//   - a report of an id whose bit is set, or whose replay already waits, adds nothing; a non-story id whose record is 5 on the
//     authority when no replay waits (its own hand-in, maybe from before this ledger) only sets the bit. Once a replay waits, the
//     record's 5 is the close's own, so it proves nothing then (nor a story id's 5 ever: a member closes story ids with a raw 5);
//   - the authority's own run while a replay waits does the world part itself: the replay is dropped (its #6 then counts once more
//     than the member's: each hand-in counts);
//   - a waiting replay is kept in the authority's save too (below), so a session that ends before the creature is loaded there
//     replays it in a later one;
//   - ids the game binds to one player (requests::own_request) are not touched: the game's own step runs as it does.
// The ledger lives in the authority's save, so a later session does not apply the world part again: the unused elements of the
// record array (256 of 4 bytes, ids 1..200 used; every reader checks id - 1 <= 0xc7, and the file is the flat CMNDAT image,
// elements 201..255 seen at their default 01 01 00 00 in a real save): element 201 holds kLedgerMagic, 202..208 hold one bit per
// id (bit (id - 1) & 7 of byte (id - 1) / 8). Element 240 holds kPendingMagic, 241..247 one bit per id whose replay waits (the
// same packing; 209..212 are pkp_coop_purchases.cpp's). Members never write either.
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
constexpr uint32_t kPendingMagicAt = 240;                      // element: the waiting replays' mark, then their bits (241..247)
constexpr uint32_t kPendingMagic = 0x646e6570;                 // "pend"

// The completion step's two calls of 0x619410 return here (main +).
constexpr uint64_t kFriendshipFrom[] = {0x618710, 0x618758};
constexpr uint64_t kSlotId = 2;   // slot + : u8 the id

// The world pieces of the step (main +), called as 0x618304 calls them (#6, 0x618b00, is the member's own run's).
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
constexpr uint64_t kCompChara = 0xc;    // request component + : u32 the creature's chara
constexpr uint64_t kCompId = 0x10;      // request component + : u8 the id it holds
// Logged only: the friend level (per-chara 0x180 record, CMNDAT).
constexpr uint64_t kFriendRecords = 0x120, kFriendSize = 0x180, kFriendLevel = 0x2e;   // save root +

// One completion step's decision (true: skipped).
struct Decision {
    bool goal = false;         // #6
    bool friendship = false;   // #11
};

struct State {
    State() { std::memset(origin, wire::kNobody, sizeof origin); }
    bool report[kIdMax + 1] = {};      // a member: per id, the completion is still to be sent to the authority
    uint8_t origin[kIdMax + 1];        // the authority: per id, the node whose report waits for the replay (kNobody: from the save)
    bool logged[kIdMax + 1] = {};      // per id: the replay's wait was logged
    uint32_t run_id = 0;               // the completion step that passed the midhook last (0: none) and its decision
    Decision run;
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

// A bit set in the save's record array: its mark at element `mark` (magic), its bits from element mark + 1.
bool save_bit(uint32_t mark, uint32_t magic, uint32_t id) {
    const uint64_t recs = records();
    if (!recs || guest::rd<uint32_t>(recs + mark * 4) != magic) return false;
    return guest::rd<uint8_t>(recs + (mark + 1) * 4 + (id - 1) / 8) >> ((id - 1) & 7) & 1;
}

void set_save_bit(uint32_t mark, uint32_t magic, uint32_t id, bool on) {
    const uint64_t recs = records();
    if (!recs) return;
    if (guest::rd<uint32_t>(recs + mark * 4) != magic) {   // first use in this save: an empty set
        if (!on) return;
        std::memset(reinterpret_cast<void*>(recs + (mark + 1) * 4), 0, kLedgerBytes);
        guest::wr<uint32_t>(recs + mark * 4, magic);
    }
    const uint64_t at = recs + (mark + 1) * 4 + (id - 1) / 8;
    const uint8_t bit = static_cast<uint8_t>(1u << ((id - 1) & 7)), was = guest::rd<uint8_t>(at);
    guest::wr<uint8_t>(at, static_cast<uint8_t>(on ? was | bit : was & ~bit));
}

static_assert(kLedgerBitsAt == kLedgerMagicAt + 1, "the ledger's bits follow its mark");
bool done(uint32_t id) { return save_bit(kLedgerMagicAt, kLedgerMagic, id); }
void set_done(uint32_t id) { set_save_bit(kLedgerMagicAt, kLedgerMagic, id, true); }
// The authority: a member finished id first and its world part waits for a safe moment here.
bool pending(uint32_t id) { return save_bit(kPendingMagicAt, kPendingMagic, id); }
void set_pending(uint32_t id, bool on) { set_save_bit(kPendingMagicAt, kPendingMagic, id, on); }

// The id of the slot in x19 (both sites), or 0 when it is not a shared unique request.
uint32_t slot_id(::Ctx* c) {
    const uint64_t slot = c->x[19];
    if (!guest::ok(slot, kSlotId + 1)) return 0;
    const uint32_t id = guest::rd<uint8_t>(slot + kSlotId);
    return id >= 1 && id <= kIdMax && !requests::own_request(id) ? id : 0;
}

// Decides one completion step's #6 and #11 and does the bookkeeping that goes with the decision.
Decision decide(uint32_t id) {
    State& s = st();
    if (member_in_world()) {
        s.report[id] = true;
        NXLOG_I(kTag, "Co-op outcomes: request %u finished here; its town goal is left to the game (it forwards it to the authority), "
                "its friendship is the authority's, reported", id);
        return {false, true};
    }
    if (role() != CoopRole::Host) return {};
    if (done(id)) {
        NXLOG_I(kTag, "Co-op outcomes: request %u finished here; the group's town goal and friendship were applied before, skipped", id);
        return {true, true};
    }
    set_done(id);
    if (pending(id)) NXLOG_I(kTag, "Co-op outcomes: request %u finished here first; node %u's replay is dropped", id, s.origin[id]);
    set_pending(id, false);
    NXLOG_I(kTag, "Co-op outcomes: request %u finished here; town goal and friendship applied for the group", id);
    return {};
}

uint16_t friend_level(uint64_t comp) {
    const uint64_t r = save_root();
    const uint32_t chara = comp ? guest::rd<uint32_t>(comp + kCompChara) : 0;
    const uint64_t at = r + kFriendRecords + chara * kFriendSize + kFriendLevel;
    return chara && chara < kEntryCharaMax && guest::ok(at, 2) ? guest::rd<uint16_t>(at) : 0;
}

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
    if (!comp) return false;   // the creature is not bound and loaded here (yet)
    const uint64_t obj = tables::row_or_first(c, base(), kRowTable, id);
    if (!obj || !guest::ok(obj + 8, 8)) return false;   // no table or row: the step itself returns before its world part (0x6184e4)
    uint64_t row = guest::rd<uint64_t>(obj + 8);
    if (!row) row = guest::rd<uint64_t>(base() + kDefaultRow);
    if (!guest::ok(row, kRowType + 1)) return false;
    const uint8_t type = guest::rd<uint8_t>(row + kRowType);
    const uint16_t goal = guest::rd<uint16_t>(row + kRowGoal), kind = guest::rd<uint16_t>(row + kRowKind);
    const uint16_t level = friend_level(comp);
    s.handle_arg = guest::rd<uint16_t>(slot);
    const uint64_t h = reinterpret_cast<uint64_t>(&s.handle_arg);
    s.replaying = true;
    if (type == kTypeMoveIn) {
        for (size_t k = 0; k < 6; ++k) s.house_arg[k] = guest::rd<uint16_t>(row + kRowHouse[k]);
        guest::call(c, base() + kMoveIn, h, kind, reinterpret_cast<uint64_t>(s.house_arg));
    }
    if (goal >= 1 && goal <= 0x1f4) {
        // 0x117320 would find this creature's own open request (the host keeps it) and clear nothing: its id is let go
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
    NXLOG_I(kTag, "Co-op outcomes: request %u (type 0x%x) finished first on node %u: its world part ran here (the town goal was the "
            "member's); friend level %u -> %u", id, type, s.origin[id], level, friend_level(comp));
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
    s.run = decide(id);
    if (s.run.goal) c->x[0] = 0;
    return 0;
}

int on_friendship(::Ctx* c) {
    State& s = st();
    if (s.replaying) return 0;
    const uint64_t from = c->x[30] - base();
    if (from != kFriendshipFrom[0] && from != kFriendshipFrom[1]) return 0;
    const uint32_t id = slot_id(c);
    const bool skip = id && id == s.run_id && s.run.friendship;
    s.run_id = 0;
    return skip ? 1 : 0;
}

void on_outcome_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t id = r.u8();
    if (!r.ok() || role() != CoopRole::Host || id < 1 || id > kIdMax || requests::own_request(id)) return;
    const uint64_t recs = records();
    if (!recs) return;
    requests::close(id, origin);   // the group's hand-in: closed for every member
    if (done(id)) {
        NXLOG_I(kTag, "Co-op outcomes: node %u finished request %u; the group's world part was applied before", origin, id);
        return;
    }
    if (pending(id)) {
        NXLOG_I(kTag, "Co-op outcomes: node %u finished request %u too; its world part waits already", origin, id);
        return;
    }
    if (!requests::story_request(id) && guest::rd<uint8_t>(recs + id * 4) == kDone) {
        set_done(id);
        NXLOG_I(kTag, "Co-op outcomes: node %u finished request %u; this player finished it before, nothing to apply", origin, id);
        return;
    }
    set_pending(id, true);
    s.origin[id] = origin;
    s.logged[id] = false;
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c || !records()) return;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        if (s.report[id] && link::to_authority(wire::Type::ReqOutcome, wire::Writer(link::self()).u8(static_cast<uint8_t>(id)).take()))
            s.report[id] = false;
    }
    // Replays only in a session (a waiting one kept in the save waits for the next session).
    if (role() != CoopRole::Host || !(session_bits() & layout::kSessionOnline) || !safe_now(in_game)) return;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        if (!pending(id)) continue;
        if (done(id) || requests::own_request(id)) {   // applied meanwhile (or no longer shared): nothing waits
            set_pending(id, false);
            continue;
        }
        if (replay(c, id)) {
            set_pending(id, false);
            return;   // one a frame
        }
        if (!s.logged[id])
            NXLOG_I(kTag, "Co-op outcomes: request %u finished on node %u waits for its creature to be loaded here", id, s.origin[id]);
        s.logged[id] = true;
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::outcomes
