// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: what the parts of seamless co-op share (pkp_coop.h). One feature, one file per job:
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
//   pkp_coop_trace.cpp    the trace for testers: quest words, players and characters that move, a wall clock, in the log
//   pkp_coop_notice.cpp   lines in the game's own message log: what another member's event unlocked or moved on here, by name
//   pkp_coop_curtain.cpp  the cover over the picture while a member joins or leaves (the PC, transfer, landing and warp)
//   pkp_coop_requests.cpp the creature requests a member took on or finished, listed in every member's requests app
//   pkp_coop_outcomes.cpp what a finished request adds to the world (town goal, friendship), applied once for the group
//   pkp_coop_recipes.cpp  the crafting recipes any member knows, learned by every member
// The parts run on the game thread (the game's own code at a hook site, or the game-frame tick) unless a comment says otherwise. Each
// part keeps its own state; pkp_coop.cpp resets them all when the feature is removed.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/overlay_canvas.h"
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
int on_quest_flag(::Ctx* c);     // entry 0x5d8774: logged only
int on_quest_effect(::Ctx* c);   // entry 0x11b214: world effects run on the authority only
// The messages (pkp_coop_link.cpp).
void on_states_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_ability_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_digest_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Moves the quests behind another member's state one step on, shares the quest states and ability bits that rose here (once a frame,
// after the channel's handlers).
void tick(::Ctx* c);
// This process joined or left a session: the other members' states are learnt again, and all of this process's are sent.
void on_session_change();
// Another member joined: all of this process's quest states are sent again, so the newcomer catches up.
void on_member_joined();
void reset();
}  // namespace story

namespace requests {
// The hook site (pkp_coop.cpp): entry 0x11435c, the bind gate; a member answers as the game does at home.
int on_giver_check(::Ctx* c);
// The message (pkp_coop_link.cpp): kept until this player can take it.
void on_unique_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Applies what other members shared when this player can walk and talks to no one, reads this player's request records and shares
// what changed (once a frame, after the channel's handlers).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: the records are read again, the whole list sent
void on_member_joined();    // another member joined: the whole list is sent again
void reset();
// For the outcomes: the id's slot in the unique request manager (0: no manager), the request component of the creature bound to a
// slot and loaded here (0: none), and the request lists.
uint64_t request_slot(uint32_t id);
uint64_t request_component(::Ctx* c, uint64_t slot);
bool story_request(uint32_t id);   // also a story step: one completion closes it for the group
bool own_request(uint32_t id);     // bound to one player: never shared
}  // namespace requests

namespace outcomes {
// The hook sites (pkp_coop.cpp), in the completion step 0x618304.
int on_goal_count(::Ctx* c);   // midhook 0x61858c: decides the run's town goal (+1) and friendship; x0 = 0 skips the goal
int on_friendship(::Ctx* c);   // entry 0x619410: the step's friendship, skipped as decided
// The message (pkp_coop_link.cpp): a member finished a request; the authority applies its world part once.
void on_outcome_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// A member sends its finished ids; the authority runs a waiting world part at a safe moment (once a frame, after the requests).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace outcomes

namespace recipes {
// The message (pkp_coop_link.cpp): kept until this player can learn it.
void on_known_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Teaches what other members know when this player can walk and talks to no one, reads this player's recipes and shares the new
// ones (once a frame, after the channel's handlers).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: the recipes are read again, the whole list sent
void on_member_joined();    // another member joined: the whole list is sent again
void reset();
}  // namespace recipes

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
// This player's name as the save keeps it for the game's language; empty before the name entry.
std::string player_name();
}  // namespace catchup

namespace notice {
// A line in the game's message log now, on the player's event slot.
void line(::Ctx* c, uint8_t slot, const char* text);
// A line for this player, shown when the player can walk (queued; once a frame in tick).
void post(const std::string& text);
// The name another member's player goes by (MemberName), or "Your teammate".
std::string name_of(uint8_t node);
void set_slot(uint8_t slot);   // this player's event slot, from a talk start
void on_name_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: names are learnt again, ours is sent
void on_member_joined();    // another member joined: ours is sent again
void reset();
}  // namespace notice

namespace curtain {
// Covers the picture (fading in) with a line of text, until lift; lift(after) keeps it up that much longer first. Game thread.
void cover(const char* text);
void lift(double after = 0.0);
void hint(const char* text);   // a second line under the text, until the next cover
bool covered();   // up and fully opaque: what it hides is not seen
// The overlay's presenting thread (the feature's overlay layer).
bool overlay_active();
void overlay_draw(gfx::OverlayCanvas& c);
void reset();
}  // namespace curtain

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

namespace trace {
// Logs what changed since the last look (once a frame in a session): quest words, the players' and the characters' places.
void tick(bool in_game);
void reset();
}  // namespace trace

}  // namespace nxrt::games::pkp::coop
