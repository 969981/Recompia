"""Mouse-aim linearity test (one scripted real-clock run of mpr.exe, game step set per rate with @rate).

One mpr.exe process: prologue, one warp to Tallon Canyon Cavern, then for each rate R in 60,144,240 the step is set to
exactly 1/R game seconds (`@rate R`) and each burst is delivered as raw mouse counts, one line per frame, bracketed by an
@hash before and after. The yaw (and pitch) before and after are read from the trace at those two frames. The measured
yaw change is wrapped to (-pi, pi] and compared with -counts * yaw_rad (yaw_rad from <run>/cwd/controls.ini [mouse], else
0.000767945). Vertical bursts (dy only) are judged on pitch: expected pitch change = -dy * pitch_rad (dy < 0 is up, + pitch
is up); their yaw is expected to stay put.

  cd <project folder>
  tools\\.venv\\Scripts\\python tools\\test\\mouse_linearity.py

Output: work/verify/mouse/run/ (the run) and work/verify/mouse/table.txt.
Exit code: 0 all bursts within 1 %, 1 a burst flagged, 2 the run did not pass (after one rerun).
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gameplay as gp  # noqa: E402

RATES = (60, 144, 240)
DEFAULT_YAW_RAD = 0.000767945
DEFAULT_PITCH_RAD = 0.000767945
FLAG_TOL = 0.01          # flag a judged value when |ratio - 1| > 1 %
EXE_DIR = gp.ROOT + '/game-build-verify/build/snap4'
OUT_DIR = os.path.join(gp.ROOT, 'work', 'verify', 'mouse', 'run')
TABLE = os.path.join(gp.ROOT, 'work', 'verify', 'mouse', 'table.txt')
WALL_S = 1500
WARP = ('tallon', 'tallon_canyon_cavern')


def nframes(seconds, rate):
    return max(1, int(round(seconds * rate)))


def spread(total, n):
    """Split a signed count total into n integer per-frame counts that sum exactly to total."""
    sign = -1 if total < 0 else 1
    base, rem = divmod(abs(total), n)
    return [sign * (base + (1 if i < rem else 0)) for i in range(n)]


def bursts(rate):
    """name, axis ('x' = dx, 'y' = dy), segments [(total counts, frames)], gap between segments in game seconds."""
    n02, n05, n10 = nframes(0.2, rate), nframes(0.5, rate), nframes(1.0, rate)
    return [
        dict(name='a_right_1000_0.2s', axis='x', segs=[(1000, n02)], gap=0.0),
        dict(name='b_left_1000_0.2s', axis='x', segs=[(-1000, n02)], gap=0.0),
        dict(name='c_right_100_1frame', axis='x', segs=[(100, 1)], gap=0.0),
        dict(name='d_right_100_1.0s', axis='x', segs=[(100, n10)], gap=0.0),
        dict(name='e_right_4000_0.5s', axis='x', segs=[(4000, n05)], gap=0.0),
        dict(name='f_split_2x500_0.1s', axis='x', segs=[(500, 1), (500, 1)], gap=0.1),
        dict(name='f_once_1000_1frame', axis='x', segs=[(1000, 1)], gap=0.0),
        dict(name='g_up_dy-500_0.2s', axis='y', segs=[(-500, n02)], gap=0.0),
        dict(name='g_down_dy+500_0.2s', axis='y', segs=[(500, n02)], gap=0.0),
    ]


def mouse_lines(vals, axis):
    return ['mouse %d %d for 1' % ((v, 0) if axis == 'x' else (0, v)) for v in vals]


def label(phase, rate, name):
    return '%s_%d_%s' % (phase, rate, name)


def script():
    lines = gp.prologue(timeout_ms=600000) + gp.warp(*WARP) + ['@run 2']
    for rate in RATES:
        lines.append('@rate %d' % rate)
        for b in bursts(rate):
            lines += ['@hash %s' % label('pre', rate, b['name']), '@log BURST %d fps %s' % (rate, b['name'])]
            for i, (total, frames) in enumerate(b['segs']):
                if i:
                    lines.append('@run %g' % b['gap'])
                lines += mouse_lines(spread(total, frames), b['axis'])
            lines += ['@run 0.7', '@hash %s' % label('post', rate, b['name'])]
        lines += ['@rate 0', '@run 1']
    return lines


def read_mouse_ini(cwd):
    """(yaw_rad, pitch_rad, source note) from [mouse] in <cwd>/controls.ini, defaults where missing."""
    vals = {}
    path = os.path.join(cwd, 'controls.ini')
    if os.path.exists(path):
        section = None
        for raw in open(path, encoding='utf-8', errors='replace'):
            line = raw.strip()
            if line.startswith('['):
                section = line.strip('[]').strip().lower()
            elif section == 'mouse' and '=' in line and not line.startswith('#'):
                k, v = [s.strip() for s in line.split('=', 1)]
                vals[k.lower()] = float(v)
        note = 'controls.ini'
    else:
        note = 'defaults (no controls.ini)'
    return vals.get('yaw', DEFAULT_YAW_RAD), vals.get('pitch', DEFAULT_PITCH_RAD), note


def read_trace(out):
    labels, rows = {}, {}
    with open(os.path.join(out, 'out', 'trace.jsonl')) as fh:
        for line in fh:
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if 'label' in d:
                labels.setdefault(d['label'], d['f'])
            elif 'f' in d and 'yaw' in d:
                rows[d['f']] = d
    return labels, rows


def row_at(rows, f):
    """Trace row at frame f, else the nearest one. Returns (row, exact)."""
    if f in rows:
        return rows[f], True
    return rows[min(rows, key=lambda k: abs(k - f))], False


def wrap(a):
    """Wrap an angle to (-pi, pi]."""
    w = (a + math.pi) % (2 * math.pi) - math.pi
    return w + 2 * math.pi if w <= -math.pi else w


def analyse(out):
    yaw_rad, pitch_rad, src = read_mouse_ini(os.path.join(out, 'cwd'))
    labels, rows = read_trace(out)
    results = []
    for rate in RATES:
        for b in bursts(rate):
            fp, fq = labels.get(label('pre', rate, b['name'])), labels.get(label('post', rate, b['name']))
            base = dict(rate=rate, name=b['name'], axis=b['axis'])
            if fp is None or fq is None or not rows:
                results.append(dict(base, error='missing hash label or trace rows'))
                continue
            p0, e0 = row_at(rows, fp)
            p1, e1 = row_at(rows, fq)
            total = sum(t for t, _ in b['segs'])
            dyaw = wrap(p1['yaw'] - p0['yaw'])
            dpitch = p1['pitch'] - p0['pitch'] if ('pitch' in p0 and 'pitch' in p1) else None
            if b['axis'] == 'x':
                exp_yaw = -total * yaw_rad
                ratio_yaw = dyaw / exp_yaw
                exp_pitch, ratio_pitch = None, None
                flag = abs(ratio_yaw - 1) > FLAG_TOL
            else:
                exp_yaw, ratio_yaw = 0.0, None
                exp_pitch = -total * pitch_rad
                ratio_pitch = dpitch / exp_pitch if (dpitch is not None and exp_pitch) else None
                flag = ratio_pitch is not None and abs(ratio_pitch - 1) > FLAG_TOL
            results.append(dict(base, counts=total, yaw=dyaw, exp_yaw=exp_yaw, ratio_yaw=ratio_yaw,
                                pitch=dpitch, exp_pitch=exp_pitch, ratio_pitch=ratio_pitch, flag=flag,
                                approx=not (e0 and e1)))
    return results, yaw_rad, pitch_rad, src


def fmt(v, spec='%+.6f', none='n/a'):
    return none if v is None else spec % v


def fmt_row(r):
    if 'error' in r:
        return '%-5s %-22s ERROR %s' % (r['rate'], r['name'], r['error'])
    counts = '%+d %s' % (r['counts'], 'dx' if r['axis'] == 'x' else 'dy')
    flag = 'FLAG' if r['flag'] else ''
    if r['axis'] == 'x':
        yaw_exp, yaw_ratio = fmt(r['exp_yaw']), fmt(r['ratio_yaw'], '%.4f')
        pitch_ratio = 'n/a'
    else:
        yaw_exp, yaw_ratio = '0 (stays)', 'n/a'
        pitch_ratio = fmt(r['ratio_pitch'], '%.4f')
    return '%-5d %-22s %-10s %-11s %-11s %-9s %-11s %-9s %s%s' % (
        r['rate'], r['name'], counts, fmt(r['yaw']), yaw_exp, yaw_ratio, fmt(r['pitch']), pitch_ratio, flag,
        '  (approx: frame not in trace, nearest used)' if r.get('approx') else '')


def main():
    header = '%-5s %-22s %-10s %-11s %-11s %-9s %-11s %-9s %s' % (
        'rate', 'burst', 'counts', 'yaw change', 'expected', 'ratio', 'pitch chg', 'ratio', 'flag')
    res = gp.run(script(), OUT_DIR, rate=None, exe_dir=EXE_DIR, seconds=WALL_S)
    note = 'exit %s' % res.get('exit')
    if res.get('exit') != 0:
        print('first run: exit %s (%s); rerunning once' % (res.get('exit'), res.get('message', '')), flush=True)
        res = gp.run(script(), OUT_DIR, rate=None, exe_dir=EXE_DIR, seconds=WALL_S)
        note = 'first attempt exit %s, rerun exit %s' % (note.split()[-1], res.get('exit'))
    if res.get('exit') != 0 or not os.path.exists(os.path.join(OUT_DIR, 'out', 'trace.jsonl')):
        text = '%s\nRUN FAILED: %s; message: %s\n' % (header, note, res.get('message', ''))
        open(TABLE, 'w', encoding='utf-8').write(text)
        sys.stdout.write(text)
        return 2

    results, yaw_rad, pitch_rad, src = analyse(OUT_DIR)
    lines = [header, '# yaw_rad %.9f, pitch_rad %.9f (%s); run dir %s; %s' % (yaw_rad, pitch_rad, src, OUT_DIR, note)]
    summary = []
    for rate in RATES:
        rr = [r for r in results if r['rate'] == rate]
        lines += [fmt_row(r) for r in rr]
        judged = [r for r in rr if 'error' not in r and r['axis'] == 'x']
        dy_judged = [r for r in rr if 'error' not in r and r['axis'] == 'y' and r['ratio_pitch'] is not None]
        lines.append('')
        if judged:
            worst = max(judged, key=lambda r: abs(r['ratio_yaw'] - 1))
            sc = sum(r['counts'] for r in judged)
            sm = sum(r['yaw'] for r in judged)
            se = sum(r['exp_yaw'] for r in judged)
            summary.append('%d fps: max |yaw ratio-1| = %.4f (%s); total yaw of %d dx bursts, counts %+d: measured %+.6f, expected %+.6f, ratio %.4f'
                           % (rate, abs(worst['ratio_yaw'] - 1), worst['name'], len(judged), sc, sm, se, sm / se))
        if dy_judged:
            worst = max(dy_judged, key=lambda r: abs(r['ratio_pitch'] - 1))
            summary.append('%d fps: max |pitch ratio-1| = %.4f (%s)' % (rate, abs(worst['ratio_pitch'] - 1), worst['name']))
    flagged = [r for r in results if r.get('flag')]
    if flagged:
        summary.append('flagged: ' + ', '.join('%d fps %s' % (r['rate'], r['name']) for r in flagged))
    text = '\n'.join(lines + summary) + '\n'
    open(TABLE, 'w', encoding='utf-8').write(text)
    sys.stdout.write(text)
    return 1 if flagged else 0


if __name__ == '__main__':
    sys.exit(main())
