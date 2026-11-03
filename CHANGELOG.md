# Changelog

This fork builds on [the original Pokopia Recomp](https://github.com/drakolordx7/pokopia-recomp) 0.1.1. You still need your own copy
of the game (version 2.0.1a): the download contains no game files.

## 0.2.0 (build F)

### What's new, in plain words

Our test builds have letters (A, B, C ...). Each entry is what got better for you in that build.

**Build A: the starting point**
- Everything from the original 0.1.1 release, plus a long list of small fixes to setup, the launcher and stability.
- Colours look right now. Before, the game looked washed out and too bright (the Pokédex was orange instead of red, Ditto was pale pink).
- AMD graphics cards no longer show black or white blocks instead of textures.
- First-time setup is gentler on your PC: it no longer runs out of memory while it builds the game, and a step that fails is retried.
- The launcher checks shaders before every start and warms them up, so there is less stutter the first time you see something new.

**Build B: names and menus**
- Typing your name works: before, only the first letter was kept ("Ash" became "A").
- The mouse no longer makes the menu selection jump around. Click to pick what is selected.

**Build C: Steam Deck and Linux picture fix**
- On Steam Deck, Legion Go and other Linux/Proton devices, the game showed a black screen. It now draws the game.
- The sky and backgrounds are no longer black, and there are no more bright white halos around things on those devices.

**Build D: tidier game files**
- The game files moved into the folder layout the launcher expects. Nothing for you to do.

**Build E: one game file, smoother on handhelds**
- The launcher can use the game as a single `.nup` package file instead of a folder of extracted files.
- Name entry: the game accepts your name and characters say it correctly.
- New controller setting: "stick max" (F1 > Controls > Gamepad) for sticks that never reach their edge.
- First speed-up for handheld PCs on Linux/Proton.

**Build F: much faster on Legion Go S and Steam Deck class devices**
- In the game world on a Legion Go S, about 35 fps before, about 55-60 fps now.
- Windows PCs are not affected by this change.

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
- Packaging: `game-build/scripts/make-release.ps1` + `.github/workflows/release.yml`; third-party payload from the upstream 0.1.1 zip.
