// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: window mode changes and the fps counter push to the overlay.
#include "play_state.h"

namespace nxrt::play {
using namespace detail;
namespace detail {

const char* window_mode_name(platform::FullscreenMode m) {
    switch (m) {
        case platform::FullscreenMode::Windowed: return "windowed";
        case platform::FullscreenMode::Borderless: return "borderless";
        case platform::FullscreenMode::Exclusive: return "exclusive";
    }
    return "?";
}

void set_window_mode(const std::string& mode) {
    G& st = g();
    if (!st.window) return;
    const platform::FullscreenMode want = mode == "borderless" ? platform::FullscreenMode::Borderless
                                         : mode == "exclusive" ? platform::FullscreenMode::Exclusive
                                                               : platform::FullscreenMode::Windowed;
    if (platform::fullscreen_mode() == want) return;
    if (!platform::set_fullscreen(want)) {
        NXLOG_W(kTag, "window mode '%s' was refused", mode.c_str());
        // Exclusive fullscreen needs the display to accept a mode change (it does not on a remote or locked session, or with some drivers).
        // The next best thing is borderless at the desktop mode, which always works; the setting stays as the player chose it.
        if (want == platform::FullscreenMode::Exclusive && platform::fullscreen_mode() == platform::FullscreenMode::Windowed) {
            if (platform::set_fullscreen(platform::FullscreenMode::Borderless)) NXLOG_W(kTag, "using borderless fullscreen instead");
            else NXLOG_W(kTag, "borderless fullscreen was refused too; staying windowed");
        }
    }
    st.limiter.reset_schedule();
}

void toggle_fullscreen() {
    G& st = g();
    if (!st.window) return;
    std::string target;
    if (platform::fullscreen_mode() == platform::FullscreenMode::Windowed)
        target = st.settings.display.window_mode == "exclusive" ? "exclusive" : "borderless";
    else
        target = "windowed";
    set_window_mode(target);
    std::lock_guard<std::mutex> lk(st.mu);
    st.settings.display.window_mode = window_mode_name(platform::fullscreen_mode());
    st.dirty = true;
}

// Tells the overlay how the frame rate counter should look (the overlay draws it; nothing happens while it is off).
void apply_fps_counter(const FpsSettings& f) {
    G& st = g();
    if (!st.overlay) return;
    gfx::Overlay::CounterConfig c;
    c.show = f.show;
    c.corner = static_cast<int>(f.corner);
    c.detail = f.detail;
    c.in_screenshots = f.in_screenshots;
    st.overlay->set_counter(c);
}

}  // namespace detail
}  // namespace nxrt::play
