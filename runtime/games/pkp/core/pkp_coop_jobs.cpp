// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a creature's item-making job on the authority (pkp_coop_internal.h; RE: modding-docs
// coop-v2/VISITOR-SERVICES-RE.md 7c).
//
// A creature that makes items keeps its job (recipe, count, timer) in a component the host ticks and replicates; a visitor never
// ticks it. The host ticks it only while the creature is "mine" there (IsMine): free (owner 0xff) or held by the host itself. A
// member's talk takes the creature's net owner (RequestCharacterOwnership; it asks for every event actor that is free, also when the
// creature is only told "I need a little more time"), and the host gives it back when it applies that talk's end record (0x2cb7ec in
// 0x2197a0). While a member's index stays in the owner byte (the talk still running, or that talk's end record late or lost, which
// leaves it there for the session), no process ticks a job the creature already has: the host does not own it and the member does
// not tick. Here the authority's IsMine test in the tick answers yes whoever holds the creature, the answer it already gets while
// the host holds it itself; the tick's other gates stay. Members still tick nothing, so the job is ticked once. The owner byte and
// its handshake are untouched.
//
// The game, 2.0.1a:
//   0x3b38f0(comp, s0 dt)             the item-making tick: returns at once on a visitor (0x376c4); finds the actor by comp u16 +8
//                                     (0x281360), returns when its resident part's vt+0x318 is set, then asks the actor's IsMine
//                                     (vt+0x478, 0x27f958: offline, GM+0x11 bit 0, owner 0xff and the host (0xa40bc), or owner ==
//                                     own index NM+0xb3381) and returns on false (`tbz w0, #0` at 0x3b39a8). Past it the timer,
//                                     the made count and the replication marks (0x282dc4, host only); w0 is not read again
//   0x3b39a8 (midhook)                that `tbz`: w0 = IsMine's answer
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"

#include <cstddef>
#include <cstdint>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::jobs {

namespace {

constexpr const char* kTag = "pkp";
constexpr int kIsMineReg = 0;      // w0 at 0x3b39a8
constexpr int kCompReg = 19;       // x19 = the job component
constexpr uint64_t kCompActor = 8; // u16 actor id
constexpr size_t kLogged = 32;     // creatures named in the log, once each per run

struct State {
    uint16_t logged[kLogged] = {};
    size_t count = 0;
};
State& st() {
    static State s;
    return s;
}

void log_once(::Ctx* c) {
    State& s = st();
    uint16_t id = 0;
    if (!guest::read(c->x[kCompReg] + kCompActor, id)) return;
    for (size_t i = 0; i < s.count; ++i)
        if (s.logged[i] == id) return;
    if (s.count < kLogged) s.logged[s.count++] = id;
    NXLOG_I(kTag, "Co-op jobs: creature 0x%04x is held by a member; the authority ticks its item-making job", id);
}

}  // namespace

int on_job_tick(::Ctx* c) {
    if (c->x[kIsMineReg] & 1) return 0;   // the game's own tick
    if (!link::in_session() || !link::is_authority()) return 0;
    log_once(c);
    c->x[kIsMineReg] = 1;
    return 0;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::jobs
