// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer, seamless co-op (pkp_coop.cpp; [pkp] coop, coop_role, coop_group, coop_member, coop_members): a launch the launcher starts as one
// member of a co-op group. The players share one story and one save; the process that opened the session (the authority) holds the save
// files, runs the world effects, grants the talk locks and relays the story state.
//
// The switch is set by the launcher in the environment of one launch (NXRT_PKP_COOP=1, NXRT_PKP_COOP_ROLE=host|visit,
// NXRT_PKP_COOP_GROUP=<id>, NXRT_PKP_COOP_MEMBER=<32 hex digits>, NXRT_PKP_COOP_MEMBERS=<n>); it has no row in the game's Settings, since it cannot change while the
// game runs. Without it the module registers no hook at all: every other launch keeps exactly the game's code paths.
#pragma once
#include <cstdint>
#include <string>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

enum class CoopRole : uint8_t { Host, Visit };

// One launch's co-op facts, as the launcher gave them.
struct CoopLaunch {
    CoopRole role = CoopRole::Host;
    std::string group;
    std::string member;   // 32 hex digits, lowercase
    int members = 0;      // how many members the group has (coop_members); 0 or 1: the session does not wait for anyone
};

// Pure: the launch's co-op facts from the settings' text. False when a value is missing or malformed, with the reason in *why.
bool coop_launch_parse(const std::string& role, const std::string& group, const std::string& member, CoopLaunch* out, const char** why);

// A feature of the registry (pkp_game.cpp): its install returns at once unless [pkp] coop is on and the launch's facts parse; then it
// registers every co-op hook site (main is not loaded yet). Its frame callback's first call checks the sites (pkp_site_check.h) and arms
// the hooks, or removes them all when one is not as expected; logs once either way.
extern const Feature kCoopFeature;

}  // namespace nxrt::games::pkp
