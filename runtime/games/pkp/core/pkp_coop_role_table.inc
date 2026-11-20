// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's role answer table (pkp_coop_role.cpp): the call sites of the game's role getters whose answer is a pure
// permission or display check that refuses or limits a member. At each, a member in a session gets the host's answer.
//
// One row per site, sorted by return address (strictly ascending: pkp_coop_role.cpp checks it at compile time):
//   PKP_ROLE_ROW(ret, getter, word, tag, what)
//     ret     main offset of the instruction after the `bl getter` (the guest x30 the call returns to); the midhook runs there, before
//             that instruction, so w0 is still the getter's answer and whatever reads it next reads the host's
//     getter  IsVisitor (0x376c4, GM+0x90 bit 2; the host's answer 0) or IsHost (0xa40bc, GM+0x90 bit 1; the host's answer 1)
//     word    the instruction word at ret, checked with every co-op site before the hooks are armed
//     tag     the system, short (shown in the log line the first time the row answers in a session)
//     what    what changes for a member, in player terms
// Never a tail call (`b getter`: it returns to the outer caller, whose x30 is not this site) and never a site whose instruction before
// ret is not `bl getter` (checked again at run time, the first time the row is reached). Every row needs its `midhook 0x71<ret>` line
// in recomp/games/pkp/main.sites (the co-op role answer block).
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

// ---- TABLE-AFTER-MIRROR rows (the mirror ships in the same build): none in this slice ----
