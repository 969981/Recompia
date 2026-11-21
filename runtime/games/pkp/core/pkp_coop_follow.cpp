// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's story follows (pkp_coop_internal.h): a story content that makes a creature follow this player, on a
// member, goes to the host as the game's own Link Play join request. RE: modding-docs coop-v2/quests/QUEST-SCRIPT-RE.md (contents
// actions), coop-v2/P0-RE.md (RequestPartyNPC on a member); the fire starter creature's script is #7099 records 0x20d / 0x23b / 0x20f.
//
// A content record's command 0xdac (data id, mode) is 0x12c940 -> 0x117b18(data id, mode, 0, 1, 0). Mode 3 is "follow this
// process's player": with its w4 clear (0x12c940 passes 0) it calls 0x2a1ed4(&npc actor id, 1), which takes the leader from the
// local player ([0x3561928] + 0x30, its u16 actor id at +0x27fe) and calls 0x659280(party manager, &leader, &npc, flag). 0x659280
// first takes the creature out of the party it is in through 0x659588 (w5 = 1), then attaches it to the leader locally (0x6599c0
// accepts only a local leader, vt+0x1e0) and marks its record +0xa (own R+0x23c3d0, or the visitor's copy R+0x16fefd8). On a
// visitor (every co-op member) 0x659588 does not detach: it sends RequestPartyNPC with +0x38 = 0 (a leave) and returns. The host's
// handler 0x5519bc reads that form as "take the creature from its current leader" (+0x34, the asking player, is not read), so a
// member's story follow took the creature away from the host's player, and the attach stayed in the member's memory only: on the
// host, which runs the creature's AI, nobody led it and it wandered (tester report: the fire starter creature "refused to follow
// the host or the players to light the campfire and just wandered around"; #7099 records 0x20d and, every 30 s, 0x23b do it).
// The game has the visitor-aware attach too: 0x117b18's mode 3 with w4 set calls 0x404e34(0, &npc, 1, 0), which passes the local
// player to 0x657a20(party manager [0x35659f8], &player, &npc, flag, w4); on a visitor 0x657a20 sends RequestPartyNPC with
// +0x38 = 1 (+0x34 the player, +0x36 the creature, +0x39 the flag, +0x3b w4), and the host's 0x5519bc makes that same 0x657a20
// call with the member's player there: the creature joins the member's party on the host.
//   0x2a1ed4 (entry hook; its only caller is 0x117b18, whose only mode-3 caller is the contents' 0x12c940), on a member in the
//   session: the game's local-only attach does not run. When this process's copy of the creature already follows a player
//   (vt+0x1d8, then vt+0x410's party block +0x35e0 +0x10, the leader test 0x2a1f44 makes), nothing is asked: the host's or a
//   teammate's follow is not taken away. Otherwise the game's 0x404e34(0, &npc, flag, 0) runs, the join request above. The host
//   keeps running its own contents' follows as before (its own player).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::follow {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kJoinRequest = 0x404e34;   // (slot, &u16 npc, w2 flag, w3): 0x657a20 for the slot's local player
constexpr uint64_t kActorById = 0x281360;     // (player manager, &u16 actor id) -> the actor, or 0
constexpr uint64_t kDataIdOf = 0x27d848;      // (&u16 actor id) -> u16 the character's data id
constexpr uint64_t kInParty = 0x1d8;          // actor vt + : bool, it is in a party
constexpr uint64_t kLeaderOf = 0x410;         // actor vt + : its leader's object, or 0
constexpr uint64_t kPartyBlock = 0x35e0;      // leader object + : its party block; +0x10 u16 the leader's actor id
constexpr uint64_t kPartyLeaderId = 0x10;

// 0x2a1f44's test of an id: bit 11 clear and (id >> 12) - 1 <= 3 (a player or character id, not the empty one).
bool valid_id(uint16_t id) { return !(id & 0x800) && static_cast<uint32_t>((id >> 12) - 1) <= 3; }

// The actor id of the player this process's copy of the creature follows, or 0 when it follows nobody here.
uint16_t leader_here(::Ctx* c, uint64_t npc) {
    const uint64_t pm = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    if (!pm) return 0;
    const uint64_t actor = guest::call(c, base() + kActorById, pm, npc);
    if (!guest::ok(actor, 8)) return 0;
    const uint64_t vt = guest::rd<uint64_t>(actor);
    if (!guest::ok(vt + kLeaderOf, 8) || !(guest::call(c, guest::rd<uint64_t>(vt + kInParty), actor) & 1)) return 0;
    const uint64_t leader = guest::call(c, guest::rd<uint64_t>(vt + kLeaderOf), actor);
    if (!guest::ok(leader + kPartyBlock + kPartyLeaderId, 2)) return 0;
    const uint16_t id = guest::rd<uint16_t>(leader + kPartyBlock + kPartyLeaderId);
    return valid_id(id) ? id : 0;
}

}  // namespace

int on_story_follow(::Ctx* c) {
    if (!quests::member_rules()) return 0;
    const uint64_t npc = c->x[0];
    if (!guest::ok(npc, 2) || !valid_id(guest::rd<uint16_t>(npc))) return 0;   // 0x659280 returns at once on these: as the game does
    const uint16_t id = guest::rd<uint16_t>(npc);
    const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, npc));
    if (const uint16_t leader = leader_here(c, npc)) {
        NXLOG_I(kTag, "Co-op follow: the story asked character %u (actor 0x%x) to follow this player; it follows player 0x%x already, "
                      "left as it is", data, id, leader);
        return 1;
    }
    guest::call(c, base() + kJoinRequest, 0, npc, c->x[1] & 1, 0);
    NXLOG_I(kTag, "Co-op follow: the story asked character %u (actor 0x%x) to follow this player; asked of the host (join request)",
            data, id);
    return 1;   // the game's local-only attach (and its leave request) does not run
}

}  // namespace nxrt::games::pkp::coop::follow
