// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a gimmick whose use did not start is given back (pkp_coop_internal.h; RE: modding-docs
// coop-v2/role-census/build/owner.md).
//
// A gimmick (a placed object one player uses at a time) keeps its user's member index in an owner byte, 0xff free; every other
// player's press then shows "Someone else is using this right now." (0x10d). A use takes the byte first (the host at once, a member
// through RequestGimmickOwnership and the grant's apply), then posts the press to the contents scheduler (0x765b54: 0x135d80 posts
// notice kind 0xe to GM+0xd8's list +0xb6530 through 0x1353a0, true when a content record takes the press, a story step waiting for
// it; 0x765b54 then answers false and the use does not start). Otherwise the class starts its event, and gives the byte back when that
// start fails. In eleven classes (thirteen vtables) the "a content took it" answer returns without a give-back, and nothing gives it
// back later: the presser keeps the object for the rest of the session, on every process (the grant reached them all). Here, in a
// session, at the `tbz w0` after that call, a false answer gives the byte back as the class's failed start does: the class's release
// function where it has one, else the base gimmick release the class itself calls at its other blocked exits (vt+0x490), else the
// game's give-back for one owner slot (the host applies the release; a member sends SyncGimmickOwnership, take 0, to the host, which
// applies it and reports the cell, as at a grant). A presser that does not own the object changes nothing: the release checks the
// owner against the presser's index (and at the seven later sites we check it first, since some of them reach the press with no take).
// The other callers of 0x7659c8 / 0x765b54 take nothing before the press (no take in the function, none reached from its class's
// own slots), so their false exit holds nothing: 0x79602c (0x3264ce0), 0x7b06fc (0x3266e68), 0x7cc490 / 0x7f6b48 (0x326a358 /
// 0x326efe0, no owner slot: vt+0x328 = 0), 0x7d5088 (0x326b918), 0x7e8578 (0x326e1b8: the use is the press alone), 0x77ce64 (the A
// press's walk of the placed-object map) and 0x78f82c / 0x78f890 (a script's start of a category 0xc / 0x18 gimmick). 0x74b484, the
// shared tick 0x74b3d0 (also called by 0x74aee8 and 0x754684), is left alone: its false exit hands on to the class (vt+0x4b8(g, 0)).
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
//   classes with the base take vt+0x488 -> vt+0x478 0x767700 (online: free -> host 0x538f60 take slot 0xff and its answer, member
//   the ownership request and 0; not free -> vt+0x2f0 owner == own; offline 1), then vt+0x400, then 0x7659c8:
//     0x3268000 slot 5 0x7b9460 `tbz` 0x7b9b0c; its slot 22 0x7b7860 (the host's deferred use, flag +0x51) `tbz` 0x7b80d0
//     0x32762f8 slot 5 0x82b4e0 `tbz` 0x82b8e4
//   classes that take inline (host 0x538f60 take slot 0xff, member RequestGimmickOwnership; owner == own -> the press):
//     0x326d7e0 slot 5 0x7e3fa0 `tbz` 0x7e469c; 0x3271340 slot 5 0x808640 `tbz` 0x808d68 (also reached with no take: offline,
//     0xa996dc false, cell flag bit 31); 0x3278b08 slot 5 0x83ca20 `tbz` 0x83d0b0
//     0x327cf68 / 0x327d8d8 / 0x327e248 slot 150 0x85e600 `tbz` 0x85eae0; their release is slot 160 0x85da94(g): host 0x538f60
//     take 0 slot 0xff, member Sync forced when 0x25af7c (the class calls it at the end of a use, 0x85ec3c)
//   all of these are one-slot classes (vt+0x328 0x76841c / 0x9d0a38 = 1); vt+0x2c8 0x75256c (s8 +0x20 >> 5, 0xff free) or 0x7689ac
//   vt+0x490(g, force, 6, 0)          the class's own give-back at its blocked exits (0x7b9a4c, 0x82b7ac, 0x808994, 0x83cd94, with
//                                     force = 0x25af7c = NM+0xb3348): 0x768650 / 0x769300 -> vt+0x480 0x76792c: needs vt+0x2f0;
//                                     host 0x538f60 take 0 quiet 1 slot 0xff + 0x3f4344 cell report; member Sync take 0 slot 0xff,
//                                     sent forced when w1, 0x2cb584 else; w2 = 6 adds no second report (> 5); w3 = 0: the Sync, not
//                                     the Link Play queue record (0x7e42fc passes 1 there, inside a visitor-only branch)
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

constexpr uint64_t kOwnerOf = 0x2c8, kBaseRelease = 0x490;   // gimmick vtable +
constexpr uint64_t kReleaseReason = 6;                       // as the classes pass it: no second cell report

enum class Slot : uint8_t { Reg, Byte57, Any };
enum class How : uint8_t {
    Fn,       // the class's release (g, slot)
    Base,     // vt+0x490(g, force, 6, 0), once the presser is the owner (one-slot classes)
    OneSlot,  // the one-slot give-back below
};
struct UseSite {
    uint64_t at;        // the `tbz w0, #0` after the history call (main offset)
    uint64_t release;   // How::Fn: the class's release
    Slot slot;
    How how;
    bool mine_first;    // give back only when the presser owns slot 0: the site is also reached with no take
};
constexpr UseSite kUses[] = {
    {0x7c141c, 0x7c1690, Slot::Reg, How::Fn, false},       // class 0x32688e0
    {0x79d558, 0x79d7a0, Slot::Reg, How::Fn, false},       // class 0x3265b20
    {0x848eac, 0, Slot::Byte57, How::OneSlot, false},      // class 0x327a3b8
    {0x79b178, 0, Slot::Any, How::OneSlot, false},         // class 0x3265668
    {0x7e78f4, 0, Slot::Any, How::OneSlot, false},         // class 0x326dc98
    {0x7b9b0c, 0, Slot::Any, How::Base, true},             // class 0x3268000 (its use)
    {0x7b80d0, 0, Slot::Any, How::Base, true},             // class 0x3268000 (the host's deferred use)
    {0x82b8e4, 0, Slot::Any, How::Base, true},             // class 0x32762f8
    {0x7e469c, 0, Slot::Any, How::Base, true},             // class 0x326d7e0
    {0x808d68, 0, Slot::Any, How::Base, true},             // class 0x3271340
    {0x83d0b0, 0, Slot::Any, How::Base, true},             // class 0x3278b08
    {0x85eae0, 0x85da94, Slot::Any, How::Fn, true},        // classes 0x327cf68, 0x327d8d8, 0x327e248 (release (g))
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
    if (u.mine_first) {
        const uint64_t owner_of = guest::deref(guest::deref(g) + kOwnerOf);
        if (!owner_of) return 0;
        const uint8_t own = static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0));
        if (static_cast<uint8_t>(guest::call(c, owner_of, g, 0)) != own) return 0;   // nothing taken, or someone else's
    }
    switch (u.how) {
        case How::Fn: guest::call(c, base() + u.release, g, slot); break;
        case How::Base: {
            const uint64_t release = guest::deref(guest::deref(g) + kBaseRelease);
            if (!release) return 0;
            const uint64_t force = guest::call(c, base() + layout::kSendGate, 0) & 1;
            guest::call(c, release, g, force, kReleaseReason, 0);
            break;
        }
        case How::OneSlot: give_back(c, g, slot); break;
    }
    if (!st().logged[i]) {
        st().logged[i] = true;
        const uint64_t kind_of = guest::deref(guest::deref(g) + kKindOf);
        NXLOG_I(kTag, "Co-op owner: a press at main+0x%llx went to a story content, not a use; the kind %u object (slot %u) is given back",
                static_cast<unsigned long long>(site), kind_of ? static_cast<unsigned>(guest::call(c, kind_of, g) & 0xffff) : 0u, slot);
    }
    return 0;
}

void reset() {
    for (bool& b : st().logged) b = false;
}

}  // namespace nxrt::games::pkp::coop::owner
