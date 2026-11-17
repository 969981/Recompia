// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: seamless co-op's shared Pokémon requests (pkp_coop_internal.h). A Pokémon's unique request (the Pokédex requests
// app) is kept in each player's own save and the game never tells the others: only the player who talked to the Pokémon saw it. Each
// process shares the requests its player has taken on or finished (ReqUnique), and every other member's app then lists them too.
//
// The game, 2.0.1 (unique requests; general, per-day requests are the game's own kind 6 replication and are not touched here):
//   save root + 0x6fabd9 + id * 4    the record of unique request id 1..200 (m_uniqueQuestHistoryVer2, saved): +0 state, +1 the
//                                    app's "new" badge, +2 progress, +3 flags. States: 1 waiting (the default), 2 offered, 3 taken
//                                    on, 4 fulfilled (waiting for the report talk), 5 done. The app lists a row by state 3 or 4 alone
//                                    (seen: a 3 written into a record listed it at once, with the Pokémon and its text).
//   *(main + 0x3565160) = MU         the unique request manager: per id a 16-byte slot at MU + id * 16 (+0 u16 the bound
//                                    Pokémon's actor handle, +2 u8 id) and one list per state; the frame update checks the
//                                    slots in the state 3 list (3 -> 4) and the report talk needs the Pokémon's request
//                                    component to hold the id. A raw write moves no slot: kQueue(MU + 0x2c70, &slot) asks the
//                                    game to re-sort it by its record.
//   kTakeOn(MU, slot, 0)             the step "taken on" as the game re-applies it when the Pokémon loads with a saved 3: writes
//                                    3, queues the slot, sets the component's id, checks the condition once. Returns at once
//                                    when the slot has no Pokémon bound (handle bit 11 set or kind not 1..4).
//   the request component            a Pokémon's actor (0x281360(*(*(main + 0x3483bd0)), &handle), its vt+0x1d8() true) has it
//                                    at vt+0x408() + 0x29a8; +0x10 u8 is the id the Pokémon offers or holds (0: none). The offer
//                                    needs it free, the take-on step writes it whatever it held (0x61491c), the report talk needs
//                                    it to hold the id at 4, and the completion 0x618304 clears it (0x6183b0).
//   data table slot 0x129 (#1081)    a row per character 1..0x1ff, read as the Pokémon's load 0x61ccc0 reads it: *(row + 8) + 2
//                                    .. + 0x15 are the unique ids that Pokémon offers, one at a time (its one component; the load
//                                    re-applies every saved 2, 3 or 4 of the list, the last one winning).
// Rules on a receiver (the repo owner's call: every member can complete a request and gets its rewards; two to four times the
// resources is fine), so nothing is undone and no history is invented:
//   - another member's 3, 4 or 5: a record here below 3 becomes 3 through the game's own take-on step (or 3 + the queue when the
//     Pokémon is not loaded here; it binds and re-applies when it loads); the game's checker and the report talk do the rest, on
//     each member's own record and with each member's own rewards. A 5 counts too, so a player who was away when another member
//     finished a request still gets it;
//   - except a request that is also a story step (kStoryIds: the story is shared, so its effects run once): another member's 5
//     closes it here from any state 1..4 (5 + the queue, and the Pokémon's component let go as the completion does), so it is
//     never offered or done again here;
//   - one request per Pokémon at a time, as the game offers them: an id waits while another id of its Pokémon (by the table, or
//     the same bound actor) is at 2..4 here, so the take-on step never takes the component from a request in progress;
//   - kOwnIds (requests the game binds to one player) are not shared at all.
// A received state is kept (per id, the latest wins) and applied from the frame tick only while this player is in the group's world
// (the host, or a member with the visitor bit), can walk (no event, menu, autosave or load) and talks to no one, at most
// kApplyPerTick ids a frame (a member's whole list comes at a join).
// Each process reads its records every frame and sends a change when it sees one (one that could not be sent goes at the next
// frame), and its whole list of taken-on and finished requests every kAllEvery seconds and when it or another member joins,
// through the authority to every member.
#include "core/pkp_coop_internal.h"
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

constexpr uint64_t kUnique = 0x6fabd9;   // save root + : the unique request records, 4 bytes each, by id
constexpr uint64_t kManager = 0x3565160; // main + : the unique request manager MU
constexpr uint64_t kSlotSize = 16;       // MU + id * : the id's slot
constexpr uint64_t kQueueList = 0x2c70;  // MU + : the re-sort queue
constexpr uint64_t kQueue = 0x6154c0;    // (list, &slot): queue a slot for the re-sort by its record
constexpr uint64_t kTakeOn = 0x614860;   // (MU, slot, talked): the step "taken on"
constexpr uint64_t kActors = 0x3483bd0;  // main + : -> the actor manager pointer
constexpr uint64_t kActorOf = 0x281360;  // (actors, &u16 handle): the actor, or 0
constexpr uint64_t kActorAlive = 0x1d8, kActorRequest = 0x408;   // actor vtable + : bool (), the request owner ()
constexpr uint64_t kRequestComp = 0x29a8, kCompId = 0x10;        // owner + : the request component; component + : its u8 id
constexpr uint64_t kCharaTable = 0x129 * 8;              // data-table manager + 0x10 + : the table of slot 0x129, a row per character
constexpr uint32_t kCharaMax = 0x1ff;                    // characters 1..0x1ff
constexpr uint64_t kCharaIds = 2, kCharaIdsEnd = 0x16;   // *(row + 8) + : the ids the character offers
// Requests that are also a story step (LINKDATA #1209 +2 != 0): one completion closes them for the group.
constexpr uint8_t kStoryIds[] = {2, 3, 7, 8, 9, 10, 11, 15, 17, 19, 20, 23, 28, 31, 54, 56, 65, 66, 76, 77, 79, 80};
// Requests the game binds to one player (#1557 +0x2c != 0): never shared.
constexpr uint8_t kOwnIds[] = {94, 95};
constexpr uint32_t kIdMax = 200;         // ids 1..200
constexpr uint8_t kWaiting = 1, kOffered = 2, kTaken = 3, kFulfilled = 4, kDone = 5;
constexpr double kAllEvery = 10.0;       // the whole list again (a member that missed a change)
constexpr double kNoticeGap = 4.0;       // at most one line per this many seconds
constexpr unsigned kApplyPerTick = 4;    // received ids applied in one frame, at most

// A received state that waits for a safe moment.
struct Pending {
    uint8_t there = 0;    // the other member's state (0: nothing waits)
    uint8_t origin = 0;   // that member's node
    bool logged = false;  // its wait for the Pokémon was logged
};

struct State {
    uint8_t seen[kIdMax + 1] = {};   // per id: the state last read here (0: not read yet in this session)
    bool have_seen = false;          // the first read of a session only takes the baseline
    bool send_all = true;            // the whole list at the next read
    double all_at = 0.0, line_at = -1e9;
    alignas(8) uint64_t slot_arg = 0;     // the queue's argument, holding the slot (guest-readable, outlives the call's frame)
    alignas(8) uint16_t handle_arg = 0;   // the actor lookup's argument: a handle
    Pending pending[kIdMax + 1] = {};   // per id: the latest received state
    uint16_t chara[kIdMax + 1] = {};    // per id: the character that offers it (0: none known)
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

// The request component of the Pokémon bound to the slot, found as the take-on step and the completion find it; 0 if none.
uint64_t component(::Ctx* c, uint64_t slot) {
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
    return owner && guest::ok(owner + kRequestComp + kCompId, 1) ? owner + kRequestComp : 0;
}

// Takes request id on here as the game does when its Pokémon loads with a saved 3.
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
            ++n;
        }
    }
    NXLOG_I(kTag, "Co-op requests: %u requests have a known Pokémon", n);
}

// Another id of the Pokémon that offers id (by the table, or the same bound actor) is offered, taken on or fulfilled here: the
// Pokémon's component holds that one.
bool chara_busy(uint64_t recs, uint64_t mu, uint32_t id) {
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
    return false;
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
    const bool first = !s.have_seen;
    s.have_seen = true;
    if (!n) return true;
    if (!link::share(wire::Type::ReqUnique, wire::Writer(link::self()).u8(n).bytes(pairs.data(), pairs.size()).take())) return false;
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t id = pairs[k * 2], state = pairs[k * 2 + 1];
        if (!first && s.seen[id] != state) NXLOG_I(kTag, "Co-op requests: request %u reached state %u here, shared", id, state);
        s.seen[id] = state;
    }
    return true;
}

void line(const std::string& text) {
    State& s = st();
    const double now = qpc::seconds();
    if (now - s.line_at < kNoticeGap) return;
    s.line_at = now;
    notice::post(text);
}

// The state another member's state `there` gives a record at `here` (the rules above).
uint8_t target(uint32_t id, uint8_t there, uint8_t here) {
    if (there == kDone && listed(kStoryIds, id)) return here >= kWaiting && here < kDone ? kDone : here;
    return here >= kWaiting && here < kTaken ? kTaken : here;
}

// Applies up to kApplyPerTick of the waiting states, lowest id first; one notice line for them.
void apply(::Ctx* c, uint64_t recs) {
    State& s = st();
    const uint64_t mu = manager();
    if (!mu) return;
    read_charas(c);
    unsigned applied = 0;
    int taken = -1, finished = -1, closed = -1;   // per kind of change: the node of its first (-1: none)
    for (uint32_t id = 1; id <= kIdMax && applied < kApplyPerTick; ++id) {
        Pending& waiting = s.pending[id];
        if (!waiting.there) continue;
        const uint8_t here = state_of(recs, id);
        const uint8_t to = target(id, waiting.there, here);
        if (to == kTaken && chara_busy(recs, mu, id)) {   // kept: taken on once the Pokémon's other request is done here
            if (!waiting.logged)
                NXLOG_I(kTag, "Co-op requests: request %u at state %u on node %u waits, its Pokémon has another one here", id,
                        waiting.there, waiting.origin);
            waiting.logged = true;
            continue;
        }
        const Pending p = waiting;
        waiting = Pending{};
        if (to == here) continue;
        ++applied;
        if (to == kTaken) {
            take_on(c, recs, mu, id);
        } else {
            const uint64_t slot = mu + id * kSlotSize;
            const uint64_t comp = component(c, slot);   // the completion lets the component go; a raw 5 does too
            if (comp && guest::rd<uint8_t>(comp + kCompId) == id) guest::wr<uint8_t>(comp + kCompId, 0);
            guest::wr<uint8_t>(recs + id * 4, to);
            queue(c, mu, slot);
        }
        if (s.have_seen) s.seen[id] = state_of(recs, id);   // ours now: not sent back as a change
        NXLOG_I(kTag, "Co-op requests: request %u at state %u on node %u, %u here -> %u", id, p.there, p.origin, here, to);
        int& first = to == kDone ? closed : p.there == kDone ? finished : taken;
        if (first < 0) first = p.origin;
    }
    if (taken >= 0) line(notice::name_of(static_cast<uint8_t>(taken)) + " took on a Pokemon request. It is in your requests too!");
    else if (finished >= 0) line(notice::name_of(static_cast<uint8_t>(finished)) + " finished a Pokemon request. You can finish it too!");
    else if (closed >= 0) line(notice::name_of(static_cast<uint8_t>(closed)) + " finished a story request for the group.");
}

}  // namespace

void on_unique_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t n = r.u8();
    if (!r.ok() || !shared_here()) return;
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t id = r.u8(), there = r.u8();
        if (!r.ok()) return;
        if (id < 1 || id > kIdMax || there < kTaken || there > kDone || listed(kOwnIds, id)) continue;
        s.pending[id].there = there;   // the latest wins; a logged wait stays logged (the whole list comes every kAllEvery)
        s.pending[id].origin = origin;
    }
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
    s.send_all = true;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::requests
