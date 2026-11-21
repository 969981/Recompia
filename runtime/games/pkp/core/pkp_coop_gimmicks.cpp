// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a gimmick held by a member that left (pkp_coop_internal.h; RE: modding-docs coop-v2/GIMMICK-BUSY-RE.md).
//
// A gimmick (a placed object that one player uses at a time, the lighthouse for one) keeps the member index of its user in an owner
// byte; 0xff is free. Using it takes the byte (Link Play's RequestGimmickOwnership on a member) and the end of the use frees it. A
// member that leaves the session in the middle of a use never frees it, and nothing in the game does that for it: every other player
// then gets "Someone else is using this right now." there until the world is loaded again. Here, once a member left, every process
// frees each owner byte that names a member index no player character has any more, through the gimmick's own setter. The game removes
// the leaver's character on its own (MemberDelete), not at once with the co-op channel's leave, so the sweep runs once a second for a
// while after a leave.
//
// The game, 2.0.1a:
//   [main+0x35616f0] + 0x46d0         the placed-object map (libc++ std::map, key the packed cell u64): the root; a node is +0 left,
//                                     +8 right, +0x20 key, +0x28 the object (0x753200 walks it). The object's vt+0x310 gives its
//                                     gimmick part (the base 0x9d0728 returns the object itself), or 0
//   gimmick vt+0x50                   its kind (base 0x9d05d4: u16 +0xa & 0x3ff)
//   gimmick vt+0x328                  1: one owner slot (the base 0x9d0a38); otherwise slots 0..2
//   gimmick vt+0x2c8(g, slot)         the owner byte of a slot (base 0x7689ac: u8 +0x20 + slot), 0xff free or out of range
//   gimmick vt+0x2e0(g, &owner, quiet, slot)  sets it (base 0x768a60): same value returns at once; with quiet clear the host also
//                                     reports the cell (0x3f4344). Called here with quiet set, as the network apply 0x538f60 does
//   player manager +0x30..+0x48       the session's player characters; u8 ch+0x5a38 is the member index the owner byte holds
//
// Multi-user objects (kind table row byte +0x11 = 2: class 0x3262c80, per-member bytes obj+0x3d+idx; 0x13: class 0x3263160,
// obj+0x58+idx, each the only vtable with its class's own slots) are not the sweep's: the host's grant (0x552e24 / 0x55309c) takes
// the owner byte with the HOST's index and sets the member's byte, so the owner never names the leaver. The game cleans these up
// itself: MemberDelete 0x535c80 (the authority's side of a leave, run when the game deletes the member, so a use survives a short
// drop exactly as long as the sweep's single-user ones do) calls 0x77ece0(placed map, idx), which calls vt+0x228(obj, idx) on every
// placed object; the base 0x7668b4 is `ret`, the two classes' 0x74cc60 / 0x7568f0 clear the byte on the host (0xa40bc) and, when
// all four are clear, release the owner (0x538f60 take 0, quiet 0, slot 0xff: the cell is reported to the others). What that step
// lacks against the host's end-of-use handler (Sync take 0, 0x54cba4: 0x54cd08..0x54cd30 / 0x54ce38..0x54ce60) is
// 0x74d5e0(vt+0x310 part, 0, idx) -> 0x2c7d44 (not on a visitor 0x376c4): category 2 -> 0x2c83a0, 0x13 -> 0x2c7dfc = 0x2c83a0 then
// obj+0x4c+idx*2 = the empty id (0x7599d8). 0x2c83a0 lets go every character the player manager [main+0x3561928] lists (+0x2b8,
// count +0x2c4) whose vt+0x408 part +0x36f8 byte +0x10 is that index (0xff, its spot cleared, character event 0x1d). Without it the
// companion the leaver had at the object stays bound to the gone index, and 0x2c7f5c never binds a character whose byte is < 4
// (0x2c8264..0x2c8290) to anyone again. on_shared_leave runs that call at the game's own step, once per object the leaver was using.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::gimmicks {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kPlaced = 0x35616f0, kPlacedRoot = 0x46d0;
constexpr uint64_t kNodeLeft = 0, kNodeRight = 8, kNodeObject = 0x28;
constexpr uint64_t kKindOf = 0x50, kGimmickOf = 0x310, kOneSlot = 0x328, kOwnerOf = 0x2c8, kSetOwner = 0x2e0;   // vtable +
constexpr uint64_t kPlayerSlots = 4, kNetSlot = 0x5a38;
constexpr uint8_t kSlots = 3, kFree = 0xff, kMembers = 4;
constexpr size_t kMaxNodes = 0x10000;   // a walk that long is a broken tree, not a world

constexpr uint64_t kSharedLeave2 = 0x74cc60;     // vt+0x228 of class 0x3262c80 (category 2); else 0x7568f0, class 0x3263160 (0x13)
constexpr uint64_t kUsers2 = 0x3d, kUsers13 = 0x58;   // object +: u8 per member index, set while that member uses it
constexpr uint64_t kEndUse = 0x74d5e0;           // (part, on, idx): the per-member step of a use (0 = its end)

constexpr double kSweepFor = 10.0;      // after a leave: the game's MemberDelete removes the character within this
constexpr double kSweepEvery = 1.0;

struct State {
    double until = 0.0;   // sweeps run until then (0: none due)
    double next = 0.0;
    alignas(8) uint8_t free = kFree;   // the owner the setter reads (host memory is guest memory)
};
State& st() {
    static State s;
    return s;
}

// Bit i: a player character with member index i is in this world.
uint32_t present() {
    const uint64_t pm = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    if (!guest::ok(pm, layout::kManagerPlayer + kPlayerSlots * 8)) return 0;
    uint32_t bits = 0;
    for (uint64_t i = 0; i < kPlayerSlots; ++i) {
        const uint64_t ch = guest::rd<uint64_t>(pm + layout::kManagerPlayer + i * 8);
        uint8_t slot = kFree;
        if (ch && guest::read(ch + kNetSlot, slot) && slot < kMembers) bits |= 1u << slot;
    }
    return bits;
}

uint64_t virt(uint64_t obj, uint64_t slot) {
    const uint64_t vt = guest::deref(obj);
    return vt ? guest::deref(vt + slot) : 0;
}

// Frees the owner slots of one gimmick that name a member with no character; the count freed.
int free_orphans(::Ctx* c, uint64_t g, uint32_t here) {
    const uint64_t one = virt(g, kOneSlot), owner_of = virt(g, kOwnerOf), set_owner = virt(g, kSetOwner);
    if (!one || !owner_of || !set_owner) return 0;
    const uint8_t slots = (guest::call(c, one, g) & 0xff) == 1 ? 1 : kSlots;
    int freed = 0;
    for (uint8_t slot = 0; slot < slots; ++slot) {
        const uint8_t owner = static_cast<uint8_t>(guest::call(c, owner_of, g, slot));
        if (owner >= kMembers || (here & (1u << owner))) continue;   // free (0xff), or its member is here
        guest::call(c, set_owner, g, reinterpret_cast<uint64_t>(&st().free), 1, slot);
        const uint64_t kind_of = virt(g, kKindOf);
        NXLOG_I(kTag, "Co-op gimmicks: a kind %u gimmick (slot %u) was held by member %u, who is gone; freed",
                kind_of ? static_cast<unsigned>(guest::call(c, kind_of, g) & 0xffff) : 0u, slot, owner);
        ++freed;
    }
    return freed;
}

void sweep(::Ctx* c) {
    const uint64_t mgr = guest::deref(base() + kPlaced);
    if (!mgr || !guest::ok(mgr + kPlacedRoot, 8)) return;
    const uint32_t here = present();
    if (!here) return;   // no character read: the world is not up
    std::vector<uint64_t> todo;
    if (const uint64_t root = guest::rd<uint64_t>(mgr + kPlacedRoot)) todo.push_back(root);
    size_t seen = 0;
    while (!todo.empty() && ++seen <= kMaxNodes) {
        const uint64_t node = todo.back();
        todo.pop_back();
        if (!guest::ok(node, kNodeObject + 8)) continue;
        if (const uint64_t l = guest::rd<uint64_t>(node + kNodeLeft)) todo.push_back(l);
        if (const uint64_t r = guest::rd<uint64_t>(node + kNodeRight)) todo.push_back(r);
        const uint64_t obj = guest::rd<uint64_t>(node + kNodeObject);
        const uint64_t gimmick_of = obj && guest::ok(obj, 8) ? virt(obj, kGimmickOf) : 0;
        const uint64_t g = gimmick_of ? guest::call(c, gimmick_of, obj) : 0;
        if (g && guest::ok(g, 0x24)) free_orphans(c, g, here);
    }
}

}  // namespace

void on_member(int, bool joined, bool self) {
    if (joined || self) return;
    st().until = qpc::seconds() + kSweepFor;
    st().next = 0.0;
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!s.until) return;
    const double now = qpc::seconds();
    if (now > s.until || !link::in_session()) {
        s.until = 0.0;
        return;
    }
    if (!in_game || now < s.next) return;
    s.next = now + kSweepEvery;
    sweep(c);
}

void reset() { st() = State{}; }

int on_shared_leave(::Ctx* c, uint64_t site) {
    if (!link::in_session() || !link::is_authority()) return 0;
    const uint64_t obj = c->x[0];
    const uint8_t idx = static_cast<uint8_t>(c->x[1]);
    const uint64_t bytes = site == kSharedLeave2 ? kUsers2 : kUsers13;
    if (idx >= kMembers || !obj || !guest::ok(obj + bytes, kMembers)) return 0;
    if (!guest::rd<uint8_t>(obj + bytes + idx)) return 0;   // that member was not using this object
    const uint64_t gimmick_of = virt(obj, kGimmickOf);
    const uint64_t part = gimmick_of ? guest::call(c, gimmick_of, obj) : 0;
    if (!part) return 0;
    guest::call(c, base() + kEndUse, part, 0, idx);
    NXLOG_I(kTag, "Co-op gimmicks: member %u left while using a shared object (category %s); its character there let go", idx,
            site == kSharedLeave2 ? "2" : "0x13");
    return 0;
}

}  // namespace nxrt::games::pkp::coop::gimmicks
