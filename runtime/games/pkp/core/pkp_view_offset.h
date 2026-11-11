// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: how far a player's rendered camera view is moved off the follow camera's own eye and look-at (world units), published by
// the module that moves it (render/pkp_shoulder_camera.cpp) for the modules that must agree with what the player sees (input/pkp_aim.cpp casts
// its ray from the crosshair, which is the view's centre). Zero when the view is the game's own. Any thread; players 0..3 (split screen).
#pragma once

namespace nxrt::games::pkp::view_offset {

inline constexpr int kPlayers = 4;

struct Offset {
    float x = 0, y = 0, z = 0;
};

void set(int player, float x, float y, float z);   // a player outside 0..3 is ignored
Offset get(int player);                            // zero for a player outside 0..3

}  // namespace nxrt::games::pkp::view_offset
