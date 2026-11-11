// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: what a feature module declares to the feature registry (the table in pkp_game.cpp). Each module exports one Feature;
// the registry installs them in table order, calls their per-frame and settings callbacks in the same order and removes them in reverse.
// Every hook a module registers goes into the HookSet it is given (pkp_hook_set.h) and is removed by the registry, so a module's remove
// only resets its own state. No module installs another one: what one needs of another is ordered by the table.
#pragma once
#include <cstddef>
#include <cstdint>

#include "nxrt/abi.h"
#include "core/pkp_hook_set.h"
#include "play_game.h"

namespace nxrt::games::pkp {

struct FeatureInstall {
    uint64_t base;                  // main's guest base
    play::GameHost* host;
    const play::Settings& settings;   // play mode's settings in force now
    HookSet& hooks;                 // the module's hooks (the registry keeps the set for the whole session)
};

struct Feature {
    const char* name;
    // Before play mode's frame-boundary hook (play_game.h GamePlay::install_render): display hooks. Gets the same set as install.
    void (*install_render)(uint64_t base, const play::DisplaySettings& d, int window_w, int window_h, HookSet& hooks) = nullptr;
    void (*install)(const FeatureInstall& in) = nullptr;
    void (*remove)() = nullptr;                                                   // after its hooks are gone: reset the module's state
    void (*frame)(bool in_game, double step_seconds, play::GameFrame* out) = nullptr;   // main thread, play mode's frame
    void (*game_frame)(::Ctx* c) = nullptr;                                      // the game thread, once a frame (layout::kFrameStep)
    void (*settings_changed)(const play::Settings& s) = nullptr;                 // new settings are in force
    void (*status_text)(char* buf, size_t n) = nullptr;                          // its part of the [play] log_status line
    // A layer over the picture (debug views, nxrt/overlay_canvas.h): the overlay's presenting thread, draw only after active said true.
    bool (*overlay_active)() = nullptr;
    void (*overlay_draw)(gfx::OverlayCanvas& c) = nullptr;
};

}  // namespace nxrt::games::pkp
