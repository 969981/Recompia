# Experimental selected-texture readback candidate (#276)

This candidate replaces deferred landing's completion wait with a timeline poll,
retains pending work, and gives each deferred request a private buffer. It is
disabled by default. It is not a confirmed general fix for #276 or the earlier
deferred-readback freeze.

## Build and activation

Build the runtime using the repository's normal Windows build instructions in
`CONTRIBUTING.md`. The change does not add guest hooks or require relifting an
otherwise matching game executable. Test only with matching runtime/game builds;
matching exports alone do not establish compatibility with every release.

For the first hardware experiment, we built an unchanged runtime and this
candidate against `v0.8.3-beta.1`, commit
`59607eccafa9d9565d1f8a76230c4d58271117f2`, using clang-cl 23.1.4 and VS 2022
Build Tools, optimized RelWithDebInfo. PDB/map generation was enabled locally for
diagnosis; those build-only changes and binaries are not part of this patch.

Use an isolated installation and copied saves. Preserve its original runtime,
configuration and saves before replacing anything. First validate the unchanged
baseline, then the candidate with `readback_defer=false`.

Only after identifying the current texture in the log, restart with:

```ini
[nvn]
readback_defer = true
readback_defer_texture = 286
readback_log = true
```

286 is the ID observed in our test, not a permanent identity. The candidate also
requires RGBA8_SRGB, 30x16x1, one level/layer, at most 64 KiB, a recent repeated
request, a valid previous result, and matching storage, range, CPU generation and
synchronous-refresh epoch. `readback_defer_texture=0` disables the experiment.
This key is a diagnostic selector, not an automatic game-specific identification
scheme.

The native policy checks can be run from the repository root in an x64 VS
developer shell with LLVM on PATH:

```powershell
clang-cl /nologo /std:c++20 /EHsc /MT /W4 runtime/tests/readback_policy_test.cpp /Fe:readback_policy_test.exe
if ($LASTEXITCODE) { throw 'Policy test compilation failed' }
.\readback_policy_test.exe
if ($LASTEXITCODE) { throw 'Policy checks failed' }
```

The 20 checks cover eligibility/invalidation and range boundaries; they do not
exercise Vulkan synchronization, asynchronous lifetime or gameplay semantics.

## First hardware result

Steam Deck OLED under Proton, using an isolated Testing installation without LSFG
launch options:

| Runtime | Observation |
| --- | --- |
| Locally compiled baseline | Loaded and played; compatibility check completed. |
| Candidate, experiment disabled | Loaded and played; recurring hitch remained. |
| Candidate, experiment enabled | User reported the hitch appeared gone in the first short test. |

These were sequential manual tests, not a controlled benchmark with verified
identical effective caps. The captured enabled log covers about four minutes
from startup. Examples below omit the ordinary log prefixes and personal paths:

```text
readback_poll: texture 286 serial 1372 state pending, reserved 1377 flushed 1368 submitted 1368 timeline 1368
readback_poll: texture 286 serial 1372 state complete, reserved 2051 flushed 2051 submitted 2051 timeline 2051
readback_log: 20 reads in 10 s: mean 0.02 ms, max 0.06 ms, 0 over 16 ms, 0 with a forced flush
readback_log: 20 reads in 10 s: mean 0.02 ms, max 0.05 ms, 0 over 16 ms, 0 with a forced flush
readback_log: 20 reads in 10 s: mean 0.02 ms, max 0.06 ms, 0 over 16 ms, 0 with a forced flush
```

The first 200 poll observations were 100 Pending and 100 Complete, with no Failed
entries. This is bounded diagnostic output, not exhaustive work accounting. No
device-lost, fatal or readback-failed messages were found in that capture.

Readback call durations measure CPU time, not asynchronous GPU-copy duration or
complete frame pacing. Zero forced-flush reports in those summaries do not mean
no GPU flush occurred: the candidate schedules an asynchronous flush explicitly.
Subsequent LSFG experiments must be reported separately from this first capture.

## Review and validation still required

- Identify the guest consumer and establish whether older texture data is valid.
  Storage/generation matching does not prove semantic correctness.
- Review buffer retirement, cancellation, submission ordering and teardown.
  Existing CPU-write notifications and backend lifetime assumptions are inherited.
- Verify normal save/load, transitions and longer gameplay without visual or
  gameplay regressions or freezes.
- Repeat matched 40/45 FPS capped and uncapped tests without LSFG; record effective
  settings and full frame/present timing, then repeat with logging disabled.

Pending work and cached-result reuse are bounded to under two seconds; other
cases use a fresh read with the separate normal buffer. This limit is a diagnostic
guard, not a demonstrated acceptable gameplay latency.

The existing blocking wait already requests submission progress. Adding an
ordered asynchronous flush does not establish that a missing flush caused the
previous freeze. The original freeze has not been reproduced and causally
explained by this test.

Developed with Codex assistance. Source review and initial manual hardware testing
are recorded here so maintainers can assess the change before considering a beta.
