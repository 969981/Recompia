# Pokopia Launcher

Tauri 2 launcher for the Pokopia PC port (`pkp.exe`). It ships no game files and never changes yours. It reads the game
files you extracted from your own copy, writes `pkp.ini` next to `pkp.exe` and builds the shader cache from your romfs.

## First run

1. **Game files**: pick the dump folder (`<dump>\base\{exefs,romfs}` and/or `<dump>\updates\<version>\{exefs,romfs}`).
   The newest complete update is used, otherwise the base. "Use these game files" writes `[fs] exefs` and `[fs] romfs`.
2. **Set up**: writes the `pkp.ini` paths (`[fs] exefs`, `[fs] romfs`, `[fs] save_dir`, `[nvn] shader_cache =
   shader_cache/spirv`) and builds `shader_cache\spirv\` next to `pkp.exe`:
   - Extract: `tools\shaders\extract_corpus.py` finds every shader in the romfs and in `exefs\main` and writes the
     distinct blocks to `shader_cache\corpus\` (about 10 GB, deleted when setup finishes).
   - Translate: `tools\spirv_harness\spirv_harness.exe --batch` in chunks of 1000 into `shader_cache\spirv\`
     (`<hash>.spv` + `results.tsv`, about 26 GB for the 99,012 blocks of 2.0.1). The used shaders go first: the
     blocks named in `tools\shaders\used_shaders.txt` (about 6,100 of the corpus) and in the install's own
     `shader_order.txt`. Set up stops after those (`ready` in setup.json) and Play turns on.
   - The other ~93,000 blocks are translated in the background while the launcher is open: idle priority, half the
     cores, chunks of 250. It stops while the game or its warm-up runs (the game reads `results.tsv` once at start, so
     a translation made during play would only cost frames), has Pause / Resume buttons, and continues at the next
     launch after the launcher was closed. A shader the game needs before it is translated is drawn without it in that
     run, dumped by `[nvn] dump_missing` and translated when the game exits, as before. The corpus is deleted once
     everything is translated.
   A progress bar shows each step. Pause stops the tools and Continue setup picks up where they stopped: a block listed
   in `results.tsv` is never translated twice, and extraction is skipped if it already ran for the same romfs. The tools
   run at below-normal priority.
   State: `shader_cache\setup.json`.
   Settings, Launcher, "Translate every shader before Play" (`full_shader_setup` in the launcher's config) brings back
   the old behaviour: Set up translates all blocks (used ones first) and Play waits for all of them.
3. **Play** turns on once the used shaders are translated (or all of them, with the option above). The launcher starts
   pkp.exe with no paths on the command line. A `pkp.ini`
   next to it makes its folder the working directory, and every path comes from the ini (relative paths start there).

`release\Play.bat` starts the game the same way without the launcher (after setup).

## Release model (b): pkp.exe built on the player's PC

When `tools\build\pkp_build.py` is present (the package from the main project's release script), the release holds no
`pkp.exe`. Set up first builds it from the chosen dump (`src-tauri/src/gamebuild.rs`): verify the ExeFS by hash, lift,
compile with the bundled Zig, link against `pkp_rt.dll`; then the shader steps below run. Play stays off until both are
done. Pause works in every step and Continue setup resumes. A built pkp.exe counts only while `build\done.json` matches
the tools of the release in use (`pkp_build.py --check`: lifter, `main.sites`, compile flags, SDK files, `pkp_rt.lib`), so
an update extracted over an install that changes any of them turns Play off with "This update changes the game code"
until Set up rebuilds pkp.exe (the shader cache is kept). Headless check of the whole first run:
`cargo run --release --example first_run -- <game dir> <exefs> <romfs> <tools dir> [shader limit] [jobs]`.

## Release folder

The launcher looks for its tools in `tools\` next to its own exe. A release folder must contain:

```
pkp.exe                     the game (game-build, -Release)
PokopiaLauncher.exe         this launcher (npm run tauri build)
Play.bat                    release\Play.bat (optional)
tools\
  spirv_harness\            self-contained publish of nx tools/shaders/harness:
                              dotnet publish tools\shaders\harness\SpirvHarness.csproj -c Release -r win-x64
                                --self-contained true -o <release>\tools\spirv_harness     (~84 MB)
                            (stage the Ryujinx sources first: tools\shaders\scripts\setup_ryujinx.ps1)
  shaders\extract_corpus.py nx tools/shaders/ampere/extract_corpus.py
  shaders\used_shaders.txt  game-build/release_b/used_shaders.txt: hashes of the shaders play runs used (no shader
                            code), made by tools/shaders/scripts/used_shaders.py (how: see its header). Optional:
                            without it Set up translates everything before Play, as before.
  python\python.exe         optional: Python 3 (for example the embeddable package) with the lz4 package installed.
                            Without it the first python.exe on PATH that can `import lz4.block` is used.
```

If a file is missing, setup stops and names it. `pkp.exe` can live in another folder (Settings, game folder), but
`tools\` always sits next to the launcher.

## Development

- `cargo check` in `src-tauri`; `npm run tauri build` for the exe.
- `cargo run --example shader_setup -- <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]` runs the setup's
  shader steps without the window (same code, `src-tauri/src/shaders.rs`); `limit` translates only that many blocks.
