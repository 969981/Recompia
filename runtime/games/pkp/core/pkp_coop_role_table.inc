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

// ---- TABLE-AFTER-MIRROR rows (the mirror ships in the same build) ----
