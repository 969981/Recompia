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

// The co-op rules this build plays a session by, as an entry of the session's mods fingerprint (services ldnapi::set_coop_mods,
// pkp_linkplay.cpp): a co-op launch adds it, so the session's own join check (the co-op module compares the fingerprint on every scan,
// WELCOME and join) refuses a player whose build plays by other rules, and the launcher says so (coop_diag.rs). Raised whenever two
// builds would play one session by different rules; the message layouts have wire::kVersion, but a mismatch there joins and only
// drops messages. 2: the authority alone moves the quests on and a member asks it (pkp_coop_quests.cpp, QuestFlagAsk); a build
// before it sends no entry. 3: every member's bag is kept as a record of ours and written into the bag the game selects
// (pkp_coop_bags.cpp, BagRecord / BagRestore); a build where the game's own join hand-back still owns a member's bag would undo
// that, and an authority without the records would never answer a member's. The launcher knows the "Recompia co-op rules" prefix.
inline constexpr const char* kCoopRules = "Recompia co-op rules 4";

// Pure: the launch's co-op facts from the settings' text. False when a value is missing or malformed, with the reason in *why.
bool coop_launch_parse(const std::string& role, const std::string& group, const std::string& member, CoopLaunch* out, const char** why);

// A feature of the registry (pkp_game.cpp): its install returns at once unless [pkp] coop is on and the launch's facts parse; then it
// registers every co-op hook site (main is not loaded yet). Its frame callback's first call checks the sites (pkp_site_check.h) and arms
// the hooks, or removes them all when one is not as expected; logs once either way.
extern const Feature kCoopFeature;

}  // namespace nxrt::games::pkp
