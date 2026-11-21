// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the co-op gate (pkp_coop_internal.h). A new group's story starts with every member in, so no one plays ahead alone:
// while fewer members than the group has ([pkp] coop_members, from the launcher) are in the session, this player's input is held (the
// input lock, pkp_coop_lock.cpp) and a notice says how many are missing. The hold starts once the player can walk (after the
// character is made; the session opens then too) and ends when the channel lists every member; it ends early when the player leaves
// the world. The gate is the start of a new world only: once every member was in, or the world is past its opening
// (story::world_started, saved, so a later boot and a late joiner learn it), an absent member holds no one; the others play with a
// notice that the player can join at any time. A late joiner or a new player taking a free place makes a character as usual and
// catches up through the story, requests, recipes and catch-up parts. coop_members of 0 or 1: the gate does nothing.
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"

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
    notices::post(kNotice,
                  missing == 1 ? strings::get(base(), strings::Str::CoopWaitingOne) : strings::fmt(base(), strings::Str::CoopWaitingMany, missing),
                  kNoticeSeconds);
}

// After the start: members are missing (left, or not here this time). Said in a session, once per change of the missing count;
// nothing is held.
void show_absent(State& s, int missing) {
    if (missing == s.shown || !link::in_session()) return;
    s.shown = missing;
    if (missing > 0)
        notices::post(kNotice,
                      missing == 1 ? strings::get(base(), strings::Str::CoopAbsentOne) : strings::fmt(base(), strings::Str::CoopAbsentMany, missing),
                      6.0);
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
    if (!s.started && session::can_walk(in_game) && story::world_started()) {   // read once the world is in, not at boot
        s.started = true;
        NXLOG_I(kTag, "Co-op: the group's world has started; %d of %d members in, no one is held", want - missing, want);
        let_go(s);
    }
    if (s.started) {
        show_absent(s, missing);
        return;
    }
    const uint64_t me = local_player();
    if (!s.holding) {
        if (!session::can_walk(in_game)) return;
        s.holding = true;
        NXLOG_I(kTag, "Co-op: waiting for %d of %d members; this player's input is held", missing, want);
    } else if (!me) {   // at the title or loading: no character to hold, the claim ends and begins again at the next walk
        let_go(s);
        return;
    }
    // Kept while in_game dips: a controller/keyboard prompt switch stops input for ~135 ms (in_game false), and a claim let go then
    // let the held stick move the character (the field host, 300 switches). A world load changes the character; lock::hold moves the
    // claim to the new one.
    lock::hold(lock::Holder::Gate, me);
    if (in_game) show(s, missing);
}

void reset() {
    let_go(st());
    st() = State{};
}

}  // namespace nxrt::games::pkp::coop::gate
