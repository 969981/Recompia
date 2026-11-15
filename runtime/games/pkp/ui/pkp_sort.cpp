// SPDX-License-Identifier: GPL-3.0-or-later
// Inventory sort (#323): the game's own sort, reachable without holding a button. The bag screen (X) and the container screens already
// sort the panel under the cursor when X is held ("Hold to Sort" in their key guide, whatever button X is bound to); this module adds
// two ways to run that same sort: a left click on the "Hold to Sort" prompt ([pkp] inventory_sort, on by default) and a sort of the bag
// each time the screen opens ([pkp] inventory_autosort, off by default). There is one sort: the game's, with its order and its
// stack merging, offline and in Link Play alike.
//
// How the game sorts (main 2.0.1a, static; the hold was seen to sort by players):
//  * The bag / container screen is UI class 0x8d (vtable main+0x333ba60, the object holds +0x10; size 0x1a8). Its per-frame virtual
//    0x7100feee78 (x0 = the screen) is the entry hook. Fields: +0x30 player helper (0x7100a6fd58 -> player index), +0x148 container
//    type (u8), +0x154 container id (u16), +0x160 state (1 browsing, 3 holding an item), +0x168 the screen's container kind (0 = bag
//    only), +0x169 the kind of the panel under the cursor.
//  * X is action 0x82, registered twice by 0x7100fef76c with one handler 0x7100ff1bb0 (mode 3 tap, mode 2 hold); it calls
//    0x7100ff8f84 (screen, held). While browsing, a hold adds frame time at +0x17c until the game's hold time ([0x3483b70] +0x18 +0x2d0,
//    ms), then sorts: 0x7100e70130 (player, cursor kind, type, id), then 0x7100ff12e8 (screen) so the panels re-read their arrays.
//    It sorts unless the screen kind is 8 with the cursor on kind 8, or the screen kind is 9 with the cursor off kind 9. Before any
//    of this the handler checks 0x7100fef220 (screen): false when the container is gone (it then closes the screen itself).
//  * 0x7100e70130: on the temp copy (an item is held) it merges (0x71005b3420: equal packed words top each other up to the max stack)
//    and sorts (0x71005b25e0: std::sort on the item table's record +0x20 then +0x10, empty slots last) the array; otherwise it asks
//    the inventory manager [0x3561928] +0x30 / +0x38 (0x71002ef480; kind 4: 0x71002ef77c), the path that also works in Link Play.
//  * The key guide shows the hold as key guide 126 "Hold to Sort" (the second X prompt, next to "Swap", in every browsing set of
//    these screens and in no holding set). Its part's layout goes to the menu pointer (pkp_key_guide.h), so a left click on it sorts.
#include "ui/pkp_sort.h"
#include "core/pkp_guest.h"
#include "ui/pkp_key_guide.h"
#include "core/pkp_settings.h"

#include <atomic>
#include <chrono>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kScreenFrame = 0xfeee78;      // bag / container screen virtual +0x48 (per frame), x0 = the screen
constexpr uint64_t kScreenVtable = 0x333ba70;     // the vtable pointer the object holds (vtable 0x333ba60 + 0x10)
constexpr uint64_t kCanAct = 0xfef220;           // (screen) -> bit 0: the container is still there
constexpr uint64_t kGameSort = 0xe70130;         // (player, kind, type, id): the game's sort of one panel
constexpr uint64_t kRefresh = 0xff12e8;          // (screen): panels re-read their arrays
constexpr uint64_t kPlayerOf = 0xa6fd58;         // (screen + 0x30) -> player index
constexpr uint64_t kPlayerHelper = 0x30, kType = 0x148, kId = 0x154, kState = 0x160, kScreenKind = 0x168, kCursorKind = 0x169;
constexpr uint8_t kBrowsing = 1;
constexpr auto kReopenGap = std::chrono::milliseconds(300);   // no screen frame for this long = the screen was closed
constexpr uint16_t kHoldToSortGuide = 126;       // key guide "Hold to Sort" (X held)

using guest::rd;
using guest::call;

struct State {
    uint64_t base = 0;
    bool click_sort = true, autosort = false, log = false;
    uint64_t screen = 0;
    std::chrono::steady_clock::time_point last_frame{};
    bool auto_pending = false;
    // the key guide prompt
    int client = -1;                              // the key guide client
    std::atomic<uint64_t> widget{0};              // the key guide widget that last filled a "Hold to Sort" part
    uint64_t row = 0;                             // key guide 126's table row
    std::atomic<bool> click{false};               // a click on the prompt (main thread), taken at the screen's next frame
};
State g;

// The panels the game's hold sorts with the cursor on kind `cursor` (0x7100ff8f84).
bool hold_sorts(uint8_t screen_kind, uint8_t cursor) {
    if (screen_kind == 8) return cursor != 8;
    if (screen_kind == 9) return cursor == 9;
    return true;
}

// Runs the game's sort of the panel of kind `kind`, as its hold does.
void game_sort(::Ctx* c, uint64_t screen, uint8_t kind, const char* why) {
    if (!(call(c, g.base + kCanAct, screen) & 1)) return;
    const uint8_t player = static_cast<uint8_t>(call(c, g.base + kPlayerOf, screen + kPlayerHelper));
    const uint8_t type = rd<uint8_t>(screen + kType);
    const uint16_t id = rd<uint16_t>(screen + kId);
    call(c, g.base + kGameSort, player, kind, type, id);
    call(c, g.base + kRefresh, screen);
    if (g.log) NXLOG_I(kTag, "inventory sort: %s, player %u, kind %u, container type %u id %u", why, player, kind, type, id);
}

int hook_screen_frame(::Ctx* c, uint64_t, void*) {
    const uint64_t screen = c->x[0];
    if (!screen || !mem::is_mapped(screen, 0x1a8) || rd<uint64_t>(screen) != g.base + kScreenVtable) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (screen != g.screen || now - g.last_frame > kReopenGap) {   // the screen (re)opened
        g.screen = screen;
        g.auto_pending = g.autosort;
        g.click = false;
    }
    g.last_frame = now;
    const bool clicked = g.click.exchange(false);
    if (!clicked && !g.auto_pending) return 0;
    if (rd<uint8_t>(screen + kState) != kBrowsing) return 0;   // as the hold: never while an item is held (the autosort waits)
    if (g.auto_pending) {
        g.auto_pending = false;
        game_sort(c, screen, 0, "bag opened");
    }
    const uint8_t kind = rd<uint8_t>(screen + kCursorKind);
    if (clicked && hold_sorts(rd<uint8_t>(screen + kScreenKind), kind)) game_sort(c, screen, kind, "click");
    return 0;
}

// ---- the key guide prompt ----

// The screen is open now (its per-frame hook ran within the reopen gap).
bool screen_up() { return g.screen && std::chrono::steady_clock::now() - g.last_frame < kReopenGap; }

// A "Hold to Sort" part is filled while the bag / container screen is up: its layout takes the menu pointer's clicks.
void part_filled(::Ctx* c, const key_guide::Part& p) {
    if (!g.row) g.row = key_guide::guide_row(c, kHoldToSortGuide);
    if (!g.row || p.row != g.row || !screen_up()) return;
    g.widget = p.widget;
    key_guide::set_click_layout(g.client, key_guide::layout_of(c, p));
}

// Main thread: a left click on the prompt. Taken only while the screen is up and its key guide still shows "Hold to Sort" (the widget
// reuses its part instances for other sets, whose clicks go on as before).
bool click(play::Act* act) {
    key_guide::Entries e;
    if (!screen_up() || !key_guide::entries(g.widget.load(), &e)) return false;
    for (uint64_t i = 0; i < e.count; ++i)
        if (e.id(i) == kHoldToSortGuide) {
            g.click = true;
            *act = play::Act::None;
            return true;
        }
    return false;
}

void install(const FeatureInstall& in) {
    const uint64_t base = in.base;
    g.base = base;
    g.click_sort = settings::get_bool(settings::Key::InventorySort);
    g.autosort = settings::get_bool(settings::Key::InventoryAutosort);
    g.log = settings::is_set(settings::Key::MenuLog) && settings::get_int(settings::Key::MenuLog) > 0;   // only when set: the menu's default 1 does not log
    if (!g.click_sort && !g.autosort) return;
    in.hooks.add_entry(base + kScreenFrame, &hook_screen_frame);
    if (g.click_sort)
        g.client = key_guide::add({.name = "inventory sort", .part_filled = &part_filled, .click = &click,
                                   .click_note = "Hold to Sort -> the game's inventory sort"});
    NXLOG_I(kTag, "inventory sort: click %s, autosort %s (the game's sort, hook main+0x%llx)", g.click_sort ? "on" : "off",
            g.autosort ? "on" : "off", static_cast<unsigned long long>(kScreenFrame));
}

}  // namespace

const Feature kSortFeature = {.name = "sort", .install = &install};

}  // namespace nxrt::games::pkp
