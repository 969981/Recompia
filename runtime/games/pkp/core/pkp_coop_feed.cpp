// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's creature events (pkp_coop_internal.h): what a member's player did to a creature, made again on the
// authority, so the group's world and story react to it once, on the authority. RE: modding-docs coop-v2/quests/QUEST-START-RE.md
// (sections 2, 4, 5a), coop-v2/P0-RE.md (talk start), coop-v2/role-census/verify/v0.md, coop-v2/role-census/build/talk-events.md.
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
//
// Kind 29, "fed": a field feed, the player holding food and using it on a creature. That is held-item command kind 9 (vtable
// 0x325a5f8, bound by the command constructor 0x588d20), whose execute slot 3 0x597820(cmd, action component, &start params) the
// action step 0x584ae0 calls at 0x5853cc. It acts only for a local player (player vt+0x1e0 at 0x597854), so the authority never
// runs it for a member's player. Its params: [0] the creature's data id, [count > 1 ? 1 : 0] the bag slot, [count > 2 ? 2 : 0] held
// in w24 (callee-saved) as the flag 0x333be0 gets (w2 = w24 > 0). It walks the player manager's PM+0x4550 list of that data id with
// 0x28cc60's rules, looks the slot's item up in the item table (manager +0x638, or +0 below 200 tables; record [row + 8], or the
// default record [GOT 0x3483c58]), and when the record's byte +0x3a is 7 (food) calls 0x333be0(0x276540(creature), item, flag, 0, 0):
// the creature eats. 0x333be0 drives the creature's world state: the AI state's handler (table [GOT 0x3484ee8][AIState byte +8],
// AIState R+0x7b26e8 + chara*0x1e0) vt+0x78(creature +0x2e0, item), the friend record's fed byte +0x176 and meter (0x3bc034), and
// the eating reaction 0x333fa0, whose 0x33503c reports request progress (unique 0x61c9f4 at 0x3353f8, general type 0x2d 0x62be5c
// at 0x335410, which returns on a visitor). On a member each of these lands in its own copy or own save (0x376c4 selects the host's
// records' copy R+0x14c2d28; 0x3bc034 returns before its store on a visitor; CharacterHostReplication does not carry AIState back;
// the unique record is the member's own CMNDAT), so the host's creature never ate: the player report "only the host can feed them;
// a teammate's creature eats, the story does not move on".
// Then 0x597820 takes the item out of the bag (0x2e8220) and calls 0x136548(SC, creature +0x27fe, item) (its only caller), the
// kind 29 notice, which returns on a visitor or on Cloud Island (0x136548..0x136598) and writes the data id at SC+0x11aad2, the item
// at +0x11aad0. No contents record ever waits on it (0x130cc4, the record's trigger mask, never sets bit 29: list SC+0xfc7b8 stays
// empty), so it is the feed's marker here, not the step itself.
//   0x136548 (entry hook), on a member in the session, called from 0x597820 (x30 = its return 0x597a6c): with the game's own tests
//   but the visitor bit (0x286de4, not Cloud Island, item 1..0x11f7, data id 1..0x3ff), the event (kind 29, stage, data id, the
//   item with the flag in bit 15) is queued and sent as kind 1's. The member's own feed has run already (its creature eats there).
//   On the authority, at the same safe moment: its creature of that data id (0x28cc60); the same item table test; for food the
//   game's own 0x333be0(0x276540(creature), item, flag, 0, 0), as 0x597820 makes it; then 0x136548 as 0x597820 calls it. Never
//   0x597820 itself: it would take the item out of the host's bag. The host's apply of a member's talk give (0x2197a0 at 0x21c440)
//   makes the same 0x333be0 call on the host, so a fed creature on the authority is the game's own design for a visitor's give.
// The wire carries any creature event kind (wire::Type::CreatureEvent); a kind this build does not post is dropped by the authority.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_tables.h"

#include <cstddef>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::feed {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kTalkedTo = 0x1355ec;    // (SC, &u16 actor id) -> 1 when a record was queued
constexpr uint64_t kFedNotice = 0x136548;   // (SC, &u16 actor id, u16 item) -> 1 when a record was queued
constexpr uint64_t kFedReturn = 0x597a6c;   // 0x136548's only call returns here (0x597820, held-item command kind 9's execute)
constexpr uint64_t kEat = 0x333be0;         // (creature object, item, w2 flag, w3, w4): the creature eats the item
constexpr uint64_t kCreatureObj = 0x276540; // (resident) -> its creature object (+0x3a50, or the GM+0xa0 list's entry), or 0
constexpr uint64_t kResident = 0x286de4;    // (&u16 actor id) -> a resident whose idx is outside 0x400..0x415
constexpr uint64_t kDataIdOf = 0x27d848;    // (&u16 actor id) -> u16 the character's data id
constexpr uint64_t kFirstOf = 0x28cc60;     // (player manager, data id) -> the first live resident of that data id, or 0
constexpr uint64_t kActorId = 0x27fe;       // resident actor + : u16 its actor id
constexpr uint64_t kScheduler = 0xd8;       // game state + : the contents scheduler
constexpr uint64_t kItemDefault = 0x3483c58;   // GOT: the item table's default record (a row with no record of its own)
constexpr uint64_t kItemKind = 0x3a;        // item record + : u8 its kind; 7 = food (0x597988)
constexpr uint8_t kFood = 7;
constexpr uint16_t kDataIdMax = 0x3ff;      // the notifiers' own bound: 1..0x3ff
constexpr uint16_t kItemMax = 0x11f7;       // 0x136548's bound: 1..0x11f7
constexpr uint16_t kItemMask = 0x1fff;      // 0x597820's item: the bag entry +8 & 0x1fff
constexpr uint16_t kFedFlag = 0x8000;       // kind 29's argument: 0x333be0's w2 (the item takes the low 13 bits)
constexpr uint8_t kTalkedToKind = 1;        // the scheduler's kind for "talked to"
constexpr uint8_t kFedKind = 29;            // the scheduler's kind for "fed" (0x136548 writes 0x1d)
constexpr double kSendWait = 30.0;          // a member's event is sent at the latest this long after the talk started
constexpr double kDrop = 120.0;             // an event the authority found no safe moment for (its own talks can be long)
constexpr size_t kQueue = 16;

struct Event {
    uint8_t kind;
    uint8_t stage;
    uint16_t data;   // the creature's data id
    uint16_t arg;    // kind 1: 0; kind 29: the item, kFedFlag for 0x333be0's w2
    double at;
    uint8_t origin;  // the authority: the member's node
};

const char* what(uint8_t kind) { return kind == kFedKind ? "feed of" : "talk to"; }

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

// The notifiers' own tests but the visitor bit, on a member: 0x286de4, Cloud Island (0x77ecb0 only compares the stage), then the data
// id's range. The data id, or 0 when one fails.
uint16_t member_data_id(::Ctx* c, uint64_t id) {
    if (!guest::ok(id, 2)) return 0;
    if (!(guest::call(c, base() + kResident, id) & 1) || stage() == layout::kStageCloudIsland) return 0;
    const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, id));
    return static_cast<uint16_t>(data - 1) < kDataIdMax ? data : 0;
}

void queue(uint8_t kind, uint16_t data, uint16_t arg) {
    State& s = st();
    if (s.outgoing.size() < kQueue) s.outgoing.push_back(Event{kind, stage(), data, arg, qpc::seconds(), link::self()});
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
        NXLOG_I(kTag, "Co-op feed: %s character %u (stage %u, arg 0x%x), sent to the authority", what(e.kind), e.data, e.stage, e.arg);
        ++sent;
    }
    s.outgoing.erase(s.outgoing.begin(), s.outgoing.begin() + static_cast<std::ptrdiff_t>(sent));
}

// The item's record is food (0x597820's test: record [row + 8], or the table's default record; byte +0x3a == 7).
bool is_food(::Ctx* c, uint16_t item) {
    const uint64_t row = tables::row_or_first(c, base(), layout::kItemTable, item);
    if (!row || !guest::ok(row + 8, 8)) return false;   // 0x597820 returns before the feed without a row
    uint64_t rec = guest::rd<uint64_t>(row + 8);
    if (!rec) rec = guest::rd<uint64_t>(base() + kItemDefault);
    return guest::ok(rec + kItemKind, 1) && guest::rd<uint8_t>(rec + kItemKind) == kFood;
}

// The authority: the game's own notice (and for kind 29 the feed) for its copy of the character. False when it has none.
bool post(::Ctx* c, const Event& e) {
    const uint64_t gm = game_state();
    const uint64_t sched = guest::ok(gm, kScheduler + 8) ? guest::rd<uint64_t>(gm + kScheduler) : 0;
    const uint64_t pm = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    if (!sched || !pm) return false;
    const uint64_t actor = guest::call(c, base() + kFirstOf, pm, e.data);
    if (!actor || !guest::ok(actor + kActorId, 2)) return false;
    if (e.kind == kFedKind) {
        const uint16_t item = e.arg & kItemMask;
        const bool food = is_food(c, item);
        const uint64_t obj = food ? guest::call(c, base() + kCreatureObj, actor) : 0;
        if (obj) guest::call(c, base() + kEat, obj, item, (e.arg & kFedFlag) ? 1 : 0, 0, 0);
        guest::call(c, base() + kFedNotice, sched, actor + kActorId, item);
        NXLOG_I(kTag, "Co-op feed: node %u fed character %u item %u; %s here (actor 0x%x)", e.origin, e.data, item,
                obj ? "it ate" : (food ? "no creature object here" : "not food"), guest::rd<uint16_t>(actor + kActorId));
        return true;
    }
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
            NXLOG_I(kTag, "Co-op feed: node %u's %s character %u dropped (%s)", e.origin, what(e.kind), e.data,
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
        NXLOG_I(kTag, "Co-op feed: node %u's %s character %u dropped (no such character here)", e.origin, what(e.kind), e.data);
}

}  // namespace

int on_talked_to(::Ctx* c) {
    if (!quests::member_rules()) return 0;
    const uint16_t data = member_data_id(c, c->x[1]);
    if (data) queue(kTalkedToKind, data, 0);
    return 0;   // the game's own code runs on and returns at its visitor test
}

int on_fed(::Ctx* c) {
    if (!quests::member_rules() || c->x[30] != base() + kFedReturn) return 0;
    const uint16_t item = static_cast<uint16_t>(c->x[2]);
    if (static_cast<uint16_t>(item - 1) >= kItemMax) return 0;
    const uint16_t data = member_data_id(c, c->x[1]);
    // x24: 0x597820's w24 (its params' [2], or [0] with fewer; 0x333be0 got w2 = (w24 > 0), which 0x333fa0 clears from the
    // eat record's item, 0x3341ac). Loaded at 0x597898; the lift writes it to the context at the next call (0x2e7efc at 0x5978a0)
    // and the callees after it keep it (callee-saved), so the context holds it here although the call to 0x136548 spills only x0..x2.
    if (data) queue(kFedKind, data, static_cast<uint16_t>(item | (static_cast<int32_t>(c->x[24]) > 0 ? kFedFlag : 0)));
    return 0;   // the game's own code runs on and returns at its visitor test
}

void on_event_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t kind = r.u8(), at = r.u8();
    const uint16_t data = r.u16(), arg = r.u16();
    if (!r.ok() || static_cast<uint16_t>(data - 1) >= kDataIdMax) return;
    if (kind != kTalkedToKind && kind != kFedKind) {
        NXLOG_I(kTag, "Co-op feed: node %u's creature event kind %u is not one this build posts; dropped", origin, kind);
        return;
    }
    if (kind == kFedKind && static_cast<uint16_t>((arg & kItemMask) - 1) >= kItemMax) return;
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
