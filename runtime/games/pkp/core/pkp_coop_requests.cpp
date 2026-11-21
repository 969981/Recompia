// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared creature requests (pkp_coop_internal.h). A creature's unique request (the requests
// app) is kept in each player's own save and the game never tells the others: only the player who talked to the creature saw it. Each
// process shares the requests its player has taken on or finished (ReqUnique), and every other member's app then lists them too.
//
// The game, 2.0.1 (unique requests; general, per-day requests are the game's own kind 6 replication and are not touched here):
//   save root + 0x6fabd9 + id * 4    the record of unique request id 1..200 (m_uniqueQuestHistoryVer2, saved): +0 state, +1 the
//                                    app's "new" badge, +2 progress, +3 flags. States: 1 waiting (the default), 2 offered, 3 taken
//                                    on, 4 fulfilled (waiting for the report talk), 5 done. The app lists a row by state 3 or 4 alone
//                                    (seen: a 3 written into a record listed it at once, with the creature and its text).
//   *(main + 0x3565160) = MU         the unique request manager: per id a 16-byte slot at MU + id * 16 (+0 u16 the bound
//                                    creature's actor handle, +2 u8 id) and one list per state; the frame update checks the
//                                    slots in the state 3 list (3 -> 4) and the report talk needs the creature's request
//                                    component to hold the id. A raw write moves no slot: kQueue(MU + 0x2c70, &slot) asks the
//                                    game to re-sort it by its record.
//   kTakeOn(MU, slot, 0)             the step "taken on" as the game re-applies it when the creature loads with a saved 3: writes
//                                    3, queues the slot, sets the component's id, checks the condition once. Returns at once
//                                    when the slot has no creature bound (handle bit 11 set or kind not 1..4).
//   the request component            a creature's actor (0x281360(*(*(main + 0x3483bd0)), &handle), its vt+0x1d8() true) has it
//                                    at vt+0x408() + 0x29a8; +0xc u32 its chara, +0x10 u8 the id the creature offers or holds (0:
//                                    none). The offer needs it free, the take-on step writes it whatever it held (0x61491c), the
//                                    report talk needs it to hold the id at 4, and the completion 0x618304 clears it (0x6183b0).
//                                    The request bubble is the kind 1 balloon of the creature's talk component, owner + 0x29c0
//                                    (0x618420); the completion takes it off with 0x3b1800(talk, 1) (0x618490).
//   the checker 0x613c9c (3 -> 4)    for the event types (+0x5d 7, 8, 9, 0x10, 0x1b, 0x26, 0x29, 0x2a: kEventIds) it tests record
//                                    byte +3 bit 0 alone (0x613df4..0x613e14; type 0x25 after two actor checks), a bit an event
//                                    sets on the player's own record; a one-time scene a teammate already played may never set it
//                                    here.
//   general entries                  the creature's general (daily) request, 0x30 bytes per chara (< 0x41a, else entry 0), +8 its
//                                    state: save root + 0xc81488 on Cloud Island (stage 0x11), + 0x16f2af8 (the host's, mirrored)
//                                    on a visitor, + 0x22fef0 otherwise. The offer 0x6132b8 offers no unique id while it is 1..5.
//   data table slot 0x129 (#1081)    a row per character 1..0x1ff, read as the creature's load 0x61ccc0 reads it: *(row + 8) + 2
//                                    .. + 0x15 are the unique ids that creatures offers in that order, one at a time (its one
//                                    component; the load re-applies every saved 2, 3 or 4 of the list, the last one winning).
//   the bind gate 0x11435c(&handle)  the load 0x61ccc0, the unbind 0x61d1ac and the creature's own "have I a request" 0x3b5624 get
//                                    its chara from 0x61d068, whose first test (its only caller) is this: the handle is a resident
//                                    creature (0x286de4), the player is no visitor (game state +0x90 bit 2) and the stage is not
//                                    Cloud Island (0x77ecb0). So on a member no slot ever binds: no offer, no take-on (kTakeOn
//                                    returns at once) and no report talk. The hook answers on a member as the game does at home
//                                    (mapped first by a community contributor's link_story_coop gifts work).
// Rules on a receiver (the repo owner's call, 2026-11-20: requests are shared, one completion closes a request for the whole group
// and every member gets its reward once), so nothing is undone and no history is invented:
//   - another member's 3 or 4, and on the host also another member's 5 (that member's own history): a record here below 3 becomes
//     3 through the game's own take-on step (or 3 + the queue when the creature is not loaded here; it binds and re-applies when it
//     loads); the game's checker and the report talk do the rest;
//   - a close (the group's completion): on a member, the authority's 5 (its world is the group's: the host's own hand-in, or a
//     member's that the host closed); on the host, a member's hand-in (requests::close from pkp_coop_outcomes.cpp, which also
//     applies the world part once). The record here goes from any state 1..4 to 5 (+ the queue), and what the completion 0x618304
//     does for the creature is done too: its component let go (0x6183b0) and its request bubble taken off (0x618490), so no member
//     keeps the bubble or can hand the request in again. The reward is the completer's own run, shared: the items its report talk
//     gives (catch-up's Items) and the world part (pkp_coop_outcomes.cpp). A 5 from another member alone does not close it on a
//     member (it may be that member's own history; the authority's word follows a real hand-in). The host's close is sent at once;
//   - an event id (kEventIds) is taken on only from another member's 4 or 5, and then with its event bit (+3 bit 0) set, so the
//     checker makes it 4 at once: its event may be a one-time scene of the shared world that this player never sees, and a 3
//     taken on from another member's 3 could stay 3 forever and keep the creature's other requests waiting here (the rule below).
//     A 3 here (taken on by this player, or by an older build) gets the bit from another member's 4 the same way (on the host
//     also from a 5; on a member a 5 alone changes nothing, so the bubble never comes back after the group's hand-in);
//   - a request that is also a story step (kStoryIds: the story is shared, so its effects run once): on a member, any other member's
//     5 closes it here too. On the host another member's 5 takes it on (as any request); the member's hand-in closes it;
//   - one request per creature at a time, as the game offers them: an id waits while another id of its creature (by the table, or
//     the same bound actor) is at 2..4 here, or while the creature has a general request (the offer's own rule), so the take-on step
//     never takes the component from a request in progress. Only a take-on waits: a request the player already holds is never
//     held back (its event bit and a story close go at once);
//   - kOwnIds (requests the game binds to one player) are not shared at all.
// A received state is kept (per id, the highest within a wait wins: a member that is behind never lowers it) and applied from the
// frame tick only while this player is in the group's world (the host, or a member with the visitor bit), can walk (no event,
// menu, autosave or load) and talks to no one, once the character table is read, at most kApplyPerTick ids a frame (a member's
// whole list comes at a join), a creature's ids in the order of its #1081 row.
// Each process reads its records every frame and sends a change when it sees one (one that could not be sent goes at the next
// frame), and its whole list of taken-on and finished requests every kAllEvery seconds and when it or another member joins,
// through the authority to every member.
// General requests are the game's own (one entry on the host, kind 6 replays its steps on every member, the bubble included), with
// two exceptions. The reward of the completion 0x6218e0(.., w2 rewards, w3 replay): 0x3bcdc0(0, &h, entry +0x14 item, +0x16 count)
// behind `tbz w20, #0` at 0x621c94, and item 0x1b1 x1 when entry +0x19 is set (0x621e3c), go to the talker alone (the replays pass
// w2 = 0); the talker's run shares them (catch-up's Items), read at the goal's site below (w20 = w2, x26 = the request component:
// +0xc chara, x24 the save root's pointer; straight line from there to 0x621c94). And the completion counts the town goal ("hand in
// requests", 0x618b00(CM) at 0x621bec, behind `cbz x0` at 0x621be8; w21 = w3 there) with no test of its own, so a replay counted it
// again on every process: the kind 6 reader's replay of the host's step on each member (0x620ba8, w3 = 1) and the host's apply of a member's
// step (0x62e0fc, w3 = 1). The talks pass w3 = 0 (0x62a404, 0x62ab90). The talker's own count is the group's: on the host it is
// the host's goal, on a member the game sends it to the host (CM frame 0x631020 -> 0x633150, RequestChallengeProgress; host
// handler 0x54fbc0 -> 0x54fd60). So in a session a replay skips the goal (x0 = 0 at 0x621be8) and each hand-in counts once.
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_tables.h"

#include <algorithm>
#include <iterator>

#include <cstring>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::requests {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kUnique = layout::kSaveUniqueRequests;   // save root + : the unique request records, 4 bytes each, by id
constexpr uint64_t kManager = 0x3565160; // main + : the unique request manager MU
constexpr uint64_t kSlotSize = 16;       // MU + id * : the id's slot
constexpr uint64_t kQueueList = 0x2c70;  // MU + : the re-sort queue
constexpr uint64_t kQueue = 0x6154c0;    // (list, &slot): queue a slot for the re-sort by its record
constexpr uint64_t kTakeOn = 0x614860;   // (MU, slot, talked): the step "taken on"
constexpr uint64_t kActors = 0x3483bd0;  // main + : -> the actor manager pointer
constexpr uint64_t kActorOf = 0x281360;  // (actors, &u16 handle): the actor, or 0
constexpr uint64_t kActorAlive = 0x1d8, kActorRequest = 0x408;   // actor vtable + : bool (), the request owner ()
constexpr uint64_t kRequestComp = 0x29a8, kTalkComp = 0x29c0;    // owner + : the request component, the talk component
constexpr uint64_t kBubbleOff = 0x3b1800;                        // (talk, kind): takes the creature's balloons of that kind off
constexpr uint32_t kRequestBubble = 1;
constexpr uint64_t kCompChara = 0xc, kCompId = 0x10;             // component + : its u32 chara, its u8 id
constexpr uint64_t kCharaTable = 0x129 * 8;              // data-table manager + 0x10 + : the table of slot 0x129, a row per character
constexpr uint32_t kCharaMax = 0x1ff;                    // characters 1..0x1ff
constexpr uint64_t kCharaIds = 2, kCharaIdsEnd = 0x16;   // *(row + 8) + : the ids the character offers, in its order
constexpr uint64_t kGeneralOwn = 0x22fef0, kGeneralMirror = 0x16f2af8, kGeneralCloud = 0xc81488;   // save root + : general entries
constexpr uint64_t kGeneralSize = 0x30, kGeneralState = 8;   // per chara; entry + : u8 state (0 none, 1..5 a request today)
constexpr uint64_t kGeneralReward = 0x14, kGeneralRewardCount = 0x16, kGeneralJuly = 0x19;   // entry + : u16 item, u8 count, u8 flag
constexpr uint16_t kJulyItem = 0x1b1;                        // the completion's second give while the July flag is set (0x621e28)
constexpr uint64_t kGoalChara = 0xc;                         // x26 at the goal's site (the request component) + : u32 chara
constexpr uint32_t kGeneralCount = 0x41a;                    // charas with an entry (another reads entry 0, as the game does)
constexpr uint8_t kCloudStage = 0x11;                        // game state stage: Cloud Island (0x77ecb0)
constexpr uint64_t kIsResident = 0x286de4;                   // (&u16 handle): bit 0, the handle is a resident creature's
constexpr uint64_t kRecFlags = 3;                            // record + : u8 flags
constexpr uint8_t kEventBit = 0x01;                          // flags: the event of an event-type request ran
// Requests that are also a story step (LINKDATA #1209 +2 != 0): one completion closes them for the group. 73 is here by the repo
// owner's rule, not the table: its state byte is read as story state (0x34b20, 0x4147c, 0x13aa20, 0x13aaf4 test 2..4 for the
// creature that offers it), so a member must not stay at 4 after the group's 5 (STORY-RECORDS.md section 3).
constexpr uint8_t kStoryIds[] = {2, 3, 7, 8, 9, 10, 11, 15, 17, 19, 20, 23, 28, 31, 54, 56, 65, 66, 73, 76, 77, 79, 80};
// Requests the game binds to one player (#1557 +0x2c != 0): never shared.
constexpr uint8_t kOwnIds[] = {94, 95};
// Requests the checker meets by the record's event bit (#1557 +0x5d 7, 8, 9, 0x10, 0x1b, 0x26, 0x29, 0x2a: the bit alone; 0x25,
// id 171: two actor checks at 0x613f64, then the same bit test).
constexpr uint8_t kEventIds[] = {8,   9,   12,  15,  17,  28,  34,  35,  40,  42,  51,  63,  67,  69,  70,  73,
                                 84,  107, 108, 110, 114, 115, 116, 121, 129, 131, 133, 134, 140, 145, 146, 147,
                                 150, 151, 154, 156, 157, 166, 171, 173, 174, 175, 176, 177, 178, 179, 180};
constexpr uint32_t kIdMax = layout::kUniqueRequestCount;   // ids 1..200
constexpr uint8_t kWaiting = 1, kOffered = 2, kTaken = 3, kFulfilled = 4, kDone = 5;
constexpr double kAllEvery = 10.0;       // the whole list again (a member that missed a change)
constexpr double kNoticeGap = 4.0;       // at most one line per this many seconds
constexpr unsigned kApplyPerTick = 4;    // received ids applied in one frame, at most

// A received state that waits for a safe moment.
struct Pending {
    uint8_t there = 0;    // the other member's state (0: nothing waits)
    uint8_t origin = 0;   // that member's node
    bool close = false;   // the group's completion: closed here (the rules above)
    bool logged = false;  // its wait for the creature was logged
};

struct State {
    uint8_t seen[kIdMax + 1] = {};   // per id: the state last read here (0: not read yet in this session)
    bool have_seen = false;          // the first read of a session only takes the baseline
    bool baseline_sent = false;      // the session's first list went out (until then a send is the baseline: no change lines)
    bool send_all = true;            // the whole list at the next read
    bool logged_giver = false;       // the bind gate's first member answer was logged
    double all_at = 0.0, line_at = -1e9;
    alignas(8) uint64_t slot_arg = 0;     // the queue's argument, holding the slot (guest-readable, outlives the call's frame)
    alignas(8) uint16_t handle_arg = 0;   // the actor lookup's argument: a handle
    Pending pending[kIdMax + 1] = {};   // per id: the highest received state not applied yet
    uint16_t chara[kIdMax + 1] = {};    // per id: the character that offers it (0: none known)
    uint8_t pos[kIdMax + 1] = {};       // per id: its place in that character's row
    uint8_t order[kIdMax] = {};         // the ids by (character, place): the order apply takes them in
    bool have_chara = false;            // the character table was read (game data: once)
};
State& st() {
    static State s;
    return s;
}

uint64_t records() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + kUnique, (kIdMax + 1) * 4) ? r + kUnique : 0;
}
uint8_t state_of(uint64_t recs, uint32_t id) { return guest::rd<uint8_t>(recs + id * 4); }

template <size_t N>
bool listed(const uint8_t (&ids)[N], uint32_t id) { return std::find(std::begin(ids), std::end(ids), id) != std::end(ids); }

uint64_t manager() {
    const uint64_t mu = guest::rd<uint64_t>(base() + kManager);
    return mu && guest::ok(mu, kQueueList + 8) ? mu : 0;
}

// The game's re-sort of the slot by its record (x1: the address of a variable that holds the slot).
void queue(::Ctx* c, uint64_t mu, uint64_t slot) {
    State& s = st();
    s.slot_arg = slot;
    guest::call(c, base() + kQueue, mu + kQueueList, reinterpret_cast<uint64_t>(&s.slot_arg));
}

bool bound(uint64_t slot) {
    const uint16_t handle = guest::rd<uint16_t>(slot);
    const unsigned kind = handle >> 12;
    return !(handle & 0x800) && kind >= 1 && kind <= 4;
}

// The request owner of the creature bound to the slot (its components at kRequestComp and kTalkComp), found as the take-on step and
// the completion find it; 0 if none.
uint64_t owner_of(::Ctx* c, uint64_t slot) {
    State& s = st();
    if (!bound(slot)) return 0;
    const uint64_t got = guest::rd<uint64_t>(base() + kActors);
    const uint64_t actors = got && guest::ok(got, 8) ? guest::rd<uint64_t>(got) : 0;
    if (!actors) return 0;
    s.handle_arg = guest::rd<uint16_t>(slot);
    const uint64_t actor = guest::call(c, base() + kActorOf, actors, reinterpret_cast<uint64_t>(&s.handle_arg));
    if (!actor || !guest::ok(actor, 8)) return 0;
    const uint64_t vt = guest::rd<uint64_t>(actor);
    if (!(guest::call(c, guest::rd<uint64_t>(vt + kActorAlive), actor) & 1)) return 0;
    const uint64_t owner = guest::call(c, guest::rd<uint64_t>(vt + kActorRequest), actor);
    return owner && guest::ok(owner + kTalkComp, 0x20) && guest::ok(owner + kRequestComp + kCompId, 1) ? owner : 0;
}

uint64_t component(::Ctx* c, uint64_t slot) {
    const uint64_t owner = owner_of(c, slot);
    return owner ? owner + kRequestComp : 0;
}

// Takes request id on here as the game does when its creature loads with a saved 3.
void take_on(::Ctx* c, uint64_t recs, uint64_t mu, uint32_t id) {
    const uint64_t slot = mu + id * kSlotSize;
    guest::wr<uint8_t>(recs + id * 4, kTaken);   // first: the step then skips its first-offer branch (old state above 1)
    if (bound(slot)) guest::call(c, base() + kTakeOn, mu, slot, 0);
    else queue(c, mu, slot);
}

// Which character offers each id, from the character table once it is loaded (an id two characters list keeps the first).
void read_charas(::Ctx* c) {
    State& s = st();
    if (s.have_chara) return;
    const uint64_t mgr = tables::manager(base());
    if (!mgr || !tables::holds(mgr, kCharaTable)) return;
    s.have_chara = true;
    unsigned n = 0;
    for (uint32_t chara = 1; chara <= kCharaMax; ++chara) {
        const uint64_t row = tables::row(c, base(), kCharaTable, chara);
        const uint64_t ids = row && guest::ok(row + 8, 8) ? guest::rd<uint64_t>(row + 8) : 0;
        if (!ids || !guest::ok(ids, kCharaIdsEnd)) continue;
        for (uint64_t at = kCharaIds; at < kCharaIdsEnd; ++at) {
            const uint8_t id = guest::rd<uint8_t>(ids + at);
            if (id < 1 || id > kIdMax || s.chara[id]) continue;
            s.chara[id] = static_cast<uint16_t>(chara);
            s.pos[id] = static_cast<uint8_t>(at - kCharaIds);
            ++n;
        }
    }
    for (uint32_t id = 1; id <= kIdMax; ++id) s.order[id - 1] = static_cast<uint8_t>(id);
    std::stable_sort(std::begin(s.order), std::end(s.order), [&s](uint8_t a, uint8_t b) {
        return s.chara[a] != s.chara[b] ? s.chara[a] < s.chara[b] : s.pos[a] < s.pos[b];
    });
    NXLOG_I(kTag, "Co-op requests: %u requests have a known creature", n);
}

// The character's general (daily) request entry as the game's 3-way select picks it here (0: no save or game state).
uint64_t general_entry(uint32_t chara) {
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!root || !guest::ok(game, layout::kStateStage + 1)) return 0;
    const bool cloud = guest::rd<uint8_t>(game + layout::kStateStage) == kCloudStage;
    const uint64_t entries = root + (cloud ? kGeneralCloud : (session_bits() & layout::kSessionVisitor) ? kGeneralMirror : kGeneralOwn);
    const uint64_t entry = entries + (chara < kGeneralCount ? chara * kGeneralSize : 0);
    return guest::ok(entry, kGeneralSize) ? entry : 0;
}

// The character has a general (daily) request here (state 1..5), read as the offer 0x6132b8 reads it before it offers a unique id.
bool general_busy(uint32_t chara) {
    const uint64_t entry = general_entry(chara);
    if (!entry) return false;
    const uint8_t state = guest::rd<uint8_t>(entry + kGeneralState);
    return state >= 1 && state <= 5;
}

// The creature that offers id is busy here: another of its ids (by the table, or the same bound actor) is offered, taken on or
// fulfilled (its component holds that one), or its loaded actor (the one the take-on step writes to) has a general request. An
// unbound slot is only queued, so the general check needs the actor's own chara; the #1081 key is another lookup (0x61d068).
bool chara_busy(::Ctx* c, uint64_t recs, uint64_t mu, uint32_t id) {
    const State& s = st();
    const uint64_t slot = mu + id * kSlotSize;
    const bool is_bound = bound(slot);
    const uint16_t handle = guest::rd<uint16_t>(slot);
    for (uint32_t other = 1; other <= kIdMax; ++other) {
        if (other == id) continue;
        const bool same = (s.chara[id] && s.chara[other] == s.chara[id]) ||
                          (is_bound && guest::rd<uint16_t>(mu + other * kSlotSize) == handle);
        const uint8_t state = same ? state_of(recs, other) : 0;
        if (state >= kOffered && state <= kFulfilled) return true;
    }
    const uint64_t comp = component(c, slot);
    return comp && general_busy(guest::rd<uint32_t>(comp + kCompChara));
}

bool shared_here() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }

// This player is in the group's world (the host, or a member once the visitor bit is set), can walk and talks to no one.
bool safe_now(bool in_game) {
    const bool in_world = role() == CoopRole::Host || (session_bits() & layout::kSessionVisitor);
    return in_world && session::can_walk(in_game) && !talk::talking();
}

// Sends (u8 id, u8 state) for every id whose state rose to 3..5 since the last read, or (all) every id at 3..5. An id is marked
// seen once its change is sent (or is none to send); false: the send failed, and the same changes go at the next read.
bool share(uint64_t recs, bool all) {
    State& s = st();
    std::vector<uint8_t> pairs;
    uint8_t n = 0;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        const uint8_t state = state_of(recs, id);
        const bool rose = s.have_seen && state != s.seen[id];
        if (state < kTaken || state > kDone || !(all || rose) || listed(kOwnIds, id)) {
            s.seen[id] = state;
            continue;
        }
        pairs.push_back(static_cast<uint8_t>(id));
        pairs.push_back(state);
        ++n;
    }
    s.have_seen = true;
    if (n && !link::share(wire::Type::ReqUnique, wire::Writer(link::self()).u8(n).bytes(pairs.data(), pairs.size()).take()))
        return false;
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t id = pairs[k * 2], state = pairs[k * 2 + 1];
        if (s.baseline_sent && s.seen[id] != state) NXLOG_I(kTag, "Co-op requests: request %u reached state %u here, shared", id, state);
        s.seen[id] = state;
    }
    s.baseline_sent = true;
    return true;
}

void line(const std::string& text) {
    State& s = st();
    const double now = qpc::seconds();
    if (now - s.line_at < kNoticeGap) return;
    s.line_at = now;
    notice::post(text);
}

// The state another member's state (or a close) gives a record at `here` (the rules above).
uint8_t target(uint32_t id, const Pending& p, uint8_t here) {
    const bool member = role() != CoopRole::Host;
    if (p.close || (p.there == kDone && member && listed(kStoryIds, id))) return here >= kWaiting && here < kDone ? kDone : here;
    if (p.there == kDone && member) return here;   // another member's 5 alone: the authority's word closes it
    if (listed(kEventIds, id) && p.there < kFulfilled) return here;
    return here >= kWaiting && here < kTaken ? kTaken : here;
}

// The record needs its event bit for the checker: an event id another member met, being taken on here or held at 3.
bool needs_event_bit(uint64_t recs, uint32_t id, uint8_t there, uint8_t here, uint8_t to, bool take) {
    const bool met = there == kFulfilled || (there == kDone && role() == CoopRole::Host);
    return listed(kEventIds, id) && met && (take || (here == kTaken && to == here)) &&
           !(guest::rd<uint8_t>(recs + id * 4 + kRecFlags) & kEventBit);
}

// Applies up to kApplyPerTick of the waiting states, a creature's ids in its row's order; one notice line for them.
void apply(::Ctx* c, uint64_t recs) {
    State& s = st();
    const uint64_t mu = manager();
    if (!mu) return;
    read_charas(c);
    if (!s.have_chara) return;   // chara_busy needs it: without it a take-on could take a component in use
    unsigned applied = 0;
    int taken = -1, finished = -1, closed = -1;   // per kind of change: the node of its first (-1: none)
    for (uint32_t k = 0; k < kIdMax && applied < kApplyPerTick; ++k) {
        const uint32_t id = s.order[k];
        Pending& waiting = s.pending[id];
        if (!waiting.there) continue;
        const uint8_t here = state_of(recs, id);
        const uint8_t to = target(id, waiting, here);
        const bool take = here < kTaken && to == kTaken;   // only a take-on waits; a request held here is never held back
        if (take && chara_busy(c, recs, mu, id)) {   // kept: taken on once the creature's other request is done here
            if (!waiting.logged)
                NXLOG_I(kTag, "Co-op requests: request %u at state %u on node %u waits, its creature has another one here", id,
                        waiting.there, waiting.origin);
            waiting.logged = true;
            continue;
        }
        const Pending p = waiting;
        waiting = Pending{};
        const bool event_bit = needs_event_bit(recs, id, p.there, here, to, take);
        if (to == here && !event_bit) continue;
        ++applied;
        if (event_bit)   // before the take-on, whose condition check then makes it 4 at once; a 3 held here: the frame check does
            guest::wr<uint8_t>(recs + id * 4 + kRecFlags, guest::rd<uint8_t>(recs + id * 4 + kRecFlags) | kEventBit);
        if (take) {
            take_on(c, recs, mu, id);
        } else if (to != here) {
            const uint64_t slot = mu + id * kSlotSize;
            // The completion lets the component go and takes the bubble off (0x6183b0, 0x618490); a close does too.
            const uint64_t owner = owner_of(c, slot);
            if (owner && guest::rd<uint8_t>(owner + kRequestComp + kCompId) == id) {
                guest::wr<uint8_t>(owner + kRequestComp + kCompId, 0);
                guest::call(c, base() + kBubbleOff, owner + kTalkComp, kRequestBubble);
            }
            guest::wr<uint8_t>(recs + id * 4, to);
            queue(c, mu, slot);
        }
        // Ours now: not sent back as a change. The host's close is the group's word, sent at the next read.
        if (s.have_seen && !(p.close && role() == CoopRole::Host)) s.seen[id] = state_of(recs, id);
        NXLOG_I(kTag, "Co-op requests: request %u at state %u on node %u, %u here -> %u%s", id, p.there, p.origin, here, to,
                event_bit ? ", its event bit set" : "");
        int& first = to == kDone ? closed : take && p.there != kDone ? taken : finished;   // only the bit: it was listed already
        if (first < 0) first = p.origin;
    }
    if (taken >= 0) line(strings::fmt(base(), strings::Str::CoopRequestTaken, notice::name_of(static_cast<uint8_t>(taken))));
    else if (finished >= 0) line(strings::fmt(base(), strings::Str::CoopRequestFinished, notice::name_of(static_cast<uint8_t>(finished))));
    else if (closed >= 0) line(strings::get(base(), strings::Str::CoopRequestClosed));
}

}  // namespace

int on_general_goal(::Ctx* c) {
    // w21 bit 0: the game's own replay of a step (the talks pass 0). Replays only run online; offline nothing changes.
    if (!(c->x[21] & 1)) {
        // The talker's own step: w20 bit 0, its reward is given here next (0x621c94), to this player alone. Shared in a session.
        if (!(c->x[20] & 1) || !link::in_session() || !guest::ok(c->x[26] + kGoalChara, 4)) return 0;
        const uint64_t entry = general_entry(guest::rd<uint32_t>(c->x[26] + kGoalChara));
        if (!entry) return 0;
        catchup::share_give(guest::rd<uint16_t>(entry + kGeneralReward), guest::rd<uint8_t>(entry + kGeneralRewardCount));
        if (guest::rd<uint8_t>(entry + kGeneralJuly)) catchup::share_give(kJulyItem, 1);
        return 0;
    }
    if (!(session_bits() & layout::kSessionOnline) || !c->x[0]) return 0;
    c->x[0] = 0;
    NXLOG_I(kTag, "Co-op requests: a general request's hand-in replayed here; its town goal counted on the process of the player who "
            "handed it in, skipped here");
    return 0;
}

int on_giver_check(::Ctx* c) {
    // The game's own session bits, as the world effects' skip: a member in the host's world, even before its channel is up.
    if (role() != CoopRole::Visit || !(session_bits() & layout::kSessionVisitor)) return 0;
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(game, layout::kStateStage + 1)) return 0;
    const bool resident = guest::call(c, base() + kIsResident, c->x[0]) & 1;
    c->x[0] = resident && guest::rd<uint8_t>(game + layout::kStateStage) != kCloudStage ? 1 : 0;
    State& s = st();
    if (c->x[0] && !s.logged_giver) {
        s.logged_giver = true;
        NXLOG_I(kTag, "Co-op requests: the creatures of the group's world bind this member's own requests");
    }
    return 1;   // the function returns x0
}

void on_unique_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t n = r.u8();
    if (!r.ok() || !shared_here()) return;
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t id = r.u8(), there = r.u8();
        if (!r.ok()) return;
        if (id < 1 || id > kIdMax || there < kTaken || there > kDone || listed(kOwnIds, id)) continue;
        Pending& p = s.pending[id];
        if (there == kDone && role() != CoopRole::Host && origin == link::authority()) {   // the group's completion
            p.close = true;
            p.origin = origin;
        }
        if (there <= p.there) continue;   // the highest wins: a member that is behind never lowers it
        p.there = there;                  // a logged wait stays logged (the whole list comes every kAllEvery)
        p.origin = origin;
    }
}

void close(uint32_t id, uint8_t origin) {
    if (id < 1 || id > kIdMax || listed(kOwnIds, id) || role() != CoopRole::Host || !shared_here()) return;
    Pending& p = st().pending[id];
    p.close = true;
    p.there = kDone;
    p.origin = origin;
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    const uint64_t recs = records();
    if (!recs || !shared_here()) return;
    if (c && safe_now(in_game)) apply(c, recs);
    const double now = qpc::seconds();
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    if (!share(recs, all) || !all) return;
    s.send_all = false;
    s.all_at = now;
}

void on_session_change() {
    State& s = st();
    std::memset(s.seen, 0, sizeof s.seen);
    std::fill(std::begin(s.pending), std::end(s.pending), Pending{});
    s.have_seen = false;
    s.baseline_sent = false;
    s.send_all = true;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

uint64_t request_slot(uint32_t id) {
    const uint64_t mu = manager();
    return mu && id >= 1 && id <= kIdMax ? mu + id * kSlotSize : 0;
}
uint64_t request_component(::Ctx* c, uint64_t slot) { return component(c, slot); }
bool story_request(uint32_t id) { return listed(kStoryIds, id); }
bool own_request(uint32_t id) { return listed(kOwnIds, id); }

}  // namespace nxrt::games::pkp::coop::requests
