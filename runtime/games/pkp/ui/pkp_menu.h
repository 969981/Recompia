// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus (pkp_menu.cpp): the pointer over the game's UI panes, hover moves the game's own cursor
// there, left click confirms, right click and the wheel are menu bindings (pkp_data.cpp). A feature of the registry; its frame answers
// are wired into play mode's GamePlay table by pkp_game.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "core/pkp_feature.h"
#include "play_game.h"

namespace nxrt::games::pkp {

extern const Feature kMenuFeature;
play::Act menu_frame(bool menu_up, const play::mm::Geometry& g);   // main thread, once per frame (play_game.h GamePlay::menu_frame)
bool menu_over_value();                                             // the pointer is over a slider: the wheel steps it
bool menu_active();                                                 // the menu hooks saw an active pointer this frame
bool menu_in_game(bool in_game);                                    // main thread, once per frame before the context: a menu is up over play
bool pause_menu_up();                                               // the menu up over play is the pause menu (+), any thread

}  // namespace nxrt::games::pkp
