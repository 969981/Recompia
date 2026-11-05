# PLAN: Steam Deck stutter rework

Status: decided by the PM (owner, 2026-11-05: the PM decides, the owner reviews at the PRs; this replaces the brief's plan-approval gate
and its "disagreements go to the owner" rule). Packages start on commit.
Integration branch: `claude/steamdeck-performance-stuttering-dsg64a`. Line numbers are at `3dc1a05` and were checked against that tree;
when this file names a function and a line, the function is the reference and the line is a hint.

Paths: `nvn/` = `runtime/src/gfx/nvn/`, `play/` = `runtime/src/gfx/play/`, `ds/` = `work/deck-stutter/`. Run lines use the queue-file form of
`ds/run.sh` (`name dll script cold|warm seconds [ENV=V ...]`).

---

## 0. Goal and fixed decisions

Goal (owner): stable frame pacing over high numbers, for the whole project. Fix every stutter: walking into new areas, camera spins, area
loads, on the Steam Deck first, without making the PC (native Windows) or the Legion Go S (Proton, same path as the Deck) worse.

Fixed decisions. They are not reopened by any package; a PR that needs one changed stops and asks the PM.

| # | Decision |
|---|---|
| F1 | Base plan = O1 (both judges). Fixes both judges kept are in; one-judge fixes are in only when they make Deck play more stable or can measurably (§7 lists the rest). Every claim a judge refuted is dropped. |
| F2 | The lazy pool is replaced under Wine by copy/shadow mode, key `[nvn] pool_copy = auto\|on\|off` (`auto` = on under Wine, off on native Windows). Phase 1 = the refs note's design (lazy per-cell host-visible shadow, re-copy of what each submission touches, no write tracking, GPU writes copied back at completion, reports/queries recorded as GPU writes). Phase 2 = dirty tracking. Phase 3 = the both-wrote merge. The lazy pool stays as the A/B fallback (`pool_copy = off`) until a checkpoint passes; a final-wave package then removes the userptr-lazy path (or keeps it for native only if needed). The owner shelved an earlier copy mode on 2026-11-02, before the userptr-scaling evidence existed; WP02's PR says what changed the call. If a checkpoint shows copy mode is worse, it is deleted before the next milestone. |
| F3 | Copy mode and fewer submissions both. Copy mode is the structural fix and starts in wave 1 (longest package). The poll-flush deferral is a wave-1 experiment behind its own key. |
| F4 | PR #126 is kept with the judges' changes: the loading gate latched per submission instead of `frames_flowing()`, compute pipelines prebuilt, `pipe.libwait` deferred, compute skipped only where the CPU does not read the result. |
| F5 | Frame cap: snap to the largest divisor of the current refresh at or below the cap, never round up. 40 runs at 30 on 90 Hz, at 40 on 80 Hz and 40 Hz, at 30 on 60 Hz. The `steamdeck` preset keeps 40. The "LCD and OLED both run 40 Hz" comment in `pkp_quality.cpp` is fixed. 45 only as an explicit player choice, never a default. |
| F6 | Precompile: opt-in compile-everything with our own sharded pipeline store and resume per batch (with the 32-bit `ftell` / whole-file-RAM fix in `ensure_cache`), a Deck launch option, and the Mesa per-app cache cap (`MESA_SHADER_CACHE_MAX_SIZE=5G`, set by Steam) documented with how to raise it. |
| F7 | Readback moves off the render thread with write-generation dedupe; the generation is read after the layout restore; only the requested level is copied. |
| F8 | Stutter-free = J-F's definition (§2). Target lock: 30 on the 90 Hz panel after the readback, compile and copy-mode fixes; 40 at 80 Hz is a stretch goal after copy mode. |
| F9 | No platform pays for the Deck: every package is neutral or better on PC and on the Legion Go S, states its PC effect, and passes PC acceptance with the Windows harness (`ds/winharness`). |
| F10 | Deck tests only at checkpoints (`ds/CHECKPOINTS.md`). Every package ships an ini toggle that restores the old path, plus ftrace zones for its new work. Addendum-2 runs fold into CP1 if still missing (§5.2). |
| F11 | Tiers: Sonnet by default. Opus for WP03 (readback), WP06 (never-block compiles), WP02 and WP09 (copy mode phases 1-2). Haiku for CONFIG rows and docs chores. Fable only after two Opus rejections. |
| F12 | Package ids `WPnn-<short-name>`, branch `claude/deck-stutter/wpNN-<name>`. Waves 1-3 plus a final cleanup wave, each ending in checkpoint CPn with its exact Deck queue and pass thresholds. |

From the brief (HANDOFF-deck-stutter.md §2): never block a frame on a shader or pipeline compile (pop-in is acceptable); the opt-in precompile
may take hours and tens of GB; existing features may be reworked or removed when the evidence says so; no PR checks on `main`.

---

## 1. Evidence summary

Device for every Deck number: Steam Deck OLED, 90 Hz panel, Proton Experimental, GPU fixed at 1300 MHz, stock `steamdeck` preset (75 % render
scale, cap 40). Sources: `ds/BASELINE.md`, `ds/BASELINE-ADDENDUM.md`, `ds/baseline/TABLES.md`, raw runs under `ds/baseline/<run>/`.

1. **Neither lock holds today.** Warm medians at the 40 cap (addendum §1): in-world fps 23.8-28.2, p50 33.3-38.3 ms, p99 74-88 ms, 15-80
   frames > 50 ms per segment; even IDLE (15 s AFK) has p99 74.4 ms. At the 30 lock (`p0_w30a/b`, addendum §2): p50 31-37 ms, p99 65-237 ms,
   4-32 frames > 66.7 ms per segment. Run-to-run spread is ~2-3 fps: every comparison uses medians of >= 2 runs per side.
2. **The floor is the kernel submit under Proton, and it scales with the imported userptr set.** warm1 `zones_30s.txt`: the `nvn-submit`
   worker is busy 92 % of wall time (1555 jobs, 27 656 ms in 30 s; ~744 present jobs + ~811 poll-forced batch flushes, so "17.8 ms" is an
   average over both kinds, J-O §1). GPU work is ~12.4 ms per frame (`gpu.exec` 1488 x 6.2 ms). `d_lazy_off` (whole pools, 3.8 GiB imported):
   0.6-3 fps, then a freeze with the submit thread at 22 380 ms kernel time inside `vkQueueSubmit` (addendum §4). So cost per submission
   grows with the userptr bytes, and the frame floor is (submissions per frame) x (cost per submission).
3. **The lazy pool's drop/re-import is not the spin cause.** `d_idle_big` (nothing dropped, 1952 MiB live): every segment inside the warm
   spread (addendum §3). Default idle 1800 presents drops only 6-15 imports per 200 s run; SPIN_B, the warm-pool repeat, stutters as much as
   SPIN_A with no pool zone. First-touch imports are a LOAD cost (~1.7 GiB imported in the area load, up to 126 ms of imports in one frame,
   warm3 `pkp.log` 800). The 21-87 ms single imports are outliers above the >= 20 ms log filter (`nvn_gpu_pool.cpp` `import_range`, :256); the
   bulk rate is ~0.16-0.18 ms/MiB (J-O).
4. **A synchronous readback every 1.14 s causes 38 % of the spikes and all of IDLE's.** `hle:nvnTextureReadTexels` from game worker threads,
   ~65 ms each, 98 % overlap a > 50 ms frame, 82 of 218 in-world spikes (warm1 `readtexels_corr.txt`). `download_texture`
   (`nvn_gpu_texture.cpp:2768`) calls `finish()` (:2783) and `run_frame_sync` (:2785), whose job waits for the GPU on the render thread
   (`queue_submit` `nvn_gpu.cpp:837`, `gpu_done` branch). `nvnTextureInvalidateTexels` (`api_texture.cpp:444`) downloads again with no
   "unchanged" test. Shared with Windows.
5. **Cold compiles block frames through a cascade.** cold1 LOAD frames of 2654 / 1952 / 616 ms (`pipe.lib` 1179 + `pipe.link` 1230 +
   `rp.dispatch` 1225). The deferral in `gpl_link` (`nvn_gpl.cpp:758`) runs only while `frames_flowing()` (:735: a present seen within 100 ms,
   refreshed once per submission by `gpl_note_presents`, `nvn_replay.cpp:4307`), so one stalled frame makes every later draw compile
   synchronously. Compute is always built at the dispatch (`compute_pipeline`, `nvn_pipeline.cpp:778`); Pokopia sets programs one stage at a
   time, so `gpl_program` never pre-builds (`nvn_gpl.cpp:695-698`, "single-stage programs 8203"). PR #126 is unbuilt (`ds/pr126.md:24`).
6. **Streaming spikes (the other ~62 %).** Per spike frame vs normal (warm1 `nonreadback_spikes.txt`): `rp.prep` 6.2 vs 1.2 ms,
   `hle:nvnMemoryPoolFlushMappedRange` 5.1 vs 0, `hle:nvnTexturePoolRegisterTexture` 4.6 vs 0, `tex.prep.job` 2.4, `tex.create` 1.1,
   `submit.worker` 42 vs 35.
7. **Long single submits (100-791 ms)** co-occur with background work: warm3 WALK_BACK 185 ms (`pipe.upgrade` 89), SPIN_C 202 ms (`pipe.upgrade`
   81), cold1 SPIN_A 279 ms (`pipe.cache.save` 96); warm3 SPIN_A 791 ms with nothing else large. Correlation only.
8. **Cap vs panel.** 40 is not a divisor of 90 Hz (BASELINE finding 6); `apply_frame_cap` (`play.cpp:308`) has no divisor logic.
9. **Prior work.** Go S issue #55 (`ds/issues.md:250-251`): fence polls force 1.6-1.9 extra submissions per frame; holding them to the present
   cost 29 % there, answering the window acquire sync console-style cost 41 %. The shelved copy mode (`HANDOFF-ARCHIVE.md`, session 3 and
   "Current (2026-11-02)"): headless PC ~4x slower, ~5300 write faults/s, ~290 MiB/s landed (eager landing of every storage "may write"
   range), ~600 MiB/s re-copied from shared texture arming. totk-pc-recomp `docs/proton.md` (same device class): copy mode cut
   `amdgpu_cs_ioctl` from 60 % to 2.8 % of the submit thread, 27-31 -> 60 fps in the same scene.
10. **Windows too.** Issue #120: camera-spin stutter on a native Windows laptop; findings 4-6 are not Wine-specific code paths.
11. **PC baseline** (`ds/winharness/WIN-BASELINE.md`, this PC: discrete desktop GPU, headless, uncapped, `3dc1a05`): in-world 57-79 fps,
    p50 11.9-17.6 ms, p99 26.8-35.7 ms over w0-w2; rare single outliers (w2 WALK_BACK 157 ms, IDLE 304 ms); cold (pkp cache only) ~ warm.
    The PC is GPU-bound (`gpu.exec`/`gpu.wait`), not submit-bound: `submit_batch 1`, `submit_thread off`, `pool_lazy off`.

---

## 2. Stutter-free definition (J-F's, final)

Measured with the Deck harness on `p0.nxs`, stock `steamdeck` preset, warm cache, median of >= 3 runs. L = the lock's period: 33.3 ms at 30,
25 ms at 40 (80 Hz panel). In-world segments: SPIN_A, SPIN_B, WALK_NEW, WALK_BACK, SPIN_C, IDLE.

| # | Criterion | at 30 (L = 33.3) | at 40 @ 80 Hz (L = 25) |
|---|---|---|---|
| S1 | No frame over 2L in any in-world segment | 0 frames > 66.7 ms | 0 frames > 50 ms |
| S2 | p99 <= 1.1 L in every in-world segment | <= 36.7 ms | <= 27.5 ms |
| S3 | p50 within +-1 ms of L in every in-world segment | 32.3-34.3 ms | 24.0-26.0 ms |
| S4 | >= 95 % of frame intervals (ftrace `frame` marks) within +-2 ms of L, interval MAD <= 1.5 ms | 31.3-35.3 ms | 23.0-27.0 ms |
| S5 | LOAD (menu -> world): at most 5 frames over 2L and none over 100 ms after the first world frame | | |
| S6 | Cold run with the opt-in precompile (WP12): S1-S5 hold | | |
| S7 | Cold run without it: S1-S5 hold except that a compile becomes pop-in (no `pipe.lib`/`pipe.link`/`pipe.libwait`/`rp.dispatch` compile zone on the render thread in play) | | |
| S8 | 0 error lines, 0 rejected CS, 0 device lost; screenshots p00-p10 equal to the reference build's | | |

Today at 30 (w30a/w30b IDLE): p50 34.0/36.4, p99 75.1/64.4, 14/18 frames > 66.7 ms: S1-S4 all fail. The CP tooling (T1, §5.1) prints S1-S5
pass/fail from `segments.txt`.

Target: S1-S8 at 30 on the 90 Hz panel at CP2 (after WP02/WP03/WP06/WP09). Stretch: S1-S5 at 40 on an 80 Hz panel at CP3.

---

## 3. Process (from `ds/PLAN-draft-process.md` and `ds/CHECKPOINTS.md`)

### 3.1 Branches and PRs
- Each package gets a fresh agent in its own worktree on `claude/deck-stutter/wpNN-<name>`, branched from the integration head. PR into the
  integration branch, never into `main`.
- PR body: what changes; config keys with their CONFIG.md rows; PC effect (harness numbers); Go S effect (expected, measured when available);
  what was not verified; `## Deviations from PLAN.md` ("None" is valid); `## Pros` / `## Cons`. No model names, personal names, hostnames, IPs,
  session links or `Claude-Session` lines anywhere.
- The PM reviews every line against this file. A mismatch is rejected with exact notes unless the PR explains it and the explanation holds.
  Two rejections: the agent is retired and the package goes one tier up (Haiku -> Sonnet -> Opus -> Fable).
- After merge: branch and worktree deleted; the agent writes `ds/handoffs/HANDOFF-AGENT00NN.md` and is retired. A follow-up is a fresh agent
  starting from that handoff, on a new package id, never a second branch of the same package.

### 3.2 Merge gate (per PR, no Deck)
1. PM review approved.
2. `pwsh -File runtime\scripts\build.ps1 -Test` green on this PC (unset `NXRT_FS_SAVE_DIR` first); same failures by test name as the
   integration head.
3. PC smoke with the Windows harness (headless, uncapped, as in `ds/winharness/WIN-BASELINE.md`), two runs:
   `pwsh -File work\deck-stutter\winharness\run.ps1 wpNN_pc_on wpNN.dll p0.nxs warm 300` and the same with the package's toggle set to the
   old path (`wpNN_pc_off ... <ENV>=<old>`). Pass (the **PC rule**, used by every package's Acceptance):
   - both boot and reach IDLE, exit 0, 0 error lines, 0 device lost; screenshots equal to `wpNN_pc_off`;
   - in every in-world segment, `wpNN_pc_on` p50 <= the WIN-BASELINE max p50 of that segment over w0-w2 + 1 ms, and p99 <= that max p99 +
     2 ms (baseline: in-world p50 11.9-17.6 ms, p99 26.8-35.7 ms, 57-79 fps; the Windows floor is GPU time, `gpu.exec`/`gpu.wait`);
   - frames > 100 ms per segment <= the baseline's worst for that segment (0 except w2 WALK_BACK 1, w2 IDLE 3); a single excess outlier
     reruns once and counts the rerun.
   A run outside a bound reruns both `on` and `off`; it fails if `on` is outside the bound twice **and** worse than `off`. Packages whose new
   path is Wine-only also run once with the path forced on, for correctness only (frame times reported, not gated). Packages that change
   cold behaviour add `wpNN_pc_cold ... cold 300` against `c1` (note: harness "cold" keeps the AMD driver's own cache). Packages that change
   present pacing (WP05, WP14, WP15, WP16) add one windowed run (`WINDOWED=1`, only with the display on: a windowed run loses the device when
   the monitor is off), since headless does not exercise vsync or present.
4. The toggle restores the old path; the PM checks it in the diff.

### 3.3 Deck checkpoints
- Deck runs only at checkpoints (end of a wave) and in probes nobody waits on. One DLL per checkpoint (`cpN.dll` = integration head after
  the wave's last merge); each package of the wave is A/B'd through its toggle, no rebuilds. Queue budget ~60-75 min; the queue is written
  before the wave ends (§5) and starts as the last PR of the wave merges.
- A Deck regression never blocks the branch: the PM flips that package's default to the old path in a one-line PR and opens a fix package
  in the next wave.
- The next wave branches from the integration head and starts while the checkpoint runs.
- Report: `ds/checkpoints/CP<n>.md` (Haiku compiles the tables with `ds/analysis/cptable.py`; the PM writes the verdict per package).
- Legion Go S: when it is in use, each checkpoint adds 2 runs on it (defaults; all of the wave's toggles off). The final checkpoint always
  includes PC and Go S runs.

### 3.4 Every package must
- Ship an ini key (env var by the CONFIG.md rule) that restores the old behaviour, with a CONFIG.md row. Docs-only packages are exempt.
- Add ftrace zones (`NXFT`/`ft::Scope`) for its new work, named in the package, so a checkpoint can attribute a regression without a rebuild.
- Never block the render thread on a compile, a GPU wait or a readback unless the old path did; the PR names every new wait.
- State its Windows behaviour (same, improved, or Wine-only through the existing Wine detection) and its PC harness numbers.
- Update CHANGELOG.md (`## Unreleased` / `### What's new`, plain words, "not yet measured on a Deck" until a checkpoint measures it) when players
  notice it, and CREDITS.md when it borrows from totk-pc-recomp, ultratears or pokopia-native (project names only, no personal names).
- Keep CRLF files CRLF.

### 3.5 No platform pays for the Deck
A package that makes PC or Go S slower or less stable is rejected or gated to Wine only. Its PR states the PC effect from the harness (§3.2
step 3). An earlier shelved idea may be retried with new evidence; the PR says what changed.

### 3.6 Same file in one wave
The brief puts packages that touch the same file in different waves. The fixed wave-1 content (F3, F12) makes that impossible for
`nvn_gpu.cpp`, `nvn_gpu_pool.cpp` and `nvn_replay.cpp`, so wave packages may share a file only with **disjoint function ownership** as
listed in §5.1 (§5.3, §5.4). A package edits only the functions it owns in a shared file, plus new declarations at the end of
`nvn_gpu_internal.h`. The second PR of a pair rebases on the first before review. CONFIG.md and CHANGELOG.md are shared by all: new rows go
directly after the anchor row the package names; on a rebase conflict in those two files keep both sides.

### 3.7 Tiers
Sonnet default; Opus for WP02, WP03, WP06, WP09; Haiku for WP07, WP08, WP20 and the checkpoint tooling; Fable only after two Opus rejections.

### 3.8 Final
Final checkpoint (CP4) = the brief's full protocol on 3 Decks plus PC and Go S on the integration head, then one PR from the integration
branch to `main` for the owner. The PM never merges into `main`.

---

## 4. Work packages

Field order per package: header line (branch, wave, tier, dependencies), Why, Touches, Behaviour, Keys, Toggle, Zones, Threading,
Failure, Windows / Proton, Docs, Acceptance.

### WP01-instr: instrumentation remainder (no behaviour change)
`claude/deck-stutter/wp01-instr` · wave 1 · Sonnet · depends: none.

- **Why:** attribution for the long submits (§1.7), import sizing (§1.3), the frame cap's refresh question, and the main-thread HLE costs
  (§1.6). The submit-worker zone split is already in (`3dc1a05`: `submit.worker.batch`, `submit.worker.present`, `present.vk_submit`,
  `present.vk_present`); readback, copy-mode and refresh logging belong to WP03, WP02 and WP05.
- **Touches (owned functions only):**
  - `nvn/nvn_gpu.cpp` `submit_cbs` (:281): slow-submission log line.
  - `nvn/nvn_gpu_pool.cpp` `import_range` (:181): import histograms. No other function in this file.
  - `nvn/nvn_gpu_internal.h`: `enum PoolReason { kReasonOther, kReasonVertex, kReasonIndex, kReasonUniform, kReasonStorage, kReasonTexel,
    kReasonIndirect, kReasonCopy, kReasonImageCopy, kReasonUpdate, kReasonUntile, kReasonCount }`, `struct PoolReasonScope` (RAII, sets and
    restores a `thread_local PoolReason t_pool_reason`), `PoolReason pool_reason()`.
  - Reason scopes (one line each) in `nvn/nvn_replay.cpp`: `build_set_resolved` (by descriptor type: uniform / storage / texel), `begin_draw`
    (vertex), `do_draw_elements` (index), `do_draw_indirect` and `indirect_index_span` (indirect), `do_copy_buffer_texture` (image copy),
    `do_write_memory` (update), and in `run_list` only the `Op::CopyBufferToBuffer` / `Op::ClearBuffer` cases (copy); `nvn/nvn_gpu_texture.cpp`
    `gpu_untile_impl` around its `resolve_buffer` call (:1677) (untile).
  - `nvn/nvn_texture_registry.cpp` `textures_cpu_wrote` (:172): zone `tex.cpu_wrote`, arg = textures hit.
  - `nvn/api_memory.cpp` `texture_pool_register` (:396): zone `tex.pool_register.lock` around the exclusive lock wait (:405).
  - `runtime/src/mem/memory.cpp`: gauge counters `mem.commits`, `mem.decommits`, `mem.protects` incremented by the heap commit / decommit /
    protect paths (each such call on an imported range invalidates a userptr BO).
  - `runtime/src/gfx/vk_device.cpp`: at device creation one log line `vk: memory heaps <size/flags per heap>; types <flags per type>;
    maxBufferSize <n>; VK_KHR_present_wait <yes|no>; VK_KHR_present_id <yes|no>`; gauge `vk.vma_blocks` (VMA block count) next to the
    existing `vk.memory_allocs`.
  - New `runtime/src/diag/host_probe.cpp` (+ header): under Wine only, once, 60 s after the first present: read `Z:\proc\self\smaps` and log
    AnonHugePages / Rss of the mapping that contains `mem::kHeapBase`, and the contents of `Z:\sys\kernel\mm\transparent_hugepage\enabled`.
    Called from `diag::monitor_run`'s summary tick (`src/diag/gauge.cpp`), not from any nvn file.
- **Behaviour:**
  1. `submit_cbs`: time the `vkQueueSubmit2` call; when > `[diag] slow_submit_ms` (default 30), log at most one line per second:
     `nvn.gpu: slow submission <ms> ms (<cbs> command buffers, worker <0|1>, lazy live <MiB> MiB, memory objects <n>)`.
  2. `import_range`: count every import (not just >= 20 ms) into gauges `nvn.pool.import.size_le{1,4,16,64,inf}` and
     `nvn.pool.import.ms_lt{1,4,16,50,inf}`, and by `pool_reason()` into `nvn.pool.import.reason.<name>`; every 64th import log one line with
     all three histograms. The existing >= 20 ms line stays.
- **Keys:** new `[diag] stutter_logs` = `true`, env `NXRT_DIAG_STUTTER_LOGS`, read by `src/diag/host_probe.cpp`, `nvn_gpu.cpp`,
  `nvn_gpu_pool.cpp`; new `[diag] slow_submit_ms` = `30`, env `NXRT_DIAG_SLOW_SUBMIT_MS`.
- **Toggle:** `stutter_logs = false` = no new log lines and no smaps read (zones and gauges cost nothing while tracing is off).
- **Zones:** `tex.cpu_wrote`, `tex.pool_register.lock`.
- **Threading:** gauges are atomics; the slow-submission rate limit uses an atomic timestamp; `t_pool_reason` is thread-local (render thread).
- **Failure:** smaps / sysfs unreadable -> one line "host probe: not available" and nothing else.
- **Windows / Proton:** log lines and gauges only; the smaps probe is Wine-only. PC effect: none (harness spread). Go S: same as Deck.
- **Docs:** CONFIG.md rows for both keys after the `[diag] watchdog_seconds` row. No CHANGELOG entry.
- **Acceptance:** merge gate; PC on/off runs identical within spread. CP1: `cp1_alloff*` within the baseline warm spread (no behaviour change);
  the new lines appear in `pkp.log` (memory heaps, histograms, THP, slow submissions).

### WP02-pool-copy-p1: copy/shadow mode, phase 1 (no write tracking)
`claude/deck-stutter/wp02-pool-copy-p1` · wave 1 · Opus · depends: none (WP01's histograms help, not required).

- **Why:** §1.2: the userptr set in every CS is the floor. Phase 1 proves the cost model: no userptr BO in any submission, the simplest
  correct copy rule. Its PR body has a section "Why copy mode again" stating: the 2026-11-02 attempt was shelved before `d_lazy_off`
  (addendum §4) showed per-submit cost scaling with imported userptr bytes; totk-pc-recomp's measurement on the same device class (§1.9);
  and how phase 1 avoids the old attempt's costs (Wine-only by default, copies by touched range, no write watch, no shared texture arming).
- **Design rule (phase 1):** a pool in copy mode keeps the lazy cell structure (`PoolGpu::lazy`, `ext`, `cell_ext`, `spans`), but each
  `PoolChunk` holds a Vulkan-owned host-visible buffer instead of an import. During recording, every resolve of a pool range copies the
  page-rounded range from guest memory into the chunk unless that page was already copied for the current submission serial or a GPU write to
  it is still in flight. GPU writes are recorded at record time and copied back to guest memory when their serial completes, before any
  caller can see the serial as complete.
- **Touches:**
  - New `nvn/nvn_pool_copy.cpp` (all copy-mode logic) and declarations in `nvn_gpu_internal.h`.
  - `nvn/nvn_gpu_pool.cpp`, owned functions: `import_pool` (:282), `lazy_chunk` (:378), `lazy_resident` (:436), `resolve_in_pool` (:529),
    `pool_lazy_wants_cpu` (:628), `lazy_drop_idle` (:647), `destroy_chunk`, `note_gpu_write` (:1166), `out_lazy` (:1444), `pool_shutdown`,
    struct `PoolChunk` (:39), struct `PoolGpu` (:56). Not `import_range` (WP01).
  - `nvn/nvn_gpu.cpp`, owned function: `read_completed` (:171) only.
  - `nvn/nvn_replay.cpp`, owned: the query-report cases of `run_list` (the two `vkCmdCopyQueryPoolResults` sites, :4102 and :4123),
    `shadows_after_dispatch` (:1749), `flush_compute_out` (:1494).
  - `include/nxrt/gfx.h` `MemoryUse` (:69) and `runtime/src/gfx/vk_device.cpp` `Device::create_buffer` (:904): new `MemoryUse::Shadow`.
  - `runtime/tests/gfx/test_nvn_pool_copy.cpp` (new) and its CMake entry.
- **Data structures:**
  - `PoolChunk` gains `gfx::Buffer shadow` (when set, `buffer = shadow.buffer`, `mem = VK_NULL_HANDLE`), `bool is_shadow`,
    `std::vector<uint64_t> page_copied` (per 4 KiB page: serial of the last guest -> shadow copy), `std::atomic<uint32_t> land_refs`.
  - `PoolGpu` gains `std::unique_ptr<std::atomic<uint64_t>[]> page_gpu` (per 4 KiB page of the pool: highest serial with a recorded GPU write;
    shared by all chunks and spans of the pool) and `bool copy`.
  - `struct LandRec { PoolGpu* pool; PoolChunk* chunk; uint64_t pool_off, size, serial; }` in a `std::deque<LandRec> g_land` under
    `std::mutex g_land_mu` (serial order).
  - Thread-local `t_last_resolve = {PoolGpu*, PoolChunk*, off, size}` written by `resolve_in_pool` (render thread).
- **Behaviour:**
  1. `bool pool_copy_on()`: `[nvn] pool_copy` = `auto` -> `wine_host()`; `on`/`off` force it; logged once
     (`nvn.pool: pool_copy on (auto, Wine)`). When on, `import_pool` cuts every pool larger than one cell (`[nvn] pool_copy_cell_mb`, default
     16) into cells as the lazy mode does, whatever `pool_lazy` says, and gives smaller pools one whole-pool chunk; both get shadows, never an
     import. `pool_lazy` is then ignored (logged).
  2. Shadow allocation (`pcopy_alloc`, called where `lazy_chunk` / `import_pool` / `pool_span` would call `import_range`):
     `device().create_buffer(size, <the usage flags import_range gives the import buffer>, MemoryUse::Shadow, ...)`. `MemoryUse::Shadow` =
     VMA `HOST_ACCESS_RANDOM | MAPPED`, required `HOST_VISIBLE | HOST_COHERENT`, preferred `HOST_CACHED`, `VMA_MEMORY_USAGE_AUTO_PREFER_HOST`.
     The chosen memory flags are logged once. No content is copied at allocation.
  3. Touch (`pcopy_touch(pg, chunk, off, size)`, called at the end of `resolve_in_pool` for shadow chunks): range = `[off, off + len)` with
     `len = size ? size : chunk end - off` (a bind of unknown size touches to the end of its chunk), rounded out to 4 KiB pages. For each page
     run where `page_copied[p] != current_serial()` and `page_gpu[p] <= completed_serial()`: `memcpy` guest -> shadow, set `page_copied[p]`.
     Runs are coalesced. The guest read is wrapped in SEH (`__try/__except`); an unreadable run is zero-filled and counted. Outside a recording
     (`current_serial()` = 0 path) the range is always copied.
  4. Invariant: in copy mode every GPU read of pool memory in a submission is preceded, in that submission, by a resolve (or an explicit
     `pcopy_touch`) of its range. The PR contains an audit table of every cache that can skip a resolve inside one submission (descriptor-set
     reuse `g_set_cache`/`SetMemo`, `tex_buffer_view`'s view cache, vertex-buffer dirty bits) and shows each resets per submission or adds a
     touch.
  5. Record GPU writes (`pcopy_note_write(addr, size)`, called from `note_gpu_write` when the pool is in copy mode): take the chunk from
     `t_last_resolve` when it covers the range, else the chunk `cell_ext` gives (counter `pool.copy.write_unmatched`); push
     `LandRec{..., serial = current_serial()}`, set `page_gpu[p]` to that serial for its pages, `chunk->land_refs++`. Every write path is
     recorded; the PR's audit table lists each with the call that records it: all `gpu_wrote` callers (storage binds, xfb, image copies,
     `do_write_memory`, fill, buffer copy); the two `vkCmdCopyQueryPoolResults` report sites (add `gpu_wrote(p.address, 16, kWriteCopy)`
     there); compute-shadow copy-backs (`shadows_after_dispatch`, `flush_compute_out`, `out_end_submit`, `out_flush`) into a shadow chunk.
  6. Compute results: in copy mode `out_lazy()` returns false (results go back into the chunk at the end of the submission, are recorded,
     and land at completion). The guard/fault path assumes the pool buffer aliases guest memory, which a shadow does not.
  7. Landing (`uint64_t pcopy_land(uint64_t reached)`): under `g_land_mu`, for every record with `serial <= reached`, in order:
     `mem::ww::before_os_write(dst, n)`, `memcpy` shadow -> guest, `land_refs--`, clear `page_gpu[p]` when it still equals the record's serial.
     `read_completed` calls it between `vkGetSemaphoreCounterValue` and `complete_serial(v)`, so `g->completed` never passes a serial whose
     writes have not landed. A thread `nvn-pool-land` (started with the first copy pool, stopped in `pool_shutdown` before device teardown)
     waits with `vkWaitSemaphores` (5 ms timeout loop) for the oldest record's serial and then calls `poll_completed()`, so data the game polls
     in memory without an API call (event words written by `do_write_memory`) lands within a few ms.
  8. GPU untile and textures: `pool_lazy_wants_cpu` returns false in copy mode (the untile's resolve copies the tiled storage into the
     shadow), so "GPU untile not used (storage not imported (pool_lazy))" goes away.
  9. Drop and cap: `lazy_drop_idle` uses `[nvn] pool_copy_idle` (default 600 presents) for copy pools and also requires `land_refs == 0`;
     one log line per drop (both modes). When live shadow bytes would exceed `[nvn] pool_copy_mb` (default 1536), `pcopy_alloc` first drops
     least-recently-used chunks that pass the idle test except the present count; when none qualifies it allocates anyway and logs once.
  10. Fallback: when `create_buffer` fails, that chunk is imported as today (`import_range`), `is_shadow = false`; counter
      `pool.copy.import_fallback`, one warning line. Imported chunks need no landing.
- **Keys:** new `[nvn] pool_copy` = `auto` (`NXRT_NVN_POOL_COPY`); `[nvn] pool_copy_cell_mb` = `16` (`NXRT_NVN_POOL_COPY_CELL_MB`, 1-1024);
  `[nvn] pool_copy_idle` = `600` (`NXRT_NVN_POOL_COPY_IDLE`); `[nvn] pool_copy_mb` = `1536` (`NXRT_NVN_POOL_COPY_MB`). Changed meaning:
  `pool_lazy`, `pool_lazy_chunk_mb`, `pool_lazy_idle`, `pool_lazy_cpu_untile` apply only while `pool_copy` is off; `compute_out_lazy` is
  ignored while `pool_copy` is on. All read once.
- **Toggle:** `pool_copy = off` = today's lazy import path byte for byte.
- **Zones / gauges:** `pool.copy.touch` (arg = KiB copied), `pool.copy.alloc`, `pool.copy.land` (arg = KiB landed), `pool.copy.evict`;
  gauges `nvn.pool.copy.live_mb`, `nvn.pool.copy.copied_kb` (per submission, sampled), `nvn.pool.copy.landed_kb`, counters
  `write_unmatched`, `import_fallback`, `unreadable`, `span_writes`; a summary line every 1200 presents (live MiB, chunks, copied and landed
  MiB/s, fallbacks).
- **Threading and sync:** touch, record, alloc, drop: render thread. Landing: any thread calling `read_completed`, plus `nvn-pool-land`;
  serialised by `g_land_mu`. Ordering: landing finishes before `complete_serial` publishes; the render thread's `page_gpu[p] <=
  completed_serial()` check therefore never copies a page whose GPU data has not landed (relaxed atomics for `page_gpu`, the acquire/release
  pair on `g->completed`). A chunk is destroyed only when `used_serial <= done` and `land_refs == 0`, through the existing deferred destroy.
  Lock order: `g_land_mu` is a leaf (nothing else is taken under it).
- **Failure:** device lost: records are dropped, nothing lands. Shadow allocation failure: import fallback (step 10). A checkpoint showing copy
  mode worse (CP2 rule, §5.3): `auto` -> off in a one-line PR, and WP19 deletes copy mode instead of the lazy path before the next milestone.
- **Windows / Proton:** `auto` = off on native Windows: no change (PC effect: none). `on` works on Windows (heap section and views are
  ignored by copy mode) for A/B and tests. Go S: same path as the Deck.
- **Known phase-1 limits (accepted):** a CPU write to bytes of a range the GPU may write, made while that submission is in flight, is
  overwritten at landing (phase 3); the whole recorded range lands, not only the bytes a shader stored (cost measured by `landed_kb`); a GPU
  write through a span and a read of the same bytes through another chunk within one submission sees stale data (`span_writes` counts it).
- **Docs:** CONFIG.md rows after the `pool_lazy_cpu_untile` row; the `pool_lazy*` rows get "only while `pool_copy` is off". CHANGELOG:
  "Steam Deck / Linux: GPU memory is copied instead of shared with the driver (`[nvn] pool_copy`), removing the cost every frame paid for
  shared memory; `pool_copy = off` goes back." CREDITS.md: add totk-pc-recomp (shadow storage design) and ultratears (pool copies) to the list.
- **Tests:** `test_nvn_pool_copy.cpp`: CPU write then draw reads it; fill lands in guest memory once the serial completes and not before;
  a pending GPU write is not overwritten by a later touch; drop with pending records is refused; cap eviction; span write counter. Every
  `gfx_nvn_*` test group gives the same results with `NXRT_NVN_POOL_COPY=on` as with `off` (run both).
- **Acceptance:** merge gate (PC default = off: neutral; one forced-on PC run: boots, reaches IDLE, 0 errors, screenshots equal; its fps is
  reported, not gated). CP1 (`cp1_w*` vs `cp1_copyoff*`): `submit.worker.batch` median <= 5 ms per call **and** <= 50 % of the copy-off
  median; 0 error lines / device lost / rejected CS; screenshots p00-p10 equal to copy-off; `mem_avail_mb` (samples.csv) >= 2048 through every
  in-world segment; copied and landed MiB per frame reported. Frame times are reported, not gated, at CP1. If the batch cost does not fall by
  >= 50 %, the cost model is wrong: WP09 does not start, `auto` goes off, and the PM reports to the owner.

### WP03-readback: readback off the render thread, deduped, one level
`claude/deck-stutter/wp03-readback` · wave 1 · Opus · depends: none.

- **Why:** §1.4; all platforms.
- **Touches:**
  - `nvn/nvn_gpu_texture.cpp` `download_texture` (:2768), struct `TexGpu` (new fields), texture destroy path (free the readback buffer).
  - `nvn/api_texture.cpp` `read_texels` (:192), `nvnTextureInvalidateTexels` (:444).
  - `nvn/nvn_gpu.h` (:61) and `nvn/nvn_gpu_stub.cpp` (:37): new signature.
  - `nvn/nvn_gpu.cpp`: two **new** functions placed directly after `run_frame_sync` (:869), nothing else in the file:
    `uint64_t queue_record(std::function<void(Frame&)> record)` (= `queue_submit(record, true, false)`: returns once the render thread has
    recorded it; no GPU wait) and `bool wait_serial_grace(uint64_t serial, uint64_t grace_ns)` (waits up to `grace_ns` for
    `flushed_pub >= serial`, polling 0.5 ms, then `wait_serial(serial, UINT64_MAX)`); declared in `nvn_gpu_internal.h`.
- **Behaviour:** `bool download_texture(Texture* t, uint32_t level_first, uint32_t level_count, uint32_t layer_first, uint32_t layer_count)`;
  `read_texels` passes `r.level, 1, r.layer, 1` (all layers for a 3D target); `nvnTextureInvalidateTexels` resolves `view` the way
  `resolve()` does (`select_level_layer`) and passes that level, or all levels when `view` is null. With `readback_async` on:
  1. Early outs as today (:2769-2780). Lock `TexGpu::rb_mu` (held by the calling thread until step 6).
  2. No `finish()`. Queue order on the one queue plus the barrier `move_layout` records orders the copy after every earlier submission.
  3. `queue_record` a job that, on the render thread: if `rb_valid[level]` and `tex_write_gen(t, level) == rb_gen[level]` for every
     requested level, sets `skipped` and records nothing; else grows `TexGpu::rb` (a `MemoryUse::Readback` buffer kept per texture, grown to
     the largest requested size, never shrunk) if needed, `move_layout` to `TRANSFER_SRC_OPTIMAL`, records `vkCmdCopyImageToBuffer` for the
     requested levels and layers only, `move_layout` back to the previous layout, **then** reads `rb_gen[level] = tex_write_gen(t, level)`
     (after the restore: the restore to a writable layout bumps the generation, `move_layout` :735-737) and sets `rb_valid[level]`, and stores
     the job's serial.
  4. `skipped`: return true (the storage already holds that data).
  5. Else `wait_serial_grace(serial, grace)` on the calling thread, `grace` = `[nvn] readback_grace_ms` (auto = one frame interval at the
     limiter's target, 33 ms when unknown; 0 = flush at once).
  6. `invalidate_buffer(rb)`, then `tegra::tile_level` per requested level and layer from `rb.mapped` into `t->mem`
     (`tools/tiling/include/tegra_tiling.h`); unlock.
  7. `[nvn] readback_check = true`: log `readback: texture <id> level <l> crc32 <x>` of the read region (A/B with `readback_async = false`).
  8. First 20 distinct textures: one log line (id, format, size, levels, layers, calling thread, level/layer asked, skipped or copied).
- **Keys:** `[nvn] readback_async` = `true` (`NXRT_NVN_READBACK_ASYNC`); `[nvn] readback_grace_ms` = `-1` (auto) (`NXRT_NVN_READBACK_GRACE_MS`);
  `[nvn] readback_check` = `false` (`NXRT_NVN_READBACK_CHECK`). Read once.
- **Toggle:** `readback_async = false` = today's path (`finish()` + `run_frame_sync`, all levels).
- **Zones:** `rb.queue` (game thread, until recorded), `rb.record` (render thread job), `rb.wait` (game thread GPU wait), `rb.tile`;
  existing `hle:nvnTextureReadTexels` stays.
- **Threading:** the dedupe compare runs on the render thread in queue order (generations are bumped during replay). `rb_mu` serialises
  readers of one texture (several guest threads read). The render thread never waits for a readback's GPU work. The buffer is freed with the
  texture through deferred destroy.
- **Failure:** buffer allocation fails -> return false (storage read as it is, as today); device lost -> `wait_serial` returns, the tile is
  skipped. The grace bounds a game that waits for this thread before presenting to one frame.
- **Windows / Proton:** same code on both; removes two full GPU drains per ~1.1 s on PC too (#120 candidate). PC effect: expected neutral or
  better; the harness measures it.
- **Docs:** CONFIG.md rows after the `tex_write_watch` row. CHANGELOG: "Fewer hitches about once a second (a texture read back from the GPU
  no longer stops rendering)".
- **Tests:** existing `test_nvn_scene_texture.cpp` / `test_nvn_layered_target.cpp` pass; new case: ReadTexels twice with no GPU write in
  between records one copy; a GPU write in between records two; level-only copy matches a full copy's level.
- **Acceptance:** merge gate (PC: no segment worse; `readback_check` CRCs equal between on and off on PC). CP1 (`cp1_w*` vs `cp1_rboff`):
  IDLE frames > 50 ms <= 3 per 15 s (median); readback-overlapping spikes (`analysis/corr.py`) <= 10 % of baseline (82 -> <= 8 per warm1-length
  window); render-thread `submit.wait_submitted` < 1 ms on frames with a readback; 0 new error lines; Pokedex picture path (#121)
  screenshot unchanged.

### WP04-poll-flush: deferred flush of polled serials (experiment)
`claude/deck-stutter/wp04-poll-flush` · wave 1 · Sonnet · depends: none.

- **Why:** §1.2 / §1.9: ~811 of 1555 worker jobs per 30 s are poll-forced batch flushes; cost = submissions x bytes, so fewer submissions
  help until copy mode lands. #55 shows the game paces on poll answers (Go S -29 % when held to the present), so it ships only if measured
  better.
- **Touches:** `nvn/nvn_gpu.cpp` owned functions `want_flush` (:367), `serial_done` (:919), `render_loop` (:200), `wait_serial` (:960) (only
  the call it makes to `want_flush`), struct `Gpu` (new fields `flush_deadline_ns`, counters); `nvn/api_device.cpp` only to pass a poll kind at
  the three `serial_done` callers (window acquire sync, queue fence, nvnSyncWait with timeout 0).
- **Behaviour:** `[nvn] poll_flush`:
  - `now` (today): a poll of a serial still in the batch flushes at once.
  - `defer`: a poll (`serial_done`) of a batched serial does not flush; the batch goes with the present.
  - `<ms>` (integer 1-50): the first deferred poll of the current batch sets `flush_deadline_ns = now + ms`; `render_loop` flushes when the
    deadline passes (its `qcv.wait` becomes `wait_until` the deadline) or at the present, whichever comes first.
  - In every mode a blocking wait (`wait_serial` with timeout > 0) flushes at once, as today.
  - `auto` = `now` everywhere until a checkpoint passes; then a one-line PR sets `auto` = the winning value under Wine.
  - Only active with `submit_batch = frame` (Wine); with `submit_batch = 1` (`want_flush` returns early) nothing changes.
- **Keys:** `[nvn] poll_flush` = `auto` (`NXRT_NVN_POLL_FLUSH`), read once.
- **Toggle:** `poll_flush = now`.
- **Zones / counters:** `submit.poll_deferred` mark; counters `nvn.flush.by_poll`, `by_deadline`, `by_present`, `by_wait`, `polls_deferred`,
  per poll kind; one summary line every 1200 presents.
- **Threading:** `flush_deadline_ns` atomic; set by any polling thread, read and cleared by the render thread under `qmu`.
- **Failure:** a game that spins on an unanswered poll (Go S symptom): the deadline bounds it; CP numbers decide.
- **Windows / Proton:** Wine only in effect (`frame` batching is Wine-only). PC effect: none.
- **Docs:** CONFIG.md row after the `submit_batch` row (WP07 adds that row; rebase). No CHANGELOG until it ships as a default.
- **Acceptance:** merge gate. CP1 (`cp1_pf8a/b`, `cp1_pfdefer` vs `cp1_w*`): the value ships as Wine default only if p50 **and** frames > 50 ms
  are both better in every in-world segment (medians), with worker jobs per present down. Else it stays `now`, is re-measured at CP3 after copy
  mode, and is removed in WP19 if CP3 also rejects it.

### WP05-frame-snap: cap snapped to a refresh divisor
`claude/deck-stutter/wp05-frame-snap` · wave 1 · Sonnet · depends: none.

- **Why:** §1.8; F5.
- **Touches:** `play/play.cpp` `apply_frame_cap` (:308); `play/play_ui.cpp` `kFpsChoices` (:43) and the Graphics page frame-limit row;
  `runtime/games/pkp/pkp_quality.cpp` preset comment (:83).
- **Behaviour:**
  1. In the `vsync` branch, when `hz > 0` and `0 < cap < hz`: `k = ceil(hz / (cap * 1.005))` (0.5 % tolerance for reported rates such as
     79.98), `cap = hz / k`. 90 Hz: 40 -> 30, 45 -> 45, 30 -> 30; 80 Hz: 40 -> 40; 60 Hz: 40 -> 30, 45 -> 30; 40 Hz: 40 -> 40. Never above
     the requested cap (beyond the tolerance). `cap >= hz` keeps today's `vsync_cap` rule. `hz` unknown (0) or vsync off: no snap.
  2. The limiter gets `hz / k` as its target, so its period is exactly k refresh intervals of the measured rate.
  3. Log at start and on every change: `play: frame cap <req> -> <snapped> (<hz> Hz panel, every <k> refreshes)` or
     `play: frame cap <req> (<hz> Hz, no snap: <reason>)`.
  4. Graphics page: when snapped, the frame-limit row shows "40 (runs at 30 on this 90 Hz screen; set 80 Hz in Steam's Performance menu for
     40)".
  5. `kFpsChoices` = `{-1, 0, 30, 40, 45, 60, 72, 90, 120, 144, 165, 240, 360}` (45 as a player choice only; no preset uses it).
  6. Comment at `pkp_quality.cpp:83`: "40 fps: runs at 40 where the refresh is a multiple (80 Hz OLED / 40 Hz LCD in Steam's Performance
     menu), else at 30 (90 Hz OLED default)". The preset value stays 40.
- **Keys:** new `[display] fps_snap` = `auto|true|false` (`NXRT_DISPLAY_FPS_SNAP`), read on every `apply_frame_cap`. `auto` (default) =
  on under Wine (Steam Deck, Go S, Linux: gamescope/fixed-refresh handheld panels), off on native Windows, where a desktop screen may be VRR
  (FreeSync/G-Sync) and a 40 cap must stay 40 (PM decision under F9: no PC player loses a cap they chose). `true` forces it on a PC.
- **Toggle:** `fps_snap = false` = today's cap.
- **Zones:** none new (no per-frame work); the log line is the evidence.
- **Threading:** `apply_frame_cap` runs where it does today.
- **Failure:** wrong reported refresh under gamescope -> visible in the log line; `fps_snap = false` restores.
- **Windows / Proton:** `auto` snaps under Wine only; native Windows keeps today's cap unless `fps_snap = true` (then a vsync'd 40 on a
  60 Hz screen becomes 30). PC acceptance: harness default identical; one forced `fps_snap = true` windowed run shows the right log line.
- **Docs:** CONFIG.md row after `[latency] vsync_cap`; the controls.ini `[display]` notes (CONFIG.md §"Window modes") mention the snap.
  CHANGELOG: "Frame limits now divide the screen's refresh rate for even pacing: on the 90 Hz Steam Deck OLED, 40 fps runs at 30; pick 80 Hz in
  Steam's Performance menu for 40. 45 fps can be chosen in F1."
- **Acceptance:** merge gate; unit test of the snap function over the table in step 1. CP1: every run's `pkp.log` shows the correct line
  (`40 -> 30 (90 Hz ...)`); `cp1_snapoff` vs `cp1_w*` reported. Interval share within +-2 ms is reported at CP1 and judged at CP2 (S4).

### WP06-never-block-compiles: loading gate, PR #126, async compute
`claude/deck-stutter/wp06-never-block-compiles` · wave 1 · Opus · depends: none.

- **Why:** §1.5; brief §2 "never block a frame on a compile"; F4.
- **Touches:** `nvn/nvn_gpl.cpp` (`get_part` :371, `gpl_link` :758, `frames_flowing` :735, new gate functions); `nvn/nvn_pipeline.cpp`
  (`graphics_pipeline` :729, `compute_pipeline` :778, PR #126's `aw_*`); `nvn/nvn_prewarm.cpp` (`prewarm_take` :373, key-file record format,
  boot prewarm of compute records); `nvn/nvn_replay.cpp` owned functions `note_frame_for_pacing` (:4265), `replay_begin_submit` (:4299),
  `do_dispatch` (:3338); `nvn/api_program.cpp` (compute program set hook); `runtime/src/host_device.cpp` + header (new `host_is_wine()`);
  `nvn/nvn_gpu_pool.cpp` one **new** exported function at the end of the file, `uint32_t pool_flags_at(uint64_t gpu_address)` (0 when not in
  a pool), nothing else in that file.
- **Behaviour:**
  1. **Gate.** Split `note_frame_for_pacing` into detection (always runs) and the limiter action (only with `[display] unpaced_loading`).
     Export `bool replay_loading()` = the detector's current `want`. In `replay_begin_submit`, replace the `gpl_note_presents` call by
     `gpl_latch_play(!replay_loading())` once per submission (keep `gpl_note_presents` too for `gpl_gate = time`). `gpl_link`'s
     `frames_flowing()` test becomes `gpl_gate_open()` = `gpl_gate == state ? latched in-play value : frames_flowing()`. A stall inside a
     submission can no longer flip later draws of it to synchronous compiles.
  2. **No `pipe.libwait` in play.** In `gpl_link`, when the gate is open: if any of the four parts (`vi_key`, `pre_key`, `fs_key`, `fo_key`) is
     not done (absent or being built by another thread), queue the missing ones first in line (`queue_exact_locked`) and defer the draw
     (`*pending = true`). `get_part` with `render = true` therefore never waits in play; while loading it waits as today.
  3. **PR #126 with changes.** Apply its diff (`ds/pr126.md`), then: `async_whole` default `true` on every platform (`auto` accepted and
     means `true`); its `gpl_frames_flowing()` replaced by `gpl_gate_open()`; its Wine check replaced by `host_is_wine()`; stream-out draws stay
     synchronous; off with `gpl_ab`. Review fixes: `gpl_take_upgrade` (:919) handles a null current pipeline; `AsyncJob::layout` points to a
     `ProgLayout` that lives as long as the back end (assert or comment); `aw_stop` runs before device teardown (after `prewarm_shutdown`,
     before `gpl_stop`). PR #126 is closed when WP06 merges.
  4. **Compute.** (a) New key-file record type for compute (shader hash + layout key), written when a compute pipeline is created;
     `[nvn] compute_prewarm` builds them on the prewarm workers at boot. (b) At `nvnProgramSetShaders` of a compute program, queue its build
     on a background thread when its layout can be derived there; else at the first dispatch. (c) In `do_dispatch`, when the pipeline is
     missing and the gate is open: skip the dispatch and queue the build **only if** (i) every storage-buffer binding of the dispatch lies in
     a pool whose flags include `NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS` (`pool_flags_at`), (ii) none of those ranges intersects a 64 KiB granule
     that a buffer copy (`Op::CopyBufferToBuffer`) has ever read into a CPU-visible pool (a set filled at that op), and (iii) no storage image is
     a texture that `download_texture` has read back (`TexGpu::read_back`). Otherwise build synchronously and log
     `compute built at the dispatch <hash> (<ms> ms)`. `g_compute` gets a mutex (background inserts). Each skip logs the shader hash (first 50).
  5. **Single-stage programs:** investigation only: whether a vertex-only / fragment-only `ProgLayout` is layout-compatible with the pair
     (`VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT`); the PR reports the answer; no code change in this package.
- **Keys:** new `[nvn] gpl_gate` = `state` (`NXRT_NVN_GPL_GATE`, `state | time`); `[nvn] async_whole` = `true` (`NXRT_NVN_ASYNC_WHOLE`, from #126,
  default changed); `[nvn] async_whole_threads` = `1` (from #126); new `[nvn] async_compute` = `true` (`NXRT_NVN_ASYNC_COMPUTE`); new
  `[nvn] compute_prewarm` = `true` (`NXRT_NVN_COMPUTE_PREWARM`); changed meaning: `gpl_async` (now uses the gate). All read once.
- **Toggle:** `gpl_gate = time`, `async_whole = false`, `async_compute = false`, `compute_prewarm = false` together = today's behaviour.
- **Zones / counters:** existing `pipe.lib`, `pipe.link`, `pipe.libwait`, `rp.dispatch`; new marks `gpl.defer.part`, `cs.skip`; `nvn.pipe`
  summary adds `deferred (parts in progress) N`, `async_whole: draws skipped N, built N`, `compute: skipped N, built at the dispatch N,
  prewarmed N`.
- **Threading:** the latched gate is written and read on the render thread only. Background builds insert under the existing `g_mu` (parts)
  and the new compute mutex; the render thread reads finished entries under the same locks.
- **Failure:** a skipped draw or dispatch shows as pop-in for a frame or two (accepted); a build that fails leaves the entry empty as today.
  A one-time loading render misclassified as play is the `gpl_gate = time` fallback.
- **Windows / Proton:** same on both (first-run pop-in instead of hitches; #120). PC acceptance: harness **cold** run on and off; cold LOAD max
  and frames > 100 ms not worse; warm identical.
- **Docs:** CONFIG.md rows after the `gpl_cold` row (`gpl_async` row comes from WP07, then updated here). CHANGELOG: "Fewer hitches the first
  time something is drawn: shaders compile in the background and the object appears a frame later."
- **Acceptance:** merge gate. CP1 (`cp1_cold1` vs `cp1_coldgploff`; `cp1_w*` vs `cp1_gploff`): cold LOAD max < 150 ms outside frames the
  detector marks as loading; cold SPIN_A frames > 100 ms = 0; no render-thread `pipe.lib`/`pipe.link`/`pipe.libwait`/`rp.dispatch` zone
  > 5 ms in play frames; warm runs within the warm spread; screenshots of the next warm run: nothing permanently missing.

### WP07-config-rows: missing CONFIG.md rows (docs only)
`claude/deck-stutter/wp07-config-rows` · wave 1 · Haiku · depends: none. Merge first in wave 1.

- **Touches:** `runtime/docs/CONFIG.md` only.
- **Rows:** `[nvn] submit_batch` (`auto`, `NXRT_NVN_SUBMIT_BATCH`, `src/gfx/nvn/nvn_gpu.cpp`: `auto` = `frame` under Wine, `1` elsewhere;
  `frame` = one kernel submission per frame, the present's, flushed early when another thread waits or polls; `N` = every N submissions and
  when the render thread idles; from the comment at `nvn_gpu.cpp:1155`), after the `submit_thread` row. `[nvn] gpl_async` (`true`,
  `NXRT_NVN_GPL_ASYNC`, `nvn_gpl.cpp`; current behaviour as in `ds/pr126.md`'s row), after `gpl_cold`. `[nvn] compute_shadows` and
  `[nvn] compute_direct` (both `true`, `nvn_replay.cpp`; meaning from the comment above `compute_shadows_on`, :1466-1484), after
  `compute_out_lazy`.
- **Acceptance:** each row matches the code it cites (PM checks against the code); no behaviour change; no toggle needed.

### WP08-lazy-tune: interim lazy-pool defaults (conditional)
`claude/deck-stutter/wp08-lazy-tune` · wave 2 · Haiku · depends: CP1.

- Starts only if CP1 shows WP02 failed its CP1 gate **and** a cell size (`a2_cell64`, or `pool_lazy_chunk_mb` 2/4/8 runs added as a probe)
  gives >= 25 % lower `submit.worker.batch` ms per call with no more frames > 50 ms. Then: change the default in `lazy_cell()`
  (`nvn_gpu_pool.cpp:139`) and the CONFIG row (:131). Toggle: the key itself (old value). Never "never drop" (d_idle_big). Else cancelled,
  noted in CP1.md.

### WP09-pool-copy-p2: dirty tracking for copy mode
`claude/deck-stutter/wp09-pool-copy-p2` · wave 2 · Opus · depends: WP02 passed CP1.

- **Why:** phase 1 copies every touched range every submission; phase 2 copies only what the CPU wrote.
- **Step 0 (probe, CHECKPOINTS rule 6):** a micro-benchmark mode (`[nvn] pool_copy_bench = 1`, test only) that times, on the Deck under
  Proton, 10 000 write faults through `mem::ww` (VirtualProtect + vectored handler) and 100 `GetWriteWatch` queries over a 64 MiB region
  reserved with `MEM_WRITE_WATCH`. 1 run as a probe. The result picks the tracker: `protect` if a fault costs < 20 us, else `os` (needs the
  Wine private-heap reservation in `mem/memory.cpp` (:450-467 path) to add `MEM_WRITE_WATCH`).
- **Touches:** `nvn/nvn_pool_copy.cpp`; `src/mem/write_watch.cpp` (regions over shadowed cells; pool pages only); `src/mem/memory.cpp`
  (only if `os`).
- **Behaviour:** a `mem::ww` region (or write-watch query) per shadowed chunk; at touch, only pages written since `page_copied` plus never
  copied pages are copied; pages rewritten 3 submissions in a row become hot (left unprotected, copied at every touch, cooled after 240
  presents: pokopia-native `HOT_STREAK` / `HOT_COOL_FRAMES`, our `kHotHits`/`kHotHold` pattern in `absorb_writes`). The texture write watch
  never arms pool pages that copy mode tracks (the shelved attempt's 600 MiB/s lesson). Re-test `tex_write_watch = true` under copy mode
  with validation on; if clean, the Deck ini workaround (`pool_mirror = off`, `tex_write_watch = false`) is documented as unnecessary.
- **Keys:** `[nvn] pool_copy_track` = `auto` (`on`) (`NXRT_NVN_POOL_COPY_TRACK`; `off` = phase 1); `[nvn] pool_copy_watch` =
  `protect | os` (default from step 0).
- **Toggle:** `pool_copy_track = off`.
- **Zones / gauges:** `pool.copy.track` (arg = pages taken), gauges `nvn.pool.copy.faults_s`, `hot_pages`.
- **Threading:** fault handler on the faulting guest thread marks pages (atomic bitmap); the render thread takes them at touch.
- **Failure:** a missed CPU write = stale data: CP2 screenshots vs `pool_copy_track = off`.
- **Windows / Proton:** Wine only by default. PC effect: none.
- **Docs:** CONFIG.md rows; CHANGELOG line if CP2 shows the gain; CREDITS.md: pokopia-native (already listed) for the hot-page design.
- **Acceptance:** CP2 (`cp2_trackoff`, `cp2_copyoff` vs `cp2_w*`): copied MiB per submission <= 20 % of `trackoff`; in-world median fps >=
  `copyoff` and frames > 50 ms <= `copyoff` in >= 5 of 6 in-world segments; `submit.worker.batch` <= 5 ms; write faults/s reported;
  `mem_avail_mb` >= 2048; screenshots equal. **Copy mode deletion rule (F2):** if copy mode (`cp2_w*`) is worse than `cp2_copyoff` in median
  fps **and** frames > 50 ms in >= 4 of 6 in-world segments, `auto` goes off and WP19 deletes copy mode.

### WP10-compile-hygiene: background compiles yield on handhelds (gated)
`claude/deck-stutter/wp10-compile-hygiene` · wave 2 · Sonnet · depends: WP06 merged; CP1's `a2_gplup_off*`.

- **Gate:** starts only if `a2_gplup_off1/2` (upgrades off) remove the > 100 ms frames whose zones include `pipe.upgrade` (or cut frames
  > 100 ms in WALK_BACK + SPIN_C by >= 1/3). Else cancelled in CP1.md.
- **Touches:** `nvn/nvn_gpl.cpp` `upgrade_main` (:542) only; `nvn/nvn_prewarm.cpp` saver loop (:256-283) only; `nvn/nvn_pipeline.cpp`
  `pipeline_cache_dirty` (:163) only; `src/host_device.cpp` (`host_is_handheld`, `host_is_wine`). Not `gpl_prepare`, `ensure_cache`,
  `save_cache` (WP12).
- **Behaviour:** on a handheld or under Wine, in play (WP06's gate): upgrade jobs run one at a time and only while the submit worker has < 2
  jobs queued (`SubmitWorker::depth()`, `nvn_submit_worker.h:112`), else wait 5 ms and re-check; the pipeline cache is saved at most every
  60 s in play, at once while loading, and at shutdown; active compile threads limited to 1 in play under Wine; `compile_priority` default
  `idle` on handhelds.
- **Keys:** `[nvn] gpl_upgrade_in_play` = `auto` (`idle` on handhelds / Wine, `on` elsewhere); `[nvn] pipeline_cache_save` = `auto` (`idle` on
  handhelds / Wine, `timer` elsewhere); `[nvn] compile_threads_in_play` = `auto` (1 under Wine, unlimited elsewhere); changed default
  `[nvn] compile_priority` = `auto` (`idle` on handhelds, `below` elsewhere).
- **Toggle:** `gpl_upgrade_in_play = on`, `pipeline_cache_save = timer`, `compile_threads_in_play = 0`, `compile_priority = below`.
- **Zones:** `pipe.upgrade.wait` (yield), `pipe.cache.save` (exists).
- **Windows / Proton:** desktop defaults unchanged (PC effect none). Go S: handheld path.
- **Acceptance:** CP2 (`cp2_hygoff` vs `cp2_w*`): none of the frames > 100 ms co-occurs with `pipe.upgrade` / `pipe.cache.save`; frames > 50
  ms not worse.

### WP11-texprep-in-play: texture preparation and uploads never wait in play
`claude/deck-stutter/wp11-texprep-in-play` · wave 2 · Sonnet · depends: WP06 (gate), WP03 merged.

- **Touches:** `nvn/nvn_texprep.cpp` `take` (:467), `eligible` (:372), `on_registered` (:427); `nvn/nvn_gpu_texture.cpp`
  `tex_ensure_current` (:1728) only.
- **Behaviour:** in play (WP06's latched gate): a Waiting/Queued job is moved to the front and the draw gets a stand-in instead of being
  prepared on the render thread (:486-496 today); a Running job is not waited for (same as `slow`, :502-505); the done result's re-hash
  (:523-526) moves to the worker (hash at job end, compare a generation on the render thread). A per-frame render-thread upload budget: once
  the frame's uploads exceed `upload_budget_ms`, a not-yet-uploaded CPU-sourced texture binds the 1x1 placeholder for that draw (render
  targets, `gpu_written`, never do); unlimited while loading. Uncompressed block-linear textures become texprep-eligible (CPU untile on the
  worker; today BC/ASTC only). With `tex_create_early`, the `VkImage` of a texture is created on the texprep worker at `texture_registered`
  time (`vkCreateImage`/VMA are thread-safe; `TexGpu` is published to the render thread with release/acquire).
- **Keys:** `[nvn] tex_prep_in_play` = `standin` (`wait` = today); `[nvn] upload_budget_ms` = `4` (0 = no budget); `[nvn] tex_create_early` =
  `true`; `[nvn] texprep_linear` = `true`.
- **Toggle:** `tex_prep_in_play = wait`, `upload_budget_ms = 0`, `tex_create_early = false`, `texprep_linear = false`.
- **Zones:** `tex.prep.standin` mark, `tex.upload.budget` mark, `tex.create.early` (worker).
- **Failure:** a texture stuck as a stand-in -> the next frame retries; CP screenshots.
- **Windows / Proton:** same on both. PC acceptance: harness cold + warm, not worse.
- **Docs:** CONFIG rows after `pool_lazy_cpu_untile`; CHANGELOG: "Fewer hitches when walking into new areas: textures that are not ready
  yet appear a frame later".
- **Acceptance:** CP2 (`cp2_texwait` vs `cp2_w*`): no render-thread `tex.prep.wait`; no LOAD frame > 100 ms with uploads > 8 ms (hitch lines);
  WALK_NEW frames > 50 ms not worse; no texture still a stand-in in p03-p06.

### WP12-precompile: opt-in compile-everything with our own store
`claude/deck-stutter/wp12-precompile` · wave 2 · Sonnet · depends: WP06 (compute records, same files); CP1's `cp1_steamcold`.

- **Step 0 (decided by CP1's `cp1_steamcold`):** a run with the Steam shader cache emptied but `pipeline_cache.bin` kept shows whether a
  RADV `VkPipelineCache` blob alone makes a run warm (no render-thread compile zones, cold LOAD like warm). If not, the store is useless on
  RADV: the package then ships only the cache-file fixes, the Mesa cap docs and the launch option, and the PR says so.
- **Touches:** `nvn/nvn_pipeline.cpp` `ensure_cache` (:101), `save_cache` (:134), `pipeline_cache_handle` (:577) and the cache argument of
  every `vkCreate*Pipelines` call in `nvn_pipeline.cpp` / `nvn_gpl.cpp` / `nvn_prewarm.cpp`; `nvn/nvn_gpl.cpp` `gpl_prepare` (:1059),
  `prepare_key` (:1024), `write_prepare_state` (:1051), `prepare_state_ready` (:1038), `learn_raster` (:616) (persist raster keys); new
  `nvn/nvn_pipeline_store.cpp` + `.h`; `game-build/src/pkp_main.cpp` (`prepare_shaders_main` :238, flags :306-322); `play/` Graphics page
  and `play_config.cpp` (new controls.ini key); `src/gfx/overlay.cpp` (progress page). Not `upgrade_main`, the saver loop or
  `pipeline_cache_dirty` (WP10).
- **Behaviour:**
  1. `ensure_cache`: size with `std::filesystem::file_size` (64-bit), refuse a `pipeline_cache.bin` over 1 GiB with one warning (it stays the
     small play-time cache); the preparation no longer writes its parts there when the store is on.
  2. Store: `shader_cache/pipelines/<prepare_key hash>/NNN.bin`, 256 shards, shard = first byte of a 64-bit hash of the vertex shader hash
     (pre-raster parts, whole pipelines), of the fragment shader hash (fragment parts) or of the compute shader hash. One `VkPipelineCache`
     per shard, loaded on a background thread (`nvn-store`) when a program of that shard is set (`gpl_program`, prewarm record load) or first
     needed; `store_cache_for(hash)` returns the loaded shard's cache or `VK_NULL_HANDLE` (then the main cache is used; it never waits).
     Resident shards are capped by `pipeline_store_mb` (LRU, unloaded only when no creation is using it: refcount).
  3. What it compiles: pre-raster + fragment parts of every program in `programs.bin` x every raster key ever seen (persisted with the key
     file; today `gpl_prepare` resets `g_rasters` to one default, :1084); vertex-input / fragment-output parts of known keys; whole pipelines
     of a merged key file (local `pipeline_keys.bin` + a shipped `pipeline_keys.shipped.bin`, deduped by digest, header stripped before
     concatenation); every compute record (WP06).
  4. Resume: after every 192-program batch (:1091) the touched shards are saved (temp + rename) and `prepare.state` gets `phase: <n>` and
     `batch: <i>`; a restart skips finished batches; a cancelled or failed run still writes the cursor (today it saves nothing on cancel,
     :1162, and writes no state after failures, :1168).
  5. Disk: the first batch measures bytes per entry and logs the projected total; the progress page shows it before continuing.
  6. UI and launch: controls.ini `[display] precompile_all = 0|1` (F1 Graphics: "Compile all shaders before playing (hours, resumable)"),
     mapped to `[nvn] precompile = everything` the way `fast_gpu_memory` maps to `pool_mirror` (`play.cpp:1199`). When set and the store is
     not complete for the current `prepare_key`, the game runs the preparation in-process before the title with a full-screen progress page
     (phase, done/total, ETA, MB written; B cancels and starts the game), and logs `nvn.prepare: phase <p> <done>/<total> eta <s> s <MB> MB`
     every 30 s. New CLI flags: `--prepare-shaders --everything` (headless, exits; the launcher's step) and `--precompile` (Steam launch option
     `%command% --precompile`: in-process page, then the game). The `prepare: <done> <total>` stdout line keeps its exact format (the
     launcher parses it, `runtime/apps/launcher/game_info.h:109`).
  7. Mesa cap docs (CONFIG.md and README's Steam Deck section): Steam sets `MESA_SHADER_CACHE_MAX_SIZE=5G` per game; to raise it, set the
     game's Steam launch options to `MESA_SHADER_CACHE_MAX_SIZE=20G %command%` (combine with `--precompile` as
     `MESA_SHADER_CACHE_MAX_SIZE=20G %command% --precompile`). Verify on the Deck at CP2 (`launch_env.txt`) before the docs claim it works.
- **Keys:** `[nvn] precompile` = `off` (`off | parts | everything`; `parts` = today's preparation) (`NXRT_NVN_PRECOMPILE`); `[nvn] pipeline_store`
  = `on` (`NXRT_NVN_PIPELINE_STORE`); `[nvn] pipeline_store_mb` = `512` (`NXRT_NVN_PIPELINE_STORE_MB`); controls.ini `[display] precompile_all`
  = `0`.
- **Toggle:** `precompile = off` and `pipeline_store = off` = today.
- **Zones:** `store.load` (arg = shard), `store.save`, `prepare.batch`.
- **Threading:** shard load/save on `nvn-store`; `VkPipelineCache` use is externally synchronised per shard by the refcount + a per-shard
  mutex for save (merge/get data); creations never wait for a load.
- **Failure:** disk full -> the batch's shard write fails, the cursor is not advanced, the page shows the error and starts the game; a Mesa /
  SteamOS update changes `prepare_key` (GPU, driver, cache UUID) -> the store is rebuilt and the page says why.
- **Windows / Proton:** same feature on both; the launcher's Prepare step can use `--everything`. PC acceptance: harness warm unchanged with
  the store off and on; a PC prepare run reports time and size.
- **Docs:** CONFIG rows after `prepare_cache`; CHANGELOG entry; README Steam Deck section (launch options, size, time).
- **Acceptance:** CP2 probe before the queue: `cp2_prep_a` (killed after 1800 s) then `cp2_prep_b` (resumes at the next batch: log shows the
  cursor), total time and bytes recorded. CP2: `cp2_storecold1-3` (cold, store kept) meet S1-S5 at 30 as the warm runs do and show 0
  render-thread compile zones in play; `cp2_nostore_cold` is the reference.

### WP13-readback-shadow: copy-ahead readback (conditional)
`claude/deck-stutter/wp13-readback-shadow` · wave 2 · Sonnet · depends: WP03 merged; CP1 data.

- Starts only if CP1's WP03 log shows the read-back texture is GPU-written every frame **and** the game's main thread waits on the reading
  thread (ReadTexels still costs a frame). Else cancelled in CP1.md.
- **Behaviour:** after the first ReadTexels of a texture, the submission that writes it (`tex_mark_gpu_written` sites in `nvn_replay.cpp`:
  :701, :721, :1192, :1294, :1385, :3539, :3652, :3700, :3738) also records the copy into its persistent `rb` buffer; ReadTexels then waits
  only for an already-submitted serial (often complete) and tiles. Off for a texture after 120 presents without a read.
- **Touches:** `nvn_gpu_texture.cpp` (`download_texture`, `tex_mark_gpu_written` :1993), `nvn_replay.cpp` only through
  `tex_mark_gpu_written`'s callee (no edits to the sites).
- **Keys / toggle:** `[nvn] readback_shadow` = `true` (`false` = WP03 behaviour). Zone `rb.ahead`.
- **Acceptance:** CP2 (`cp2_rbshadowoff` vs `cp2_w*`): `hle:nvnTextureReadTexels` p50 on the guest thread -50 %; frames > 50 ms not worse; CRC
  equal (`readback_check`).

### WP14-pacing-grid: present cadence locked to the refresh grid
`claude/deck-stutter/wp14-pacing-grid` · wave 3 · Sonnet · depends: WP05; CP2 (frames fit the period).

- **Touches:** `runtime/src/gfx/frame_limiter.cpp` (`begin_frame_nohook` :143, `kMaxCarryPeriods` :27); `runtime/src/gfx/vk_renderer.cpp`
  present path only if WP01's log shows `VK_KHR_present_wait` exposed under Proton.
- **Behaviour:** when snapped (WP05), schedule on the grid `t0 + n * k / hz`: a late frame takes the next grid point instead of shifting the
  schedule (today's carry rule moves the schedule off the grid after every late frame); no catch-up burst. With `present_wait`: the submit
  worker waits for present N-1's completion and the limiter re-phases `t0` to it (render thread never waits on it). Check that
  `submit_drain` before the limiter (`nvn_present.cpp:78`) is not a second pacing point (zone timing in the PR).
- **Keys / toggle:** `[display] fps_grid` = `true` (`false` = WP05 behaviour). Zone `limiter.grid` (arg = slots skipped).
- **Windows / Proton:** both; PC uses `present_wait` where exposed. PC acceptance: harness interval MAD not worse.
- **Acceptance:** CP3 (`cp3_gridoff` vs `cp3_w*`): S4 (>= 95 % of intervals within +-2 ms of 33.3, MAD <= 1.5 ms) in IDLE and SPIN_B; 0
  intervals > 50 ms in IDLE. Expected side effect: an even dt also evens the game's variable timestep (`timestep: ... smooth 4`).

### WP15-submit-slack: one present job of slack
`claude/deck-stutter/wp15-submit-slack` · wave 3 · Sonnet · depends: WP02/WP09 passed (the worker is no longer 92 % busy).

- **Touches:** `nvn/nvn_present.cpp` (:78 `submit_drain` call), `nvn/nvn_gpu.cpp` `submit_drain` (:783), `nvn/nvn_submit_worker.h` `drain`
  (:106) (new `drain_but_last_present()`).
- **Behaviour:** before a present, drain the worker only up to all-but-the-last present job when a swapchain image is free, so one long
  submit is absorbed instead of delaying the next frame (up to one frame of latency). Full drains stay for swapchain recreation,
  screenshots, sparse binds and shutdown.
- **Keys / toggle:** `[nvn] submit_slack` = `auto` (1 under Wine, 0 elsewhere; `0` = today). Zone `submit.drain.slack`.
- **Windows / Proton:** `submit_thread` is off on Windows: no change.
- **Acceptance:** CP3 (`cp3_slack0` vs `cp3_w*`): render `present` < 3 ms per frame; spikes whose worst zone is `submit.drain` -50 %; S1
  not worse. Probe after merge (rule 6): 1 run; if render `present` stays > 3 ms per frame, WP16 starts.

### WP16-present-on-worker: acquire + present as one worker job (conditional)
`claude/deck-stutter/wp16-present-on-worker` · wave 3 · Sonnet · depends: WP15 and its probe.

- **Behaviour:** fence wait + acquire + present-pass record + submit + present become one worker job with depth <= 2; the overlay recorded
  into its own secondary buffer; OUT_OF_DATE / HDR metadata / latency markers handled on the worker; full drain kept for recreation,
  screenshots, sparse binds, shutdown.
- **Touches:** `nvn/nvn_present.cpp`, `runtime/src/gfx/vk_renderer.cpp` (`begin_frame`, present path), `nvn/nvn_gpu.cpp`
  (`take_batch_for_present` :789, `present_batch_done`).
- **Keys / toggle:** `[nvn] present_on_worker` = `false` until CP3 passes, then `auto` (Wine). Zone `present.worker`.
- **Acceptance:** CP3 (`cp3_pow0` vs `cp3_w*`): render `present` + `submit.drain` < 1.5 s per 30 s (from 10 979 / 10 486 ms); S1 not worse;
  0 swapchain errors over the queue.

### WP17-hle-costs: game-thread flush / register costs
`claude/deck-stutter/wp17-hle-costs` · wave 3 · Sonnet · depends: WP01 zones; WP11 merged (texprep).

- **Touches:** `nvn/nvn_texture_registry.cpp` `textures_cpu_wrote` (:172); `nvn/api_memory.cpp` `texture_pool_register` (:396);
  `nvn/nvn_texprep.cpp` `on_registered` (:427), `on_flushed` (:415).
- **Behaviour:** index sparse-texture maps by 64 KiB page (interval map) so a flush touches only overlapping maps; batch `on_flushed`;
  copy the texture entry under the exclusive lock and call `texture_registered` after releasing it; move scheduling out of `on_registered`.
- **Keys / toggle:** `[nvn] tex_range_index` = `true` (`false` = today).
- **Acceptance:** CP3 (`cp3_hleoff` vs `cp3_w*`): `hle:nvnMemoryPoolFlushMappedRange` and `hle:nvnTexturePoolRegisterTexture` max < 3 ms
  (from 23 / 29 ms); PC harness not worse.

### WP18-pool-copy-p3: both-wrote merge and span merge
`claude/deck-stutter/wp18-pool-copy-p3` · wave 3 · Sonnet · depends: WP09 passed CP2.

- **Behaviour:** (1) for a recorded GPU write <= `pool_copy_merge_max_mb` (16), snapshot the guest bytes at record time; at landing, a byte
  that differs from the snapshot is the CPU's and is kept, the others get the GPU's (ultratears `pc_put` / `pc_back`); larger writes land whole
  as in phase 1. (2) No new spans in copy mode: a bind crossing into another chunk allocates one chunk over the union of the cells, copies the
  old chunks' pending GPU ranges into it with `vkCmdCopyBuffer` in the current command buffer, remaps `cell_ext`, and frees the old chunks by
  deferred destroy.
- **Keys / toggle:** `[nvn] pool_copy_merge` = `on` (`off` = phase 2); `[nvn] pool_copy_merge_max_mb` = `16`. Zones `pool.copy.snap`,
  `pool.copy.merge`.
- **Acceptance:** CP3 (`cp3_mergeoff` vs `cp3_w*`): `span_writes` = 0; landed MiB/s not worse; screenshots equal; frames > 50 ms not worse.

### WP19-retire-lazy: remove dead paths
`claude/deck-stutter/wp19-retire-lazy` · final wave · Sonnet · depends: a checkpoint passed with copy mode as the Wine default (CP2 or CP3).

- **Behaviour:** remove the userptr-lazy path under Wine: `pool_lazy` `auto` no longer turns on under Wine; keep `pool_lazy = on` for native
  only if CP numbers on PC show a use, else remove `lazy_on`, `lazy_cell`, `lazy_idle`, `lazy_cpu_untile`, `pool_lazy_wants_cpu` and the CPU
  untile fallback they feed; remove `poll_flush` if CP1 and CP3 both rejected it; remove the `gpl_gate = time` path only if no checkpoint used
  it. If F2's deletion rule fired, this package deletes copy mode instead and keeps lazy. One shared `host_is_wine()` replaces the duplicate
  checks (`nvn_gpu.cpp:1153`, `nvn_gpu_pool.cpp:121`).
- **Docs:** CONFIG rows removed or rewritten; CHANGELOG; the Deck ini workaround documented as unnecessary if WP09 showed it.
- **Acceptance:** merge gate; CP4.

### WP20-cleanup-docs: docs pass
`claude/deck-stutter/wp20-cleanup-docs` · final wave · Haiku · depends: WP19.

- CHANGELOG `## Unreleased` consolidated with the CP numbers (device named on every number); README Steam Deck section (frame limit and
  80 Hz, precompile, launch options, Mesa cap); CONFIG.md rows match the final defaults. No code.

---

## 5. Waves and checkpoints

### 5.1 Checkpoint tooling (Haiku, in `ds/`, git-ignored, PM reviews directly; before CP1)
- T1 `segments.py` + `analysis/cptable.py`: per segment add frames > 50 ms, frames > 66.7 ms, share of intervals within +-2 ms of the target,
  `p50 - L`; `cptable.py --stutter-free <L>` prints S1-S5 pass/fail over the medians.
- T2 `run.sh` / `queue.sh`: a `TARGET_MS=<ms>` token (passed to `segments.py`, not to the game); cache mode `steamcold` (empty the Steam
  shadercache, keep `pipeline_cache.bin` / keys); a `PREPARE=everything` token (game args `--prepare-shaders --everything`); record
  `launch_env.txt` as today.
- T3 `soak.nxs`: `p0.nxs`'s in-world part repeated to 60 min.
- T4 PM (SSH, during `cp1_w1`): `ps -eLo tid,ni,comm` for the compile threads' nice level; `/proc/vmstat` thp/compact counters at 10 Hz;
  `perf record -g` of the `nvn-submit` thread for 20 s in WALK_BACK if `perf` exists on SteamOS (else noted).
- T5 `ds/earlier-deck-findings.md` corrections (J-F Fix 13: `vkm_sync_all` is :368-386; totk figure is 27-31 -> 60 fps same scene).

### 5.2 Addendum-2 runs
`BASELINE-ADDENDUM-2.md` is not in (Deck offline). What each run gates: `d_gplup_off` x2 -> WP10's start; `d_cell64` -> WP08's start; the
`i1` submit split -> the reading of WP04's result and the priority of WP15/WP16 (and every "17.8 ms per submission" figure). If still missing
at CP1 they fold in: `a2_gplup_off1/2` and `a2_cell64` on `base0.dll` (first lines of the CP1 queue); the `i1` split is carried by
`cp1_alloff1/2` (the CP1 DLL contains `3dc1a05` and every wave-1 toggle is set to its old path there).

### 5.3 Wave 1 -> CP1

Packages: WP07 (merge first), WP01, WP02, WP03, WP04, WP05, WP06. Function ownership in shared files:

| File | WP01 | WP02 | WP03 | WP04 | WP06 |
|---|---|---|---|---|---|
| `nvn_gpu.cpp` | `submit_cbs` | `read_completed` | new `queue_record`, `wait_serial_grace` after `run_frame_sync` | `want_flush`, `serial_done`, `render_loop`, `wait_serial`, `Gpu` fields | - |
| `nvn_gpu_pool.cpp` | `import_range` | functions listed in WP02 | - | - | new `pool_flags_at` at end of file |
| `nvn_replay.cpp` | reason scopes in `build_set_resolved`, `begin_draw`, `do_draw_elements`, `do_draw_indirect`, `indirect_index_span`, `do_copy_buffer_texture`, `do_write_memory`, `run_list` copy/clear cases | `run_list` query-report cases, `shadows_after_dispatch`, `flush_compute_out` | - | - | `note_frame_for_pacing`, `replay_begin_submit`, `do_dispatch`, `run_list` `CopyBufferToBuffer` case (granule set: one line after WP01's scope) |
| `nvn_gpu_texture.cpp` | `gpu_untile_impl` (scope) | - | `download_texture`, `TexGpu` | - | `TexGpu::read_back` field (set in WP03's `download_texture` after rebase; WP06 adds the field) |
| `vk_device.cpp` | device-creation log, gauge | `create_buffer` | - | - | - |

WP06 merges after WP01 and WP03 (it rebases on them for the shared lines); WP04 after WP07 (its CONFIG row anchor).

CP1 queue (`ds/checkpoints/cp1.q`; `cp1.dll` = integration head after the last wave-1 merge; ~20 runs, ~70 min; `TARGET_MS=33.333` because
the snapped stock cap is 30 on the 90 Hz panel; the all-off and snap-off runs run at 40):

```
a2_gplup_off1  base0.dll p0.nxs warm 200 NXRT_NVN_GPL_UPGRADE=off
a2_gplup_off2  base0.dll p0.nxs warm 200 NXRT_NVN_GPL_UPGRADE=off
a2_cell64      base0.dll p0.nxs warm 200 NXRT_NVN_POOL_LAZY_CHUNK_MB=64
cp1_w1         cp1.dll   p0.nxs warm 200 TARGET_MS=33.333
cp1_w2         cp1.dll   p0.nxs warm 200 TARGET_MS=33.333
cp1_w3         cp1.dll   p0.nxs warm 200 TARGET_MS=33.333
cp1_alloff1    cp1.dll   p0.nxs warm 200 TARGET_MS=25 NXRT_NVN_POOL_COPY=off NXRT_NVN_READBACK_ASYNC=false NXRT_NVN_GPL_GATE=time NXRT_NVN_ASYNC_WHOLE=false NXRT_NVN_ASYNC_COMPUTE=false NXRT_NVN_COMPUTE_PREWARM=false NXRT_NVN_POLL_FLUSH=now NXRT_DISPLAY_FPS_SNAP=false
cp1_alloff2    cp1.dll   p0.nxs warm 200 TARGET_MS=25 NXRT_NVN_POOL_COPY=off NXRT_NVN_READBACK_ASYNC=false NXRT_NVN_GPL_GATE=time NXRT_NVN_ASYNC_WHOLE=false NXRT_NVN_ASYNC_COMPUTE=false NXRT_NVN_COMPUTE_PREWARM=false NXRT_NVN_POLL_FLUSH=now NXRT_DISPLAY_FPS_SNAP=false
cp1_copyoff1   cp1.dll   p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POOL_COPY=off
cp1_copyoff2   cp1.dll   p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POOL_COPY=off
cp1_copycell4  cp1.dll   p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POOL_COPY_CELL_MB=4
cp1_rboff      cp1.dll   p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_READBACK_ASYNC=false
cp1_gploff     cp1.dll   p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_GPL_GATE=time NXRT_NVN_ASYNC_WHOLE=false NXRT_NVN_ASYNC_COMPUTE=false NXRT_NVN_COMPUTE_PREWARM=false
cp1_pf8a       cp1.dll   p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POLL_FLUSH=8
cp1_pf8b       cp1.dll   p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POLL_FLUSH=8
cp1_pfdefer    cp1.dll   p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POLL_FLUSH=defer
cp1_snapoff    cp1.dll   p0.nxs warm 200 TARGET_MS=25 NXRT_DISPLAY_FPS_SNAP=false
cp1_steamcold  cp1.dll   p0.nxs steamcold 200 TARGET_MS=33.333
cp1_cold1      cp1.dll   p0.nxs cold 200 TARGET_MS=33.333
cp1_coldgploff cp1.dll   p0.nxs cold 200 TARGET_MS=33.333 NXRT_NVN_GPL_GATE=time NXRT_NVN_ASYNC_WHOLE=false NXRT_NVN_ASYNC_COMPUTE=false NXRT_NVN_COMPUTE_PREWARM=false
```

The `a2_*` lines are dropped if addendum 2 already has those runs. Cold runs stay last (they delete the caches).

CP1 thresholds (medians; `cp1_w*` = defaults): WP01, WP02, WP03, WP04, WP05, WP06 as in their Acceptance fields. Headline vs the baseline
warm medians (frames > 50 ms and > 100 ms per segment, absolute ms, since the cap differs): reported, not gated. CP1.md also records the
WP08 / WP10 / WP12-step-0 / WP13 start decisions.

### 5.4 Wave 2 -> CP2

Packages: WP09, WP10 (if its gate passed), WP11, WP12, WP13 (if started), WP08 (if started). Function ownership: WP10 owns `upgrade_main`,
the prewarm saver loop, `pipeline_cache_dirty`; WP12 owns `ensure_cache`, `save_cache`, `pipeline_cache_handle`, the cache argument at
pipeline creation sites, `gpl_prepare` and the prepare-state functions, `learn_raster`; WP11 owns `tex_ensure_current` and texprep; WP13
owns `download_texture` and `tex_mark_gpu_written`; WP09 owns `nvn_pool_copy.cpp` and `write_watch.cpp`. WP12 merges after WP10.

Probes before the queue (overnight, not in the budget): `cp2_prep_a`, `cp2_prep_b` (WP12), `cp2_soak`:

```
cp2_prep_a     cp2.dll p0.nxs warm 1800  PREPARE=everything
cp2_prep_b     cp2.dll p0.nxs warm 43200 PREPARE=everything
cp2_soak       cp2.dll soak.nxs warm 3700 TARGET_MS=33.333
```

CP2 queue (`ds/checkpoints/cp2.q`, ~15 runs, ~55 min; lines for packages that did not start are dropped):

```
cp2_w1          cp2.dll p0.nxs warm 200 TARGET_MS=33.333
cp2_w2          cp2.dll p0.nxs warm 200 TARGET_MS=33.333
cp2_w3          cp2.dll p0.nxs warm 200 TARGET_MS=33.333
cp2_w4          cp2.dll p0.nxs warm 200 TARGET_MS=33.333
cp2_w5          cp2.dll p0.nxs warm 200 TARGET_MS=33.333
cp2_trackoff    cp2.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POOL_COPY_TRACK=off
cp2_copyoff1    cp2.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POOL_COPY=off
cp2_copyoff2    cp2.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POOL_COPY=off
cp2_hygoff      cp2.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_GPL_UPGRADE_IN_PLAY=on NXRT_NVN_PIPELINE_CACHE_SAVE=timer NXRT_NVN_COMPILE_THREADS_IN_PLAY=0 NXRT_NVN_COMPILE_PRIORITY=below
cp2_texwait     cp2.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_TEX_PREP_IN_PLAY=wait NXRT_NVN_UPLOAD_BUDGET_MS=0 NXRT_NVN_TEX_CREATE_EARLY=false NXRT_NVN_TEXPREP_LINEAR=false
cp2_rbshadowoff cp2.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_READBACK_SHADOW=false
cp2_storecold1  cp2.dll p0.nxs cold 200 TARGET_MS=33.333
cp2_storecold2  cp2.dll p0.nxs cold 200 TARGET_MS=33.333
cp2_storecold3  cp2.dll p0.nxs cold 200 TARGET_MS=33.333
cp2_nostore_cold cp2.dll p0.nxs cold 200 TARGET_MS=33.333 NXRT_NVN_PIPELINE_STORE=off
```

CP2 thresholds: each package's Acceptance; **the 30-lock target: S1-S5 and S8 on `cp2_w1-5`, S6 on `cp2_storecold1-3`, S7 on
`cp2_nostore_cold`**; soak: S1-S4 hold in its last 200 s and `nvn.pool.copy.live_mb` / `mem_avail_mb` do not drift by more than 10 % over the
last 50 min. A failed criterion lists the zones of the failing frames in CP2.md; they seed wave 3 fix packages. F2's deletion rule is
evaluated here (WP09 Acceptance).

### 5.5 Wave 3 -> CP3

Packages: WP14, WP15, WP16 (after WP15's probe), WP17, WP18. WP14 owns `frame_limiter.cpp` (and `vk_renderer.cpp` present-wait only);
WP15 owns the `nvn_present.cpp:78` call, `submit_drain`, `SubmitWorker::drain`; WP16 rebases on both; WP17 owns the registry / api_memory /
texprep functions it lists; WP18 owns `nvn_pool_copy.cpp`.

CP3 queue (`ds/checkpoints/cp3.q`, ~16 runs, ~60 min; before the `cp3_80*` lines the PM sets the panel to 80 Hz in Steam's Performance menu
and checks the `play: frame cap 40 (80 Hz ...)` line):

```
cp3_w1        cp3.dll p0.nxs warm 200 TARGET_MS=33.333
cp3_w2        cp3.dll p0.nxs warm 200 TARGET_MS=33.333
cp3_w3        cp3.dll p0.nxs warm 200 TARGET_MS=33.333
cp3_gridoff   cp3.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_DISPLAY_FPS_GRID=false
cp3_slack0    cp3.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_SUBMIT_SLACK=0
cp3_pow0      cp3.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_PRESENT_ON_WORKER=false
cp3_hleoff    cp3.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_TEX_RANGE_INDEX=false
cp3_mergeoff  cp3.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POOL_COPY_MERGE=off
cp3_pf8a      cp3.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POLL_FLUSH=8
cp3_pf8b      cp3.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POLL_FLUSH=8
cp3_copyoff   cp3.dll p0.nxs warm 200 TARGET_MS=33.333 NXRT_NVN_POOL_COPY=off
cp3_storecold cp3.dll p0.nxs cold 200 TARGET_MS=33.333
cp3_80a       cp3.dll p0.nxs warm 200 TARGET_MS=25
cp3_80b       cp3.dll p0.nxs warm 200 TARGET_MS=25
cp3_80c       cp3.dll p0.nxs warm 200 TARGET_MS=25
cp3_80off     cp3.dll p0.nxs warm 200 TARGET_MS=25 NXRT_DISPLAY_FPS_GRID=false
```

(If WP04 shipped a Wine default at CP1, the `cp3_pf8*` lines use `NXRT_NVN_POLL_FLUSH=now` instead.) The PM sets the panel back to 90 Hz
after the queue.

CP3 thresholds: each package's Acceptance; S1-S5 and S8 at 30 hold on `cp3_w*` (no regression from CP2); stretch: S1-S5 at 40 on
`cp3_80a-c`. WP04 re-measure as in its Acceptance.

### 5.6 Final wave -> CP4 (final)

Packages: WP19, WP20. CP4 = the brief's full protocol on the integration head: on each of the 3 Decks 3x warm (`p0.nxs`), 3x cold with the
store, 1x cold without it, 1x soak; one Deck also 3x at 80 Hz. PC: `winharness` 3x warm + 1x cold + 1x windowed vs WIN-BASELINE.md (the PC rule of
§3.2 step 3 on the medians; every in-world segment neutral or better). Legion Go S: 3x warm vs its own pre-rework runs. Pass: S1-S8 at 30 on every Deck; PC and Go S neutral or better.
Then the integration branch -> `main` PR for the owner.

### 5.7 Package list

| Id | Wave | Tier | Gate to start |
|---|---|---|---|
| WP01-instr | 1 | Sonnet | - |
| WP02-pool-copy-p1 | 1 | Opus | - |
| WP03-readback | 1 | Opus | - |
| WP04-poll-flush | 1 | Sonnet | - |
| WP05-frame-snap | 1 | Sonnet | - |
| WP06-never-block-compiles | 1 | Opus | - |
| WP07-config-rows | 1 | Haiku | - |
| WP08-lazy-tune | 2 | Haiku | CP1: WP02 failed and a cell size won |
| WP09-pool-copy-p2 | 2 | Opus | CP1: WP02 passed |
| WP10-compile-hygiene | 2 | Sonnet | CP1: `a2_gplup_off` gate |
| WP11-texprep-in-play | 2 | Sonnet | WP06, WP03 merged |
| WP12-precompile | 2 | Sonnet | WP06 merged; CP1 `cp1_steamcold` decides scope |
| WP13-readback-shadow | 2 | Sonnet | CP1: WP03 log shows the need |
| WP14-pacing-grid | 3 | Sonnet | CP2 |
| WP15-submit-slack | 3 | Sonnet | WP02/WP09 passed |
| WP16-present-on-worker | 3 | Sonnet | WP15 probe: render `present` > 3 ms/frame |
| WP17-hle-costs | 3 | Sonnet | WP11 merged |
| WP18-pool-copy-p3 | 3 | Sonnet | CP2: WP09 passed |
| WP19-retire-lazy | final | Sonnet | a checkpoint passed with copy mode default |
| WP20-cleanup-docs | final | Haiku | WP19 |

---

## 6. Disagreements and how they were decided

**Lazy pool fate.** O1 and F1 replaced it with copy mode; O2 kept it (never drop + prefetch) and F2 reworked it (async import, skipped draws,
never drop), both gated on a kernel profile or on `d_lazy_off`. Both judges picked replacement: `d_lazy_off` met the O2/F2 gates (cost scales
with userptr bytes) and `d_idle_big` refuted never-drop. Decided: replace under Wine with copy mode (F2), keep lazy as the A/B fallback until a
checkpoint passes, then WP19 retires it.

**Copy-mode design and key.** J-O and J-F designed a device-local or GTT shadow backed in 2 MiB sparse granules on top of the S38 mirror, with
exact write tracking from the start, under `pool_memory` / `pool_mode = auto|import|lazy|copy`. The refs note (`ds/refs/COPY-MODE-REFERENCE.md`
§7) proposes a phased host-visible per-cell shadow. Decided by the PM: the refs note's phases (WP02 -> WP09 -> WP18) under `pool_copy =
auto|on|off`. Phase 1 copies by touched page range rather than whole cells because the note's own cost estimate (15-28 MB per frame, the bytes
draws read) only holds at range granularity; a whole 16 MiB cell per bind would be hundreds of MB per frame. Sparse granules stay out (sparse
binds drain the worker today, `nvn_gpu_texture.cpp:398-403`).

**When copy mode starts.** J-O put it in wave 3 after the readback fix (the readback noise would hide its effect); J-F in wave 2. Decided (F3):
wave 1, because it is the longest package and its CP1 metric (`submit.worker.batch` ms per call) is not hidden by the readback stalls.

**Copy mode first or fewer submissions first.** F2 led with one submission per frame; O1 made fewer submissions conditional on a fixed
per-submit cost; F1 did both. Both judges: both levers multiply (cost = submissions x bytes). Decided (F3): both; WP04 is a keyed wave-1
experiment that ships only if CP1 shows p50 and spikes both better; the "answer the window acquire sync" variant is dropped (-41 % on the Go S).

**PR #126.** All four plans kept the mechanism and rejected merging it as is; they differed on compute (O1: prebuild, wait when missing; O2:
skip; F1: prebuild only; F2: skip except CPU-read). Both judges: keep with changes, F2's compute exception. Decided (F4): WP06, with a concrete
skip rule (CPU-no-access pools only, minus ranges ever copied to CPU-visible pools, minus read-back storage images).

**Frame cap and preset.** F1 rounded to the nearest divisor (40 -> 45 on 90 Hz); F2 made 30 the preset; O1/O2 snapped down. Both judges
rejected rounding up; J-F leaned to shipping 30 until copy mode passes, J-O left the preset number to the owner. Decided (F5): snap down, preset
stays 40 (runs as 30 on the 90 Hz panel), 45 only as a choice.

**Precompile storage.** O1/O2: own sharded store; F1: `pipeline_cache.bin` plus a raised Mesa cap; F2: `pipeline_cache.bin` plus a sample check.
J-O took the launch option only as a fallback, J-F as belt and braces. Decided (F6): own sharded store with the cache-file fixes, the Mesa cap
documented with how to raise it, and CP1's `cp1_steamcold` deciding whether the RADV blob makes the store useful at all (WP12 step 0).

**Readback flush.** O2/F1/F2 forced a flush for the copy (one more ~18 ms submission per readback); O1 waited up to one frame for the present
to carry it. Both judges took O1's grace, flushing after it so a game that waits on the reader cannot deadlock. Decided: WP03 step 5.

**Loading gate.** O2 proposed game-thread presents; the others the replay's light-frame detector. Both judges refuted the game-thread signal
(`present.backpressure` stalls it too) and took the detector latched per submission. Decided: WP06 step 1, with detection split from the
`unpaced_loading` limiter action so the gate works when that key is off.

**Render thread / worker decoupling.** F1 moved the whole present to the worker early; O1 proposed one job of slack after copy mode. J-O took
F1's design with slack as the smaller variant; J-F took slack first and F1's design only if the drain stays high. Decided: WP15 (slack) after
copy mode, WP16 (present on the worker) only if WP15's probe shows render `present` > 3 ms per frame.

**Stutter-free definition.** J-O: p50 within 2 % of L, at most 2 frames over 2L per load, a 60-min soak. J-F: p50 within +-1 ms, at most 5
frames over 2L and none over 100 ms per load, the same numbers cold with the precompile. Decided (F8): J-F's (§2); the soak stays as a CP2
probe.

**Tiers.** Both judges put copy mode, never-block compiles and present-on-worker at Fable, precompile and texprep at Opus. Decided (F11):
Opus for WP02, WP03, WP06, WP09; Sonnet for the rest (including WP12, WP16, WP18); Fable only after two Opus rejections.

**Same file in one wave.** The brief separates same-file packages into waves; the fixed wave-1 content puts WP02, WP03, WP04 and WP06 (and
WP01) into `nvn_gpu.cpp` / `nvn_gpu_pool.cpp` / `nvn_replay.cpp` at once. Decided by this plan: disjoint function ownership per file (§3.6,
§5.3), new copy-mode code in its own file (`nvn_pool_copy.cpp`), and a fixed merge order. The PM confirms this when approving the plan.

**Interim lazy tuning.** J-O: retune cells/idle by measurement until copy mode lands; J-F: config A/B, code items (span double-imports, the
import mutex) only if copy mode slips. Decided: config runs in CP1 only (`a2_cell64`, `cp1_copycell4` for copy mode); WP08 changes a default
only if copy mode failed CP1; the code items are dropped (§7).

---

## 7. Dropped items

- Never-drop (`pool_lazy_idle` huge) as a fix (O2, F2, F1, O1 option): `d_idle_big` showed no gain; under byte scaling it raises the floor.
- Prefetching lazy cells ahead of the bind (O2 WP5, F2 C3b, F1 option 2): imports more bytes (raises the floor), still stalls the concurrent
  submit, moot under copy mode; its "existing worker" and "recording layer resolves addresses" premises are false.
- Async import as a main line (O2, F2): moves the import onto the worker beside the submit; no effect on the floor.
- Skipping draws while their cell imports (F2 C3a): superseded by copy mode; missing geometry costs more than shader pop-in.
- Span double-import and import-mutex cleanups in the lazy path (J-F Fix 5 code items): J-F conditioned them on copy mode slipping; copy mode
  starts in wave 1; WP18 removes spans in copy mode.
- "Import size is not the lever" / "cost did not fall with bytes" (O2, F2) and H2 as a hypothesis (O1, F2): contradicted by `d_lazy_off`.
- "Drop + re-import causes the spin stutter" (handoff §2 suspicion): refuted by `d_idle_big` and the drop timing.
- "21-87 ms per 16 MiB import, 5-20x nominal" cost arguments (O1, O2, F1): selection artefact of the >= 20 ms log filter.
- "17.8 ms per `vkQueueSubmit2`" as an exact per-submission cost (all plans): an average over batch and present jobs; the `i1` split
  (CP1 `cp1_alloff*`) replaces it.
- Merging the present into the poll flush (F2 C1b): the batch is gone once flushed; cannot give one submission per frame.
- Answering the window acquire sync as "presented" (F2 C1): measured -41 % on the Go S (`ds/issues.md:251`).
- Snap `hz / round(hz / cap)` (F1 C8): 40 -> 45 on 90 Hz, above the cap.
- 30 as the `steamdeck` preset value (F2 C9a): F5 keeps 40 (runs as 30 on 90 Hz by the snap).
- One-period limiter catch-up after a late frame (F2 C9c): uneven pacing by S4; WP14 slots late frames onto the grid instead.
- Game-thread presents as the loading gate (O2 WP3): the game thread blocks in `present.backpressure` too.
- `present_wait` as a planned fix (O1 F7.2, O2 WP9): kept only as "if exposed" inside WP14 (`present_wait` is unused today).
- Present on the worker as early work (F1 C4 in wave 2): no gain while the worker is 92 % busy; WP16 is conditional after WP15.
- Sparse windows "because the Deck's pool exceeds 2 GiB" (F2 C4): the 2 GiB limit is the AMD Windows driver's; RADV took 3500 MiB as one buffer.
- Device-local sparse granules for the copy-mode shadow (J-O M2, J-F Fix 4): replaced by the refs note's host-visible cells (F2); sparse
  binds drain the worker.
- "Compute shadows copy ~285 MiB/s" as a stutter source (O2 A12, F1): not seen in the baseline; WP02 measures landed bytes instead.
- "3-4 min" Deck precompile estimate and "~9 KB per whole pipeline" (O2 WP4): unsupported; WP12 measures its own size and time.
- Changing the launcher's `prepare:` line format (O2 WP4): the launcher parses `prepare: <done> <total>`; WP12 keeps it and logs phase/ETA
  separately.
- Windows CI runs of the copy-mode tests (J-O M2): PR checks stay off (#127); the merge gate runs both modes locally.
- Separate worker-zone split package (O2 WP0, F1 C0, F2 C10): already in `3dc1a05`.
- "Fewer submissions only if H2" (O1 F10): the condition is gone; WP04 runs unconditionally as an experiment.
- Persisting a "game-thread present" or "a 70 ms frame moves the spin ~7 deg" claim (O2 A10): unsupported as stated; the even-dt point is kept
  in WP14's expected effect.
