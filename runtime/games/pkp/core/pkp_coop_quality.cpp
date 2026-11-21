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

constexpr double kGapWindow = 5.0;   // seconds of frame gaps the level line looks back over

struct State {
    uint32_t level = 0;
    double logged = -100.0;
    double last_step = 0;      // the previous simulation step
    double worst_gap = 0;      // the longest time between two steps in the current window
    double gap_since = 0;      // the window's start
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
    // A long frame here delays this process's answers to the others' pings, which raises their ping: the line says how long the
    // longest frame of the last few seconds was, so a stall can be told from the network.
    if (s.last_step > 0 && now - s.last_step > s.worst_gap) s.worst_gap = now - s.last_step;
    s.last_step = now;
    const double worst = s.worst_gap;
    if (now - s.gap_since > kGapWindow) {
        s.gap_since = now;
        s.worst_gap = 0;
    }
    if (level == s.level || now - s.logged < kLogGap) return 0;
    s.level = level;
    s.logged = now;
    NXLOG_I(kTag, "Co-op link: the game's quality level is now %u (ping %d ms, loss %d/10000; longest frame here in the last %.0f-%.0f s: %.0f ms)",
            level, guest::rd<int32_t>(nm + layout::kNetQualityPing), guest::rd<int32_t>(nm + layout::kNetQualityLoss), kGapWindow,
            2 * kGapWindow, worst * 1000.0);
    return 0;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::quality
