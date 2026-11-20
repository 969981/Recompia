// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's creature events (pkp_coop_internal.h): what a member's player did to a creature, fed to the authority's
// contents scheduler, so the group's story reacts to it once, on the authority. RE: modding-docs coop-v2/quests/QUEST-START-RE.md
// (sections 2, 4, 5a), coop-v2/P0-RE.md (talk start), coop-v2/role-census/verify/v0.md.
//
// The contents scheduler SC = GM+0xd8 runs the contents scripts (the story's per-character behaviour). A notifier writes the event
// into SC's notification block (u32 kind at SC+0x11aae0, its payload SC+0x11aaa0..), calls 0x1353a0(SC, the kind's list, p3, p4),
// which evaluates that list's records now (an event condition is true only inside that call), and clears the block. A quest whose
// condition is the contents (type 0x15) is stepped by the authority's own pump (pkp_coop_quests.cpp), so the authority's SC is the
// one whose records count for the group; what they do reaches every member through the story's sharing.
//
// Kind 1, "talked to": 0x1355ec(SC, &u16 actor id). Its only caller is the A press 0x31347c, right after the talk start 0x3180e0
// claimed the talk (0x314058 `tbz w0` skips it otherwise). It posts only when 0x286de4(&id) (a resident whose idx +0x39f2 is not in
// the special range 0x400..0x415), not GM+0x90 bit 2 (a Link Play visitor: every co-op member), not on Cloud Island (0x77ecb0:
// GM+0x18 == 0x11), and the data id 0x27d848(&id) is 1..0x3ff; it then writes the data id at SC+0x11aaa0, the kind at +0x11aae0
// and calls 0x1353a0(SC, SC+0x88ed8, 1, 0). So a member's talk never reached any scheduler: content records waiting on "talked to
// X" (quest starts and steps) never ran for it. Nothing else forwards the event (the talk-end record carries the item and request steps of
// 0x46e308, not kind 1), so it reaches the authority once, from here.
//   0x1355ec (entry hook), on a member in the session (quests::member_rules()): when the game's own tests other than the visitor bit
//   pass (0x286de4 called, the stage compared, 0x27d848 called), the event (kind, this stage, the data id) is queued for the
//   authority, and sent once this player's talk has ended (the creature's ownership is back by then, so a scene the record queues on
//   the authority is not refused for it) or after kSendWait. The game's own code runs on and returns at its visitor test, as before.
//   On the authority the event waits for a safe moment (in the same stage, can walk, no talk of its own), then the game's own 0x1355ec
//   runs with the authority's copy of that character: 0x28cc60(player manager, data id), the first live resident of that data id
//   outside the special idx range (the same rule 0x286de4 applies), and its u16 actor id at actor +0x27fe (0x597820 passes that
//   address to the triad notifier 0x136548 as &actor id). The scheduler keys on the data id alone, so which of two residents of one
//   data id is found does not change the event. One a frame (a record may queue a scene); dropped after kDrop or on a stage change.
// The wire carries any creature event kind (wire::Type::CreatureEvent); a kind this build does not post is dropped by the authority.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::feed {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kTalkedTo = 0x1355ec;    // (SC, &u16 actor id) -> 1 when a record was queued
constexpr uint64_t kResident = 0x286de4;    // (&u16 actor id) -> a resident whose idx is outside 0x400..0x415
constexpr uint64_t kDataIdOf = 0x27d848;    // (&u16 actor id) -> u16 the character's data id
constexpr uint64_t kFirstOf = 0x28cc60;     // (player manager, data id) -> the first live resident of that data id, or 0
constexpr uint64_t kActorId = 0x27fe;       // resident actor + : u16 its actor id
constexpr uint64_t kScheduler = 0xd8;       // game state + : the contents scheduler
constexpr uint16_t kDataIdMax = 0x3ff;      // 0x1355ec's own bound: 1..0x3ff
constexpr uint8_t kTalkedToKind = 1;        // the scheduler's kind for "talked to"
constexpr double kSendWait = 30.0;          // a member's event is sent at the latest this long after the talk started
constexpr double kDrop = 120.0;             // an event the authority found no safe moment for (its own talks can be long)
constexpr size_t kQueue = 16;

struct Event {
    uint8_t kind;
    uint8_t stage;
    uint16_t data;   // the creature's data id
    uint16_t arg;    // kind 1: 0
    double at;
    uint8_t origin;  // the authority: the member's node
};

struct State {
    std::vector<Event> outgoing;   // a member's events not sent yet
    std::vector<Event> incoming;   // the authority's received events not posted yet
};
State& st() {
    static State s;
    return s;
}

uint64_t game_state() { return guest::rd<uint64_t>(base() + layout::kGameState); }
uint8_t stage() {
    const uint64_t gm = game_state();
    return guest::ok(gm, layout::kStateStage + 1) ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}

void send() {
    State& s = st();
    if (s.outgoing.empty()) return;
    const double now = qpc::seconds();
    size_t sent = 0;
    for (const Event& e : s.outgoing) {
        if (talk::talking() && now - e.at < kSendWait) break;   // after this player's talk, in order
        if (!link::to_authority(wire::Type::CreatureEvent,
                                wire::Writer(link::self()).u8(e.kind).u8(e.stage).u16(e.data).u16(e.arg).take()))
            break;   // kept for the next frame
        NXLOG_I(kTag, "Co-op feed: talked to character %u (stage %u), sent to the authority", e.data, e.stage);
        ++sent;
    }
    s.outgoing.erase(s.outgoing.begin(), s.outgoing.begin() + static_cast<std::ptrdiff_t>(sent));
}

// The authority: the game's own 0x1355ec for its copy of the character. False when it has none.
bool post(::Ctx* c, const Event& e) {
    const uint64_t gm = game_state();
    const uint64_t sched = guest::ok(gm, kScheduler + 8) ? guest::rd<uint64_t>(gm + kScheduler) : 0;
    const uint64_t pm = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    if (!sched || !pm) return false;
    const uint64_t actor = guest::call(c, base() + kFirstOf, pm, e.data);
    if (!actor || !guest::ok(actor + kActorId, 2)) return false;
    const uint64_t ran = guest::call(c, base() + kTalkedTo, sched, actor + kActorId);
    NXLOG_I(kTag, "Co-op feed: node %u talked to character %u; posted to the contents here (actor 0x%x), a record ran: %s", e.origin,
            e.data, guest::rd<uint16_t>(actor + kActorId), (ran & 1) ? "yes" : "no");
    return true;
}

void apply(::Ctx* c, bool in_game) {
    State& s = st();
    if (s.incoming.empty()) return;
    const uint8_t here = stage();
    const double now = qpc::seconds();
    for (size_t i = 0; i < s.incoming.size();) {
        const Event& e = s.incoming[i];
        if (e.stage != here || now - e.at >= kDrop) {
            NXLOG_I(kTag, "Co-op feed: node %u's talk to character %u dropped (%s)", e.origin, e.data,
                    e.stage != here ? "another stage here" : "no safe moment");
            s.incoming.erase(s.incoming.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    if (s.incoming.empty() || !session::can_walk(in_game) || talk::talking()) return;
    const Event e = s.incoming.front();
    s.incoming.erase(s.incoming.begin());
    if (!post(c, e))
        NXLOG_I(kTag, "Co-op feed: node %u's talk to character %u dropped (no such character here)", e.origin, e.data);
}

}  // namespace

int on_talked_to(::Ctx* c) {
    if (!quests::member_rules()) return 0;
    const uint64_t id = c->x[1];
    if (!guest::ok(id, 2)) return 0;
    // 0x1355ec's own tests but the visitor bit: 0x286de4, Cloud Island (0x77ecb0 only compares the stage), then the data id's range.
    if (!(guest::call(c, base() + kResident, id) & 1) || stage() == layout::kStageCloudIsland) return 0;
    const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, id));
    if (static_cast<uint16_t>(data - 1) >= kDataIdMax) return 0;
    State& s = st();
    if (s.outgoing.size() < kQueue) s.outgoing.push_back(Event{kTalkedToKind, stage(), data, 0, qpc::seconds(), link::self()});
    return 0;   // the game's own code runs on and returns at its visitor test
}

void on_event_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t kind = r.u8(), at = r.u8();
    const uint16_t data = r.u16(), arg = r.u16();
    if (!r.ok() || static_cast<uint16_t>(data - 1) >= kDataIdMax) return;
    if (kind != kTalkedToKind) {
        NXLOG_I(kTag, "Co-op feed: node %u's creature event kind %u is not one this build posts; dropped", origin, kind);
        return;
    }
    if (s.incoming.size() >= kQueue) return;
    s.incoming.push_back(Event{kind, at, data, arg, qpc::seconds(), origin});
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c) return;
    if (!link::in_session()) {
        s = State{};
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

}  // namespace nxrt::games::pkp::coop::feed
