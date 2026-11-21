// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a gimmick held with no use running is given back (pkp_coop_internal.h; RE: modding-docs
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
// A member's grant is the same job: the host's grant reaches the member's reply handler 0x539620, which takes the byte locally
// (0x538f60), applies the reply's data (vt+0x2f8) and only when that apply answers true runs the use (0x77e260 -> vt+0x340, the
// class's event handler; action 4 re-runs its slot 5 with no player). A false apply skips the use and nothing gives the byte back
// (the reply's other step, vt+0x430 on the reply's second cell, does nothing on a grant in these classes, and the handler's only
// give-back, 0x539680..0x539750, runs before the take). The game's apply answers false in these classes only: 0x765f00 (the
// online container snapshot apply; tailed by vt+0x2f8 of 0x3268000, 0x326d7e0, 0x32762f8, 0x3278b08, 0x3266040, 0x3272d50,
// 0x3274430, called first by the 0x327cf68 classes' 0x85fe9c) answers 0 when the object's container type (vt+0x270) is outside
// 1..0x1e or 0x753420 finds no container record of that type at the cell; 0x843e64 (0x32794c0) also when its +0x53 is clear;
// 0x757178 (0x3263160) when the own index is 0xff or above 3. All of them are one-slot classes. Here, at that `tbz`, a false apply
// gives the byte back with the data-less give-back below (not vt+0x490: its Sync carries the object's data, +0xa2 = 1, which
// the host would apply although this process never applied the host's snapshot), once the presser owns it. A use that runs is
// never reached here: the true answer goes on to the use.
//
// Class 0x326dc98 (slot 5 0x7e73e0, its use 0x7e77a4; a container that opens with an animation and then the container screen) runs
// nothing from a grant: its vt+0x2f8 is the base `return 1`, its vt+0x340 the base `ret` 0x9d0740, its vt+0x430 the base `ret`
// 0x9d07c0, and its per-frame slot 22 0x7e633c only goes on with a use that already started (the timer +0x50 the use sets, the
// screen flag +0x4c). The host's own press takes the byte at 0x7e7884 and falls into the use 0x7e78c4 at once; a member's sends
// the request at 0x7e7a84 and returns, and the grant then runs nothing. The member's next press fails the free check
// (vt+0x2d0 0x768f48: the owner is not 0xff) and, owning the object (vt+0x60 0x766800), returns quietly at 0x7e76d0: the member
// never uses it and everyone else gets 0x10d for good. Running the use for the member instead is not done: it opens the member's
// own copy of the container (0x7e6680 -> 0xa94b38) and its close 0x7e67c0 moves items out of it (0x5b2f34) on that copy alone,
// and nothing in the class's code gives the byte back after a use (no 0x538f60 / vt+0x490 / Sync in 0x7e5f54..0x7e8044 but the
// take). So the game's own use is not run here, at the same `tbz`: the grant goes to pkp_coop_deposit.cpp (below).
// What the class is: the one placed kind 0x1d9 (#956 category 0x4f; the factory 0x76c248 maps category 0x4f to slot 0x2d, filled
// by 0x76bcc0 with the creator table 0x32853b0, whose creator 0x9fd6b8 makes this class), container type 0x11 (0x773c80 into
// +0x4b). It is not a store: it is the story's materials drop with ten stages (+0x4a, table slot 0x144 = #1108; a row's +8..+0xa
// item categories and +0xc..+0xe counts, +0 / +4 scenes). Its close 0x7e67c0 goes on only when this process's container screen
// manager flags it (0xa94bbc at 0x7e6938: [0x3561780]+0x11d8 u8 +0x5a), checks the deposit against the row (0x78d050), queues the
// row's scenes for the local player (0x2e6ce0, the scene queue), steps +0x4a and the record R+0x73b258 (+0xc = R+0x73b264, which
// quest condition 9 reads), and only then takes the deposit out of the container (0x5b2f34). A member's use would make all of that
// on its own copies (the deposit in its copy of the container record, the stage in its own common block), and no request carries a
// deposit to the host: the class has no snapshot apply (vt+0x2f8 is the base) and sends nothing at its close, unlike the store
// chests (class 0x3268000's close 0x7b8960: host release 0x538f60 take 0, a member's kind 0x1a / 0x19 / 0xf record through the
// request queue 0x214974, which the host's handler checks against its copy of the member's bag and which releases the owner there,
// 0x221d80). So the grant is handed to pkp_coop_deposit.cpp (deposit::take_grant), which runs the use for the member, keeps its
// deposit in the member's own store, has the authority make the step and gives the object back after the use; only when it does
// not take the grant (an ask of this member's still waits) is the grant given back here as before.
// The host's own use takes the byte (0x7e7884: take 1, its own index) and nothing frees it again: not the close, not the class's
// per-frame slot 22 0x7e633c or the base gimmick tick 0x75cb60 it calls (no 0x538f60, vt+0x480 / +0x490 or Sync within five calls;
// the vt slots that tick calls are leaves here), and no reader of the character's gimmick cell ch+0x5a96 (the gimmick state
// 0x6ce540 and the player-mode entry 0x3ee8fc release categories 0x18, 0xc, 0x22, 0x43 only; 0x2de2a0 category 0xc). After the
// host used it once, a member's press there was refused for the rest of the session (0x10d once the host's take report reached
// it). Here, at the close's entry, on the authority in a session, the host gives the byte back as the store chest's close does
// (0x7b8b34: 0x538f60 take 0, quiet 0, slot 0xff), once it is the owner. Not covered: a use whose open finds no container record
// (0x7e6680 leaves +0x4c 0 and the close never runs).
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
//                                     take there is slot 0xff. One slot (vt+0x328 = 1: slot 0xff is slot 0 for the setter).
//                                     Its close 0x7e67c0(g) (x0 = the gimmick; called by slot 22 0x7e633c once the screen is
//                                     shut, and tailed by 0x7e6680 with w1 clear) frees nothing
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
//   0x539620(reply, granted, a, b)    the member's grant reply (x19 = reply: +0 packed cell, +0xc kind, +0x10 slot); at the `tbz w0`
//                                     0x539b44, x23 = the gimmick found at the cell with the reply's kind, w0 = vt+0x2f8's answer.
//                                     Kinds of categories 2 / 0x13 take with the host's index (NM+0xb3380), so the owner check
//                                     leaves them alone
//
// The tester log (rate-limited, kLinesPerMinute per kind of line, on every process in a session): every ownership apply 0x538f60
// (entry hook; a take or a release, on the host for its own and every member's, on a member for its granted takes) with the object's
// kind, cell, slot, the index it is for, the owner the object has before it (a take of an object someone else holds is a refusal),
// and this process's index; every "Someone else is using this right now." shown (entry hook 0xa8fba0, the message log's post,
// x1 = {u32 +0 0x44, u16 +8 0x10d} at every gimmick 0x10d site, e.g. 0x7b997c..0x7b998c), with the object this player pressed last
// (midhook 0x313eb4, the player's call of an object's interaction handler: x0 the object; a gimmick's busy line comes from inside
// that call). An object is named only once the placed-object lookup 0x753200([main+0x35616f0], x, y, z), the game's own lookup in
// the grant reply (0x539b00), finds a gimmick of that kind at its cell: then its vt+0x2c8 (owner of a slot) is safe to call. A
// member also logs each take the host turned down: entry hook 0x539620, the grant reply (x0 the reply, w1 bit 0 the grant; 0 goes
// to 0x539754, which never takes the byte and, for kind 0x93, tests the object's free check again; the take is 0x539adc, reached
// only with w1 set).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::owner {

namespace {

constexpr const char* kTag = "pkp";
constexpr int kAnswerReg = 0, kGimmickReg = 19, kSlotReg = 21;
constexpr int kReplyReg = 19, kGrantGimmickReg = 23;    // at 0x539b44
constexpr uint64_t kReplySlot = 0x10;                   // reply +: the granted slot (u8)
constexpr uint64_t kReplyKind = 0xc, kReplyCell = 0;     // reply +: the kind (u16), the packed cell (u32)
constexpr uint64_t kNoGrantUseClass = 0x326dc98;        // main +: the vtable of the class that runs no use from a grant
constexpr int kGimmickArg = 0;                          // at 0x7e67c0's entry: x0 = the gimmick
constexpr uint64_t kKindOf = 0x50;                      // gimmick vtable +: its kind (u16)
constexpr uint64_t kCellLo = 0x18, kCellHi = 0x1c;      // gimmick +: u32 x | y << 16, u16 z
constexpr uint64_t kMsgCell = 0x34, kMsgKind = 0x38, kMsgIndex = 0x9e, kMsgSlot = 0x9f, kMsgTake = 0xa0;
constexpr size_t kMsgBytes = 0x100;                     // the message is 0xb0 bytes (the game builds it on its stack)
constexpr uint8_t kAnySlot = 0xff;

constexpr uint64_t kOwnerOf = 0x2c8, kBaseRelease = 0x490;   // gimmick vtable +
constexpr uint64_t kReleaseReason = 6;                       // as the classes pass it: no second cell report

// The tester log.
constexpr int kLinesPerMinute = 30;
constexpr double kPressWindow = 1.0;                         // a busy line names the press this many seconds back at most
constexpr uint32_t kNoticeCategory = 0x44, kBusyText = 0x10d;
constexpr uint64_t kNoticeText = 8;                          // the notice's u16 text id
constexpr uint8_t kUnknown = 0xfe;                           // the owner could not be read (an owner index is 0..3 or 0xff)
struct Limit {
    double start = -1e9;
    int n = 0, left_out = 0;
};

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
    bool grant_logged[2] = {};   // [applied]
    bool end_logged = false;
    alignas(16) uint8_t msg[kMsgBytes] = {};   // guest addresses are host addresses
    alignas(8) uint16_t kind = 0;
    alignas(8) uint8_t index = 0;
    Limit takes, releases, busy, refusals;
    uint64_t press = 0;       // the object this player pressed last, and when
    double press_at = -1e9;
};
State& st() {
    static State s;
    return s;
}

// A member's give-back: SyncGimmickOwnership, take 0, sent to the host past the send gate, as 0x848ebc..0x848fb0 makes it.
void send_release(::Ctx* c, uint16_t kind, uint32_t cell, uint8_t slot) {
    State& s = st();
    if (!(guest::call(c, base() + layout::kIsVisitor, 0) & 1)) return;
    std::memset(s.msg, 0, sizeof s.msg);
    const uint64_t m = reinterpret_cast<uint64_t>(s.msg);
    guest::call(c, base() + layout::kGimmickOwnerMsg, m);
    guest::wr<uint32_t>(m + kMsgCell, cell);
    guest::wr<uint16_t>(m + kMsgKind, kind);
    guest::wr<uint8_t>(m + kMsgIndex, static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0)));
    guest::wr<uint8_t>(m + kMsgSlot, slot);
    guest::wr<uint8_t>(m + kMsgTake, 0);
    guest::call(c, base() + layout::kSendToHostForced, m);
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
    send_release(c, s.kind, (lo & 0xfff) | ((lo >> 16) & 0xff) << 12 | static_cast<uint32_t>(hi) << 20, slot);
}

// A rate-limited line of one kind: true while the minute's lines are not used up.
bool allow(Limit& l, const char* what) {
    const double now = qpc::seconds();
    if (now - l.start >= 60.0) {
        if (l.left_out) NXLOG_I(kTag, "Co-op owner: %d more %s lines in the last minute left out", l.left_out, what);
        l = Limit{now, 0, 0};
    }
    if (l.n < kLinesPerMinute) {
        ++l.n;
        return true;
    }
    ++l.left_out;
    return false;
}

// The gimmick at a cell when it has this kind (see the header), else 0.
uint64_t gimmick_at(::Ctx* c, uint16_t kind, uint32_t x, uint32_t y, uint32_t z) {
    const uint64_t map = guest::rd<uint64_t>(base() + layout::kPlacedObjects);
    if (!guest::ok(map, 8)) return 0;
    const uint64_t g = guest::call(c, base() + layout::kPlacedObjectAt, map, x, y, z);
    if (!guest::ok(g, 0x60)) return 0;
    const uint64_t kind_of = guest::deref(guest::deref(g) + kKindOf);
    if (!kind_of || static_cast<uint16_t>(guest::call(c, kind_of, g)) != kind) return 0;
    return guest::deref(guest::deref(g) + kOwnerOf) ? g : 0;
}

// The pressed object g when the lookup at its cell finds this very object (a placed gimmick), else 0. Nothing on g is called before.
uint64_t gimmick_at_object(::Ctx* c, uint64_t g, uint32_t x, uint32_t y, uint32_t z) {
    const uint64_t map = guest::rd<uint64_t>(base() + layout::kPlacedObjects);
    if (!guest::ok(map, 8)) return 0;
    if (guest::call(c, base() + layout::kPlacedObjectAt, map, x, y, z) != g) return 0;
    return guest::deref(guest::deref(g) + kKindOf) && guest::deref(guest::deref(g) + kOwnerOf) ? g : 0;
}

// The owner of a slot of a gimmick gimmick_at() found (slot 0xff: the one-slot classes' slot 0), kUnknown for none.
uint8_t owner_now(::Ctx* c, uint64_t g, uint8_t slot) {
    if (!g) return kUnknown;
    return static_cast<uint8_t>(guest::call(c, guest::deref(guest::deref(g) + kOwnerOf), g, slot == kAnySlot ? 0 : slot));
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

int on_grant_no_use(::Ctx* c) {
    if (!link::in_session() || link::is_authority()) return 0;
    const uint64_t g = c->x[kGrantGimmickReg], reply = c->x[kReplyReg];
    if (!guest::ok(g, 0x60) || !guest::ok(reply + kReplySlot, 1)) return 0;
    const bool applied = c->x[kAnswerReg] & 1;
    if (applied && guest::deref(g) != base() + kNoGrantUseClass) return 0;   // the use runs from 0x77e260
    const uint8_t slot = guest::rd<uint8_t>(reply + kReplySlot);
    const uint64_t owner_of = guest::deref(guest::deref(g) + kOwnerOf);
    if (!owner_of) return 0;
    const uint8_t own = static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0));
    if (static_cast<uint8_t>(guest::call(c, owner_of, g, slot)) != own) return 0;   // not taken with our index (categories 2 / 0x13)
    if (applied && deposit::take_grant(c, g)) return 0;   // the drop's use runs from pkp_coop_deposit.cpp
    give_back(c, g, slot);
    if (!st().grant_logged[applied]) {
        st().grant_logged[applied] = true;
        const uint64_t kind_of = guest::deref(guest::deref(g) + kKindOf);
        NXLOG_I(kTag, "Co-op owner: a grant started no use (%s); the kind %u object (slot %u) is given back",
                applied ? "the class runs none from a grant" : "its data did not apply",
                kind_of ? static_cast<unsigned>(guest::call(c, kind_of, g) & 0xffff) : 0u, slot);
    }
    return 0;
}

int on_host_use_end(::Ctx* c) {
    if (!link::in_session() || !link::is_authority()) return 0;
    const uint64_t g = c->x[kGimmickArg];
    if (!guest::ok(g, 0x60) || guest::deref(g) != base() + kNoGrantUseClass) return 0;
    const uint64_t owner_of = guest::deref(guest::deref(g) + kOwnerOf);
    if (!owner_of) return 0;
    const uint8_t own = static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0));
    if (static_cast<uint8_t>(guest::call(c, owner_of, g, 0)) != own) return 0;   // free, or not taken by the host's own use
    give_back(c, g, kAnySlot);
    if (!st().end_logged) {
        st().end_logged = true;
        NXLOG_I(kTag, "Co-op owner: the host's use of a kind 0x1d9 object ended; the object is given back");
    }
    return 0;
}

void give_back_cell(::Ctx* c, uint16_t kind, uint32_t cell) {
    if (link::in_session() && !link::is_authority()) send_release(c, kind, cell, kAnySlot);
}

int on_owner_apply(::Ctx* c) {
    if (!link::in_session()) return 0;
    const uint64_t kind_at = c->x[0], index_at = c->x[4];
    if (!guest::ok(kind_at, 2) || !guest::ok(index_at, 1)) return 0;
    const uint16_t kind = guest::rd<uint16_t>(kind_at);
    const uint8_t index = guest::rd<uint8_t>(index_at);
    const uint32_t x = c->x[1] & 0xfff, y = c->x[2] & 0xff, z = c->x[3] & 0xfff;   // as 0x539ad0..0x539ad8 unpack the cell
    const bool take = c->x[5] & 1, quiet = c->x[6] & 1;
    const uint8_t slot = static_cast<uint8_t>(c->x[7]);
    const uint8_t own = static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0));
    if (take && index == own && !link::is_authority()) linkqueue::on_take(c, kind, x | y << 12 | z << 20);
    State& s = st();
    if (!allow(take ? s.takes : s.releases, take ? "take" : "release")) return 0;
    const uint8_t before = owner_now(c, gimmick_at(c, kind, x, y, z), slot);
    const bool refused = take && before != kAnySlot && before != kUnknown && before != index;
    NXLOG_I(kTag, "Co-op owner: %s kind %u at (%u,%u,%u) slot %u for index %u%s (owner before %u%s; this process index %u, %s)",
            refused ? "take REFUSED" : take ? "take" : "release", kind, x, y, z, slot, index, quiet ? " quiet" : "", before,
            before == kAnySlot ? " free" : before == kUnknown ? " unknown" : "", own, link::is_authority() ? "host" : "member");
    return 0;
}

int on_grant_reply(::Ctx* c) {
    if (!link::in_session() || link::is_authority() || (c->x[1] & 1)) return 0;
    const uint64_t reply = c->x[0];
    if (!guest::ok(reply, kReplySlot + 1) || !allow(st().refusals, "refusal")) return 0;
    const uint32_t cell = guest::rd<uint32_t>(reply + kReplyCell);
    const uint16_t kind = guest::rd<uint16_t>(reply + kReplyKind);
    const uint8_t slot = guest::rd<uint8_t>(reply + kReplySlot);
    const uint32_t x = cell & 0xfff, y = (cell >> 12) & 0xff, z = cell >> 20;
    const uint8_t here = owner_now(c, gimmick_at(c, kind, x, y, z), slot);
    NXLOG_I(kTag, "Co-op owner: the host refused the take of kind %u at (%u,%u,%u) slot %u (owner here %u%s; this process index %u, "
            "member)", kind, x, y, z, slot, here, here == kAnySlot ? " free" : here == kUnknown ? " unknown" : "",
            static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0)));
    return 0;
}

int on_press(::Ctx* c) {
    if (!link::in_session()) return 0;
    st().press = c->x[0];
    st().press_at = qpc::seconds();
    return 0;
}

int on_notice(::Ctx* c) {
    if (!link::in_session()) return 0;
    const uint64_t n = c->x[1];
    if (!guest::ok(n, kNoticeText + 2) || guest::rd<uint32_t>(n) != kNoticeCategory || guest::rd<uint16_t>(n + kNoticeText) != kBusyText)
        return 0;
    State& s = st();
    if (!allow(s.busy, "busy")) return 0;
    const uint8_t own = static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0));
    const char* who = link::is_authority() ? "host" : "member";
    const uint64_t g = s.press;
    if (qpc::seconds() - s.press_at > kPressWindow || !guest::ok(g, 0x60)) {
        NXLOG_I(kTag, "Co-op owner: busy line shown, no recent press (this process index %u, %s)", own, who);
        return 0;
    }
    const uint64_t vt = guest::deref(g);
    const uint32_t lo = guest::rd<uint32_t>(g + kCellLo);
    const uint32_t x = lo & 0xfff, y = (lo >> 16) & 0xff, z = guest::rd<uint16_t>(g + kCellHi) & 0xfffu;
    // Named only when the lookup by its cell finds this very object: the press may have been on something that is not a gimmick, whose
    // vtable has no kind slot to call.
    const uint64_t found = gimmick_at_object(c, g, x, y, z);
    if (!found) {
        NXLOG_I(kTag, "Co-op owner: busy line shown after a press on class main+0x%llx at (%u,%u,%u), not a placed gimmick (this "
                "process index %u, %s)", static_cast<unsigned long long>(vt - base()), x, y, z, own, who);
        return 0;
    }
    NXLOG_I(kTag, "Co-op owner: busy line shown for kind %u (class main+0x%llx) at (%u,%u,%u), owner %u (this process index %u, %s)",
            static_cast<unsigned>(guest::call(c, guest::deref(vt + kKindOf), g) & 0xffff), static_cast<unsigned long long>(vt - base()),
            x, y, z, owner_now(c, g, 0), own, who);
    return 0;
}

void reset() {
    for (bool& b : st().logged) b = false;
    for (bool& b : st().grant_logged) b = false;
    st().end_logged = false;
    st().takes = st().releases = st().busy = st().refusals = Limit{};
    st().press = 0;
    st().press_at = -1e9;
}

}  // namespace nxrt::games::pkp::coop::owner
