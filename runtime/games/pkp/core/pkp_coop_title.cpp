// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: a co-op start goes past the title screen on its own (pkp_coop_internal.h), and the co-op save never shows the game's
// "did not disconnect correctly" message.
//
// The save load sequence 0x245740 (vtable 0x322ef08) loads the save in its states 2..3, opens the title in state 4, turns its input on
// in state 6, then polls the title's result 0xa88214() in state 7 at 0x245a24 (its only caller) every frame until it is 0..3; result 1
// is "continue": state 9 then reads the home stage and the save's flags and state 0xa files the trip home (modding-docs coop-v2/P0-RE.md
// P0RE-9). The hook is that getter's entry, so the save is already loaded at every call. The title object (UI class 0x65, found as
// 0xa88214 finds it) is waiting for a press while its input is on (u8 +0xb8), its state (+0xa4) is 1 and its result (+0xa8) is still -1;
// there the hook writes the result 1, as a press of A followed by the title's own animation would (0xf714ac -> title state 2 -> +0xa8 =
// 1 at 0xf6fcb0), and the getter, which runs on, returns it. Run P0RE-9 made this very write while the title waited and the saved world
// loaded with no button pressed. A save whose player name still needs the name entry (0xf71250, the test 0xf714ac makes before it
// continues) is left at the title: a press there runs the game's name entry first. A new game never reaches the hook: with no save the
// load sequence starts the opening from its state 0 without the title.
//
// The message: save root + 0xa0 bit 1 is a copy of the CMNDAT header bit "the last save was written online" (P0RE-10), and every
// autosave in a session is written online. State 9 reads it after state 7 (0x245c18) into the trip home, and the stage loader then shows
// #2324 0x8b. Cleared here, in memory only, so the co-op save loads without it.
//
// Phones (facts::kPhone) keep the title for a press: there the written result left the title's layer up after the load (its key guide
// stayed on screen in the house, every menu the player opened closed again at once and Leave never fired, so the player never got
// outside and the session never started; Pixel 7a, alpha 14 and 16, the same save and group a desktop visitor played). One press of A
// runs the title's own path, as before alpha 14. The mark is still cleared on phones.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/platform_facts.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::title {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint32_t kWaiting = 1;            // the title's state while it waits for a press
constexpr uint32_t kNoResult = 0xffffffffu;
constexpr uint32_t kContinue = 1;

struct State {
    bool cleared = false;   // the unclean-exit bit was found set and cleared (logged once)
    bool passed = false;    // the title was answered (logged once)
};
State& st() {
    static State s;
    return s;
}

// The title object, as 0xa88214 finds it: the UI list of the owner *(main+kUiOwner) (+kUiList entries, +kUiCount their number), entry
// kTitleClass (entry 0 when the list is not longer than that), then two pointers in; 0 when any link is missing.
uint64_t title_object() {
    const uint64_t owner = guest::rd<uint64_t>(base() + layout::kUiOwner);
    if (!guest::ok(owner, layout::kUiCount + 8)) return 0;
    const uint64_t list = guest::rd<uint64_t>(owner + layout::kUiList);
    const uint64_t count = guest::rd<uint64_t>(owner + layout::kUiCount);
    const uint64_t index = count > layout::kTitleClass ? layout::kTitleClass : 0;
    if (!guest::ok(list + index * 8, 8)) return 0;
    const uint64_t entry = guest::rd<uint64_t>(list + index * 8);
    if (!guest::ok(entry, 8)) return 0;
    const uint64_t holder = guest::rd<uint64_t>(entry);
    if (!guest::ok(holder, 16) || !guest::rd<uint64_t>(holder + 8)) return 0;
    const uint64_t inner = guest::rd<uint64_t>(holder);
    if (!guest::ok(inner, 8)) return 0;
    const uint64_t obj = guest::rd<uint64_t>(inner);
    return guest::ok(obj, layout::kTitleInput + 1) ? obj : 0;
}

void clear_unclean_exit() {
    const uint64_t save = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (!guest::ok(save, layout::kSaveFlags + 1)) return;
    const uint8_t flags = guest::rd<uint8_t>(save + layout::kSaveFlags);
    if (!(flags & layout::kSaveUncleanExit)) return;
    guest::wr<uint8_t>(save + layout::kSaveFlags, flags & ~layout::kSaveUncleanExit);
    State& s = st();
    if (!s.cleared) NXLOG_I(kTag, "Co-op title: the save's \"did not disconnect correctly\" mark is cleared (the co-op save is written online)");
    s.cleared = true;
}

}  // namespace

int on_past_title(::Ctx* c) {
    clear_unclean_exit();
    if constexpr (facts::kPhone) return 0;   // a press of A continues (see the top of the file)
    const uint64_t obj = title_object();
    if (!obj || !guest::rd<uint8_t>(obj + layout::kTitleInput) || guest::rd<uint32_t>(obj + layout::kTitleState) != kWaiting ||
        guest::rd<uint32_t>(obj + layout::kTitleResult) != kNoResult)
        return 0;
    if (guest::call(c, base() + layout::kTitleNameMissing, 0) & 1) return 0;   // the name entry first: the player presses A
    guest::wr<uint32_t>(obj + layout::kTitleResult, kContinue);
    State& s = st();
    if (!s.passed) NXLOG_I(kTag, "Co-op title: the co-op save continues past the title with no press");
    s.passed = true;
    return 0;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::title
