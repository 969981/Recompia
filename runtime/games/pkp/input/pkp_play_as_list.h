// SPDX-License-Identifier: GPL-3.0-or-later
// Play as Pokemon, the Pokemon the player has copied (pkp_play_as_list.cpp): the list the mod's copy adds to (pkp_play_as.cpp) and the
// Play as tab of the game's Settings screen picks from (ui/pkp_options_play_as.cpp), kept in play_as_pokemon.tsv in the game folder,
// never in the save. The game thread only. The mod starts and stops it (start, stop); while it is stopped there are no names.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/abi.h"

namespace nxrt::games::pkp::play_as {

struct Copied {
    uint16_t number = 0;    // the character number: the form the player turns into
    uint16_t species = 0;   // its id in the game's species name table (layout::kSpeciesName), 0 when not known
};

void start(uint64_t base);             // main's base, for the names
void stop();

constexpr size_t kMost = 20;           // the list's places; a full list takes no new Pokemon until one is removed

const std::vector<Copied>& copied();   // oldest first; read from the file at the first call
bool remember(Copied c);               // adds it and writes the file (a number already in the list keeps its place); false: the list is full
void forget(uint16_t number);          // takes it out of the list and writes the file

// The species' name in the game's language (UTF-8), empty when the game has none.
std::string species_name(::Ctx* c, uint16_t species);

}  // namespace nxrt::games::pkp::play_as
