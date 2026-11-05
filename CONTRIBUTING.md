# Contributing

- Open pull requests against `main` and say what they change.
- Code for this game goes in `recomp/games/pkp/` and `runtime/games/pkp/`; the rest is shared with other games.

## Building from source

Needs Visual Studio 2022 Build Tools or newer (with the C++ x64 tools and CMake), LLVM 23, PowerShell 7, Python 3.13 with [uv](https://docs.astral.sh/uv/), sccache, .NET 10 SDK, the Vulkan SDK (graphics test validation), vcpkg with `libvpx:x64-windows-static` (`C:\vcpkg` or `VCPKG_ROOT`), Rust and Node.js. The scripts find Visual Studio with vswhere.

First build, from the repository root:

1. `uv venv tools\.venv --python 3.13` and `uv pip install --python tools\.venv -r recomp\requirements.txt`
2. `pwsh -File tools\textures\scripts\fetch_third_party.ps1`, then `pwsh -File tools\textures\scripts\build.ps1` (the runtime links its `astc2bc7.lib`)
3. Put the dump in `game\dump\` as the launcher expects it (`updates\<version>\exefs` and `romfs`) or the exefs files in `game\dump\exefs\`, then `recomp\scripts\build_game.cmd pkp`
4. `pwsh -File game-build\scripts\build.ps1`

- Shaders: [tools/shaders/README.md](tools/shaders/README.md). Launcher: `npm install`, `npm run tauri build` in `launcher\`.
- Runtime tests: `pwsh -File runtime\scripts\build.ps1 -Test`.
