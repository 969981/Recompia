// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: Story co-op's rule for quests that change a world ([pkp] link_story_coop, pkp_linkplay.h). A story quest's completion
// runs up to four effect commands of its table row (and four more in its second phase): most are the player's own (rewards, flags of
// the story), some change who lives in the town or what stands in it. Done as a visitor, such an effect would mark the visitor's own
// save ("this creature lives in my town") while the creature and its home were made in the host's world, or not at all: the game refuses
// the arrival to a visitor (0x9bdf50), and a world's residents come from that world's own save, never from the player's record.
//
// So while the player is a visitor, only those commands wait: the quest completes as the game always does, with every other command (a
// move, an item, the story's own flags) run at once, and each held command (its quest, command set and slot) is noted next to the save,
// <save dir>/story-held/<key>.tsv. Back in the player's own world (offline or hosting) the game runs each noted command once, as the
// quest's completion would have, with the world changes made there; one whose quest is not completed in the save (a save from before
// it was put back: the launcher's import, a story-coop-backup copy) is dropped instead. The host is never held: the world is theirs.
// Journal lines (pkp_story_guard.h): "skip" and "replay" (and "replay-dropped" for a command that is no longer a held type or whose
// quest is not completed), the effect type in the before column and set * 4 + slot in the after column.
#pragma once
#include <cstdint>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

// Pure: true for the effect command types (the first byte of a quest row's 0x14-byte command, run by 0x11b214) that are held:
// 0x26 registers a character in the town record and starts its arrival, 0x39 gives a character its home, 0x38 and 0x0f bring a
// character into the stage or take it out, 0x05, 0x06, 0x2e, 0x2f and 0x37 write the town record's other bits, 0x0d ends a character's
// part in the step (0x123384: the live character, 0x28cc60, is released from its partner and its talk state is reset, 0x2a1f44,
// 0x2a1dc0, 0x291fcc, 0x29205c); run by a visitor, that is the host's character, which then no longer starts the host's own step with
// it (a visitor who took a creature's move first left the host's quest 79 unable to start). Not held: 0x10, where the character (live in
// the stage) teaches its move to the local player and is marked in the town record as having taught it; it only works where that
// character stands, so a visitor learns the move in the host's world.
bool story_effect_held(uint8_t type);

// The registry's feature after story_guard (it writes the journal's "skip" and "replay" lines through it).
extern const Feature kStoryHoldFeature;

}  // namespace nxrt::games::pkp
