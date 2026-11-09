// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, button prompts (pkp_kbm.cpp): the game's inline button glyphs (the text tag ^30~<action>~) show the key bound to that
// button while a keyboard style shows, or the pad's own button icon (from the prompt icon pack) while an xbox/playstation style shows.
// A feature of the registry (pkp_game.cpp): its game-frame callback looks the glyph sprites up once the sprite sheets are loaded.
#pragma once

#include "pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kKbmFeature;

}  // namespace nxrt::games::pkp
