// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, title screen (pkp_title.cpp): a "Quit Game" key guide next to "(-) Settings"; (+) (Escape) there opens the runtime's
// "Close the game?" prompt. [pkp] title_quit, on by default. Wired in by pkp_game.cpp.
#pragma once
#include <cstdint>

#include "play_game.h"

namespace nxrt::games::pkp {

void title_install(uint64_t base, play::GameHost* host);
void title_frame();   // main thread, once per frame
bool title_up();      // the title's key guides (set 152) are on screen

}  // namespace nxrt::games::pkp
