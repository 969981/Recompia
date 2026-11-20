// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's held story events (pkp_coop_internal.h; RE: modding-docs coop-v2/HELD-EVENTS-RE.md).
// A queued event whose flag word (rec +0x704) has bit 22 waits, on a Link Play visitor, until 0x313418 says so: on a visitor that is
// "Link Play's request counters are equal" (block map +0x9218378: u16 requests sent, u16 answers taken). Every co-op member is a
// visitor. Once one request goes unanswered the two never meet again, and every such event (story text, the scene after a placement)
// waits for the rest of the session; when the member drops home, the visitor bit clears and they all play at once.
//   0x313418 (entry hook) called by the queue's ready test 0x479c60 (return to main+0x479fec), on a member in a session: when the
//   counters have stood unequal, without the answers moving, for kStall seconds, the answer is "ready" (as at home). While they are
//   equal, or still moving, the game's own test runs. 0x313418's other callers (UI and the Link Play screens) are not touched.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::held {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kReadyTestReturn = 0x479fec;   // 0x479c60's `bl 0x313418` returns here
constexpr double kStall = 5.0;

struct State {
    bool unequal = false;    // the counters stood unequal at the last call
    uint16_t sent = 0, answered = 0;
    double since = 0;        // when they became unequal, or last moved
    bool passing = false;    // the last answer was ours; logged once per stall
    uint32_t passed = 0;     // events let through in this stall
};
State& st() {
    static State s;
    return s;
}

}  // namespace

int on_visitor_sync(::Ctx* c) {
    if (c->x[30] - base() != kReadyTestReturn || role() == CoopRole::Host || !link::in_session()) return 0;
    if (!(session_bits() & layout::kSessionVisitor)) return 0;
    State& s = st();
    const uint64_t bm = guest::rd<uint64_t>(base() + layout::kWorld);
    if (!guest::ok(bm, 8) || !guest::ok(bm + layout::kWorldRequests, 4)) return 0;
    const uint16_t sent = guest::rd<uint16_t>(bm + layout::kWorldRequests), answered = guest::rd<uint16_t>(bm + layout::kWorldRequests + 2);
    const double t = qpc::seconds();
    if (sent == answered) {
        if (s.passing)
            NXLOG_I(kTag, "Co-op held events: Link Play's request counters met again (%u); %u held events were let through", sent, s.passed);
        s = State{};
        return 0;   // the game's own test passes
    }
    if (!s.unequal || sent != s.sent || answered != s.answered) {
        s.unequal = true;
        s.sent = sent;
        s.answered = answered;
        s.since = t;
        return 0;
    }
    if (t - s.since < kStall) return 0;
    if (!s.passing) {
        s.passing = true;
        NXLOG_W(kTag,
                "Co-op held events: Link Play's request counters stand at %u sent / %u answered for %.0f s; story events that wait on "
                "them run now, as at home",
                sent, answered, t - s.since);
    }
    ++s.passed;
    c->x[0] = 1;
    return 1;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::held
