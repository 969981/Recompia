// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a member's take-out removes from the store it listed (pkp_coop_internal.h; RE: modding-docs
// coop-v2/role-census/build/menus.md). Ordered Items (container kind 0x14, slots 80..89) and Gifts (kind 0x1a, slots
// 1298..1307) live in each player's own save, in its common container store (save root + 0x13d5970), as bags and coins do.
//
// The game, main 2.0.1a:
//   The take-out screen (UI class 0x80) lists and counts both from the own store (0x5b1c84 with w3 = 0) and takes from it through
//   0xe8d6a0 (take all) and 0xe8d920 (take one). Each calls the shared move 0x2ee774(player, &own slot, slot, 0, 1, kind, 0, ..), which
//   adds the item to the bag (0x2e70e0) and then removes the amount added from the source: at 0x2eea70 `bl IsVisitor`, 0x2eea74
//   `and w3, w0, #1`, then 0x5b1c84(.., kind w21, id, w3) at 0x2eea90 and the decrement of [x0 + slot * 16]. For a common-store kind
//   a set w3 makes 0x5b1c84 return the visitor's temp list (save root + 0x170f658) instead of the own store. On a member (a Link Play
//   visitor) the own slot was never emptied: the same items could be taken again, and the decrement landed in the temp list.
//   The visitor rule is right for the generic container screen, which lists a visitor's containers from that temp list (0xe6dc2c ->
//   0x5bc618 with IsVisitor) and moves through the same function (0xe6ad0c, 0xe6fc80). Nothing lists or writes kinds 0x14 / 0x1a
//   in the temp list: the screen's own reads, the third take 0xe8db80 (its move descriptor's temp byte is 0), the shop's order write
//   (0xe88e24) and the gift box write (0x492240 / 0x492664) all use the own store. Vanilla never reaches this case: the screen is
//   offered to the host or offline only.
// The midhook at 0x2eea74: w0 = IsVisitor's answer, read by the `and` alone. It is set to 0 (the own store) when the kind (w21, the
// move's w5) is 0x14 or 0x1a and 0x2ee774 was called by one of the two take-out calls (its frame record, x29 + 8, holds the return
// address: 0xe8d870 or 0xe8db18). Every other kind and caller keeps the game's answer. A non-visitor already gets the own store, so
// single player and the host see no change; on a member the own store is the one listed in every state (in or out of a session), so
// the hook asks nothing of the link.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"

#include <atomic>
#include <cstdint>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::takeout {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kFromTakeAll = 0xe8d870;   // 0xe8d6a0's `bl 0x2ee774` returns here
constexpr uint64_t kFromTakeOne = 0xe8db18;   // 0xe8d920's
constexpr uint8_t kOrdered = 0x14, kGifts = 0x1a;
constexpr int kKindReg = 21;                  // 0x2ee774 keeps its w5 (the source's container kind) in w21 to the end
constexpr int kFpReg = 29;
constexpr uint64_t kCallerReturn = 8;         // 0x2ee774's frame record (x29 = sp + 0x40): +8 the caller's x30

}  // namespace

int on_remove_store(::Ctx* c) {
    if (!(c->x[0] & 1)) return 0;   // not a visitor: 0x5b1c84 returns the own store already
    const uint8_t kind = static_cast<uint8_t>(c->x[kKindReg]);
    if (kind != kOrdered && kind != kGifts) return 0;
    const uint64_t rec = c->x[kFpReg] + kCallerReturn;
    if (!guest::ok(rec, 8)) return 0;
    const uint64_t from = guest::rd<uint64_t>(rec) - base();
    if (from != kFromTakeAll && from != kFromTakeOne) return 0;
    c->x[0] = 0;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        NXLOG_I(kTag, "Co-op take-out: %s taken out of this player's own box (%s); removed there, as listed",
                kind == kOrdered ? "an ordered item" : "a gift", from == kFromTakeAll ? "take all" : "take one");
    return 0;
}

}  // namespace nxrt::games::pkp::coop::takeout
