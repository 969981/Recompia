// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the whole game module as one nxrt::game::Info (pkp_game.h). Play-mode hooks come here as they are found.
#include "pkp_game.h"
#include "pkp_aim.h"
#include "pkp_craft.h"
#include "pkp_feature.h"
#include "pkp_jump.h"
#include "pkp_kbm.h"
#include "pkp_key_guide.h"
#include "pkp_layout.h"
#include "pkp_linkplay.h"
#include "pkp_menu.h"
#if NXRT_TESTTOOLS
#include "pkp_movie_test.h"
#include "pkp_taa_probe.h"
#endif
#include "pkp_options.h"
#include "pkp_play.h"
#include "pkp_quality.h"
#include "pkp_save_uuid.h"
#include "pkp_sort.h"
#include "pkp_taa.h"
#include "pkp_title.h"

#include <cstring>
#include <iterator>

#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::pkp {

namespace {

// The main loop (0x7100882c60, called from nnMain's startup) calls 0x7101fdcde4 once per iteration on the main thread: the frame boundary
// that pumps window events and samples the pad/keyboard for play mode (needs `entryhook 0x7101fdcde4` in recomp/games/pkp/main.sites).
constexpr uint64_t kFrameUpdate = 0x1fdcde4;

// ---- the feature registry ----
// Every feature module, in install order. Their frame, game-frame, settings and status callbacks run in this order too; removal runs in
// reverse. Before any of them the registry adds its game-frame tick at layout::kFrameStep, so the game-frame callbacks run before play's
// frame-rate hook at that site, as they did when that hook called them. Where the order matters:
//   save_uuid           first: its install repairs the save before the game is loaded (pkp_save_uuid.h)
//   linkplay            before play: its frame callback (the one-time check of main) runs first in play mode's frame
//   play, kbm           play's status fields come first in the status line, kbm's right after them
//   kbm, movie_test     game-frame callbacks, kbm's first (movie_test and taa_probe are test tools: in the table only with NXRT_TESTTOOLS, which
//                       a release package builds without; the registry has no stub for them)
//   title, sort, craft  key guide clients (pkp_key_guide.h): they add themselves at install, and the key guide's callbacks run in that
//                       order
//   key_guide           after its clients: its install registers the hooks they need
//   options             after title: title's frame callback runs first
// No other two features hook the same address.
constexpr const Feature* kFeatures[] = {
    &kSaveUuidFeature, &kLinkplayFeature, &kPlayFeature, &kKbmFeature,
#if NXRT_TESTTOOLS
    &kMovieTestFeature,
#endif
    &kMenuFeature, &kQualityFeature,
    &kTaaFeature,
#if NXRT_TESTTOOLS
    &kTaaProbeFeature,
#endif
    &kJumpFeature, &kAimFeature, &kTitleFeature, &kSortFeature, &kCraftFeature, &key_guide::kKeyGuideFeature,
    &kOptionsFeature,
};
constexpr size_t kFeatureCount = std::size(kFeatures);

struct Registry {
    HookSet sets[kFeatureCount];   // each feature's hooks (pkp_hook_set.h)
    HookSet tick;                  // the game-frame tick
    bool render_installed = false, installed = false;
};
Registry& reg() {
    static Registry r;
    return r;
}

// The game thread, once a frame: every feature's game-frame callback (before the frame-rate hook at the same site, pkp_frame_rate.cpp).
int hook_game_frame(::Ctx* c, uint64_t, void*) {
    for (const Feature* f : kFeatures)
        if (f->game_frame) f->game_frame(c);
    return 0;
}

void install_render_all(uint64_t base, const play::DisplaySettings& d, int window_w, int window_h) {
    Registry& r = reg();
    if (r.render_installed) return;
    for (size_t i = 0; i < kFeatureCount; ++i)
        if (kFeatures[i]->install_render) kFeatures[i]->install_render(base, d, window_w, window_h, r.sets[i]);
    r.render_installed = true;
}
void install_all(uint64_t base, play::GameHost* host, const play::Settings& s) {
    Registry& r = reg();
    if (r.installed) return;
    r.tick.add_mid(base + layout::kFrameStep, &hook_game_frame);
    for (size_t i = 0; i < kFeatureCount; ++i)
        if (kFeatures[i]->install) kFeatures[i]->install(FeatureInstall{base, host, s, r.sets[i]});
    r.installed = true;
}
void frame_all(bool in_game, double step_seconds, play::GameFrame* out) {
    for (const Feature* f : kFeatures)
        if (f->frame) f->frame(in_game, step_seconds, out);
}
void settings_changed_all(const play::Settings& s) {
    for (const Feature* f : kFeatures)
        if (f->settings_changed) f->settings_changed(s);
}
void remove_all() {
    Registry& r = reg();
    r.tick.clear();
    for (size_t i = kFeatureCount; i-- > 0;) {
        r.sets[i].clear();
        if (kFeatures[i]->remove) kFeatures[i]->remove();
    }
    r.installed = r.render_installed = false;
}
void status_all(char* buf, size_t n) {
    if (!n) return;
    buf[0] = 0;
    for (const Feature* f : kFeatures) {
        if (!f->status_text) continue;
        const size_t k = std::strlen(buf);
        if (k + 1 < n) f->status_text(buf + k, n - k);
    }
}

const play::GamePlay& game_play() {
    static const play::GamePlay t = [] {
        play::GamePlay p;
        p.frame_update = kFrameUpdate;
        p.display_options = play::kDisplayOptQuality;   // the quality preset (pkp_quality.cpp)
        p.quality_preset = &quality_preset;
        p.install_render = &install_render_all;
        p.install = &install_all;
        p.remove = &remove_all;
        p.frame = &frame_all;
        p.settings_changed = &settings_changed_all;
        p.display_live = &play_display_live;
        p.display_hook_calls = &play_display_calls;
        p.status_text = &status_all;
        p.menu_frame = &menu_frame;
        p.menu_over_value = &menu_over_value;
        p.menu_active = &menu_active;
        p.menu_in_game = &menu_in_game;
        return p;
    }();
    return t;
}

}  // namespace

const game::Info& full_info() {
    static const game::Info info = [] {
        game::Info g = core_info();
        g.play = &game_play();
        g.render = &render_info();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::pkp

void nxrt_register_game_pkp() { nxrt::game::set_current(&nxrt::games::pkp::full_info()); }
