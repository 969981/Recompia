// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's escort record in the save (pkp_coop_internal.h, follow::marks): the game's own story mark of a
// character, set for every escort (pkp_coop_follow.cpp). RE: modding-docs coop-v2/PARTY-CHECKS-RE.md sections 14 and 15.
//
// The game keeps one 0x10-byte follow row per character in the player's own save file (the one that holds the story): byte +9 the
// invited mark, byte +0xa the story mark. The story's own attach sets +0xa (0x659280) and its leave clears it (0x659bf8, only for
// a creature in the story's list). The game asks "is this creature the story's follower" through that byte in about twenty
// places (the creature's AI classes, which then keep following; the talks that pick a line or an option by it; the island trip),
// and outside a session it puts a marked creature back as the player's story follower when the creature spawns (0x296474).
// An escort rides the invited follow, so the game never marks it. Co-op does, in the row the running game reads:
//   - the host's own save. The record then survives a save, a reload and a session that closes and opens again in the file the
//     story itself is saved in, by the same save, and a load of that save without co-op needs nothing from co-op: the game
//     restores the creature as a story follower and the story's release ends it;
//   - on a member, the copy of the host's rows its game reads while it visits, and nothing else: a member's own save is its own
//     story's and is never written.
// A row is named by the creature's save index, the u32 the game's own attach, leave and restore take from the creature's party
// block: an index is only ever read from a live creature, never derived.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

namespace nxrt::games::pkp::coop::follow::marks {

namespace {

// The story mark of a save index, in this player's own save or in the visiting copy. 0: not readable, or no row of its own.
uint64_t mark_at(uint32_t index, bool visiting) {
    if (index == 0 || index >= layout::kFollowRowCount) return 0;   // row 0 is the row of every index out of range: nobody's
    const uint64_t save = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t at = save + (visiting ? layout::kFollowRowsVisiting : layout::kFollowRows) + uint64_t{index} * layout::kFollowRowSize +
                        layout::kFollowRowStory;
    return save && guest::ok(at, 1) ? at : 0;
}

}  // namespace

bool marked(uint32_t index) {
    const uint64_t at = mark_at(index, false);
    return at && guest::rd<uint8_t>(at);
}

bool put(uint32_t index, bool on, bool visiting) {
    const uint64_t at = mark_at(index, visiting);
    if (!at) return false;
    if ((guest::rd<uint8_t>(at) != 0) != on) guest::wr<uint8_t>(at, on ? 1 : 0);
    return true;
}

}  // namespace nxrt::games::pkp::coop::follow::marks
