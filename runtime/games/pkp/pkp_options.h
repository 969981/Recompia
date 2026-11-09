// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options (pkp_options.cpp): the PC settings as tabs and rows of the game's own Settings screen. [pkp] options_menu, on by default.
// A feature of the registry (pkp_game.cpp); its frame callback runs the Mouse Mode Key row's key capture.
#pragma once

#include "pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kOptionsFeature;

}  // namespace nxrt::games::pkp
