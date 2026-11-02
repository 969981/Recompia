# PKP Recomp

Pokémon Pokopia (Switch 2) recompiled to run natively on Windows.

No release yet; build it yourself for now.

You need your own extracted copy of the game, version 2.0.1a. Not affiliated with Nintendo, The Pokémon Company or Koei Tecmo.

## Requirements

- Windows 10 or 11, 64-bit
- GPU with Vulkan 1.3
- 16 GB of RAM
- To build: Visual Studio 2022 Build Tools or newer, LLVM, CMake, Ninja, Python 3.13, .NET 8 SDK, vcpkg with `libvpx:x64-windows-static`, Rust and Node.js (launcher)

## Usage

1. Copy the game's `exefs` files to `game\dump\exefs\`.
2. In a Developer Command Prompt, run `recomp\scripts\build_game.cmd pkp`.
3. Run `pwsh -File game-build\scripts\build.ps1`.
4. Build the shader cache from your `romfs` ([tools/shaders/README.md](tools/shaders/README.md)).
5. Build the launcher in `launcher\` (`npm install`, `npm run tauri build`), point it at your `romfs` and press Play.

F1 opens the settings in game. Middle mouse toggles Joy-Con 2 mouse mode for placing items.

## Reporting problems

[Open an issue](../../issues/new/choose) with `pkp.log` from the session where it happened, your GPU and your Windows version.

## Contributing

- Open pull requests against `main` and say what you tested on which GPU.
- Code for this game goes in `recomp/games/pkp/` and `runtime/games/pkp/`; the rest is shared with other games.
- Runtime tests: `pwsh -File runtime\scripts\build.ps1 -Test`.

## Credits

Built on [nxrecomp](https://github.com/drakolordx7/nxrecomp). Uses code from [Ryujinx](https://github.com/Ryubing/Ryujinx), [Mesa](https://gitlab.freedesktop.org/mesa/mesa), smo-recomp, ScarletNX, ssbu-recomp, SuperMarioWonderRecomp, yuzu, Eden, libnx, [astc-encoder](https://github.com/ARM-software/astc-encoder), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [libvpx](https://chromium.googlesource.com/webm/libvpx), AMD FidelityFX, NVIDIA Image Scaling, [SDL3](https://github.com/libsdl-org/SDL) and [Tauri](https://tauri.app), plus [Kenney input prompts](https://kenney.nl/assets/input-prompts) and DejaVu fonts. Licences are in `runtime/THIRD_PARTY_NOTICES.md`, `recomp/THIRD_PARTY.md` and `tools/shaders/licenses/`.

## License

GPL-3.0. Third-party code keeps its own licence.
