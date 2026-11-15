// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play time for the launcher (pkp_play_time.cpp). The game counts its save's play time itself (layout::kSavePlayTime);
// while the player is in the game this module copies it once a minute to <save dir>/play_time.ini, one line per save:
// [play_time] <account folder name> = whole seconds. The launcher shows it on its Home page and in the profile list (launcher
// play_time.rs). It is the game's own count, so it holds the time played before this build too, and a copy is never added to: the file
// is at most a minute behind the game (the process ends without removing the features), and the next run goes on from the save's count.
// A port() feature of the registry (pkp_game.cpp): no setting, and nothing in the game changes.
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kPlayTimeFeature;

}  // namespace nxrt::games::pkp
