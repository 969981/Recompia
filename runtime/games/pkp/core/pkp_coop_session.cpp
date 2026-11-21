// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the co-op session, opened by role (pkp_coop_internal.h). No menu: the host role opens a Link Play session, the visit
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
//
// A joining member arrives at the host world's PC, wherever the host is (in a new game the host is in the first cave, which cannot be
// reached from there). Once the member can walk again after the load (play mode's in_game comes back about 13 s after the visitor bit,
// after the game's own last placing), the game's player warp 0x275760(character, vec4* pos, 0, 1, 1, 1, 0, 0, [sp] u8 0) puts it on the
// host's spot, and the game moves the two a little apart. In a session of two the host's character is the player manager's second slot
// (+0x38). A second later it looks again and warps once more while still far from the host (at most kPlaceTries warps).
//
// A visitor's search that finds nothing (the host not up yet) is made again every kSearchWait, not on the growing backoff: a search costs
// nothing, and a visitor started before the host would otherwise wait up to kMaxWait after the host is up.
// The curtain (pkp_coop_curtain.cpp) covers a visitor's travel: from the join request until it can walk in the host's world (at most
// kArrivalCover after the session is up: a dialog of the game's may wait for A there, so a hint says so from kHintAfter), and again
// around the warp, which waits for the cover to be opaque (at most kWarpWait). When the session ends it covers the trip home until the
// player's world has loaded and the player is back in play (at most kLeaveCover; the game's own error popup when the host quits waits
// for A under it, so the hint shows at once).
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cstring>

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
constexpr double kSearchWait = 2.0;                      // between a visitor's searches that found nothing
constexpr double kArrivalCover = 16.0, kHintAfter = 6.0, kWarpWait = 1.0, kWarpHold = 0.6, kLeaveCover = 6.0;
constexpr double kOpenTimeout = 30.0, kSearchTimeout = 15.0, kJoinTimeout = 45.0;

constexpr uint64_t kWarp = 0x275760;
constexpr uint64_t kOtherPlayer = layout::kManagerPlayer + 8;
constexpr unsigned kPlaceTries = 3;
constexpr double kPlaceCheck = 1.0;
constexpr float kNearHost = 300.0f;   // world units; the game puts the two about 60 apart after a warp

enum class Step { Wait, Opening, Searching, Joining, Up };
enum class Place { Loading, Ready, Check, Done };

struct State {
    Step step = Step::Wait;
    double next_try = 0.0, started = 0.0, backoff = kFirstWait;
    unsigned tries = 0;
    // A joining member's placement next to the host.
    Place place = Place::Loading;
    bool loaded = false;   // in_game went away during the join (the host's world loads)
    double check_at = 0.0, up_at = 0.0, ready_at = 0.0;
    unsigned warps = 0;
    // The trip home after the session ended (visit role): covered until the player is back in play.
    bool leaving = false, left_world = false;
    double left_at = 0.0;
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

void retry_later(State& s, double now, const char* why, bool backoff = true) {
    if (s.step == Step::Joining) curtain::lift();
    s.step = Step::Wait;
    const double wait = backoff ? s.backoff : kSearchWait;
    s.next_try = now + wait;
    NXLOG_I(kTag, "Co-op session (%s): %s; trying again in %.0f s", role_name(), why, wait);
    if (backoff) s.backoff = std::min(s.backoff * 2.0, kMaxWait);
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
    const bool found = (guest::rd<uint32_t>(sm + kSmFlags) & kSmSearched) && results != 0;
    drop::on_search(found);   // a member that lost its session and finds none closes (pkp_coop_drop.cpp)
    if (!found) return retry_later(s, now, "no session found yet", false);
    if (!(guest::call(c, base() + kJoin, 0) & 1)) return retry_later(s, now, "the session manager refused to join");
    s.step = Step::Joining;
    s.started = now;
    curtain::cover(strings::get(base(), strings::Str::CoopJoining));
    NXLOG_I(kTag, "Co-op session (visit): joining the host's session (%u found)", results);
}

uint64_t host_player() {
    const uint64_t players = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    if (!guest::ok(players, kOtherPlayer + 8)) return 0;
    const uint64_t host = guest::rd<uint64_t>(players + kOtherPlayer);
    return guest::ok(host, layout::kPosition + 16) ? host : 0;
}

bool placed(const float* p) { return p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f; }

// The squared distance between the two characters' positions; negative while either has none yet.
float distance2(uint64_t me, uint64_t host) {
    float a[4], b[4];
    std::memcpy(a, reinterpret_cast<const void*>(me + layout::kPosition), sizeof a);
    std::memcpy(b, reinterpret_cast<const void*>(host + layout::kPosition), sizeof b);
    if (!placed(a) || !placed(b)) return -1.0f;
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

void warp_to_host(::Ctx* c, uint64_t me, uint64_t host) {
    const uint64_t sp = (c->x[31] - 0x100) & ~uint64_t{0xf};   // below the game thread's stack: free at the frame tick
    guest::wr<uint64_t>(sp, 0);                                 // the stack argument (u8 0)
    std::memcpy(reinterpret_cast<void*>(sp + 0x10), reinterpret_cast<const void*>(host + layout::kPosition), 16);
    guest::call_n<8>(c, base() + kWarp, {me, sp + 0x10, 0, 1, 1, 1, 0, 0}, sp);
}

// A joining member, once the session is up: next to the host as soon as it can walk again.
void place(::Ctx* c, State& s, bool in_game, double now) {
    if (s.place == Place::Done) return;
    if (!in_game) s.loaded = true;
    if (s.place == Place::Loading) {
        if (s.loaded && in_game) {
            s.place = Place::Ready;
            s.ready_at = now;
            curtain::cover(strings::get(base(), strings::Str::CoopJoining));   // up already, or back for the warp
        } else if (now - s.up_at > kArrivalCover) {
            curtain::lift();
        } else if (now - s.up_at > kHintAfter) {
            curtain::hint(strings::get(base(), strings::Str::CoopTakingLong));
        }
        return;
    }
    if (now - s.ready_at > kArrivalCover) curtain::lift();   // no host to warp to: the cover does not stay
    const uint64_t me = local_player(), host = host_player();
    if (!me || !host) return;
    const float d2 = distance2(me, host);
    if (d2 < 0.0f) return;
    if (s.place == Place::Check && now < s.check_at) return;
    if (d2 <= kNearHost * kNearHost || s.warps >= kPlaceTries) {
        s.place = Place::Done;
        curtain::lift(kWarpHold);
        NXLOG_I(kTag, "Co-op session (visit): %s the host (%u warps)", d2 <= kNearHost * kNearHost ? "next to" : "still far from", s.warps);
        return;
    }
    if (s.place == Place::Ready && !curtain::covered() && now - s.ready_at < kWarpWait) return;   // the warp is not seen
    warp_to_host(c, me, host);
    ++s.warps;
    s.place = Place::Check;
    s.check_at = now + kPlaceCheck;
    NXLOG_I(kTag, "Co-op session (visit): warped to the host (try %u)", s.warps);
}

}  // namespace

bool can_walk(bool in_game) {
    if (!in_game || !local_player()) return false;
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(game, layout::kStateAutosave + 4)) return false;
    const uint32_t autosave = guest::rd<uint32_t>(game + layout::kStateAutosave);
    if (autosave - 1 < 5) return false;
    const uint64_t event = guest::rd<uint64_t>(game + layout::kStateEvent);
    return !(event && guest::ok(event, layout::kEventBusy + 1) && guest::rd<uint8_t>(event + layout::kEventBusy));
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    const double now = qpc::seconds();
    const bool online = session_bits() & layout::kSessionOnline;
    if (online) {
        if (s.step != Step::Up) {
            NXLOG_I(kTag, "Co-op session (%s): the session is up (session bits %02x)", role_name(), session_bits());
            s.up_at = now;
            if (s.leaving) curtain::lift();
            s.leaving = false;
        }
        s.step = Step::Up;
        s.backoff = kFirstWait;
        if (role() == CoopRole::Visit) place(c, s, in_game, now);
        return;
    }
    if (s.step == Step::Up) {
        s.step = Step::Wait;
        s.place = Place::Loading;
        s.loaded = false;
        s.warps = 0;
        s.next_try = now + kFirstWait;
        if (role() == CoopRole::Visit) {
            s.leaving = true;
            s.left_world = false;
            s.left_at = now;
            curtain::cover(strings::get(base(), strings::Str::CoopLeaving));
            curtain::hint(strings::get(base(), strings::Str::CoopTakingLong));
        }
        NXLOG_W(kTag, "Co-op session (%s): the session ended; starting it again once the player can walk", role_name());
        return;
    }
    if (s.leaving) {
        if (!in_game) s.left_world = true;
        if ((s.left_world && in_game) || now - s.left_at > kLeaveCover) {
            s.leaving = false;
            curtain::lift(s.left_world && in_game ? kWarpHold : 0.0);
        }
    }
    const uint64_t sm = manager();
    if (!sm) return;
    const bool idle = guest::rd<uint32_t>(sm + kSmState) == 0;
    switch (s.step) {
        case Step::Wait:
            if (drop::closing()) return;   // a member's game is about to close: no new session
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
            if (!in_game) s.loaded = true;
            if (now - s.started > kJoinTimeout) retry_later(s, now, "the join did not finish");
            return;
        case Step::Up: return;
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::session
