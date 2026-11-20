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
