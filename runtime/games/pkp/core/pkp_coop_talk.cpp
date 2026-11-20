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
// the event's talk already holds this player's lock for it, and that caller does not look at the answer. One to another character
// asks for that character's lock as a press does. A member's talk takes the host's path in the talk handler (on_talk_kind),
// so a member gets the character's own talk, quest talks too, instead of the visitor's stub; the lock above keeps it to one player.
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
// The refusal's line goes to the game's message log through notice::line (pkp_coop_notice.cpp), on the talker's slot; the slot of
// this player's own talk starts is handed to the notices.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_journal.h"

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
constexpr uint64_t kQueue = 0x3565100, kQueueSlot = 0x20fa0, kQueuePending = 0x20e08;
constexpr uint32_t kRunnerIdle = 9;
constexpr uint8_t kSlots = 2;

constexpr const char* kBusyLine = "This NPC is in a conversation with someone.";

constexpr uint64_t kFlagsReg = 3;       // 0x3180e0's w3: bit 0 clear, a talk started from inside a running event (0x48fd50)
constexpr uint64_t kIsHostReg = 0;      // at 0x4c06d8: IsHost's answer, or'd into the "take the host's path" bit

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
};

struct State {
    std::unordered_map<uint32_t, uint8_t> holders;   // character -> node: the authority's table, or a member's copy of it
    std::optional<Held> held;                        // the lock this process holds
    uint32_t asked = 0;                              // a member's TalkAsk in flight
    double asked_at = 0.0;
    bool chained = false;                            // the talk start running now came from inside an event (w3 = 0)
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
        notice::line(c, slot, kBusyLine);
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
    if (s.held && s.held->chara == chara && holder != link::self()) s.held.reset();
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
