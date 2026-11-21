// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: a log line when the game's link-quality level changes (pkp_coop_internal.h; RE: modding-docs coop-v2/UNSTABLE-RE.md).
//
// The "connection is unstable" message and the signal icon come only from the game's link-quality step 0x53ca60 (the network manager's
// tick): the worst member's ping (mean of the last ten round trips, ms; NM+0xb337c) and loss (10000 minus the ratio of packets received to
// packets the peer says it sent, 1/10000; NM+0xb3378) give the level NM+0xb336c (0 good, 1 and 2 unstable, 3 the 20 s self-disconnect).
// Nothing here changes the level; this only says, at the simulation step's entry (after the network tick, before the HUD reads it), which
// of the two raised it and when, so the notice can be tied to a moment (a request, a save, a stall) in a run's log.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::quality {

namespace {

constexpr const char* kTag = "pkp";
constexpr double kLogGap = 2.0;   // seconds between two lines

struct State {
    uint32_t level = 0;
    double logged = -100.0;
};
State& st() {
    static State s;
    return s;
}

}  // namespace

int on_sim_step(::Ctx*) {
    if (!link::in_session()) return 0;
    const uint64_t nm = guest::rd<uint64_t>(base() + layout::kNetManager);
    if (!guest::ok(nm, layout::kNetQualityPing + 4)) return 0;
    const uint32_t level = guest::rd<uint32_t>(nm + layout::kNetQualityLevel);
    State& s = st();
    const double now = qpc::seconds();
    if (level == s.level || now - s.logged < kLogGap) return 0;
    s.level = level;
    s.logged = now;
    NXLOG_I(kTag, "Co-op link: the game's quality level is now %u (ping %d ms, loss %d/10000)", level,
            guest::rd<int32_t>(nm + layout::kNetQualityPing), guest::rd<int32_t>(nm + layout::kNetQualityLoss));
    return 0;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::quality
