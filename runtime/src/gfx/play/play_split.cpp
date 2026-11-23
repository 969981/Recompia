// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: split screen (nxrt/split.h). This process is one of two to four windows: it sits in its part of the display's work area
// without a frame, or fills a display of its own, and is put back when anything moves it, it plays one controller (by its place in
// connection order) and it can be silent. The keyboard and mouse belong to the window with the focus, as always; every window but the
// first is made unable to take the focus until it is in its place, so starting it does not take the keyboard from the first.
#include "nxrt/split.h"

#include "nxrt/text.h"
#include "play_state.h"

namespace nxrt::split {
namespace {

struct Cfg {
    int window = 0;
    int count = 2;
    bool side = false;
    std::vector<int> displays;   // of each window; empty: one screen for all
    int pad = 0;
    bool mute = false;
};

// [split] displays: one number per window, or nothing (logged) when it is anything else.
std::vector<int> parse_displays(const std::string& s, int count) {
    std::vector<int> out;
    if (text::trim(s).empty()) return out;
    for (size_t at = 0; at <= s.size();) {
        const size_t end = std::min(s.find(',', at), s.size());
        const std::string part = text::trim(s.substr(at, end - at));
        char* stop = nullptr;
        const long n = std::strtol(part.c_str(), &stop, 10);
        if (part.empty() || *stop || n < 1 || n > 32) break;
        out.push_back(static_cast<int>(n));
        at = end + 1;
    }
    const size_t parts = static_cast<size_t>(std::count(s.begin(), s.end(), ',')) + 1;
    if (out.size() == parts && out.size() == static_cast<size_t>(count)) return out;
    NXLOG_W("play", "[split] displays = %s is not %d display numbers: the windows share one screen", s.c_str(), count);
    return {};
}

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
        r.displays = parse_displays(config_get_str("split", "displays", ""), r.count);
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

// Where this window belongs.
struct Place {
    platform::Rect rect;
    bool whole = false;         // a display of its own, filled (borderless fullscreen); else a part of a work area
    bool operator==(const Place&) const = default;
};

// What split_keep compares the window with. Main thread only.
struct Held {
    bool placed = false;        // the window system took our position: the window is kept there
    Place want;                 // its place, as last computed
    platform::Rect got;         // where the window system put it then (it may round a size or keep a margin: that is not fought)
    platform::FullscreenMode mode = platform::FullscreenMode::Windowed;   // and in which mode (a refused fullscreen is not fought either)
    uint64_t next_check_ns = 0;
};
Held& held() {
    static Held h;
    return h;
}

// The work area of that display, the primary one's when there is no such display (or number is 0).
bool work_area(int number, platform::Rect* out) {
    return (number > 0 && platform::display_work_area(number, out)) || platform::display_work_area(0, out);
}

// The place of this window now: its part of the one screen ([display] monitor), or with [split] displays its own display, shared with the
// windows that are on it too. Displays are told apart by where they are, so a missing one is the primary one and mirrored ones are one.
bool find_place(Place* out) {
    const split::Cfg& c = split::cfg();
    int display = g().settings.display.monitor, nth = split::window(), sharing = c.count;
    if (!c.displays.empty()) {
        const auto bounds = [](int number, platform::Rect* r) { return platform::display_bounds(number, r) || platform::display_bounds(0, r); };
        display = c.displays[static_cast<size_t>(split::window()) - 1];
        platform::Rect own;
        if (!bounds(display, &own)) return false;
        nth = sharing = 0;
        for (int w = 1; w <= c.count; ++w) {
            platform::Rect other;
            if (!bounds(c.displays[static_cast<size_t>(w) - 1], &other) || other != own) continue;
            ++sharing;
            if (w <= split::window()) ++nth;
        }
        if (sharing == 1) {
            *out = {own, true};
            return true;
        }
    }
    platform::Rect area;
    if (!work_area(display, &area)) return false;
    *out = {split_part(area, nth, sharing, c.side), false};
    return true;
}

bool place() {
    Held& h = held();
    Place p;
    if (!find_place(&p)) return false;
    h.want = p;
    if (!platform::place_window(p.rect)) return false;
    if (p.whole) platform::set_fullscreen(platform::FullscreenMode::Borderless);   // of the display it is on now; refused: logged, it covers the display as it is
    h.mode = platform::fullscreen_mode();
    if (!platform::window_rect(&h.got)) h.got = p.rect;
    g().limiter.reset_schedule();
    apply_frame_cap(g().settings);   // another display can mean another refresh rate
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
    const platform::Rect& want = h.want.rect;
    if (h.placed && h.got != want)
        NXLOG_W(kTag, "split screen: the window system put window %d at %d,%d %dx%d, not at its place %d,%d %dx%d: it is left there", c.window, h.got.x,
                h.got.y, h.got.w, h.got.h, want.x, want.y, want.w, want.h);
    if (!c.displays.empty() && c.displays[static_cast<size_t>(c.window) - 1] > platform::display_count())
        NXLOG_W(kTag, "split screen: there is no display %d for window %d: it is on the primary one", c.displays[static_cast<size_t>(c.window) - 1], c.window);
    // the window was made unable to take the focus (the game's start-up names that in its WindowDesc); from here a click gives it the keyboard
    if (st.window) platform::set_window_focusable(true);
    NXLOG_I(kTag, "split screen: window %d of %d, %s, %s, sound %s, %s %d,%d %dx%d", c.window, c.count,
            h.want.whole ? "a screen of its own" : c.side ? "side by side" : "stacked",
            c.pad ? ("controller " + std::to_string(c.pad)).c_str() : "no controller (keyboard and mouse)", c.mute ? "off" : "on",
            h.placed ? "at" : "not placed, wanted", h.placed ? h.got.x : want.x, h.placed ? h.got.y : want.y,
            h.placed ? h.got.w : want.w, h.placed ? h.got.h : want.h);
}

void split_keep(uint64_t now) {
    Held& h = held();
    if (!h.placed || now < h.next_check_ns) return;
    h.next_check_ns = now + 250 * kMs;
    if (platform::minimized()) return;      // the player's own doing; it is put back when it is restored somewhere else
    platform::Rect cur;
    if (!platform::window_rect(&cur)) return;
    const bool moved = cur != h.got || platform::fullscreen_mode() != h.mode;
    // the place itself changes with the displays and their work areas (a taskbar that moved, another resolution, a display that came or
    // went) and with [display] monitor
    Place p;
    const bool other_place = find_place(&p) && p != h.want;
    if (!moved && !other_place) return;
    NXLOG_D(kTag, "split screen: the window is at %d,%d %dx%d, not in its place: putting it back", cur.x, cur.y, cur.w, cur.h);
    if (!place()) h.placed = false;   // refused now: not asked again four times a second
}

}  // namespace detail
}  // namespace nxrt::play
