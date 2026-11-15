// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: Story co-op's rule for quests that change a world ([pkp] link_story_coop, pkp_linkplay.h). A story quest's completion
// runs up to four effect commands of its table row (and four more in its second phase): most are the player's own (rewards, flags of
// the story), some change who lives in the town or what stands in it. Done as a visitor, such an effect would mark the visitor's own
// save ("this Pokémon lives in my town") while the Pokémon and its home were made in the host's world, or not at all: the game refuses
// the arrival to a visitor (0x9bdf50), and a world's residents come from that world's own save, never from the player's record.
//
// So while the player is a visitor, a quest with such an effect is not completed: the quest pump (0x11b600) sees its "done" flag as not
// set and leaves it in the active list, with the flag kept. In the player's own world the pump completes it as the game always does,
// with the world changes made there. Quests with only the player's own effects complete anywhere. The host is never held: the world
// is theirs.
#pragma once
#include <cstdint>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

// Pure: true for the effect command types (the first byte of a quest row's 0x14-byte command, run by 0x11b214) that are held:
// 0x26 registers a character in the town record and starts its arrival, 0x39 gives a character its home, 0x38 and 0x0f bring a
// character into the stage or take it out, 0x05, 0x06, 0x10, 0x2e, 0x2f and 0x37 write the town record's other bits.
bool story_effect_held(uint8_t type);

// The registry's feature after story_guard (it writes the journal's "hold" lines through it).
extern const Feature kStoryHoldFeature;

}  // namespace nxrt::games::pkp
