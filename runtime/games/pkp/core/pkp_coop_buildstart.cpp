// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a member's start of a building kit (the town's rebuilding kits), applied on the authority as the host's
// own start (pkp_coop_internal.h; RE: modding-docs coop-v2/wave-1127/RIDGES-RE.md sections 1.1, 1.4, 1.5).
//
// The game, 2.0.1a. A kit is a class 0x327cf68 object; its build state is +0x51 (s8) and, for slot +0x59 <= 0xf, the stage data's
// facility record byte (**(main + 0x3483b60) + 0xa316c8 + slot * 0x30, saved with the stage). Its writer is SetBuildState
// (vt+0x778 = 0x865da4, (obj, state, from, leave_screen)): with +0x51 == 3 (the kit screen open, 0x85b160) and leave_screen 0 it
// writes nothing; on the host it sends every new state to the others, tagged with `from`.
//   - the host's own start, 0x85b430 (slot 267): state = 6 (the screen's start chosen) or +0x52 (the state before the screen), then
//     the presser's own screen (vt+0x900 = 0x872160), SetBuildState(obj, state, 0xff, 1), the owner release 0x538f60 and its sync.
//     No requirement check. A member's 0x85b430 writes only its own copy and sends request kind 0x1a (state at +0x367).
//   - the authority's apply, 0x221d00: the owner check (0x538f60, 0x221d84) and the bag balance (0x22292c) refuse at 0x2229a8 (answer
//     0xf2); 0x7716c0 finds the object (0x222778; only kinds whose #956 row +0x11 is 15..17 or 119); for state 0, 1 or 6 it calls
//     vt+0x4e0 (0x8603dc) -> vt+0x428 = 0x862ee0(obj, state, force 0, sender) at 0x2227ac. There, for state 6: the requirement
//     check vt+0x7d8 = 0x86bc20 (the 1100-line 0x873e20, its result into +0x55; failed: SetBuildState(obj, 0, sender, 0) at
//     0x863324, the build back to 0), +0x5e = sender, then only when (s8)state >= +0x51 SetBuildState(obj, state, sender, 0) at
//     0x86322c. So a member's start was lost when the requirement check failed on the authority, or the host had the kit screen open.
// Every member counts as player 1, so on the authority in a session a member's state-6 request takes the host's own route inside
// the game's apply: entry 0x86bc20 called from 0x863138 passes without running (as the host's start has no check), and the commit
// at 0x86322c leaves the kit screen state (leave_screen 1, the host's). `from` stays the sender, the value the game's member route already
// sends to the others (the state message's +0x42, 0x865fa4), and +0x5e, the state order test (no start over a build already past
// it: a late or repeated request), the display calls after it and vt+0x928 run as the game's. The presser's screen side (0x872160,
// event 4230) already ran on the member. Over a host whose own kit screen is open (+0x51 == 3) +0x52 becomes the start too, so the
// host's own close (which writes +0x52 back) keeps it. States 0 and 1 (a close without a start) run as the game's.
// Every outcome is journalled (j.sync "build start"): the refusals with their gate, no object found, the order test, the commit and
// the state and record byte after it.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_layout.h"

#include <cstdint>

namespace nxrt::games::pkp::coop::buildstart {

namespace {

// The sites (main offsets).
constexpr uint64_t kApply = 0x8603dc;      // entry: vt+0x4e0 of the kit class (obj, state, sender)
constexpr uint64_t kApplyReturn = 0x2227b0;   // 0x221d00's return from that call: the apply's end (midhook too)
constexpr uint64_t kCheck = 0x86bc20;      // entry: the requirement check vt+0x7d8 (obj, _, sender) -> bit 0: met
constexpr uint64_t kCheckReturn = 0x863138;   // its return in 0x862ee0
constexpr uint64_t kCommit = 0x86322c;     // midhook: 0x862ee0's `blr` to SetBuildState (x0 obj, w1 state, w2 from, w3 0)
constexpr uint64_t kFound = 0x222778;      // midhook: `cbz x0` after 0x7716c0 (x0 the object, x19 the request)
constexpr uint64_t kOwner = 0x221d84;      // midhook: `tbz w0` after the owner check 0x538f60
constexpr uint64_t kBalance = 0x22292c;    // midhook: `tbz w8` on the bag balance
constexpr uint64_t kRefused = 0x2229a8;    // midhook: the refusal (answer 0xf2), x19 the request, [sp + 8] the sender
// The request record and the kit.
constexpr uint64_t kRecCell = 0x258, kRecKind = 0x2be, kRecState = 0x367;
constexpr uint64_t kSenderSlot = 8;        // 0x221d00's sp + : u8 the requester's Link Play index
constexpr uint64_t kBuild = 0x51, kBefore = 0x52, kSlot = 0x59;
constexpr uint64_t kRecordBase = 0xa316c8, kRecordStride = 0x30;   // the stage data's facility records (layout::kCharaStatusRoot)
constexpr uint8_t kStart = 6, kScreen = 3, kMaxSlot = 0xf;

struct Pending {
    uint64_t obj = 0;     // 0: none
    uint64_t rec = 0;
    uint8_t state = 0, sender = 0;
    int8_t before = 0;
    bool checked = false, committed = false;
};
struct State {
    Pending p;
    const char* gate = nullptr;   // the refusal's gate, named by the midhook that saw it fail
};
State& st() {
    static State s;
    return s;
}

bool authority() { return link::in_session() && link::is_authority(); }

void say_request(journal::Rec& r, uint64_t rec, uint8_t sender) {
    r.in("m%u kind %u cell %08x state %u", static_cast<unsigned>(sender), static_cast<unsigned>(guest::rd<uint16_t>(rec + kRecKind)),
         guest::rd<uint32_t>(rec + kRecCell), static_cast<unsigned>(guest::rd<uint8_t>(rec + kRecState)));
}

// The facility record byte the save holds for the kit, or -1 when it has none.
int record_byte(uint64_t obj) {
    const uint8_t slot = guest::rd<uint8_t>(obj + kSlot);
    if (slot > kMaxSlot) return -1;
    const uint64_t data = guest::deref(guest::deref(base() + layout::kCharaStatusRoot));
    const uint64_t a = data ? data + kRecordBase + slot * kRecordStride : 0;
    return a && guest::ok(a, 1) ? guest::rd<int8_t>(a) : -1;
}

int on_apply(::Ctx* c) {
    State& s = st();
    s.p = Pending{};
    if (!authority() || c->x[30] != base() + kApplyReturn || !guest::ok(c->x[0], kSlot + 1)) return 0;
    const uint8_t sender = static_cast<uint8_t>(c->x[2]);
    if (sender == static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0))) return 0;
    s.p.obj = c->x[0];
    s.p.rec = c->x[19];   // 0x221d00's request, callee-saved
    s.p.state = static_cast<uint8_t>(c->x[1]);
    s.p.sender = sender;
    s.p.before = guest::rd<int8_t>(c->x[0] + kBuild);
    return 0;
}

int on_check(::Ctx* c) {
    State& s = st();
    if (!s.p.obj || s.p.state != kStart || c->x[0] != s.p.obj || c->x[30] != base() + kCheckReturn) return 0;
    s.p.checked = true;
    c->x[0] = 1;   // met: the host's own start has no requirement check
    return 1;
}

int on_commit(::Ctx* c) {
    State& s = st();
    if (!s.p.obj || c->x[0] != s.p.obj) return 0;
    s.p.committed = true;
    if (s.p.state != kStart) return 0;
    c->x[3] = 1;   // leaves the kit screen state, as the host's own start
    if (s.p.before == kScreen) guest::wr<uint8_t>(s.p.obj + kBefore, s.p.state);
    return 0;
}

int on_done() {
    State& s = st();
    const Pending p = s.p;
    s.p = Pending{};
    if (!p.obj) return 0;
    const int8_t after = guest::rd<int8_t>(p.obj + kBuild);
    journal::Rec r(journal::Area::Sync, "build start");
    say_request(r, p.rec, p.sender);
    r.out("build %d -> %d, slot %u record %d", p.before, after, static_cast<unsigned>(guest::rd<uint8_t>(p.obj + kSlot)),
          record_byte(p.obj));
    if (p.state != kStart) {
        r.note("not a start: the game's own route");
    } else if (!p.committed) {
        r.refused("the build is past the start here (%d > %u), nothing written", p.before, static_cast<unsigned>(p.state));
    } else {
        r.note("as the host's own start (requirement check %s, kit screen state left%s)", p.checked ? "passed over" : "not asked",
               p.before == kScreen ? ", the host's screen was open: its close keeps the start" : "");
        r.expect("build %u", static_cast<unsigned>(p.state)).ok(after == static_cast<int8_t>(p.state));
    }
    return 0;
}

int on_found(::Ctx* c) {
    if (c->x[0] || !authority() || guest::rd<uint8_t>(c->x[19] + kRecState) != kStart) return 0;
    journal::Rec r(journal::Area::Sync, "build start");
    say_request(r, c->x[19], guest::rd<uint8_t>(c->x[31] + kSenderSlot));
    r.refused("no building object of that kind at the cell here (0x7716c0)");
    return 0;
}

int on_refused(::Ctx* c) {
    State& s = st();
    const char* gate = s.gate;
    s.gate = nullptr;
    if (!authority()) return 0;
    const bool start = guest::rd<uint8_t>(c->x[19] + kRecState) == kStart;   // other kind 0x1a releases (chests) refused here too
    journal::Rec r(journal::Area::Sync, start ? "build start" : "release request");
    say_request(r, c->x[19], guest::rd<uint8_t>(c->x[31] + kSenderSlot));
    r.refused("%s", gate ? gate : "the request's kind or container test (0x221d00)");
    return 0;
}

}  // namespace

int on_site(::Ctx* c, uint64_t site) {
    switch (site) {
        case kApply: return on_apply(c);
        case kCheck: return on_check(c);
        case kCommit: return on_commit(c);
        case kApplyReturn: return on_done();
        case kFound: return on_found(c);
        case kOwner:
            if (!(c->x[0] & 1)) st().gate = "the sender does not own the object here (0x538f60)";
            return 0;
        case kBalance:
            if (!(c->x[8] & 1)) st().gate = "the bag balance with the host's mirror";
            return 0;
        case kRefused: return on_refused(c);
        default: return 0;
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::buildstart
