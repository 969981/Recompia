// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, Link Play "Interact everywhere" (pkp_linkplay.cpp, [pkp] link_interact_everywhere): an experimental opt-in that gives
// a Link Play session in the story areas Palette Town's Spectator-Off permissions (visitors can use moves and items, pick up, place and
// build). Every player of a session must have the same setting: it is sent as the ldn session variant (services_api.h ldnapi), so a peer
// whose setting differs is refused when it joins.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

// One supported main build: the permission-mask builder `fn`, the site in it and the instruction word expected there (offsets in main).
struct InteractSite {
    const char* build;   // main's build id, lowercase hex (a prefix of Module::build_id())
    uint64_t fn, site;
    uint32_t word;
};
enum class InteractCheck { Ok, UnknownBuild, NotLifted, WordDiffers };

constexpr uint8_t kInteractVariant = 1;                   // the ldn session variant while the hook is armed
constexpr const char* kInteractName = "Interact everywhere";
// Story co-op's name in the co-op mods fingerprint (ldnapi::set_coop_mods). Not pokopia-native's COOP_STORY_MOD: here every player keeps
// a story of their own (below), there only the host does, so a session refuses a player whose Story co-op is the other one.
constexpr const char* kStoryName = "Story co-op: own story (experimental)";

// Pure: the site of main build `build_id` (null when none is known).
const InteractSite* interact_site(const std::string& build_id);
// Pure: whether the hook may be armed for this main: a known build, the site lifted as its own function (the lifted libs carry the
// midhook) and the expected instruction word at it. *site: the build's site, or null.
InteractCheck interact_check(const std::string& build_id, bool site_lifted, uint32_t word, const InteractSite** site);
// Pure: the register change at the site (x = x0..x30): while the session's Spectator flag (x22 bit 0) is clear, the Spectator pattern in
// x13 becomes the normal online one (0x5ffffffe; the movk at the site then adds bit 32, online), with bit 0 (the story system) when
// keep_story. True when x13 changed.
bool interact_apply(uint64_t* x, bool keep_story);

// A feature of the registry (pkp_game.cpp): its install registers the midhook when [pkp] link_interact_everywhere is on (main is not
// loaded yet, so nothing is checked there); its frame callback's first call checks main (its build id, the lifted site, the instruction
// word) and arms the hook or removes it, through linkplay_arm(); logs once either way.
extern const Feature kLinkplayFeature;
// The decision of the frame callback with main's facts given: arms the hook and sets the ldn session variant on Ok, else removes the hook
// and clears the variant. Returns the check. Does nothing (returns Ok) when the hook was not installed.
InteractCheck linkplay_arm(const std::string& build_id, bool site_lifted, uint32_t word);

// Story co-op ([pkp] link_story_coop): every player of a session plays their own story in the story areas, the host and each visitor alike.
// The story's state is the player's own save (m_saveCommon, CMNDAT: m_story's quest history and flags), so each player's progress stays in
// their own save, under their own UUID. Two of the game's rules stop it online, and the sites below lift both for every player:
// - the permission mask's bit 0 switches the story system on (the per-frame story update 0xba660, the quest pump 0x11b600): every online
//   pattern clears it; the mask site keeps it, and the builder's first instruction (Role) forgets it before each rebuild;
// - a Pokémon's talks are picked by conditions (table main+0x34c2958): the story and request talks need 0x19 "not online" (0x4cfe10) or
//   0x60 (0x4d4844), their stand-in "bring <the town's owner> here" needs 0x22 "online" (0x4d04fc, `b IsOnline`); while the story is
//   kept, the three read the player as offline.
// And a visitor's talk takes the host's path in the talk handler (Talk: a visitor's reply has no conversation). Only with [net] protocol =
// native (it is in the mods fingerprint after Interact everywhere); turns Interact everywhere on with it; armed by the frame callback when
// every site is lifted with its expected word.
enum class StorySiteKind : uint8_t { Talk, Role, NotOnline, Online };
struct StorySite {
    uint64_t site;
    uint32_t word;
    StorySiteKind kind;
    bool entry;          // an entry hook (the function is the one instruction); else a midhook, run before the instruction at `site`
};
// Pure: the story sites of main build `build_id` (*count of them; null and 0 when the build is not supported).
const StorySite* story_sites(const std::string& build_id, size_t* count);

}  // namespace nxrt::games::pkp
