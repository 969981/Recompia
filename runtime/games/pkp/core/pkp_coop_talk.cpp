// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: seamless co-op's talk lock (pkp_coop_internal.h): one player at a time talks to a character, whoever it is.
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
//
// The game, 2.0.1:
//   0x3180e0(obj, w1 slot, x2 &actor id, w3 flags)  the one real talk start (every other caller of the talk selector only probes).
//                                     Returning 0 starts no talk, as its own busy check does (it shows its line through 0xa8fba0 then).
//                                     0x285434(&actor id) gives the character id: the same character on every process.
//   0x3181cc (midhook)                right after the talk event call 0x4bf7c0, on every return of it: w22 bit 0 clear there is the
//                                     path where no talk started (the function then returns 0); w19 is the slot
//   0x47bd00(runner)                  the end of a slot's whole event chain; byte runner+0x2000 is the slot
//   event runners ER = [main+0x35650f8], ER + slot*0x2010: busy while u32 +8 < 9 or u32 +0xc < 9
//   event queue Q = [main+0x3565100], Q + slot*0x20fa0 + 0x20e08: u32, the events still waiting for that slot's runner
//   0xa8fba0(w0 slot, x1 text, w2 1)  a line in the message log. text: +0 u32 kind, +8 u64 (the text template's word at
//                                     [main+0x3483e98] with the text id in its low 16 bits), +0x10 arguments, +0x18 their count,
//                                     +0x20 capacity 6; 6 arguments of 0x88 bytes follow (+0 u32 type, -1 unused). Kind 0x1c id 0xa8
//                                     is the text "<string>"; argument type 7 carries the string inline at +8 (up to 0x80 bytes).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::talk {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kCharacterOf = 0x285434;
constexpr uint64_t kSlotReg = 1, kActorReg = 2, kTalkedReg = 22, kNoTalkSlotReg = 19;
constexpr uint64_t kRunners = 0x35650f8, kRunnerSize = 0x2010, kRunnerSlot = 0x2000;
constexpr uint64_t kQueue = 0x3565100, kQueueSlot = 0x20fa0, kQueuePending = 0x20e08;
constexpr uint32_t kRunnerIdle = 9;
constexpr uint8_t kSlots = 2;

constexpr uint64_t kShowLine = 0xa8fba0, kTextTemplate = 0x3483e98;
constexpr uint32_t kTextPlain = 0x1c, kTextString = 0xa8, kArgString = 7, kArgNone = 0xffffffffu;
constexpr size_t kTextArgs = 0x28, kArgSize = 0x88, kArgCount = 6, kArgText = 0x80;
constexpr const char* kBusyLine = "This NPC is in a conversation with someone.";

constexpr double kLease = 5.0;        // a grant not used for a talk by then is given back
constexpr double kAskAgain = 2.0;     // a press on the same character asks again after this long without an answer
constexpr int kIdleFrames = 90;       // a talk with no event running or waiting for this many frames has ended

struct Held {
    uint32_t chara = 0;
    uint8_t slot = 0;
    bool talking = false;   // false: granted, the talk has not started yet
    double since = 0.0;
    int idle = 0;
};

struct State {
    std::unordered_map<uint32_t, uint8_t> holders;   // character -> node: the authority's table, or a member's copy of it
    std::optional<Held> held;                        // the lock this process holds
    uint32_t asked = 0;                              // a member's TalkAsk in flight
    double asked_at = 0.0;
    alignas(16) uint8_t text[kTextArgs + kArgCount * kArgSize] = {};   // the busy line's text, guest-readable
};
State& st() {
    static State s;
    return s;
}

uint32_t character_of(::Ctx* c, uint64_t actor) {
    return guest::ok(actor, 2) ? static_cast<uint32_t>(guest::call(c, base() + kCharacterOf, actor)) : 0;
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
    NXLOG_D(kTag, "Co-op talk: character %u holder node %u", chara, node);
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

void busy_line(::Ctx* c, uint8_t slot) {
    State& s = st();
    std::memset(s.text, 0, sizeof s.text);
    const uint64_t text = reinterpret_cast<uint64_t>(s.text);
    const uint64_t tmpl = guest::rd<uint64_t>(base() + kTextTemplate);
    const uint64_t word = guest::ok(tmpl, 8) ? guest::rd<uint64_t>(tmpl) : 0;
    guest::wr<uint32_t>(text, kTextPlain);
    guest::wr<uint64_t>(text + 8, (word & ~uint64_t{0xffff}) | kTextString);
    guest::wr<uint64_t>(text + 0x10, text + kTextArgs);
    guest::wr<uint64_t>(text + 0x18, 1);
    guest::wr<uint64_t>(text + 0x20, kArgCount);
    for (size_t i = 0; i < kArgCount; ++i) guest::wr<uint32_t>(text + kTextArgs + i * kArgSize, kArgNone);
    guest::wr<uint32_t>(text + kTextArgs, kArgString);
    std::memcpy(s.text + kTextArgs + 8, kBusyLine, std::min(std::strlen(kBusyLine), kArgText - 1));
    guest::call(c, base() + kShowLine, slot, text, 1);
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
    if (!link::in_session()) return 0;
    const uint8_t slot = static_cast<uint8_t>(c->x[kSlotReg]);
    const uint32_t chara = character_of(c, c->x[kActorReg]);
    if (!chara || slot >= kSlots) return 0;
    const uint8_t self = link::self();
    const auto it = s.holders.find(chara);
    if (it != s.holders.end() && it->second != self) {
        busy_line(c, slot);
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
    }
    c->x[0] = 0;
    return 1;
}

int on_no_talk(::Ctx* c) {
    State& s = st();
    if (s.held && s.held->talking && !(c->x[kTalkedReg] & 1) && s.held->slot == static_cast<uint8_t>(c->x[kNoTalkSlotReg]))
        release("no talk started");
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
            s.held = Held{chara, 0, false, qpc::seconds(), 0};
            NXLOG_I(kTag, "Co-op talk: character %u granted; the next press talks", chara);
        }
    }
    if (s.held && s.held->chara == chara && holder != link::self()) s.held.reset();
}

void on_member(::Ctx*, int node, bool joined) {
    State& s = st();
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
        return;
    }
    if (!joined && node == link::authority()) {   // the table left with it
        s.holders.clear();
        s.held.reset();
        s.asked = 0;
    }
}

void tick() {
    State& s = st();
    if (!s.held) return;
    if (!s.held->talking) {
        if (qpc::seconds() - s.held->since > kLease) release("the grant was not used");
        return;
    }
    s.held->idle = slot_idle(s.held->slot) ? s.held->idle + 1 : 0;
    if (s.held->idle > kIdleFrames) release("no event of the talk is running");
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::talk
