// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a newly met creature moving in (pkp_coop_internal.h). A creature no player has befriended yet (its
// 0x180-byte friend record's "parents" user id +0x8..+0x17 is null) asks at its first talk whether it may live here, and the talk's
// end makes it the town's resident. Outside Cloud Island that end lands only for the host: a member's talk leaves the creature new.
//
// The game, 2.0.1:
//   0x4c024c        the talk handler: a creature with no parents (0x285100 false, 0x4c0618) takes the online host / visitor split
//                   at 0x4c06d4 (IsHost); pkp_coop_talk.cpp's TalkKind (midhook 0x4c06d8) already gives a member the host's talk.
//   0x481e2c        the talk event's end step (event runner state 5) calls, at 0x48235c, 0x3b9674(comp, &own user id R+0x13d3a89, 0,
//                   1) (comp = the creature's component: actor vt+0x408 + 0x2ca0, or the stand-in main+0x3567db0), then the dex step
//                   0x3b88b4 (pkp_coop_discovery.cpp's part).
//   0x3b9674(comp, &uid, w2, w3)  the befriend. For a creature with no parents (0x3b990c): a visitor outside Cloud Island whose
//                   AIState (R+0x7b26e8 + chara*0x1e0) byte +0xa is 0xc returns at once; w2 == 0 with 0x3b87f4 (own record +0x173
//                   set) returns; then 0x296770(comp, &uid); then, only on the host, offline, with GM+0x11 bit 0 or on Cloud Island
//                   (0x3b996c..0x3b9998), the resident block 0x3b999c..0x3b9afc (comp +0x10 = 1, the character status
//                   0x2a680c(actor, 1), the record's met date, +0x17c, 0x3b7630, 0x3b5a14); then 0x3b9b40(comp, w3 & 1, 0).
//   0x296770(comp, &uid)  the parents: returns for a null uid or a record that has parents. The host (or offline, or GM+0x11) writes
//                   record +0x8 = uid (0x296afc) and raises GameReplication kind 0xb (0x296b44: 0x4b1d54(GM+0x3bb620, 0xb, chara, 0)),
//                   whose member reader 0x57e3d4 copies +0x8..+0x1a into each member's copy. A member writes nothing, and sends the
//                   game's RequestSetNPCParents (host handler 0x54c5bc, which only calls 0x296770) only for the special characters
//                   (idx +0x39f2 in 0x400..0x40e) on Cloud Island (w22 at 0x2968d8, the test at 0x296b4c).
//   0x3b9b40(comp, w1, w2)  the settle step (the AIState reset, the moves the creature teaches 0x5d1620 (a set bit: once), +0x17c).
//                   A visitor whose actor +0x20 +0x264 is set only marks the talk delta +0x248 bit 2 (0x3b9c78), which the talk-end
//                   forward carries to the host, whose apply 0x2197a0 runs 0x3b9b40(comp, 1, !Cloud Island) (0x21badc). That is all
//                   of a member's talk that reaches the host: the parents and the resident block never do, so the host's creature
//                   stays new (it greets the host as new again) and no member's copy ever gets the parents.
//
// Here, a member in the session outside Cloud Island (quests::member_rules()): at 0x48235c (midhook on the `bl 0x3b9674`, x0 = comp)
// in a talk this player holds, for a creature its copy has no parents for, the creature (actor handle comp +0x8, chara comp +0xc, the
// stage) is queued for the authority and sent once the talk has ended (or after kSendWait). The member's own call runs as before.
// The authority, at a safe moment (same stage, can walk, no talk of its own), finds the creature (0x333f10 on the handle: its
// component, or the stand-in when it is not loaded here: dropped; the chara must match) and, when it still has no parents (the
// "once": another member's or the host's own talk got there first), makes the host's own end-step call 0x3b9674(comp, &its own user
// id, 0, 1). The creature is then the host's resident, as when the host talks to it, and its parents reach every member by kind 0xb.
// The game's own forward runs 0x3b9b40 on the host a second time (in either order); with w2 = 1 that run skips 0x1be640, and the
// rest of it sets state and once-bits (0x5d1620 returns when the bit is set already).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::newcomer {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kComponentOf = 0x333f10;   // (&u16 handle) -> the creature's component, or main + kNoComponent
constexpr uint64_t kNoComponent = 0x3567db0;
constexpr uint64_t kHasParents = 0x285100;    // (comp) -> bit 0: its record (as this process selects it) has a parents user id
constexpr uint64_t kBefriend = 0x3b9674;      // (comp, &u8[16] user id, w2, w3): the talk end step's call
constexpr uint64_t kCompHandle = 8, kCompChara = 0xc;
constexpr double kSendWait = 30.0;   // a member's newcomer is sent at the latest this long after the talk's end step
constexpr double kDrop = 120.0;      // the authority found no safe moment for it
constexpr size_t kQueue = 16;

struct Newcomer {
    uint16_t handle;
    uint16_t chara;
    uint8_t stage;
    uint8_t origin;   // the authority: the member's node
    double at;
};

struct State {
    std::vector<Newcomer> outgoing;   // a member's newcomers not sent yet
    std::vector<Newcomer> incoming;   // the authority's received newcomers not applied yet
    alignas(8) uint16_t handle_arg = 0;                     // 0x333f10's argument (guest addresses are host addresses)
    alignas(8) uint8_t uid_arg[layout::kUserIdSize] = {};   // 0x3b9674's user id
};
State& st() {
    static State s;
    return s;
}

uint8_t stage() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kStateStage + 1) ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}

void send() {
    State& s = st();
    const double now = qpc::seconds();
    size_t sent = 0;
    for (const Newcomer& n : s.outgoing) {
        if (talk::talking() && now - n.at < kSendWait) break;   // after this player's talk, in order
        if (!link::to_authority(wire::Type::NewcomerAsk, wire::Writer(link::self()).u16(n.handle).u16(n.chara).u8(n.stage).take()))
            break;   // kept for the next frame
        NXLOG_I(kTag, "Co-op newcomer: chara %u (actor %04x) met for the first time here, sent to the authority", n.chara, n.handle);
        ++sent;
    }
    s.outgoing.erase(s.outgoing.begin(), s.outgoing.begin() + static_cast<std::ptrdiff_t>(sent));
}

// The authority: the host's own end-step call for the creature. False when the creature is not here.
bool settle(::Ctx* c, const Newcomer& n) {
    State& s = st();
    s.handle_arg = n.handle;
    const uint64_t comp = guest::call(c, base() + kComponentOf, reinterpret_cast<uint64_t>(&s.handle_arg));
    if (!comp || comp == base() + kNoComponent || !guest::ok(comp, kCompChara + 4) || guest::rd<uint32_t>(comp + kCompChara) != n.chara)
        return false;
    if (guest::call(c, base() + kHasParents, comp) & 1) {
        NXLOG_I(kTag, "Co-op newcomer: node %u's first talk with chara %u: it lives here already", n.origin, n.chara);
        return true;
    }
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (!r || !guest::ok(r + layout::kOwnUserId, layout::kUserIdSize)) return false;
    for (size_t i = 0; i < layout::kUserIdSize; ++i) s.uid_arg[i] = guest::rd<uint8_t>(r + layout::kOwnUserId + i);
    const uint64_t ret = guest::call(c, base() + kBefriend, comp, reinterpret_cast<uint64_t>(s.uid_arg), 0, 1);
    NXLOG_I(kTag, "Co-op newcomer: node %u's first talk with chara %u (actor %04x) made here: returned %u, has parents now: %s",
            n.origin, n.chara, n.handle, static_cast<unsigned>(ret & 1),
            (guest::call(c, base() + kHasParents, comp) & 1) ? "yes" : "no");
    return true;
}

void apply(::Ctx* c, bool in_game) {
    State& s = st();
    if (s.incoming.empty()) return;
    const uint8_t here = stage();
    const double now = qpc::seconds();
    for (size_t i = 0; i < s.incoming.size();) {
        const Newcomer& n = s.incoming[i];
        if (n.stage != here || now - n.at >= kDrop) {
            NXLOG_I(kTag, "Co-op newcomer: node %u's first talk with chara %u dropped (%s)", n.origin, n.chara,
                    n.stage != here ? "another stage here" : "no safe moment");
            s.incoming.erase(s.incoming.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    if (s.incoming.empty() || !session::can_walk(in_game) || talk::talking()) return;
    const Newcomer n = s.incoming.front();
    s.incoming.erase(s.incoming.begin());
    if (!settle(c, n))
        NXLOG_I(kTag, "Co-op newcomer: node %u's first talk with chara %u dropped (the creature is not loaded here)", n.origin, n.chara);
}

}  // namespace

int on_befriend(::Ctx* c) {
    // Midhook 0x48235c in 0x481e2c, on its `bl 0x3b9674`: x0 = the creature's component (or the stand-in), x1 = &own user id.
    if (!quests::member_rules() || stage() == layout::kStageCloudIsland) return 0;
    const uint64_t comp = c->x[0];
    if (comp == base() + kNoComponent || !guest::ok(comp, kCompChara + 4)) return 0;
    const uint32_t chara = guest::rd<uint32_t>(comp + kCompChara);
    if (chara >= layout::kFriendCount) return 0;
    if (guest::call(c, base() + kHasParents, comp) & 1) return 0;   // befriended already: 0x3b9674 takes its other path
    if (!talk::talking()) {   // an event this player did not start by a talk: kept out, as pkp_coop_friendship.cpp keeps its gain
        NXLOG_I(kTag, "Co-op newcomer: chara %u's first-talk end ran here, but this player holds no talk: not sent", chara);
        return 0;
    }
    State& s = st();
    if (s.outgoing.size() < kQueue)
        s.outgoing.push_back(
            Newcomer{guest::rd<uint16_t>(comp + kCompHandle), static_cast<uint16_t>(chara), stage(), link::self(), qpc::seconds()});
    return 0;   // the member's own call runs on
}

void on_ask_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    const uint16_t handle = r.u16(), chara = r.u16();
    const uint8_t at = r.u8();
    State& s = st();
    if (!r.ok() || !link::is_authority() || chara >= layout::kFriendCount || at == layout::kStageCloudIsland) return;
    for (const Newcomer& n : s.incoming)
        if (n.chara == chara) return;   // asked already
    if (s.incoming.size() >= kQueue) return;
    s.incoming.push_back(Newcomer{handle, chara, at, origin, qpc::seconds()});
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c) return;
    if (!link::in_session()) {
        s.outgoing.clear();
        s.incoming.clear();
        return;
    }
    if (link::is_authority()) {
        s.outgoing.clear();
        apply(c, in_game);
    } else {
        s.incoming.clear();
        send();
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::newcomer
