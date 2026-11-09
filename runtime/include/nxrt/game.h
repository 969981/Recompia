// SPDX-License-Identifier: GPL-3.0-or-later
// The game boundary (docs/GAME_BOUNDARY.md): everything one title supplies to the game-independent base.
//
// The runtime and the test tooling know no game. A game module (runtime/games/<id>/, a static library the game exe links)
// fills one Info and registers it with set_current() before anything reads it: before nxrt::init() in the game exe and at the top of the
// test mains. Every part is optional. A null table or an empty string turns off what needs it (no hooks, no
// probes, neutral title facts), so a new title can start with the identity alone and add tables as its hooks are found.
//
// What each table is for, and where the Pokopia one lives:
//   title_defaults   services::TitleProfile defaults (main.npdm, control.nacp); ini keys still override them    games/pkp/pkp_core.cpp
//   timestep         the sites of the real-time frame step (nxrt/timestep.h, Sites)                           (Pokopia sets none)
//   probes           test-tooling game-state probes and script steps (nxrt/tt.h, GameProbes)                  games/pkp/pkp_tt_probe.cpp
//   play_defaults    factory controls.ini text and the bindings added by later revisions (play_types.h)       games/pkp/pkp_data.cpp
//   play             play-mode hooks: aim, camera, display, menu pointer, F1 entries (play_game.h)            games/pkp/pkp_game.cpp
//   prompts          the game's button icon textures (play/prompt_resolve.h, PromptGame)                      (Pokopia sets none)
//   poll_sleeps      SleepThread calls inside the game's own polling loops, shortened (PollSleep below)        games/pkp/pkp_core.cpp
//   settings_doc     the keys of the game's own game-ini section, described (nxrt/setting_doc.h)              games/pkp/pkp_settings_table.cpp
// The registry is header-only, so programs that do not link the runtime library (the launcher's tools) use it too.
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt {
namespace setting_doc { struct Row; }
namespace services { struct TitleProfile; }
namespace timestep { struct Sites; }
namespace tt { struct GameProbes; }
namespace play { struct GamePlay; struct GameDefaults; struct PromptGame; }
}  // namespace nxrt

namespace nxrt::game {

// One nn::os::SleepThread call that sits in a polling loop of the game: the loop re-checks its condition after every sleep, so a shorter
// sleep only checks more often (a 1 s poll for work that takes milliseconds costs up to a second each time). `ret` is the return address
// of the call (the instruction after the bl), as an offset in main; `ns` the sleep it gets instead, when that is shorter than the request.
// The kernel's SleepThread looks the caller up while [sched] poll_sleeps is on (default on).
struct PollSleep {
    uint64_t ret = 0;
    int64_t ns = 0;
};

// The console the game was built for. Switch 2 games run on the same Horizon model with a few platform facts that differ
// (system tick 31.25 MHz instead of 19.2 MHz, the NVN2 graphics API).
enum class Platform : uint8_t { Switch1, Switch2 };

struct Info {
    // ---- identity ----
    const char* id = "";                    // short id, also the stem of the game's file names: "pkp"
    const char* title = "";                 // the game's own name: "Pokémon Pokopia"
    const char* product = "";               // the PC port's name in window titles and dialogs: "Pokémon Pokopia PC"
    const char* short_title = "";           // a short name for file headers: "Pokopia" (empty: product)
    uint64_t program_id = 0;                // the Switch title id
    const char* version = "";               // the game version the hooks were found in: "2.0.1"
    uint64_t main_base = 0x7100000000ull;   // where main is mapped (the lifter's base for it): hook offsets are relative to this
    uint32_t languages = 0;                 // control.nacp SupportedLanguage: bit n = services::Language n (0: American English only)
    Platform platform = Platform::Switch1;  // the console the game targets

    // ---- files and branding ----
    const char* exe_name = "";              // the game exe: "pkp.exe"
    const char* game_ini = "";              // the settings file the game exe reads: "pkp.ini"
    const char* shader_pack = "";           // the shader pack file: "shader_cache.pkpshc"
    const char* save_folder = "";           // save data folder under %APPDATA%: "PokopiaRecompPC" (empty: "nxrt")

    // ---- game data ----
    // The RomFS of the supported version holds at least this many files and bytes (0: not checked). A RomFS with less is missing files and
    // the game fails at an unpredictable point when it reaches one (a null read at start for Pokopia, fork #128), so the exe stops at start
    // with a message instead. Extra files (desktop.ini and the like) pass.
    uint32_t romfs_min_files = 0;
    uint64_t romfs_min_bytes = 0;

    // ---- tables (null = the feature is off) ----
    void (*title_defaults)(services::TitleProfile& p) = nullptr;
    const timestep::Sites* timestep = nullptr;
    const tt::GameProbes* probes = nullptr;
    const play::GameDefaults* play_defaults = nullptr;
    const play::GamePlay* play = nullptr;
    const play::PromptGame* prompts = nullptr;
    const PollSleep* poll_sleeps = nullptr;
    uint32_t poll_sleep_count = 0;
    // The keys the game reads from its own sections of the game ini ([pkp]), one row each: the docs (runtime/docs/CONFIG.md) and the
    // launcher's schema of the game ini are generated from it (apps/settings_doc.cpp). Pure data: the pure-data library can set it.
    std::vector<setting_doc::Row> (*settings_doc)() = nullptr;
};

// The registered game, or nullptr. One slot per program (the function-local static is shared by every translation unit that inlines it).
inline const Info*& current_slot() {
    static const Info* slot = nullptr;
    return slot;
}
inline void set_current(const Info* g) { current_slot() = g; }
inline const Info* current() { return current_slot(); }
// The registered game, or an empty Info (every table null) when none is.
inline const Info& info() {
    static const Info none;
    const Info* g = current();
    return g ? *g : none;
}

}  // namespace nxrt::game
