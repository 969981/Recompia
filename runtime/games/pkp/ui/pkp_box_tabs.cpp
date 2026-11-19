// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs (main 2.0.1a): each backing ID provides one native page for type 1 or three for type 2. The screen and
// panel use that ID for transactions and painting. Teardown and removal retain the physical ID. The allocator skips unplaced
// slices with items, so reusing a physical ID recovers its tabs. Switching requires inactive per-player staging.
// The pages show as the game's own page dots under the grid, one per page of the whole box (a standard box also gets the big box's
// frame pose for them and its L / R icons); the panel's own page count and page stay native.
#include "ui/pkp_box_tabs.h"
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

#include "nxrt/input_source.h"
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
};
constexpr uint64_t kScreenVtable = 0x333ba70;
constexpr uint64_t kPanelOf = 0xfed634, kRefresh = 0xff12e8, kPlayerOf = 0xa6fd58;
constexpr uint64_t kActivePane = 0xfef30c, kSetPanelPage = 0xfe5310, kRebuildGrid = 0xfee7d4;
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
constexpr uint64_t kPanelType = 0xc1, kPanelId = 0xd4, kPanelPageCount = 0xd8;
constexpr uint64_t kSelectorCallback = 0x18, kSelectorScreen = 0x20, kSelectorPage = 0x98;
constexpr uint8_t kBoxType = 1, kBigBoxType = 2, kBoxPanel = 1, kBrowsing = 1;
constexpr uint32_t kBoxSelector = 2;
constexpr int kMaxPages = 12, kMaxBigTabs = 6;   // fixed for good: they set where every tab's items live (available_tabs)
constexpr auto kReopenGap = std::chrono::milliseconds(300);

using guest::call;
using guest::ok;
using guest::rd;
using guest::wr;

struct Tabs {
    std::array<uint16_t, kMaxPages> ids{};
    int count = 1;
    int taken = 0;   // requested units whose ID is unavailable
};
struct OpenBox {
    uint64_t screen = 0;
    std::chrono::steady_clock::time_point last_frame{};
    uint16_t physical = 0, shown = 0;
    uint8_t type = 0;
    bool active = false;
    bool restore_physical = false;
    int pending = 0;
    uint64_t prev_buttons = ~uint64_t{0};   // L / R as last seen (all set at open: a button held from before does not count)
    Tabs tabs;                              // the pages / tabs at open or at the last switch
    int frames = 0;                         // screen frames since the open
    int dots = 0, lit = -1;                 // the page dots as last set (0: the game's own)
};
struct State {
    uint64_t base = 0;
    HookSet* pending_hooks = nullptr;   // play mode installs before main is mapped
    int max_pages = 0, max_big_tabs = 0;
    bool log = false;
    OpenBox box;                       // game thread
};
State g;

bool supported_type(uint8_t type) {
    return type == kBoxType || type == kBigBoxType;
}

struct BoxStorage {
    uint64_t records, store;
    uint32_t first_record, count, slots;

    uint64_t record(uint32_t id) const {
        const uint32_t index = first_record + id;
        // Native indices outside the cabinet pool use record zero, but the item slice uses the logical ID.
        return records + (index < layout::kCabinetRecordCount ? index : 0) * layout::kCabinetRecordSize;
    }

    bool slice_has_items(uint32_t id) const {
        const uint64_t first = store + uint64_t{id} * slots * items::kSlotSize;
        for (uint32_t slot = 0; slot < slots; ++slot) {
            if (items::id_of(rd<uint32_t>(first + slot * items::kSlotSize + items::kSlotPayload))) return true;
        }
        return false;
    }
};

BoxStorage box_storage(uint8_t type) {
    const uint64_t root = rd<uint64_t>(g.base + layout::kSaveRoot);
    if (!root) return {};
    const uint64_t offset = type * sizeof(uint16_t);
    const uint32_t first_record = rd<uint16_t>(g.base + layout::kCabinetBases + offset);
    const uint32_t count = rd<uint16_t>(g.base + layout::kCabinetCounts + offset);
    const uint32_t slots = rd<uint16_t>(g.base + layout::kContainerSlotCounts + offset);
    const uint32_t first_slot = rd<uint16_t>(g.base + layout::kContainerSlotBases + offset);
    const uint64_t store = root + layout::kStageContainerStore + first_slot * items::kSlotSize;
    if (!count || !slots || !ok(store, uint64_t{count} * slots * items::kSlotSize)) return {};
    return {root + layout::kCabinetRecords, store, first_record, count, slots};
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
    const BoxStorage storage = box_storage(box.type);
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

bool can_switch(::Ctx* c, uint64_t screen) {
    if (rd<uint8_t>(screen + kState) != kBrowsing) return false;
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

bool switch_in_frame(::Ctx* c, OpenBox& box, uint16_t target) {
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
    }
    call(c, g.base + kRefresh, box.screen);
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
    // The dots once the panel's own open setup is done (it sets them for its native pages), again a little later in case the setup
    // ran late, and whenever the shown page changes (a big box's native page change lights its own dot).
    if (box.active && box.tabs.count > 1 && box.frames >= 2)
        if (const uint64_t panel = ready_panel(c, box)) show_dots(c, box, panel, box.frames == 2 || box.frames == 15);
    if (box.type == kBoxType && box.active) {
        // A one-page box's page selector takes no input (0xe980c0 is never asked), so L / R are read here, from the pad state the game
        // reads too (keyboard and pad bindings included).
        uint64_t buttons = 0;
        if (auto src = hid::input_source()) {
            hid::InputSnapshot snap;
            src->poll(snap);
            buttons = (snap.npad[0].buttons | snap.npad[hid::kNpadSlotHandheld].buttons) & (hid::btn(hid::kBtnL) | hid::btn(hid::kBtnR));
        }
        const uint64_t pressed = buttons & ~box.prev_buttons;
        box.prev_buttons = buttons;
        if (pressed & hid::btn(hid::kBtnR)) box.pending = 1;
        else if (pressed & hid::btn(hid::kBtnL)) box.pending = -1;
    }
    const int direction = std::exchange(box.pending, 0);
    if (!tab_shown && !direction) return 0;
    const bool online = call(c, g.base + layout::kIsOnline, 0) & 1;
    // A session that opens while a tab is shown: back to the box itself as soon as staging permits (tabs are offline only). A frame gap
    // with a tab still shown is the same open (a reopen writes the physical id to +0x154 and is caught above), so the tab stays.
    if (tab_shown && online) box.restore_physical = true;
    if ((!box.restore_physical && (!box.active || online || !direction)) || !can_switch(c, screen)) return 0;

    const Tabs tabs = online ? Tabs{{box.physical}} : available_tabs(box);
    const int target = box.restore_physical ? 0 : adjacent_tab(tabs, id, direction, true);
    if (tabs.ids[target] == id || !switch_in_frame(c, box, tabs.ids[target])) return 0;
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
    if (box.type != kBigBoxType) return 0;   // a one-page box's selector never asks (standard boxes: L / R in the frame hook)

    if (box.restore_physical || rd<uint16_t>(box.screen + kPhysicalPayload) != box.physical ||
        rd<uint16_t>(box.screen + kId) != box.shown || !ok(selector, kSelectorPage + sizeof(uint32_t)))
        return 0;
    const uint64_t panel = ready_panel(c, box);
    if (!panel) return 0;
    const uint32_t page_count = rd<uint8_t>(panel + kPanelPageCount);
    if (page_count < 2) return 0;
    const uint32_t last_page = page_count - 1;
    if (rd<uint32_t>(selector + kSelectorPage) != (direction > 0 ? last_page : 0)) return 0;
    if ((call(c, g.base + layout::kIsOnline, 0) & 1) || !can_switch(c, box.screen) ||
        call(c, g.base + kActivePane, box.screen) != kBoxPanel)
        return 0;

    const Tabs tabs = available_tabs(box);
    const int target = adjacent_tab(tabs, box.shown, direction, (c->x[2] & 1) != 0);
    if (target < 0 || tabs.ids[target] == box.shown) return 0;
    set_tab_id(box, panel, tabs.ids[target]);
    wr<uint32_t>(selector + kSelectorPage, direction > 0 ? 0 : last_page);
    // The selector dispatcher sees a changed index and runs the native panel, grid and cursor update (the frame hook relights the dots).
    box.tabs = tabs;
    c->x[0] = 1;
    return 1;
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
    const BoxStorage storage = box_storage(type);
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
    if (!supported_type(type) || !box_storage(type).count) return 0;   // no readable save: the game's own allocator
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
