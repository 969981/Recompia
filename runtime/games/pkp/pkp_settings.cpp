// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the [pkp] keys (pkp_settings.h). Defaults and ranges are the ones each module used before the table; see
// runtime/docs/CONFIG.md for the full description of each key.
#include "pkp_settings.h"

#include <algorithm>
#include <atomic>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::settings {

namespace {

constexpr const char* kSection = "pkp";
constexpr const char* kTag = "pkp";

using enum Key;
using enum Type;
using enum Applies;

constexpr Spec b(Key k, const char* name, bool def, const char* doc) { return {k, name, Bool, def ? 1.0 : 0.0, "", 0, 0, "", AtStart, doc}; }
constexpr Spec i(Key k, const char* name, int64_t def, int64_t lo, int64_t hi, const char* doc) {
    return {k, name, Int, static_cast<double>(def), "", static_cast<double>(lo), static_cast<double>(hi), "", AtStart, doc};
}
constexpr Spec d(Key k, const char* name, double def, double lo, double hi, const char* doc) { return {k, name, Double, def, "", lo, hi, "", AtStart, doc}; }
constexpr Spec s(Key k, const char* name, const char* def, const char* choices, const char* doc) { return {k, name, String, 0, def, 0, 0, choices, AtStart, doc}; }

// In Key order (checked below). Every key is read once, when its feature installs.
constexpr Spec kSpecs[] = {
    b(TallerFov, "taller_fov", true, "Screens taller than 16:9: the taller view through the camera's FOV and aspect (off: 16:9 with bars)."),
    b(RenderOverride, "render_override", true, "Render the scene at the chosen internal size instead of the game's 1920x1080 layout."),
    s(TargetFix, "target_fix", "screen", "off screen all", "Render targets made at a literal 1920x1080 follow the render size: screen-relative ones, all, or none."),
    b(SceneViewportFix, "scene_viewport_fix", true, "Keep the 3D scene's viewport whole when it renders below its targets' size."),
    b(FrameTimeFix, "frame_time_fix", true, "Above 60 fps, the game's 60 Hz steps run at the right speed."),
    b(EventHold, "event_hold", true, "With frame_time_fix: hold the game at 60 Hz steps while the camera cannot turn (events, loading)."),
    i(EventHoldMs, "event_hold_ms", 1500, 0, 10000, "How long the event hold stays after free play resumes, in ms."),
    b(MouseCamera, "mouse_camera", true, "Turn the follow camera with the raw mouse."),
    b(GameFrameLimit, "game_frame_limit", false, "Keep the game's own 60 Hz frame wait."),
    b(CameraEveryFrame, "camera_every_frame", true, "Place the camera every rendered frame instead of on the game's 120 Hz tick."),
    i(CameraGraceMs, "camera_grace_ms", 250, 0, 5000, "How long after the camera's turn branch last ran the mouse still turns the camera, in ms."),
    b(MenuMouse, "menu_mouse", true, "The mouse pointer in the game's menus."),
    i(MenuLog, "menu_log", 1, 0, 0, "Menu logging: 0 off, 1 what the pointer does (and the sort's and search's lists), 2 more detail, 3 the layout chain once a second."),
    b(MenuInGame, "menu_in_game", true, "The mouse pointer also works in menus opened during play."),
    s(MenuInGameIgnore, "menu_in_game_ignore", "", "", "Layouts (id/sub ...) never treated as an in-game menu."),
    s(MenuKeyGuides, "menu_key_guides", "881468973=btn_minus", "", "Key guide layouts a click sends a button for (id=action ...)."),   // gitleaks:allow (a layout id)
    b(KbmIcons, "kbm_icons", true, "Keyboard and mouse icons in the game's button prompts."),
    s(QualityPreset, "quality_preset", "switch2", "switch2 high ultra steamdeck legion_go_s laptop desktop",
      "When set, the graphics preset (wins over the F1 page's)."),
    d(FogStrength, "fog_strength", 1.0, 0.0, 1.5, "Multiplier on the height fog's amount."),
    d(LodDistance, "lod_distance", 1.0, 0.25, 8.0, "When set, multiplier on the distance where placed objects switch to their low-detail model."),
    b(QualityProbe, "quality_probe", false, "Log what the quality hooks see (diagnostics)."),
    i(GameTaa, "game_taa", 0, 0, 2, "Experimental: 1 turns on the engine's camera TAA jitter, 2 also its TemporalAA3 pass switch (0: the game's)."),
    b(LinkInteractEverywhere, "link_interact_everywhere", false, "Experimental: Link Play visitors can interact outside Palette Town."),
    b(LinkStoryCoop, "link_story_coop", false, "Experimental: Link Play visitors can take part in the host's story."),
    b(UuidRepair, "uuid_repair", true, "At start, give a save with the player UUID earlier builds shared a UUID of its own."),
    b(JumpMomentum, "jump_momentum", true, "A running jump keeps the running speed in the air."),
    d(JumpMomentumStrength, "jump_momentum_strength", 1.0, 0.0, 1.0, "With jump_momentum: how much of the extra running speed is kept."),
    b(CameraAim, "camera_aim", true, "Abilities and tools aim where the camera looks instead of where the character faces."),
    i(CameraAimReach, "camera_aim_reach", 2, 0, 4, "With camera_aim: extra cells of reach."),
    b(InventorySort, "inventory_sort", true, "A Sort button in the bag's and the container screens' key guide."),
    b(InventoryAutosort, "inventory_autosort", false, "Sort the bag each time the bag or a container screen opens."),
    b(CraftSearch, "craft_search", true, "A Search button in the crafting screen's key guide."),
    b(TitleQuit, "title_quit", true, "A Quit Game key guide on the title screen."),
    b(OptionsMenu, "options_menu", true, "The PC settings as tabs and rows of the game's own Settings screen."),
};
static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == static_cast<size_t>(Count), "one Spec per Key");

constexpr bool in_key_order() {
    for (size_t n = 0; n < static_cast<size_t>(Count); ++n)
        if (static_cast<size_t>(kSpecs[n].key) != n) return false;
    return true;
}
static_assert(in_key_order(), "kSpecs in Key order");

std::atomic<uint64_t> g_misread{0};   // one bit per key read through the wrong getter, logged once

bool typed(const Spec& sp, Type t) {
    if (sp.type == t) return true;
    const uint64_t bit = uint64_t{1} << static_cast<unsigned>(sp.key);
    if (!(g_misread.fetch_or(bit, std::memory_order_relaxed) & bit)) NXLOG_W(kTag, "settings: [pkp] %s read as the wrong type", sp.name);
    return false;
}

double clamp(const Spec& sp, double v) { return sp.lo < sp.hi ? std::clamp(v, sp.lo, sp.hi) : v; }

}  // namespace

static_assert(static_cast<size_t>(Key::Count) <= 64, "g_misread holds one bit per key");

const Spec& spec(Key k) { return kSpecs[static_cast<size_t>(k)]; }

bool get_bool(Key k) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::Bool)) return sp.def != 0;
    return config_get_bool(kSection, sp.name, sp.def != 0);
}

int64_t get_int(Key k) {
    const Spec& sp = spec(k);
    const int64_t def = static_cast<int64_t>(sp.def);
    if (!typed(sp, Type::Int)) return def;
    const int64_t v = config_get_int(kSection, sp.name, def);
    return sp.lo < sp.hi ? std::clamp(v, static_cast<int64_t>(sp.lo), static_cast<int64_t>(sp.hi)) : v;
}

double get_double(Key k) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::Double)) return sp.def;
    return clamp(sp, config_get_double(kSection, sp.name, sp.def));
}

std::string get_str(Key k) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::String)) return sp.def_str;
    return config_get_str(kSection, sp.name, sp.def_str);
}

bool is_set(Key k) { return config_has(kSection, spec(k).name); }

}  // namespace nxrt::games::pkp::settings
