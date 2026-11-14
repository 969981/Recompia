# Pokopia Launcher

Tauri 2 launcher for the Pokopia PC port (`pkp.exe`). It ships no game files and never changes yours. It reads the game
files you extracted from your own copy, writes `pkp.ini` next to `pkp.exe` and builds the shader cache from your romfs.

## First run

1. **Game files**: pick the dump folder (`<dump>\base\{exefs,romfs}` and/or `<dump>\updates\<version>\{exefs,romfs}`).
   The newest complete update is used, otherwise the base. "Use these game files" writes `[fs] exefs` and `[fs] romfs`.
2. **Set up**: writes the `pkp.ini` paths (`[fs] exefs`, `[fs] romfs`, `[fs] save_dir`, `[nvn] shader_cache =
   shader_cache/spirv`) and builds `shader_cache\spirv\` next to `pkp.exe`:
   - Extract: `tools\shaders\extract_corpus.py` finds every shader in the romfs and in `exefs\main` and writes the
     distinct blocks to `shader_cache\corpus\` (a few pack files, about 5 GB, deleted when setup finishes).
   - Translate: `tools\spirv_harness\spirv_harness.exe --serve`, one process for the whole translation that takes a chunk of 7000
     blocks at a time and writes each block's `<hash>.spv` and its row of `results.tsv` into `shader_cache\stage\` as the block
     finishes (about 1.8 GB per chunk; 26 GB of SPIR-V for the 99,012 blocks of 2.0.1).
   - Commit: the chunk's rows are appended to `shader_cache\spirv\results.tsv`, which is what makes a block count as translated,
     after its SPIR-V is in the cache.
   - Pack (on by default, "Packed shader files" on the Set up page): each chunk goes from the stage straight into
     `shader_cache\spirv.archive\NNNNNN.pack` (zstd segments the runtime reads, `src-tauri/src/shader_archive.rs`), so a fresh
     translation is never written to `shader_cache\spirv\` loose. It runs beside the translator: a chunk is packed while the next
     one is translated, on the cores the translator leaves. On Linux the stage is a RAM filesystem (`/dev/shm`) when it has
     room and is writable, so on a Steam Deck's card a chunk costs one compressed write and no loose write at all; where it has
     not, the stage is the game folder. With packing off, the chunk is moved into `shader_cache\spirv\` instead. A cache that
     already has loose files keeps them until the player packs it.
   A progress bar shows each step. Pause stops the tools and Continue setup picks up where they stopped: a block listed
   in `results.tsv` is never translated twice, and extraction is skipped if it already ran for the same romfs. The tools
   run at below-normal priority.
   State: `shader_cache\setup.json`.
   Before it starts, Set up checks the free disk space on the game drive (it refuses to start without about 14 GB with packed shader files or 31 GB with loose ones, less
   when resumed), Vulkan 1.3 (vulkan-1.dll and a GPU), RAM, and warns about 13th/14th generation Intel desktop CPUs
   whose microcode is older than 0x12B (a BIOS update fixes their crashes under long loads). Warnings can be continued
   past. "Use fewer cores" runs setup with a quarter of the threads (2 to 8) at idle priority.
   **Shader files** (Settings > Game, and a question at launcher start while loose files are left): "Free up space" moves the loose
   `.spv` files into the archive without translating anything (a segment is flushed and renamed before its loose files are deleted;
   it can be stopped and continued). "Use loose files" writes them back out. Neither runs while the game, setup or an update runs.
   Everything setup does is appended to `setup.log` next to `pkp.exe` (UTC timestamps, the commands, the tools' error
   output; rotated to `setup.log.prev` at 2 MB). The Set up card has an "Open setup log" button; attach the file to
   bug reports about a failed setup.
3. **Play** turns on once setup has finished. The launcher starts pkp.exe with no paths on the command line. A `pkp.ini`
   next to it makes its folder the working directory, and every path comes from the ini (relative paths start there).

`release\Play.bat` starts the game the same way without the launcher (after setup).
`PokopiaLauncher --play [--game <folder>]` does what Play does without the window (it also prepares the run-time shaders
before the start and translates the new ones after it) and waits for the game: for a Steam, Sunshine or desktop shortcut.
The game folder is the launcher's own folder unless `--game` names another.

## Release model (b): pkp.exe built on the player's PC

When `tools\build\pkp_build.py` is present (the package from the main project's release script), the release holds no
`pkp.exe`. Set up first builds it from the chosen dump (`src-tauri/src/gamebuild.rs`): verify the ExeFS by hash, lift,
compile with the bundled Zig, link against `pkp_rt.dll`; then the shader steps below run. Play stays off until both are
done. Pause works in every step and Continue setup resumes. A built pkp.exe counts only while `build\done.json` matches
the tools of the release in use (`pkp_build.py --check`: lifter, `main.sites`, compile flags, SDK files, `pkp_rt.lib`), so
an update extracted over an install that changes any of them turns Play off with "This update changes the game code"
until Set up rebuilds pkp.exe (the shader cache is kept). The one-click update avoids that wait: between unpacking and
swapping its files it runs the downloaded package's own `pkp_build.py --prepare` (`src-tauri/src/update_game.rs`), which
builds into `build
ext` with the shared object cache while the installed game stays playable; the swap waits for the game
to close, then `--adopt` moves the prepared build into place (journaled, so a power cut resumes it) and keeps the old one in
`build\prev` until the new launcher runs (`--revert` if it does not). Cancel before the swap changes nothing, and the next
update continues from the cache. Installs never set up, development trees and releases whose script has no `--prepare`
skip the step. Headless check of the whole first run:
`cargo run --release --example first_run -- <game dir> <exefs> <romfs> <tools dir> [shader limit] [jobs]`.

## Release folder

The launcher looks for its tools in `tools\` next to its own exe. A release folder must contain:

```
pkp.exe                     the game (game-build)
PokopiaLauncher.exe         this launcher (npm run tauri build)
Play.bat                    release\Play.bat (optional)
tools\
  spirv_harness\            self-contained publish of nx tools/shaders/harness:
                              dotnet publish tools\shaders\harness\SpirvHarness.csproj -c Release -r win-x64
                                --self-contained true -o <release>\tools\spirv_harness     (~84 MB)
                            (stage the Ryujinx sources first: tools\shaders\scripts\setup_ryujinx.ps1)
  shaders\extract_corpus.py nx tools/shaders/ampere/extract_corpus.py
  python\python.exe         optional: Python 3 (for example the embeddable package) with the lz4 package installed.
                            Without it the first python.exe on PATH that can `import lz4.block` is used.
```

If a file is missing, setup stops and names it. `pkp.exe` can live in another folder (Settings, game folder), but
`tools\` always sits next to the launcher.

## Save Editor (PokoHex)

The Save Editor page (`src/pages/saveedit.js`) shows [PokoHex](https://github.com/berichan/PokoHex) (GPL-3.0), a Blazor
WebAssembly save editor, in a frame. It is built into the launcher's own files, so it needs no internet and no extra file in
the release folder. Before `npm run tauri build`, run (needs git and the .NET 10 SDK):

```
sh scripts/build-pokohex.sh
```

It clones PokoHex at the commit in `pokohex/PIN`, applies `pokohex/launcher-host.patch` (host mode: PokoHex offers **Load Game
save** and **Write to Game** when the page that frames it provides `window.pokohexHost`) and publishes it to `public/pokohex/`
(git-ignored), which Vite copies into the build. Without it the page says the editor was not built in. Load and Write go to
`save_edit_read` / `save_edit_write` (`src-tauri/src/saveedit.rs`): only `G00/` and `S00/` travel; a write stages a copy of the
live save with the edited files, makes a `before-edit` backup and swaps it in like a restore (`savebackup::swap_in`). Both need
the game closed. To move to a newer PokoHex, change `pokohex/PIN` and refresh the patch against it.

## Linux (SteamOS, the Steam Deck, Bazzite, other distributions)

The launcher also builds as a native Linux program (`npm run tauri build -- --no-bundle` on Linux, or an AppImage with
`--bundles appimage`). Tauri draws it with WebKitGTK there, so it needs no WebView2, which Proton lacks (the Windows launcher
does not start under Proton). Releases ship it as `PokopiaRecomp-<version>-linux-x64.tar.gz`: the Windows package with
`PokopiaLauncher.AppImage` in place of `PokopiaLauncher.exe` (built by the `linux` job of `.github/workflows/release.yml`). A
build of your own goes in the release folder next to `tools/`. The game stays the Windows
`pkp.exe`; what differs on Linux is in `src-tauri/src/host.rs`:

- **Build tools:** the first Set up downloads Python 3.13 (python-build-standalone) with the lz4 and capstone wheels, and
  Zig 0.17.0 (the version the release bundles for Windows), into `tools/linux/`, each pinned by size and SHA-256.
  `pkp_build.py` compiles with that Zig and reads free memory from `/proc/meminfo` there.
- **Native, no Proton:** the shader translator (`tools/harness/spirv_harness`, a self-contained .NET publish for linux-x64) and
  `pkp` (the hidden first run and Play) are Linux programs and run as such, with plain paths and no runner in front of them.
  Pause kills the tool's process group; the hidden first run is stopped if it is still going 4 minutes after its 60 s.
  Every run gets GPU driver shader caches of the game's own that are never trimmed
  (`~/.local/share/pokopia-recomp/shadercache/{nvidia,mesa}`), as Steam sets them up for the games it starts.
- **The checks before Set up** read statvfs, `/proc` and `libvulkan.so.1`.
- **Desktop:** Link Play lists addresses from getifaddrs and `/proc/net/route` (Tailscale and ZeroTier as overlays; bridges,
  veth and VPN interfaces skipped), and the firewall hint names firewalld; save import looks in `~/.local/share/eden` and
  `~/.config/Ryujinx`; files and links open with `xdg-open`; on NVIDIA the launcher sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` (a
  blank window otherwise).
- **AppImage:** `tools/` is looked up next to the `.AppImage` file (`$APPIMAGE`), and the programs the launcher starts get the
  environment without the variables the AppImage runtime points into its mount.
- **Building** needs Rust, Node.js and the webkit2gtk-4.1, gtk3 and libsoup3 development packages. Built on Ubuntu 22.04 the
  binary and the AppImage need only glibc 2.35.

Tested on Bazzite 45 (Ryzen 9 9900X, 16 GB, RTX 4070 Ti, Proton Experimental in the Steam Linux Runtime 4; that run was before
the native port, and nothing goes through Proton now), end to end with a
2.0.1 dump: Set up built pkp.exe in 12 minutes, translated the 99,012 shaders in 17 minutes (also paused and continued in the
middle), the hidden first run collected 212 shaders, and Play ran the game; from the native binary and from the AppImage.

## Development

- `cargo check` in `src-tauri`; `npm run tauri build` for the exe.
- `cargo run --example shader_setup -- <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]` runs the setup's
  shader steps without the window (same code, `src-tauri/src/shaders.rs`); `limit` translates only that many blocks.
