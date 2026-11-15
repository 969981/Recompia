// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, Link Play in the story areas ([pkp] link_interact_everywhere, link_story_coop; pkp_linkplay.h): no Dream Island trip
// from a story area's session.
//
// The trip is the game's own group feature in Palette Town and on a Cloud Island: a poll ("<player> is going to a Dream Island"), then
// everyone who said yes moves to the island. The game never starts it from a story area online (those sessions are Spectator Mode, and
// the island code knows only the two towns as the place to come back to). With the story areas opened to a session, Drifloon's talk
// starts it there: the session is renamed to the Dream Island and gets its arrival point, but the area's terrain stays loaded.
//
// So while the player is online in a story area, the poll's result reads "this player stays" (the game's own path of a player who
// voted no), and a request for the island that still arrives (a visitor's build without this rule) is dropped. Palette Town and the
// Cloud Islands keep the trip.
#pragma once
#include <cstdint>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

// Pure: the stage is a story area (stage table type 1: stages 1-10), where a session has no Dream Island trip.
bool link_trip_blocked(uint8_t stage);

extern const Feature kLinkTripsFeature;

}  // namespace nxrt::games::pkp
