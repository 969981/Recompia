# Changelog

This fork builds on [the original Pokopia Recomp](https://github.com/drakolordx7/pokopia-recomp) 0.1.1. You still need your own copy
of the game (version 2.0.1a): the download contains no game files.

## Unreleased

### What's new
- New quality presets on the F1 Graphics page for your device: Steam Deck, Legion Go S, Laptop and Desktop PC (#122). Next to
  view distance and texture sharpness they also pick a render scale and a frame limit (Steam Deck 40 fps, Legion Go S and
  Laptop 60 fps, Desktop your display's refresh rate). Hover a preset to see what it sets. Changing one of those settings
  afterwards switches to Custom, as before. The render scale part applies after a restart.
- Steam Deck / Linux: the game's GPU memory is copied instead of shared with the graphics driver (`[nvn] pool_copy`),
  which removes a cost every frame paid for the shared memory. Not yet measured on a Deck. `pool_copy = off` goes back.

### Fixes
- Taking a picture with the Pokédex no longer crashes the game with "The graphics device was lost" (#121). A copy from an image
  into the game's memory could write past the end of a GPU buffer; such a copy is now skipped (and logged). Pictures taken at a
  higher render scale are also scaled down to the size the game asked for, instead of the top-left part of the screen.

## 0.5.0 (build I: Iguana)

### What's new
- Multiplayer (Link Play) is part of every release now. A new Link Play page in the launcher: play alone, host on this PC or visit
  a friend. Hosting shows the address to give out (LAN, Tailscale or ZeroTier) with Copy buttons, warns when Windows treats your
  network as Public, and gives you a room code. In the game, pick Local connection. Each player needs their own save: a copied save
  can't join its original.
- Room codes: a room hosted with a code is hidden from everyone else and only players with the code can join. Rooms without a code
  work as before on a LAN. Every player and the host need this version (older ones are refused with a message).
- Game traffic can travel over UDP to the relay, so a lossy internet link no longer stalls everything. TCP stays as the fallback
  (`[net] udp = off` to always use TCP).
- Steam Deck, Legion Go S and other 16:10 screens: with `aspect = auto` the picture fills the whole screen with the taller view, and
  objects and players at the right height (#73). Not yet verified in the world. `taller_fov = false` under `[pkp]` goes back to bars.
- Import your save from Eden or Ryujinx (or any folder) on the Settings page. Your current save is moved to a dated backup first.
- Set up: a setup log (Open setup log button), checks before it starts (free disk space, Vulkan, memory, a warning for Intel
  13th/14th gen desktop CPUs), and a "fewer cores" option that keeps the PC usable while it builds.
- "Set up again" is much faster after an update: compiled game code is kept and reused (up to 3 GB, `--cache-mb`).
- .nup dumps: checked against their checksums, the version is checked before the big unpack, free space is checked, and the unpack
  can be cancelled.
- Clearer dump errors: files copied over FTP in text mode, an older game version or a base-only dump now tell you what to do.
- Both sticks reach full speed by default (outer zone 0.92, new `right_stick_max`). Saved settings keep their value.
- A way out of the game besides Alt+F4 (#70): hold Select and Start (View and Menu on an Xbox pad) together, or press "Quit game" on
  the F1 page, and confirm with A.
- Steam Deck, Legion Go S and other Linux / SteamOS players: drawing work is handed to the graphics card from a separate thread by
  default (`[nvn] submit_thread = auto`); about 26 -> 53 fps on a Steam Deck (measured by a player).

### Fixes
- Saves: if a save can't be written (full disk, a file locked by OneDrive or antivirus), the last good save is kept and a notice
  says so. Before, a half-written save could replace it.
- Sound comes back after unplugging headphones, a Bluetooth reconnect or the monitor turning off, instead of staying silent.
- Windows 11 no longer slows the game down when its window is behind other windows or on a second monitor.
- Graphics: particles, light culling and a volume effect get their real buffer sizes; depth-of-field blur and lens flares get the
  right instance; volumetric fog sees its real size; shadow tests on default textures match the console.
- A rare hang in the game's waiting loops is prevented (needs Set up again).
- Safer on GPUs that lack some formats (blending, filtering and vertex formats are checked; signed 10-bit textures fall back).
- Faster pipeline lookups (less CPU per frame).

### For developers
- New opt-in keys, all off: `[sched] precise_sleep`, `[sched] precise_waits`, `[sched] yield_backoff`, `[diag] yield_census`,
  `[nvn] shader_key_check`, `[nvn] reduced_precision`. On by default: `[sched] power_throttling`, `[nvn] pipe_last`. See CONFIG.md.
- Releases run the launcher tests first and ship SHA256SUMS.txt; pull requests are scanned for private text.
- Uses code and ideas from pokopia-native (MIT), credited in CREDITS.md.

## 0.4.1 (build H: Hedgehog, hotfix)

### Fixes
- Steam Deck, Legion Go S and other 16:10 screens: trees, rocks, stairs, players and Pokémon are drawn at the right height again. They
  used to float above the screen centre and sink into the ground below it, and only settled when you walked up to them. With
  `aspect = auto` the picture now stays 16:9 with thin bars on screens taller than 16:9 (#73). `aspect = 16:10` still gives the full
  taller view, with the old placement problem, until it is fixed properly.

## 0.4.0 (build H: Hedgehog)

### What's new
- Much faster on the Legion Go S: in the world at its native 1920x1200, with no upscaler, about 55 -> 75 fps (+36%), and the slowest
  frames are smoother (1% low 31 -> 38 fps). Measured on this release build, three before/after pairs.
- Windows PCs gain too: the game needs about 20% less CPU time per frame to hand its drawing work to the graphics card (RX 9060 XT:
  6.9 -> 5.5 ms per frame).
- Each speed-up has its own setting in `pkp.ini` (see the developer notes below), so a problem on one machine can be switched off
  without losing the rest.
- The launcher shows an "Update available" banner with a "What's new" link when a newer release is out. One request at start,
  silent when offline, nothing is downloaded.

### Fixes
- Render resolutions of about 3800x2140 and up (4K) no longer crash at start: the game's graphics memory now grows with the render
  size. 1920x1080 and below are unchanged. Very large sizes (above about 5300x2990) are lowered to the largest that fits, with a
  warning in the log. Setup builds pkp.exe again once (new hook sites).
- Xbox and PlayStation button prompts now show in the game when picked in the settings or by "auto" (before, Switch prompts stayed).
- When the graphics driver loses the GPU, the game shows one clear error and closes, instead of hanging and filling the log.
- Timed waits inside the game no longer end too early (they assumed the wrong console clock rate).

### Known issues
- The launcher does not start under Proton/Linux: it needs Microsoft's WebView2, which Proton does not include. So Steam Deck and
  Legion Go S players do not see the update banner.
- Legion Go S: about 75 fps at native, not yet a steady 90.

---

### Release notes for developers

Base: fork 0.3.0 (`9edefb8`).

- **#59** (fixes #49) `pkp_gfx_heap.cpp`: mid hooks at `0x710112f008` (pool config: general `sp+0x40`, render targets `sp+0xa0`) and `0x710112fb08`
  (graphics arena `[x3+0x48]`), registered only for game targets above 1920x1080. General pool +28 B per extra pixel, render-target
  pool only past 3500 MiB (937.6 MiB + 160 B/pixel + 1 GiB), arena by the sum; pools capped under 4 GiB, larger sizes clamped.
  `main.sites` changed: installs re-lift.
- **#40** pkp prompts: the UI atlas glyph patch (`pkp_kbm.cpp`) now covers the Xbox and PlayStation styles, not only keyboard
  (fixes #39).
- **#50** Kernel wait deadlines use the registered console tick frequency instead of 19.2 MHz; regression test with 10 ms hook slicing.
- **#51** The first `VK_ERROR_DEVICE_LOST` is latched at the Vulkan device layer: replay, acquire, submit and present stop, NVN serials
  complete on the CPU so game threads leave GPU waits, one error on the main thread, then exit.
- **#42**, **#43**, **#56** README: multiplayer work-in-progress note, new layout, link to the multiplayer tracking issue.
- **#71** Performance overhaul (runtime only; no re-lift). Each item names its ini key and default:
  - Config lookups cached per thread (`[config] cache`, on). Idea from ultratears (GPL-3.0).
  - Self-sampling draws use `VK_EXT_attachment_feedback_loop_layout` instead of a copy of the attachment (`[nvn] feedback_loop`,
    auto). Legion Go S: +9% on its own.
  - Guest memory is imported into Vulkan lazily in 16 MiB pieces when first bound under Wine (`[nvn] pool_lazy`, auto = Wine only).
  - Descriptor sets are reused when the bindings did not change (`[nvn] set_reuse`, on).
  - Fewer Vulkan calls per draw: dynamic state and bindings are only set when they change (`[nvn] bind_coalesce`, on); texture
    handles are resolved once per draw (`[nvn] resolve_once`, on) with a pool lookup cache (`[nvn] pool_cache`, on). PC: dynamic-state
    calls 1542 -> 384 per frame, resolves 4.2 -> 1.0 per draw.
  - Cheaper per replay call setup: write-watch polls read a summary bitmap (`[nvn] setup_coalesce`, on). PC: 1.5 -> 0.24 ms per frame.
  - The GPU wait before the input sample lets the GPU hold two frames instead of one (`[latency] gpu_frames_ahead`, 2; 1 = the old
    wait, one frame less latency).
  - A submit thread for NVN submissions and presents (`[nvn] submit_thread`, off: no gain at the frame rate on the Legion Go S).
  - Profiling: `[prof] gpu_passes`, `replay_fine`, `render_split` and the `ftrace_report.py` / `gpu_pass_report.py` scripts (all off).
  - Legion Go S numbers: SteamOS, Proton 11, in the world at native 1920x1200 on a time-based walk, three ABAB pairs on the release
    build: 54.0 / 55.3 / 56.9 -> 74.5 / 76.1 / 75.0 fps (mean 55.4 -> 75.2, +36%), 1% low 31.3 -> 37.5, 0 error lines.
- **#72** `launcher/src-tauri/src/update.rs`: one GET of `/repos/featureJosh/pokopia-recomp/releases/latest` at start (ureq 2 +
  rustls, 5 s timeout), semver compare against the launcher's own version; untagged builds (0.0.0, 0.1.1) skip it. The banner's link
  opens only `https://github.com/` URLs.

## 0.3.0 (build G: Gecko)

### What's new
- Name entry has an on-screen keyboard drawn in the game. It works with a controller, mouse or keyboard, including in Steam's Game Mode
  on handhelds, where the old Windows popup lost keys or never got focus.
- Faster start: after the first launch, the game packs its boot shaders into one file (about 285 MB) and reads that instead of
  thousands of small files. The first launch writes it in the background.
- Steam Deck and Legion Go S are detected. Button prompts stay as controller icons when you use the trackpad or touchscreen (the
  title screen used to show Enter for A), and automatic render scale follows the screen instead of rendering at 1080p or more.
- Setup works when the Windows user folder has accented or other non-English characters in its name (it failed after compiling).

### Known issues
- The launcher does not start under Proton/Linux: it needs Microsoft's WebView2, which Proton does not include.
- Legion Go S: 60 fps, not yet a steady 90.

---

### Release notes for developers

Base: fork 0.2.0 (`98e50b8`).

- **#31** swkbd: ImGui on-screen keyboard instead of the Win32 prompt (fork #29). The game blocks its window thread while the applet
  is open: kernel waits on that thread run a hook in 10 ms slices and the last frame is presented again with the overlay on top.
- **#32** `pkp_build.py` writes the `zig ar` response files as UTF-8 (orezraey, upstream drakolordx7/pokopia-recomp#6).
- **#33** `host_is_handheld()`: `SteamDeck=1`, SMBIOS product Jupiter/Galileo or family `Legion Go S*` (registry BIOS key, else
  Wine's `Z:\sys\class\dmi`); `[host] handheld`. Render scale auto = window size; prompt style auto keeps pad prompts on mouse
  input (based on tinfoil-chewer/pokopia-recomp-linux 8b150a4).
- **#34** `<shader_cache>.preload.pack`: the order file's SPIR-V in one zstd file (1096 -> 284 MiB), written in the background after
  a boot preload, read front to back on later boots; stale entries by results.tsv line hash; `[nvn] shader_pack`. zstd 1.5.7 pinned.
  Go S: written in 20 s, preload 2.6 -> 1.7 s (tinfoil-chewer/pokopia-recomp-linux c7a15e4).

## 0.2.0 (build F: Fennec fox)

### What's new

Builds A to F were the test builds that led to 0.2.0.

**Build A: the starting point**
- Everything from the original 0.1.1 release, plus a long list of small fixes to setup, the launcher and stability.
- Colours look right now. Before, the game looked washed out and too bright (the Pokédex was orange instead of red, Ditto was pale pink).
- AMD graphics cards no longer show black or white blocks instead of textures.
- First-time setup no longer runs out of memory while it builds the game, and a step that fails is retried.
- The launcher checks shaders before every start and warms them up, so there is less stutter the first time you see something new.

**Build B: names and menus**
- Typing your name works: before, only the first letter was kept ("Ash" became "A").
- The mouse no longer makes the menu selection jump around. Click to pick what is selected.

**Build C: Steam Deck and Linux picture fix**
- On Steam Deck, Legion Go and other Linux/Proton devices, the game showed a black screen. It now draws the game.
- The sky and backgrounds are no longer black, and there are no more bright white halos around things on those devices.

**Build D: tidier game files**
- The game files moved into the folder layout the launcher expects.

**Build E: one game file, smoother on handhelds**
- The launcher can use the game as a single `.nup` package file instead of a folder of extracted files.
- Name entry: the game accepts your name and characters say it correctly.
- New controller setting: "stick max" (F1 > Controls > Gamepad) for sticks that never reach their edge.
- First speed-up for handheld PCs on Linux/Proton.

**Build F: much faster on Legion Go S and Steam Deck class devices**
- In the game world on a Legion Go S, about 35 fps before, about 55-60 fps now.
- Windows PCs are not affected by this change.

**Also in 0.2.0**
- Playing above 1080p (for example 1440p): dark outlines of characters drawn in the wrong place, and blotchy shadows, are fixed.
- Updating: when a new version changes the game code, the launcher asks you to run Set up again (once) so the new code is built.
  Every existing install rebuilds once after this update.
- Steam Deck / Legion Go S: for more fps, lower the render scale or turn on FSR 3 in F1 > Graphics (about 70-80 fps on a
  Legion Go S at 67%).

### Known issues
- Legion Go S: 60 fps, not yet a steady 90 (being worked on).
- First setup still takes a while: about 7 minutes to build the game, then 1 to 2 hours for the shaders (you can pause and continue).

---

### Release notes for developers

Base: upstream `2c29515` (v0.1.1). Fork PRs on featureJosh/pokopia-recomp; issue numbers are fork issues.

- **#1** sweep: build, test, NVN, kernel, shader and launcher fixes (upstream tests compile again).
- **#2** spirv_harness with server GC (~4x faster translation). **#4** HFMA2/HADD2 `.H0_NH1` swaps lanes and negates the low one.
  The package's `spirv_harness` is built from this source; the upstream 0.1.1 binary does not match its source (34k of 99k shaders
  differ, washed-out colours, fork #8).
- **#3** the launcher translates shaders the game built at run time; shader warm-up run before Play.
- AMD depth: `D24S8` unsupported -> `D32S8` view fallback applied to views as well as images (black/white textures on RDNA).
- **#5** setup compiles within free memory and retries a failed unit alone.
- **#9 / #20** swkbd: SDK 0x140010 packed 0x530-byte config (use_utf8 at 0x3AA); `ngc:u` name check answered; text check in UTF-8;
  #16 reverted (fork #6).
- **#10** menus: pointer movement no longer drives the selection; click = A on the game's selection (fork #7).
- **#14** NVN: every vertex-stage output starts at (0,0,0,1) at module load (`spirv_init_outputs`, `[nvn] init_outputs`); RADV left
  unwritten components NaN -> black sky, NaN bloom halos (fork #13).
- **#18** heap: private memory under Wine so pools are imported as userptr (Wine sections are file-backed), whole-pool imports
  (fork #11). Under Proton keep `[nvn] tex_write_watch = false`: write-watch mprotect invalidates the userptr BOs.
- **#15 / #17** `.nup` package as the game source (launcher + runtime, stored zstd blocks).
- **#21** `[pad] stick_max`.
- **#22** `[nvn] submit_batch` (auto = `frame` under Wine): one kernel submission per frame, merged into the present's
  `vkQueueSubmit2`; early flush only when a thread waits on or polls a batched serial. amdgpu re-walks every userptr BO on each CS
  (~4-5 ms with 4.7 GB imported). Go S in game 33-39 -> 55-60 fps (fork #19; follow-up #23).
- **#25** viewports recorded by 0x17c0420 (64 callers) follow the render size: at render heights above 1080 the G-buffer AO
  composite landed at 1.333x (dark silhouettes, misplaced ground AO; upstream drakolordx7/pokopia-recomp#17).
- **#26** rebuild on update: pkp_build.py tool stamp (lifter, main.sites, sdk, pkp_rt.lib, flags; FORMAT 2) and `--check`; the
  launcher treats a stale build as not set up.
- Packaging: `game-build/scripts/make-release.ps1` + `.github/workflows/release.yml`; third-party payload from the upstream 0.1.1 zip.
