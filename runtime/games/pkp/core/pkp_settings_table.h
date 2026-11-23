// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the table of [pkp] keys (pkp_settings.h). A header of constexpr data, so the feature registry (pkp_game.cpp) can check
// at compile time that every gameplay mod switch belongs to a feature and every mod feature names its switches. spec() and the doc rows
// are in pkp_settings_table.cpp (nxrt_game_pkp_data, which the docs generator reads without the runtime), the getters in
// pkp_settings.cpp. Defaults and ranges are the ones each module used before the table.
#pragma once
#include <cstddef>
#include <cstdint>

#include "core/pkp_settings.h"

namespace nxrt::games::pkp::settings {

namespace table {

using enum Key;
using enum Type;
using enum Applies;

constexpr Spec b(Key k, const char* name, bool def, const char* doc) { return {k, name, Bool, def ? 1.0 : 0.0, "", 0, 0, "", AtStart, doc}; }
constexpr Spec i(Key k, const char* name, int64_t def, int64_t lo, int64_t hi, const char* doc) {
    return {k, name, Int, static_cast<double>(def), "", static_cast<double>(lo), static_cast<double>(hi), "", AtStart, doc};
}
constexpr Spec d(Key k, const char* name, double def, double lo, double hi, const char* doc) { return {k, name, Double, def, "", lo, hi, "", AtStart, doc}; }
constexpr Spec s(Key k, const char* name, const char* def, const char* choices, const char* doc) { return {k, name, String, 0, def, 0, 0, choices, AtStart, doc}; }
// A setting read again while the game runs: a change takes effect at once (Applies::Live).
constexpr Spec live(Spec sp) {
    sp.applies = Live;
    return sp;
}
// Unset leaves another value in force (the getter's default is then never used): see Spec::optional.
constexpr Spec unset_by_default(Spec sp) {
    sp.optional = true;
    return sp;
}
// A gameplay mod's switch (Spec::gameplay): a bool, on by default, the player opts out. The launcher shows it in Settings > Gameplay.
constexpr Spec gameplay(Spec sp) {
    sp.gameplay = true;
    return sp;
}
// With gameplay(): a mod the repo owner made optional, off by default (the player opts in).
constexpr Spec opt_in(Spec sp) {
    sp.opt_in = true;
    return sp;
}
// With gameplay(): no launcher row (the ini still takes it).
constexpr Spec hidden(Spec sp) {
    sp.hidden = true;
    return sp;
}
// A setting of the mod switched by `parent`: in the gameplay group, shown under that switch in the launcher.
constexpr Spec under(Key parent, Spec sp) {
    sp.gameplay = true;
    sp.parent = parent;
    return sp;
}

// In Key order (checked below). Every key is read once, when its feature installs.
inline constexpr Spec kSpecs[] = {
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
    live(d(CameraShoulderSide, "camera_shoulder_side", 0.0, -2.0, 2.0,
           "Over-the-shoulder camera: how far the view is moved to the side of the character, in blocks (positive: to the right, 0: the game's own camera).")),
    live(d(CameraShoulderHeight, "camera_shoulder_height", 0.0, -1.0, 2.0,
           "Over-the-shoulder camera: how far the view is moved up, in blocks (positive: up, 0: the game's own camera).")),
    b(MenuMouse, "menu_mouse", true, "The mouse pointer in the game's menus."),
    i(MenuLog, "menu_log", 1, 0, 0, "Menu logging: 0 off, 1 what the pointer does (and the sort's and search's lists), 2 more detail, 3 the layout chain once a second."),
    b(MenuInGame, "menu_in_game", true, "The mouse pointer also works in menus opened during play."),
    s(MenuInGameIgnore, "menu_in_game_ignore", "", "", "Layouts (id/sub ...) never treated as an in-game menu."),
    s(MenuKeyGuides, "menu_key_guides", "881468973=btn_minus", "", "Key guide layouts a click sends a button for (id=action ...)."),   // gitleaks:allow (a layout id)
    b(KbmIcons, "kbm_icons", true, "Keyboard and mouse icons in the game's button prompts."),
    unset_by_default(s(QualityPreset, "quality_preset", "switch2", "switch2 high ultra steamdeck legion_go_s laptop desktop",
                       "When set, the graphics preset (wins over the F1 page's).")),
    d(FogStrength, "fog_strength", 1.0, 0.0, 1.5, "Multiplier on the height fog's amount."),
    b(ExposureScaled, "exposure_scaled", true, "The exposure histogram measures the scene at the size it is rendered (off: the full 1080p layout, slower, same brightness)."),
    live(i(SceneUpscaler, "scene_upscaler", 0, 0, 2, "On a phone, the upscaler of the 3D picture: 0 off, 1 SGSR, 2 bicubic (with [gfx] ui_native the menus stay sharp).")),
    d(LodDistance, "lod_distance", 1.0, 0.25, 8.0, "When set, multiplier on the distance where placed objects switch to their low-detail model."),
    b(QualityProbe, "quality_probe", false, "Log what the quality hooks see (diagnostics)."),
    i(GameTaa, "game_taa", 0, 0, 2, "Experimental: 1 turns on the engine's camera TAA jitter, 2 also its TemporalAA3 pass switch (0: the game's)."),
    b(LinkInteractEverywhere, "link_interact_everywhere", false, "Experimental: Link Play visitors can interact outside Palette Town."),
    // Seamless co-op: written by the launcher into the environment of one launch (NXRT_PKP_COOP, NXRT_PKP_COOP_ROLE, ...), never by a
    // Settings row. Not a gameplay mod: it is a way to start the game, not a change to how it plays.
    b(Coop, "coop", false, "Launcher only: this launch plays a seamless co-op session (with coop_role, coop_group and coop_member)."),
    unset_by_default(s(CoopRole, "coop_role", "", "host visit", "Launcher only, with coop: host opens the co-op session, visit joins it.")),
    s(CoopGroup, "coop_group", "", "", "Launcher only, with coop: the id of the co-op group this launch plays in."),
    s(CoopMember, "coop_member", "", "", "Launcher only, with coop: this player's member id in the group (32 hex digits)."),
    unset_by_default(i(CoopMembers, "coop_members", 0, 0, 16, "Launcher only, with coop: how many members the group has; the session waits for all of them (0: no waiting).")),
    b(UuidRepair, "uuid_repair", true, "At start, give a save with the player UUID earlier builds shared a UUID of its own."),
    gameplay(b(JumpMomentum, "jump_momentum", true, "A running jump keeps the running speed in the air.")),
    d(JumpMomentumStrength, "jump_momentum_strength", 1.0, 0.0, 1.0, "With jump_momentum: how much of the extra running speed is kept."),
    gameplay(b(CameraAim, "camera_aim", true, "Abilities and tools aim where the camera looks instead of where the character faces.")),
    under(CameraAim, i(CameraAimReach, "camera_aim_reach", 0, 0, 4, "With camera_aim: extra cells of reach (0: the game's own range).")),
    live(b(CameraAimDebug, "camera_aim_debug", false, "Draw camera aim's ray, the cells it tested and the cell it picked (debugging).")),
    gameplay(b(InventorySort, "inventory_sort", true, "A click on Hold to Sort in the bag and the container screens sorts at once (the game's own sort, as holding X).")),
    under(InventorySort, b(InventoryAutosort, "inventory_autosort", false, "Sort the bag each time the bag or a container screen opens.")),
    opt_in(gameplay(b(BoxTabs, "box_tabs", false, "Extra pages for standard storage boxes and extra tabs for big storage boxes. L and R switch pages. Each standard page uses one of 271 standard box spots (20 slots each). Each big box tab uses one of 256 big box spots (60 slots each). About the first 22 standard boxes and 42 big boxes placed get every extra. Later boxes get fewer or none. Off during Link Play. Items stay saved."))),
    under(BoxTabs, i(BoxTabPages, "box_tab_pages", 4, 2, 12, "With box_tabs: pages per standard storage box, including its first page (a page past this that holds items still shows).")),
    under(BoxTabs, i(BigBoxTabs, "big_box_tabs", 4, 1, 6, "Tabs per big storage box, including the box itself. At 1, only the box's 3 pages (60 slots) are enabled. At 6, up to 18 pages (360 slots) are enabled. L and R move between tabs at page boundaries. Each tab uses one of the island's 256 big box spots (60 slots each), so about the first 42 big boxes placed get every tab and later boxes get fewer or none. A tab with items stays available if you lower this. Extra tabs are off during Link Play. Items stay saved.")),
    opt_in(gameplay(b(CraftSearch, "craft_search", false, "A Search button in the crafting screen's key guide."))),
    opt_in(gameplay(b(ItemEditor, "item_editor", false, "Experimental: an Items tab in the game's Settings screen that changes the count of a stack in the bag."))),
    hidden(gameplay(b(TitleQuit, "title_quit", true, "A Quit Game key guide on the title screen."))),
    gameplay(b(PokemonCap, "pokemon_cap_50", true, "Experimental: more creatures can live in the town than the game's 25 (pokemon_cap_limit).")),
    under(PokemonCap, i(PokemonCapLimit, "pokemon_cap_limit", 31, 25, 50,
                        "With pokemon_cap_50: how many creatures can live in the town; visitors and spawns share the same room, and the game draws at most 50 creatures.")),
    b(OptionsMenu, "options_menu", true, "The PC settings as tabs and rows of the game's own Settings screen."),
    s(EventsFile, "events_file", "pokopia_events.tsv", "", "The Events tab's list: one event per line, start date, end date and name, separated by tabs. No file: the built-in list of the game's events."),
    gameplay(b(SaveNow, "save_now", true, "The pause menu's Save prompt and the quick-save key write the save and keep the player in the world.")),
    // Not read by the runtime: the launcher reads it while the game runs, so the Online tab of the game's Settings and the launcher share it.
    live(b(OnlinePresence, "online_presence", false, "Launcher only (online.rs): while the game runs, tell the Recompia online server that this player is online, with the launcher's online name, version, Windows or Linux and the creatures-at-home count. Sends data off the PC, so off by default.")),
    // Read by the network services at start (game::Info::report_online, pkp_game.cpp). On by default so events that need a connection
    // start on their date; online services other than the clock and nifm answers still fail.
    b(OnlineEvents, "online_events", true, "The game is told it has an internet connection, so events that need one the first time (Apples to Applin) can start. Nothing is sent: online services still fail."),
    live(i(Resolution, "resolution", 0, 0, 100, "On a phone, the 3D picture's resolution in percent of the screen per side, 15 to 100 (0: worked out once from [display] render_scale and [upscale] scene_scale, the keys before it).")),
    live(s(DynamicResolutionMode, "dynamic_resolution_mode", "gpu", "gpu thermal", "On a phone with Dynamic Resolution: gpu follows the GPU's time per frame; thermal also caps the resolution by the phone's thermal headroom (dynamic_resolution_headroom, same section), so the GPU can run at lower clocks before the phone throttles.")),
    live(d(DynamicResolutionHeadroom, "dynamic_resolution_headroom", 0.85, 0.5, 0.95, "With dynamic_resolution_mode = thermal: the thermal headroom to hold (1.0 = the severe throttling status; about 0.033 per degree C); the device's light-throttling threshold minus 0.03 when lower.")),
    live(s(DynamicResolutionState, "dynamic_resolution_state", "", "", "Written by a phone's Dynamic Resolution: what it learned (the settled percent, its floor, the thermal cap) for one screen, mode, Resolution, upscaler and frame cap, so the next start begins there. Cleared by deleting it."))
};
static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == static_cast<size_t>(Count), "one Spec per Key");

// Keys of older builds, read and ignored without a warning (an old pkp.ini keeps loading); the launcher never writes them.
inline constexpr const char* kRetired[] = {
    "link_story_coop",   // Story co-op (every player's own story), replaced by seamless co-op (coop)
};

constexpr bool in_key_order() {
    for (size_t n = 0; n < static_cast<size_t>(Count); ++n)
        if (static_cast<size_t>(kSpecs[n].key) != n) return false;
    return true;
}
static_assert(in_key_order(), "kSpecs in Key order");

constexpr const Spec& at(Key k) { return kSpecs[static_cast<size_t>(k)]; }

// A mod's switch: a gameplay row that is not the setting of another switch.
constexpr bool is_switch(Key k) { return k < Count && at(k).gameplay && at(k).parent == Count; }

// The mod markers fit together: a switch is a bool whose default matches opt_in; opt_in and hidden only on a switch; a mod setting's
// parent is a switch (never itself a setting, never hidden: the launcher shows the setting under it).
constexpr bool mods_ok() {
    for (const Spec& sp : kSpecs) {
        if (!sp.gameplay) {
            if (sp.opt_in || sp.hidden || sp.parent != Count) return false;
        } else if (sp.parent == Count) {
            if (sp.type != Bool || (sp.opt_in && sp.def != 0)) return false;
        } else {
            if (sp.opt_in || sp.hidden || !is_switch(sp.parent) || at(sp.parent).hidden) return false;
        }
    }
    return true;
}
static_assert(mods_ok(), "[pkp] gameplay markers: a switch is a bool (opt_in: default off), a setting's parent is a shown switch");

// Release check (PKP_GAMEPLAY_DEFAULTS_CHECK, pkp_settings_table.cpp): every switch the repo owner did not make opt_in defaults on.
constexpr bool switches_default_on() {
    for (const Spec& sp : kSpecs)
        if (sp.gameplay && sp.parent == Count && !sp.opt_in && sp.def == 0) return false;
    return true;
}

}  // namespace table

}  // namespace nxrt::games::pkp::settings
