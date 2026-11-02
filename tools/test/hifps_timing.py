"""S11 timing oracle: does the game's clock follow wall-clock time at 60, 120 and 240 steps per second?

Runs the lifted game headless (the title screen and front end: movies, menu animation, render timings) once per case through
hifps_wt.sh-style (local test script) boot.py, with the [time] keys of the runtime's timestep module, and reads the per-frame CSV it writes
(frame, host_s, elapsed_s, step_s, sim_s, movie_frames). Per case it reports, over the steady part of the run:

  fps        frames per wall second
  sim/wall   seconds of game step handed to the game per wall second (retail: frames / 60 per wall second)
  render x   render-timing clock (texture scroll, UV animation) seconds per wall second; retail counts one per call: frames / 60
  movie x    video frames the movie player consumed per wall second, divided by 60 (the title movies are 60 fps): 1.00 = real time
  dt jitter  standard deviation of the step in ms

Cases: detretail (the same with the game's own 1/60 per frame: what the unpatched game does on that clock), det (deterministic virtual clock at N steps per second: the game's clock is exact whatever the machine can render; 'wall' is virtual
time), retail (mode=off, off_rate=N: the game's own 1/60 per call, held to N frames per second: what the unpatched game does),
real (mode=real, max_rate=N: measured frame time, held to N), fixed (mode=fixed, rate=N: the lock).

  tools\\.venv\\Scripts\\python tools\\test\\hifps_timing.py [--rates 60,120,240] [--modes retail,real,fixed] [--seconds 40] [--warmup 14] [--exe PATH]

Exit code 0 when every det case has movie x and sim/wall, and every real/fixed case has movie x, within --tol (default 0.06) of 1.0; retail rows are informational
(they are expected to show N/60). Output: a table on stdout and work/hifps_timing/<case>.csv + summary.json.
"""
import argparse, csv, json, math, os, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'work', 'hifps_timing')


def run_case(name, env_kv, seconds, exe_env):
    os.makedirs(OUT, exist_ok=True)
    csv_path = os.path.join(OUT, name + '.csv')
    if os.path.exists(csv_path):
        os.remove(csv_path)
    env = dict(os.environ, MSYS2_ARG_CONV_EXCL='*', **exe_env)
    cmd = [os.path.join(ROOT, 'tools', '.venv', 'Scripts', 'python.exe'), os.path.join(ROOT, 'tools', 'boot', 'boot.py'), '--no-build',
           '--seconds', str(seconds), '--tail', '0']
    for k, v in env_kv.items():
        cmd += ['--env', '%s=%s' % (k, v)]
    cmd += ['--env', 'NXRT_TIME_CSV=' + csv_path]
    t = time.time()
    subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
    return csv_path, time.time() - t


def analyse(csv_path, warmup, retail, det=False, warmup_virtual=6.0):
    rows = []
    with open(csv_path, newline='') as f:
        for r in csv.DictReader(f):
            if None in r.values() or '' in r.values():
                continue          # the run is ended by a deadline kill: the last line may be cut
            rows.append((int(r['frame']), float(r['host_s']), float(r['elapsed_s']), float(r['step_s']), float(r['sim_s']), int(r['movie_frames']), float(r.get('render_s') or 0)))
    rows = rows[:-1]          # the last line may be cut by the deadline kill
    if len(rows) < 100:
        return None
    t0 = rows[0][1] + (warmup if not det else warmup_virtual)
    sel = [r for r in rows if r[1] >= t0]
    if len(sel) < 50:
        return None
    wall = sel[-1][1] - sel[0][1]
    frames = sel[-1][0] - sel[0][0]
    sim = frames / 60.0 if retail else sel[-1][4] - sel[0][4]
    mov = (sel[-1][5] - sel[0][5]) / 60.0
    ren = frames / 60.0 if retail else sel[-1][6] - sel[0][6]      # retail: the render clock counts one frame per call
    if ren < 0:
        ren += 53000 / 60.0      # the render clock wrapped
    steps = [r[3] * 1e3 for r in sel[1:]]
    mean = sum(steps) / len(steps)
    sd = math.sqrt(sum((x - mean) ** 2 for x in steps) / len(steps))
    return dict(wall=wall, fps=frames / wall, sim_per_wall=sim / wall, movie_x=mov / wall, render_x=ren / wall, dt_mean_ms=mean, dt_sd_ms=sd, frames=frames)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rates', default='60,120,240')
    ap.add_argument('--modes', default='det,retail,real,fixed')
    ap.add_argument('--seconds', type=int, default=40)
    ap.add_argument('--warmup', type=float, default=14.0, help='seconds of the run skipped (boot, logo card)')
    ap.add_argument('--warmup-virtual', dest='warmup_virtual', type=float, default=6.0, help='det cases: virtual seconds skipped')
    ap.add_argument('--tol', type=float, default=0.06)
    ap.add_argument('--exe', default=os.path.join(ROOT, 'game-build', 'build', 'hifps'))
    ap.add_argument('--extra', action='append', default=[], help='extra K=V environment for every case')
    a = ap.parse_args()
    exe_env = {'MPR_BUILD_DIR': a.exe, 'MPR_RUNTIME': os.path.join(ROOT, 'runtime-wt', 'hifps')}
    extra = dict(kv.split('=', 1) for kv in a.extra)
    results, bad = {}, 0
    print('%-14s %7s %9s %9s %9s %8s %10s' % ('case', 'fps', 'sim/wall', 'movie x', 'render x', 'dt ms', 'dt sd ms'))
    for mode in a.modes.split(','):
        for rate in [int(x) for x in a.rates.split(',')]:
            name = '%s_%d' % (mode, rate)
            kv = dict(extra)
            if mode == 'retail':
                kv.update(NXRT_TIME_MODE='off', NXRT_TIME_OFF_RATE=rate)
            elif mode == 'real':
                kv.update(NXRT_TIME_MODE='real', NXRT_TIME_MAX_RATE=rate)
            elif mode == 'detretail':
                kv.update(NXRT_TIME_MODE='off', NXRT_TIME_OFF_RATE=1, NXRT_TEST_DETERMINISTIC=1, NXRT_TEST_CLOCK_STEP_TICKS=int(round(19200000 / rate)))
            elif mode == 'det':
                # deterministic test mode: the clock is virtual, one frame = 1/rate s; the run is as slow as the machine, the game's time is exact
                kv.update(NXRT_TIME_MODE='real', NXRT_TEST_DETERMINISTIC=1, NXRT_TEST_CLOCK_STEP_TICKS=int(round(19200000 / rate)))
            else:
                kv.update(NXRT_TIME_MODE='fixed', NXRT_TIME_RATE=rate)
            csv_path, took = run_case(name, kv, a.seconds, exe_env)
            if not os.path.exists(csv_path):
                print('%-14s no csv (the run failed to start)' % name)
                bad += 1
                continue
            r = analyse(csv_path, a.warmup, mode in ('retail', 'detretail'), mode in ('det', 'detretail'), a.warmup_virtual)
            if not r:
                print('%-14s too few frames' % name)
                bad += 1
                continue
            results[name] = r
            # det: the game's own clock must be exact (movie consumption and the step sum). real/fixed on a real clock: only the movie clock is
            # required to follow wall time; the step sum falls behind wall time by design whenever a frame is slower than 1/min_rate (the step is clamped).
            ok = mode in ('retail', 'detretail') or (abs(r['movie_x'] - 1.0) <= a.tol and (mode in ('real', 'fixed') or abs(r['sim_per_wall'] - 1.0) <= a.tol))
            if not ok:
                bad += 1
            print('%-14s %7.1f %9.3f %9.3f %9.3f %8.3f %10.3f  %s' % (name, r['fps'], r['sim_per_wall'], r['movie_x'], r['render_x'], r['dt_mean_ms'], r['dt_sd_ms'], 'PASS' if ok else 'FAIL'))
    with open(os.path.join(OUT, 'summary.json'), 'w') as f:
        json.dump(results, f, indent=1)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
