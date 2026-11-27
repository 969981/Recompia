// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: co-op stage mode's table (pkp_coop_stage.cpp): the record families a co-op session keeps in Cloud Island's record set
// (m_CloudStageSaveData), and the sites where the game's Cloud Island test 0x77ecb0 (GM+0x18 == 0x11) picks that set for one of them.
//
//   PKP_STAGE_FAMILY(id, wire, name, own, cloud, at, bytes, size, count, tally)
//     id      the Family enumerator (pkp_coop_internal.h, in this order); wire the u8 that names it in StageAsk / StageTable (never
//             reused); name for the log
//     own     save root + : this save's own records (m_saveCommon, CMNDAT); what solo play and the next session read
//     cloud   save root + : Cloud Island's records (m_saveStage, STGDAT); what the rows make every process read in a session
//     at, bytes  the fields of each record the mode keeps the group's (seeded, sent, carried over area loads, copied back)
//     size, count  the record stride and how many records there are
//     tally   record + : a u8 field the log counts the records with it set of, next to each checksum (one the game replicates, so
//             host and member lines compare after the seed too)
//   PKP_STAGE_ROW(ret, word, family, what)
//     ret     main offset of the instruction after one `bl 0x77ecb0` (its x30); the midhook runs there, before that instruction, while
//             w0 is still the test's answer and nothing has read it (every result in these functions has exactly one reader, the
//             tbz / tbnz at or two instructions after the row: TALK-PILOT-DESIGN 3); x0 = 1 (the Cloud Island answer)
//     word    the instruction word at ret, checked with every co-op site before the hooks are armed; `bl 0x77ecb0` at ret - 4 is
//             checked the first time the row is reached
//     what    the select or gate, for the log line of the row's first answer in a session
//   PKP_STAGE_RESET(ret, word, family, reg, what)
//     a reset of one record through its own select (ret and word as a row's): the game's answer stands, and the handler also clears
//     the family's fields of record wReg in the Cloud set (record 0 when wReg >= count, as the game's own csel does)
// One group per family, ascending in each group; pkp_coop_stage.cpp sorts the rows and refuses a site listed twice. Every row needs its
// `midhook 0x71<ret>` line in recomp/games/pkp/main.sites (the co-op stage mode block, ascending). pkp_coop.cpp's site table includes
// this file too (each row and reset one Site::StageAnswer midhook).

#ifdef PKP_STAGE_FAMILY
// The talk record, 0x38 bytes per chara (< 0x41a; serializer 0x5eb6c0): +0x8..+0x13 m_TalkComponentSaveParam (+8 f32 the walk-up
// timer, +0xc u16, +0xe / +0xf u8, +0x10 u8 the event-talk flag, +0x11 u8). The story-skit fields +0x14..+0x31 stay on today's
// records (pkp_coop_skits.cpp, StorySkits): their 18 selects are not rows.
PKP_STAGE_FAMILY(Talk, 1, "talk", layout::kTalkRecOwn, layout::kTalkRecCloud, layout::kTalkRecParams, layout::kTalkRecParamBytes,
                 layout::kTalkRecSize, layout::kFriendCount, layout::kTalkRecArmed)
#endif

// ---- talk: the talk-parameter selects (52), each picks the Cloud record for a read or write of +0x8..+0x11 ----
PKP_STAGE_ROW(0x2ac4d0, 0x36000160u, Talk, "0x2ac320 replication send: +8")              // tbz w0, #0, 0x2ac4fc
PKP_STAGE_ROW(0x2ac5c0, 0x36000160u, Talk, "0x2ac320 replication send: +0x10")           // tbz w0, #0, 0x2ac5ec
PKP_STAGE_ROW(0x2adddc, 0x36000160u, Talk, "0x2ad820 replication read: +8")              // tbz w0, #0, 0x2ade08
PKP_STAGE_ROW(0x2adecc, 0x360001c0u, Talk, "0x2ad820 replication read: +0x10")           // tbz w0, #0, 0x2adf04
PKP_STAGE_ROW(0x33a130, 0x360000a0u, Talk, "0x339d60: +0xc write")                       // tbz w0, #0, 0x33a144
PKP_STAGE_ROW(0x3ac540, 0xb0018db6u, Talk, "0x3ac440 talk component: +8 read")           // adrp x22 (tbz two later)
PKP_STAGE_ROW(0x3acb64, 0x360001a0u, Talk, "0x3acb4c record pointer (0x3af844 +0x10)")   // tbz w0, #0, 0x3acb98
PKP_STAGE_ROW(0x3acc08, 0x360001c0u, Talk, "0x3acbf0: +0x10 read")                       // tbz w0, #0, 0x3acc40
PKP_STAGE_ROW(0x3ad2c0, 0x360001e0u, Talk, "0x3ad29c flag apply: +0x10 write")           // tbz w0, #0, 0x3ad2fc
PKP_STAGE_ROW(0x3add44, 0x36000240u, Talk, "0x3add24: +0xf read")                        // tbz w0, #0, 0x3add8c
PKP_STAGE_ROW(0x3ae488, 0x360000e0u, Talk, "0x3ae37c: +0x11 read")                       // tbz w0, #0, 0x3ae4a4
PKP_STAGE_ROW(0x3afb30, 0xd0018d94u, Talk, "0x3afafc walk-up timer: +8 read")            // adrp x20 (tbz two later)
PKP_STAGE_ROW(0x3afbe4, 0x36000180u, Talk, "0x3afafc walk-up timer: +8 write")           // tbz w0, #0, 0x3afc14
PKP_STAGE_ROW(0x3b0a70, 0x37000740u, Talk, "0x3b09c4 condition 29: +0x10 read")          // tbnz w0, #0, 0x3b0b58
PKP_STAGE_ROW(0x3b0ad0, 0x37000440u, Talk, "0x3b09c4 condition 29: +0x10 read")          // tbnz w0, #0, 0x3b0b58
PKP_STAGE_ROW(0x3b0ae8, 0x37000380u, Talk, "0x3b09c4 condition 29: +0x10 read")          // tbnz w0, #0, 0x3b0b58
PKP_STAGE_ROW(0x3b0b54, 0x3607fcc0u, Talk, "0x3b09c4 condition 29: +0x10 read")          // tbz w0, #0, 0x3b0aec
PKP_STAGE_ROW(0x3b0c54, 0x37000740u, Talk, "0x3b0ba8 condition 31: +0x10 read")          // tbnz w0, #0, 0x3b0d3c
PKP_STAGE_ROW(0x3b0cb4, 0x37000440u, Talk, "0x3b0ba8 condition 31: +0x10 read")          // tbnz w0, #0, 0x3b0d3c
PKP_STAGE_ROW(0x3b0ccc, 0x37000380u, Talk, "0x3b0ba8 condition 31: +0x10 read")          // tbnz w0, #0, 0x3b0d3c
PKP_STAGE_ROW(0x3b0d38, 0x3607fcc0u, Talk, "0x3b0ba8 condition 31: +0x10 read")          // tbz w0, #0, 0x3b0cd0
PKP_STAGE_ROW(0x3b0dd0, 0x360001c0u, Talk, "0x3b0dac: +8 read")                          // tbz w0, #0, 0x3b0e08
PKP_STAGE_ROW(0x3b304c, 0x360001c0u, Talk, "0x3b3034: +0x10 read")                       // tbz w0, #0, 0x3b3084
PKP_STAGE_ROW(0x3b3100, 0x360001e0u, Talk, "0x3b30e4: +0xe write")                       // tbz w0, #0, 0x3b313c
PKP_STAGE_ROW(0x3b3260, 0x360001e0u, Talk, "0x3b3244: +0x11 write")                      // tbz w0, #0, 0x3b329c
PKP_STAGE_ROW(0x4a5280, 0x37000600u, Talk, "0x4a5040: +0x11 write")                      // tbnz w0, #0, 0x4a5340
PKP_STAGE_ROW(0x4a52e0, 0x37000300u, Talk, "0x4a5040: +0x11 write")                      // tbnz w0, #0, 0x4a5340
PKP_STAGE_ROW(0x4a52f8, 0x37000240u, Talk, "0x4a5040: +0x11 write")                      // tbnz w0, #0, 0x4a5340
PKP_STAGE_ROW(0x4a533c, 0x3607fe00u, Talk, "0x4a5040: +0x11 write")                      // tbz w0, #0, 0x4a52fc
PKP_STAGE_ROW(0x4ab444, 0x37000920u, Talk, "0x4ab3d4: +0xf then +0xe write")             // tbnz w0, #0, 0x4ab568
PKP_STAGE_ROW(0x4ab4a4, 0x37000620u, Talk, "0x4ab3d4: +0xf then +0xe write")             // tbnz w0, #0, 0x4ab568
PKP_STAGE_ROW(0x4ab4bc, 0x37000560u, Talk, "0x4ab3d4: +0xf then +0xe write")             // tbnz w0, #0, 0x4ab568
PKP_STAGE_ROW(0x4ab518, 0x36000420u, Talk, "0x4ab3d4: +0xe write")                       // tbz w0, #0, 0x4ab59c
PKP_STAGE_ROW(0x4ab564, 0x3607fae0u, Talk, "0x4ab3d4: +0xf then +0xe write")             // tbz w0, #0, 0x4ab4c0
PKP_STAGE_ROW(0x4ab598, 0x3707fc20u, Talk, "0x4ab3d4: +0xe write")                       // tbnz w0, #0, 0x4ab51c
PKP_STAGE_ROW(0x4ae708, 0xf0018594u, Talk, "0x4ae68c callback 195: +0x10 write")         // adrp x20 (tbz two later)
PKP_STAGE_ROW(0x4ae770, 0xf0018594u, Talk, "0x4ae68c callback 195: +0x10 write")         // adrp x20 (tbz two later)
PKP_STAGE_ROW(0x4ae790, 0xf0018594u, Talk, "0x4ae68c callback 195: +0x10 write")         // adrp x20 (tbz two later)
PKP_STAGE_ROW(0x4ae894, 0xf0018594u, Talk, "0x4ae68c callback 195: +0x10 write")         // adrp x20 (tbz two later)
PKP_STAGE_ROW(0x4bbedc, 0x3607fd40u, Talk, "0x4bbde0: +0xe / +0xf write")                // tbz w0, #0, 0x4bbe84
PKP_STAGE_ROW(0x4bc96c, 0x360000c0u, Talk, "0x4bc8c0: +0xe read")                        // tbz w0, #0, 0x4bc984
PKP_STAGE_ROW(0x4bc9e4, 0x37000620u, Talk, "0x4bc8c0: +0xf read")                        // tbnz w0, #0, 0x4bcaa8
PKP_STAGE_ROW(0x4bca5c, 0x36000460u, Talk, "0x4bc8c0: +0xe / +0xf write")                // tbz w0, #0, 0x4bcae8
PKP_STAGE_ROW(0x4bcaa4, 0x3607fa20u, Talk, "0x4bc8c0: +0xf read")                        // tbz w0, #0, 0x4bc9e8
PKP_STAGE_ROW(0x4bcac0, 0x3707f2e0u, Talk, "0x4bc8c0: +0xe / +0xf write")                // tbnz w0, #0, 0x4bc91c
PKP_STAGE_ROW(0x4bd544, 0x37000760u, Talk, "0x4bd420 flag clear: +0x10 write")           // tbnz w0, #0, 0x4bd630
PKP_STAGE_ROW(0x4bd610, 0x3607f9c0u, Talk, "0x4bd420 flag clear: +0x10 write")           // tbz w0, #0, 0x4bd548
PKP_STAGE_ROW(0x4bd62c, 0x3607f8e0u, Talk, "0x4bd420 flag clear: +0x10 write")           // tbz w0, #0, 0x4bd548
PKP_STAGE_ROW(0x4c2cf8, 0x36000240u, Talk, "0x4c2900: +8 read")                          // tbz w0, #0, 0x4c2d40
PKP_STAGE_ROW(0x4ee06c, 0x37000740u, Talk, "0x4edf60 event clear: +0x10 write")          // tbnz w0, #0, 0x4ee154
PKP_STAGE_ROW(0x4ee13c, 0x3607fa20u, Talk, "0x4edf60 event clear: +0x10 write")          // tbz w0, #0, 0x4ee080
PKP_STAGE_ROW(0x4ee150, 0x3607f980u, Talk, "0x4edf60 event clear: +0x10 write")          // tbz w0, #0, 0x4ee080
// ---- talk: the gates (13). A dirty mark 0x282dc4(&actor, 0x10) acts only online, not GameShare, on the host (its talk component
// replication then carries +8 / +0x10 to the members); the delta bit 40 is sent only by a visitor's talk end (0x2159c0); the two
// refusals are condition 29 / 31's "visitor and not Cloud -> 0" ----
PKP_STAGE_ROW(0x2adefc, 0x37000300u, Talk, "0x2ad820 dirty-mark gate")                   // tbnz w0, #0, 0x2adf5c
PKP_STAGE_ROW(0x2adf58, 0x360000c0u, Talk, "0x2ad820 dirty-mark gate")                   // tbz w0, #0, 0x2adf70
PKP_STAGE_ROW(0x3ad2f4, 0x37000320u, Talk, "0x3ad29c dirty-mark gate (Cloud arm)")       // tbnz w0, #0, 0x3ad358
PKP_STAGE_ROW(0x3ad354, 0x360000c0u, Talk, "0x3ad29c dirty-mark gate (own arm)")         // tbz w0, #0, 0x3ad36c
PKP_STAGE_ROW(0x3b0b90, 0x3607f3c0u, Talk, "0x3b09c4 condition 29 visitor refusal")      // tbz w0, #0, 0x3b0a08
PKP_STAGE_ROW(0x3b0d74, 0x3607f3c0u, Talk, "0x3b0ba8 condition 31 visitor refusal")      // tbz w0, #0, 0x3b0bec
PKP_STAGE_ROW(0x4ae7ec, 0x360000c0u, Talk, "0x4ae68c dirty-mark gate (off-Cloud arm)")   // tbz w0, #0, 0x4ae804
PKP_STAGE_ROW(0x4ae808, 0x360001e0u, Talk, "0x4ae68c talk delta bit 40 gate")            // tbz w0, #0, 0x4ae844
PKP_STAGE_ROW(0x4ae8c8, 0x3707f940u, Talk, "0x4ae68c dirty-mark gate (Cloud arm)")       // tbnz w0, #0, 0x4ae7f0
PKP_STAGE_ROW(0x4bd580, 0x360000c0u, Talk, "0x4bd420 dirty-mark gate")                   // tbz w0, #0, 0x4bd598
PKP_STAGE_ROW(0x4bd654, 0x3607fa20u, Talk, "0x4bd420 dirty-mark gate")                   // tbz w0, #0, 0x4bd598
PKP_STAGE_ROW(0x4ee0b8, 0x360000c0u, Talk, "0x4edf60 dirty-mark gate")                   // tbz w0, #0, 0x4ee0d0
PKP_STAGE_ROW(0x4ee178, 0x3607fac0u, Talk, "0x4edf60 dirty-mark gate")                   // tbz w0, #0, 0x4ee0d0
// ---- talk: the per-chara reset 0x2cc660 (callers 0x2cc600, 0x2cd67c), which clears the whole record (both field groups) through
// its select: the game's answer stands (the story-skit fields are cleared where StorySkits keeps them), the handler clears the
// talk parameters of chara w19 in the Cloud set too (record 0 for w19 >= 0x41a, as the game's csel does) ----
PKP_STAGE_RESET(0x2ccb34, 0x36000120u, Talk, 19, "0x2cc660 per-chara reset")              // tbz w0, #0, 0x2ccb58
