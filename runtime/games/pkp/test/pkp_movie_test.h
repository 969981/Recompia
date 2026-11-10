// SPDX-License-Identifier: GPL-3.0-or-later
// Test hook: plays a movie through the game's own player and saves decoded frames ([test] movie = MV02). See pkp_movie_test.cpp.
// A feature of the registry (pkp_game.cpp) with a game-frame callback only.
#pragma once

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kMovieTestFeature;

}  // namespace nxrt::games::pkp
