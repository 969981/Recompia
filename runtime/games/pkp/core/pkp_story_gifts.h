// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: Story co-op's gifts for a visitor ([pkp] link_story_coop, pkp_linkplay.h).
//
// A story step can end in a gift a creature hands over (a creature's move: quest 64 sets gift 89's flag, the gift goes to that creature, the
// player talks to it, the gift is received and quest 64 goes on and teaches the move). Gifts are the player's own (save state), but the
// gift manager only gives one to a creature it has linked the gift to, and the game links no gift to a creature while the player is a
// visitor (its "can this creature give gifts" test says no to every visitor): the gift is never queued and the step never ends for the
// visitor. With Story co-op on, that test answers for a visitor as it does at home, so the visitor gets its own gift from the same
// creature as the host, in the host's world. The host and a player at home are untouched.
#pragma once

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kStoryGiftsFeature;

}  // namespace nxrt::games::pkp
