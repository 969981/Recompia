// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's challenge counts (pkp_coop_internal.h). A member's action counts toward the group's challenges (town
// goals), which live in the authority's save: the game's own way for a visitor is a pending add on its challenge manager, which the
// manager's frame sends to the host (0x631020 -> 0x633150, RequestChallengeProgress) and the host applies (0x6333e0). Most handlers
// already run for a member's action on the host; this file lets through the ones that return on a visitor before the add.
//
// The game, 2.0.1 (modding-docs coop-v2/role-census/verify/v2.md):
//   0x6438c0(CM, count)   the "buy items" count (goal type 0x29). Callers: the shop's buy 0xe88868 on the buyer's process, and the host's
//                         request case 0x21f38c (a visitor's buy on Cloud Island, which the host makes). Off Cloud Island (0x77ecb0) it
//                         returns on a visitor at `bl 0x376c4; tbnz w0, #0` (0x643934 / 0x643938), before the loop over the listed
//                         goals: a goal of type 0x29 whose state (the mirror +0xc on a visitor) is 1..4 gets the pending add 0x54fd60
//                         (CM, id, count, 0, 0xffff), the same add the host makes for its own buy. A member's buy off Cloud Island is
//                         made on the member alone (nothing sends it; the purchases log gives the item through the give 0xe889f8, which
//                         never calls 0x6438c0), so the host never counted it.
// Rules:
//   - a member's own buy counts once, through the game's pending add (on_buy_visitor: the visitor answer reads 0 in a session).
#include "core/pkp_coop_internal.h"

namespace nxrt::games::pkp::coop::challenges {

int on_buy_visitor(::Ctx* c) {
    if (link::in_session() && !link::is_authority()) c->x[0] = 0;
    return 0;
}

}  // namespace nxrt::games::pkp::coop::challenges
