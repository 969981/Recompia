"""Frame-time probe for mpr.exe: a scripted gameplay walk, with and without background CPU load.

Run from the project folder:

  tools\\.venv\\Scripts\\python.exe tools\\test\\stutter_probe.py [--variants SPEC] [--load N] [--repeat K] [--out DIR]
  tools\\.venv\\Scripts\\python.exe tools\\test\\stutter_probe.py --load-test SECONDS    (spinners only, for checking the load generator)

SPEC is "name:ENV=VAL,ENV=VAL;name2:..."; "name:" means no environment change.
Each variant runs at load 0 and at load N, K times each. The order is interleaved (repeat, then variant, then load) and one
mpr.exe runs at a time. The frame times are the "ms" values of the trace rows between the walk_begin and walk_end checkpoints.

Output in DIR: table.txt (also printed), raw.json (per-run stats and the ms samples, rewritten after every run),
runs\\<variant>_L<load>_r<rep>\\ (one game out dir per run: out\\trace.jsonl, out\\result.json, mpr.log).
Exit 5 (freeze) and 124 (wall timeout) are counted as failed runs, as is any run without a usable walk window.
"""
import argparse, ctypes, json, math, os, statistics, subprocess, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gameplay as gp  # noqa: E402

DEFAULT_VARIANTS = ('base:;normal:NXRT_SCHED_PRIORITY_CLASS=normal;'
                    'high:NXRT_SCHED_PRIORITY_CLASS=high;free:NXRT_SCHED_CONSOLE_CORES=0')
SPIN = 'while True: pass'
NORMAL_PRIORITY_CLASS = 0x00000020
CREATE_NO_WINDOW = 0x08000000
FAILED_EXITS = (5, 124)
STATS = ('p50', 'p95', 'p99', 'max', 'over50', 'over100')

# ---- Windows job object: the spinners are killed when this process exits, even if it crashes or is killed. ----
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.CreateJobObjectW.restype = wintypes.HANDLE
kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
kernel32.SetInformationJobObject.restype = wintypes.BOOL
kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
JobObjectExtendedLimitInformation = 9
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000


class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ('ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount',
                                                 'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]


class _BASIC_LIMIT(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD), ('SchedulingClass', wintypes.DWORD)]


class _EXTENDED_LIMIT(ctypes.Structure):
    _fields_ = [('BasicLimitInformation', _BASIC_LIMIT), ('IoInfo', _IO_COUNTERS), ('ProcessMemoryLimit', ctypes.c_size_t),
                ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                ('PeakJobMemoryUsed', ctypes.c_size_t)]


class LoadGen:
    """N busy-loop python processes at NORMAL priority. Use as a context manager: started on enter, killed on exit."""

    def __init__(self, n):
        self.n = n
        self.procs = []
        self.job = None

    def __enter__(self):
        if self.n > 0:
            try:
                self._start()
            except BaseException:
                self.stop()
                raise
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    def _start(self):
        job = kernel32.CreateJobObjectW(None, None)
        if not job:
            raise ctypes.WinError(ctypes.get_last_error())
        self.job = job
        info = _EXTENDED_LIMIT()
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel32.SetInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(info), ctypes.sizeof(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        for _ in range(self.n):
            p = subprocess.Popen([sys.executable, '-I', '-c', SPIN],
                                 creationflags=NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.procs.append(p)
            if not kernel32.AssignProcessToJobObject(job, int(p._handle)):
                raise ctypes.WinError(ctypes.get_last_error())

    def stop(self):
        for p in self.procs:
            try:
                p.kill()
            except OSError:
                pass
        for p in self.procs:
            try:
                p.wait(timeout=10)
            except Exception:
                pass
        self.procs = []
        if self.job:
            kernel32.CloseHandle(self.job)  # kill-on-close also reaps anything still left in the job
            self.job = None


# ---- statistics ----
def pct(sorted_vals, p):
    """Nearest-rank percentile of an already sorted list."""
    n = len(sorted_vals)
    i = min(n - 1, max(0, math.ceil(p / 100 * n) - 1))
    return sorted_vals[i]


def stats(ms):
    s = sorted(ms)
    return {'n': len(s), 'p50': pct(s, 50), 'p95': pct(s, 95), 'p99': pct(s, 99), 'max': s[-1],
            'over50': sum(1 for x in s if x > 50.0), 'over100': sum(1 for x in s if x > 100.0)}


# ---- scripted run ----
WALK = ['stick left 0 1.0', '@run 6', 'stick left 0 -1.0', '@run 6'] * 3  # 36 game seconds, back and forth


def script():
    return (gp.prologue() + gp.warp('tallon', 'tallon_canyon_cavern') + ['@hash walk_begin'] + WALK
            + ['@hash walk_end', '@pass'])


def checkpoint_frame(res, label):
    for c in res.get('checkpoints') or []:
        if isinstance(c, dict) and c.get('label') == label and isinstance(c.get('frame'), int):
            return c['frame']
    return None


def ms_between(out_dir, begin, end):
    """ms of every trace frame row with begin < f <= end (checkpoint lines and rows without ms are skipped)."""
    ms = []
    with open(os.path.join(out_dir, 'out', 'trace.jsonl'), encoding='utf-8', errors='replace') as fh:
        for line in fh:
            try:
                d = json.loads(line)
            except ValueError:
                continue  # torn last line
            f = d.get('f')
            if isinstance(f, int) and 'ms' in d and begin < f <= end:
                ms.append(float(d['ms']))
    return ms


def parse_variants(spec):
    out, names = [], set()
    for item in spec.split(';'):
        item = item.strip()
        if not item:
            continue
        name, _, rest = item.partition(':')
        name = name.strip()
        if not name or name in names:
            raise SystemExit(f'variant name missing or repeated in {item!r}')
        names.add(name)
        env = {}
        for kv in rest.split(','):
            kv = kv.strip()
            if not kv:
                continue
            k, eq, v = kv.partition('=')
            if not eq or not k.strip():
                raise SystemExit(f'bad ENV=VAL item {kv!r} in variant {name!r}')
            env[k.strip()] = v.strip()
        out.append((name, env))
    if not out:
        raise SystemExit('no variants given')
    return out


def run_one(name, env, load, rep, out_root):
    base = os.path.join(out_root, 'runs', f'{name}_L{load}_r{rep}')
    out = base if not os.path.exists(base) else f'{base}_{int(time.time())}'
    rec = {'variant': name, 'load': load, 'repeat': rep, 'env': env, 'out_dir': out, 'ok': False, 'reason': None}
    with LoadGen(load):
        res = gp.run(script(), out, env=env, seconds=600)
    rec['exit'] = res.get('exit')
    rec['wall'] = round(res.get('wall', 0.0), 1)
    if rec['exit'] in FAILED_EXITS:
        rec['reason'] = f"exit {rec['exit']}"
    elif rec['exit'] != 0:
        rec['reason'] = f"exit {rec['exit']}"
    else:
        b, e = checkpoint_frame(res, 'walk_begin'), checkpoint_frame(res, 'walk_end')
        if b is None or e is None:
            rec['reason'] = 'missing walk checkpoints'
        else:
            ms = ms_between(out, b, e)
            if not ms:
                rec['reason'] = 'no frame rows between checkpoints'
            else:
                rec.update(stats(ms))
                rec['ms'] = [round(x, 3) for x in ms]
                rec['ok'] = True
    return rec


def median_or_none(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def fmt(v):
    if v is None:
        return '-'
    return f'{v:.0f}' if float(v).is_integer() else f'{v:.1f}'


def make_table(variants, loads, runs):
    header = f"{'variant':<10} {'load':>4} " + ' '.join(f'{c:>8}' for c in STATS) + f" {'failed':>7}"
    lines = [header, '-' * len(header)]
    for name, _ in variants:
        failed = sum(1 for r in runs if r['variant'] == name and not r['ok'])
        for i, load in enumerate(loads):
            rs = [r for r in runs if r['variant'] == name and r['load'] == load and r['ok']]
            cells = [median_or_none([r[c] for r in rs]) for c in STATS]
            tail = f'{failed:>7}' if i == len(loads) - 1 else f"{'':>7}"
            lines.append(f'{name:<10} {load:>4} ' + ' '.join(f'{fmt(v):>8}' for v in cells) + ' ' + tail)
    lines.append('')
    lines.append('All values are in host ms per frame; median over the repeats that ran OK. failed = runs with exit 5/124 or no usable window, all loads.')
    return '\n'.join(lines)


def write_raw(out_root, cfg, runs):
    with open(os.path.join(out_root, 'raw.json'), 'w', encoding='utf-8') as fh:
        json.dump({'config': cfg, 'runs': runs}, fh, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--variants', default=DEFAULT_VARIANTS, help='"name:ENV=VAL,ENV=VAL;name2:..."')
    ap.add_argument('--load', type=int, default=24, help='number of spinner processes for the loaded runs (default 24)')
    ap.add_argument('--repeat', type=int, default=3, help='runs per variant per load (default 3)')
    ap.add_argument('--out', default=os.path.join(gp.ROOT, 'work', 'verify', 'stutter_probe'))
    ap.add_argument('--load-test', type=float, metavar='SECONDS', help='start --load spinners, hold them SECONDS, stop; no game run')
    a = ap.parse_args()

    # Stop a stray NXRT_SCHED_* in the shell from leaking into the baseline variant.
    for k in list(os.environ):
        if k.startswith('NXRT_SCHED_'):
            del os.environ[k]

    if a.load_test is not None:
        with LoadGen(a.load) as g:
            print('spinner pids:', ' '.join(str(p.pid) for p in g.procs), flush=True)
            time.sleep(a.load_test)
        print('spinners stopped', flush=True)
        return 0

    variants = parse_variants(a.variants)
    loads = sorted({0, a.load})
    out_root = os.path.abspath(a.out)
    os.makedirs(out_root, exist_ok=True)
    cfg = {'variants': [{'name': n, 'env': e} for n, e in variants], 'loads': loads, 'repeat': a.repeat,
           'walk': 'walk_begin..walk_end, 36 game s', 'started': time.strftime('%Y-%m-%d %H:%M:%S')}
    runs = []
    total = len(variants) * len(loads) * a.repeat
    for rep in range(1, a.repeat + 1):
        for name, env in variants:
            for load in loads:
                n = len(runs) + 1
                print(f'[{n}/{total}] {name} load={load} repeat={rep}', flush=True)
                rec = run_one(name, env, load, rep, out_root)
                runs.append(rec)
                write_raw(out_root, cfg, runs)
                print(f"    exit={rec['exit']} wall={rec['wall']}s " + (f"n={rec['n']} p50={rec['p50']} max={rec['max']}"
                                                                      if rec['ok'] else f"FAILED ({rec['reason']})"),
                      flush=True)
    table = make_table(variants, loads, runs)
    with open(os.path.join(out_root, 'table.txt'), 'w', encoding='utf-8') as fh:
        fh.write(table + '\n')
    print()
    print(table)
    return 0


if __name__ == '__main__':
    sys.exit(main())
