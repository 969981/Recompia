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
//   Every state change is the game's own (the talk, the frame checkers); this part only writes byte +0, never calls a step.
// Rules on a receiver, so nothing is undone and no history is invented:
//   - another member's 3 or 4: a record here below 3 becomes 3 (listed; the game's own checker and talk decide the rest here);
//   - another member's 5 (the request finished): a record here at 3 or 4 becomes 5 (closed for the group, as a shared story step is);
//     a record that was never taken on here stays as it is.
// Each process sends a change when it sees one (the records are read every kPoll seconds) and its whole list of taken-on and
// finished requests every kAllEvery seconds and when it or another member joins, through the authority to every member.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstring>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::requests {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kUnique = 0x6fabd9;   // save root + : the unique request records, 4 bytes each, by id
constexpr uint32_t kIdMax = 200;         // ids 1..200
constexpr uint8_t kWaiting = 1, kTaken = 3, kFulfilled = 4, kDone = 5;
constexpr double kPoll = 0.25;           // seconds between two reads of the records
constexpr double kAllEvery = 10.0;       // the whole list again (a member that missed a change)
constexpr double kNoticeGap = 4.0;       // at most one line per this many seconds

struct State {
    uint8_t seen[kIdMax + 1] = {};   // per id: the state last read here (0: not read yet in this session)
    bool have_seen = false;          // the first read of a session only takes the baseline
    bool send_all = true;            // the whole list at the next read
    double poll_at = 0.0, all_at = 0.0, line_at = -1e9;
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

bool shared_here() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }

// Sends (u8 id, u8 state) for every id whose state rose to 3..5 since the last read, or (all) every id at 3..5.
void share(uint64_t recs, bool all) {
    State& s = st();
    std::vector<uint8_t> pairs;
    uint8_t n = 0;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        const uint8_t state = state_of(recs, id);
        const bool rose = s.have_seen && state != s.seen[id];
        s.seen[id] = state;
        if (state < kTaken || state > kDone || !(all || rose)) continue;
        pairs.push_back(static_cast<uint8_t>(id));
        pairs.push_back(state);
        ++n;
        if (!all) NXLOG_I(kTag, "Co-op requests: request %u reached state %u here, shared", id, state);
    }
    s.have_seen = true;
    if (n) link::share(wire::Type::ReqUnique, wire::Writer(link::self()).u8(n).bytes(pairs.data(), pairs.size()).take());
}

void line(const std::string& text) {
    State& s = st();
    const double now = qpc::seconds();
    if (now - s.line_at < kNoticeGap) return;
    s.line_at = now;
    notice::post(text);
}

}  // namespace

void on_unique_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t n = r.u8();
    const uint64_t recs = records();
    if (!r.ok() || !recs || !shared_here()) return;
    unsigned taken = 0, done = 0;
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t id = r.u8(), there = r.u8();
        if (!r.ok()) return;
        if (id < 1 || id > kIdMax) continue;
        const uint8_t here = state_of(recs, id);
        uint8_t to = here;
        if ((there == kTaken || there == kFulfilled) && here >= kWaiting && here < kTaken) to = kTaken;
        else if (there == kDone && (here == kTaken || here == kFulfilled)) to = kDone;
        if (to == here) continue;
        guest::wr<uint8_t>(recs + id * 4, to);
        if (s.have_seen) s.seen[id] = to;   // ours now: not sent back as a change
        NXLOG_I(kTag, "Co-op requests: request %u at state %u on node %u, %u here -> %u", id, there, origin, here, to);
        if (to == kTaken) ++taken;
        else ++done;
    }
    if (taken) line(notice::name_of(origin) + " took on a Pokemon request. It is in your requests too!");
    else if (done) line(notice::name_of(origin) + " finished a Pokemon request for the group.");
}

void tick() {
    State& s = st();
    const uint64_t recs = records();
    if (!recs || !shared_here()) return;
    const double now = qpc::seconds();
    if (now - s.poll_at < kPoll) return;
    s.poll_at = now;
    bool all = s.send_all || now - s.all_at >= kAllEvery;
    if (all) {
        s.send_all = false;
        s.all_at = now;
    }
    share(recs, all);
}

void on_session_change() {
    State& s = st();
    std::memset(s.seen, 0, sizeof s.seen);
    s.have_seen = false;
    s.send_all = true;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::requests
