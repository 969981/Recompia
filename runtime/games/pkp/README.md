# Pokémon Pokopia game module

The title-specific part of the runtime (`nxrt`): everything the game-independent base needs to run Pokopia, as one `nxrt::game::Info` (`core/pkp_game.h`, `runtime/docs/GAME_BOUNDARY.md`). Nothing outside this folder and `recomp/games/pkp` names the game.

Includes are relative to this folder (`#include "core/pkp_game.h"`); no `../`. File names keep the `pkp_` prefix.

## Folders

| Folder | What belongs there |
|---|---|
| `core/` | Registration and shared plumbing: the game info (`pkp_game`, `pkp_core`, `pkp_data`), the feature registry types (`pkp_feature.h`, `pkp_hook_set.h`), guest-memory helpers, game offsets (`pkp_layout.h`), data-table and text helpers, the `[pkp]` settings (`pkp_settings*`), the save UUID repair and Link Play. |
| `render/` | Picture: render resolution and targets, aspect ratios, wide-text spacing, frame rate, graphics pools, quality presets, the engine's TAA, and `game::Info::render` (`pkp_render_info.cpp`). |
| `input/` | Play mode's hub (`pkp_play*`), keyboard and mouse prompts, the mouse camera and mouse mode, camera aim, jump momentum. |
| `ui/` | The game's menus and screens: the mouse in menus (`pkp_menu*`), the in-game Options, the key guide and its clients (title Quit Game, inventory sort, crafting search). |
| `test/` | Test-only sources: the test tooling's probes, the movie test, the TAA probe. Built only with `NXRT_TESTTOOLS`; a release package has none of them. |

## Adding a feature

A feature is one `Feature` entry (`core/pkp_feature.h`) in the table in `core/pkp_game.cpp`. Its module exports the `Feature`, puts every hook it registers into the `HookSet` the registry gives it, and reads its `[pkp]` keys through `core/pkp_settings.h` (declared once in `core/pkp_settings_table.cpp`). Install order matters in a few places; the comment above the table says where. Put the new files in the folder of their main job and list the sources in `CMakeLists.txt`.

## Libraries

`CMakeLists.txt` builds three libraries, each a superset of the one before: `nxrt_game_pkp_data` (pure data, no runtime), `nxrt_game_pkp_core` (+ title facts) and `nxrt_game_pkp` (+ play-mode features; needs the graphics back end). With `NXRT_TESTTOOLS` the `test/` sources are added to `nxrt_game_pkp_core` and `nxrt_game_pkp`.
