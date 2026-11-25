// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's records run on a member's behalf (pkp_coop_internal.h). A member's notice that the authority posts
// in its own contents scheduler (pkp_coop_feed.cpp: an event's end, an object's use) makes the authority's copy of a record run,
// for the group's story. When the member's own copy of that record ran too, the member has had what the record does to the acting
// player: its scene, its warp, its push. The authority's run of the same record would give those to the host's player, who did
// nothing (a member's press of a story gate played the gate's refusal on the host's screen). So for such a record the authority
// runs everything but those actions.
//
// The contents scheduler SC (RE: modding-docs coop-v2/quests/QUEST-START-RE.md sections 1-3): a notifier 0x1353a0 matches its
// list's records (0x133020) and puts each that passes into the run queue with 0x1350c0(SC, run slot), whose first u64 is the
// record; its two calls of it return to 0x13552c and 0x13559c. The tick then runs each queued slot with 0x131200(run slot), which
// calls the action interpreter 0x12f690 per action; a record stays queued (record + 0x2c bit 1) until it has run.
//   0x1350c0 (entry hook), called from the notifier while the feed posts a member's notice whose own record took it there
//   (begin / end around the post): the record is remembered with the member's node.
//   0x131200 (entry hook): the record whose actions run from now.
//   0x12b544 (the entry hook co-op's contents scenes have), 0x12af00, 0x12b108, 0x12dbb8 (entry hooks): the actions on the acting
//   player: play scene 2000 (it queues on this process's first event slot), the warps 0x4a6 / 0x4ab and the push 0xe74 (each on
//   this process's own player). While a remembered record runs, on the authority, they do not run (the handlers return nothing the
//   interpreter reads: it steps by the action's own length).
// A record is forgotten once it is no longer queued, or after kKeep seconds.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::behalf {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kQueuedFromA = 0x13552c, kQueuedFromB = 0x13559c;   // the notifier's two returns from 0x1350c0
constexpr uint64_t kRecordFlags = 0x2c;   // record + : u16, bit 1 queued
constexpr uint16_t kQueuedBit = 0x2;
constexpr uint64_t kRecordContent = 0, kRecordId = 4;   // record + : u32 its content entry, u32 its id in the content
constexpr double kKeep = 120.0;
constexpr size_t kMost = 16;

struct Quiet {
    uint64_t record;
    uint8_t origin;   // the member whose own copy ran
    double at;
    bool logged;
};

struct State {
    std::vector<Quiet> quiet;
    int posting = -1;       // the member whose notice the feed is posting now (-1: none)
    uint64_t running = 0;   // the record whose actions run now
};
State& st() {
    static State s;
    return s;
}

Quiet* find(uint64_t record) {
    for (Quiet& q : st().quiet)
        if (q.record == record) return &q;
    return nullptr;
}

}  // namespace

void begin(uint8_t origin) { st().posting = origin; }
void end() { st().posting = -1; }

int on_record_queued(::Ctx* c) {
    State& s = st();
    if (s.posting < 0) return 0;
    const uint64_t from = c->x[30] - base();
    if (from != kQueuedFromA && from != kQueuedFromB) return 0;
    uint64_t record = 0;
    if (!guest::read(c->x[1], record) || !guest::ok(record, kRecordFlags + 2) || find(record) || s.quiet.size() >= kMost) return 0;
    s.quiet.push_back(Quiet{record, static_cast<uint8_t>(s.posting), qpc::seconds(), false});
    return 0;
}

int on_record_run(::Ctx* c) {
    State& s = st();
    if (s.quiet.empty()) {
        s.running = 0;
        return 0;
    }
    uint64_t record = 0;
    s.running = guest::read(c->x[0], record) ? record : 0;
    return 0;
}

int on_actor_action(::Ctx*) {
    State& s = st();
    if (s.quiet.empty() || !s.running || !link::in_session() || !link::is_authority()) return 0;
    Quiet* q = find(s.running);
    if (!q) return 0;
    if (!q->logged) {
        q->logged = true;
        NXLOG_I(kTag, "Co-op feed: content entry %u record %u runs here for node %u, whose own copy ran: its scene, warp and push are "
                "that player's and are not made here", guest::rd<uint32_t>(q->record + kRecordContent),
                guest::rd<uint32_t>(q->record + kRecordId), q->origin);
    }
    return 1;
}

void tick() {
    State& s = st();
    if (s.quiet.empty()) return;
    if (!link::in_session() || !link::is_authority()) {
        s = State{};
        return;
    }
    const double now = qpc::seconds();
    for (size_t i = 0; i < s.quiet.size();) {
        const Quiet& q = s.quiet[i];
        const bool queued = guest::ok(q.record, kRecordFlags + 2) && (guest::rd<uint16_t>(q.record + kRecordFlags) & kQueuedBit);
        if (queued && now - q.at < kKeep) {
            ++i;
            continue;
        }
        if (s.running == q.record) s.running = 0;
        s.quiet.erase(s.quiet.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::behalf
