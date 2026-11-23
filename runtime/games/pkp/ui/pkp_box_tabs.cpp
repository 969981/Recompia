// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs (main 2.0.1a): each backing ID provides one native page for type 1 or three for type 2. The screen and
// panel use that ID for transactions and painting. Teardown and removal retain the physical ID. The allocator skips unplaced
// slices with items, so reusing a physical ID recovers its tabs. A switch while an item is held carries it over (pkp_box_tabs_carry.h).
// The pages show as the game's own page dots under the grid, one per page of the whole box (a standard box also gets the big box's
// frame pose for them and its L / R icons); the panel's own page count and page stay native. The bag's and the box's actions work on
// the whole box: (+) Store All / Retrieve All go over every tab (pkp_box_tabs_store.h), a stack added to a full tab goes on into the next
// ones (pkp_box_tabs_move.h), and the game's sort sorts all tabs as one (pkp_box_tabs_sort.h).
#include "ui/pkp_box_tabs.h"
#include "ui/pkp_box_tabs_carry.h"
#include "ui/pkp_box_tabs_move.h"
#include "ui/pkp_box_tabs_sort.h"
#include "ui/pkp_box_tabs_storage.h"
#include "ui/pkp_box_tabs_store.h"
#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_site_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <utility>

#include "nxrt/log.h"
#include "nxrt/notices.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr const char* kNotice = "pkp.box_tabs";
constexpr HookSite kSites[] = {
    {0xe980c0, 0xd503237fu, true},  // selector (selector, direction, flag)
    {0xff08c0, 0x12001c29u, true},  // removal lookup (screen, type, id)
    {0x776540, 0xd503237fu, true},  // cabinet allocator (type, x, y, z)
    {0xfeee78, 0xd503237fu, true},  // bag / container screen frame (screen)
    {0xff6f80, 0xd503237fu, true},  // bulk transfer after the Store All / Retrieve All dialog (screen, from kind, to kind, matching)
    {0xfee7d4, 0xd503237fu, true},  // the container grid's rebuild (screen, keep cursor)
    {0xe70fb4, 0xd503237fu, true},  // how many of an item a container holds (player, kind, item*, type, id, exact)
    {0xff2bf0, 0xd503237fu, true},  // the selected stack to the other side: Add, Add Stack, half, amount (screen, amount, 0 all / half)
    {0xe70130, 0xd503237fu, true},  // the game's sort of one panel (player, kind, type, id)
    {0xfef30c, 0xd503237fu, true},  // the panel L / R page now (screen)
};
constexpr uint64_t kScreenVtable = 0x333ba70;
constexpr uint64_t kPanelOf = 0xfed634, kRefresh = 0xff12e8, kPlayerOf = 0xa6fd58;
constexpr uint64_t kSetPanelPage = 0xfe5310, kRebuildGrid = 0xfee7d4;
constexpr uint64_t kPagedPanel = 0xfef30c;   // (screen) -> the kind of the panel L / R page: the cursor's, else the other one, kNoPanel
constexpr uint64_t kRoutePages = 0xff0c9c;   // (screen, 0, 0): gives L / R to the paged panel's selector and takes them from the other
constexpr uint64_t kSelectorOf = 0xa86c5c, kPageCallback = 0xff19bc;
// The page dots (carousel, 20 dots, one layout state per count) and the L / R icons of the container frame:
constexpr uint64_t kWidgetOf = 0xa6f92c;     // (panel + 0x48, element id, u16* class) -> widget
constexpr uint64_t kStopAnims = 0xa68240;    // (widget + 8)
constexpr uint64_t kPlayState = 0xa67df4;    // (widget + 8, state): the carousel's state count - 1 shows count dots
constexpr uint64_t kLightDot = 0xebef3c;     // (widget, dot): the lit dot, 0-based
constexpr uint64_t kShowElement = 0xa7b894;  // (panel + 0x30, element id, visible)
constexpr uint64_t kShowSlot = 0xa7b720;     // (panel + 0x30, holder slot, visible)
constexpr uint64_t kFramePose = 0xa67f64;    // (panel + 8, pose): the frame's end pose, 5 type_normal (standard box), 6 type_large (big box)
constexpr uint32_t kLargePose = 6;
constexpr uint64_t kPanelHolder = 0x30, kPanelChildren = 0x48, kPanelCarouselId = 0xc6, kPanelPage = 0xd9;
constexpr uint32_t kCarousel = 0x11e, kCarouselClass = 0x1f, kIconL = 0xf4, kIconR = 0xf5, kLargeParts = 0x36;
constexpr int kMaxDots = 20;
constexpr uint64_t kPlayerHelper = 0x30, kSelectors = 0x80, kOpenState = 0x90, kPhysicalPayload = 0xc8;
constexpr uint64_t kType = 0x148, kId = 0x154, kState = 0x160, kKind = 0x168;
constexpr uint64_t kPanelPageSlide = 0xf8, kBagRange = 0x180;   // panel virtual: the page change's slide; screen: list 0's bag range
constexpr uint64_t kPanelType = 0xc1, kPanelId = 0xd4, kPanelPageCount = 0xd8;
constexpr uint64_t kSelectorCallback = 0x18, kSelectorScreen = 0x20, kSelectorAction = 0x94, kSelectorPage = 0x98, kSelectorPrevious = 0x9c;
constexpr uint16_t kActionPrevious = 0x10, kActionNext = 0x11;   // the selector's L / R actions
constexpr uint8_t kBoxType = 1, kBigBoxType = 2, kBoxPanel = 1, kBrowsing = 1, kPageSlide = 5, kDialog = 6;
constexpr uint8_t kNoPanel = 0xff;
constexpr uint8_t kBagKind = 0, kHandyBagKind = 4;   // the transfer's kinds: main bag, handy bag, the open container (kBoxPanel)
constexpr uint64_t kCursorKind = 0x169, kStatus = 0x170, kFlags = 0x1a0;
constexpr uint64_t kMoveStack = 0xff2bf0, kSelectedCell = 0xff14c0, kCellSlot = 0xc2;
constexpr uint64_t kItemAt = 0xe6d930;      // (player, kind, slot, type, id) -> the live slot record, 0 when empty
constexpr uint32_t kHalf = 1u << 0;         // +0x1a0: the stack's half is asked for
constexpr uint32_t kOwnsCopies = 1u << 5;   // +0x1a0: the screen holds temp copies (an item held)
constexpr uint64_t kStateBefore = 0x164;   // screen: the state a page change's slide (state 5) returns to
constexpr uint32_t kBoxSelector = 2;
constexpr auto kReopenGap = std::chrono::milliseconds(300);

using guest::call;
using guest::ok;
using guest::rd;
using guest::wr;
using box_tabs::BoxStorage;
using box_tabs::Tabs;
using box_tabs::kMaxBigTabs;
using box_tabs::kMaxPages;

struct OpenBox {
    uint64_t screen = 0;
    std::chrono::steady_clock::time_point last_frame{};
    uint16_t physical = 0, shown = 0;
    uint8_t type = 0;
    bool active = false;
    bool restore_physical = false;
    int pending = 0;
    Tabs tabs;                              // the pages / tabs at open or at the last switch
    int frames = 0;                         // screen frames since the open
    int dots = 0, lit = -1;                 // the page dots as last set (0: the game's own)
    // A big box crossing into another tab through the game's own page change: the id switches when that change rebuilds the grid
    // (hook_rebuild), after its slide has painted the outgoing cells from the old tab.
    bool crossing = false;
    uint16_t crossing_id = 0;
    uint8_t crossing_page = 0;
    Tabs crossing_tabs;
};
struct State {
    uint64_t base = 0;
    HookSet* pending_hooks = nullptr;   // play mode installs before main is mapped
    int max_pages = 0, max_big_tabs = 0;
    bool log = false;
    bool moving = false;               // hook_move_stack is running the game's own move
    bool paging = false;               // hook_paged_panel is asking the game's own choice
    OpenBox box;                       // game thread
};
State g;

bool supported_type(uint8_t type) {
    return type == kBoxType || type == kBigBoxType;
}

bool is_screen(uint64_t screen) {
    return ok(screen, 0x1a8) && rd<uint64_t>(screen) == g.base + kScreenVtable;
}

uint8_t box_type(uint64_t screen) {
    const uint8_t type = rd<uint8_t>(screen + kType);
    return rd<uint8_t>(screen + kKind) == kBoxPanel && supported_type(type) ? type : 0;
}

bool showing_tab(uint64_t screen, uint16_t id) {
    const OpenBox& box = g.box;
    return screen == box.screen && box.shown != box.physical && id == box.shown &&
           rd<uint16_t>(screen + kPhysicalPayload) == box.physical;
}

Tabs available_tabs(const OpenBox& box) {
    Tabs tabs{{box.physical}};
    const BoxStorage storage = box_tabs::box_storage(g.base, box.type);
    if (box.physical >= storage.count) return tabs;

    const int max_units = box.type == kBigBoxType ? kMaxBigTabs : kMaxPages;
    const int limit = box.type == kBigBoxType ? g.max_big_tabs : g.max_pages;
    for (int k = 1; k < max_units; ++k) {
        // A setting change must never move saved items to a different tab or physical box.
        const int id = static_cast<int>(storage.count) - 1 - (max_units - 1) * box.physical - (k - 1);
        if (id < 0 || id == box.physical) {
            if (k < limit) ++tabs.taken;
            continue;
        }
        const uint64_t record = storage.record(static_cast<uint32_t>(id));
        if (!ok(record, layout::kCabinetRecordSize)) continue;
        if (rd<uint8_t>(record + layout::kCabinetFlags) & layout::kCabinetOccupied) {
            if (k < limit) ++tabs.taken;
            continue;
        }
        if (k >= limit && !storage.slice_has_items(static_cast<uint32_t>(id))) continue;
        tabs.ids[tabs.count++] = static_cast<uint16_t>(id);
    }
    return tabs;
}

int adjacent_tab(const Tabs& tabs, uint16_t shown, int direction, bool wrap) {
    for (int i = 0; i < tabs.count; ++i) {
        if (tabs.ids[i] != shown) continue;
        const int next = i + direction;
        if (next >= 0 && next < tabs.count) return next;
        return wrap ? (next + tabs.count) % tabs.count : -1;
    }
    return 0;   // The displayed ID stopped being available. Go back to the physical box.
}

// At open, only when there is something to explain: pages that are not free, or Link Play. The page itself shows as the game's own dots.
void notice(uint8_t type, int count, int taken, bool online) {
    if (!taken && !online) return;
    if (online && count + taken < 2) return;
    const bool big = type == kBigBoxType;
    const char* name = big ? "Big storage box" : "Storage box";
    const char* unit = big ? "tab" : "page";
    char text[192];
    if (online)
        std::snprintf(text, sizeof(text), "%s: extra %ss are off during Link Play (their items stay saved)", name, unit);
    else
        std::snprintf(text, sizeof(text), "%s: %d more %s%s not free: the island's %s slots are in use", name, taken, unit,
                      taken == 1 ? " is" : "s are", big ? "big storage box" : "storage box");
    notices::post(kNotice, text, 4.0);
}

// The page dots for the whole box: one per page (a big box: its native pages times its tabs, at most kMaxDots), the shown one lit; a
// standard box also gets the frame's L / R icons. Presentation only: the panel's own page count (+0xd8) and page (+0xd9) stay native.
void show_dots(::Ctx* c, OpenBox& box, uint64_t panel, bool recount) {
    int at = 0;
    while (at < box.tabs.count && box.tabs.ids[at] != box.shown) ++at;
    if (at == box.tabs.count) return;
    const int per = box.type == kBigBoxType ? std::max<int>(1, rd<uint8_t>(panel + kPanelPageCount)) : 1;
    const int count = std::min(box.tabs.count * per, kMaxDots);
    const int lit = std::min(at * per + (box.type == kBigBoxType ? rd<uint8_t>(panel + kPanelPage) : 0), count - 1);
    if (!recount && count == box.dots && lit == box.lit) return;
    const uint64_t out = (c->x[31] - 0x40) & ~uint64_t{0xf};
    wr<uint16_t>(out, 0);
    const uint64_t widget = call(c, g.base + kWidgetOf, panel + kPanelChildren, rd<uint16_t>(panel + kPanelCarouselId), out, 0, 0, out - 0x10);
    if (!ok(widget, 0x40) || rd<uint16_t>(out) != kCarouselClass) return;
    const uint64_t holder = panel + kPanelHolder;
    if (box.type == kBoxType) {
        if (recount) call(c, g.base + kFramePose, panel + 8, kLargePose);   // the big box's frame: room for the dots and the L / R icons
        call(c, g.base + kShowSlot, holder, kLargeParts, 1);
        call(c, g.base + kShowElement, holder, kIconL, 1);
        call(c, g.base + kShowElement, holder, kIconR, 1);
    }
    call(c, g.base + kShowElement, holder, kCarousel, 1);
    if (recount || count != box.dots) {
        call(c, g.base + kStopAnims, widget + 8);
        call(c, g.base + kPlayState, widget + 8, static_cast<uint64_t>(count - 1));
    }
    call(c, g.base + kLightDot, widget, static_cast<uint64_t>(lit));
    box.dots = count;
    box.lit = lit;
}

// Browsing, or a page change's slide that started from browsing (state 5, the state before it at +0x164): presses that come in while a
// page still slides cross into the next tab as the game's own presses page within one, without a stop every few pages.
bool can_switch(::Ctx* c, uint64_t screen) {
    const uint8_t state = rd<uint8_t>(screen + kState);
    if (state != kBrowsing && !(state == kPageSlide && rd<uint8_t>(screen + kStateBefore) == kBrowsing)) return false;
    const auto player = static_cast<uint8_t>(call(c, g.base + kPlayerOf, screen + kPlayerHelper));
    return player != 0xff && rd<uint8_t>(g.base + layout::kItemTempFlag + player) == 0;
}

uint64_t container_selector(::Ctx* c, uint64_t screen) {
    const uint64_t selectors = rd<uint64_t>(screen + kSelectors);
    return ok(selectors, 0x18) ? call(c, g.base + kSelectorOf, selectors, kBoxSelector) : 0;
}

uint64_t ready_panel(::Ctx* c, const OpenBox& box) {
    const uint64_t panel = call(c, g.base + kPanelOf, box.screen, kBoxPanel);
    if (!ok(panel, kPanelPageCount + sizeof(uint8_t)) || rd<uint8_t>(panel + kPanelType) != box.type ||
        rd<uint16_t>(panel + kPanelId) != box.shown)
        return 0;
    return panel;
}

void set_tab_id(OpenBox& box, uint64_t panel, uint16_t target) {
    wr<uint16_t>(box.screen + kId, target);
    wr<uint16_t>(panel + kPanelId, target);
    box.shown = target;
    if (g.log) NXLOG_I(kTag, "storage box tabs: box %u, showing container %u", box.physical, box.shown);
}

bool switch_in_frame(::Ctx* c, OpenBox& box, uint16_t target, int direction) {
    const uint64_t panel = ready_panel(c, box);
    if (!panel) return false;
    if (box.type == kBigBoxType) {
        // Only the Link Play restore takes this path for a big box. The grid indices depend on the panel page.
        const uint64_t selector = container_selector(c, box.screen);
        if (!ok(selector, kSelectorPage + sizeof(uint32_t))) return false;
        set_tab_id(box, panel, target);
        wr<uint32_t>(selector + kSelectorPage, 0);
        call(c, g.base + kSetPanelPage, panel, 0);
        call(c, g.base + kRebuildGrid, box.screen, 1);
    } else {
        set_tab_id(box, panel, target);
        // The page change's slide over the grid, as the panel's own page change runs it (panel virtual +0xf8: next, list 0, the bag
        // range start, 0), so a one-page box turns its pages like a big box.
        const uint64_t selectors = rd<uint64_t>(box.screen + kSelectors);
        const uint64_t list0 = ok(selectors, 0x18) ? call(c, g.base + kSelectorOf, selectors, 0) : 0;
        const uint64_t slide = rd<uint64_t>(rd<uint64_t>(panel) + kPanelPageSlide);
        if (list0 && slide) call(c, slide, panel, direction > 0 ? 1 : 0, list0, rd<uint32_t>(box.screen + kBagRange), 0);
    }
    call(c, g.base + kRefresh, box.screen);
    return true;
}

// An item held across the tab boundary (pkp_box_tabs_carry.h): parked in the bag, the copies written back, the ids switched, the copies
// taken again. A big box also moves to the new tab's first page (R) or last page (L), as its own page change would.
bool carry_to(::Ctx* c, OpenBox& box, uint16_t target, int direction) {
    const uint64_t panel = ready_panel(c, box);
    if (!panel) return false;
    const box_carry::Parked parked = box_carry::park(c, g.base, box.screen);
    if (!parked.ok) {
        notices::post(kNotice, box.type == kBigBoxType ? "Big storage box: no free bag slot to carry the item to another tab"
                                                       : "Storage box: no free bag slot to carry the item to another page", 3.0);
        return false;
    }
    const uint64_t selector = box.type == kBigBoxType ? container_selector(c, box.screen) : 0;
    const uint32_t last = std::max<uint32_t>(1, rd<uint8_t>(panel + kPanelPageCount)) - 1;
    const uint32_t from = ok(selector, kSelectorPage + 8) ? rd<uint32_t>(selector + kSelectorPage) : 0, to = direction > 0 ? 0 : last;
    set_tab_id(box, panel, target);
    box_carry::resume(c, g.base, box.screen, parked);   // false: the hold ended, the item is in the bag; the new tab shows either way
    if (ok(selector, kSelectorPage + 8)) {
        wr<uint32_t>(selector + kSelectorPrevious, from);
        wr<uint32_t>(selector + kSelectorPage, to);
        wr<uint16_t>(selector + kSelectorAction, direction > 0 ? kActionNext : kActionPrevious);
        call(c, g.base + kSetPanelPage, panel, to);
        call(c, g.base + kRebuildGrid, box.screen, 1);
    }
    call(c, g.base + kRefresh, box.screen);
    if (g.log) NXLOG_I(kTag, "storage box tabs: carried the held item to container %u (parked in bag slot %u)", target, parked.slot);
    return true;
}

int hook_screen_frame(::Ctx* c, uint64_t, void*) {
    const uint64_t screen = c->x[0];
    if (!is_screen(screen)) return 0;
    OpenBox& box = g.box;
    const uint8_t type = box_type(screen);
    if (!type) {
        if (screen == box.screen) box = {};
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint16_t id = rd<uint16_t>(screen + kId);
    const bool tab_shown = type == box.type && showing_tab(screen, id);
    const bool gap = now - box.last_frame > kReopenGap;
    const bool opening = gap || screen != box.screen || type != box.type || id != box.physical || box.shown != box.physical;
    if (opening && !tab_shown) {
        const bool online = call(c, g.base + layout::kIsOnline, 0) & 1;
        box = {.screen = screen, .last_frame = now, .physical = id, .shown = id, .type = type, .active = !online};
        box.tabs = available_tabs(box);
        notice(type, box.tabs.count, box.tabs.taken, online);
        if (g.log)
            NXLOG_I(kTag, "storage box tabs: opened type %u id %u, %d pages (%d taken)%s", type, id, box.tabs.count, box.tabs.taken,
                    online ? ", online" : "");
        return 0;
    }

    box.last_frame = now;
    ++box.frames;
    box.crossing = false;   // a crossing the game did not carry out (its page change runs within the frame that asked for it)
    // The open gave L / R to the bag, before the box was known to have pages (hook_paged_panel).
    if (box.frames == 2 && box.type == kBoxType && box.active && box.tabs.count > 1 && rd<uint32_t>(screen + kState) == kBrowsing)
        call(c, g.base + kRoutePages, screen, 0, 0);
    // The dots once the panel's own open setup is done (it sets them for its native pages), again a little later in case the setup
    // ran late, and whenever the shown page changes (a big box's native page change lights its own dot).
    if (box.active && box.tabs.count > 1 && box.frames >= 2)
        if (const uint64_t panel = ready_panel(c, box)) show_dots(c, box, panel, box.frames == 2 || box.frames == 15);
    const int direction = std::exchange(box.pending, 0);
    if (!tab_shown && !direction) return 0;
    const bool online = call(c, g.base + layout::kIsOnline, 0) & 1;
    // A session that opens while a tab is shown: back to the box itself as soon as staging permits (tabs are offline only). A frame gap
    // with a tab still shown is the same open (a reopen writes the physical id to +0x154 and is caught above), so the tab stays.
    if (tab_shown && online) box.restore_physical = true;
    const bool carry = !box.restore_physical && box_carry::holding(c, g.base, screen);
    if ((!box.restore_physical && (!box.active || online || !direction)) || (!carry && !can_switch(c, screen))) return 0;

    const Tabs tabs = online ? Tabs{{box.physical}} : available_tabs(box);
    const int target = box.restore_physical ? 0 : adjacent_tab(tabs, id, direction, true);
    if (tabs.ids[target] == id) return 0;
    if (!(carry ? carry_to(c, box, tabs.ids[target], direction) : switch_in_frame(c, box, tabs.ids[target], direction))) return 0;
    box.restore_physical = false;
    box.tabs = tabs;
    if (box.active && tabs.count > 1)
        if (const uint64_t panel = ready_panel(c, box)) show_dots(c, box, panel, false);
    return 0;
}

int hook_page_request(::Ctx* c, uint64_t, void*) {
    const auto request = static_cast<uint32_t>(c->x[1]);
    const uint64_t selector = c->x[0];
    OpenBox& box = g.box;
    if ((request != 2 && request != 3) || !box.active || !ok(selector, kSelectorScreen + sizeof(uint64_t)) ||
        rd<uint64_t>(selector + kSelectorCallback) != g.base + kPageCallback ||
        rd<uint64_t>(selector + kSelectorScreen) != box.screen || !is_screen(box.screen) || box_type(box.screen) != box.type)
        return 0;
    // Both page lists use the same callback. Only list 2 belongs to the container.
    if (container_selector(c, box.screen) != selector) return 0;
    const int direction = request == 2 ? -1 : 1;
    if (box.type == kBoxType) {
        // One native page per tab: the frame hook switches. A press wraps at the ends, a held repeat stops there.
        const Tabs tabs = available_tabs(box);
        const int target = adjacent_tab(tabs, box.shown, direction, (c->x[2] & 1) != 0);
        if (target >= 0 && tabs.ids[target] != box.shown) box.pending = direction;
        c->x[0] = 0;
        return 1;
    }

    if (box.restore_physical || rd<uint16_t>(box.screen + kPhysicalPayload) != box.physical ||
        rd<uint16_t>(box.screen + kId) != box.shown || !ok(selector, kSelectorPage + sizeof(uint32_t)))
        return 0;
    const uint64_t panel = ready_panel(c, box);
    if (!panel) return 0;
    const uint32_t page_count = rd<uint8_t>(panel + kPanelPageCount);
    if (page_count < 2) return 0;
    const uint32_t last_page = page_count - 1;
    if (rd<uint32_t>(selector + kSelectorPage) != (direction > 0 ? last_page : 0)) return 0;
    if (call(c, g.base + layout::kIsOnline, 0) & 1) return 0;

    // No check of the panel the cursor or the menu pointer is on: the game gave L / R to the container (hook_paged_panel), from the bag
    // too when the bag has one page (a check let the game wrap back to page 1 while the pointer was over the bag).
    const Tabs tabs = available_tabs(box);
    const int target = adjacent_tab(tabs, box.shown, direction, (c->x[2] & 1) != 0);
    if (target < 0 || tabs.ids[target] == box.shown) return 0;
    if (!can_switch(c, box.screen)) {
        // An item held: carried over in the frame hook (the copies are written back and taken again there, on the screen's own frame).
        // Anything else (an item held while a page still slides, a dialog): stay on this page. The game must not wrap back to the first.
        if (box_carry::holding(c, g.base, box.screen)) box.pending = direction;
        c->x[0] = 0;
        return 1;
    }
    // The ids switch when the page change rebuilds the grid (hook_rebuild). Its slide first repaints the cells it slides out from the
    // shown id and the old page's slot list; switched any earlier, those cells showed the new tab's other end (mostly empty).
    box.crossing = true;
    box.crossing_id = tabs.ids[target];
    box.crossing_page = static_cast<uint8_t>(direction > 0 ? 0 : last_page);
    box.crossing_tabs = tabs;
    // A press: the game's own wrap at the end (last page -> first, first -> last) now runs on the new id, with its slide and sound, and
    // its page change repaints the grid. A held repeat does not wrap: the page is set here and the dispatcher sees the change.
    if (c->x[2] & 1) return 0;
    wr<uint32_t>(selector + kSelectorPage, direction > 0 ? 0 : last_page);
    c->x[0] = 1;
    return 1;
}

// The open box's screen in a state its actions may work on every tab: offline, no temp copies (no item held), the ids as tracked.
// The player it belongs to, or -1.
int live_player(::Ctx* c, uint64_t screen) {
    const OpenBox& box = g.box;
    if (!box.active || !screen || screen != box.screen || !is_screen(screen) || box_type(screen) != box.type ||
        (rd<uint32_t>(screen + kFlags) & kOwnsCopies) || rd<uint16_t>(screen + kPhysicalPayload) != box.physical ||
        rd<uint16_t>(screen + kId) != box.shown)
        return -1;
    const auto player = static_cast<uint8_t>(call(c, g.base + kPlayerOf, screen + kPlayerHelper));
    if (player > 1 || rd<uint8_t>(g.base + layout::kItemTempFlag + player) != 0) return -1;
    return (call(c, g.base + layout::kIsOnline, 0) & 1) ? -1 : player;
}

bool bag_kind(uint8_t kind) {
    return kind == kBagKind || kind == kHandyBagKind;
}

// Store All and Retrieve All, either choice of the dialog: every tab in turn, the shown one first.
int hook_transfer(::Ctx* c, uint64_t, void*) {
    if (box_store::running()) return 0;
    const uint64_t screen = c->x[0];
    const auto source = static_cast<uint8_t>(c->x[1]), target = static_cast<uint8_t>(c->x[2]);
    OpenBox& box = g.box;
    const bool store = bag_kind(source) && target == kBoxPanel;
    if ((!store && !(source == kBoxPanel && target == kBagKind)) || live_player(c, screen) < 0 ||
        rd<uint8_t>(screen + kState) != kDialog || rd<uint8_t>(screen + kStateBefore) != kBrowsing)
        return 0;
    const uint64_t panel = ready_panel(c, box);
    if (!panel) return 0;
    const Tabs tabs = available_tabs(box);
    const int shown = tabs.index_of(box.shown);
    if (tabs.count < 2 || shown < 0) return 0;
    const BoxStorage storage = box_tabs::box_storage(g.base, box.type);
    box.tabs = tabs;
    c->x[0] = box_store::sweep(c, g.base, {.screen = screen, .panel = panel, .storage = &storage, .tabs = &tabs, .shown = shown,
                                           .source = source, .target = target, .store = store, .matching = (c->x[3] & 1) != 0,
                                           .log = g.log});
    return 1;
}

// Add / Add Stack / half / an amount, from the bag: the game fills the shown tab and turns to the page it filled. A tab that takes none
// of it gives way to the first one that does (one that holds the item before an empty slot), and what does not fit goes on into the next
// tabs. Out of the box the game's own move is enough.
int hook_move_stack(::Ctx* c, uint64_t, void*) {
    if (g.moving) return 0;
    const uint64_t screen = c->x[0];
    OpenBox& box = g.box;
    const int player = live_player(c, screen);
    if (player < 0 || rd<uint8_t>(screen + kState) != kBrowsing) return 0;
    const auto amount = static_cast<uint32_t>(c->x[1]);
    const uint8_t kind = rd<uint8_t>(screen + kCursorKind);
    if (!bag_kind(kind)) return 0;
    const uint64_t cell = call(c, g.base + kSelectedCell, screen);
    if (!ok(cell, 0xc8)) return 0;
    box_move::Stack stack{.player = static_cast<uint8_t>(player), .kind = kind, .slot = rd<uint16_t>(cell + kCellSlot), .type = box.type};
    stack.item = call(c, g.base + kItemAt, stack.player, kind, stack.slot, box.type, box.shown);
    if (!ok(stack.item, items::kSlotSize)) return 0;
    const uint64_t payload = rd<uint64_t>(stack.item + items::kSlotPayload);
    stack.packed = static_cast<uint32_t>(payload) & 0x7fffffff;
    const uint32_t count = items::count_of(payload);
    if (!items::id_of(payload) || !count) return 0;
    stack.max_stack = static_cast<uint16_t>(call(c, g.base + layout::kMaxStack, 0));
    const uint32_t half = (rd<uint32_t>(screen + kFlags) & kHalf) ? std::max(1u, count >> 1) : count;
    const uint32_t want = amount ? std::min(amount, count) : half;

    const Tabs tabs = available_tabs(box);
    int shown = tabs.index_of(box.shown);
    const BoxStorage storage = box_tabs::box_storage(g.base, box.type);
    if (tabs.count < 2 || shown < 0 || !storage.count) return 0;
    const auto room = [&](int i) { return storage.room(tabs.ids[i], stack.packed, stack.max_stack); };
    if (!room(shown).total) {
        int target = -1;
        for (int n = 1; n < tabs.count; ++n) {
            const int i = (shown + n) % tabs.count;
            const BoxStorage::Room r = room(i);
            if (r.merge) { target = i; break; }
            if (r.total && target < 0) target = i;
        }
        if (target < 0 || !switch_in_frame(c, box, tabs.ids[target], target > shown ? 1 : -1)) return 0;
        box.tabs = tabs;
        shown = target;
    }
    if (room(shown).total >= want) return 0;

    g.moving = true;
    uint64_t result = call(c, g.base + kMoveStack, screen, amount);
    g.moving = false;
    const uint32_t went = count - std::min(count, box_move::left(stack));
    if (went && went < want) {
        const uint32_t more = box_move::spread(c, g.base, stack, want - went, storage, tabs, shown);
        if (g.log) NXLOG_I(kTag, "storage box tabs: %u of %u into container %u, %u into other tabs", went, want, box.shown, more);
        if (went + more >= want) {
            wr<uint32_t>(screen + kStatus, 1);   // all of it went: no "could not be stored"
            result = 1;
        }
        if (more) call(c, g.base + kRefresh, screen);
    }
    c->x[0] = result;
    return 1;
}

// The sort (X held, or a click on its prompt) with the cursor on the box: all its tabs as one.
int hook_sort(::Ctx* c, uint64_t, void*) {
    OpenBox& box = g.box;
    if (static_cast<uint8_t>(c->x[1]) != kBoxPanel || static_cast<uint8_t>(c->x[2]) != box.type ||
        static_cast<uint16_t>(c->x[3]) != box.shown || live_player(c, box.screen) < 0 || rd<uint8_t>(box.screen + kState) != kBrowsing)
        return 0;
    const Tabs tabs = available_tabs(box);
    const BoxStorage storage = box_tabs::box_storage(g.base, box.type);
    if (tabs.count < 2 || !storage.count || !box_sort::sort(c, g.base, storage, tabs)) return 0;
    if (g.log) NXLOG_I(kTag, "storage box tabs: sorted box %u over %d tabs", box.physical, tabs.count);
    return 1;
}

// Which panel L / R page: the game takes the panel the cursor is on if that one has pages, else the other one. A standard box has one
// page there, so the bag got L / R with the cursor on the box too. With tabs the box counts as a panel with pages, as a big box does.
int hook_paged_panel(::Ctx* c, uint64_t, void*) {
    if (g.paging) return 0;
    const uint64_t screen = c->x[0];
    const OpenBox& box = g.box;
    if (!box.active || box.type != kBoxType || box.tabs.count < 2 || screen != box.screen || !is_screen(screen) ||
        box_type(screen) != box.type || rd<uint16_t>(screen + kPhysicalPayload) != box.physical)
        return 0;
    if (call(c, g.base + layout::kIsOnline, 0) & 1) return 0;
    g.paging = true;
    uint64_t kind = call(c, g.base + kPagedPanel, screen) & 0xff;
    g.paging = false;
    if (rd<uint8_t>(screen + kCursorKind) == kBoxPanel || kind == kNoPanel) kind = kBoxPanel;
    c->x[0] = kind;
    return 1;
}

// The page change's grid rebuild (0xff939c -> 0xfee7d4(screen, 1)) after a tab crossing was requested: the new id and its page, then
// the game's own rebuild and repaint from them.
int hook_rebuild(::Ctx* c, uint64_t, void*) {
    OpenBox& box = g.box;
    if (!box.crossing || c->x[0] != box.screen || (c->x[1] & 1) == 0) return 0;
    box.crossing = false;
    const uint64_t panel = ready_panel(c, box);
    if (!panel || !is_screen(box.screen) || box_type(box.screen) != box.type) return 0;
    set_tab_id(box, panel, box.crossing_id);
    box.tabs = box.crossing_tabs;
    if (rd<uint8_t>(panel + kPanelPage) != box.crossing_page) call(c, g.base + kSetPanelPage, panel, box.crossing_page);
    return 0;
}

int hook_match_open_box(::Ctx* c, uint64_t, void*) {
    const uint64_t screen = c->x[0];
    if (screen != g.box.screen || g.box.shown == g.box.physical || !is_screen(screen) || box_type(screen) != g.box.type ||
        !showing_tab(screen, rd<uint16_t>(screen + kId)))
        return 0;
    const auto type = static_cast<uint8_t>(c->x[1]);
    const auto id = static_cast<uint16_t>(c->x[2]);
    c->x[0] = type - 1u < 30u && rd<uint8_t>(screen + kKind) != 0 && rd<uint8_t>(screen + kType) == type &&
              id == g.box.physical && rd<uint32_t>(screen + kOpenState) - 1u < 2u;
    return 1;
}

bool allocate_box(::Ctx* c, uint8_t type) {
    const BoxStorage storage = box_tabs::box_storage(g.base, type);
    for (uint32_t id = 0; id < storage.count; ++id) {
        const uint64_t record = storage.record(id);
        if (!ok(record, layout::kCabinetRecordSize)) return false;
        const uint8_t flags = rd<uint8_t>(record + layout::kCabinetFlags);
        if ((flags & layout::kCabinetOccupied) || storage.slice_has_items(id)) continue;
        wr<uint16_t>(record + layout::kCabinetCellX, static_cast<uint16_t>(c->x[1]));
        wr<uint16_t>(record + layout::kCabinetCellY, static_cast<uint16_t>(c->x[2]));
        wr<uint8_t>(record + layout::kCabinetFlags, static_cast<uint8_t>(flags | layout::kCabinetOccupied));
        wr<uint16_t>(record + layout::kCabinetCellZ, static_cast<uint16_t>(c->x[3]));
        wr<uint8_t>(record + layout::kCabinetType, type);
        return true;
    }
    return false;
}

int hook_allocate_box(::Ctx* c, uint64_t, void*) {
    const auto type = static_cast<uint8_t>(c->x[0]);
    if (!supported_type(type) || !box_tabs::box_storage(g.base, type).count) return 0;   // no readable save: the game's own allocator
    c->x[0] = allocate_box(c, type);
    return 1;
}

void frame(bool, double, play::GameFrame*) {
    HookSet* hooks = std::exchange(g.pending_hooks, nullptr);
    if (!hooks) return;
    uint32_t word = 0;
    if (const HookSite* bad = first_bad_site(g.base, kSites, std::size(kSites), &word)) {
        NXLOG_W(kTag, "storage box tabs: off: main+0x%llx is not the expected hook site (word %08x, expected %08x)",
                static_cast<unsigned long long>(bad->site), word, bad->word);
        return;
    }
    hooks->add_entry(g.base + kSites[0].site, &hook_page_request);
    hooks->add_entry(g.base + kSites[1].site, &hook_match_open_box);
    hooks->add_entry(g.base + kSites[2].site, &hook_allocate_box);
    hooks->add_entry(g.base + kSites[3].site, &hook_screen_frame);
    hooks->add_entry(g.base + kSites[4].site, &hook_transfer);
    hooks->add_entry(g.base + kSites[5].site, &hook_rebuild);
    hooks->add_entry(g.base + kSites[6].site, &box_store::hook_count_matching);
    hooks->add_entry(g.base + kSites[7].site, &hook_move_stack);
    hooks->add_entry(g.base + kSites[8].site, &hook_sort);
    hooks->add_entry(g.base + kSites[9].site, &hook_paged_panel);
    NXLOG_I(kTag, "storage box tabs: on, up to %d pages per standard box and %d tabs per big box, offline only",
            g.max_pages, g.max_big_tabs);
}

void install(const FeatureInstall& in) {
    if (!settings::get_bool(settings::Key::BoxTabs)) return;
    g.base = in.base;
    g.max_pages = static_cast<int>(settings::get_int(settings::Key::BoxTabPages));
    g.max_big_tabs = static_cast<int>(settings::get_int(settings::Key::BigBoxTabs));
    g.log = settings::is_set(settings::Key::MenuLog) && settings::get_int(settings::Key::MenuLog) > 0;
    g.pending_hooks = &in.hooks;
}

void remove() { g = {}; }

}  // namespace

const Feature kBoxTabsFeature = {.name = "box_tabs", .install = &install, .remove = &remove, .frame = &frame};

}  // namespace nxrt::games::pkp
