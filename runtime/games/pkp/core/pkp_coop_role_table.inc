// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's role answer table (pkp_coop_role.cpp): the places where the game asks "am I the visitor / the host" for
// a pure permission or display check that refuses or limits a member. At each, a member in a session gets the host's answer.
//
// One row per site, in groups by system (pkp_coop_role.cpp sorts them at compile time and refuses a site listed twice). Two kinds:
//   PKP_ROLE_ROW(ret, getter, word, tag, what)        a call of a role getter
//     ret     main offset of the instruction after the `bl getter` (the guest x30 the call returns to); the midhook runs there, before
//             that instruction, so w0 is still the getter's answer and whatever reads it next reads the host's
//     getter  IsVisitor (0x376c4, GM+0x90 bit 2; the host's answer 0) or IsHost (0xa40bc, GM+0x90 bit 1; the host's answer 1)
//   PKP_ROLE_BIT(site, reg, bit, word, tag, what)     an inline read of the session bits: `ldrb wReg, [.., #0x90]` right before site
//     site    main offset of the `tbz / tbnz wReg, #bit` that tests them; the midhook clears bit 2 (the visitor's) or sets bit 1 (the
//             host's) in wReg before the test. Only a site where nothing but that test reads wReg (every path the host answer takes
//             writes it or calls out first)
//   word      the instruction word at the site, checked with every co-op site before the hooks are armed
//   tag       the system, short (shown in the log line the first time the row answers in a session)
//   what      what changes for a member, in player terms
// Never a tail call (`b getter`: it returns to the outer caller, whose x30 is not this site). The instruction before each site is
// checked again at run time, the first time the row is reached (`bl getter`, or the `ldrb` of the session bits). Every row needs its
// `midhook 0x71<site>` line in recomp/games/pkp/main.sites (the co-op role answer block, ascending).
//
// The pkp_coop.cpp site table includes this file too (each row one Site::RoleAnswer midhook).

// ---- TABLE rows ----
PKP_ROLE_ROW(0x7596cc, IsVisitor, 0x37000300u, "effects", "this gimmick plays its type effect when it changes state (fx 0x25 / 0x27 / 0x3e)")
PKP_ROLE_ROW(0x7fca2c, IsVisitor, 0x370005a0u, "effects", "the cell effect after a use of this gimmick plays")
PKP_ROLE_ROW(0x7fd1ac, IsVisitor, 0x370005a0u, "effects", "this gimmick cell effect plays")
PKP_ROLE_ROW(0x7fd6b0, IsVisitor, 0x36000060u, "effects", "this gimmick cell effect plays")
PKP_ROLE_ROW(0x7fe00c, IsVisitor, 0x370005a0u, "effects", "the cell effect plays (the player cue already ran on both paths)")
PKP_ROLE_ROW(0x7fe450, IsVisitor, 0x360000c0u, "effects", "the end effect plays")
PKP_ROLE_ROW(0x81ce3c, IsVisitor, 0x36001160u, "effects", "the end-of-use effect plays (fx 0x18)")
PKP_ROLE_ROW(0x81d584, IsVisitor, 0x370000c0u, "effects", "this gimmick cell effect plays (fx 0x18)")
PKP_ROLE_ROW(0x81d608, IsVisitor, 0x36000200u, "effects", "this gimmick cell effect plays")
PKP_ROLE_ROW(0x81d728, IsVisitor, 0x370000c0u, "effects", "this gimmick cell effect plays")
PKP_ROLE_ROW(0x81d874, IsVisitor, 0x36000540u, "effects", "this gimmick cell effect plays")
PKP_ROLE_ROW(0x81dce8, IsVisitor, 0x36000320u, "effects", "this gimmick cell effect plays")
PKP_ROLE_ROW(0x81de30, IsVisitor, 0x36000320u, "effects", "this gimmick cell effect plays")
PKP_ROLE_ROW(0x81f270, IsVisitor, 0x36000120u, "effects", "this gimmick effect plays (fx 0x19), first lookup path")
PKP_ROLE_ROW(0x81f290, IsVisitor, 0x37000100u, "effects", "the same, second lookup path")
PKP_ROLE_ROW(0xab5ad8, IsVisitor, 0x36000200u, "crafting", "with ability 77 off, recipes 0x9a, 0x9c, 0x133 and 0x2a5 can be crafted, not 'cannot craft'")
PKP_ROLE_ROW(0xfa9e00, IsVisitor, 0x360000e0u, "requests app", "the tabs of every opened area, not only the current one (type-3 stage, not Cloud Island)")
PKP_ROLE_ROW(0xfaa3d0, IsVisitor, 0xf9000bf3u, "requests app", "the list includes the normal missions and the unique / general requests (first of three inlined copies)")
PKP_ROLE_ROW(0xfaa3e4, IsVisitor, 0xf9000bf3u, "requests app", "the same, second copy")
PKP_ROLE_ROW(0xfaa3f8, IsVisitor, 0xf9000bf3u, "requests app", "the same, third copy")
PKP_ROLE_ROW(0xfab940, IsVisitor, 0x360000e0u, "requests app", "the 'new' badge also scans the normal missions and unique requests")

// ---- session-bit rows, after the story-record mirror (pkp_coop_records.cpp, the same build) ----
// The host's path reads the member's own copy of the story records (stage data R+0x972ba0, the town flags R+0x4a27cc), which the
// mirror keeps the authority's; without it these would read stale records.
PKP_ROLE_BIT(0x16b70, 8, 2, 0x371002e8u, "story scene", "a creature the story holds in a scene or forced work counts as busy (availability, talk prompt)")
PKP_ROLE_BIT(0x16c44, 8, 2, 0x37100288u, "town flags", "town flag bit 5 of a creature leaves it out of the availability check")
PKP_ROLE_BIT(0x31c84, 8, 2, 0x37100348u, "story scene", "a creature in a story scene is reported as such")
PKP_ROLE_BIT(0x3f650, 8, 2, 0x37100328u, "talk", "talk callbacks 51 / 52 refuse a creature in forced work or a story scene")
PKP_ROLE_BIT(0x3f6e0, 8, 2, 0x37100228u, "talk", "the same callbacks: town flag bit 1 refuses the branch")
PKP_ROLE_BIT(0x3f7a0, 8, 2, 0x37100288u, "town flags", "town flag bit 1 of each creature (talk prompts, a general request read)")
PKP_ROLE_BIT(0x47b9c, 8, 2, 0x37100068u, "specialty", "a story-forced specialty on or off applies (may do its specialty)")
PKP_ROLE_BIT(0x485d8, 8, 2, 0x37100068u, "specialty", "item-making and specialty offers follow the story-forced specialty")
PKP_ROLE_BIT(0x48820, 8, 2, 0x37100428u, "specialty", "a story-forced-off specialty is reported")
PKP_ROLE_BIT(0x49f90, 8, 2, 0x371009a8u, "town flags", "town flag bit 3 (and the per-town rule of creature 0x2c)")
PKP_ROLE_BIT(0x4a028, 8, 2, 0x371004e8u, "town flags", "the same function, the other creature path")
PKP_ROLE_BIT(0x4a0a0, 8, 2, 0x37100128u, "town flags", "the same function, the creature 0x2c fall-through")
PKP_ROLE_BIT(0x4a114, 8, 2, 0x37100288u, "town flags", "town flag bit 3 (talk prompt, daily fill, removal)")
PKP_ROLE_BIT(0x4a808, 8, 2, 0x371004c8u, "interaction", "the interaction-range check refuses a creature in forced work or a story scene")
PKP_ROLE_BIT(0x4a9c0, 8, 2, 0x37100388u, "story scene", "'in a story scene' (talk prompts, availability)")
PKP_ROLE_BIT(0x4a9f0, 8, 2, 0x37100208u, "story scene", "the same, second inlined copy")
PKP_ROLE_BIT(0x4ab5c, 8, 2, 0x37100328u, "interaction", "the interaction check refuses a creature in forced work or a story scene")
PKP_ROLE_BIT(0x51494, 8, 2, 0x37100288u, "story scene", "creature condition 68, in a story scene")
PKP_ROLE_BIT(0x519e4, 8, 2, 0x37100308u, "town flags", "creature condition 76, town flag bit 3")
PKP_ROLE_BIT(0x526a0, 8, 2, 0x371002c8u, "town flags", "creature condition 74, town flag bit 2")
PKP_ROLE_BIT(0x11418c, 8, 2, 0x3717fe08u, "specialty", "the specialty check by creature id follows the story-forced specialty")
PKP_ROLE_BIT(0x13a774, 8, 2, 0x371003a8u, "story scene", "the talk check reads the story-situation entry")
PKP_ROLE_BIT(0x13a834, 8, 2, 0x371002e8u, "story scene", "the same, a second entry")
PKP_ROLE_BIT(0x13a8f4, 8, 2, 0x37100208u, "story scene", "the same, a third entry")
PKP_ROLE_BIT(0x13ae48, 8, 2, 0x37100288u, "story scene", "a creature the story holds back does not walk up to the player")
PKP_ROLE_BIT(0x13af80, 8, 2, 0x37100248u, "story scene", "the story-forced specialty of creature 0x2c (talk prompt)")
PKP_ROLE_BIT(0x13b294, 8, 2, 0x37100068u, "talk", "the present limit: no present the host would refuse")

// ---- talk start: the first talk with a new creature (coop-s3-talkstart) ----
// The talk handler 0x4c024c, outside Cloud Island: on a visitor, a creature with no parents (0x285100 false) of species 0x46 or
// 0x95 returns 0 at once (0x4c03e8..0x4c0474), so the handler never reaches its host / visitor split at 0x4c06d4 (TalkKind) and
// the selector's last stage 0x4c8ba0 offers the visitor's own talk instead (row 35 of its table #960: conditions 0x25 IsVisitor,
// 0x24 no parents, 0x22 online, 0xa4 species 0x46 / 0x95; u16 +2 0xe63). w0 is read by the `tbz` alone; the host's arm 0x4c053c
// reads only what the member has (the actor's own talk record +0x10, 0x332c80 / 0x3bad4c on the member's copy of the friend
// records, the species talk table), as the TalkKind path already does for every other creature.
PKP_ROLE_ROW(0x4c03e4, IsVisitor, 0x36000ac0u, "talk", "the first talk with a new creature of species 0x46 / 0x95 is the host's talk, not the visitor's own one")

// ---- the census's last gates (coop-s7-rows) ----
// 0x4a380(chara): town flag bit 3 (R+0x4a27cc, mirrored) after the resident and Cloud Island tests; its callers 0x4eed90, 0x4f06c0,
// 0x5a2910 and 0x1c91e0 (through 0x284308's tail call) leave a creature out when it is set, as the 0x4a114 row's do. Its stage-start
// caller 0x11915c writes the member's own story bytes R+0x4a2fb7 / +0x4a2f59 / +0x4a2fb8 (not mirrored) when it is set, so those
// three calls keep the visitor's answer (the PKP_ROLE_KEEP rows below).
PKP_ROLE_BIT(0x4a3d0, 8, 2, 0x3717fee8u, "town flags", "town flag bit 3 by creature leaves it out (work, talk and condition checks), as on the host")
// 0x1f0f40(grid, &pos, w2): blocks near a position whose kind has flag bit 1. Talk callback 45 (0x4deb24, w2 = 0) only reads it: a
// block nearby starts talk script 316, none script 321. The replicated block grid is the member's own copy, so the read is the
// world's. Callback 75 (0x498238, w2 = 1) changes every such block (0x1f1200: grid and placed-object calls): a member's local
// change of the world, so that call keeps the visitor's answer (PKP_ROLE_KEEP). The fall-through reloads x8 at once.
PKP_ROLE_BIT(0x1f0f84, 8, 2, 0x371011c8u, "talk", "talk callback 45 sees the blocks near the creature (talk 316 when one is near, not always 321)")
// 0x988fe0(obj, level): a level change of a house this player owns (the owner id at the object's entry vs 0x2fc0bc, the own id
// R+0x13d3a89) posts "Your comfort level is now N!" (message 0xec, 0xa8ec90); in stage 0xc only a visitor does not get it.
// Display only; w0 is read by the `eor` alone.
PKP_ROLE_ROW(0x98906c, IsVisitor, 0x52000008u, "notice", "in stage 0xc the comfort-level notice of a house this player owns shows")
// 0x7fe270 (gimmick class 0x32701a0 slot 103): after a use of types 0x6e / 0x16 the 1 s object timer +0x44 starts, which the class's
// own step (slot 22, 0x7fb894) counts down and slot 87 (0x7fe260) reads. Its sibling uses start the same timer on every process
// (0x7fca24, 0x7fd46c, 0x7fd588, 0x7fd658, no role test); only this one skips a visitor. Local object state, nothing sent.
PKP_ROLE_ROW(0x7fe42c, IsVisitor, 0x37000100u, "gimmicks", "this gimmick's short timer after a use starts, as its siblings' do")
// Unique request 73's state byte R+0x6facfd (R+0x6fabd9 + 73 * 4): pkp_coop_requests.cpp keeps it the group's (taken on, fulfilled,
// done), and the request is a story one. 0x34b20 refuses its creature while the request is 2..4; 0x13aaf4 (creature screen 0xf17d30)
// answers "held by the story" while it is 2 or 3 and the creature's stage record (R+0x972c26 + 0x5f92b4, mirrored) agrees. Both
// only answer no (0x34b20 returns 0 to its callers, 0xf17d30 refuses its action), and the fall-through of each is `bl 0x77ecb0`.
PKP_ROLE_BIT(0x34c68, 8, 2, 0x37100308u, "story request", "the creature of the story request 73 is not free while that request is open")
PKP_ROLE_BIT(0x13ab38, 8, 2, 0x37100548u, "story request", "the creature screen refuses its action for that creature while the request is open")
PKP_ROLE_BIT(0x13aba8, 8, 2, 0x371001c8u, "story request", "the same, the inlined second test")

#ifdef PKP_ROLE_KEEP
// PKP_ROLE_KEEP(site, fn, from, why): at the row at site (in the function fn), when fn's frame record (x29 + 8, the caller's x30;
// fn sets x29 before the row on every path) is main + from, the row keeps the game's answer. from - 4 is checked to be `bl fn`.
PKP_ROLE_KEEP(0x4a3d0, 0x4a380, 0x119250, "stage start, creature 0xdb: writes the own R+0x4a2fb7")
PKP_ROLE_KEEP(0x4a3d0, 0x4a380, 0x1192b0, "stage start, creature 0x7d: writes the own R+0x4a2f59")
PKP_ROLE_KEEP(0x4a3d0, 0x4a380, 0x119310, "stage start, creature 0xdc: writes the own R+0x4a2fb8")
PKP_ROLE_KEEP(0x1f0f84, 0x1f0f40, 0x49832c, "talk callback 75 changes the blocks it finds")
#endif
// ---- the Star Piece swap (coop-s7-starpiece) ----
// Script callback slot 119 0x4a17fc (talk 0x1b3: a creature takes the held Star Piece and gives its item), online, off Cloud Island,
// GM+0x11 bit 0 clear: `bl IsHost` at 0x4a1b6c, then `eor w8, w0, #1` is the flag that queues line 438 ("not a good time") instead
// of the swap. w0 is read by the `eor` alone. The swap's give (slot 27) and receive (slot 28) are the member's own bag; the give's
// friendship reaches the authority by its talk-end apply (0x21c46c, pkp_coop_friendship.cpp). Its parent term: TalkSwapParent.
PKP_ROLE_ROW(0x4a1b70, IsHost, 0x52000008u, "talk", "a creature takes a teammate's Star Piece and gives its item, as for the host")
// ---- the campaign creatures' talk test ----
// 0x29d910 (may this creature be talked to; 2 = no), for a creature the area test 0x284660 ties to an event campaign, online and with
// GM+0x11 bit 0 clear: `bl IsHost` at 0x29dcc4, then `tbz w0, #0` sends a non-host to the short test (refused while the creature's
// state is 4). The host's arm reads the campaign history of this process's save (R+0x6cec38 + slot * 6, bit 0), which is the group's
// on a member (pkp_coop_records.cpp record 7), then 0x168e4 / 0x23fb4c. w0 is read by the `tbz` alone.
PKP_ROLE_ROW(0x29dcc8, IsHost, 0x36000420u, "talk", "a campaign creature can be talked to by a teammate when the host could")
