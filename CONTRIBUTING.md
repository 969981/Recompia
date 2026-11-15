# Contributing

## How we build

Always build code cleanly, modularly, and with proper architecture and separation of concerns. Never simply bolt on a feature because someone asked for it. If you can't find an easy way to build it naturally, and you'd recommend a refactor to make the whole thing cleanly, stop your work ahead of time and say so.

## Pull requests

- Open pull requests against `main` and say what they change.
- Release notes are the version's section of [CHANGELOG.md](CHANGELOG.md); write it from [.github/RELEASE_TEMPLATE.md](.github/RELEASE_TEMPLATE.md).
- Code for this game goes in `recomp/games/pkp/` and `runtime/games/pkp/`; the rest is shared with other games.

## Changes that make players compile the game again

Every player builds the game code on their own PC, and a full compile is 15 minutes on a fast PC and an hour on a slow one. Set up
keeps each compiled unit in a cache, so an update compiles only the units whose key changed. A change to any of these can change keys:

- `recomp/nxrecomp/` (the lifter) and `recomp/games/pkp/` (`config.py`, `main.sites`): the units whose generated text changes
- `recomp/include/`: every unit, unless the change is a comment or a branch the x86-64 build does not take
- the compile flags (`recomp/nxrecomp/compile_flags.py`) or the bundled Zig: every unit
- the key formula in `game-build/release_b/pkp_build.py` (`KEY_SCHEME`): every unit

Before merging one, measure it and put the number in the pull request:

    python recomp/scripts/rebuild_impact.py --exefs <your dump>/exefs --zig <the bundled zig>

It prints how many of the 284 units a player updating from the last release compiles again. A change that costs a few units ships
any time. Changes that cost most of the game are collected and shipped together in one release, not one after another.

## Keeping up with pokopia-native

[pokopia-native](https://github.com/xFractals/pokopia-native) is this project's parent. Every update it publishes gets mirrored here:

1. Read its commits since the last reviewed one (named at the top of [CHANGELOG.md](CHANGELOG.md)): `git remote add native https://github.com/xFractals/pokopia-native.git`, `git fetch native`, `git log <last reviewed>..native/main`.
2. For each change that touches the game, the runtime, the builder or the launcher, decide: we already have it, take it, or skip it (Mac/iPhone-only, or not how this project works). Its runtime is different code, so taking a change usually means writing the same fix in ours.
3. One pull request per change taken, saying "Based on pokopia-native (MIT) `<path>` @`<commit>`"; copied code gets a header comment naming the file and commit.
4. Only a version that works is merged; behaviour changes for all players go behind their own ini key unless they are plain bug fixes.
5. Move the "last reviewed" commit at the top of the changelog, and the commit named in `runtime/THIRD_PARTY_NOTICES.md` if code was copied.

## Building from source

Needs Visual Studio 2022 Build Tools or newer (with the C++ x64 tools and CMake), LLVM 23, PowerShell 7, Python 3.13 with [uv](https://docs.astral.sh/uv/), sccache, .NET 10 SDK, the Vulkan SDK (validation layers), vcpkg with `libvpx:x64-windows-static` (`C:\vcpkg` or `VCPKG_ROOT`), Rust and Node.js. The scripts find Visual Studio with vswhere.

First build, from the repository root:

1. `uv venv tools\.venv --python 3.13` and `uv pip install --python tools\.venv -r recomp\requirements.txt`
2. `pwsh -File tools\textures\scripts\fetch_third_party.ps1`, then `pwsh -File tools\textures\scripts\build.ps1` (the runtime links its `astc2bc7.lib`)
3. Put the dump in `game\dump\` as the launcher expects it (`updates\<version>\exefs` and `romfs`) or the exefs files in `game\dump\exefs\`, then `recomp\scripts\build_game.cmd pkp`
4. `pwsh -File game-build\scripts\build.ps1`

- Shaders: [tools/shaders/README.md](tools/shaders/README.md). Launcher: `npm install`, `npm run tauri build` in `launcher\`.
