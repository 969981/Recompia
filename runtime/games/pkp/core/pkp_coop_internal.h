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
//   pkp_coop_gimmicks.cpp a gimmick a member was using when it left: freed, so the others can use it again
//   pkp_coop_owner.cpp    a gimmick whose press a story content took instead of its use: given back, so the others can use it
//   pkp_coop_catchup.cpp  what the talker's talk gives the talker alone (a name, items) given to every other member too
//   pkp_coop_gate.cpp     the wait for every member: this player's input is held until the whole group is in the session
//   pkp_coop_lock.cpp     the input lock the catch-up's name prompt and the gate share
//   pkp_coop_trace.cpp    the trace for testers: quest words, players and characters that move, a wall clock, in the log
//   pkp_coop_notice.cpp   lines in the game's own message log: what another member's event unlocked or moved on here, by name
//   pkp_coop_curtain.cpp  the cover over the picture while a member joins or leaves (the PC, transfer, landing and warp)
//   pkp_coop_requests.cpp the creature requests a member took on or finished, listed in every member's requests app
//   pkp_coop_mud.cpp      the cleaning request of a save that cleaned all the sludge before co-op counted it: the count raised by
//                         what is no longer in the stage
//   pkp_coop_outcomes.cpp what a finished request adds to the world (town goal, friendship), applied once for the group
//   pkp_coop_recipes.cpp  the crafting recipes any member knows, learned by every member
//   pkp_coop_unlocks.cpp  the moves and emotes any member learned, the areas visited, buildings built and area gates opened, unlocked
//                         for every member
//   pkp_coop_purchases.cpp what any member buys in a shop, given to every member (and to later joiners) too
//   pkp_coop_purchase_world.cpp the world side of a purchase the game places in the world (the rebuilding kits): which entries, their
//                         stage, and whether the object is there
//   pkp_coop_money.cpp    the Life Coins of the challenges: each member claims its own at the PC, once, into its own coins
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
//   pkp_coop_wirelog.cpp  every message sent, received, passed on or dropped, in the journal (pkp_journal.h area Sync)
//   pkp_coop_digest.cpp   the shared state's digests: the authority's, compared by every member; the entries that differ, logged on both
//   pkp_coop_bag.cpp      the game's own bag writers that fight the records (the join hand-back, stage-load clears and moves, host
//                         saves into its per-user table, a refusal's bag reset), switched off
//   pkp_coop_bags.cpp     every member's bag kept as a record of ours (a file per member key in the co-op save), written into the bag
//                         the game selects and given back at every join; the host's copy of each visitor's bag fed from it
//                         (pkp_coop_bags_store.cpp: the records' files; pkp_coop_bags_credit.cpp: a gain credited at its source)
//   pkp_coop_records.cpp  the authority's story records (effect-command record, town flags and targets, stage flags, hint groups)
//                         written into every member's own save
//   pkp_coop_levels.cpp   the authority's Environment Levels (the 32 area-level bytes), filled into every member's save at the join
//   pkp_coop_role.cpp     the role answers: at the listed permission and display checks (pkp_coop_role_table.inc) a member gets the
//                         host's answer from the game's role getters
//   pkp_coop_party.cpp    the party facts: what the party as a whole has (the creatures following any player)
//   pkp_coop_party_bags.cpp the party facts, the bags: how many of an item each player holds (the authority's sums of the bag
//                         records, sent to every member)
//   pkp_coop_party_answers.cpp the party answers: at the listed checks of one player (pkp_coop_party_table.inc) every process
//                         answers for the party, and a refusal a session has no reason for does not refuse
//   pkp_coop_story_flags.cpp the story flags (contents condition 0x32): set by any player's record, cleared by the authority's, the
//                         authority's in every member's save
//   pkp_coop_friendship.cpp a member's friendship gains with a creature made on the authority (the group's), its records shown to all
//   pkp_coop_challenges.cpp what a member does counts toward the group's challenges (its shop buys)
//   pkp_coop_feed.cpp     what a member's player did to a creature (talked to it), posted to the authority's contents scheduler
//   pkp_coop_behalf.cpp   a record the authority runs for a member's notice, when the member's own copy ran: without the actions
//                         on the acting player (its scene, warp, push)
//   pkp_coop_newcomer.cpp a member's first talk with a new creature (it moves in) made on the authority, as the host's own talk
//   pkp_coop_doors.cpp    the session's doors in the menus: the PC's Link Play screen makes no session call, the pause menu never
//                         sends a teammate home or rejects one; Save and Quit stays
//   pkp_coop_title.cpp    a co-op start goes past the title screen on its own, with no "did not disconnect correctly" message
//   pkp_coop_follow.cpp   a story follower is an escort: it follows the player who asked until the story releases it (the escort
//                         record); a member's party leave takes no other player's follower; the release at a group move
//   pkp_coop_escort_marks.cpp the escort record in the save: the game's own story mark of a character, on every process
//   pkp_coop_savecopy.cpp the one shared save: the authority's save points copied to every member's co-op folder, stamped
//                         (pkp_coop_saveset.cpp: the folder's files on disk)
//   pkp_coop_drop.cpp     a dropped player is waited for: the group holds while it comes back through the normal join
//   pkp_coop_stage.cpp    co-op stage mode: the game's Cloud Island test answers yes at the rows of pkp_coop_stage_table.inc, so
//                         every process uses Cloud Island's record set for those families, kept the group's (seeded, saved, carried)
// The parts run on the game thread (the game's own code at a hook site, or the game-frame tick) unless a comment says otherwise. Each
// part keeps its own state; pkp_coop.cpp resets them all when the feature is removed.
#pragma once
#include <bitset>
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
// The same key as its 16 bytes (the hex pairs in order), as the messages carry it; all 0 before install.
constexpr size_t kMemberKeyBytes = 16;
const uint8_t* member_key_bytes();

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
// Why can_walk is false (its first failing test, for a log line), or nullptr when it is true.
const char* walk_block(bool in_game);
// The stage the player is in (GM+0x18; 0 while the game state is not there).
uint8_t area_stage();
// The area's data is loaded and still: play ran in it (play mode's in_game), or its stage has held for a while and the stage-change
// record is not active. Holds under a game menu or a talk too, where in_game is false.
bool area_settled(bool in_game);
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
void on_music_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
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
// session, so each hand-in counts once, on the talker's process; the talker's own step (w21 0, w20 1) shares its reward.
int on_general_goal(::Ctx* c);
// Midhooks 0x6522ec / 0x652940, the cleaning count's online gate (`tbz w0, #0` after IsOnline in 0x6520e0 / 0x652880): in a session
// the authority counts (w0 = 0), a member keeps the game's skip and takes the authority's count (story record 8).
int on_clean_count(::Ctx* c);
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
// The state byte of request id in this save (1..5; 0: no save), and its row (#1557) as the checker and the completion read it (0: no
// table or row).
uint8_t request_state(uint32_t id);
uint64_t request_row(::Ctx* c, uint32_t id);
bool story_request(uint32_t id);   // also a story step: one completion closes it for the group
bool own_request(uint32_t id);     // bound to one player: never shared
// The authority: node origin handed request id in; it is closed here at the next safe moment, and the close is shared.
void close(uint32_t id, uint8_t origin);
}  // namespace requests

namespace mud {
// The authority, in the cleaning request's stage while that request is taken: scans the stage for the sludge left over a few seconds
// of frames, twice, and raises the cleaning count by what was removed before it counted (once a frame, after the requests).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace mud

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
// The hook site (pkp_coop.cpp): midhook 0x5c05c0, the area gate record's session guard (`tbnz w0, #0` after 0x207b0 in the setter
// 0x5c059c): in a session the write goes through, and on a visitor the copy its getter reads is set too.
int on_area_gate_record(::Ctx* c);
// The message (pkp_coop_link.cpp): kept until this player can take it.
void on_known_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Sets what other members have when this player can walk and talks to no one, reads this player's unlock lists and shares what grew
// (once a frame, after the channel's handlers).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: the lists are read again, all of them sent
void on_member_joined();    // another member joined: all lists are sent again
void reset();
}  // namespace unlocks

namespace tutorials {
// The messages (pkp_coop_link.cpp): tips another member has shown, and the waiting tip another process closed.
void on_known_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_closed_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Closes the waiting tip when it left the stage record here or elsewhere, shares the shown tips and sets the others' (once a frame).
void tick(::Ctx* c, bool in_game);
void on_session_change();
void on_member_joined();
void reset();
}  // namespace tutorials

namespace hints {
void on_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // another member's habitat hints
// Shares the habitat hints that became known here and sets the others' (once a frame).
void tick(::Ctx* c, bool in_game);
void on_session_change();
void on_member_joined();
void reset();
}  // namespace hints

namespace purchases {
// The hook site (pkp_coop.cpp): entry 0xe889f8, a purchase's give on the buyer's process; it is logged for the group.
int on_grant(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0xe87260, "this shop row is for offline play only"; in a session no row is.
int on_offline_only(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0xe8d428, the PC menu's "a visitor may open the shop" test; a member in a session may.
int on_shop_tile(::Ctx* c);
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

// The place class: purchases the game puts in the world next to the buyer, not in the bag (pkp_coop_purchase_world.cpp).
namespace world {
// The item (of a shop entry, with its item category) is placed in the world: the game's own test.
bool is_place_class(::Ctx* c, uint32_t item, uint8_t category);
// The stage the shop row belongs to (the stage its kit is placed in); 0 when the row cannot be read.
uint8_t shop_area(::Ctx* c, uint16_t entry);
// This process's player stands in that stage (and exists).
bool in_stage(uint8_t area);
// The occupied cabinet records of the world; added_since() says whether a record was occupied after the snapshot that was free before.
struct Snapshot {
    bool valid = false;
    std::bitset<0x610> occupied;
};
Snapshot snapshot();
bool added_since(const Snapshot& before);
}  // namespace world
}  // namespace purchases

namespace stock {
// The hook sites (pkp_coop.cpp), on a member in a session off Cloud Island: the group's stock is the authority's.
int on_get(::Ctx* c);       // entry 0x646864 (M, entry): the stock left, from the own record (the authority's numbers)
int on_set(::Ctx* c);       // entry 0x646ae8 (M, entry, n): the own record, and the count taken goes to the authority
int on_restock(::Ctx* c);   // entry 0x632380 (CM): the daily restock does not run (the authority's reaches the member)
// The messages (pkp_coop_link.cpp).
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);     // the authority: counts a member took, an ask
void on_state_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // a member: the authority's shop records
// The authority sends its records when they change; a member asks until they arrive and sends what it took (once a frame).
void tick(::Ctx* c);
void on_session_change();   // this process joined or left: asked and sent again
void on_member_joined();    // another member joined: the records are sent again
void reset();
}  // namespace stock

namespace shoprows {
// The message (pkp_coop_link.cpp): a member, the authority's answer for every shop row.
void on_rows_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// The authority computes its rows over several frames and sends them when they change; a member writes them into its copy of the
// host's shop (once a frame; the authority only in the world).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: sent again, a member's set is dropped
void on_member_joined();    // another member joined: the rows are sent again
// A member: the authority's rows are written into its copy of the shop, so a bent game answer may read it.
bool applied();
void reset();
}  // namespace shoprows

namespace pclock {
// The hook site (pkp_coop.cpp): entry 0x1364f4 (SC, u8), the kind-28 notice of the PC's Shop pane (1) and challenges pane (0).
int on_pane_notice(::Ctx* c);
// The player's press on an object (midhook 0x313eb4, shared with the owner's: x0 the object, w1 the kind): a member keeps the PC's.
void on_press(::Ctx* c);
// Ends a PC menu lock the game's own way cannot end here (once a frame).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace pclock

namespace money {
// The hook sites (pkp_coop.cpp).
int on_pay(::Ctx* c);            // entry 0x637480, the coins of a claimed challenge: on a member only once per challenge, marked paid
int on_claim_visitor(::Ctx* c);  // midhook 0x645d18, the PC's claim after its visitor test: a member's own claim pays here
// The challenges claimed in the authority's save (pkp_coop_link.cpp): members on older builds pay from it; this build ignores it.
void on_claimed_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// The authority sends what it claimed; a member's view of the done challenges follows its own claims (once a frame).
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: the set is sent and learnt again
void on_member_joined();    // another member joined: the whole set is sent again
void reset();
}  // namespace money

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
// A member: a request went to the host and its answer is not taken yet (q[0] != q[1]); false with no world.
bool unanswered();
// A member: a record is kept here, not sent yet (a release, a talk-end); its request is still to come.
bool kept();
// ---- a gimmick's release record (pkp_coop_linkqueue.cpp) ----
// The hook sites (pkp_coop.cpp): midhooks at the `tbz w0` after a close's `bl 0x313418` (site: its main offset), turned true on a
// member in a session; entry 0x214974, the generic request send, whose release records (by return address) are kept when the queue
// would refuse them. Each returns what the site's hook returns.
int on_release_gate(::Ctx* c, uint64_t site);
int on_send_request(::Ctx* c);
// This member took the object (kind, packed cell) again: a release still kept for it is dropped.
void on_take(::Ctx* c, uint16_t kind, uint32_t cell);
}  // namespace linkqueue

namespace bag {
// The hook sites (pkp_coop.cpp); each returns what the site's hook returns.
int on_join_bag_send(::Ctx* c);    // midhook 0x5728b4, the host's side of a join's bag sync: no stored bag is sent
int on_stage_bag_clear(::Ctx* c);  // entry 0x5afee0: the stage load's clear of the online bag, skipped
int on_bag_to_storage(::Ctx* c);   // entry 0x5affe0 / 0x5b0d60 (holder): the move of the bag into the PC, skipped; the share bit cleared
int on_host_bag_store(::Ctx* c);   // entry 0x5b0a60 / 0x5af6a0: the host's moves between its per-user table and a bag, skipped
int on_refusal_reset(::Ctx* c, bool releases_wait);   // midhooks in 0x226280: a member's bag reset to the host's copy, skipped
                                                      // (the record is written back instead, pkp_coop_bags.cpp; not in kind 0xb;
                                                      // kind 0x1e keeps the game's reset, bags::on_stand_trade)
}  // namespace bag

namespace bags {
// Applies this player's record to the bag the game selects, takes the bag's changes into it (or refuses a wipe), sends a member's to
// the authority and writes the records' files (once a frame, after the channel's handlers). reset() writes what changed first.
void tick(::Ctx* c);
void reset();
// Another member joined (its node): the authority answers its first record of the session again.
void on_member_joined(int node);
// A member: the host refused a request that took items out of the bag (pkp_coop_bag.cpp): the record, still the bag from before
// that request, is written back into the bag once the bag / container screen is closed.
void on_request_refused();
// A module that adds to this player's bag (a give of co-op's own) brackets the add: gain_begin right before, gain_end right after, on
// the game thread. The bag's gain in between is credited to the record at once, so no write-back (an apply, a refused request's
// restore) can take it away; a loss in between is not credited (the header of pkp_coop_bags.cpp has the rule).
void gain_begin(::Ctx* c);
void gain_end(::Ctx* c);
// The authority: the records it holds for the players now in the session, its own first (node, the record's sequence number, its
// wire::kBagRecordSlots slot payloads; the pointers hold until the next tick). At most `most`; the count. 0 anywhere else.
struct View {
    int node;
    uint64_t seq;
    const uint64_t* slots;
};
size_t records(View* out, size_t most);
// The hook sites (pkp_coop.cpp).
int on_mirror_fill(::Ctx* c);   // entry 0x575644 (member index), the host at a join: the mirror fed from that member's record
int on_bag_screen(::Ctx* c);    // entry 0xfeee78 (screen), the bag / container screen's frame: an apply waits while it is open
int on_stand_trade(::Ctx* c);   // entry 0x223760 (sender, request), the host's stand trade: the mirror is the bag before the trade
// The messages (pkp_coop_link.cpp).
void on_record_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);    // the authority: a member's record
void on_restore_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // a member: the authority's copy of its record
}  // namespace bags

namespace placement {
// The hook site (pkp_coop.cpp): entry 0x13596c, the "object placed" notice, from the placement 0x593120 only. On a member it waits
// until the object is in this process's world grid.
int on_placed_notice(::Ctx* c);
// Raises a waiting notice once its object is in the world and the player can walk, or after a while as the game would (once a frame).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace placement

namespace scenes {
// Shares the talk-lock keys of the characters this process's running events hold (SceneHolds, on a change; once a frame) and keeps
// the other members' sets: held_elsewhere(key) is true while another member's scene holds that character.
void tick(::Ctx* c);
void on_holds_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_member(int node, bool joined, bool self);
bool held_elsewhere(uint32_t key);
void reset();
}  // namespace scenes

namespace contents {
// The hook site (pkp_coop.cpp): entry 0x12b544, a content's "play scene" command. A scene not played yet is remembered.
int on_play_scene(::Ctx* c);
// The hook sites (pkp_coop.cpp): midhooks 0x4774e8 (the event setup's "every character ready" test) and 0x47bfe4 (the start's IsMine
// of one character). A remembered scene refused for a long time by a character another player holds takes its characters.
int on_scene_take(::Ctx* c);
int on_scene_start(::Ctx* c);
// Asks the game again for a remembered scene at a safe moment until it plays (once a frame); on the authority, gives a quest a
// scene it was waiting on that an older save lost.
void tick(::Ctx* c, bool in_game);
// The authority's contents got an "object used" notice (0x135d80: object kind, use category), its own player's or a member's
// (pkp_coop_feed.cpp): a lost scene whose record waited on that use is looked at once, after the game's own records had their chance.
void on_object_used(uint16_t object, uint16_t use);
void reset();
}  // namespace contents

namespace travel {
// The hook sites (pkp_coop.cpp). Entry 0xbda20, the area move by destination index: in a session it runs only after the group's poll
// passed; a member's never runs (the host's move brings it). Entry 0x7b0d80, an area gate's entry (its passage): in a session it
// runs only after the group's poll for the gate's area passed, so the poll comes before anything fades.
int on_area_move(::Ctx* c);
int on_gate_entry(::Ctx* c);
// The message (pkp_coop_link.cpp): the host opens the area poll a member asked for, in the member's name.
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Watches the game's poll: a member's area poll that passed is carried out by the host; a held move of the host's runs (once a frame).
void tick(::Ctx* c);
void reset();
// The group is on its way to another area: from the moment its followers were released for an agreed move until the stage change
// has run or the move was called off, and whenever the game's own stage change runs.
bool moving();
}  // namespace travel

namespace talk {
// The hook sites (pkp_coop.cpp).
int on_talk_start(::Ctx* c);   // entry 0x3180e0: the lock is asked; refused, the busy line and no talk
int on_no_talk(::Ctx* c);      // midhook 0x3181cc: no talk started after all
int on_talk_end(::Ctx* c);     // entry 0x47bd00: a slot's event chain ended
int on_talk_kind(::Ctx* c);    // midhook 0x4c06d8: a member's talk takes the host's path in the talk handler
int on_talk_own(::Ctx* c);     // midhook 0x4c0574: a member is the own player of a creature living in the group's town
int on_cond_parent(::Ctx* c);   // midhook 0x4d35f8: talk condition 0x52 answers a member as the host
int on_cond_visitor(::Ctx* c);  // entry 0x4d0620: talk condition 0x25 (IsVisitor) is false for a member
int on_cond_online(::Ctx* c);   // entry 0x4d04fc: talk condition 0x22 (IsOnline) is false in the session for a character with no parents
// This process's player holds a character's talk and the talk runs.
bool talking();
// The lock key of the character whose u16 actor id is at id (guest), 0 for none; another member holds that character's talk (a
// grant or a talk, until its event chain ends), as this process's table or copy says.
uint32_t character_key(::Ctx* c, uint64_t id);
bool held_elsewhere(uint32_t key);
// The messages and member events (pkp_coop_link.cpp).
void on_ask(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_done(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_holder(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_member(::Ctx* c, int node, bool joined, bool authority, bool self);
// The lease of an unused grant and the watchdog of a talk with nothing running (once a frame).
void tick();
void reset();
}  // namespace talk

namespace gimmicks {
// A member left (pkp_coop_link.cpp): the next while, gimmicks still held by a member index with no character are freed.
void on_member(int node, bool joined, bool self);
// The sweep after a leave (once a frame; once a second while due).
void tick(::Ctx* c, bool in_game);
void reset();
// The hook sites (pkp_coop.cpp): entry of a multi-user object class's "member left" step vt+0x228 (site: its main offset;
// x0 the object, w1 the member index). On the authority, for a member that was using the object, the rest of the game's own
// end-of-use sequence runs first (the character the member had there is let go); returns 0, the game's step runs on.
int on_shared_leave(::Ctx* c, uint64_t site);
}  // namespace gimmicks

namespace catchup {
// The hook sites (pkp_coop.cpp).
int on_name_entry(::Ctx* c);     // entry 0x43d3e0: script command 0x44, the name entry
int on_script_items(::Ctx* c);   // entry 0x12c734: script command 0xbea, items given
int on_reward_item(::Ctx* c);    // entry 0x3bcbc0: the item of a character's reward list
// A give that ran on this process for its own player: every other member gets it too (Items; nothing outside a session).
void share_give(uint16_t item, uint32_t count);
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
enum class Holder : uint8_t { Name, Gate, Drop };
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

namespace wirelog {
// The link's calls (pkp_coop_link.cpp): one journal line per message, equal periodic payloads counted instead of written.
const char* type_name(uint32_t type);
struct Pending {   // a send, taken before the payload moves into the channel
    uint32_t type = 0;
    int node = 0;
    bool write = false;
    uint32_t same = 0;
    std::string payload;
};
Pending sending(uint32_t type, int node, const uint8_t* p, size_t n);
void sent(const Pending& d, bool queued);
void sent_all(uint32_t type, const uint8_t* p, size_t n, int count);
void received(uint32_t type, int from, uint8_t origin, const uint8_t* p, size_t n, const char* outcome);
void forwarded(uint32_t type, uint8_t origin, const uint8_t* p, size_t n, const char* to);   // to: " n1 n3", or ""
void dropped(uint32_t type, int from, const uint8_t* p, size_t n, const char* why);
void install();   // the journal's "who" (node and role)
void reset();
}  // namespace wirelog

namespace digest {
// One shared domain's entries as this process has them: (id, value) of every entry set, ids ascending.
struct Entry {
    uint32_t id, value;
};
using Entries = std::vector<Entry>;
// The domains' readers (each in its part's file); 0 / false when the save is not there yet.
bool quest_states(Entries& out);   // pkp_coop_story.cpp: quest id -> state word (low byte) past 0
bool abilities(Entries& out);      // pkp_coop_story.cpp: ability id -> 1
bool dex(Entries& out);            // pkp_coop_discovery.cpp: dex number -> entry bits
bool habitats(Entries& out);       // pkp_coop_discovery.cpp: habitat id -> bits
bool recipes(Entries& out);        // pkp_coop_recipes.cpp: recipe id -> 1
bool unlocks(Entries& out);        // pkp_coop_unlocks.cpp: (list << 16) | entry id -> 1
bool scenes(Entries& out);         // pkp_coop_events.cpp: played scene id -> 1
bool area_levels(Entries& out);    // pkp_coop_digest.cpp: stage -> the level the game reads here
bool gates(Entries& out);          // pkp_coop_digest.cpp: gate id -> the byte the gate getter reads here (non-zero)
bool trainer_level(Entries& out);  // pkp_coop_digest.cpp: 0 -> m_trainerLevel
bool open_areas(Entries& out);     // pkp_coop_records.cpp: area 1..31 -> the open byte the area gate reads here (non-zero)
bool story_flags(Entries& out);    // pkp_coop_digest.cpp: flag 1..20 -> 1 (m_storyCtrlFlag)
bool homes(Entries& out);          // pkp_coop_digest.cpp: character index -> (kind << 8) | stage of its home, as read here
// The authority sends its digest every few seconds; a member compares (once a frame, in a session).
void tick(::Ctx* c, bool in_game);
void on_digest_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_full_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_report_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_session_change();
void reset();
}  // namespace digest

// ---- the ability check (pkp_coop_story.cpp) ----
namespace story {
// The hook site (pkp_coop.cpp): midhook 0x1140e8 in the ability check 0x113f10, its "a visitor reads the host's record" branch; a
// member in the session takes the own-bits branch instead.
int on_ability_check(::Ctx* c);
}  // namespace story

// ---- the story records (pkp_coop_records.cpp): the authority's story records, written into every member's save ----
namespace records {
// The message (pkp_coop_link.cpp): a member keeps the authority's copy of one record.
void on_record_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// The authority sends the records that changed; a member writes the authority's copies into its save at a safe moment (once a frame).
void tick(bool in_game);
void on_session_change();   // this process joined or left: copies forgotten, all records sent again
void on_member_joined();    // another member joined: all records sent again
// A member: this record's copy of the authority is in the save (for this area, a per-area record), so a bent game answer may read it.
bool applied(uint8_t id);
void reset();
}  // namespace records
// ---- the Environment Levels (pkp_coop_levels.cpp): the authority's area levels, filled into every member's save ----
namespace levels {
// The message (pkp_coop_link.cpp): a member keeps the authority's 32 levels.
void on_levels_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// The authority sends its levels at a join, on a change and every so often; a member fills the mirror bytes that are still 0.
void tick();
// A member: the fill, at once; how many mirror bytes it wrote.
size_t fill_now();
void on_session_change();   // this process joined or left: the copy forgotten, levels sent again
void on_member_joined();    // another member joined: the levels are sent again
void reset();
}  // namespace levels
// ---- the story flags (pkp_coop_story_flags.cpp): the 20 flags the content records set and test are the group's ----
namespace storyflags {
// The hook site (pkp_coop.cpp): entry 0x12c46c, contents action 0xbbd (x0 = its words). On a member that has the authority's
// flags a set runs and is asked of the authority (returns 0), a clear does not run (returns 1). 0 everywhere else.
int on_flag_action(::Ctx* c);
// The messages (pkp_coop_link.cpp): a member keeps the authority's flags; the authority sets the flag a member's record set.
void on_flags_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_set_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// The authority sends its flags at a join, on a change and every so often; a member writes them into its save (once a frame).
void tick();
void on_session_change();   // this process joined or left: the copy forgotten, the flags sent again
void on_member_joined();    // another member joined: the flags are sent again
void reset();
}  // namespace storyflags
// ---- the story talks (pkp_coop_skits.cpp): each character's story talk, its bubble and the objective that follows it ----
namespace skits {
// The message (pkp_coop_link.cpp): a member keeps the authority's story talks.
void on_skits_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// The authority sends the story talks that changed; a member sets the authority's through the game's setter at a safe moment.
void tick(::Ctx* c, bool in_game);
void on_session_change();   // this process joined or left: copies forgotten, all sent again
void on_member_joined();    // another member joined: all sent again
void reset();
}  // namespace skits
// ---- the role answers (pkp_coop_role.cpp) ----
namespace answers {
// The hook sites (pkp_coop.cpp): one midhook per row of pkp_coop_role_table.inc, after a `bl` to a role getter or at a test of the
// session bits (site: its main offset). A member in a session gets the host's answer (x0, or the tested register); returns 0 (the
// game's code runs on).
int on_answer(::Ctx* c, uint64_t site);
void tick();    // once a frame: the marks of the rows logged this session clear when the session closes
void reset();
}  // namespace answers
// ---- the party answers (pkp_coop_party.cpp the facts, pkp_coop_party_answers.cpp the hooks): what the party has, where the game
// checks one player ----
namespace party {
// The facts (game thread, inside a hook). A creature follows some player of the session, as far as this process knows (its own
// player's followers and every player's invited ones): a town creature whose data id is `data`, or any follower when `data` is
// no data id (the game's condition 0x87a); a town creature with `ability` in one of its two ability slots (condition 0x87c).
// *own: it is one of this process's own player's followers (written only when true is returned).
bool follows(::Ctx* c, uint32_t data, bool* own);
bool follower_ability(::Ctx* c, uint32_t ability, bool* own);
// The creature whose component is `comp` (a resident's part + 0x2ca0) is the group's own, on a member: this process is a member in
// the session, outside Cloud Island, and the creature's record has parents. False on the authority and outside a session, where
// the game's own test already is the answer.
bool owns(::Ctx* c, uint64_t comp);
// The actor ids of the creatures the session's players invited to follow them (every slot's list), at most `most`; the count.
constexpr size_t kInvitedMost = 20;   // 4 lists of 5
size_t invited(uint16_t* ids, size_t most);
// The actor ids of the creatures the session player of `slot` invited to follow it (its list 0..3), at most `most`; the count.
size_t followers_of(uint8_t slot, uint16_t* ids, size_t most);
// The bags (pkp_coop_party_bags.cpp). The largest count of item `id` any other player of the session holds, as far as this
// process knows (0: none; *node: that player's, written when the count is above 0); and whether one of them holds exactly
// `exactly` of it (*node: that player's).
uint32_t teammates_most(uint32_t id, uint8_t* node);
bool teammate_holds(uint32_t id, uint32_t exactly, uint8_t* node);
// The message (pkp_coop_link.cpp): the authority's sums of every player's bag. A member joined or left (the authority).
void on_bags_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_member_change(int node, bool joined);
void bags_tick();    // once a frame: the authority follows the bag records and sends the sums when they changed
void bags_reset();
// The hook sites (pkp_coop.cpp), one per row of pkp_coop_party_table.inc (site: its main offset). A fact row's entry hook: in a
// session the party's answer is written and the handler does not run (returns 1). An off row's midhook after a `bl`: in a session
// x0 becomes the answer that does not refuse (returns 0). A count row's midhook after the `bl` that counts an item in this
// player's bag: in a session x0 becomes the count the party's answer is made from (returns 0). All return 0 untouched outside a
// session.
// A predicate row's entry hook (x0 = the function's argument, x30 = its caller): on a member the party's answer 1 is returned and
// the function does not run (returns 1), except for the callers the row keeps the game's answer for.
int on_fact(::Ctx* c, uint64_t site);
int on_off(::Ctx* c, uint64_t site);
int on_count(::Ctx* c, uint64_t site);
int on_pred(::Ctx* c, uint64_t site);
// A leader row's midhook at a compare of a creature's leader with this player: in a session a leader that is any player of the
// session passes (returns 0).
int on_leader(::Ctx* c, uint64_t site);
void tick();    // once a frame: the marks of the rows logged this session clear when the session closes
void reset();
}  // namespace party
// ---- the creature jobs (pkp_coop_jobs.cpp) ----
namespace jobs {
// Midhook 0x3b39a8 in the item-making tick: on the authority, a job on a creature a member holds is ticked too.
int on_job_tick(::Ctx* c);
void reset();
}  // namespace jobs
// ---- friendship (pkp_coop_friendship.cpp): a member's friendship gains and flags made on the authority; its records shown to all ----
namespace friendship {
// The hook sites (pkp_coop.cpp).
int on_add_points(::Ctx* c);   // entry 0x3a89fc: a member's gain is sent to the authority
int on_flag_17a(::Ctx* c);     // midhook 0x4ab358: a member's friend record flag +0x17a is sent to the authority
int on_name_tag(::Ctx* c);     // midhook 0x3b7cd0: a member's name-tag friendship test reads its copy of the host's records
int on_gift_apply(::Ctx* c);   // midhook 0x21c46c: the authority's apply of a member's talk gift makes its friendship
// The messages (pkp_coop_link.cpp).
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);     // the authority: a member's gain or flag
void on_state_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // a member: the authority's friend records
// Another member joined (its node): the authority sends it every friend record it has.
void on_member_joined(int node);
// A member sends what waits; the authority sends the friend records that changed (once a frame).
void tick(::Ctx* c);
void reset();
}  // namespace friendship
// ---- pkp_coop_challenges.cpp ----
namespace challenges {
// The hook site (pkp_coop.cpp): midhook 0x643938, the "buy items" challenge count after its visitor test: a member's buy counts.
int on_buy_visitor(::Ctx* c);
}  // namespace challenges
namespace feed {
// The hook site (pkp_coop.cpp): entry 0x1355ec, the contents scheduler's "talked to" notice. On a member in the session the talk is
// queued for the authority; the game's own code runs everywhere (returns 0).
int on_talked_to(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0x136548, the "fed" notice of a field feed (from 0x597820 only). On a member in the session the
// feed is queued for the authority, which makes the creature eat it there; the game's own code runs everywhere (returns 0).
int on_fed(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0x1353a0, the contents notifier. From one of the three "event ended" notifiers (0x1356b8,
// 0x135720, 0x135788): on a member in the session the event's end is queued for the authority; the authority remembers its own
// posts, so a member's of the same event is not posted twice. From one of the two "object used" notifiers (0x135d80, 0x135cf4): on a
// member in the session the notice is queued for the authority. The game's own code runs everywhere (returns 0).
int on_notice(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0x1350c0, a record put into the scheduler's run queue. On a member, called from the notifier
// of a notice just queued for the authority: that event is marked as taken by a record here. Returns 0.
int on_record_queued(::Ctx* c);
// The message (pkp_coop_link.cpp): a member's creature event, posted on the authority at a safe moment.
void on_event_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Sends a member's events and posts the authority's received ones (once a frame, after the channel's handlers).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace feed
// ---- the authority's run of a record for a member whose own copy ran (pkp_coop_behalf.cpp) ----
namespace behalf {
// The feed, around its post of a member's notice (taken: a record on the member's process took it): the records the notifier
// queues between the two were queued by that member's notice, and with taken they are run for that member.
void begin(uint8_t origin, bool taken);
void end();
// The record whose actions run now: its content entry and id, and the member whose relayed notice queued it (-1: none did).
struct Run {
    uint32_t entry = 0, id = 0;
    int origin = -1;
};
bool running(Run* out);
// The hook sites (pkp_coop.cpp), each returns 0 but the last. Entry 0x1350c0: a record queued by the notifier between begin and
// end is remembered. Entry 0x131200: the record whose actions run from now. Entries 0x12b544 (before the contents scenes' own
// look), 0x12af00, 0x12b108, 0x12dbb8, the actions on the acting player (play scene, the two warps, the push): on the authority,
// while a remembered record runs, the action does not run (returns 1).
int on_record_queued(::Ctx* c);
int on_record_run(::Ctx* c);
int on_actor_action(::Ctx* c);
void tick();    // once a frame: a remembered record that is no longer queued is forgotten
void reset();
}  // namespace behalf
// ---- a gimmick press a story content took instead of the use (pkp_coop_owner.cpp) ----
namespace owner {
// The hook sites (pkp_coop.cpp): midhooks at the `tbz w0` after a use posts its press to the contents (0x765b54) in five gimmick
// classes (site: its main offset). In a session, a press a content took gives the object back as the class's failed event start
// does; returns 0.
int on_use_refused(::Ctx* c, uint64_t site);
// The hook site (pkp_coop.cpp): midhook 0x539b44 in the member's grant reply 0x539620, the `tbz w0` after the grant's data apply
// (vt+0x2f8). In a session, on a member, a grant after which no use runs (the apply failed, or the class runs none from a grant)
// gives the object back; returns 0.
int on_grant_no_use(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0x7e67c0, class 0x326dc98's close. On the authority in a session, the host's own use is
// given back (nothing in the class frees it); returns 0.
int on_host_use_end(::Ctx* c);
void reset();
// ---- the release a member's queue held too long, the tester log (pkp_coop_owner.cpp) ----
// A member: gives the object (kind, packed cell as the release record and SyncGimmickOwnership carry it) back with the forced Sync.
void give_back_cell(::Ctx* c, uint16_t kind, uint32_t cell);
// The hook sites (pkp_coop.cpp), logging only (each returns 0): entry 0x538f60, the ownership apply; midhook 0x313eb4, the player's
// press; entry 0xa8fba0, the message log's post (the busy line); entry 0x539620, a member's grant reply (a refused take).
int on_owner_apply(::Ctx* c);
int on_press(::Ctx* c);
int on_notice(::Ctx* c);
int on_grant_reply(::Ctx* c);
}  // namespace owner
// ---- a newly met creature moving in (pkp_coop_newcomer.cpp) ----
namespace newcomer {
// The hook site (pkp_coop.cpp): midhook 0x48235c, the talk end step's `bl 0x3b9674` (the befriend). On a member in the session, a
// creature with no parents yet is queued for the authority; the game's own call runs on (returns 0).
int on_befriend(::Ctx* c);
// The message (pkp_coop_link.cpp): a member's new creature, befriended on the authority at a safe moment.
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// Sends a member's new creatures; the authority makes the waiting ones (once a frame, after the channel's handlers).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace newcomer
// ---- the session's doors in the menus (pkp_coop_doors.cpp) ----
namespace doors {
// The hook site (pkp_coop.cpp): entry 0xf282a8, the PC's Link Play screen's state entry. In a session, a state that makes a session call
// is refused (the screen goes back to its first state) and the step does not run (returns 1); otherwise returns 0.
int on_link_menu(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0xfb9ea0, the pause menu's state entry. In a session, the host's "send this member home" and
// the rejection of a member are refused (the menu goes back to its first state; returns 1); otherwise returns 0.
int on_pause_menu(::Ctx* c);
void reset();
}  // namespace doors
// ---- a co-op start past the title (pkp_coop_title.cpp) ----
namespace title {
// The hook site (pkp_coop.cpp): entry 0xa88214, the title's result getter (only caller the save load sequence's state 7). Clears the
// save's unclean-exit mark and, while the title waits for a press, answers it with "continue"; the getter runs on (returns 0).
int on_past_title(::Ctx* c);
void reset();
}  // namespace title
// ---- a member's take-out of its Ordered Items and Gifts (pkp_coop_takeout.cpp) ----
namespace takeout {
// The hook site (pkp_coop.cpp): midhook 0x2eea74 in the shared move 0x2ee774, the `and w3, w0, #1` after its IsVisitor call that picks
// the store the moved item is removed from. Called by the take-out screen for kinds 0x14 / 0x1a, a visitor's answer becomes 0 (the
// own store, the one listed); returns 0.
int on_remove_store(::Ctx* c);
// Asks the game's autosave to save after each such take until a save written after it holds the emptied box (once a frame).
void tick(::Ctx* c);
void reset();
// Another part changed this member's own store outside the take-out screen (pkp_coop_deposit.cpp): a save is owed as for a take.
void owe_save();
}  // namespace takeout
// ---- a member's deposit into the story's materials drop (pkp_coop_deposit.cpp) ----
namespace deposit {
// pkp_coop_owner.cpp, at a member's grant of the drop (class 0x326dc98, x23 = g, owned here): true when this module runs the use
// (at the next frame); false: the grant is given back as before (no session, or an ask of this member's is still waiting).
bool take_grant(::Ctx* c, uint64_t g);
// The hook sites (pkp_coop.cpp). Entry 0x5b1c84, the container slice: on a member, a type 0x11 slice asked with the visitor's temp
// list resolves to the own store. Midhook 0x7e693c in the drop's close, the `tbz w0` after the screen's confirm flag: a member's
// confirm does not go on (x0 = 0) and is asked of the authority. Each returns 0.
int on_store(::Ctx* c);
int on_confirm(::Ctx* c);
// The messages (pkp_coop_link.cpp): the authority takes a member's ask; a member takes the authority's stage and answers.
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_state_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// A member runs its granted use, sends its ask and plays a paid stage's scenes; the authority answers the asks and sends its drop's
// stage (once a frame).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace deposit
// ---- a member's building kit start, applied on the authority (pkp_coop_buildstart.cpp) ----
namespace buildstart {
// The hook sites (pkp_coop.cpp; site: its main offset): entry 0x8603dc (the kit's apply of request kind 0x1a), entry 0x86bc20 (its
// requirement check: passed over for a member's start, returns 1), midhooks 0x86322c (the commit), 0x2227b0 (the apply's end), and in
// 0x221d00 0x222778, 0x221d84, 0x22292c, 0x2229a8 (the gates, journalled). Every other site returns 0.
int on_site(::Ctx* c, uint64_t site);
void reset();
}  // namespace buildstart
// ---- a story follow on a member (pkp_coop_follow.cpp) ----
namespace follow {
// The hook site (pkp_coop.cpp): entry 0x2a1ed4, the contents' "follow this player" attach (command 0xdac mode 3, through 0x117b18).
// In a session it does not run (returns 1): on the authority the creature becomes an escort of its own player, on a member the ask
// is sent to the authority. 0 outside a session.
int on_story_follow(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0x659588, the leave every follower's leave ends in (x3 = &the creature's actor id). In a
// session a creature in the escort record does not leave (returns 1); this module's own leaves and every other creature's run.
int on_leave(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0x659bf8, the story's own leave (x2 = &the creature's actor id). In a session, for an escort:
// on the authority it is released; on the member that leads it the release is sent to the authority. Returns 0.
int on_story_leave(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0x2a1f44, the scripted release (x0 = &the creature's actor id), which reaches 0x659bf8 only
// for a creature with a leader. In a session an escort that follows nobody here is released as on_story_leave does. Returns 0.
int on_story_release(::Ctx* c);
// The hook site (pkp_coop.cpp): entry 0x5519bc, the host's RequestPartyNPC handler. On the authority in the session a member's leave
// of a creature another player leads is refused as handled (x0 = 0, returns 1); 0 elsewhere (the game's handler runs).
int on_party_request(::Ctx* c);
// The messages (pkp_coop_link.cpp): a member's story follow (the creature becomes the asker's escort), the leading member's
// release, and on a member the authority's escort record.
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_release_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
void on_record_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// A member joined or left (the authority): the record is sent again; a leader that left hands its escorts to the authority's player.
void on_member_change(int node, bool joined);
// The authority, when the group's agreed move to another area is made (pkp_coop_travel.cpp): every player's invited followers
// leave their leaders through the game's own leave; an escort stays in the record. Nothing on a member or outside a session.
void release_for_move(::Ctx* c);
// The invited follower (guest &u16 actor id at npc, in slot's list) is taken as the authority's party tick takes a yes from the
// creature: the leader id is written and the slot's list is raised to the members. False when the invite did not turn its follow on.
bool accept(::Ctx* c, uint64_t npc, uint16_t leader, uint8_t slot);
// The actor id of the session player of `slot` here (0: none); the creature of data id `data` is in the escort record.
uint16_t player_actor(uint8_t slot);
bool is_escort(uint16_t data);
// A member sends its asks and releases and marks the escorts of the record it received; the authority finds the saved record's
// creatures, attaches the escorts that do not follow their leader and sends the record (once a frame, after the channel's
// handlers).
void tick(::Ctx* c, bool in_game);
void reset();
// The escort record in the save (pkp_coop_escort_marks.cpp): the game's own story mark of a character, named by the creature's
// save index (the u32 of its party block; 0 and anything out of range name no row).
namespace marks {
// The mark in this player's own save.
bool marked(uint32_t index);
// Sets or clears the mark: in this player's own save, or with `visiting` in the copy of the host's rows a visiting game reads.
// False when there is no such row to write.
bool put(uint32_t index, bool on, bool visiting);
}  // namespace marks
}  // namespace follow
// ---- the followers who can come cross areas with the group (pkp_coop_carry.cpp) ----
namespace carry {
// The authority, right before the group's move releases its followers (follow::release_for_move): the record is taken now.
void before_move(::Ctx* c);
// The authority records every slot's invited followers who can come while the group walks in an area, and attaches them to their
// leaders once the group walks in the next one (once a frame, after the follow module's tick). Nothing on a member.
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace carry
// ---- the one shared save (pkp_coop_savecopy.cpp) ----
namespace savecopy {
// At the feature's install, before the game opens its save: a received copy left half moved is completed; this PC's copy is read.
void install();
// The messages (pkp_coop_link.cpp).
void on_have_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);    // the authority: a member's copy
void on_offer_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // a member: the authority's copy, announced
void on_chunk_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // a member: a part of one of its files
// A member event (pkp_coop_link.cpp on_member): self, this process joined or left a session.
void on_member(int node, bool joined, bool self);
// The authority finds its save points and sends copies; a member says hello and takes a received copy (once a frame).
void tick(::Ctx* c);
void reset();
}  // namespace savecopy
// ---- a creature's talk record flag a member's talk arms (pkp_coop_talkrec.cpp) ----
namespace talkrec {
// The hook site (pkp_coop.cpp): midhook 0x4ae7e8 in script callback slot 195 0x4ae68c, the `bl 0x77ecb0` after its store of the talk
// record's +0x10 outside Cloud Island. On a member in the session the set is queued for the authority; returns 0.
int on_armed(::Ctx* c);
// The message (pkp_coop_link.cpp): the authority sets that flag in its own talk record.
void on_armed_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
// A member sends what waits (once a frame).
void tick();
void reset();
}  // namespace talkrec
// ---- co-op stage mode (pkp_coop_stage.cpp): in a session every process uses Cloud Island's record set for the families of
// pkp_coop_stage_table.inc, as Cloud Island does for host and visitor alike ----
namespace stage {
// One per PKP_STAGE_FAMILY of the table, in its order (pkp_coop_stage.cpp checks).
enum class Family : uint8_t { Talk, Count };
// The hook sites (pkp_coop.cpp), each returns 0 (the game's code runs on). A row (midhook after a `bl 0x77ecb0`, site its main
// offset): while the family's mode is on, x0 = 1 (Cloud Island); a reset row also clears that record's fields in the Cloud set.
int on_answer(::Ctx* c, uint64_t site);
// Entry 0x5aa8b8 (the pre-save fixups; game thread or the TaskMgr thread) and entry 0x241324 (the save request), site the entry's
// main offset: the authority copies the group's fields Cloud -> own, so the CMNDAT it writes holds them.
int on_save_point(::Ctx* c, uint64_t site);
// Entry 0x5a8748 (a stage data load posted): the group's fields are set aside before a visited area's STGDAT replaces the Cloud
// set. Entry 0x5af540 (the post-STGDAT-load fix-up): they are written back.
int on_load_job(::Ctx* c);
int on_load_fixup(::Ctx* c);
// A member event (pkp_coop_link.cpp on_member): self, this process joined or left the session's channel.
void on_member(int node, bool joined, bool self);
// The messages (pkp_coop_link.cpp).
void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);     // the authority: a member asks for a family's fields
void on_table_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // a member: the authority's fields of a family
// Seeds and ends each family's mode, asks for the authority's fields, logs the answer counts (once a frame).
void tick(bool in_game);
void reset();
// The family's mode is on here: in the session, this process's Cloud set seeded, no area load in flight. Any thread.
bool on(Family f);
// Save root + : the record set the authority's game uses for the family (Cloud while its mode is on, else its own set).
uint64_t authority_set(Family f);
}  // namespace stage
// ---- the Star Piece swap in a member's talk (pkp_coop_talk.cpp) ----
namespace talk {
// The hook site (pkp_coop.cpp): midhook 0x4a1adc in script callback slot 119 0x4a17fc. A member's parent term is the group's record of
// the creature (its copy of the host's has parents), as for the host; returns 0.
int on_swap_parent(::Ctx* c);
}  // namespace talk
// ---- a dropped player is waited for (pkp_coop_drop.cpp) ----
namespace drop {
// The channel's member events (pkp_coop_link.cpp): the authority tells a member's drop from its leave.
void on_member(int node, bool joined, bool self);
// The messages (pkp_coop_link.cpp).
void on_here_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);      // the authority: a member's key, it is in the world
void on_leaving_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // the authority: a member's own leave is filed
void on_wait_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);      // a member: who the authority waits for
void on_closing_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r);   // a member: the host closes the session
// A member whose session ended and whose game is about to close: the session part starts no search then.
bool closing();
// The session part's search finished (found: there is a session to join). A member that lost its session and finds none closes.
void on_search(bool found);
// The hook sites (pkp_coop.cpp): the entries of the trip filers (site: its main offset). A member's filed leave and the host's
// close are announced (returns 0); the kick's filing on a member is refused (returns 1).
int on_trip_file(::Ctx* c, uint64_t site);
// Entry 0x231890, the simulation step: skipped (returns 1, x0 = 1) while the wait pauses the world on this process.
int on_sim_step(::Ctx* c);
// Entry 0x547278, the online error display: error 3103 held back in a co-op session and its rejoin window (returns 1), anything else shown.
int on_error(::Ctx* c);
// Announces this member to the authority; the authority's waits, countdown and their ends; this player's hold while anyone is
// awaited (once a frame, after the channel's handlers).
void tick(::Ctx* c, bool in_game);
void reset();
}  // namespace drop

namespace quality {
// Entry 0x231890, the simulation step (pkp_coop.cpp): logs a change of the game's link-quality level (never handled: returns 0).
int on_sim_step(::Ctx* c);
void reset();
}  // namespace quality

}  // namespace nxrt::games::pkp::coop
