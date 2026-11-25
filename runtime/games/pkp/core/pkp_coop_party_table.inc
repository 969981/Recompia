// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's party answer table (pkp_coop_party_answers.cpp): the places where the game checks ONE player for
// something the party as a whole has, or refuses one player for a reason a session does not have. At each, every process in a
// session gets the party's answer; outside a session the game's own code runs untouched.
//
// What is a row and what is not. A check of what a player HAS (a creature following, an item, something unlocked) is the party's:
// true when it is true of any player. A check of WHERE a player is or what it is doing right now (its position, its distance to
// something, busy, the thing in its hand, "this player pressed / talked") names who is acting and stays that player's: no row. A
// refusal is never widened to the party: it either stays the acting player's (no row) or, when its reason does not exist in a
// session, is switched off for everyone (a PKP_PARTY_OFF row). A plain "am I the visitor / the host" test is a row of
// pkp_coop_role_table.inc, not of this table.
//
// One row per site (pkp_coop_party_answers.cpp sorts them at compile time and refuses a site listed twice). Two kinds:
//   PKP_PARTY_FACT(entry, fact, word, tag, what)        a leaf of one of the game's condition evaluators, replaced whole
//     entry   main offset of the handler's first instruction (an entry hook: the handler does not run in a session)
//     fact    the party fact that answers it (pkp_coop_party.cpp): Follows, FollowerAbility
//   PKP_PARTY_OFF(ret, callee, answer, word, tag, what)  a refusal the session does not have
//     ret     main offset of the instruction after the `bl callee` whose result decides the refusal; the midhook runs there, before
//             that instruction, so x0 is still the callee's answer and whatever reads it next reads `answer`
//     callee  main offset of the function called (checked the first time the row is reached)
//     answer  the value that does not refuse
//   word      the instruction word at the site, checked with every co-op site before the hooks are armed
//   tag       the system, short (shown in the log line the first time the row changes an answer in a session)
//   what      what changes for the players
// Every row needs its `entryhook` / `midhook 0x71<site>` line in recomp/games/pkp/main.sites (the co-op party answer block).
//
// The pkp_coop.cpp site table includes this file too (each row one Site::PartyFact or Site::PartyOff hook).

// ---- a creature follows a player: the party's followers, not only this process's player's ----
// Contents conditions (the story's and the quests' scripts, evaluator 0x12483c; x0 = the condition's words, x8 = the u32 result).
PKP_PARTY_FACT(0x1273a0, Follows, 0xd503237fu, "followers", "a story step that asks whether a creature is following counts the creature when it follows any player (condition 0x87a)")
PKP_PARTY_FACT(0x127480, FollowerAbility, 0xd503237fu, "followers", "a story step that asks for a follower with an ability counts every player's followers (condition 0x87c)")

// ---- an area gate and the followers of the player who presses it (the walk in 0x7b0690, 0x7b09a0..0x7b0a24) ----
// The press stops at the first invited follower the walk picks: online any town creature, offline one that cannot come along
// (0x4b1558, which answers "cannot" for every town creature on a visitor). The host is then asked (event 0xe5f, whose end releases
// its followers and enters); a visitor's followers are released with no word and no passage. A session is always online, so a
// step that needs a creature brought through a gate could not be done, and the creature stopped following at the press.
// In a session the walk picks nobody: the press goes to the gate's entry, where the group is asked (pkp_coop_travel.cpp). The
// group's invited followers leave when the agreed move is made, not at a press (pkp_coop_follow.cpp release_for_move). With
// both rows the question event and the visitor branch are not reached from a press in a session.
PKP_PARTY_OFF(0x7b0a14, 0x257e44, 0, 0x3607fd80u, "area gate", "pressing an area gate no longer releases the creatures following the player (the online rule)")
PKP_PARTY_OFF(0x7b09d4, 0x4b1558, 1, 0x36000280u, "area gate", "a follower never stops the press at an area gate: the group is asked, and followers leave when the group moves")

// ---- moving to another area while a story follower is attached ----
// The game refuses an area gate (0x7b0c50 results 2 / 3, scenes 0xa7f / 0xa80; the same test in the gate object 0x7cc180 and its
// "can start" 0x7ccb50) and a move from the map page (0xfbacd0) while the story's follower list (party manager list 9) holds a
// creature. That list is this process's own and only the authority's ever fills (a member's story follow is made on the authority,
// pkp_coop_follow.cpp), so the refusal bound the host alone while any member's press or map ask moved the whole group, the host
// and its follower included; and the player has no way to end it (the release by hand, 0x2df220, walks the invited followers
// only). In a session the move is the group's: nobody is refused.
PKP_PARTY_OFF(0x7b0cfc, 0x4e7a40, 0, 0x7100041fu, "story follower", "an area gate opens for the host while a story follower is with it, as it does for a teammate")
PKP_PARTY_OFF(0x7cc76c, 0x4e7a40, 0, 0x7100041fu, "story follower", "the same refusal of the gate object's press (first copy)")
PKP_PARTY_OFF(0x7cc890, 0x4e7a40, 0, 0x7100041fu, "story follower", "the same, second copy")
PKP_PARTY_OFF(0x7ccba8, 0x4da0e0, 0, 0x52000008u, "story follower", "the gate object's passage can start while a story follower is with the host")
PKP_PARTY_OFF(0xfbacfc, 0x4da0e0, 0, 0x360000a0u, "story follower", "the map page lets the host travel to another area while a story follower is with it")
