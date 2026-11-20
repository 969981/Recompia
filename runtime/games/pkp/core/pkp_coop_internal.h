// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: what the parts of seamless co-op share (pkp_coop.h). One feature, one file per job:
//   pkp_coop.cpp          the feature: the switch, the hook sites, their arming, the dispatch of each site to its part, the frame order
//   pkp_coop_session.cpp  the session: opened (host role) or searched and joined (visit role) by call once the player is in the world;
//                         a joining member is then put next to the host
//   pkp_coop_link.cpp     the co-op channel: opened with the session, drained once a frame; every rule change goes through the authority
//   pkp_coop_wire.h       the messages: type ids and payload layouts
//   pkp_coop_story.cpp    the shared story: full permissions on every process, quest flags and abilities shared
//   pkp_coop_quests.cpp   the quest authority: in a session only the authority moves quests on; a member asks it for its player's steps
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
//   pkp_coop_unlocks.cpp  the moves and emotes any member learned, the areas visited and buildings built, unlocked for every member
//   pkp_coop_purchases.cpp what any member buys in a shop, given to every member (and to later joiners) too
//   pkp_coop_events.cpp   what an ended talk or scene leaves in the save (scenes played, conversations seen, story lines moved on)
//                         kept by every member
//   pkp_coop_held.cpp     the story events a member's game holds for Link Play's request counters: let through once they are stuck
//   pkp_coop_linkqueue.cpp Link Play's request queue on a member: a talk-end record the busy queue would drop is kept and sent once it
//                         is idle; a strict request left out by an earlier session is cleared at the next session start
//   pkp_coop_travel.cpp   the group's trips: a move to another area runs only after the game's poll passed, and the host makes it for all
//   pkp_coop_discovery.cpp the Pokédex entries and habitats any member found, found by every member, with the game's own scenes
//   pkp_coop_placement.cpp a member's "object placed" notice to the contents, raised once the host has put the object in its world
//   pkp_coop_contents.cpp  a scene a content queued that the event queue refused: asked for again until it plays
//   pkp_coop_bag.cpp      a member's bag stays its own at a join: the host's stored copy of it is never sent
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
// This player's member key (the launch's 32 lower-case hex digits; the same on every launch of this player in the group).
const std::string& member_key();

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
int on_quest_flag(::Ctx* c);     // entry 0x5d8774: logged (a member's set is pkp_coop_quests.cpp's first)
int on_quest_effect(::Ctx* c);   // entry 0x11b214: world effects run on the authority only
int on_wild_pop(::Ctx* c);       // entry 0x1240dc: a member's wild-creature appearance is sent to the authority
int on_town_action(::Ctx* c);    // entry 0x10c270: story-line step action 6 (town register) runs on the authority only
int on_ability_effect(::Ctx* c); // entry 0x1227fc (quest effect 0x19): a clear (+4 value 0) is kept and shared
int on_ability_step(::Ctx* c);   // entry 0x10c0d0 (story-line step, ability set/clear): a clear (+6 value 0) likewise
// The messages (pkp_coop_link.cpp).
void on_states_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_ability_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_digest_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_wild_pop_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_cleared_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Moves the quests behind another member's state one step on, shares the quest states and ability bits that rose here (once a frame,
// after the channel's handlers).
void tick(::Ctx* c);
// This process joined or left a session: the other members' states are learnt again, and all of this process's are sent.
void on_session_change();
// Another member joined: all of this process's quest states are sent again, so the newcomer catches up.
void on_member_joined();
void reset();
// The group's world is past its opening: the first talk's quest is closed in this save or on another member (saved, so it holds
// across boots, and a newcomer learns it from the authority's states).
bool world_started();
}  // namespace story

namespace quests {
// The hook site (pkp_coop.cpp): entry 0x5d8774, a quest's flag set. On a member in the session the set does not run (returns 1);
// a step its own player made is asked of the authority. 0 everywhere else (the game's own set runs).
int on_quest_flag(::Ctx* c);
// Sets quest id's flag in S as co-op's own step (the catch-up, the authority's answer): never skipped by on_quest_flag.
void set_flag(::Ctx* c, uint64_t s, uint32_t id);
// This process is a member in the session (the visit role, the game's visitor bits, the channel up and not the authority): it
// moves no quest by itself and shares no quest state.
bool member_rules();
// The message (pkp_coop_link.cpp): a member's ask, applied on the authority.
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Sends a member's asks; the authority retries the ones waiting for their quest to be listed (once a frame, after the story).
void tick(::Ctx* c);
void on_session_change();
void reset();
}  // namespace quests

namespace requests {
// The hook site (pkp_coop.cpp): entry 0x11435c, the bind gate; a member answers as the game does at home.
int on_giver_check(::Ctx* c);
// Midhook 0x621be8 in a general request's completion 0x6218e0: a replay of the step (w21 bit 0) skips its town goal (x0 = 0) in a
// session, so each hand-in counts once, on the talker's process.
int on_general_goal(::Ctx* c);
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

namespace events {
// The messages (pkp_coop_link.cpp): kept until this player can take them.
void on_history_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_lines_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Takes what other members' events left when this player can walk and talks to no one, reads this player's and shares what grew
// (once a frame, after the channel's handlers).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: everything is read and sent again
void on_member_joined();    // another member joined: everything is sent again
void reset();
}  // namespace events
namespace unlocks {
// The message (pkp_coop_link.cpp): kept until this player can take it.
void on_known_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Sets what other members have when this player can walk and talks to no one, reads this player's unlock lists and shares what grew
// (once a frame, after the channel's handlers).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: the lists are read again, all of them sent
void on_member_joined();    // another member joined: all lists are sent again
void reset();
}  // namespace unlocks

namespace purchases {
// The hook site (pkp_coop.cpp): entry 0xe889f8, a purchase's give on the buyer's process; it is logged for the group.
int on_grant(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0xe87260, "this shop row is for offline play only"; in a session no row is.
int on_offline_only(::Ctx* c);
// True while a teammate's purchase is given here, and shortly after a received upgrade (its scripted event may still give items):
// those gives are not shared again (pkp_coop_catchup.cpp).
bool replaying();
// The messages (pkp_coop_link.cpp).
void on_purchase_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // the authority: a member bought something
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);        // the authority: a member asks for what it lacks
void on_log_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);        // a member: purchases of the group's log
// Sends this player's purchases, asks for missing ones and gives another's purchase here at a safe moment (once a frame).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: what it lacks is asked again
void reset();
}  // namespace purchases

namespace discovery {
// The hook sites (pkp_coop.cpp): the scenes the game queues on the finder's process, sent to the group.
int on_dex_scene(::Ctx* c);       // entry 0x3b9060: the "registered in the Pokédex" scene (slot, &actor id)
int on_habitat_scene(::Ctx* c);   // entry 0x45f2a0: the "new habitat" scene (slot, habitat, placed habitat, flag)
// The messages (pkp_coop_link.cpp).
void on_dex_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_dex_scene_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_habitat_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_habitat_scene_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Plays another member's scene or sets its finds at a safe moment, reads this player's lists and shares what grew (once a frame).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: the lists are read again, the whole lists sent
void on_member_joined();    // another member joined: the whole lists are sent again
void reset();
}  // namespace discovery
namespace held {
// The hook site (pkp_coop.cpp): entry 0x313418, the visitor's "requests answered" test, from the event queue's ready test only.
int on_visitor_sync(::Ctx* c);
void reset();
}  // namespace held
namespace linkqueue {
// The hook sites (pkp_coop.cpp).
int on_send_record(::Ctx* c);     // entry 0x2159c0: the talk-end record's send, from the talk-end walk 0x480b60
int on_session_reset(::Ctx* c);   // entry 0x214954: the visitor's request queue reset at session start
// Sends a kept record once the queue is idle and the player walks (once a frame).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace linkqueue

namespace bag {
// The hook site (pkp_coop.cpp): midhook 0x5728b4, the host's side of a join's bag sync (0x5727c0): the found flag is cleared, so the
// joiner keeps its own bag.
int on_join_bag_send(::Ctx* c);
}  // namespace bag

namespace placement {
// The hook site (pkp_coop.cpp): entry 0x13596c, the "object placed" notice, from the placement 0x593120 only. On a member it waits
// until the object is in this process's world grid.
int on_placed_notice(::Ctx* c);
// Raises a waiting notice once its object is in the world and the player can walk, or after a while as the game would (once a frame).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace placement

namespace contents {
// The hook site (pkp_coop.cpp): entry 0x12b544, a content's "play scene" command. A scene not played yet is remembered.
int on_play_scene(::Ctx* c);
// Asks the game again for a remembered scene at a safe moment until it plays (once a frame); on the authority, gives a quest a
// scene it was waiting on that an older save lost.
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace contents

namespace travel {
// The hook site (pkp_coop.cpp): entry 0xbda20, the area move by destination index. In a session it runs only after the group's poll
// passed; a member's never runs (the host's move brings it).
int on_area_move(::Ctx* c);
// Watches the game's poll: a member's area poll that passed is carried out by the host; a held move of the host's runs (once a frame).
void tick(::Ctx* c);
void reset();
}  // namespace travel

namespace talk {
// The hook sites (pkp_coop.cpp).
int on_talk_start(::Ctx* c);   // entry 0x3180e0: the lock is asked; refused, the busy line and no talk
int on_no_talk(::Ctx* c);      // midhook 0x3181cc: no talk started after all
int on_talk_end(::Ctx* c);     // entry 0x47bd00: a slot's event chain ended
int on_talk_kind(::Ctx* c);    // midhook 0x4c06d8: a member's talk takes the host's path in the talk handler
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
int on_reward_item(::Ctx* c);    // entry 0x3bcbc0: the item of a character's reward list
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
// Holds this player's input, with a notice, while fewer members than the group has are in the session and the group's world has not
// started (once a frame).
void tick(bool in_game);
void reset();
}  // namespace gate

namespace trace {
// Logs what changed since the last look (once a frame in a session): quest words, the players' and the characters' places.
void tick(bool in_game);
void reset();
}  // namespace trace

}  // namespace nxrt::games::pkp::coop
