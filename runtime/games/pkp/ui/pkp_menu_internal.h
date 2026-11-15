// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the mouse in the game's menus (pkp_menu.cpp and the files it is split into): the scan's state, the game's layout and pane
// offsets, and the functions one of those files calls in another. Internal to those files.
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "play_game.h"

namespace nxrt::games::pkp::menu_detail {

constexpr const char* kTag = "pkp";

using guest::rd;

// ---- offsets ----
using layout::kUiMgr;                           // [ptr] the UI manager
// The UI manager (ctor 0x710087bd60) holds two placeable pools of one type (ctor 0x71009fee60), at +0x84808 (A) and +0x8c858 (B). Pool:
// +0x8 free-slot ring, +0x1030 {entry** data, u64 count} of the entries in use (cap 512), +0x8048 mutex*, slots of 48 bytes at +0x2048.
//   pool A entry (looked up by (id, sub) in 0x710087da00 / 0x7100911bc0): +8 u32 id, +12 u32 sub, +32 -> {placeable** data, u64 count} =
//     every instance of that layout, +40 -> ring {placeable** data, u64 size, u64 cap, u64 head, u64 count} = the instances not in use
//     (0x710087da00 pops one when a screen opens).
//   pool B entry (0x7100912aa0, 0x71009125e0): +0 placeable*, +8 u32 id, +12 u32 sub. Seen empty at run time (p5c: 0 layouts).
constexpr uint64_t kPoolA = 0x84808, kPoolB = 0x8c858;
constexpr uint64_t kPoolEntries = 0x1030;         // {entry** data, u64 count}
constexpr size_t kEntInstances = 32, kEntFree = 40;
constexpr uint64_t kMaxPoolEntries = 512, kMaxInstances = 64;
constexpr size_t kPlaceableObj = 40, kObjLayout = 16;     // placeable +40 -> object, +16 -> the layout (DoPaneHitTest 0x2611d00)
// layout
constexpr size_t kLayPaneCount = 146, kLayFirstNode = 32, kLaySentinel = 64;   // u16 pane count; pane list: node +8 next, +16 pane
// pane
using layout::kPaneName;
using layout::kPaneNameLen;
using layout::kPaneParent;
using layout::kPaneWorld;
constexpr size_t kPaneTag = 184, kPaneSize = 240, kPaneFlags = 260, kPaneAnchor = 268, kPaneIndex = 270, kPaneAlpha = 275;
constexpr float kCanvasW = 1920.f, kCanvasH = 1080.f;
constexpr size_t kMaxLayouts = 256, kMaxPanes = 4096;

struct Pane {
    uint64_t ptr = 0;
    uint32_t lay_id = 0, lay_sub = 0;
    int lay = -1;                      // index into the frame's layouts
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // canvas pixels, y down
    float z = 0;
    uint8_t alpha = 0;
    uint32_t flags = 0;
    bool item = false, cursor = false, slider = false;
    char name[48] = {};
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
    float area() const { return (x1 - x0) * (y1 - y0); }
    bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    float dist(float x, float y) const {
        const float dx = std::max({x0 - x, 0.f, x - x1}), dy = std::max({y0 - y, 0.f, y - y1});
        return std::sqrt(dx * dx + dy * dy);
    }
};

struct Layout {
    uint64_t ptr = 0;
    uint32_t id = 0, sub = 0;
    int panes = 0, visible = 0;
};

struct State {
    uint64_t base = 0x7100000000ull;
    bool installed = false;
    bool enabled = true;
    int log_level = 1;
    // the frame's scan
    std::vector<Layout> layouts;
    std::vector<Pane> panes;
    int hovered = -1, focused = -1, cursor = -1, slider = -1;
    // how pick() found the selection (menus outside play, see pick_extend): 0 the cursor's rule, 1 promoted to the list entry that holds
    // the cursor's part, 2 sibling instances of one button layout (group = their entry panes)
    int mode = 0;
    std::vector<int> group;
    uint64_t mode_sig = 0;
    std::unordered_map<uint32_t, play::Act> key_guides;   // [pkp] menu_key_guides: a key guide layout id -> the button its click sends
    bool active = false;
    uint64_t last_focus = 0, last_hover = 0;   // for the log
    // in-game menus (menu_in_game): a layout with the game's cursor on an entry while the game is in play
    bool ig_enabled = false;
    bool ig_up = false;                // the in-game menu is up (debounced)
    std::atomic<bool> pause_up{false}; // the in-game menu is the pause menu's main view (pkp_save_now.cpp reads it)
    char ig_focus[48] = {};            // the selected entry's pane name
    int ig_tick = 0, ig_hits = 0, ig_miss = 0;
    bool frame_scanned = false;        // menu_frame scanned this frame (the next poll reuses its result)
    bool frame_present = false;
    uint32_t ig_lay_id = 0, ig_lay_sub = 0;
    char ig_cursor[48] = {};
    std::unordered_set<uint64_t> ig_ignore;   // [pkp] menu_in_game_ignore: layouts (id << 32 | sub) whose cursor never counts (a HUD with a cursor)
    std::atomic<uint32_t> n_ig_opens{0};
    int n_not_drawn = 0;               // this scan: pooled layouts skipped as not drawn
    int rec_panes = 0, rec_with = 0, rec_fallback_layouts = 0;      // this scan: shown panes of drawn layouts, those with a draw record,
    std::vector<uint32_t> rec_ages;                                 // layouts with no fresh record (fallback), the stale records' ages
    // diagnostics
    bool was_up = false;
    uint64_t layout_sig = 0;
    std::unordered_set<uint64_t> dumped;    // layout (id, sub) whose panes were listed
    std::chrono::steady_clock::time_point last_log{};
    uint32_t log_lines = 0;
    std::atomic<uint32_t> n_layouts{0}, n_panes{0}, n_items{0}, n_clicks{0};
    char hover_name[48] = {}, focus_name[48] = {};
    // page readability cache (one VirtualQuery per page and scan)
    std::unordered_map<uint64_t, bool> pages;
};

State& st();

// ---- reading the game's panes (pkp_menu_scan.cpp) ----
bool ok(uint64_t a, uint64_t n);
bool has(const std::string& s, const char* k);
bool rect_from(const float* m, float m14, uint32_t size, uint8_t anchor, Pane* out);
bool log_ok(bool force = false);
extern bool g_trace_pick;
void scan();
void log_layouts();

// ---- the layouts the game draws (pkp_menu_draw.cpp) ----
struct DrawSite { uint64_t off; int reg; };
constexpr DrawSite kDrawSites[] = {
    {0x1f5ac6c, 0}, {0x1f5b26c, 0}, {0x1f63094, 0}, {0x1f642a8, 0}, {0x1f6a028, 0}, {0x1f6a8c4, 0},
    {0x1f76168, 0}, {0x1f765b4, 0}, {0x1f78030, 21}, {0x1f78124, 0}, {0x1f7aa2c, 0}, {0x1f7fe48, 0},
};
extern std::atomic<uint32_t> g_frame;
extern std::atomic<uint64_t> g_draw_calls;
extern std::atomic<uint64_t> g_pane_table_full;
bool drawn_recent(uint64_t lay);
bool draw_hooks_live();
enum { kNoRecord = 0, kStale = 1, kFresh = 2 };
int pane_record(uint64_t pane, Pane* out, uint32_t* age);
int hook_pane_draw(::Ctx* c, uint64_t, void* user);
bool pane_shown(uint64_t pane, uint32_t flags);

// ---- the selection (pkp_menu_pick.cpp) ----
void pick(float px, float py, bool inside);

}  // namespace nxrt::games::pkp::menu_detail

namespace nxrt::games::pkp {

bool key_guide_drawn(const menu_detail::State& s);   // pkp_menu_in_game.cpp

}  // namespace nxrt::games::pkp
