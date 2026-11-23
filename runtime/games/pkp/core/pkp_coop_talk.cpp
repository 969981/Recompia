// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's talk lock (pkp_coop_internal.h): one player at a time talks to a character, whoever it is.
//
// The authority keeps the table (character id -> the node that holds its talk) and is the only one that grants; every member keeps a
// copy from the authority's TalkHolder messages, so a talk to a character someone else holds is refused at once, with our line, on
// every process. A talk start cannot wait for the network on the game thread, so:
//   - the authority's own player takes a free character's lock by direct call, and the talk runs on the same press;
//   - a member's first press on a free character asks the authority (TalkAsk) and starts no talk; the grant comes back as a TalkHolder
//     naming the member, and the next press talks. A grant not used within kLease is given back;
//   - two members asking for one character: the authority's order decides, the other one's copy then names the holder.
// The lock is given back (TalkDone, or by the authority itself) when the talk's event chain ends, when the press started no talk after
// all, when nothing of the talk is running any more (watchdog, below) and, on the authority, when its holder leaves the session.
// A talk an event starts from inside its own chain (w3 = 0) to the character this player holds is never refused and takes no lock:
// the event's talk already holds this player's lock for it, and that caller does not look at the answer. A chained talk to a
// character of this player's own running event (its runner's actor list, slot w1) that no other player holds is this player's at
// once: the talk runs on that call (0x48fd50 never presses again, so asking would lose it: a member's confirm talk with a creature
// that just appeared at a habitat, event 3708 -> callback 11), and a member tells the authority with the same TalkAsk; an answer
// naming another holder drops the claim (the talk already runs). Any other chained talk asks as a press does. A member's talk takes
// the host's path in the talk handler (on_talk_kind),
// so a member gets the character's own talk, quest talks too, instead of the visitor's stub; the lock above keeps it to one player.
// A member is also the own player of every creature that lives in the group's town (on_talk_own): its talks with a resident go on to
// the same later stages as the host's, outside Cloud Island (whose records and visitor rules are the game's own).
//
// The game, 2.0.1:
//   0x3180e0(obj, w1 slot, x2 &actor id, w3 flags)  the one real talk start (every other caller of the talk selector only probes).
//                                     Returning 0 starts no talk, as its own busy check does (it shows its line through 0xa8fba0 then).
//                                     The key is the character itself, not its species (0x285434 gives the species, so two of one
//                                     species would share it): 0x281360(actors = [[main+0x3483bd0]], &actor id) finds the actor;
//                                     a resident (vt+0x1d8 true, vt+0x410 its resident part: the actor itself, 0x666d48 `ret`) has
//                                     u16 +0x39f0 the data id (a row of #469, 1..0x3ff) and u16 +0x39f2 its idx, both set by its init
//                                     0x295600 from the one save; key (data id << 16) | idx. Other talk targets (vt+0x1f0 true,
//                                     vt+0x420) have only u16 +0x2ba4: key bit 31 | (that id << 16). Read in 0x285434's order.
//                                     The pair is how the game itself knows one character on another process: the replication
//                                     receive 0x289700 applies a record of actor id 0x2xxx only when both equal the receiver's.
//   0x3181cc (midhook)                right after the talk event call 0x4bf7c0, on every return of it: w22 bit 0 clear there is the
//                                     path where no talk started (the function then returns 0); w19 is the slot
//   0x47bd00(runner)                  the end of a slot's whole event chain; byte runner+0x2000 is the slot
//   event runners ER = [main+0x35650f8], ER + slot*0x2010: busy while u32 +8 < 9 or u32 +0xc < 9
//   event queue Q = [main+0x3565100], Q + slot*0x20fa0 + 0x20e08: u32, the events still waiting for that slot's runner
//   0x3180e0's callers: the A press 0x31347c (w3 = 1) and event callback 11, 0x48fd50 (w3 = 0: a talk with the runner's actor
//                                     u16 +0x1514, started from inside a running event; it ignores the return)
//   0x4c06d8 (midhook)                the talk handler 0x4c024c, online and outside Cloud Island: `orr w8, w25, w0` with w0 = IsHost
//                                     0xa40bc; w8 bit 0 clear takes the visitor path 0x4c07e4 (the stub talk kinds 10 / 11), set the
//                                     host's 0x4c06e0 (kind 3, or the character's talk table). w0 is not read after it
//   0x4c0574 (midhook)                the same handler's `tbz w0, #0, 0x4c059c` after 0x332c80(comp x22, &own user id) at 0x4c0570:
//                                     "this player is the creature's own". 0x332c80 reads the creature's 0x180-byte friend record
//                                     (Cloud Island's set, else the own records or, on a visitor, its copy of the host's: 0x376c4) and
//                                     is true when the user id is its parents (+0x8, m_ParentsUserID) or one of the 10 ids of the ring
//                                     at +0x31 (m_AcquaintanceParentsUserID). True returns 0: this stage gives no talk and the selector
//                                     0x4c3ec8 goes on to its later stages (the requests, the crafting talk, the talk tables). False on
//                                     a creature with parents (and not 0x52 / 0x72 / 0xaf) skips the IsHost split above: online,
//                                     outside Cloud Island, it writes talk id 4 (0x4c06f4) and returns 1, so no later stage runs.
//                                     Off Cloud Island only the host writes parents, so a resident's parents is the host's id, and the
//                                     ring is not written by a talk there (its writer 0x3b8128 runs from 0x2a25c0, whose callers are
//                                     0x4eab20 for a creature a player made appear, 0x2a0b40 whose one caller seen, the host's 0x2aec60,
//                                     passes w4 = 1, which skips it, and RequestAcquaintance for the characters of idx 0x400..0x40e):
//                                     a member whose copy holds a resident's parents gets talk id 4 for it, and for 0x52 / 0x72 /
//                                     0xaf the host path 0x4c06e0, where the host goes on to the later stages for all of them.
//                                     w0 is not read after the tbz
// Inside a talk, the selector's later stages read talk conditions (the table main+0x34c2958 + id*8, by the condition-set readers;
// condition 0x52 also straight from script callback slot 208 0x4afa24 at 0x4afbe8, whose true starts talk script 0x1b3). Two answer
// a member differently from the host for the same creature:
//   0x4d3368 (condition 0x52)         the talk 0x1b3 (#971 row 13, #1093 row 3, callback 208): the player's held item (0x2ec2ec on
//                                     the bag, per player) is item 0x5c, the species is not 0x22 / 0x6f, and the creature lacks
//                                     specialty 0x20 (0x28586c false returns 1 at 0x4d34dc) or this player is not its parent:
//                                     w19 = (w22, its row's +0x1a in 0x11f8..0x23ee with a +0x65 bit) | 0x3b7948 (own records R+0x120,
//                                     no visitor select: parents == own user id) | 0x284ef4 (the record as selected: parents == own
//                                     id); then, online, off Cloud Island, GM+0x11 bit 0 clear, 0x4d35f4 `bl IsHost` and 0x4d35f8
//                                     `eor w8, w0, #1` (midhook): w8 | w19 clear returns 1. So a member got 0 for every creature with
//                                     that specialty, where the host gets 1 for one with no parents. Its own-save term reads the
//                                     member's own world and its selected term compares the host's parents with the member's id, so
//                                     the host's answer is not a role flip: here x0 = 1 (IsHost) and x19 = w22 | the creature's
//                                     record in the member's copy of the host's (R+0x14c2d28 + (data id w21 + 4) * 0x180, the
//                                     chara index 0x3b7948 uses) has parents, compared with the null id main+0x3566a38 as 0x285100
//                                     does. Off Cloud Island only the host writes parents (later-talks.md), and the game's
//                                     replication (kind 0xb, +0x8..+0x1a) keeps them current in the copy, so this is "the host is its
//                                     parent". w21 (the data id, 0x4d3538) and w22 (0x4d3594) are set on the one path to 0x4d35f8;
//                                     x19 is read only by the `orr` at 0x4d3604
//   0x4d0620 (condition 0x25)         `b IsVisitor`, a tail call (no role row): its one user, #960 row 35 (conditions 0x25, 0x24 no
//                                     parents, 0x22 online, 0xa4 species 0x46 / 0x95), offers the visitor's own first talk with those
//                                     two at the last stage 0x4c8ba0. The host never gets it; a member in the session off Cloud
//                                     Island now does not either (entry hook: returns 0). Condition 0x16 (0x4cfb08 `b IsHost`) has no
//                                     user: no table row, no fixed-offset call ([table + 0xb8] = 0x17 and + 0x290 = 0x52 are the
//                                     only ones), and the loads at 0x4c9bd0 / 0x4c9cd4 in 0x4c8ba0 index hash buckets on its stack
// Talk 0x1b3's answer, script callback slot 119 0x4a17fc (a creature takes the held Star Piece, item 0x5c, and gives its item), tests
// condition 0x52's terms again before it queues line 437 ("give it to someone else"), 438 ("not a good time") or the exchange (line
// 436, the give 0x45f580 -> slot 27, the receive 0x3bcdc0 -> slot 28). Lines 436..438 are talk lines, not scripted events:
// 0x339580 queues a record of id 0 with kind +0x664 = 3 and the line at +0x1b4, which the runner (0x4763e8) plays as one script
// command 3 (0x431200, the message), so they write no saved state. Off Cloud Island its parent term is again the own-save one
// (record R + data id * 0x180 + 0x728 = own record +0x8, m_ParentsUserID, against m_UserID R+0x13d3a89), or'd with the row term at:
//   0x4a1adc (midhook)                `orr w24, w22, w8`: w22 the row term (+0x65 bit, item +0x1a in 1..0x11f7), w8 the parent term,
//                                     w24 (off Cloud Island) the data id (1..0x3ff, 0x4a19ac). A member's w8 becomes "the group's
//                                     record of the creature (the member's copy of the host's) has parents", as at 0x4d35f8, so a
//                                     member is refused where the host is, for the town's residents, and accepted for a creature
//                                     with none. w8 is read by this `orr` alone (0x4a1ae8 calls out, 0x4a1af0 loads x8). The
//                                     0x284ef4 term that follows compares the copy's parents with the member's own id (false off
//                                     Cloud Island, where only the host writes parents). The IsHost answer at 0x4a1b70 that picks
//                                     line 438 is a role row (pkp_coop_role_table.inc)
// Condition 0x22 (0x4d04fc, `b IsOnline 0x207b0`, reached only through the table) is the stand-in line's: #1093 row 7 (conditions
// +6 0x4f own creature, +7 0x24 no parents, +8 0x22 online) gives event 3682, "they want to meet <world owner>", to anyone online
// whose character has no story talk set; offline the row is out and the character gives its ordinary talk. The selector stage
// 0x4c7260 evaluates a #1093 row's three condition bytes +6 / +7 / +8 through the table ([GOT 0x34853b8] = 0x34c2958), and row 7
// is the only #1093 row with 0x22. Its other users are #960 rows (0x4c8ba0, the last stage, and 0x4ba5c0): row 35 (0x25 IsVisitor,
// 0x24, 0x22, species 0x46 / 0x95) and row 34 (talk script 182: 0x4f own creature, 0x50 its talk component +0x29c0 byte +0x10 bit 0
// clear, 0x27 not its parent (0x284ef4 false), 0x22), whose script is not read. Condition 0x24 (0x4d0594) is "no parents":
// 0x285100(comp) false, comp = the resident part (actor vt+0x410) + 0x2ca0 of 0x281360([main+0x3561928], x1 &actor id), else the
// stand-in 0x3567db0. So offline is answered only for a character with no parents: rows 7 and 35 are out as offline, and row 34
// for such a character changes nothing online played, since row 7 (0x4f, 0x24, 0x22) passes wherever row 34 does and 0x4c7260
// claims the talk before 0x4c8ba0 runs. A character with parents (the town's residents) keeps every row as the game has it.
//   0x4d04fc (entry hook)             in the session, off Cloud Island, for a character with no parents (0x24's own test on the
//                                     condition's x1): answers 0 (as offline), so it gives its ordinary talk instead of the stand-in
// The refusal's line goes to the game's message log through notice::line (pkp_coop_notice.cpp), on the talker's slot; the slot of
// this player's own talk starts is handed to the notices.
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::talk {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kActors = 0x3483bd0;   // main + : -> the actor manager
constexpr uint64_t kActorOf = 0x281360;   // (actors, &u16 actor id): the actor, or 0
constexpr uint64_t kIsResident = 0x1d8, kResident = 0x410, kIsOther = 0x1f0, kOther = 0x420;   // actor vtable +
constexpr uint64_t kDataId = 0x39f0, kIdx = 0x39f2, kOtherId = 0x2ba4;                        // resident part / other part +
constexpr uint32_t kDataIds = 0x3ff;      // data ids 1..0x3ff, as 0x285434 accepts them
constexpr uint32_t kOtherKey = 0x80000000u;
constexpr uint64_t kSlotReg = 1, kActorReg = 2, kTalkedReg = 22, kNoTalkSlotReg = 19;
constexpr uint64_t kRunners = 0x35650f8, kRunnerSize = 0x2010, kRunnerSlot = 0x2000;
constexpr uint64_t kRunActors = 0x1a88, kRunActorCount = 0x1a90;   // runner +: the event's u16 actor ids (pointer, count)
constexpr uint64_t kMaxRunActors = 64;
constexpr uint64_t kQueue = 0x3565100, kQueueSlot = 0x20fa0, kQueuePending = 0x20e08;
constexpr uint32_t kRunnerIdle = 9;
constexpr uint8_t kSlots = 2;

constexpr uint64_t kFlagsReg = 3;       // 0x3180e0's w3: bit 0 clear, a talk started from inside a running event (0x48fd50)
constexpr uint64_t kIsHostReg = 0;      // at 0x4c06d8: IsHost's answer, or'd into the "take the host's path" bit (also at 0x4d35f8)
constexpr uint64_t kOwnReg = 0;         // at 0x4c0574: 0x332c80's answer, "this player is the creature's own"
constexpr uint64_t kCompReg = 22;       // at 0x4c0574: the creature's component (actor vt+0x410 + 0x2ca0), or the stand-in
constexpr uint64_t kNoComponent = 0x3567db0;   // main + : the stand-in component of a talk target with none
constexpr uint64_t kHasParents = 0x285100;     // (comp) -> bit 0: its record (as this process selects it) has a parents user id
constexpr uint64_t kCloudIsland = 0x77ecb0;    // () -> bit 0: Cloud Island
constexpr uint64_t kCondRowReg = 22, kCondDataReg = 21, kCondParentReg = 19;   // at 0x4d35f8 (condition 0x52), see the header
constexpr uint32_t kCharaOfData = 4;            // a creature's friend record index is its data id + 4 (0x3b7948's +0x600)
constexpr uint64_t kParents = 0x8;              // friend record: 16 bytes, m_ParentsUserID
constexpr uint64_t kNullUser = 0x3566a38;       // main + : the 16-byte null user id 0x285100 compares parents with
constexpr uint64_t kSwapDataReg = 24, kSwapParentReg = 8;   // at 0x4a1adc (talk 0x1b3's answer, slot 119), see the header
constexpr uint64_t kCondActors = 0x3561928;    // main + : -> the actor manager the talk conditions pass to 0x281360
constexpr uint64_t kCondComp = 0x2ca0;         // resident part + : the component conditions 0x24 / 0x27 / 0x4f read
constexpr double kOnlineLogEvery = 60.0;   // the selector probes every frame: one line, then one a minute with the count

constexpr double kLease = 5.0;        // a grant not used for a talk by then is given back
constexpr double kAskAgain = 2.0;     // a press on the same character asks again after this long without an answer
constexpr int kIdleFrames = 90;       // a talk with no event running or waiting for this many frames has ended

struct Held {
    uint32_t chara = 0;
    uint8_t slot = 0;
    bool talking = false;   // false: granted, the talk has not started yet
    double since = 0.0;
    int idle = 0;
    bool forced = false;   // the host-path line of this talk is written
    bool owned = false;    // the "own creature" line of this talk is written
};

struct State {
    std::unordered_map<uint32_t, uint8_t> holders;   // character -> node: the authority's table, or a member's copy of it
    std::optional<Held> held;                        // the lock this process holds
    uint32_t asked = 0;                              // a member's TalkAsk in flight
    double asked_at = 0.0;
    bool chained = false;                            // the talk start running now came from inside an event (w3 = 0)
    double online_logged = -kOnlineLogEvery;         // condition 0x22's offline answer: its last journal line, and the answers since
    uint32_t online_since = 0;
};
State& st() {
    static State s;
    return s;
}

// The lock key of the talk target whose u16 actor id is at id (guest): one character, the same on every process; 0 for none.
uint32_t character_of(::Ctx* c, uint64_t id) {
    if (!guest::ok(id, 2)) return 0;
    const uint64_t got = guest::rd<uint64_t>(base() + kActors);
    const uint64_t actors = got && guest::ok(got, 8) ? guest::rd<uint64_t>(got) : 0;
    if (!actors) return 0;
    const uint64_t actor = guest::call(c, base() + kActorOf, actors, id);
    if (!actor || !guest::ok(actor, 8)) return 0;
    const uint64_t vt = guest::rd<uint64_t>(actor);
    if (!guest::ok(vt + kOther, 8)) return 0;
    if (guest::call(c, guest::rd<uint64_t>(vt + kIsResident), actor) & 1) {
        const uint64_t part = guest::call(c, guest::rd<uint64_t>(vt + kResident), actor);
        if (part && guest::ok(part + kDataId, 4)) {
            const uint32_t data = guest::rd<uint16_t>(part + kDataId);
            return data - 1 < kDataIds ? (data << 16) | guest::rd<uint16_t>(part + kIdx) : 0;
        }
    }
    if (!(guest::call(c, guest::rd<uint64_t>(vt + kIsOther), actor) & 1)) return 0;
    const uint64_t part = guest::call(c, guest::rd<uint64_t>(vt + kOther), actor);
    if (!part || !guest::ok(part + kOtherId, 2)) return 0;
    const uint32_t data = guest::rd<uint16_t>(part + kOtherId);
    return data - 1 < kDataIds ? kOtherKey | (data << 16) : 0;
}

void tell_members(uint32_t chara, uint8_t holder) {
    link::to_members(wire::Type::TalkHolder, wire::Writer(link::self()).u32(chara).u8(holder).take());
}

// The authority: the table changes and every member hears it.
void set_holder(uint32_t chara, uint8_t node) {
    State& s = st();
    if (node == wire::kNobody) s.holders.erase(chara);
    else s.holders[chara] = node;
    tell_members(chara, node);
    NXLOG_I(kTag, "Co-op talk: character %u %s", chara, node == wire::kNobody ? "is free" : "is taken");
    NXLOG_I(kTag, "Co-op talk: character %u holder node %u", chara, node);
}

void release(const char* why) {
    State& s = st();
    if (!s.held) return;
    const uint32_t chara = s.held->chara;
    s.held.reset();
    NXLOG_I(kTag, "Co-op talk: character %u given back (%s)", chara, why);
    if (link::is_authority()) set_holder(chara, wire::kNobody);
    else link::to_authority(wire::Type::TalkDone, wire::Writer(link::self()).u32(chara).take());
}

// Nothing of the slot's talk runs or waits: its runner is idle and its queue empty.
bool slot_idle(uint8_t slot) {
    const uint64_t runners = guest::rd<uint64_t>(base() + kRunners);
    const uint64_t queue = guest::rd<uint64_t>(base() + kQueue);
    const uint64_t runner = runners + slot * kRunnerSize;
    const uint64_t pending = queue + slot * kQueueSlot + kQueuePending;
    if (!guest::ok(runner, kRunnerSize) || !guest::ok(pending, 4)) return false;
    return guest::rd<uint32_t>(runner + 8) >= kRunnerIdle && guest::rd<uint32_t>(runner + 0xc) >= kRunnerIdle &&
           guest::rd<uint32_t>(pending) == 0;
}

// The u16 actor id at id is in the actor list of the slot's runner: the event running there is about that character.
bool in_running_event(uint8_t slot, uint64_t id) {
    const uint64_t run = guest::rd<uint64_t>(base() + kRunners) + slot * kRunnerSize;
    if (!guest::ok(run, kRunnerSize) || !guest::ok(id, 2)) return false;
    const uint64_t list = guest::rd<uint64_t>(run + kRunActors), count = guest::rd<uint64_t>(run + kRunActorCount);
    if (!count || count > kMaxRunActors || !guest::ok(list, count * 2)) return false;
    const uint16_t actor = guest::rd<uint16_t>(id);
    for (uint64_t i = 0; i < count; ++i)
        if (guest::rd<uint16_t>(list + i * 2) == actor) return true;
    return false;
}

}  // namespace

int on_talk_start(::Ctx* c) {
    State& s = st();
    s.chained = false;
    if (!link::in_session()) return 0;
    const uint8_t slot = static_cast<uint8_t>(c->x[kSlotReg]);
    const uint32_t chara = character_of(c, c->x[kActorReg]);
    // Chained from inside an event, to the character whose lock this player holds: it runs under that event's talk and its lock. A
    // chained talk to another character (the runner's actor need not be the event's) takes the lock path below like a press.
    if (!(c->x[kFlagsReg] & 1) && chara && s.held && s.held->chara == chara) {
        s.chained = true;
        return 0;
    }
    if (!chara || slot >= kSlots) return 0;
    notice::set_slot(slot);
    const uint8_t self = link::self();
    const auto it = s.holders.find(chara);
    if (it != s.holders.end() && it->second != self) {
        notice::line(c, slot, strings::get(base(), strings::Str::CoopTalkBusy));
        journal::Rec(journal::Area::Talk, "lock").in("character=0x%08x (data id %u, idx %u) slot=%u", chara, chara >> 16 & 0x7fff,
                                                     chara & 0xffff, slot)
            .refused("held by n%u", it->second);
        c->x[0] = 0;
        return 1;
    }
    if (s.held && s.held->chara == chara) {   // granted (or still ours): the talk runs
        s.held->talking = true;
        s.held->slot = slot;
        s.held->idle = 0;
        return 0;
    }
    if (s.held) release("another character was talked to");
    const double now = qpc::seconds();
    if (link::is_authority()) {
        set_holder(chara, self);
        s.held = Held{chara, slot, true, now, 0};
        return 0;
    }
    if (!(c->x[kFlagsReg] & 1) && in_running_event(slot, c->x[kActorReg])) {   // chained from this player's own event with it
        s.held = Held{chara, slot, true, now, 0};
        if (s.asked == chara) s.asked = 0;
        link::to_authority(wire::Type::TalkAsk, wire::Writer(self).u32(chara).take());
        NXLOG_I(kTag, "Co-op talk: character %u claimed for a talk chained from this player's event", chara);
        journal::Rec(journal::Area::Talk, "lock").in("character=0x%08x (data id %u, idx %u) slot=%u", chara, chara >> 16 & 0x7fff,
                                                     chara & 0xffff, slot)
            .out("chained from this player's own event: talk runs, the authority is told");
        return 0;
    }
    if (s.asked != chara || now - s.asked_at > kAskAgain) {
        s.asked = chara;
        s.asked_at = now;
        link::to_authority(wire::Type::TalkAsk, wire::Writer(self).u32(chara).take());
        NXLOG_I(kTag, "Co-op talk: asking for character %u", chara);
        journal::Rec(journal::Area::Talk, "lock").in("character=0x%08x (data id %u, idx %u) slot=%u", chara, chara >> 16 & 0x7fff,
                                                     chara & 0xffff, slot)
            .out("asked the authority; no talk on this press");
    }
    c->x[0] = 0;
    return 1;
}

int on_no_talk(::Ctx* c) {
    State& s = st();
    if (s.chained) return 0;   // a chained talk that did not start: the event's own talk still runs, and its lock stays
    if (s.held && s.held->talking && !(c->x[kTalkedReg] & 1) && s.held->slot == static_cast<uint8_t>(c->x[kNoTalkSlotReg]))
        release("no talk started");
    return 0;
}

int on_talk_kind(::Ctx* c) {
    if (!link::in_session() || link::is_authority()) return 0;
    State& s = st();
    if (s.held && s.held->talking && !s.held->forced) {   // once per talk: the selector's probes may come through here too
        s.held->forced = true;
        journal::Rec(journal::Area::Talk, "kind forced").in("character=0x%08x IsHost w0=%u", s.held->chara,
                                                            static_cast<uint32_t>(c->x[kIsHostReg] & 1))
            .out("host path");
    }
    c->x[kIsHostReg] = 1;
    return 0;
}

int on_talk_own(::Ctx* c) {
    // Only where the game said no, for a member, on a creature with parents outside Cloud Island: a creature with none keeps false
    // (its first talk is the IsHost split above, on_talk_kind and pkp_coop_newcomer.cpp), and Cloud Island keeps its own rules.
    if ((c->x[kOwnReg] & 1) || !quests::member_rules()) return 0;
    const uint64_t comp = c->x[kCompReg];
    if (comp == base() + kNoComponent || (guest::call(c, base() + kCloudIsland, 0) & 1)) return 0;
    if (!(guest::call(c, base() + kHasParents, comp) & 1)) return 0;
    State& s = st();
    if (s.held && s.held->talking && !s.held->owned) {   // once per talk, as on_talk_kind
        s.held->owned = true;
        journal::Rec(journal::Area::Talk, "own creature").in("character=0x%08x", s.held->chara).out("a resident of the town: later stages");
    }
    c->x[kOwnReg] = 1;
    return 0;
}

int on_cond_parent(::Ctx* c) {
    // Reached only online, off Cloud Island, with GM+0x11 bit 0 clear (0x4d35cc..0x4d35f0), and data id - 1 < 0x3ff (0x4d353c).
    if (!quests::member_rules()) return 0;
    const uint32_t chara = static_cast<uint32_t>(c->x[kCondDataReg] & 0xffff) + kCharaOfData;
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t rec = root + layout::kFriendHostCopy + static_cast<uint64_t>(chara) * layout::kFriendSize;
    if (!root || chara >= layout::kFriendCount || !guest::ok(rec + kParents, 16) || !guest::ok(base() + kNullUser, 16)) return 0;
    const bool parents = guest::rd<uint64_t>(rec + kParents) != guest::rd<uint64_t>(base() + kNullUser) ||
                         guest::rd<uint64_t>(rec + kParents + 8) != guest::rd<uint64_t>(base() + kNullUser + 8);
    c->x[kCondParentReg] = (c->x[kCondRowReg] & 1) | (parents ? 1 : 0);
    c->x[kIsHostReg] = 1;
    return 0;
}

int on_swap_parent(::Ctx* c) {
    // Off Cloud Island only: there w24 is the data id; on Cloud Island (w24 = data id + 4, w8 the Cloud record's +0x178) the game's
    // own rules stay. Any read that fails leaves the game's answer.
    if (!quests::member_rules() || (guest::call(c, base() + kCloudIsland, 0) & 1)) return 0;
    const uint32_t data = static_cast<uint32_t>(c->x[kSwapDataReg] & 0xffff);
    if (data - 1 >= kDataIds) return 0;
    const uint32_t chara = data + kCharaOfData;
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t rec = root + layout::kFriendHostCopy + static_cast<uint64_t>(chara) * layout::kFriendSize;
    if (!root || chara >= layout::kFriendCount || !guest::ok(rec + kParents, 16) || !guest::ok(base() + kNullUser, 16)) return 0;
    const bool parents = guest::rd<uint64_t>(rec + kParents) != guest::rd<uint64_t>(base() + kNullUser) ||
                         guest::rd<uint64_t>(rec + kParents + 8) != guest::rd<uint64_t>(base() + kNullUser + 8);
    if (parents != ((c->x[kSwapParentReg] & 1) != 0))
        journal::Rec(journal::Area::Talk, "star piece swap")
            .in("data=%u", data)
            .out(parents ? "the group's creature: declined, as for the host" : "no parents in the group: taken, as for the host");
    c->x[kSwapParentReg] = parents ? 1 : 0;
    return 0;
}

int on_cond_visitor(::Ctx* c) {
    if (!quests::member_rules() || (guest::call(c, base() + kCloudIsland, 0) & 1)) return 0;
    c->x[0] = 0;
    return 1;
}

int on_cond_online(::Ctx* c) {
    if (!link::in_session() || (guest::call(c, base() + kCloudIsland, 0) & 1)) return 0;
    // Condition 0x24's own reading of the character (0x4d0594): a character with parents keeps the game's answer.
    const uint64_t id = c->x[1];
    const uint64_t actors = guest::rd<uint64_t>(base() + kCondActors);
    const uint64_t actor = actors && guest::ok(id, 2) ? guest::call(c, base() + kActorOf, actors, id) : 0;
    uint64_t comp = base() + kNoComponent;
    if (actor && guest::ok(actor, 8) && guest::ok(guest::rd<uint64_t>(actor) + kResident, 8)) {
        const uint64_t vt = guest::rd<uint64_t>(actor);
        if (guest::call(c, guest::rd<uint64_t>(vt + kIsResident), actor) & 1) {
            const uint64_t part = guest::call(c, guest::rd<uint64_t>(vt + kResident), actor);
            if (part) comp = part + kCondComp;
        }
    }
    if (guest::call(c, base() + kHasParents, comp) & 1) return 0;
    State& s = st();
    ++s.online_since;
    const double now = qpc::seconds();
    if (now - s.online_logged >= kOnlineLogEvery) {
        journal::Rec(journal::Area::Talk, "condition")
            .in("cond=0x22 (online) actor=0x%04x, no parents", guest::ok(id, 2) ? guest::rd<uint16_t>(id) : 0u)
            .out("0")
            .expect("0 in a co-op session, as offline: no stand-in line 3682")
            .note("x%u since the last line", s.online_since);
        s.online_logged = now;
        s.online_since = 0;
    }
    c->x[0] = 0;
    return 1;   // the condition is `b IsOnline`: x0 is all it returns
}

int on_talk_end(::Ctx* c) {
    State& s = st();
    const uint64_t runner = c->x[0];
    if (s.held && s.held->talking && guest::ok(runner + kRunnerSlot, 1) && guest::rd<uint8_t>(runner + kRunnerSlot) == s.held->slot)
        release("the talk ended");
    return 0;
}

bool talking() {
    const State& s = st();
    return s.held && s.held->talking;
}

uint32_t character_key(::Ctx* c, uint64_t id) { return character_of(c, id); }

bool held_elsewhere(uint32_t key) {
    const State& s = st();
    const auto it = key ? s.holders.find(key) : s.holders.end();
    return it != s.holders.end() && it->second != link::self();
}

void on_ask(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint32_t chara = r.u32();
    if (!r.ok() || !chara || !link::is_authority()) return;
    const auto it = s.holders.find(chara);
    if (it == s.holders.end()) set_holder(chara, origin);
    else tell_members(chara, it->second);   // taken: the asker's copy learns who holds it
}

void on_done(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint32_t chara = r.u32();
    if (!r.ok() || !link::is_authority()) return;
    const auto it = s.holders.find(chara);
    if (it != s.holders.end() && it->second == origin) set_holder(chara, wire::kNobody);
}

void on_holder(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    const uint32_t chara = r.u32();
    const uint8_t holder = r.u8();
    if (!r.ok() || link::is_authority()) return;
    if (holder == wire::kNobody) s.holders.erase(chara);
    else s.holders[chara] = holder;
    if (s.asked == chara) {
        s.asked = 0;
        if (holder == link::self()) {
            if (s.held) release("another character was granted");
            s.held = Held{chara, 0, false, qpc::seconds(), 0};
            NXLOG_I(kTag, "Co-op talk: character %u granted; the next press talks", chara);
            journal::Rec(journal::Area::Talk, "lock").in("character=0x%08x", chara).out("granted; the next press talks");
        }
    } else if (holder == link::self() && !(s.held && s.held->chara == chara)) {   // a grant asked for before another press: not wanted
        link::to_authority(wire::Type::TalkDone, wire::Writer(link::self()).u32(chara).take());
    }
    if (s.held && s.held->chara == chara && holder != link::self()) {   // the authority's order gave it to another player
        if (s.held->talking && holder != wire::kNobody)
            journal::Rec(journal::Area::Talk, "lock").in("character=0x%08x", chara).out("held by n%u after all; the talk runs on", holder);
        s.held.reset();
    }
}

void on_member(::Ctx*, int node, bool joined, bool authority, bool self) {
    State& s = st();
    if (self || (!joined && authority)) {   // this process joined or left a session, or the table left with its
        s.holders.clear();                                   // authority: nothing of the old one holds
        s.held.reset();
        s.asked = 0;
        return;
    }
    if (link::is_authority()) {
        if (joined) {   // the newcomer's copy starts with the whole table
            for (const auto& [chara, holder] : s.holders)
                link::to_node(node, wire::Type::TalkHolder, wire::Writer(link::self()).u32(chara).u8(holder).take());
            return;
        }
        for (auto it = s.holders.begin(); it != s.holders.end();) {
            if (it->second != node) {
                ++it;
                continue;
            }
            const uint32_t chara = it->first;
            ++it;
            set_holder(chara, wire::kNobody);
            NXLOG_I(kTag, "Co-op talk: character %u freed, its holder left", chara);
        }
    }
}

void tick() {
    State& s = st();
    if (!s.held) return;
    if (!s.held->talking) {
        if (qpc::seconds() - s.held->since > kLease) {
            journal::Rec(journal::Area::Talk, "lock").in("character=0x%08x", s.held->chara).out("lease expired after %.0f s", kLease);
            release("the grant was not used");
        }
        return;
    }
    s.held->idle = slot_idle(s.held->slot) ? s.held->idle + 1 : 0;
    if (s.held->idle > kIdleFrames) release("no event of the talk is running");
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::talk
