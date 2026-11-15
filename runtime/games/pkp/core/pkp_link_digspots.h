// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, Link Play in the story areas ([pkp] link_interact_everywhere, link_story_coop; pkp_linkplay.h): a story dig spot a
// visitor digs up is still the host's find.
//
// A story dig spot is a block that glows gold with an item buried in it. The game makes the spots from the player's own save (a
// closed story quest unlocks each, a "found" bit ends it), keeps them on the host only in a session, and ends one in a single place:
// the host's own change of that block, which drops the item, sets the found bit and removes the glow. A visitor, who sees a plain
// block there, can break it once the story areas are open to visitors; when the block then goes without that code running, the glow
// stays over the empty cell for good and the item is never given.
//
// So on the host of a session, each frame, a spot whose block was natural terrain and is gone is ended with the game's own code, as
// if the host had dug it up: the item drops at the spot in the host's world.
#pragma once
#include <cstdint>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

// What one spot was seen as, frame after frame.
struct DigSpotWatch {
    uint16_t key = 0;       // the spot's data row; another row is another spot
    uint8_t gone = 0;       // frames in a row its block was not natural terrain, after it was
    bool natural = false;   // its block was natural terrain at some frame
    bool ended = false;     // the game's code was run for it
};
constexpr uint8_t kDigSpotGoneFrames = 30;   // a block missing for a moment (a chunk loading) is not a block dug up

// Pure: one frame's look at a spot (key 0: no valid spot in the current stage). True when the spot is to be ended now.
bool dig_spot_step(DigSpotWatch* w, uint16_t key, bool natural_now);

extern const Feature kLinkDigSpotsFeature;

}  // namespace nxrt::games::pkp
