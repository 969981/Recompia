"""S11 physics and timing probe: walk, jump and turn at 60, 120, 144 and 240 steps per second, in game time.

ONE mpr.exe process runs everything. The script is the prologue (title -> New Game, real time), then for each rate R and scenario S:

  gameplay.warp('tallon', 'tallon_canyon_cavern')      real time, includes the settle
  @rate R                                             the game step is exactly 1/R s per frame (the clock itself stays real)
  @hash <S>_<R>_start
  the scenario actions (below)
  @hash <S>_<R>_end
  @rate 0

Scenario actions (times in game seconds with @run; pad and mouse steps count frames, one frame = 1/R s under @rate)
  walk   stick left 0 1.0, @run 3, stick left 0 0, @run 1.5
  jump   press B for round(0.4 R) frames, @run 2
  turn   mouse 1000/n 0 for n frames with n = round(0.2 R), @run 0.5

Analysis: for each (scenario, rate) the trace rows between the start and end checkpoint frames (result.json "checkpoints") are the window.
All time is igt (game play time). Horizontal motion is x and z, height is y. Walk speed is the trace spd (the game's physics speed, 3D
magnitude; where a row has no spd, the x/z position difference over igt is used and counted). Tolerances are relative to the 60 Hz run.

  tools\\.venv\\Scripts\\python tools\\test\\hifps_probe.py [--rates 60,120,144,240] [--scenarios walk,jump,turn] [--exe DIR] [--max-seconds 1500] [--no-run]

Output: work/hifps_probe/all/ (script.nxs, mpr.log, out/trace.jsonl, out/result.json, cwd/) and one deviation table per scenario.
Exit code: 0 all checks pass, 1 a check fails or a run did not end with @pass, 3 a window has no player samples (gameplay not reached), 2 usage.
The run is retried once when it fails at the prologue's mash step (exit 1, no player samples).
"""
import argparse, json, math, os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gameplay as gp  # noqa: E402

OUT = os.path.join(ROOT, 'work', 'hifps_probe')
RUN_DIR = os.path.join(OUT, 'all')
DEFAULT_EXE = os.environ.get('NXRT_VERIFY_EXE') or os.path.join(ROOT, 'game-build-verify', 'build', 'snap4')

RATES = (60, 120, 144, 240)
SCENARIOS = ('walk', 'jump', 'turn')
PROLOGUE_TIMEOUT_MS = 600000
MAX_SECONDS = 1500

WALK_HOLD_S = 3.0
WALK_TAIL_S = 1.5
JUMP_BUTTON = 'B'
JUMP_PRESS_S = 0.4
JUMP_RUN_S = 2.0
TURN_COUNTS = 1000
TURN_SPREAD_S = 0.2
TURN_RUN_S = 0.5
RAD_PER_COUNT = 0.000767945          # mouse right lowers yaw
TURN_EXPECT = -TURN_COUNTS * RAD_PER_COUNT

TOL = {'walk': {'steady': 0.01, 'dist': 0.02, 't95': 0.06, 'slide': 0.06},
       'jump': {'peak': 0.02, 'air': 0.03},
       'turn': {'yaw': 0.01}}
EXPECT = {'turn': {'yaw': TURN_EXPECT}}


def frames(sec, rate):
    return max(1, int(round(sec * rate)))


# ---- script ---------------------------------------------------------------------------------------------------------------------------------
def actions(scen, rate):
    if scen == 'walk':
        return ['stick left 0 1.0', '@run %g' % WALK_HOLD_S, 'stick left 0 0', '@run %g' % WALK_TAIL_S]
    if scen == 'jump':
        return ['press %s for %d' % (JUMP_BUTTON, frames(JUMP_PRESS_S, rate)), '@run %g' % JUMP_RUN_S]
    if scen == 'turn':
        n = frames(TURN_SPREAD_S, rate)
        return ['mouse %.6f 0 for %d' % (TURN_COUNTS / float(n), n), '@run %g' % TURN_RUN_S]
    raise ValueError(scen)


def script_all(rates=RATES, scenarios=SCENARIOS):
    lines = ['# S11 physics probe: one process, @rate per block'] + gp.prologue(timeout_ms=PROLOGUE_TIMEOUT_MS)
    for rate in rates:
        for scen in scenarios:
            lines += gp.warp('tallon', 'tallon_canyon_cavern')
            lines += ['@rate %d' % rate, '@hash %s_%d_start' % (scen, rate)]
            lines += actions(scen, rate)
            lines += ['@hash %s_%d_end' % (scen, rate), '@rate 0']
    lines.append('@pass')
    return lines + ['']


# ---- trace and windows ----------------------------------------------------------------------------------------------------------------------
def load_trace(d):
    """All trace lines (player samples and '@hash' label lines)."""
    path = os.path.join(d, 'out', 'trace.jsonl')
    rows = []
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line.endswith('}'):
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
    return rows


def checkpoints(d, rows):
    """label -> frame from result.json; the '@hash' lines of the trace fill in anything missing."""
    cps = {}
    rp = os.path.join(d, 'out', 'result.json')
    if os.path.exists(rp):
        try:
            for c in json.load(open(rp)).get('checkpoints', []):
                cps[c['label']] = c['frame']
        except ValueError:
            pass
    for r in rows:
        if 'label' in r:
            cps.setdefault(r['label'], r['f'])
    return cps


def window(rows, cps, scen, rate):
    """player samples from the start checkpoint frame to the end checkpoint frame (inclusive)."""
    fs = cps.get('%s_%d_start' % (scen, rate))
    fe = cps.get('%s_%d_end' % (scen, rate))
    if fs is None or fe is None:
        return []
    return [r for r in rows if 'pos' in r and 'igt' in r and fs <= r['f'] <= fe]


# ---- metrics (game time = igt) --------------------------------------------------------------------------------------------------------------
def _split(series, ts):
    """trapezoid integral of (igt, value) pairs, split at igt = ts (value at ts interpolated). Returns (before, after)."""
    before = after = 0.0
    for (t0, s0), (t1, s1) in zip(series, series[1:]):
        if t1 <= ts:
            before += 0.5 * (s0 + s1) * (t1 - t0)
        elif t0 >= ts:
            after += 0.5 * (s0 + s1) * (t1 - t0)
        else:
            sm = s0 + (s1 - s0) * (ts - t0) / (t1 - t0)
            before += 0.5 * (s0 + sm) * (ts - t0)
            after += 0.5 * (sm + s1) * (t1 - ts)
    return before, after


def _speeds(pl):
    """(igt, speed) per sample from spd; samples without spd use the x/z position difference over igt. Returns (series, fallback count)."""
    out, fb = [], 0
    for i, r in enumerate(pl):
        if 'spd' in r:
            out.append((r['igt'], r['spd']))
        elif i > 0 and r['igt'] > pl[i - 1]['igt']:
            p0 = pl[i - 1]
            dt = r['igt'] - p0['igt']
            out.append((r['igt'], math.hypot(r['pos'][0] - p0['pos'][0], r['pos'][2] - p0['pos'][2]) / dt))
            fb += 1
    return out, fb


def walk_metrics(pl, rate):
    if len(pl) < 10:
        return None
    t0 = pl[0]['igt']
    end = t0 + WALK_HOLD_S                       # stick released here
    series, fb = _speeds(pl)
    hold = [(t, s) for t, s in series if t <= end + 1e-9]
    steady_pts = [s for t, s in hold if t >= end - WALK_TAIL_S - 1e-9]
    if len(hold) < 10 or not steady_pts:
        return None
    steady = sum(steady_pts) / len(steady_pts)
    t95 = next((t - t0 for t, s in hold if s >= 0.95 * steady), None)
    dist, slide = _split(series, end)
    return dict(steady=steady, dist=dist, t95=t95, slide=slide, fallback=fb)


def jump_metrics(pl, rate):
    if len(pl) < 10:
        return None
    y0 = pl[0]['pos'][1]
    peak = max(r['pos'][1] for r in pl) - y0
    air = sum(b['igt'] - a['igt'] for a, b in zip(pl, pl[1:]) if a.get('state') in (1, 2, 3))
    return dict(peak=peak, air=air)


def turn_metrics(pl, rate):
    if len(pl) < 10:
        return None
    d = pl[-1]['yaw'] - pl[0]['yaw']
    while d > math.pi:
        d -= 2 * math.pi
    while d < -math.pi:
        d += 2 * math.pi
    return dict(yaw=d)


FUNCS = {'walk': walk_metrics, 'jump': jump_metrics, 'turn': turn_metrics}


# ---- checks ---------------------------------------------------------------------------------------------------------------------------------
def print_table(scen, res, rates):
    """res: rate -> metric dict or None. Prints the table, returns True when every cell is within tolerance of the 60 Hz run."""
    keys = list(TOL[scen])
    base = 60 if 60 in res else min(res)
    ok = True
    print('== %s  (deviation from %d Hz; tolerance %s; ! = over tolerance)' % (scen, base, ', '.join('%s %.0f%%' % (k, 100 * TOL[scen][k]) for k in keys)))
    print('%-6s %s' % ('Hz', ' '.join('%10s' % k for k in keys)))
    for r in rates:
        cells = []
        for k in keys:
            v = (res.get(r) or {}).get(k)
            b = (res.get(base) or {}).get(k)
            if v is None or b is None:
                bad, cell = True, 'n/a!'
            else:
                dev = abs(v - b) / abs(b) if b else (0 if v == b else 1)
                bad = dev > TOL[scen][k]
                cell = '%8.4f%s' % (v, '!' if bad else ' ')
            ok = ok and not bad
            cells.append(cell)
        print('%-6d %s' % (r, ' '.join('%10s' % c for c in cells)))
    for k, ref in EXPECT.get(scen, {}).items():
        for r in rates:
            v = (res.get(r) or {}).get(k)
            if v is not None:
                print('   expected %s %.6f: %d Hz %.6f (%+.2f%%)' % (k, ref, r, v, 100.0 * (v - ref) / abs(ref)))
    for r in rates:
        fb = (res.get(r) or {}).get('fallback')
        if fb:
            print('   note: %d Hz used the x/z position difference on %d samples (no spd on those rows)' % (r, fb))
    return ok


def analyse(d, rates, scenarios):
    """Returns ({scen: {rate: metrics or None}}, {scen: {rate: number of window samples}})."""
    rows = load_trace(d)
    cps = checkpoints(d, rows)
    res, counts = {}, {}
    for scen in scenarios:
        res[scen], counts[scen] = {}, {}
        for rate in rates:
            pl = window(rows, cps, scen, rate)
            counts[scen][rate] = len(pl)
            res[scen][rate] = FUNCS[scen](pl, rate) if pl else None
    return res, counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rates', default=','.join(str(r) for r in RATES))
    ap.add_argument('--scenarios', default=','.join(SCENARIOS))
    ap.add_argument('--exe', default=DEFAULT_EXE, help='directory holding mpr.exe')
    ap.add_argument('--max-seconds', type=int, default=MAX_SECONDS)
    ap.add_argument('--no-run', action='store_true', help='only analyse the run already in work/hifps_probe/all')
    a = ap.parse_args()
    rates = [int(x) for x in a.rates.split(',')]
    scenarios = a.scenarios.split(',')
    for s in scenarios:
        if s not in FUNCS:
            print('unknown scenario', s)
            return 2
    status = 0
    if not a.no_run:
        lines = script_all(rates, scenarios)
        res = gp.run(lines, RUN_DIR, rate=None, exe_dir=a.exe, seconds=a.max_seconds)
        has_player = any('igt' in r for r in load_trace(RUN_DIR))
        if res['exit'] == 1 and not has_player and 'timeout waiting for area' in res.get('message', ''):
            print('run failed at the prologue (no player samples); retrying once')
            res = gp.run(lines, RUN_DIR, rate=None, exe_dir=a.exe, seconds=a.max_seconds)
        print('run: exit %s, verdict %s, wall %.0f s, message: %s' % (res['exit'], res.get('verdict', '-'), res['wall'], res.get('message', '-')))
        if res['exit'] != 0:
            status = max(status, 1)
    results, counts = analyse(RUN_DIR, rates, scenarios)
    for scen in scenarios:
        empty = [r for r in rates if counts[scen][r] == 0]
        if len(empty) == len(rates):
            print('== %s: no player samples in any window (gameplay not reached)' % scen)
            status = max(status, 3)
            continue
        if empty:
            print('== %s: no player samples at %s' % (scen, empty))
            status = max(status, 3)
        if not print_table(scen, results[scen], rates):
            status = max(status, 1)
    return status


if __name__ == '__main__':
    sys.exit(main())
