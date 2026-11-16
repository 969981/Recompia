// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: what the parts of seamless co-op share (pkp_coop.h). One feature, one file per job:
//   pkp_coop.cpp          the feature: the switch, the hook sites, their arming, the dispatch of each site to its part, the frame order
//   pkp_coop_session.cpp  the session: opened (host role) or searched and joined (visit role) by call once the player is in the world;
//                         a joining member is then put next to the host
//   pkp_coop_link.cpp     the co-op channel: opened with the session, drained once a frame; every rule change goes through the authority
//   pkp_coop_wire.h       the messages: type ids and payload layouts
// The parts run on the game thread (the game's own code at a hook site, or the game-frame tick) unless a comment says otherwise. Each
// part keeps its own state; pkp_coop.cpp resets them all when the feature is removed.
#pragma once
#include <cstdint>
#include <vector>

#include "nxrt/abi.h"
#include "core/pkp_coop.h"
#include "core/pkp_coop_wire.h"

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

namespace link {
// Opens the channel once the session is up, then runs the handlers of what arrived (once a frame). reset() closes it.
void tick(::Ctx* c);
void reset();
// The channel is open and this process is in its session (the authority known); this process's node (wire::kNobody outside it),
// whether it is the authority, and the authority's node.
bool in_session();
uint8_t self();
bool is_authority();
int authority();
// A rule change of this process's player (a shared route): to the authority, which applies it and passes it on to every other member.
bool share(wire::Type type, std::vector<uint8_t>&& payload);
// One message to the authority, to one node, or (the authority) to every member.
bool to_authority(wire::Type type, std::vector<uint8_t>&& payload);
bool to_node(int node, wire::Type type, std::vector<uint8_t>&& payload);
void to_members(wire::Type type, const std::vector<uint8_t>& payload);
}  // namespace link

}  // namespace nxrt::games::pkp::coop
