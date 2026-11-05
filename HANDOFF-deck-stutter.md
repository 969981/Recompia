# Handoff: Steam Deck stutter rework (paste this into the local session)

You are the **Project Manager** for a rework of the project's performance on the Steam Deck and other handhelds in `featureJosh/pokopia-recomp`. A cloud session started this. It moved here because the cloud container couldn't reach the Steam Decks: it allowed web traffic only, so SSH timed out. This session runs on the local network and **can SSH into the Decks and handhelds**. Use them.

Nothing below has been implemented yet. **Before starting Phase 1**, confirm with me that you've read this whole file. Also check the SSH access listed in §6.

---

## 1. The goal, in my words

- At the moment we reach high frame rates, but we **stutter a lot**. The stutter comes from loading and other memory-related work: how we load shaders, pool and memory imports, texture uploads, and so on.
- It happens when **walking to new areas** and when **spinning the camera**. A plain 360° camera spin stutters, and the GitHub issues confirm this.
- **I much prefer stable fps and frame pacing over high numbers, for the whole project.** Smoothness comes before everything else. The stutter is worst on handhelds (Steam Deck), and Windows and PC have it to a smaller degree.
- **Fix all stutters.** How is up to you, within this brief.

## 2. Decisions already made

| Topic | Decision |
|---|---|
| Existing features | **Don't let them hold you back.** Some existing features, including the Proton-specific ones, are part of the reason it stutters. You may rework, overhaul or remove them if the evidence supports it. We don't always get it right the first time. |
| Main suspect | **Lazy pool** (`[nvn] pool_lazy`, auto-on under Wine, `src/gfx/nvn/nvn_gpu_pool.cpp`). Cells are imported the first time a draw touches them, which fits stutter in new areas. Imports are dropped after `pool_lazy_idle` presents and then imported again, which fits the camera-spin stutter. Every research agent must cover the lazy pool explicitly, with evidence. |
| Shaders | **Never block a frame on a shader or pipeline compile.** The default is asynchronous compiling; pop-in is acceptable ("a pop-in isn't bad"). Add an **opt-in player setting that compiles everything up front**, however long that takes. I'd rather wait 20 hours to play for 30 minutes, and I'd accept a 60 GB shader cache for a 20 GB game, to never compile a shader during play. Plan for its disk size, its progress UI or log, and resuming if it's interrupted. |
| Frame cap | Steam Deck runs **locked at 40** (current default) or **30**, with steady pacing. A locked lower rate beats an unlocked higher one. |
| Windows CI | A build-only Windows CI job for our branches is **allowed but not a priority**. PR checks were removed on purpose in #127, so don't add any to `main`. |
| Agent budget | **Start with 15 agents**; more is fine within reason. (I ran 30 agents yesterday to take what we could from `pokopia-native`.) |
| Testing | **I have a separate set of agents that test every PR on 3 Steam Decks** and post comments. That happens only **after** a PR is opened. You also have SSH to the Decks here (§6) for the baseline and for checking things yourself. |

## 3. Facts about the repo (checked in the cloud session)

- The runtime builds for **native Windows x64 only** (`runtime/CMakeLists.txt` stops on anything else). It builds with clang-cl and Ninja via `runtime\scripts\build.ps1`, and tests run with `ctest --test-dir build\release`. On the Deck the game (`mpr.exe`) runs **through Proton**. The launcher does not run under Proton yet: setup happens on Windows and the game folder is copied over.
- The game files are not in the repo. The supported game version is 2.0.1a.
- Rough current frame rates measured by players: Steam Deck 26–53 fps, Legion Go S 55–75 fps.
- **Existing stutter-related features** (see `runtime/docs/CONFIG.md`). Any of these may be the problem:
  - `[nvn] submit_thread`: auto under Wine. It moves submits and presents to the `nvn-submit` thread; Deck went from 26 to 53 fps.
  - `[nvn] pool_lazy`, `pool_lazy_chunk_mb`, `pool_lazy_idle`, `pool_import_chunk_mb`, `pool_mirror`: lazy and chunked memory-pool imports. This was the fix for RADV and amdgpu re-walking userptr pages on every submission.
  - `[memory] heap_section`: under Wine the heap is private memory, not a section, so there are no alias or private views.
  - `[nvn] shader_pack`: the zstd preload pack, `shader_cache/spirv.preload.pack`. Boot shader wait on the Deck dropped from 63 s to about 11 s.
  - `[nvn] shader_order`, `src/gfx/nvn/nvn_prewarm.cpp`, `nvn_gpl.cpp` (graphics pipeline library), `nvn_pipeline.cpp`, `nvn_shaders.cpp`, and `[nvn] compile_eco` (compile thread settings).
  - Texture path: `nvn_texup.cpp`, `nvn_texprep.cpp`, `nvn_untile_shader.inc`, `nvn_texdata.cpp`.
  - Pacing: `src/gfx/frame_limiter.cpp`, `fps_stats.cpp`, and `[sched] precise_sleep` (**off by default**; under Wine the timer wakes ~0.4–0.5 ms late per 1 ms sleep), plus `yield_backoff_cap_us` and `power_throttling`.
  - Handheld detection: `[host] handheld` (`src/host_device.cpp`). Device quality presets: F1 Graphics page, #122, `games/pkp/pkp_quality.cpp`. The Steam Deck preset is x1 view distance, LOD bias 0, anisotropy 4, 75% render scale, 40 fps limit.
  - Other hitch causes to check: file I/O through wineserver, which is slow from a microSD card.
- **Measurement tools:**
  - `tools/test/stutter_probe.py` runs a scripted gameplay walk and reports p50, p95, p99, max, and the counts over 50 and 100 ms. It uses `ctypes.WinDLL`, so it is **Windows-only as written**: run it inside the Proton prefix, or adapt it for the Deck.
  - Related tools: `tools/test/hifps_probe.py`, `hifps_timing.py`, `gameplay.py`.
  - Trace output goes to `trace.jsonl`, with trace zones such as `submit.*` and gauges such as `nvn.pool.lazy_mb`, alongside `mpr.log`.
- **Project conventions you must keep:**
  - Every setting gets a row in `runtime/docs/CONFIG.md`: section, key, default, env var, source file, and a full explanation including measurements.
  - Proton-specific behaviour uses the `auto` = on under Wine pattern.
  - Borrowing from `pokopia-native` (MIT) is fine with credit in `CREDITS.md`, as already done for `precise_sleep`.
  - Update `CHANGELOG.md` for changes players will notice.

## 4. Workflow (agreed)

### Phase 0: baseline on the Decks (you, over SSH)

- Before any research, record real traces on the Deck(s):
  1. a **360° camera spin** where the player starts
  2. a **walk into a new area**
  3. an area transition
  4. each of the above with a **cold** shader and pool cache and with a **warm** one
- Collect `trace.jsonl`, `mpr.log` and the frame-time stats for each.
- Find out which trace zones and gauges spike on the bad frames.
- Every research agent gets this baseline. It turns guesses from code into evidence.
- If the trace doesn't show enough, a small instrumentation-only change (more trace zones) is allowed first. Keep it separate from any fixes.

### Phase 1: four independent plans

- Spawn **2 Opus and 2 Fable (high effort)** agents **at the same time**, with the **same brief**, **without seeing each other's work**.
- One task: *how do we remove the stutter on the Steam Deck (and handhelds and PC)?*
- Each agent gets §1–§3, the baseline, and the GitHub issues on stutter and camera spin. They must read the issues.
- Every agent returns the **same structure**:
  - an inventory of stutter sources, each with `file:line` evidence and the baseline trace evidence
  - the root cause of each
  - the proposed fix, including whether an existing feature is reworked or removed, and why
  - expected effect, risk, and the effect on Windows
  - how to measure it on the Deck
  - estimated difficulty, as a model tier
- The lazy pool section is **mandatory**.

### Phase 2: cross-reference and pick the best

- Spawn **1 Opus and 1 Fable** judge. They work independently and don't see each other's verdict.
- They check **every claim in all four plans against the code and the baseline**. Claims that don't hold up are dropped.
- Each produces a **merged plan**: the strongest plan as the base, plus the checked findings from the other three, and a note on what was dropped and why.
- I (the PM) reconcile the two judges. **Disagreements on anything important go to the user**; don't settle them quietly.

### Phase 3: PLAN.md

- The PM writes `PLAN.md` and commits it to the integration branch. This is **not a loose plan or general guidance.** It must be detailed enough to rule out confusion or misinterpretation.
- For each work package:
  - the exact files, functions and data structures it touches
  - the exact new behaviour and expected outcome
  - new, changed and removed config keys, with defaults and env vars
  - the `CONFIG.md`, `CHANGELOG.md` and `CREDITS.md` changes
  - threading and synchronization rules
  - what happens on failure
  - what changes on Windows versus Proton
  - acceptance criteria with numbers, and a Deck measurement protocol
  - dependencies on other packages, and which wave it goes in
  - its **model tier: Fable, Opus or Sonnet** (Sonnet is the default)
- It also includes a global definition of "stutter-free" with numbers. Proposed: at the 40 fps lock (25 ms), over the Phase 0 scenarios with a warm cache, **no frame over 2× target**, p99 within 10% of the target, and even frame pacing. With the opt-in precompile, the same holds on a cold run. Finalise these numbers from the baseline.
- **The user approves PLAN.md before any implementation starts.**

### Phase 4: implementation with a dynamic workflow

- Use dynamic `/workflows` to spawn agents per work package. The model depends on difficulty and risk:
  - **Fable** for the hard architecture: shader and pipeline compiling, and the memory-pool rework and threading.
  - **Opus** for medium-risk packages.
  - **Sonnet** for well-specified mechanical work.
- Each agent works in its **own git worktree and branch**, `claude/deck-stutter/wpNN-<name>`, and opens a **PR into the integration branch `claude/steamdeck-performance-stuttering-dsg64a`, never into `main`**.
- Packages that touch the same files go in **separate waves**, so agents don't conflict.
- Every PR body has a **"Deviations from PLAN.md"** section. "None" is a valid entry.

### Review rules (strict; the PM is the gatekeeper)

- The PM reviews every PR **line by line against PLAN.md**, especially Sonnet PRs.
- **If the result is even one line off from what's expected, reject the PR** and give exact notes on what to fix.
- If the agent is **badly off the mark, prune it and delete the branch**.
- **Exception:** the agent explains why it didn't meet the plan, or shows it went beyond it. If the explanation holds and the code is good, it can be merged. Code that's backed up by a good explanation is fine; unexplained drift is not.
- **Rejecting is better than merging bad code.**
- After **2 rejections**, prune the agent and give the package to the **next tier up**.
- **Order for each PR:**
  1. The agent opens the PR.
  2. The PM reviews it strictly and loops until it matches.
  3. The user's Deck-testing agents test it on 3 Decks and comment.
  4. The implementing agent fixes against those comments, and the PM re-reviews.
  5. The PM merges only when the PM approves **and** the Deck testing has passed.
- At the end there is **one PR from the integration branch to `main`** for the user. Don't merge into `main` yourself.

## 5. Ground rules

- Development happens on `claude/steamdeck-performance-stuttering-dsg64a`, which is the integration branch. Agent branches come off it.
- No PR to `main` unless the user asks.
- No model identifiers in commits, PRs or code comments.
- Don't invent measurements. Every claimed improvement needs a Deck number behind it.

## 6. Remote access to the devices

- The Decks run `sshd`. Use **key authentication**, and don't put passwords in chat or in files.
- On the local network there is **no need to forward port 22 to the internet**. If the forward to the public IP from the cloud session is still open, close it.
- Fill in before starting, or ask the user:

| Device | Host / IP (LAN) | User | Notes (OLED/LCD, SteamOS version, storage: internal/microSD) |
|---|---|---|---|
| Steam Deck 1 | | `deck` | |
| Steam Deck 2 | | `deck` | |
| Steam Deck 3 | | `deck` | |
| Other handhelds (e.g. Legion Go S) | | | |

- On each device, find out:
  - where the game folder is
  - which Proton version runs it, and how to launch `mpr.exe` from the shell. That's usually Steam's `steam://rungameid/...`, or `proton run` with the right `STEAM_COMPAT_*` variables; check which works.
  - where `out/trace.jsonl` and `mpr.log` are written
  - how to clear the shader and pool caches for a cold run without deleting the player's data. **Back up anything before deleting it**, and never touch save data.
