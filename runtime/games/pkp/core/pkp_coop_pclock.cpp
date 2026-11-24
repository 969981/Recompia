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
//
// A PC press that cannot open (modding-docs coop-v2/MEMBER-PC-WAIT-RE.md section 3): the press (0x7d4d80 -> 0x7d5418) takes the
// per-player lock of kind 0x10 and sets the object's +0x4c; its tick (0x7d41f4) opens the menu only while the Environment Level
// 0x58042c(stage) is 1..10, and nothing but the menu's own close or the object's cancel (vtable slot 48, 0x7d5b04) releases the
// lock. A member whose level is still 0 (the game's own fill is late) stood locked for 57 s. On a member, a pending press whose level
// stays 0 for kFillAfter seconds first gets the authority's levels filled in (pkp_coop_levels.cpp: the game's own tick then opens
// the menu); if it is still 0 after kCancelAfter, the object's own cancel releases the player. This does not depend on play mode:
// a player parked in the PC's menu is "not playing".
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
constexpr double kFillAfter = 2.0;     // a pending PC press whose level reads 0: the authority's levels are filled in
constexpr double kCancelAfter = 6.0;   // and still 0: the press is cancelled (unverified: chosen to leave the game's own fill time)

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
    uint64_t pc = 0;          // a member: the PC object last pressed (0: none)
    double waiting = 0.0;     // since when its press waits at level 0 (0: not)
    bool filled = false;      // the fill was tried for this wait
};
State& st() {
    static State s;
    return s;
}

// A member: the pending press of the PC object, if it still waits for the level.
bool pc_waiting(uint64_t pc) {
    if (!guest::ok(pc, layout::kPcPending + 1) || guest::rd<uint64_t>(pc) != base() + layout::kPcClass) return false;   // freed, or reused
    return guest::rd<uint8_t>(pc + layout::kPcPending) == 1 && guest::rd<uint8_t>(pc + layout::kPcMenuOpen) == 0;
}

// A member: ends the wait of a PC press that the level never opens.
void release_stuck_press(::Ctx* c, double now) {
    State& s = st();
    if (!s.pc || link::is_authority()) return;
    if (!pc_waiting(s.pc)) {
        if (!guest::ok(s.pc, 8) || guest::rd<uint64_t>(s.pc) != base() + layout::kPcClass) s.pc = 0;
        s.waiting = 0.0;
        s.filled = false;
        return;
    }
    if (guest::call(c, base() + layout::kLevelOf, session::area_stage()) & 0xff) {   // the game's own tick opens the menu now
        s.waiting = 0.0;
        s.filled = false;
        return;
    }
    if (!s.waiting) s.waiting = now;
    if (!s.filled && now - s.waiting >= kFillAfter) {
        s.filled = true;
        NXLOG_I(kTag, "Co-op PC menu: a press waits for the area's level, still 0 after %.0f s: the authority's levels are filled in (%zu)",
                kFillAfter, levels::fill_now());
    }
    if (now - s.waiting < kCancelAfter) return;
    guest::call(c, base() + layout::kPcCancel, s.pc);
    NXLOG_I(kTag, "Co-op PC menu: the press was cancelled here, the area's level was still 0 after %.0f s (pending flag now %u)", kCancelAfter,
            static_cast<unsigned>(guest::rd<uint8_t>(s.pc + layout::kPcPending)));
    s.pc = 0;
    s.waiting = 0.0;
    s.filled = false;
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

void on_press(::Ctx* c) {
    if (!link::in_session() || link::is_authority() || static_cast<uint32_t>(c->x[1]) != 4) return;
    const uint64_t pc = c->x[0];
    if (guest::ok(pc, 8) && guest::rd<uint64_t>(pc) == base() + layout::kPcClass) st().pc = pc;
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c) return;
    if (!link::in_session()) {
        s.pc = 0;
        s.waiting = 0.0;
        s.filled = false;
        return;
    }
    const double now = qpc::seconds();
    if (now - s.at < kEvery) return;
    s.at = now;
    release_stuck_press(c, now);
    if (!in_game) return;
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
