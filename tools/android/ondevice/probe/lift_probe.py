# SPDX-License-Identifier: GPL-3.0-or-later
"""Measurement helper (not shipped): lifts modules with pkp_build.py's own lift(), so module table, args and guest base
match the real build. [NOVERIFY=1] python -I lift_probe.py <kit/tools/build> <exefs> <out> <jobs> [module ...]
Prints per-module wall time and the peak RSS of the lifter processes (RUSAGE_CHILDREN, the largest single process)."""
import json, os, resource, sys, time
kit, exefs, out, jobs = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
sys.path.insert(0, kit)
import pkp_build as pb
want = sys.argv[5:] or [m[0] for m in pb.MODULES]
print('guest base', hex(pb.GUEST_BASE or 0), 'modules', [m[:1] + m[2:] for m in pb.MODULES], flush=True)
if not os.environ.get('NOVERIFY'):     # NOVERIFY=1: an exefs holding only the modules to lift
    pb.verify(exefs)
runner = pb.Runner(os.path.join(out, 'cancel'))
os.makedirs(out, exist_ok=True)
res = {}
for name in want:
    skip = {m[0] for m in pb.MODULES if m[0] != name}
    t = time.time()
    pb.lift(runner, exefs, out, jobs, {}, skip)
    res[name] = round(time.time() - t, 1)
    print('@@lifted', name, res[name], 's, child maxrss so far', resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss, 'KiB', flush=True)
print(json.dumps(res))
