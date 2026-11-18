// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the co-op gate (pkp_coop_internal.h). A group's story starts with every member's character made, so no one plays
// ahead alone: while fewer members than the group has ([pkp] coop_members, from the launcher) are in the session, this player's input is
// held (the input lock, pkp_coop_lock.cpp) and a notice says how many are missing. The hold starts once the player can walk (the
// session opens then too) and ends when the channel lists every member; it ends early when the player leaves the world. The gate is
// the story's start only: once this process has seen every member in, a member that leaves holds no one; the others play on with a
// notice that a player left and can rejoin (a rejoining process is gated by its own first wait). coop_members of 0 or 1: the gate
// does nothing.
#include "core/pkp_coop_internal.h"

#include <algorithm>
#include <string>

#include "nxrt/log.h"
#include "nxrt/notices.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::gate {

namespace {

constexpr const char* kTag = "pkp";
constexpr const char* kNotice = "pkp.coop_gate";
constexpr double kNoticeSeconds = 3.0, kNoticeRepeat = 1.0;   // a post lasts 3 s and is renewed every second

struct State {
    bool holding = false;    // the lock is claimed and the notice is up
    bool complete = false;   // the last look found every member in (logged once per change)
    bool started = false;    // every member was in once: the story has started, a later leave is not gated
    int shown = -1;          // how many members were missing in the notice up now
    double posted = 0.0;
};
State& st() {
    static State s;
    return s;
}

void show(State& s, int missing) {
    const double now = qpc::seconds();
    if (missing == s.shown && now - s.posted < kNoticeRepeat) return;
    s.shown = missing;
    s.posted = now;
    notices::post(kNotice, "Waiting for " + std::to_string(missing) + (missing == 1 ? " more player" : " more players") +
                               " to finish their character", kNoticeSeconds);
}

// After the start: a member left. Said once per change of the missing count; nothing is held.
void show_left(State& s, int missing) {
    if (missing == s.shown) return;
    s.shown = missing;
    if (missing > 0)
        notices::post(kNotice, missing == 1 ? "A player left; they can rejoin at any time" :
                                              std::to_string(missing) + " players left; they can rejoin at any time", 6.0);
}

void let_go(State& s) {
    if (!s.holding) return;
    s.holding = false;
    s.shown = -1;
    lock::release(lock::Holder::Gate);
    notices::post(kNotice, "", 0.0);
}

}  // namespace

void tick(bool in_game) {
    State& s = st();
    const int want = members();
    if (want <= 1) return;
    const int present = 1 + (link::in_session() ? static_cast<int>(link::member_count()) : 0);   // this process included
    const int missing = std::max(want - present, 0);
    if (missing == 0) {
        if (!s.complete) NXLOG_I(kTag, "Co-op: every member is in (%d)", want);
        s.complete = true;
        s.started = true;
        let_go(s);
        s.shown = 0;
        return;
    }
    if (s.complete) NXLOG_I(kTag, "Co-op: %d of %d members in%s", want - missing, want, s.started ? "; play goes on" : "");
    s.complete = false;
    if (s.started) {
        show_left(s, missing);
        return;
    }
    const uint64_t me = local_player();
    if (!s.holding) {
        if (!session::can_walk(in_game)) return;
        s.holding = true;
        NXLOG_I(kTag, "Co-op: waiting for %d of %d members; this player's input is held", missing, want);
    } else if (!in_game || !me) {   // loading or at the title: nothing to hold, the claim ends and begins again at the next walk
        let_go(s);
        return;
    }
    lock::hold(lock::Holder::Gate, me);
    show(s, missing);
}

void reset() {
    let_go(st());
    st() = State{};
}

}  // namespace nxrt::games::pkp::coop::gate
