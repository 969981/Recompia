// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the PC features of play mode (pkp_play.cpp): native render resolution, frame rates above 60 at the right game speed,
// the raw mouse camera, the Joy-Con 2 mouse mode key. A feature of the registry (pkp_game.cpp); the display answers below go into play
// mode's GamePlay table there.
#pragma once
#include <cstdint>

#include "core/pkp_feature.h"
#include "play_game.h"

namespace nxrt::games::pkp {

extern const Feature kPlayFeature;
play::DisplayLive play_display_live();
int play_ui_canvas_height();   // logical layout-camera height; the authored 1080 when no UI override is active
uint32_t play_display_calls();
bool play_follow_live();   // the follow camera updated within the last second: the player is in the world (not the title)
bool play_photo_live();    // the photo camera took look input within the grace time ([pkp] camera_grace_ms): photo mode, in play

}  // namespace nxrt::games::pkp
