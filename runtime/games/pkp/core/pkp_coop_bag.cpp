// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, the game's own bag writers that fight the records (pkp_coop_internal.h; RE: modding-docs coop-v2/P0-RE.md
// P0RE-11, coop/SAVE-BAG-RE.md, REJOIN-BAG-2-RE.md). In co-op each member's bag is a record of ours (pkp_coop_bags.cpp, PROJECT.md 6.7).
// Vanilla Link Play instead treats a visitor's bag as the host's to keep: the host holds a working copy of it (the mirror, fed from
// the records now), stores it in a per-user table S of its own save and hands it back at the next join; on top of that, stage loads,
// host saves and refused requests move or reset whole bags. Each such path is switched off here, on the side it runs on:
//   0x5728b4 (midhook, the host): in the join sync's bag piece 0x5727c0, the writer's `ldr x0, [x19, #8]` right before the stream
//     writes the one-byte found flag [x29 + 0x1c]. The flag is set to 0 there, so the host sends "no stored bag" and nothing more (a
//     first join's shape); the joiner keeps its own bag.
//   0x5afee0 (entry, every process): the end of every stage load clears the online bag on Cloud Island, and on a visitor in a session
//     (the visitor's bag emptied at each load while visiting). Skipped.
//   0x5affe0 (entry, every process) and 0x5b0d60 (entry): the stage load's step, and the end of a GameShare session, that move the bag
//     into the PC's storage and clear the online bag while the common block's share bit is set. Skipped; the bit is cleared, as the
//     game does after the move. (The GameShare start 0x5b0e00 is not hooked: no guest ever joins GameShare here, matchmaking ends as
//     cancelled, and split screen runs a process per window, so its start state never runs.)
//   0x5b0a60 (entry, the host): on Cloud Island, the host's own S entry written over its bag (overflow into the PC). Skipped.
//   0x5af6a0 (entry, the host): before a host save, each visitor's mirror (and on Cloud Island the host's own bag) stored into S.
//     Skipped: S is never read back into a bag in co-op, and the records are kept by pkp_coop_bags.cpp.
//   Six midhooks (a member): the member's handler of the host's answer to a request (0x226280) resets the whole selected bag to the
//     host's mirror when the host refused one of six request kinds ("Your items have been reset"). Each `cbz x0` right after the bag
//     selector 0x2ecc04 gets x0 = 0, so the game's own "no bag" path runs: the bag keeps what the record holds. Kind 0xb's null path
//     also skips its wait release 0x46aa6c, which the hook calls itself.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstdint>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::bag {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kFoundFlag = 0x1c;   // 0x5727c0's frame: u8 at x29 + 0x1c, 1 when S holds the joiner's bag
constexpr int kFpReg = 29;
constexpr int kMemberReg = 20;          // w20: the member the join snapshot is for (low byte)
constexpr int kOutcomeReg = 19;         // kind 0xb's reply: w19 bit 0, the host's answer (the reset runs on 0, a refusal)
constexpr double kResetLogEvery = 1.0;

double& reset_logged_at() {
    static double at = -1e9;
    return at;
}

}  // namespace

int on_join_bag_send(::Ctx* c) {
    const uint64_t flag = c->x[kFpReg] + kFoundFlag;
    if (!guest::ok(flag, 1)) return 0;
    const uint8_t found = guest::rd<uint8_t>(flag);
    if (found) {
        guest::wr<uint8_t>(flag, 0);
        NXLOG_I(kTag, "Co-op bag: member %u joins with its own bag (this host's stored copy of it is not sent)",
                static_cast<unsigned>(c->x[kMemberReg] & 0xff));
    }
    return 0;
}

int on_stage_bag_clear(::Ctx*) { return 1; }

int on_bag_to_storage(::Ctx* c) {
    const uint64_t flags = c->x[0] + layout::kCommonFlags;
    if (guest::ok(flags, 1)) guest::wr<uint8_t>(flags, guest::rd<uint8_t>(flags) & ~layout::kCommonFlagShare);
    return 1;
}

int on_host_bag_store(::Ctx*) { return role() == CoopRole::Host ? 1 : 0; }

int on_refusal_reset(::Ctx* c, bool releases_wait) {
    if (role() != CoopRole::Visit || !c->x[0]) return 0;
    c->x[0] = 0;
    if (releases_wait) guest::call(c, base() + layout::kResourceWaitEnd, c->x[kOutcomeReg] & 1);
    const double now = qpc::seconds();
    if (now - reset_logged_at() >= kResetLogEvery) {
        reset_logged_at() = now;
        NXLOG_I(kTag, "Co-op bag: a host refusal's bag reset is skipped; the record keeps the bag");
    }
    return 0;
}

}  // namespace nxrt::games::pkp::coop::bag
