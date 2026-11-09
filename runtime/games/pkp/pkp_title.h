// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, title screen (pkp_title.cpp): a "Quit Game" key guide next to "(-) Settings"; (+) (Escape) there opens the runtime's
// "Close the game?" prompt. [pkp] title_quit, on by default. A feature of the registry (pkp_game.cpp).
#pragma once
#include "pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kTitleFeature;

}  // namespace nxrt::games::pkp
