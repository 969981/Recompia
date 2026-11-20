// SPDX-License-Identifier: GPL-3.0-or-later
// SDL3 platform layer: a window at a place the caller chooses (platform.h: display_work_area, place_window, window_rect,
// set_window_focusable). Split screen keeps its windows this way (src/gfx/play/play_split.cpp). The window is platform.cpp's, reached
// through native_window(); same thread as everything else in the platform layer.
#include "nxrt/platform.h"

#include <SDL3/SDL.h>

#include "nxrt/log.h"

namespace nxrt::platform {
namespace {

constexpr const char* kTag = "platform";

SDL_Window* window() { return static_cast<SDL_Window*>(native_window()); }

}  // namespace

bool display_work_area(int number, Rect* out) {
    if (!out) return false;
    SDL_DisplayID id = 0;
    if (number == 0) {
        id = SDL_GetPrimaryDisplay();
    } else if (number >= 1) {
        int n = 0;
        if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {   // the system's order, as display_count and display_name number them
            if (number <= n) id = ids[number - 1];
            SDL_free(ids);
        }
    }
    SDL_Rect r{};
    if (!id || !SDL_GetDisplayUsableBounds(id, &r) || r.w <= 0 || r.h <= 0) return false;
    *out = {r.x, r.y, r.w, r.h};
    return true;
}

bool place_window(const Rect& r) {
    SDL_Window* w = window();
    if (!w || r.w <= 0 || r.h <= 0) return false;
    if (fullscreen_mode() != FullscreenMode::Windowed && !set_fullscreen(FullscreenMode::Windowed)) return false;
    if (SDL_GetWindowFlags(w) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) SDL_RestoreWindow(w);
    // Whether this window system lets a window choose its position is asked with the position it has, before anything changes (SDL's
    // Wayland back end refuses every position).
    int x = 0, y = 0;
    if (!SDL_GetWindowPosition(w, &x, &y) || !SDL_SetWindowPosition(w, x, y)) {
        NXLOG_W(kTag, "place_window(%d,%d %dx%d): the window system places windows itself: %s", r.x, r.y, r.w, r.h, SDL_GetError());
        return false;
    }
    SDL_SetWindowBordered(w, false);
    // A window manager fits each request to the window as it is at that moment: KWin keeps a window that still has its old, larger size
    // inside the work area (so it left the lower half), and limits a new size to the display the window is on (so a window coming from
    // a narrower display stayed narrower). One pass gets the display and the size right, the second the position.
    for (int pass = 0; pass < 2; ++pass) {
        if (!SDL_SetWindowPosition(w, r.x, r.y)) NXLOG_W(kTag, "place_window: position %d,%d refused: %s", r.x, r.y, SDL_GetError());
        if (!SDL_SetWindowSize(w, r.w, r.h)) NXLOG_W(kTag, "place_window: size %dx%d refused: %s", r.w, r.h, SDL_GetError());
        SDL_SyncWindow(w);
        Rect now;
        if (window_rect(&now) && now == r) break;
    }
    return true;
}

bool window_rect(Rect* out) {
    SDL_Window* w = window();
    if (!w || !out) return false;
    Rect r;
    if (!SDL_GetWindowPosition(w, &r.x, &r.y) || !SDL_GetWindowSize(w, &r.w, &r.h)) return false;
    *out = r;
    return true;
}

void set_window_focusable(bool on) {
    SDL_Window* w = window();
    if (w && !SDL_SetWindowFocusable(w, on)) NXLOG_W(kTag, "set_window_focusable(%d) failed: %s", on ? 1 : 0, SDL_GetError());
}

}  // namespace nxrt::platform
