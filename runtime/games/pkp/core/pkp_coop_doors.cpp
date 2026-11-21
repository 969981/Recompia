// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the session's doors a player can open from a menu (pkp_coop_internal.h). In a co-op session the launcher runs the
// Link Play session, and the group stays together until a player leaves with Save and Quit, so:
//   - the PC's Link Play screen makes no session call: every state of it that opens, searches, joins, closes or leaves a session (or
//     starts GameShare, goes online, or goes to a Cloud Island) is refused as the game refuses a request it cannot make, back to the
//     screen's first state, and a notice says why. The screen itself opens and closes as usual;
//   - the pause menu (UI class 0xba) never sends a teammate home or rejects one: only those two states are refused, the same way. The
//     pause menu's Save and Quit stays as it is: the host's closes the session for everyone (state 0xf, 0xbdc68), a teammate's leaves
//     it (state 0x10 with no member named, 0xbd45c reason 4), and the GameShare end (state 0x12) is untouched.
//
// The PC's Link Play screen (the multiplayer screen): 0xf266d0(screen, state) stores the state at +0xa4 (the old one at +0xa8) and calls
// 0xf282a8(screen), which makes the state's request on entry (the hook). The states whose entry makes a session call
// (modding-docs coop-session/LEAVE-DOORS-RE.md 2, the jump table at main+0x283bdcc): 9 / 0xa host, 0xb / 0xc GameShare start,
// 0xe..0x11 search, 0x12 / 0x13 join, 0x14 Close the Connection (0xbdc68), 0x15 Go Back Home (0xe800ac), 0x17 GameShare end, 0x22 a
// found session's Link Code, 0x27..0x2a online steps and the Cloud Island number, 0x2e..0x31 Cloud Island settings and trips, 0x3b..0x3d
// Cloud Island create / search / join, 0x40 / 0x42 / 0x43 the island list. When the game's own request refuses there (0xbdc68 at
// 0xf28508, 0xe800ac at 0xf2851c), the screen enters state 1 (0xf28740); state 1 re-reads the open request the screen was given and
// clears it as it takes it (+0x8 / +0xc of the record at +0xbc, 0xf28aa8..0xf28ae4), so a refusal there cannot repeat. A refusal made
// while the hook is already refusing goes to state 2 instead (the other refusal of the game's, 0xf289a4: no entry action at all).
//
// The pause menu: 0xfb8f58(menu, state, 1) stores the state at +0xa4 (the old one at +0xa8) and calls 0xfb9ea0(menu), the state's entry
// (the hook; jump table at main+0x283c3ee). u8 +0x108 is the member a choice names (0xff: none). State 0x10 files 0xbd45c(record,
// reason, member, 1): the host with a member named is reason 2, "send that member home" (0xfba5d4..0xfba5e8); state 0x19 rejects the
// member named (0xe7da08 at 0xfba7bc, which asks it to file kind 8 home). When 0xbd45c refuses, the menu enters state 1 (0xfba744:
// 0xfb8f58(menu, 1, 1)); both refusals here do the same.
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/log.h"
#include "nxrt/notices.h"

namespace nxrt::games::pkp::coop::doors {

namespace {

constexpr const char* kTag = "pkp";
constexpr const char* kNotice = "pkp.coop_doors";
constexpr double kNoticeSeconds = 5.0;

constexpr uint32_t kLinkFirstState = 1, kLinkPageState = 2;   // the screen's refusals: re-read the page (1), or just show it (2)
constexpr uint32_t kPauseFirstState = 1;
constexpr uint32_t kPauseFileTrip = 0x10, kPauseReject = 0x19;
constexpr uint8_t kNoMember = 0xff;

// The Link Play screen's states whose entry makes a session call (the header).
bool session_call(uint32_t state) {
    switch (state) {
        case 0x9: case 0xa: case 0xb: case 0xc:
        case 0xe: case 0xf: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x17:
        case 0x22: case 0x27: case 0x28: case 0x29: case 0x2a:
        case 0x2e: case 0x2f: case 0x30: case 0x31:
        case 0x3b: case 0x3c: case 0x3d: case 0x40: case 0x42: case 0x43:
            return true;
        default:
            return false;
    }
}

struct State {
    bool refusing = false;   // inside the Link Play screen's refusal (its own state entry runs again from there)
};
State& st() {
    static State s;
    return s;
}

}  // namespace

int on_link_menu(::Ctx* c) {
    const uint64_t screen = c->x[0];
    if (!link::in_session() || !guest::ok(screen, layout::kLinkMenuState + 4)) return 0;
    const uint32_t state = guest::rd<uint32_t>(screen + layout::kLinkMenuState);
    if (!session_call(state)) return 0;
    State& s = st();
    const uint32_t back = s.refusing ? kLinkPageState : kLinkFirstState;
    NXLOG_I(kTag, "Co-op doors: the Link Play screen's state 0x%x (a session call) is refused in the co-op session, back to state %u", state,
            back);
    notices::post(kNotice, strings::get(base(), strings::Str::CoopDoorsLinkMenu), kNoticeSeconds);
    s.refusing = true;
    guest::call(c, base() + layout::kLinkMenuEnter, screen, back);
    s.refusing = false;
    return 1;
}

int on_pause_menu(::Ctx* c) {
    const uint64_t menu = c->x[0];
    if (!link::in_session() || !guest::ok(menu, layout::kPauseMenuMember + 1)) return 0;
    const uint32_t state = guest::rd<uint32_t>(menu + layout::kPauseMenuState);
    const uint8_t member = guest::rd<uint8_t>(menu + layout::kPauseMenuMember);
    const bool send_home = state == kPauseFileTrip && member != kNoMember;   // only the host's "send this member home" names one
    if (!send_home && state != kPauseReject) return 0;
    NXLOG_I(kTag, "Co-op doors: the pause menu's %s member %u is refused in the co-op session", send_home ? "send-home of" : "rejection of",
            member);
    notices::post(kNotice, strings::get(base(), strings::Str::CoopDoorsSendHome), kNoticeSeconds);
    guest::call(c, base() + layout::kPauseMenuEnter, menu, kPauseFirstState, 1);
    return 1;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::doors
