// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the [pkp] section of the ini (pkp_settings.cpp). Every key is declared once here, with its type, default, range and
// a one-line description; the feature modules read typed values through it instead of naming the key and its default at each read.
// runtime/docs/CONFIG.md's [pkp] table is generated from the table (doc_rows below); the long notes on each key sit under it there.
// NXRT_PKP_<KEY> environment variables win over the ini (nxrt/config.h).
//
// The table is also what a row of the game's own Settings screen (pkp_options.cpp) would be built from: the type and range pick the row
// kind (switch, list, number), `choices` names a string key's values and `applies` says whether a change takes effect at once or after
// a restart. No [pkp] key has a row yet.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/setting_doc.h"

namespace nxrt::games::pkp::settings {

inline constexpr const char* kSection = "pkp";   // the game-ini section of every key

enum class Key : uint8_t {
    // play mode (pkp_play.cpp, pkp_render_size.cpp, pkp_render_targets.cpp)
    TallerFov,
    RenderOverride,
    TargetFix,
    SceneViewportFix,
    FrameTimeFix,
    EventHold,
    EventHoldMs,
    MouseCamera,
    GameFrameLimit,
    CameraEveryFrame,
    CameraGraceMs,
    // the menu pointer (pkp_menu.cpp) and the prompt icons (pkp_kbm.cpp)
    MenuMouse,
    MenuLog,
    MenuInGame,
    MenuInGameIgnore,
    MenuKeyGuides,
    KbmIcons,
    // graphics (pkp_quality.cpp, pkp_taa.cpp)
    QualityPreset,
    FogStrength,
    LodDistance,
    QualityProbe,
    GameTaa,
    // gameplay and menus
    LinkInteractEverywhere,
    LinkStoryCoop,
    UuidRepair,
    JumpMomentum,
    JumpMomentumStrength,
    CameraAim,
    CameraAimReach,
    InventorySort,
    InventoryAutosort,
    CraftSearch,
    TitleQuit,
    OptionsMenu,
    Count
};

enum class Type : uint8_t { Bool, Int, Double, String };

enum class Applies : uint8_t {
    AtStart,   // read when its feature installs: a change takes effect at the next start
    Live,      // read again while the game runs
};

struct Spec {
    Key key;
    const char* name;             // the ini key ([pkp] <name>)
    Type type;
    double def;                   // Bool (0/1), Int and Double
    const char* def_str;          // String
    double lo, hi;                // Int and Double: values are clamped into [lo, hi]; lo == hi: no range
    const char* choices;          // String: the values it takes, space-separated ("" = free text)
    Applies applies;
    const char* doc;              // one line, for a log or a Settings row's description
    bool optional = false;        // unset means "leave another value in force" (is_set), not the default: documented as unset
    // A gameplay mod: a new way of playing, a bool that is on by default and switched off by the player (opt out). Its doc row is in the
    // "gameplay" group, from which the launcher's Gameplay settings are built. A release build refuses one that defaults off
    // (PKP_GAMEPLAY_DEFAULTS_CHECK, pkp_settings_table.cpp): a test build may turn one off, a release never ships it off.
    bool gameplay = false;
};

// The table (pkp_settings_table.cpp, in the pure-data library nxrt_game_pkp_data).
const Spec& spec(Key k);
// Every key as a doc row (nxrt/setting_doc.h): game::Info::settings_doc, from which runtime/docs/CONFIG.md's [pkp] table and
// runtime/docs/pkp_ini_schema.json are generated (runtime/scripts/gen_settings_doc.ps1).
std::vector<setting_doc::Row> doc_rows();

// The value in force: env, then the ini, then the default; Int and Double clamped to the range. A read through the wrong getter for the
// key's type is a programming error (logged once, the default returned).
bool get_bool(Key k);
int64_t get_int(Key k);
double get_double(Key k);
std::string get_str(Key k);
// Set by the env or the ini (a key whose absence means "leave the game's / another page's value": quality_preset, lod_distance).
bool is_set(Key k);

}  // namespace nxrt::games::pkp::settings
