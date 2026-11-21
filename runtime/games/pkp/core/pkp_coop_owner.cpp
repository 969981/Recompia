// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a gimmick whose use did not start is given back (pkp_coop_internal.h; RE: modding-docs
// coop-v2/role-census/build/owner.md).
//
// A gimmick (a placed object one player uses at a time) keeps its user's member index in an owner byte, 0xff free; every other
// player's press then shows "Someone else is using this right now." (0x10d). A use takes the byte first (the host at once, a member
// through RequestGimmickOwnership and the grant's apply), then posts the press to the contents scheduler (0x765b54: 0x135d80 posts
// notice kind 0xe to GM+0xd8's list +0xb6530 through 0x1353a0, true when a content record takes the press, a story step waiting for
// it; 0x765b54 then answers false and the use does not start). Otherwise the class starts its event, and gives the byte back when that
// start fails. In five classes the "a content took it" answer returns without a give-back, and nothing gives it back later: the
// presser keeps the object for the rest of the session, on every process (the grant reached them all). Here, in a session, at the
// `tbz w0` after that call, a false answer gives the byte back as the class's failed start does: the class's release function where
// it has one, else the game's give-back for one owner slot (the host applies the release; a member sends SyncGimmickOwnership, take 0,
// to the host, which applies it and reports the cell, as at a grant). A presser that does not own the object changes nothing: the
// release checks the owner against the presser's index.
//
// The game, 2.0.1a (x19 = the gimmick at every site, w0 = 0x765b54's answer):
//   class 0x32688e0 slot 5 0x7c0be0   the use at 0x7c13d8: 0x765b54 at 0x7c1418, `tbz w0, #0` 0x7c141c returns; a failed event
//                                     start (0x7c1838) releases with 0x7c1690(g, slot) (slot w21; +0x5d cleared, host 0x538f60
//                                     take 0, member Sync take 0 forced when 0x25af7c)
//   class 0x3265b20 slot 5 0x79cf40   the same shape: `tbz` 0x79d558, release 0x79d7a0(g, slot), slot w21
//   class 0x327a3b8 slot 5 0x848860   `tbz` 0x848eac returns; the failed start (0x848fb8) gives back inline at 0x848ebc: host
//                                     0x538f60(slot u8 g+0x57, take 0, quiet 0), member 0x74fd48 Sync sent with 0x30c69c. One slot
//   class 0x3265668 slot 5 0x79aa80   `tbz` 0x79b178 returns; 0x79b188 starts (0x79b7ec) or gives back with slot 0xff. One slot
//   class 0x326dc98 slot 5 0x7e73e0   its use 0x7e77a4: `tbz` 0x7e78f4 after 0x7659c8 (which returns 0x765b54's answer) returns; the
//                                     take there is slot 0xff. One slot (vt+0x328 = 1: slot 0xff is slot 0 for the setter)
//   0x538f60(&kind, x, y, z, &idx, take, quiet, slot)  the ownership apply: on the host a release needs owner == idx
//   0x74fd48(msg)                     SyncGimmickOwnership: +0x34 packed cell (x 12 bits, y 8, z 12), +0x38 kind, +0x9e member
//                                     index, +0x9f slot, +0xa0 take, +0xa1 quiet; the host's handler 0x54cba4 applies it (0x538f60)
//   0x30c69c(msg)                     the send to the host that passes the NM+0xb3348 send gate (0x2cb584 drops it there)
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::owner {

namespace {

constexpr const char* kTag = "pkp";
constexpr int kAnswerReg = 0, kGimmickReg = 19, kSlotReg = 21;
constexpr uint64_t kKindOf = 0x50;                      // gimmick vtable +: its kind (u16)
constexpr uint64_t kCellLo = 0x18, kCellHi = 0x1c;      // gimmick +: u32 x | y << 16, u16 z
constexpr uint64_t kMsgCell = 0x34, kMsgKind = 0x38, kMsgIndex = 0x9e, kMsgSlot = 0x9f, kMsgTake = 0xa0;
constexpr size_t kMsgBytes = 0x100;                     // the message is 0xb0 bytes (the game builds it on its stack)
constexpr uint8_t kAnySlot = 0xff;

enum class Slot : uint8_t { Reg, Byte57, Any };
struct UseSite {
    uint64_t at;        // the `tbz w0, #0` after the history call (main offset)
    uint64_t release;   // the class's release (g, slot), or 0: the one-slot give-back
    Slot slot;
};
constexpr UseSite kUses[] = {
    {0x7c141c, 0x7c1690, Slot::Reg},     // class 0x32688e0
    {0x79d558, 0x79d7a0, Slot::Reg},     // class 0x3265b20
    {0x848eac, 0, Slot::Byte57},         // class 0x327a3b8
    {0x79b178, 0, Slot::Any},            // class 0x3265668
    {0x7e78f4, 0, Slot::Any},            // class 0x326dc98
};
constexpr size_t kUseCount = sizeof kUses / sizeof kUses[0];

struct State {
    bool logged[kUseCount] = {};
    alignas(16) uint8_t msg[kMsgBytes] = {};   // guest addresses are host addresses
    alignas(8) uint16_t kind = 0;
    alignas(8) uint8_t index = 0;
};
State& st() {
    static State s;
    return s;
}

// The give-back of one owner slot as 0x848ebc..0x848fb0 makes it.
void give_back(::Ctx* c, uint64_t g, uint8_t slot) {
    State& s = st();
    const uint64_t vt = guest::deref(g);
    const uint64_t kind_of = vt ? guest::deref(vt + kKindOf) : 0;
    if (!kind_of) return;
    const uint32_t lo = guest::rd<uint32_t>(g + kCellLo);
    const uint16_t hi = guest::rd<uint16_t>(g + kCellHi);
    s.kind = static_cast<uint16_t>(guest::call(c, kind_of, g));
    s.index = static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0));
    if (guest::call(c, base() + layout::kIsHost, 0) & 1) {
        const uint64_t cell = lo | static_cast<uint64_t>(hi) << 32;
        guest::call_n<8>(c, base() + layout::kGimmickOwnerApply,
                         {reinterpret_cast<uint64_t>(&s.kind), lo, static_cast<uint32_t>(cell >> 16), hi,
                          reinterpret_cast<uint64_t>(&s.index), 0, 0, slot});
        return;
    }
    if (!(guest::call(c, base() + layout::kIsVisitor, 0) & 1)) return;
    std::memset(s.msg, 0, sizeof s.msg);
    const uint64_t m = reinterpret_cast<uint64_t>(s.msg);
    guest::call(c, base() + layout::kGimmickOwnerMsg, m);
    guest::wr<uint32_t>(m + kMsgCell, (lo & 0xfff) | ((lo >> 16) & 0xff) << 12 | static_cast<uint32_t>(hi) << 20);
    guest::wr<uint16_t>(m + kMsgKind, s.kind);
    guest::wr<uint8_t>(m + kMsgIndex, s.index);
    guest::wr<uint8_t>(m + kMsgSlot, slot);
    guest::wr<uint8_t>(m + kMsgTake, 0);
    guest::call(c, base() + layout::kSendToHostForced, m);
}

}  // namespace

int on_use_refused(::Ctx* c, uint64_t site) {
    if (c->x[kAnswerReg] & 1) return 0;   // no content took the press: the use goes on, the class keeps the object until its end
    if (!link::in_session()) return 0;
    size_t i = 0;
    while (i < kUseCount && kUses[i].at != site) ++i;
    const uint64_t g = c->x[kGimmickReg];
    if (i == kUseCount || !guest::ok(g, 0x60)) return 0;
    const UseSite& u = kUses[i];
    const uint8_t slot = u.slot == Slot::Reg      ? static_cast<uint8_t>(c->x[kSlotReg])
                         : u.slot == Slot::Byte57 ? guest::rd<uint8_t>(g + 0x57)
                                                  : kAnySlot;
    if (u.release) guest::call(c, base() + u.release, g, slot);
    else give_back(c, g, slot);
    if (!st().logged[i]) {
        st().logged[i] = true;
        NXLOG_I(kTag, "Co-op owner: a press at main+0x%llx went to a story content, not a use; the object (slot %u) is given back",
                static_cast<unsigned long long>(site), slot);
    }
    return 0;
}

void reset() {
    for (bool& b : st().logged) b = false;
}

}  // namespace nxrt::games::pkp::coop::owner
