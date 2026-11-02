"""Gameplay-run helpers for scripted runs on the real game (S13).

  import gameplay as gp
  lines = gp.prologue() + gp.warp('tallon', 'tallon_canyon_cavern') + ['stick left 0 1.0', '@run 3', '@pass']
  res = gp.run(lines, 'work/verify/walk60', rate=60)      # rate: deterministic mode with one frame = 1/rate s of game time; None = real time
  rows = gp.rows('work/verify/walk60')                    # trace rows (dicts) that carry the player

Facts the scripts rely on (all read off the running game, see runtime/docs/S13.md):
  * The world is Y up: forward is +Z at yaw 0 and left is +X, walking speed 12.8 units/s. yaw in the trace is the heading about Y (+ = turn left).
  * The game clock (igt) stands still while an area loads, so waits are written in game seconds with `@run`, not frames.
  * Front-end presses are made with `@mash`, which presses A every few seconds until the new game's first area is current.
  * `@warp world area` loads any area with the player in control after about 4 s of game time; a new game start (Frigate) comes first.
"""
import json, os, subprocess, sys, time

def _project_root():
    """The project folder: NXRT_ROOT, else the nearest parent of this file that holds both tools/ and work/."""
    import os, pathlib
    env = os.environ.get('NXRT_ROOT')
    if env:
        return pathlib.Path(env).as_posix()
    here = pathlib.Path(__file__).resolve()
    for p in here.parents:
        if (p / 'tools').is_dir() and (p / 'work').is_dir():
            return p.as_posix()
    return here.parents[2].as_posix()


ROOT = _project_root()
EXE_DIR = os.environ.get('NXRT_VERIFY_EXE', ROOT + '/game-build-verify/build/snap')
RUN_DIR = ROOT + '/work/run_verify'
TICK_HZ = 19200000


def prologue(first_area='frigate_exterior_docking_hangar', timeout_ms=240000):
    """Title -> New Game -> the Frigate's first area (cutscene running, player not yet controllable)."""
    return ['@timeout 0', '@sleep 8000', '@mash A 5000 area %s timeout=%d' % (first_area, timeout_ms), '@run 2']


def warp(world, area, settle=4.0):
    """Warp and wait until the player is in control (game seconds)."""
    return ['@warp %s %s' % (world, area), '@run %g' % settle]


def run(lines, out, rate=None, env=None, seconds=600, gfx='headless', exe_dir=None, watchdog=60):
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    cwd = os.path.join(out, 'cwd')            # own working directory per run: save folder, controls.ini, logs; the texture cache is shared by a junction
    os.makedirs(cwd, exist_ok=True)
    cache = os.path.join(cwd, 'bc7_cache')
    if not os.path.exists(cache):
        src = os.path.normpath(os.path.join(ROOT, 'work', 'run', 'bc7_cache'))
        subprocess.run('mklink /J "%s" "%s"' % (os.path.normpath(cache), src), shell=True, capture_output=True)
    script = os.path.join(out, 'script.nxs')
    open(script, 'w').write('\n'.join(lines) + '\n')
    e = dict(os.environ)
    e.update(NXRT_TEST_SCRIPT=script, NXRT_TEST_OUT=os.path.join(out, 'out'), NXRT_TEST_REGIONS=ROOT + '/tools/test/regions.tsv', NXRT_BOOT_GFX=gfx,
             NXRT_FS_ROMFS=ROOT + '/game/romfs', NXRT_FS_SAVE_DIR=os.path.join(cwd, 'save'), NXRT_TEST_WALL_TIMEOUT_S=str(seconds), NXRT_TIME_MODE='real',
             NXRT_DIAG_WATCHDOG_SECONDS=str(watchdog))
    if gfx == 'headless':
        e['NXRT_GFX_HEADLESS'] = '1'
    if rate:
        e['NXRT_TEST_DETERMINISTIC'] = '1'
        e['NXRT_TEST_CLOCK_STEP_TICKS'] = str(int(round(TICK_HZ / rate)))
    e.update(env or {})
    cmd = [(exe_dir or EXE_DIR) + '/mpr.exe', '--log', os.path.join(out, 'mpr.log'), '--max-seconds', str(seconds)]
    t = time.time()
    try:
        rc = subprocess.run(cmd, cwd=cwd, env=e, capture_output=True, text=True, timeout=seconds + 60).returncode
    except subprocess.TimeoutExpired:
        rc = 124
    res = dict(exit=rc, wall=time.time() - t)
    rp = os.path.join(out, 'out', 'result.json')
    if os.path.exists(rp):
        res.update(json.load(open(rp)))
    return res


def rows(out, area_prefix='', player_only=True):
    r = []
    for line in open(os.path.join(out, 'out', 'trace.jsonl')):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if 'f' not in d or (player_only and 'pos' not in d) or not d.get('area', '').startswith(area_prefix):
            continue
        r.append(d)
    return r
