// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's PC menu locks (pkp_coop_internal.h). The story's lessons lock the PC menu to one tile; in a session a
// player can be behind the lesson (its tile not open yet, or the content that ends the lesson not armed), and then the menu cannot
// be closed. A lock that cannot end is ended here, the way the game ends its own. RE: modding-docs coop-v2/SHOP-UNLOCK-RE.md.
//
// The game, 2.0.1:
//   the lock byte    [main + layout::kUiOwner] + 0x1289: bit 0 (0xefd99c, the menu is busy), bit 1 (event command 0x57), bit 2
//                    (every tile but tile 0, the challenges), bit 3 (every tile but tile 1, the Shop). While bit 2 or 3 is set,
//                    0xefd380 does not close the menu. 0xeffba8 tests a bit, 0xefe124 sets it, 0xf01770(owner, n) clears it.
//   contents 0x852   (0x12ba1c) a = 0: bit 2, a = 1: bit 3; b == 1 sets it, otherwise clears it. Bit 3 is set by content 50 (#7148,
//                    quest 21) record 5, before its record 8 plays the Shop's intro, scene 77 (event 98). It is cleared by content 64
//                    (#7162, quest 35) record 1 on condition 0x60e (a kind-28 notice with payload 1), whose record 2 then queues scene
//                    114. Bit 2 likewise: content 49 (#7147, quest 20) sets it, its record on condition 0x60e == 0 clears it.
//   kind 28 notice   0x1364f4(SC, u8), from two panes only: the Shop pane 0xf0f870 (payload 1 at 0xf0f9d4, when lock bit 3 is set)
//                    and the challenges pane (payload 0 at 0xf056fc, when bit 2 is set). Opening the locked tile's pane is the
//                    lesson's end.
//   0xeff9a8(_, t)   a tile's ability gate through 0x377cc (jump table 0x283bbe6: tile 0 ability 27, tile 1 ability 30). A tile whose gate is shut cannot be opened, so a
//                    lock to it can never end by the game's own way.
//   0x43f2a4         event command 0x57 (bit 1's), the game's own clear: it waits while 0xa88630, bit 0 or 0xa87f88, then clears.
// Rules, on any process in a session, the lock's bit set:
//   - the Shop's lock only once its intro, scene 77, has played (before it the lesson is still starting);
//   - the locked tile's pane sent its notice and kAfterNotice seconds later the lock is still set (the content that ends it is
//     not armed here): cleared;
//   - on a member, the locked tile's gate stays shut for kShut seconds: cleared (the player could not leave the menu; the host's
//     lesson is the game's own);
//   - a clear waits as 0x43f2a4 does: while 0xa88630, bit 0 or 0xa87f88 holds.
// Content 64's own clear runs first whenever it is armed; its scene 114 still follows the notice.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::pclock {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kLockByte = 0x1289;        // the UI owner + : the PC menu's lock bits
constexpr uint64_t kClearLock = 0xf01770;     // (owner, n): clears bit n
constexpr uint64_t kTileGate = 0xeff9a8;      // (unused, tile) -> bit 0: the tile's ability gate is open
constexpr uint64_t kWaitA = 0xa88630;         // () -> bit 0: 0x43f2a4 waits while set
constexpr uint64_t kWaitB = 0xa87f88;         // () -> bit 0: likewise
constexpr uint64_t kPlayedFlags = 0x4a2cec;   // save root + : the played-scene bits (pkp_coop_events.cpp)
constexpr uint32_t kShopIntro = 77;           // scene 77 (event 98): the Shop's intro, content 50 record 8
constexpr double kAfterNotice = 5.0;
constexpr double kShut = 30.0;
constexpr double kEvery = 0.25;

struct Lock {
    uint8_t bit;
    uint8_t tile;
    uint32_t intro;   // a scene that must have played first (0: none)
};
constexpr Lock kLocks[] = {{2, 0, 0}, {3, 1, kShopIntro}};

struct State {
    double noticed[2] = {};   // when the pane's notice for kLocks[i] came (0: not since the lock was last clear)
    double shut[2] = {};      // since when its tile's gate is shut (0: open)
    double at = 0.0;
};
State& st() {
    static State s;
    return s;
}

bool played(uint32_t scene) {
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return root && guest::ok(root + kPlayedFlags + (scene >> 3), 1) &&
           (guest::rd<uint8_t>(root + kPlayedFlags + (scene >> 3)) & (1u << (scene & 7)));
}

}  // namespace

int on_pane_notice(::Ctx* c) {
    const uint8_t payload = static_cast<uint8_t>(c->x[1]);
    if (payload < 2 && link::in_session()) st().noticed[payload] = qpc::seconds();
    return 0;
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c || !in_game || !link::in_session()) return;
    const double now = qpc::seconds();
    if (now - s.at < kEvery) return;
    s.at = now;
    const uint64_t owner = guest::rd<uint64_t>(base() + layout::kUiOwner);
    if (!owner || !guest::ok(owner + kLockByte, 1)) return;
    for (size_t i = 0; i < 2; ++i) {
        const Lock& l = kLocks[i];
        const uint8_t bits = guest::rd<uint8_t>(owner + kLockByte);
        if (!(bits & (1u << l.bit))) {
            s.noticed[i] = s.shut[i] = 0.0;
            continue;
        }
        if (l.intro && !played(l.intro)) continue;   // the lesson's start: its lock is the game's until the intro has played
        const bool open = guest::call(c, base() + kTileGate, 0, l.tile) & 1;
        if (open) s.shut[i] = 0.0;
        else if (!s.shut[i]) s.shut[i] = now;
        const bool stuck = !open && now - s.shut[i] >= kShut && !link::is_authority();
        const bool unanswered = s.noticed[i] && now - s.noticed[i] >= kAfterNotice;
        if (!stuck && !unanswered) continue;
        if ((guest::call(c, base() + kWaitA, 0) & 1) || (bits & 1) || (guest::call(c, base() + kWaitB, 0) & 1)) continue;
        guest::call(c, base() + kClearLock, owner, l.bit);
        NXLOG_I(kTag, "Co-op PC menu: lock bit %u (all tiles but tile %u) cleared here: %s", l.bit, l.tile,
                stuck ? "the tile's gate stayed shut" : "the lesson's end did not clear it");
        s.noticed[i] = s.shut[i] = 0.0;
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::pclock
