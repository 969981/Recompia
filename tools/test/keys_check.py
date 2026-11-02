"""S22: keyboard beam and visor keys in a real game run.

Warps to tallon_canyon_cavern, grants the other beams and visors, presses the keys one at a time (`key <name>`: the keyboard path of the
play layer, the same one a window key takes) and reads the game's own beam and visor numbers from the trace after each press.

  tools\\.venv\\Scripts\\python tools\\test\\keys_check.py [--exe DIR] [--out DIR] [--no-run]

Beam numbers in the trace are the game's (0 power, 1 ice, 2 wave, 3 plasma; the keys follow the bindings' order power, wave, ice, plasma); visors 0 combat, 1 x-ray, 2 scan, 3 thermal (the keys follow the bindings' order combat, scan, thermal, x-ray).
Exit 0 when every key leaves the expected beam/visor, 1 otherwise.
"""
import argparse, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gameplay as gp  # noqa: E402

ROOT = gp.ROOT
# (label, script lines, expected (beam, visor); None = do not check that field)
STEPS = [
    ('start', [], (0, 0)),
    ('key2', ['key 2'], (2, None)),
    ('key3', ['key 3'], (1, None)),
    ('key4', ['key 4'], (3, None)),
    ('key1', ['key 1'], (0, None)),
    ('wheel_next', ['key WheelDown'], (2, None)),
    ('wheel_next2', ['key WheelDown'], (1, None)),
    ('wheel_prev', ['key WheelUp'], (2, None)),
    ('key6', ['key 6'], (None, 2)),
    ('key7', ['key 7'], (None, 3)),
    ('key8', ['key 8'], (None, 1)),
    ('key5', ['key 5'], (None, 0)),
]


def script():
    lines = ['# S22 keyboard beam and visor keys'] + gp.prologue(timeout_ms=600000) + gp.warp('tallon', 'tallon_canyon_cavern')
    for item in ('WaveBeam', 'IceBeam', 'PlasmaBeam', 'ScanVisor', 'ThermalVisor', 'XRayVisor'):
        lines.append('@give %s 1' % item)
    lines.append('@run 1')
    for label, acts, _ in STEPS:
        lines += acts
        lines += ['@run 0.8', '@hash %s' % label]
    lines += ['@pass', '']
    return lines


def analyse(d):
    rows = []
    for line in open(os.path.join(d, 'out', 'trace.jsonl')):
        line = line.strip()
        if line.endswith('}'):
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass
    cps = {r['label']: r['f'] for r in rows if 'label' in r}
    ok = True
    print('%-12s %5s %6s   expected' % ('after', 'beam', 'visor'))
    for label, _, want in STEPS:
        f = cps.get(label)
        r = next((x for x in rows if x.get('f') == f and 'beam' in x), None)
        if r is None:
            print('%-12s no sample' % label)
            ok = False
            continue
        good = all(w is None or w == g for w, g in zip(want, (r['beam'], r['visor'])))
        ok = ok and good
        print('%-12s %5d %6d   %s%s' % (label, r['beam'], r['visor'], want, '' if good else '   <-- MISMATCH'))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=ROOT + '/game-build-rel/build/rel')
    ap.add_argument('--out', default=ROOT + '/work/run_rel/keys')
    ap.add_argument('--no-run', action='store_true')
    a = ap.parse_args()
    if not a.no_run:
        env = {'NXRT_NVN_SHADER_CACHE': ROOT + '/work/shader_cache/spirv', 'NXRT_NVN_BC7_CACHE': ROOT + '/work/bc7_cache_polish', 'NXRT_AUDIO_SINK': 'null'}
        r = gp.run(script(), a.out, env=env, seconds=900, exe_dir=a.exe)
        print('run: exit %s verdict %s wall %.0f s %s' % (r['exit'], r.get('verdict', '-'), r['wall'], r.get('message', '')))
        if r['exit'] != 0:
            return 1
    return 0 if analyse(a.out) else 1


if __name__ == '__main__':
    sys.exit(main())
