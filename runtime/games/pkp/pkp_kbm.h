// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, keyboard and mouse button prompts (pkp_kbm.cpp): the game's inline button glyphs (the text tag ^30~<action>~) show the key
// bound to that button while the keyboard and mouse were used last. Installed by pkp_play.cpp.
#pragma once
#include <cstdint>
#include <vector>

#include "nxrt/hooks.h"
#include "play_game.h"

namespace nxrt::games::pkp {

void kbm_install(uint64_t base, std::vector<uint32_t>* hooks);
void kbm_frame(::Ctx* c);                             // the game thread, once a frame (pkp_play.cpp frame hook)
void kbm_settings_changed(const play::Settings& s);   // main thread: the key names for every pad button
void kbm_status_text(char* buf, size_t n);

}  // namespace nxrt::games::pkp
