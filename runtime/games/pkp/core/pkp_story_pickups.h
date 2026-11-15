// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: Story co-op's own copy of a world's important items for every visitor ([pkp] link_story_coop, pkp_linkplay.h).
//
// Items lying in a world are the world's (the drop lists of its save), and in a Link Play session the host decides every pickup: a
// visitor asks, the host gives the item to the visitor and removes it from its world for everyone. With every player on a story of
// their own, a visitor who takes an item a story step needs would leave the host without it. The game has one item kind it treats the
// other way, the memory pieces: the host keeps the piece in its world and the visitor notes it in its own save. This module gives the
// items of the world's "important" list (the game's own per-item table decides which items those are) the same rule:
// - host: an important item a visitor picks up stays in the host's world, free for the host and the other players;
// - visitor: each such item is taken once: what the visitor took is kept in <save dir>/story-pickups/<key>.tsv (stage, item, place),
//   and a pickup of one of them is not asked for again, in this session or a later one, in this host's world or another's.
// Never items of the normal list (materials, crops, what breaking a block gives), never an item a player dropped from the bag, never
// a memory piece (the game's own rule stays). The host's own pickups are the game's: the item is gone for everyone.
#pragma once
#include <cstdint>
#include <string>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

// Pure: the line a taken item has in the visitor's file: the stage, the item word's id and variant bits, the 16 bytes of its place.
std::string story_pickup_key(uint8_t stage, uint32_t item_word, const uint8_t place[16]);

extern const Feature kStoryPickupsFeature;

}  // namespace nxrt::games::pkp
