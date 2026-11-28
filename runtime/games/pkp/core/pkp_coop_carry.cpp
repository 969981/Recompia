// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's follower carry (pkp_coop_internal.h): an invited follower who can come along crosses to the next
// area with its leader, as it does offline. RE: modding-docs coop-v2/wave-1127/RIDGES-RE.md sections 5 and 7.
//
// Offline the game carries them itself. The trip executor 0xbcde4 writes each follower that passes the "can come" test 0x4b1558
// (data id 0x2c always) into a table on the save root (R + 0x7400f4, 5 rows of 0x14: u16 data id, +2 the 16 bytes 0x284d20 gives for
// the creature, +0x12 0x286c48 & 1); on arrival 0x6571a0 spawns each with 0x658028 by its leader and invites it (0x657a20). Both
// sides are offline only (an online host skips the write after 0x472460, the arrival tests 0x207b0 / 0xb62e4), and the game clears
// the party lists at the stage change without a leave (0xb6320 -> 0x655dc0). So in a session no invited follower ever crossed: the
// group's move released them (pkp_coop_follow.cpp release_for_move), a host alone kept them attached up to the stage change (the
// filer 0xbd5a0 leaves only the ones that cannot come), and the departure callbacks' online rule refused every town creature
// (pkp_coop_party_table.inc, cb 0x4e / 0x50).
// Here the authority keeps the same table itself:
//   The record: every slot's invited list (party manager lists 0..3, by session slot) while the authority's player walks in an
//   area and no move runs; for each creature that is no escort (the escort record re-attaches those), passes 0x4b1558 and has the
//   16 bytes the game's write keeps (not the "none" value at main + kNoParents): data id, leader slot, the 16 bytes, the 0x286c48
//   byte. Taken again whenever the lists change, every kRefresh, and right before the group's move releases them. Frozen while a
//   move runs, a talk or event runs, or the player cannot walk, so it holds the followers of the last moment the group walked.
//   The arrival: the area stage changed. Once the authority's player walks again, each recorded creature is attached to its
//   leader's player here: the live creature when this stage has one of that data id, else one spawned by the leader with
//   0x658028 in the offline carry's mode (0, or 2 for a creature whose 0x286c48 byte is set: 0x6576b8), then the game's invite
//   0x657a20(party manager, &leader, &creature, 0, 0) and the follow module's accept (the authority's party tick 0x656200 asks
//   "will it come" 0x16570 of every new follower and drops it on a no). An entry whose leader's player is not here, or whose
//   attach did not take, is tried again every kRetry until kArriveWait.
// On a member a spawn is a request to the host (0x2c2480 with the session's net mode set sends it and returns no actor), which is
// why only the authority carries; the creature it spawns is the host's, as every creature of the group's world is.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::carry {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kCanCome = 0x4b1558;     // (&u16 actor id) -> bit 0: the creature can come along (the trip's own test)
constexpr uint64_t kParentsOf = 0x284d20;   // (&u16 actor id) -> the creature's 16 bytes the carry keeps (never null)
constexpr uint64_t kNoParents = 0x3561b40;  // main + : the 16 bytes of "none"; the game's write skips a creature that has them
constexpr uint64_t kSpecial = 0x286c48;     // (&u16 actor id) -> bit 0: its index is one of 0x400..0x40e (the carry's +0x12 byte)
constexpr uint64_t kDataIdOf = 0x27d848;    // (&u16 actor id) -> u16 the character's data id
constexpr uint64_t kFirstOf = 0x28cc60;     // (player manager, data id) -> the first live resident of that data id, or 0
constexpr uint64_t kActorById = 0x281360;   // (player manager, &u16 actor id) -> the actor, or 0
constexpr uint64_t kSpawn = 0x658028;       // (x0 -, &u16 leader id, w2 data id, w3 mode, &16 bytes; x8 &u16 out actor id)
constexpr uint64_t kInvite = 0x657a20;      // (party manager, &u16 leader id, &u16 npc id, w3 0, w4 0): as 0x6571a0 / 0x656200 call it
constexpr uint64_t kActorId = 0x27fe;       // resident actor + : u16 its actor id
constexpr uint32_t kModeCarry = 0, kModeSpecial = 2;
constexpr uint16_t kDataIdMax = 0x3ff;
constexpr size_t kPlaces = 5;               // a slot's invited list holds 5
constexpr size_t kMost = layout::kPartySlotLists * kPlaces;
constexpr double kRefresh = 2.0;            // the record is taken again this often while the lists stay the same
constexpr double kRetry = 1.0;              // an arrival entry is tried again this often
constexpr double kArriveWait = 30.0;        // and given up after this long

struct Entry {
    uint16_t data = 0;
    uint8_t slot = 0;
    bool special = false;
    uint8_t parents[16] = {};
    bool tried = false;   // an attach was tried (its failure is logged once)
    uint16_t placed = 0;  // the creature this carry spawned here (0: none): a retry invites it again, never spawns a second
};

struct State {
    uint8_t stage = 0;                 // the area the record was taken in
    uint16_t seen[kMost] = {};         // the lists as last read, by slot (0: an empty place)
    double taken_at = -1e9;
    std::vector<Entry> record;         // the followers that cross with the group at the next area change
    std::vector<Entry> arriving;       // the record of the area the group came from, to attach here
    double arrived_at = 0.0;           // the first walkable try in the new area (0: not yet): kArriveWait counts from there
    double tried_at = -1e9;
};
State& st() {
    static State s;
    return s;
}

bool valid_id(uint16_t id) { return !(id & 0x800) && static_cast<uint32_t>((id >> 12) - 1) <= 3; }

// The authority's player walks in its area and nothing moves it: the record holds, and an arrival can be attached.
bool walking(bool in_game) { return session::can_walk(in_game) && !talk::talking() && !travel::moving(); }

// Reads every slot's invited list into ids (kPlaces per slot); true when it differs from what was read last time.
bool read_lists(State& s, uint16_t* ids) {
    for (uint8_t slot = 0; slot < layout::kPartySlotLists; ++slot) {
        uint16_t got[kPlaces];
        const size_t n = party::followers_of(slot, got, kPlaces);
        for (size_t i = 0; i < kPlaces; ++i) ids[slot * kPlaces + i] = i < n ? got[i] : 0;
    }
    return std::memcmp(ids, s.seen, sizeof(s.seen)) != 0;
}

// The record, from the lists in ids (the game's write side, 0xbcde4: a can-come follower with parents).
void take(::Ctx* c, State& s, const uint16_t* ids, uint8_t stage) {
    std::memcpy(s.seen, ids, sizeof(s.seen));
    s.taken_at = qpc::seconds();
    s.stage = stage;
    s.record.clear();
    const uint64_t sp = guest::scratch(c);
    for (size_t i = 0; i < kMost; ++i) {
        if (!valid_id(ids[i])) continue;
        guest::wr<uint16_t>(sp + 0x10, ids[i]);
        const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, sp + 0x10, 0, 0, 0, 0, sp));
        if (static_cast<uint16_t>(data - 1) >= kDataIdMax || follow::is_escort(data)) continue;
        if (!(guest::call(c, base() + kCanCome, sp + 0x10, 0, 0, 0, 0, sp) & 1)) continue;
        const uint64_t parents = guest::call(c, base() + kParentsOf, sp + 0x10, 0, 0, 0, 0, sp);
        if (!guest::ok(parents, 16) || !guest::ok(base() + kNoParents, 16)) continue;
        Entry e;
        e.data = data;
        e.slot = static_cast<uint8_t>(i / kPlaces);
        for (size_t b = 0; b < 16; ++b) e.parents[b] = guest::rd<uint8_t>(parents + b);
        bool none = true;
        for (size_t b = 0; b < 16; ++b) none = none && e.parents[b] == guest::rd<uint8_t>(base() + kNoParents + b);
        if (none) continue;
        e.special = guest::call(c, base() + kSpecial, sp + 0x10, 0, 0, 0, 0, sp) & 1;
        s.record.push_back(e);
    }
}

// The u16 actor id of slot's invited follower of data id `data` here, or 0.
uint16_t followed_by(::Ctx* c, uint8_t slot, uint16_t data) {
    uint16_t ids[kPlaces];
    const size_t n = party::followers_of(slot, ids, kPlaces);
    const uint64_t sp = guest::scratch(c);
    for (size_t i = 0; i < n; ++i) {
        guest::wr<uint16_t>(sp + 0x10, ids[i]);
        if (static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, sp + 0x10, 0, 0, 0, 0, sp)) == data) return ids[i];
    }
    return 0;
}

// Attaches one arriving entry to its leader here: true once it follows (or cannot be attached at all).
bool attach(::Ctx* c, Entry& e) {
    const uint16_t leader = follow::player_actor(e.slot);
    if (!leader) return false;   // its leader's player is not in this area (yet)
    if (followed_by(c, e.slot, e.data)) return true;
    const uint64_t pm = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    const uint64_t pg = guest::deref(base() + layout::kPartyManager);
    if (!pm || !pg) return false;
    const uint64_t sp = guest::scratch(c);
    uint16_t npc = 0;
    if (e.placed) {
        guest::wr<uint16_t>(sp + 0x10, e.placed);
        if (guest::call(c, base() + kActorById, pm, sp + 0x10, 0, 0, 0, sp)) npc = e.placed;
    }
    if (!npc) {
        const uint64_t actor = guest::call(c, base() + kFirstOf, pm, e.data, 0, 0, 0, sp);
        npc = actor && guest::ok(actor + kActorId, 2) ? guest::rd<uint16_t>(actor + kActorId) : 0;
    }
    const bool spawned = !valid_id(npc);
    if (spawned) {   // as 0x6571a0 calls it: x1 &leader id, w2 data, w3 mode, x4 &the 16 bytes, x8 &out
        guest::wr<uint16_t>(sp + 0x10, leader);
        guest::wr<uint16_t>(sp + 0x14, 0x800);
        for (size_t b = 0; b < 16; ++b) guest::wr<uint8_t>(sp + 0x20 + b, e.parents[b]);
        guest::call_n<9>(c, base() + kSpawn,
                         {0, sp + 0x10, e.data, e.special ? kModeSpecial : kModeCarry, sp + 0x20, 0, 0, 0, sp + 0x14}, sp);
        npc = guest::rd<uint16_t>(sp + 0x14);
        if (valid_id(npc)) e.placed = npc;
        if (!valid_id(npc)) {
            if (!e.tried)
                NXLOG_I(kTag, "Co-op carry: character %u could not be placed by player %u in this area; tried again", e.data, e.slot);
            e.tried = true;
            return false;
        }
    }
    guest::wr<uint16_t>(sp + 0x10, leader);
    guest::wr<uint16_t>(sp + 0x14, npc);
    guest::call(c, base() + kInvite, pg, sp + 0x10, sp + 0x14, 0, 0, sp);
    guest::wr<uint16_t>(sp + 0x14, npc);
    if (followed_by(c, e.slot, e.data) == npc) follow::accept(c, sp + 0x14, leader, e.slot);
    const bool led = followed_by(c, e.slot, e.data) == npc;
    if (led || !e.tried)
        NXLOG_I(kTag, "Co-op carry: character %u (actor 0x%x, %s) %s player %u into this area", e.data, npc,
                spawned ? "placed by its leader" : npc == e.placed ? "placed earlier" : "already here", led ? "followed" : "did not attach to; tried again", e.slot);
    e.tried = !led;
    return led;
}

void arrive(::Ctx* c, State& s) {
    const double now = qpc::seconds();
    if (now - s.tried_at < kRetry) return;
    s.tried_at = now;
    if (s.arrived_at == 0.0) s.arrived_at = now;
    for (size_t i = 0; i < s.arriving.size();) {
        if (attach(c, s.arriving[i])) {
            s.arriving.erase(s.arriving.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    if (s.arriving.empty() || now - s.arrived_at < kArriveWait) return;
    for (const Entry& e : s.arriving)
        NXLOG_W(kTag, "Co-op carry: character %u did not join player %u in this area within %.0f s; it stays where it lives", e.data,
                e.slot, kArriveWait);
    s.arriving.clear();
}

}  // namespace

void before_move(::Ctx* c) {
    if (!c || !link::in_session() || !link::is_authority()) return;
    State& s = st();
    const uint8_t stage = session::area_stage();
    if (!stage || !s.arriving.empty()) return;   // still placing the last area's followers: that record stands
    uint16_t ids[kMost];
    read_lists(s, ids);
    take(c, s, ids, stage);
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c) return;
    if (!link::in_session() || !link::is_authority()) {
        s = State{};
        return;
    }
    const uint8_t stage = session::area_stage();
    if (!stage) return;
    if (s.stage && stage != s.stage) {   // the group changed area: the last record crosses
        if (!s.record.empty()) {   // an area passed on the way (no record taken there) keeps the arrivals it has
            NXLOG_I(kTag, "Co-op carry: the group moved from stage %u to %u; %zu followers who can come follow their leaders here",
                    s.stage, stage, s.record.size());
            s.arriving = std::move(s.record);
            s.arrived_at = 0.0;
            s.tried_at = -1e9;
        }
        s.record.clear();
        std::memset(s.seen, 0, sizeof(s.seen));
        s.stage = stage;
        s.taken_at = -1e9;
    }
    if (!walking(in_game)) return;
    if (!s.arriving.empty()) {
        arrive(c, s);
        if (!s.arriving.empty()) return;   // the record is taken again once they follow (the lists then hold them)
    }
    uint16_t ids[kMost];
    if (read_lists(s, ids) || qpc::seconds() - s.taken_at >= kRefresh) take(c, s, ids, stage);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::carry
