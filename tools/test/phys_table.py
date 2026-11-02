"""S22 physics table: walk, jump and morph-ball numbers at 60, 120, 144 and 240 steps per second, from the guest's own player state.

One mpr.exe process per rate runs the whole list (title -> New Game -> Frigate, then for each scenario a warp to tallon_canyon_cavern (the jumps: tallon_tallon_canyon),
`@rate R`, the actions, `@rate 0`). `@rate R` makes the game step exactly 1/R s for every frame while the clock stays real, so this isolates
the physics from frame-time jitter. `--real` runs the same scenarios without `@rate`, in a hidden window: `[time] mode = real` (the step is the measured frame time) with
the play-mode frame limiter at the rate (`fps_cap`). A frame the machine cannot deliver in time makes the step longer.

Scenarios (times in game seconds, `@run`; pad steps count frames, one frame = 1/R s under @rate):
  walk    full left stick forward 3 s, release, 1.5 s tail            steady speed (position difference), time to 95 %, slide after release, total distance
  half    the same with the stick at 0.5
  jump    B held for 0.4 s standing still, 2 s of observation          peak height, air time (takeoff to landing, interpolated on y)
  rjump   stick forward 1 s, then B held 0.4 s with the stick held     peak height, air time, horizontal distance in the air
  ball    MorphBall granted, Y pressed, 1.5 s settle, then like walk   steady speed (position difference), time to 95 %, slide after release, total distance
All time is igt. The player speed is the trace `spd` (the game's physics speed: CPhysicsActorMP1::GetVelocityWR, 3D magnitude);
distances come from the position.

  tools\\.venv\\Scripts\\python tools\\test\\phys_table.py --exe game-build-rel\\build\\rel [--rates 60,120,144,240] [--jobs 2]
        [--scenarios walk,half,jump,rjump,ball] [--out work\\run_rel\\phys] [--real] [--no-run] [--tag name]
Output: <out>/<tag>_r<rate>/ (script.nxs, mpr.log, out/trace.jsonl, out/result.json) and the table; <out>/<tag>_results.json.
Exit code: 0 all rows within 1 % of the 60 step/s column, 1 deviation or a run that did not pass, 3 a window without samples, 2 usage.
"""
import argparse, json, math, os, re, subprocess, sys, threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gameplay as gp  # noqa: E402

ROOT = gp.ROOT
RATES = (60, 120, 144, 240)
SCENARIOS = ('walk', 'half', 'jump', 'rjump', 'ball')
HOLD_S, TAIL_S, JUMP_S = 3.0, 1.5, 0.4
HOLD = {'walk': 3.0, 'half': 1.5, 'ball': 3.0}      # stick-down time per scenario (s): half speed and the ball run into the cavern wall on a longer hold
TAIL = {'walk': 1.5, 'half': 1.0, 'ball': 0.5}
TOL = 0.01
# where each scenario runs. The jumps need open sky: the cavern's ceiling is 3.5 units above the floor and clips every jump at exactly that height.
AREA = {}
DEFAULT_AREA = 'tallon_canyon_cavern'
SHADERS = ROOT + '/work/shader_cache/spirv'
BC7 = ROOT + '/work/bc7_cache_polish'


def frames(sec, rate):
    return max(1, int(round(sec * rate)))


# ---- script -----------------------------------------------------------------------------------------------------------------------------------
def pre(scen):
    """actions after the warp and before the start checkpoint"""
    if scen == 'ball':
        return ['@give MorphBall 1', 'press Y', '@run 1.5']
    return []


def actions(scen, rate, real=False):
    """rate = steps per second the pad steps are counted in (frames per second)"""
    if scen in ('walk', 'ball', 'half'):
        return ['stick left 0 %g' % (0.5 if scen == 'half' else 1.0), '@run %g' % HOLD[scen], 'stick left 0 0', '@run %g' % TAIL[scen]]
    press = ['hold B', '@run %g' % JUMP_S, 'release B'] if real else ['press B for %d' % frames(JUMP_S, rate)]   # real time: frames per second vary
    if scen == 'jump':
        return press + ['@run 2']
    if scen == 'rjump':
        return ['stick left 0 1.0', '@run 1'] + press + ['@run 1.5', 'stick left 0 0', '@run 0.5']
    raise ValueError(scen)


def script(rate, scenarios, real):
    lines = ['# S22 physics table, rate %s' % rate] + gp.prologue(timeout_ms=600000)
    tag = rate
    for scen in scenarios:
        lines += gp.warp('tallon', AREA.get(scen, DEFAULT_AREA))
        if not real:
            lines.append('@rate %d' % rate)
        lines += pre(scen)
        lines.append('@hash %s_%d_start' % (scen, tag))
        lines += actions(scen, rate, real)
        lines.append('@hash %s_%d_end' % (scen, tag))
        if not real:
            lines.append('@rate 0')
    lines.append('@pass')
    return lines + ['']


# ---- trace ------------------------------------------------------------------------------------------------------------------------------------
def load_trace(d):
    rows = []
    p = os.path.join(d, 'out', 'trace.jsonl')
    if os.path.exists(p):
        with open(p) as f:
            for line in f:
                line = line.strip()
                if line.endswith('}'):
                    try:
                        rows.append(json.loads(line))
                    except ValueError:
                        pass
    return rows


def checkpoints(d, rows):
    cps = {}
    rp = os.path.join(d, 'out', 'result.json')
    if os.path.exists(rp):
        try:
            with open(rp) as f:
                data = json.load(f)
            for c in data.get('checkpoints', []):
                cps[c['label']] = c['frame']
        except ValueError:
            pass
    for r in rows:
        if 'label' in r:
            cps.setdefault(r['label'], r['f'])
    return cps


def window(rows, cps, scen, rate):
    fs, fe = cps.get('%s_%d_start' % (scen, rate)), cps.get('%s_%d_end' % (scen, rate))
    if fs is None or fe is None:
        return []
    pl = [r for r in rows if 'pos' in r and 'igt' in r and fs <= r['f'] <= fe]
    out = []
    for r in pl:                                   # drop frames where the game clock did not move (loads)
        if not out or r['igt'] > out[-1]['igt']:
            out.append(r)
    return out


# ---- metrics ----------------------------------------------------------------------------------------------------------------------------------
def at_time(pl, t, f):
    """f(row) interpolated linearly at igt = t"""
    for a, b in zip(pl, pl[1:]):
        if a['igt'] <= t <= b['igt']:
            w = (t - a['igt']) / (b['igt'] - a['igt'])
            va, vb = f(a), f(b)
            return va + (vb - va) * w
    return f(pl[-1]) if t > pl[-1]['igt'] else f(pl[0])


def xz(r):
    return r['pos'][0], r['pos'][2]


def dist_xz(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def pos_speeds(pl):
    """(igt, horizontal speed) per sample from the position difference to the previous sample. The probe velocity (`spd`) is not used:
    it is read at the frame boundary, before the tick's damping, and differs from the distance moved by about one step of acceleration."""
    out = []
    for p, r in zip(pl, pl[1:]):
        out.append((r['igt'], dist_xz(xz(r), xz(p)) / (r['igt'] - p['igt'])))
    return out


def crossing(series, level, rising, start=0):
    """igt where the speed series first crosses `level` (rising or falling) at or after index `start`, linear between samples"""
    for i in range(max(1, start), len(series)):
        (t0, v0), (t1, v1) = series[i - 1], series[i]
        if (rising and v0 < level <= v1) or (not rising and v0 > level >= v1):
            return t0 + (t1 - t0) * (level - v0) / (v1 - v0)
    return None


def walk_metrics(pl, scen='walk'):
    """steady: mean speed of the last 1.5 s of the hold. rise: time the speed takes to go from 25 % to 75 % of steady after the stick goes down.
    fall: the same from 75 % to 25 % after the release. Both are read off interpolated crossings, so they do not depend on which frame the
    input landed on. total: distance covered in the whole window."""
    if len(pl) < 10:
        return None
    t0 = pl[0]['igt']
    end = t0 + HOLD[scen]
    sp = pos_speeds(pl)
    hold = [(t, v) for t, v in sp if t <= end + 1e-9]
    steady_pts = [v for t, v in hold if t >= end - 0.5 * HOLD[scen]]
    if not hold or not steady_pts:
        return None
    steady = sum(steady_pts) / len(steady_pts)
    r25, r75 = crossing(sp, 0.25 * steady, True), crossing(sp, 0.75 * steady, True)
    start = next((i for i, (t, v) in enumerate(sp) if t >= end), len(sp))
    f75, f25 = crossing(sp, 0.75 * steady, False, start), crossing(sp, 0.25 * steady, False, start)
    return dict(steady=steady, rise=(r75 - r25) if r25 is not None and r75 is not None else None,
                fall=(f25 - f75) if f25 is not None and f75 is not None else None, total=dist_xz(xz(pl[-1]), xz(pl[0])),
                d1=dist_xz((at_time(pl, t0 + 1.0, lambda r: r['pos'][0]), at_time(pl, t0 + 1.0, lambda r: r['pos'][2])), xz(pl[0])))


def jump_metrics(pl, horizontal=False):
    if len(pl) < 10:
        return None
    # ground level = the height before the jump (first sample after the settle); takeoff/landing = crossings of ground + 0.05
    y0 = pl[0]['pos'][1]
    thr = y0 + 0.05
    peak_i = max(range(len(pl)), key=lambda i: pl[i]['pos'][1])
    peak = pl[peak_i]['pos'][1] - y0
    up = next((i for i in range(1, len(pl)) if pl[i]['pos'][1] > thr), None)
    if up is None:
        return None
    down = next((i for i in range(peak_i, len(pl)) if pl[i]['pos'][1] <= thr), None)

    def cross(i, j):
        a, b = pl[i], pl[j]
        w = (thr - a['pos'][1]) / (b['pos'][1] - a['pos'][1]) if b['pos'][1] != a['pos'][1] else 0.0
        return a['igt'] + (b['igt'] - a['igt']) * w, [a['pos'][0] + (b['pos'][0] - a['pos'][0]) * w, a['pos'][2] + (b['pos'][2] - a['pos'][2]) * w]

    t_up, p_up = cross(up - 1, up)
    res = dict(peak=peak)
    if down is not None and down > 0:
        t_dn, p_dn = cross(down - 1, down)
        res['air'] = t_dn - t_up
        if horizontal:
            res['hdist'] = dist_xz(p_up, p_dn)
    else:
        res['air'] = None
    return res


def metrics(scen, pl):
    if not pl:
        return None
    if scen in ('walk', 'half', 'ball'):
        return walk_metrics(pl, scen)
    if scen == 'jump':
        return jump_metrics(pl)
    if scen == 'rjump':
        return jump_metrics(pl, horizontal=True)
    return None


INFO = ('rise', 'd1')     # depend on how many frames the input takes to reach the player (a frame count, so shorter at a high rate), not on the physics: shown, not judged
KEYS = {'walk': ('steady', 'rise', 'fall', 'd1', 'total'), 'half': ('steady', 'rise', 'fall', 'd1'), 'ball': ('steady', 'rise', 'd1'),
        'jump': ('peak', 'air'), 'rjump': ('peak', 'air', 'hdist')}


def analyse(outdir, rates, scenarios, tag):
    res, counts = {}, {}
    for rate in rates:
        d = os.path.join(outdir, '%s_r%d' % (tag, rate))
        rows = load_trace(d)
        cps = checkpoints(d, rows)
        for scen in scenarios:
            pl = window(rows, cps, scen, rate)
            counts[(scen, rate)] = len(pl)
            res[(scen, rate)] = metrics(scen, pl)
    return res, counts


def table(res, rates, scenarios):
    """prints one block per scenario; returns True when every cell is within TOL of the first (lowest) rate"""
    base = min(rates)
    ok = True
    for scen in scenarios:
        keys = KEYS[scen]
        print('== %s  (deviation from %d steps/s; ! = beyond %.0f %%, ~ = beyond it but not judged)' % (scen, base, 100 * TOL))
        print('%-6s ' % 'Hz' + ' '.join('%18s' % k for k in keys))
        for r in rates:
            cells = []
            for k in keys:
                v = (res.get((scen, r)) or {}).get(k)
                b = (res.get((scen, base)) or {}).get(k)
                if v is None or b is None:
                    cells.append('%18s' % 'n/a!')
                    ok = False
                    continue
                dev = (v - b) / b if b else 0.0
                bad = abs(dev) > TOL and r != base and k not in INFO
                ok = ok and not bad
                cells.append('%18s' % ('%.4f (%+.2f%%)%s' % (v, 100 * dev, '!' if bad else ('~' if abs(dev) > TOL and k in INFO and r != base else ''))))
            print('%-6d ' % r + ' '.join(cells))
    return ok


# ---- running ----------------------------------------------------------------------------------------------------------------------------------
def run_one(a, rate, outdir, results):
    d = os.path.join(outdir, '%s_r%d' % (a.tag, rate))
    env = {'NXRT_NVN_SHADER_CACHE': SHADERS, 'NXRT_NVN_BC7_CACHE': BC7, 'NXRT_AUDIO_SINK': 'null'}
    gfx = 'headless'
    if a.real:
        # a hidden window with the frame limiter of play mode at `rate` (controls.ini [display] fps_cap): the game step is the measured frame time.
        # The headless offscreen path is too slow on this PC (it runs at the 30 Hz step clamp), so it cannot show the real-time step.
        gfx = 'window'
        env.update(NXRT_TIME_MODE='real', NXRT_GFX_HIDDEN='1')
        cwd = os.path.join(d, 'cwd')
        os.makedirs(cwd, exist_ok=True)
        ini = open(os.path.join(ROOT, 'work', 'run', 'controls.ini')).read()
        for k, v in (('fps_cap', rate), ('vsync', 0), ('width', 1280), ('height', 720)):
            ini = re.sub(r'(?m)^%s\s*=.*$' % k, '%s = %s' % (k, v), ini)
        open(os.path.join(cwd, 'controls.ini'), 'w').write(ini)
    r = gp.run(script(rate, a.scenarios, a.real), d, rate=None, env=env, seconds=a.max_seconds, exe_dir=a.exe, gfx=gfx)
    print('rate %d: exit %s verdict %s wall %.0f s %s' % (rate, r['exit'], r.get('verdict', '-'), r['wall'], r.get('message', '')), flush=True)
    results[rate] = r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=ROOT + '/game-build-rel/build/rel', help='folder with mpr.exe')
    ap.add_argument('--rates', default=','.join(str(r) for r in RATES))
    ap.add_argument('--scenarios', default=','.join(SCENARIOS))
    ap.add_argument('--out', default=ROOT + '/work/run_rel/phys')
    ap.add_argument('--tag', default='a')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--max-seconds', type=int, default=1500)
    ap.add_argument('--real', action='store_true', help='no @rate: real-time step with the frame limiter at the rate')
    ap.add_argument('--no-run', action='store_true')
    a = ap.parse_args()
    a.scenarios = a.scenarios.split(',')
    for s in a.scenarios:
        if s not in KEYS:
            print('unknown scenario', s)
            return 2
    rates = [int(x) for x in a.rates.split(',')]
    os.makedirs(a.out, exist_ok=True)
    status = 0
    if not a.no_run:
        results = {}
        pending = list(rates)
        lock = threading.Lock()

        def worker():
            while True:
                with lock:
                    if not pending:
                        return
                    rate = pending.pop(0)
                run_one(a, rate, a.out, results)

        ts = [threading.Thread(target=worker) for _ in range(max(1, a.jobs))]
        [t.start() for t in ts]
        [t.join() for t in ts]
        if any(results.get(r, {}).get('exit') != 0 for r in rates):
            status = 1
    res, counts = analyse(a.out, rates, a.scenarios, a.tag)
    for scen in a.scenarios:
        miss = [r for r in rates if not counts[(scen, r)]]
        if miss:
            print('== %s: no samples at %s' % (scen, miss))
            status = max(status, 3)
    if not table(res, rates, a.scenarios):
        status = max(status, 1)
    json.dump({'%s@%d' % k: v for k, v in res.items()}, open(os.path.join(a.out, '%s_results.json' % a.tag), 'w'), indent=1)
    return status


if __name__ == '__main__':
    sys.exit(main())
