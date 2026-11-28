// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's party facts (pkp_coop_internal.h): what the party as a whole has, for the checks the game makes of
// one player. The answer sites that use them are pkp_coop_party_answers.cpp (rows in pkp_coop_party_table.inc). Nothing else
// reads another player's state.
//
// The followers. The party manager PG = [main+0x35659f8] holds 11 lists of 0x28 bytes from PG+8 (list n: +0 the u16 actor ids, +8
// their count; 5 slots, an empty slot holds an id that fails the test below):
//   lists 0..3   the creatures each session player invited to follow it, by the player's session slot. The authority writes them
//                (0x6587e0 / 0x658560) and the game sends each to every member (its replication kind 4: 0x656200 raises, 0x5490ec
//                applies into lists 4..7, the member's tick 0x656880 copies them to 0..3), so every process holds all four
//   list 9       the story's followers of this process's own player (0x6599c0), never sent to anyone
//   list 10      every follower of this process's own player: its invited ones and its story ones
// The game's checks read list 10 alone, so each process knows only its own player's followers. The party's followers here are
// list 10 and lists 0..3 together: everything this process knows. A teammate's story follower is in no list of this process.
// An id is a follower when bit 11 is clear and (id >> 12) - 1 <= 3 (0x4e7a40's test).
//
// The town's creatures. A creature's friend record holds the user id of the player who befriended it (its parents, +0x8); the
// game's own tests compare it with this process's user id (save root + 0x13d3a89). Outside Cloud Island only the host's game
// writes parents, and a member reads its copy of the host's records, so on a member every creature of the group's town reads as
// somebody else's. The party's answer: a creature that has parents is the group's (owns).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

namespace nxrt::games::pkp::coop::party {

namespace {

constexpr uint64_t kResident = 0x286de4;    // (&u16 actor id) -> bool: a town creature (its index is none of the special ones)
constexpr uint64_t kDataIdOf = 0x27d848;    // (&u16 actor id) -> u16 the character's data id
constexpr uint64_t kAbilityOf = 0x285ad8;   // (&u16 actor id, w1 slot 0 / 1) -> u8 the creature's ability in that slot
constexpr uint32_t kDataIdMax = 0x3ff;      // a data id is 1..this; any other argument asks for "any follower" (0x1273a0)
constexpr uint32_t kAbilityMax = 0x3f;      // an ability is 1..this (0x127480)
constexpr uint64_t kListIds = 0, kListCount = 8;
constexpr uint64_t kListMost = 64;          // a count above this is no list (the game's lists hold 5 slots, 4 for its join queue)

bool follower_id(uint16_t id) { return !(id & 0x800) && static_cast<uint32_t>((id >> 12) - 1) <= 3; }

// Calls fn(guest address of a follower's u16 actor id) for every follower in party list `list`; stops at the first true and
// returns it.
template <class Fn>
bool walk_list(uint64_t list, Fn&& fn) {
    const uint64_t pg = guest::deref(base() + layout::kPartyManager);
    const uint64_t at = pg + layout::kPartyLists + list * layout::kPartyListSize;
    if (!pg || !guest::ok(at, layout::kPartyListSize)) return false;
    const uint64_t ids = guest::rd<uint64_t>(at + kListIds);
    const uint64_t count = guest::rd<uint64_t>(at + kListCount);
    if (count > kListMost || !guest::ok(ids, count * 2)) return false;
    for (uint64_t i = 0; i < count; ++i)
        if (follower_id(guest::rd<uint16_t>(ids + i * 2)) && fn(ids + i * 2)) return true;
    return false;
}

// Calls fn(guest address of a follower's u16 actor id, the list is this player's own) for every follower this process knows, own
// list first; stops at the first true and returns it.
template <class Fn>
bool any_follower(Fn&& fn) {
    if (walk_list(layout::kPartyOwnList, [&](uint64_t id) { return fn(id, true); })) return true;
    for (uint64_t slot = 0; slot < layout::kPartySlotLists; ++slot)
        if (walk_list(slot, [&](uint64_t id) { return fn(id, false); })) return true;
    return false;
}

}  // namespace

bool follows(::Ctx* c, uint32_t data, bool* own) {
    const bool any = ((data - 1) & 0xffff) >= kDataIdMax;
    return any_follower([&](uint64_t id, bool mine) {
        if (!any && (!(guest::call(c, base() + kResident, id) & 1) ||
                     (guest::call(c, base() + kDataIdOf, id) & 0xffff) != (data & 0xffff)))
            return false;
        *own = mine;
        return true;
    });
}

bool owns(::Ctx* c, uint64_t comp) {
    if (!quests::member_rules() || comp == base() + layout::kNoComponent) return false;
    // The game's "online in the host's world" state, the one its own tests read before their role tests.
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!gm || !guest::ok(gm + layout::kStateOwnBag, 1) || (guest::rd<uint8_t>(gm + layout::kStateOwnBag) & 1)) return false;
    if (guest::call(c, base() + layout::kCloudIsland, 0) & 1) return false;
    return guest::call(c, base() + layout::kHasParents, comp) & 1;
}

size_t invited(uint16_t* ids, size_t most) {
    size_t n = 0;
    any_follower([&](uint64_t id, bool own) {
        if (!own && n < most) ids[n++] = guest::rd<uint16_t>(id);
        return false;
    });
    return n;
}

size_t followers_of(uint8_t slot, uint16_t* ids, size_t most) {
    size_t n = 0;
    if (slot < layout::kPartySlotLists)
        walk_list(slot, [&](uint64_t id) {
            if (n < most) ids[n++] = guest::rd<uint16_t>(id);
            return false;
        });
    return n;
}

bool follower_ability(::Ctx* c, uint32_t ability, bool* own) {
    const uint8_t want = static_cast<uint8_t>(ability);
    if (static_cast<uint8_t>(want - 1) >= kAbilityMax) return false;
    return any_follower([&](uint64_t id, bool mine) {
        if (!(guest::call(c, base() + kResident, id) & 1)) return false;
        for (uint64_t slot = 0; slot < 2; ++slot) {
            if (static_cast<uint8_t>(guest::call(c, base() + kAbilityOf, id, slot)) != want) continue;
            *own = mine;
            return true;
        }
        return false;
    });
}

}  // namespace nxrt::games::pkp::coop::party
