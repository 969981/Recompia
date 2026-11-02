// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus (pkp_menu.cpp): the pointer over the game's UI panes, hover moves the game's own cursor
// there, left click confirms, right click and the wheel are menu bindings (pkp_game.cpp). Wired into play mode's GamePlay table by pkp_game.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "play_game.h"

namespace nxrt::games::pkp {

void menu_install(uint64_t base);
void menu_remove();
play::Act menu_frame(bool menu_up, const play::mm::Geometry& g);   // main thread, once per frame (play_game.h GamePlay::menu_frame)
bool menu_over_value();                                             // the pointer is over a slider: the wheel steps it
bool menu_active();                                                 // the menu hooks saw an active pointer this frame
bool menu_open();                                                   // a menu with the game's cursor is on screen (for other parts of pkp)
bool menu_in_game(bool in_game);                                    // main thread, once per frame before the context: a menu is up over play
void menu_status_text(char* buf, size_t n);

}  // namespace nxrt::games::pkp
