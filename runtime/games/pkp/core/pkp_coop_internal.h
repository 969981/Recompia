// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: what the parts of seamless co-op share (pkp_coop.h). One feature, one file per job:
//   pkp_coop.cpp          the feature: the switch, the hook sites, their arming, the dispatch of each site to its part, the frame order
//   pkp_coop_session.cpp  the session: opened (host role) or searched and joined (visit role) by call once the player is in the world;
//                         a joining member is then put next to the host
// The parts run on the game thread (the game's own code at a hook site, or the game-frame tick) unless a comment says otherwise. Each
// part keeps its own state; pkp_coop.cpp resets them all when the feature is removed.
#pragma once
#include <cstdint>

#include "nxrt/abi.h"
#include "core/pkp_coop.h"

namespace nxrt::games::pkp::coop {

// The launch's facts, fixed at install: main's base and this process's role.
uint64_t base();
CoopRole role();

// The game state's Link Play session bits (layout::kSession*); 0 with no game state or no session.
uint8_t session_bits();
// The local player's character (the player manager's first slot); 0 while there is none.
uint64_t local_player();

namespace session {
// Opens or joins the session by role once the player is in the world (play mode's in_game); retries with a backoff; logs each step.
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace session

}  // namespace nxrt::games::pkp::coop
