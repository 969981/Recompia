// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: what the parts of seamless co-op share (pkp_coop.h). One feature, one file per job:
//   pkp_coop.cpp          the feature: the switch, the hook sites, their arming, the dispatch of each site to its part, the frame order
//   pkp_coop_session.cpp  the session: opened (host role) or searched and joined (visit role) by call once the player is in the world;
//                         a joining member is then put next to the host
//   pkp_coop_link.cpp     the co-op channel: opened with the session, drained once a frame; every rule change goes through the authority
//   pkp_coop_wire.h       the messages: type ids and payload layouts
//   pkp_coop_story.cpp    the shared story: full permissions on every process, quest flags and abilities shared
//   pkp_coop_talk.cpp     the talk lock: one player at a time talks to a character
//   pkp_coop_catchup.cpp  what the talker's talk gives the talker alone (a name, items) given to every other member too
//   pkp_coop_gate.cpp     the wait for every member: this player's input is held until the whole group is in the session
//   pkp_coop_lock.cpp     the input lock the catch-up's name prompt and the gate share
// The parts run on the game thread (the game's own code at a hook site, or the game-frame tick) unless a comment says otherwise. Each
// part keeps its own state; pkp_coop.cpp resets them all when the feature is removed.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nxrt/abi.h"
#include "core/pkp_coop.h"
#include "core/pkp_coop_wire.h"

namespace nxrt::games::pkp::coop {

// The launch's facts, fixed at install: main's base, this process's role and how many members the group has (0: not given).
uint64_t base();
CoopRole role();
int members();

// The game state's Link Play session bits (layout::kSession*); 0 with no game state or no session.
uint8_t session_bits();
// The local player's character (the player manager's first slot); 0 while there is none.
uint64_t local_player();

namespace session {
// Opens or joins the session by role once the player is in the world (play mode's in_game); retries with a backoff; logs each step.
void tick(::Ctx* c, bool in_game);
void reset();
// The player walks: in the world, no event running, no autosave in progress.
bool can_walk(bool in_game);
}  // namespace session

namespace link {
// Opens the channel once the session is up, then runs the handlers of what arrived (once a frame). reset() closes it.
void tick(::Ctx* c);
void reset();
// The channel is open and this process is in its session (the authority known); this process's node (wire::kNobody outside it),
// whether it is the authority, and the authority's node.
bool in_session();
// How many other members the channel has (not this process).
size_t member_count();
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

namespace story {
// The hook sites (pkp_coop.cpp): each returns what the site's hook returns.
int on_mask_start(::Ctx* c);     // midhook 0xb0f74: the mask builder's arguments
int on_mask_pattern(::Ctx* c);   // midhook 0xb1030: the story-area online pattern
int on_quest_state(::Ctx* c);    // entry 0x5d865c
int on_quest_flag(::Ctx* c);     // entry 0x5d8774
int on_quest_effect(::Ctx* c);   // entry 0x11b214: world effects run on the authority only
int on_ability(::Ctx* c);        // entry 0x121bb8, 0x1227fc
// The messages (pkp_coop_link.cpp).
void on_flag_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_ability_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Applies the received flags this process has caught up with (once a frame, after the channel's handlers).
void tick(::Ctx* c);
// This process joined or left a session: received flags still waiting belong to the old one.
void on_session_change();
void reset();
}  // namespace story

namespace talk {
// The hook sites (pkp_coop.cpp).
int on_talk_start(::Ctx* c);   // entry 0x3180e0: the lock is asked; refused, the busy line and no talk
int on_no_talk(::Ctx* c);      // midhook 0x3181cc: no talk started after all
int on_talk_end(::Ctx* c);     // entry 0x47bd00: a slot's event chain ended
// This process's player holds a character's talk and the talk runs.
bool talking();
// The messages and member events (pkp_coop_link.cpp).
void on_ask(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_done(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_holder(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_member(::Ctx* c, int node, bool joined, bool authority, bool self);
// The lease of an unused grant and the watchdog of a talk with nothing running (once a frame).
void tick();
void reset();
}  // namespace talk

namespace catchup {
// The hook sites (pkp_coop.cpp).
int on_name_entry(::Ctx* c);     // entry 0x43d3e0: script command 0x44, the name entry
int on_script_items(::Ctx* c);   // entry 0x12c734: script command 0xbea, items given
// The messages (pkp_coop_link.cpp).
void on_name_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_items_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// The talker's name watch, this player's name prompt and the received items (once a frame).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace catchup

namespace lock {
// The local character's input lock, claimed by one part each: the bit stays set while any claim is on the character.
enum class Holder : uint8_t { Name, Gate };
void hold(Holder who, uint64_t chara);   // idempotent; a claim on another character is let go first
void release(Holder who);                // no claim left on the character: the lock is cleared
}  // namespace lock

namespace gate {
// Holds this player's input, with a notice, while fewer members than the group has are in the session (once a frame).
void tick(bool in_game);
void reset();
}  // namespace gate

}  // namespace nxrt::games::pkp::coop
