// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: split screen (nxrt/split.h). This process is one of two to four windows: it sits in its part of the display's work area
// without a frame and is put back when anything moves it, it plays one controller (by its place in connection order) and it can be
// silent. The keyboard and mouse belong to the window with the focus, as always; every window but the first is made unable to take the
// focus until it is in its place, so starting it does not take the keyboard from the first.
#include "nxrt/split.h"

#include "play_state.h"

namespace nxrt::split {
namespace {

struct Cfg {
    int window = 0;
    int count = 2;
    bool side = false;
    int pad = 0;
    bool mute = false;
};

const Cfg& cfg() {
    static const Cfg c = [] {
        Cfg r;
        const int64_t w = config_get_int("split", "window", 0);
        if (w < 1 || w > 4) {
            if (w != 0) NXLOG_W("play", "[split] window = %lld is not 0 to 4: no split screen", static_cast<long long>(w));
            return r;
        }
        r.window = static_cast<int>(w);
        const int64_t n = config_get_int("split", "count", 2);
        // a window beyond the count has no part of the screen: the count is then the window's number (logged, the starter's mistake)
        r.count = static_cast<int>(std::clamp<int64_t>(n, std::max<int64_t>(2, w), 4));
        if (r.count != n) NXLOG_W("play", "[split] count = %lld does not fit window %d of 2 to 4: %d", static_cast<long long>(n), r.window, r.count);
        const std::string layout = config_get_str("split", "layout", "stacked");
        r.side = layout == "side";
        if (!r.side && layout != "stacked") NXLOG_W("play", "[split] layout = %s is not stacked or side: stacked", layout.c_str());
        r.pad = static_cast<int>(std::clamp<int64_t>(config_get_int("split", "pad", r.window), 0, 8));
        r.mute = config_get_bool("split", "mute", false);
        return r;
    }();
    return c;
}

}  // namespace

int window() { return cfg().window; }

}  // namespace nxrt::split

namespace nxrt::play {
using namespace detail;
namespace detail {
namespace {

// What split_keep compares the window with. Main thread only.
struct Held {
    bool placed = false;        // the window system took our position: the window is kept there
    platform::Rect want;        // its half, as last computed
    platform::Rect got;         // where the window system put it then (it may round a size or keep a margin: that is not fought)
    uint64_t next_check_ns = 0;
};
Held& held() {
    static Held h;
    return h;
}

// The work area the pair shares: the display of [display] monitor, else the primary one.
bool work_area(platform::Rect* out) {
    const int monitor = g().settings.display.monitor;
    return (monitor > 0 && platform::display_work_area(monitor, out)) || platform::display_work_area(0, out);
}

bool place() {
    Held& h = held();
    platform::Rect area;
    if (!work_area(&area)) return false;
    h.want = split_part(area, split::window(), split::cfg().count, split::cfg().side);
    if (!platform::place_window(h.want)) return false;
    if (!platform::window_rect(&h.got)) h.got = h.want;
    g().limiter.reset_schedule();
    return true;
}

}  // namespace

platform::Rect split_part(const platform::Rect& area, int window, int count, bool side) {
    // the work area cut in two along `across` (true: left and right), `second` picks the right or lower half
    const auto half = [](platform::Rect r, bool across, bool second) {
        int& pos = across ? r.x : r.y;
        int& len = across ? r.w : r.h;
        const int first = len / 2;
        if (second) pos += first;
        len = second ? len - first : first;
        return r;
    };
    if (count <= 2) return half(area, side, window == 2);
    if (count == 3) {   // the first window keeps a whole half, the other two share the other half along the other direction
        if (window == 1) return half(area, side, false);
        return half(half(area, side, true), !side, window == 3);
    }
    // four: rows of two, in reading order
    return half(half(area, false, window > 2), true, window == 2 || window == 4);
}

bool split_active() { return split::window() != 0; }

bool split_muted() { return split::cfg().mute; }

size_t own_pads(input::GamepadInfo* out, size_t max) {
    if (!split_active()) return input::gamepad_list(out, max);
    const int place = split::cfg().pad;
    input::GamepadInfo all[8];
    const size_t n = input::gamepad_list(all, 8);
    if (place <= 0 || static_cast<size_t>(place) > n || !out || max == 0) return 0;
    out[0] = all[place - 1];
    return 1;
}

void split_start() {
    G& st = g();
    const split::Cfg& c = split::cfg();
    Held& h = held();
    h.placed = st.window && place();
    if (st.window && !h.placed)
        NXLOG_W(kTag, "split screen: window %d could not be put in its place (the window system places windows itself): move it there by hand", c.window);
    if (h.placed && h.got != h.want)
        NXLOG_W(kTag, "split screen: the window system put window %d at %d,%d %dx%d, not at its place %d,%d %dx%d: it is left there", c.window, h.got.x,
                h.got.y, h.got.w, h.got.h, h.want.x, h.want.y, h.want.w, h.want.h);
    // the window was made unable to take the focus (the game's start-up names that in its WindowDesc); from here a click gives it the keyboard
    if (st.window) platform::set_window_focusable(true);
    NXLOG_I(kTag, "split screen: window %d of %d, %s, %s, sound %s, %s %d,%d %dx%d", c.window, c.count, c.side ? "side by side" : "stacked",
            c.pad ? ("controller " + std::to_string(c.pad)).c_str() : "no controller (keyboard and mouse)", c.mute ? "off" : "on",
            h.placed ? "at" : "not placed, wanted", h.placed ? h.got.x : h.want.x, h.placed ? h.got.y : h.want.y,
            h.placed ? h.got.w : h.want.w, h.placed ? h.got.h : h.want.h);
}

void split_keep(uint64_t now) {
    Held& h = held();
    if (!h.placed || now < h.next_check_ns) return;
    h.next_check_ns = now + 250 * kMs;
    if (platform::minimized()) return;      // the player's own doing; it is put back when it is restored somewhere else
    platform::Rect cur, area;
    if (!platform::window_rect(&cur)) return;
    const bool moved = cur != h.got || platform::fullscreen_mode() != platform::FullscreenMode::Windowed;
    // the half itself changes with the work area (a taskbar that moved, another resolution) and with [display] monitor
    const bool other_half = work_area(&area) && split_part(area, split::window(), split::cfg().count, split::cfg().side) != h.want;
    if (!moved && !other_half) return;
    NXLOG_D(kTag, "split screen: the window is at %d,%d %dx%d, not in its place: putting it back", cur.x, cur.y, cur.w, cur.h);
    if (!place()) h.placed = false;   // refused now: not asked again four times a second
}

}  // namespace detail
}  // namespace nxrt::play
