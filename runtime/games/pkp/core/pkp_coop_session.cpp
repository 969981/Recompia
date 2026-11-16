// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the co-op session, opened by role (pkp_coop_internal.h). No menu: the host role opens a Link Play session, the visit
// role searches and joins it, by the same calls the multiplayer screen makes, as soon as the player is in the world. The PC's Link Play
// entry is locked early in the story; these calls do not look at that lock, so a group can start in the first cave.
//
// The game's session manager SM = *(*(main+0x3561780) + 0x11f0) exists from boot. u32 SM+0 is its state (0 idle, else the step of the
// request it works on); every request below refuses (returns 0) unless SM is idle, and returns 1 when it took the request.
//   0xe7d644(SM)       open a session with the code at SM+4 (empty: no code). SM saves the game first, then hosts; GM+0x90 bit 1 (host)
//                      is set when it is up, about 5 to 7 s later
//   0xe7d8d0(SM, 0)    a fresh search. Done when SM is idle again with SM+0x1a7c bit 4 set; u32 SM+0x1358 is the number of results.
//                      A search before the host is up finds nothing and leaves SM idle
//   0xe7ffc8(index)    join result `index`; GM+0x90 bit 2 (visitor) is set once the host's world has loaded, 10 to 20 s later
// A request is made only while the player can walk: play mode's in_game, the event bus not busy and the autosave idle. The game state
// GM = layout::kGameState, its session bits at layout::kStateSession.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::session {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kSessionOwner = 0x3561780, kOwnerSession = 0x11f0;   // main: -> the owner of SM; owner + 0x11f0 -> SM
constexpr uint64_t kOpen = 0xe7d644, kSearch = 0xe7d8d0, kJoin = 0xe7ffc8;
constexpr uint64_t kSmState = 0x0, kSmCode = 0x4, kSmResults = 0x1358, kSmFlags = 0x1a7c, kSmSize = 0x1a80;
constexpr uint32_t kSmSearched = 1u << 4;

constexpr double kFirstWait = 3.0, kMaxWait = 30.0;      // backoff between tries
constexpr double kOpenTimeout = 30.0, kSearchTimeout = 15.0, kJoinTimeout = 45.0;

enum class Step { Wait, Opening, Searching, Joining, Up };

struct State {
    Step step = Step::Wait;
    double next_try = 0.0, started = 0.0, backoff = kFirstWait;
    unsigned tries = 0;
};
State& st() {
    static State s;
    return s;
}

const char* role_name() { return role() == CoopRole::Host ? "host" : "visit"; }

uint64_t manager() {
    const uint64_t owner = guest::rd<uint64_t>(base() + kSessionOwner);
    if (!guest::ok(owner, kOwnerSession + 8)) return 0;
    const uint64_t sm = guest::rd<uint64_t>(owner + kOwnerSession);
    return guest::ok(sm, kSmSize) ? sm : 0;
}

// The player walks: in the world, no event running, no autosave in progress.
bool can_walk(bool in_game) {
    if (!in_game || !local_player()) return false;
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(game, layout::kStateAutosave + 4)) return false;
    const uint32_t autosave = guest::rd<uint32_t>(game + layout::kStateAutosave);
    if (autosave - 1 < 5) return false;
    const uint64_t event = guest::rd<uint64_t>(game + layout::kStateEvent);
    return !(event && guest::ok(event, layout::kEventBusy + 1) && guest::rd<uint8_t>(event + layout::kEventBusy));
}

void retry_later(State& s, double now, const char* why) {
    s.step = Step::Wait;
    s.next_try = now + s.backoff;
    NXLOG_I(kTag, "Co-op session (%s): %s; trying again in %.0f s", role_name(), why, s.backoff);
    s.backoff = std::min(s.backoff * 2.0, kMaxWait);
}

void start(::Ctx* c, State& s, uint64_t sm, double now) {
    ++s.tries;
    if (role() == CoopRole::Host) {
        guest::wr<uint8_t>(sm + kSmCode, 0);   // no code: the private relay room keeps other groups out
        if (!(guest::call(c, base() + kOpen, sm) & 1)) return retry_later(s, now, "the session manager refused to open a session");
        s.step = Step::Opening;
        NXLOG_I(kTag, "Co-op session (host): opening the session (try %u)", s.tries);
    } else {
        if (!(guest::call(c, base() + kSearch, sm, 0) & 1)) return retry_later(s, now, "the session manager refused to search");
        s.step = Step::Searching;
        NXLOG_I(kTag, "Co-op session (visit): searching for the host's session (try %u)", s.tries);
    }
    s.started = now;
}

// A finished search: joins the first result (the relay room holds only this group's session), or searches again later.
void searched(::Ctx* c, State& s, uint64_t sm, double now) {
    const uint32_t results = guest::rd<uint32_t>(sm + kSmResults);
    if (!(guest::rd<uint32_t>(sm + kSmFlags) & kSmSearched) || results == 0) return retry_later(s, now, "no session found yet");
    if (!(guest::call(c, base() + kJoin, 0) & 1)) return retry_later(s, now, "the session manager refused to join");
    s.step = Step::Joining;
    s.started = now;
    NXLOG_I(kTag, "Co-op session (visit): joining the host's session (%u found)", results);
}

}  // namespace

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    const double now = qpc::seconds();
    const bool online = session_bits() & layout::kSessionOnline;
    if (online) {
        if (s.step != Step::Up) NXLOG_I(kTag, "Co-op session (%s): the session is up (session bits %02x)", role_name(), session_bits());
        s.step = Step::Up;
        s.backoff = kFirstWait;
        return;
    }
    if (s.step == Step::Up) {
        s.step = Step::Wait;
        s.next_try = now + kFirstWait;
        NXLOG_W(kTag, "Co-op session (%s): the session ended; starting it again once the player can walk", role_name());
        return;
    }
    const uint64_t sm = manager();
    if (!sm) return;
    const bool idle = guest::rd<uint32_t>(sm + kSmState) == 0;
    switch (s.step) {
        case Step::Wait:
            if (now >= s.next_try && idle && can_walk(in_game)) start(c, s, sm, now);
            return;
        case Step::Opening:
            if (now - s.started > kOpenTimeout) retry_later(s, now, "the session did not come up");
            return;
        case Step::Searching:
            if (idle) searched(c, s, sm, now);
            else if (now - s.started > kSearchTimeout) retry_later(s, now, "the search did not finish");
            return;
        case Step::Joining:
            if (now - s.started > kJoinTimeout) retry_later(s, now, "the join did not finish");
            return;
        case Step::Up: return;
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::session
