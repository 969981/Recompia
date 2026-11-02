# PKP Recomp

An unofficial native Windows version of Pokémon Pokopia (Nintendo Switch 2), made by static recompilation. The game code is generated and compiled on your own PC from your own copy of the game.

**No download yet.** There is no release until the build can be packaged without any game code in it (see [Releases](#releases)). For now you can build it yourself.

No game files, game code, keys or artwork are included, and none will be. You need your own copy of the game, version 2.0.1a, already extracted. This project is not affiliated with Nintendo, The Pokémon Company, Game Freak, Creatures or Koei Tecmo. Read [LEGAL.md](LEGAL.md) before using it.

## Requirements

- Windows 10 or 11, 64-bit
- A GPU with Vulkan 1.3
- 16 GB of RAM (a session uses about 6 GB)
- Several GB of free disk space for the generated code, the build and the shader cache
- Your own extracted copy of the game: the `exefs` (main, sdk, subsdk0, rtld) and `romfs` folders of the base game with update 2.0.1a applied. This project doesn't explain how to get them and doesn't decrypt anything.

To build:
- Visual Studio 2022 Build Tools or newer (C++ workload, Windows SDK)
- LLVM (clang-cl, lld)
- CMake and Ninja
- Python 3.13
- the .NET 8 SDK (shader translator)
- vcpkg with `libvpx:x64-windows-static` (video playback)
- Rust and Node.js (launcher only)

## Usage

1. Put the game's `exefs` files in `game\dump\exefs\` inside your clone.
2. Lift and compile the game code, from a Developer Command Prompt:
   `recomp\scripts\build_game.cmd pkp`
   This writes the generated C++ and libraries to `work\` and never leaves your PC.
3. Build the executable:
   `pwsh -File game-build\scripts\build.ps1`
4. Translate the game's shaders into `shader_cache\spirv\` beside `pkp.exe`: build the translator and run it over your `romfs`, as described in [tools/shaders/README.md](tools/shaders/README.md) (`tools\shaders\ampere\extract_corpus.py`, then `spirv_harness --batch`).
5. Start the launcher (`launcher\`, built with `npm install` then `npm run tauri build`). Point it at your `romfs` folder and press Play. Or run `pkp.exe` with `pkp.ini` set up as described in [runtime/docs/CONFIG.md](runtime/docs/CONFIG.md).

In game, F1 opens the settings: display, graphics, controls and audio. The mouse turns the camera directly, and middle mouse switches the Joy-Con 2 mouse mode on and off for placing items.

Do not share anything from `game\` or `work\`, or a `pkp.exe` you built. They contain code generated from the game.

## Releases

Planned: a release with only the runtime, the launcher and the tools, plus a builder that generates and compiles the game code on your PC from your files. Until that package is ready and checked, nothing is released. Releases will only ever be published on this page. Any other download of this project is not from us.

## Reporting problems

[Open an issue](../../issues/new/choose) with `pkp.log` from the session where it happened, plus your GPU and Windows version. Never attach game files, keys, dumps, generated code or a built `pkp.exe`. Issues about pirated or modified copies are closed.

## Contributing

- `main` is the only branch. Open pull requests against it.
- Say what you tested and on which GPU.
- The code in `runtime/`, `recomp/` and `tools/` is shared with other games. Code only for this game goes in `recomp/games/pkp/` and `runtime/games/pkp/`.
- Pull requests must not contain game code, decompiler output, game data or anything copied from the game.
- Build steps are under [Usage](#usage). Runtime tests: `pwsh -File runtime\scripts\build.ps1 -Test`.

## Credits

Built on [nxrecomp](https://github.com/drakolordx7/nxrecomp) by [drakolordx7](https://github.com/drakolordx7).

Borrowed code, all under their own licences:
- Shader translation: [Ryujinx](https://github.com/Ryubing/Ryujinx) (MIT), [Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa) (MIT, encoding reference), smo-recomp, ScarletNX and ssbu-recomp (GPL-3.0), SuperMarioWonderRecomp (MIT), yuzu and Eden (GPL-2.0-or-later)
- Textures: [astc-encoder](https://github.com/ARM-software/astc-encoder), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), ISPC
- Video: [libvpx](https://chromium.googlesource.com/webm/libvpx)
- Other components: libnx (ISC), AMD FidelityFX (MIT), [SDL3](https://github.com/libsdl-org/SDL), [Tauri](https://tauri.app)
- Assets: [Kenney input prompts](https://kenney.nl/assets/input-prompts) (CC0), DejaVu fonts

Full notices are in `runtime/THIRD_PARTY_NOTICES.md`, `recomp/THIRD_PARTY.md` and `tools/shaders/licenses/`.

## License

GPL-3.0 for this project's own code ([LICENSE](LICENSE)). Third-party code keeps its own licence. The game and everything generated from it belong to their owners and are not covered by this licence. See [LEGAL.md](LEGAL.md).
