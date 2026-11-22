# SPDX-License-Identifier: GPL-3.0-or-later
"""Measurement helper (not shipped): compiles lifted units with the phone's clang the way pkp_build.py does and reports, per
unit, wall seconds and the compiler's peak RSS (os.wait4), the batch's wall time, and the hottest CPU thermal zone and the
big cores' clock sampled every 2 s.
    python -I compile_probe.py --cxx <clang++> --jobs N --out <dir> [--flag F ...] unit.cpp ...
The flags after --flag are added after compile_flags.NDK_ANDROID_ARM64-like defaults given by the caller."""
import argparse, glob, json, os, subprocess, threading, time

ap = argparse.ArgumentParser()
ap.add_argument('--cxx', required=True)
ap.add_argument('--jobs', type=int, default=1)
ap.add_argument('--out', required=True)
ap.add_argument('--flag', action='append', default=[])
ap.add_argument('units', nargs='+')
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
zones = [z for z in glob.glob('/sys/class/thermal/thermal_zone*') if open(z + '/type').read().startswith('cpu-')]
samples, stop = [], threading.Event()


def sample():
    while not stop.is_set():
        try:
            t = max(int(open(z + '/temp').read()) for z in zones) / 1000.0
            f = int(open('/sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq').read()) // 1000
            f2 = int(open('/sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq').read()) // 1000
            samples.append((round(time.time() - t0, 1), t, f, f2))
        except (OSError, ValueError):
            pass
        stop.wait(2)


def thermal_status():
    try:
        out = subprocess.run(['dumpsys', 'thermalservice'], capture_output=True, text=True, timeout=10).stdout
        return int(out.split('Thermal Status:')[1].split()[0])
    except (OSError, IndexError, ValueError, subprocess.SubprocessError):
        return None


status0 = thermal_status()
todo = sorted(a.units, key=lambda u: -os.path.getsize(u))
res, running = [], {}
t0 = time.time()
threading.Thread(target=sample, daemon=True).start()
while todo or running:
    while todo and len(running) < a.jobs:
        u = todo.pop(0)
        o = os.path.join(a.out, os.path.basename(u)[:-4] + '.o')
        cmd = [a.cxx] + a.flag + ['-I', os.path.dirname(u), '-c', u, '-o', o]
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL)
        running[p.pid] = (u, time.time())
    pid, status, ru = os.wait4(-1, 0)
    if pid in running:
        u, ts = running.pop(pid)
        res.append({'unit': os.path.basename(u), 'src_bytes': os.path.getsize(u), 's': round(time.time() - ts, 1),
                    'maxrss_mib': ru.ru_maxrss // 1024, 'cpu_s': round(ru.ru_utime + ru.ru_stime, 1), 'rc': status})
        print(json.dumps(res[-1]), flush=True)
stop.set()
wall = time.time() - t0
print(json.dumps({'jobs': a.jobs, 'thermal_status_start_end': [status0, thermal_status()], 'units': len(res), 'wall_s': round(wall, 1), 'sum_unit_s': round(sum(r['s'] for r in res), 1),
                  'max_temp_c': max((s[1] for s in samples), default=None),
                  'big_mhz_min_max': [min((s[2] for s in samples), default=0), max((s[2] for s in samples), default=0)],
                  'failed': [r['unit'] for r in res if r['rc']]}), flush=True)
print('samples', json.dumps(samples[::5]))
