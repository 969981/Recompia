// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's member bags (pkp_coop_internal.h; RE: modding-docs coop-v2/P0-RE.md P0RE-11 and its second pass,
// coop/SAVE-BAG-RE.md).
// Vanilla Link Play keeps a visitor's island bag on the host: the host holds a working copy of each visitor's bag (R+0x17154a8 +
// idx*0x820, the mirror), stores it under the visitor's user id in a table S of its own save (R+0x6bee48, or R+0xfd85a0 on stage
// 0x11) when the visitor leaves (0x6027c0) and at every host save (0x5af6a0), and hands it back at the next join: the join sync's bag
// piece 0x5727c0 sends {found, 130 x (item, count)} for the joiner's user id, and the joiner overwrites its selected bag (the online
// bag R+0x13d4930 for a visitor) with it. The mirror only follows what goes through the host (pickups, station crafts); quest and
// script items, item use and the co-op catch-up gives never reach it, and it is cleared at each join.
// In co-op every member's bag is its own (its own copy of the co-op save holds it), so that hand-back replaced a rejoining member's
// bag with the host's stale copy: a member who dropped or left and joined again found a bag holding only what the host had seen
// it pick up since its last join (issue: blank bag after a rejoin).
//   0x5728b4 (midhook), 0x5727c0's writer path (the host's join snapshot; the joiner's read branches away at 0x5727e0 before it): the
//   `ldr x0, [x19, #8]` right before the stream writes the one-byte found flag at [x29 + 0x1c]. The flag is set to 0 there, so the
//   host sends "no stored bag" and nothing more; the joiner reads that and keeps its own bag. The stream stays the game's own shape
//   (a 0 flag is what a first join sends). The host's mirror is still cleared and filled as the game does (0x575644 at join state
//   3 -> 4): the host's request checks keep the copy they had before.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"

#include <cstdint>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::bag {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kFoundFlag = 0x1c;   // 0x5727c0's frame: u8 at x29 + 0x1c, 1 when S holds the joiner's bag
constexpr int kFpReg = 29;
constexpr int kMemberReg = 20;          // w20: the member the join snapshot is for (low byte)

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

}  // namespace nxrt::games::pkp::coop::bag
