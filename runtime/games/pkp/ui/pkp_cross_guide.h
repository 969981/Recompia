// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the cross key guide (pkp_cross_guide.cpp): the prompt beside the player, the action's button large with the other
// three face buttons faint around it. The game draws the four where they sit on a Switch pad; with an Xbox layout ([pad] label_abxy) or
// remapped face buttons ([padmap]) they sit elsewhere on the player's pad, and this module draws them there: the same pad buttons the
// prompt icons show (pkp_kbm.cpp), each at its own place.
// A feature of the registry (pkp_game.cpp).
#pragma once

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kCrossGuideFeature;

}  // namespace nxrt::games::pkp
