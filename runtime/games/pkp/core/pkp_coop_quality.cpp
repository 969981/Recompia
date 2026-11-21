// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the game's "connection is unstable" level, judged for the internet (pkp_coop_internal.h; RE: modding-docs
// coop-v2/UNSTABLE-RE.md).
//
// The message and the signal icon come from the game's own link-quality step 0x53ca60 (the network manager's tick, every frame): it takes
// the worst member's ping (the mean of that link's last ten round trips, ms; NM+0xb337c) and loss (10000 minus the delivery ratio in
// 1/10000; NM+0xb3378), and sets the level NM+0xb336c (0 good, 1 and 2 the "unstable" states, 3 the 20 s self-disconnect when the ping
// is over 1499) and the bars NM+0xb3370. Its thresholds are those of a local wireless link, where a round trip is a few milliseconds:
// level 1 from a ping of 330 ms or 3 % loss, level 2 from 950 ms or 10 %. A game over the internet through the relay has a round trip of
// about 200 ms before anything is wrong (the log's send and delivered stamps), so a ping and a little jitter reach 330 and the message
// shows all the time on a connection that is fine.
//
// Bend: the game's step is kept as it is (its self-disconnect, its bars' source, the ping and the loss it measures). Right after it, at
// the entry of the simulation step 0x231890 (before the game reads the level for the icon and the message in the same frame), a
// co-op session re-judges levels 1 and 2 with thresholds for the internet. Level 3 and the self-disconnect are untouched. The ping and
// the loss are logged when the level changes, so a run says which of the two raises it.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::quality {

namespace {

constexpr const char* kTag = "pkp";
// Thresholds for a session over the internet: the ping in ms (the average of ten round trips) and the loss in 1/10000.
constexpr int kPing1 = 800, kPing2 = 1200;
constexpr int kLoss1 = 1000, kLoss2 = 1800;
constexpr double kLogGap = 2.0;   // seconds between two level lines

struct State {
    uint32_t raw = 0, ours = 0;   // the levels of the last line
    double logged = -100.0;
};
State& st() {
    static State s;
    return s;
}

float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

}  // namespace

int on_sim_step(::Ctx*) {
    if (!link::in_session()) return 0;
    const uint64_t nm = guest::rd<uint64_t>(base() + layout::kNetManager);
    if (!guest::ok(nm, layout::kNetQualityPing + 4)) return 0;
    const uint32_t level = guest::rd<uint32_t>(nm + layout::kNetQualityLevel);
    const int loss = guest::rd<int32_t>(nm + layout::kNetQualityLoss);
    const int ping = guest::rd<int32_t>(nm + layout::kNetQualityPing);
    State& s = st();
    uint32_t ours = level;
    if (level == 1 || level == 2) {   // 0 is good already, 3 is the self-disconnect's and stays
        ours = (ping >= kPing2 || loss >= kLoss2) ? 2 : (ping >= kPing1 || loss >= kLoss1) ? 1 : 0;
        float bars = 0.f;
        if (ours == 1) {
            bars = std::min(0.99f, std::max(clamp01(static_cast<float>(ping - kPing1) / static_cast<float>(kPing2 - kPing1)),
                                            clamp01(static_cast<float>(loss - kLoss1) / static_cast<float>(kLoss2 - kLoss1))));
        } else if (ours == 2) {
            bars = 1.f + std::max(clamp01(static_cast<float>(ping - kPing2) / 300.f), clamp01(static_cast<float>(loss - kLoss2) / 3000.f));
        }
        guest::wr<uint32_t>(nm + layout::kNetQualityLevel, ours);
        uint32_t bits;
        std::memcpy(&bits, &bars, sizeof bits);
        guest::wr<uint32_t>(nm + layout::kNetQualityBars, bits);
    }
    const double now = qpc::seconds();
    if ((level != s.raw || ours != s.ours) && now - s.logged >= kLogGap) {
        s.raw = level;
        s.ours = ours;
        s.logged = now;
        NXLOG_I(kTag, "Co-op link: the game's quality level is %u (ping %d ms, loss %d/10000); shown as %u over the internet", level, ping,
                loss, ours);
    }
    return 0;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::quality
