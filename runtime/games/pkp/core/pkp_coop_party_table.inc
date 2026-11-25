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
// One row per site (pkp_coop_party_answers.cpp sorts them at compile time and refuses a site listed twice). The kinds:
//   PKP_PARTY_FACT(entry, fact, word, tag, what)        a leaf of one of the game's condition evaluators, replaced whole
//     entry   main offset of the handler's first instruction (an entry hook: the handler does not run in a session)
//     fact    the party fact that answers it (pkp_coop_party.cpp): Follows, FollowerAbility
//   PKP_PARTY_OFF(ret, callee, answer, word, tag, what)  a refusal the session does not have
//     ret     main offset of the instruction after the `bl callee` whose result decides the refusal; the midhook runs there, before
//             that instruction, so x0 is still the callee's answer and whatever reads it next reads `answer`
//     callee  main offset of the function called (checked the first time the row is reached)
//     answer  the value that does not refuse
//   PKP_PARTY_COUNT(ret, callee, item_sp, n_reg, op_reg, word, tag, what)  a count of an item in this player's bag
//     ret     main offset of the instruction after the `bl callee` that counted; the midhook runs there, x0 the count
//     item_sp the handler's stack offset of the item word (its low 13 bits the id)
//     n_reg   the register that holds the wanted number
//     op_reg  the register that holds the compare (0 more than, 1 at least, 2 exactly, 3 at most, 4 fewer), or 0xff where the
//             site only compares "at least"
//     For a check that takes nothing. A check whose pass is followed by a take of the item from the checked bag gets a row only
//     where the take is known to do no harm when that bag has none (the request's give below); a give in a talk gets none.
//   PKP_PARTY_COUNT_OF(ret, callee, item_sp, kind_reg, kinds, word, tag, what)  the same count at a site several kinds of check
//     share, answered for some of them only ("at least" alone)
//     kind_reg  the register that holds the kind of the check that reached the site (0..63)
//     kinds     a bit per kind the row answers; every other kind keeps this player's own count
//   PKP_PARTY_PRED(entry, fact, word, tag, what)        a predicate function about one creature and this player, answered whole
//     entry   main offset of the function's first instruction (an entry hook: x0 its argument, x30 its caller)
//     fact    the party fact that answers it (pkp_coop_party.cpp): Owns
//     Members only: the authority's own answer already is the group's. The callers that keep the game's answer (by their return
//     address) are listed with the fact in pkp_coop_party_answers.cpp.
//   word      the instruction word at the site, checked with every co-op site before the hooks are armed
//   tag       the system, short (shown in the log line the first time the row changes an answer in a session)
//   what      what changes for the players
// Every row needs its `entryhook` / `midhook 0x71<site>` line in recomp/games/pkp/main.sites (the co-op party answer block).
//
// The pkp_coop.cpp site table includes this file too (each row one Site::PartyFact, Site::PartyOff, Site::PartyCount or
// Site::PartyPred hook).

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
// creature. That list is this process's own and no other process sees it, so the refusal bound the host alone while any member's
// press or map ask moved the whole group, the host and its follower included; and the player has no way to end it (the release
// by hand, 0x2df220, walks the invited followers only). In a session the move is the group's: nobody is refused. A session's
// story follower is an escort and is not in that list (pkp_coop_follow.cpp); the list holds a creature only between the game's
// own restore of a saved story follower and the escort's first attach.
// The other readers of that list get no row. Each is a refusal of the same kind and reads "none" in a session: the follow
// talk's offer while a story follower is with the player (talk conditions 0x4e / 0x51; the leader's five places bound it), the
// island trip's offers while one character is the story's follower (0x116dc0; false on every visitor already), the map page's
// wording of the refusals above. "Is this creature the story's follower" is not asked there but through the creature's story
// mark, which every process sets for an escort (pkp_coop_escort_marks.cpp).
PKP_PARTY_OFF(0x7b0cfc, 0x4e7a40, 0, 0x7100041fu, "story follower", "an area gate opens for the host while a story follower is with it, as it does for a teammate")
PKP_PARTY_OFF(0x7cc76c, 0x4e7a40, 0, 0x7100041fu, "story follower", "the same refusal of the gate object's press (first copy)")
PKP_PARTY_OFF(0x7cc890, 0x4e7a40, 0, 0x7100041fu, "story follower", "the same, second copy")
PKP_PARTY_OFF(0x7ccba8, 0x4da0e0, 0, 0x52000008u, "story follower", "the gate object's passage can start while a story follower is with the host")
PKP_PARTY_OFF(0xfbacfc, 0x4da0e0, 0, 0x360000a0u, "story follower", "the map page lets the host travel to another area while a story follower is with it")

// ---- a player has an item: any one player's bag (pkp_coop_party_bags.cpp) ----
// Contents conditions 0x7e4 (0x126ebc: the item, the wanted number and the compare, counted with 0x2e9d74 on this process's
// player) and 0x7da (0x126db8: the same over the save's bag holder, 0x5b253c), and quest condition type 0x11 (in 0x11ccb8: at
// least n). None of them is followed by a take: no contents action or quest effect takes an item. Not here: contents condition
// 0x7e5 (a count by item category), the request checker's bag types (their hand-in takes the items) and the talk scripts' item
// tests (each belongs to a give or a hand-over).
PKP_PARTY_COUNT(0x126f58, 0x2e9d74, 0x18, 21, 22, 0x710012dfu, "bag", "a story step that asks for an item counts it in any one player's bag (condition 0x7e4)")
PKP_PARTY_COUNT(0x126e44, 0x5b253c, 0x08, 20, 21, 0x710012bfu, "bag", "the same for the story's other item test (condition 0x7da)")
PKP_PARTY_COUNT(0x11d118, 0x2e9d74, 0x18, 20, 0xff, 0x6b20229fu, "bag", "a quest that waits for an item is met when any one player holds it (quest condition 0x11)")

// ---- a creature's request for items the players hold (request types 1 and 0xd; the checker 0x613c9c) ----
// The request checker counts this player's bag for four request types at one site (w21 the type) and for type 0xd a second
// item right after. Type 0xd only asks to have two items: nothing in the request code raises its progress or takes them, the
// check alone makes it ready. Type 1 asks to bring an item: the hand-in (0x61ddf8) asks the checker again and queues the give
// (0x61e588), whose callback 0x49101c takes the item from the talking player's bag, does not look at what it took and raises
// the progress by the asked count. So both types count any one player's bag: any player can hand a type 1 request in while a
// teammate holds the item, and the item then stays in that teammate's bag (nothing moves between bags; the owner's call: the
// group's progress comes first). Types 0x33 (met by feeding the creature in the field) and 0x39 (a give by item tag) keep this
// player's own count.
PKP_PARTY_COUNT_OF(0x614028, 0x2e9d74, 0x28, 21, 1ull << 0xd | 1ull << 1, 0x12003c16u, "request", "a creature's request for an item is ready, and can be handed in, when any one player of the session holds it (request types 1 and 0xd, the first item)")
PKP_PARTY_COUNT_OF(0x614560, 0x2e9d74, 0x28, 21, 1ull << 0xd, 0x0b2022d6u, "request", "the second item of a request to have two items (request type 0xd)")

// ---- this player befriended the creature: the group's town did (0x284ef4, 47 call sites) ----
// 0x284ef4(comp) compares the parents of the creature's friend record (as selected: Cloud Island's, on a visitor the copy of the
// host's, else the own) with this process's user id. On a member that is false for every creature of the group's town, so a
// member lost what the host has with them: the talk option to move a creature into a house (0x4db9f4), the talk rows and
// conditions for one's own creatures (talk conditions 0x7, 0x26, 0x27, 0x46, 0x47, 0x52), the friendship points and meter (they
// read 0 through 0x31fb00 / 0x3b7ee8), the "has something to say" balloon (0x3af3ec) and its flag (script callback 0x4ab268,
// whose send to the authority in pkp_coop_friendship.cpp sits behind this test), list availability (0x3bcf9c). A member
// answers 1 for a creature that has parents, as the host does. Five callers keep the game's answer (kOwnsKept): a member must
// not run their step.
PKP_PARTY_PRED(0x284ef4, Owns, 0xd503237fu, "own creature", "a teammate has the town's creatures as its own, as the host does: the move-in talk option, the talk rows for one's own creatures, their friendship values")
