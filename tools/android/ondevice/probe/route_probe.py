# SPDX-License-Identifier: GPL-3.0-or-later
"""Measurement helper (not shipped): pkp_build.py's own --target android-arm64 route with the PHONE's tools (use_target with
cxx/ar/sysroot, compile_all with the object cache, archive, link), on a work folder that already holds lifted units, so the
route can be proven without the 2.6 GiB of a full build.
    python -I route_probe.py <kit/tools/build> <toolchain dir> <work> <jobs> <mem_per_job_mb>
<work>/lift/<module>/ : lifted units to compile (any subset), <work>/obj/<module>/ : objects already compiled elsewhere
(the PC's, for the units not lifted here). Run twice with obj/ of the lifted units removed in between: the second run must
take every unit from the object cache (warm-cache rebuild). Needs CCC_OVERRIDE_OPTIONS as the driver sets it."""
import json, os, sys, time
kit, tc, work, jobs, mem = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
sys.path.insert(0, kit)
import pkp_build as pb
b = os.path.join(tc, 'bin')
pb.use_target('android-arm64', cxx=os.path.join(b, 'clang++'), ar=os.path.join(b, 'llvm-ar'), sysroot=os.path.join(tc, 'sysroot'))
print('sys.platform', sys.platform, 'COMPILER', pb.COMPILER, flush=True)
runner = pb.Runner(os.path.join(work, 'cancel'))
env = dict(os.environ)
gen, obj, lib = (os.path.join(work, d) for d in ('lift', 'obj', 'lib'))
lifted = [m[0] for m in pb.MODULES if os.path.isdir(os.path.join(gen, m[0]))]
skip = {m[0] for m in pb.MODULES} - set(lifted)
compiler = pb.compiler_id()
cache = pb.make_cache(os.path.join(work, 'objcache'), 3072, compiler, env)
t = time.time()
pb.compile_all(runner, gen, obj, jobs, env, cache, skip, mem << 20)
tc_s = time.time() - t
hits = len(os.listdir(os.path.join(work, 'objcache')))
t = time.time()
record = os.path.join(work, 'mod-hooks-built.tsv')
open(record, 'w').close()
pb.archive(runner, obj, lib, {'probe': 1}, env, record, {})
ta = time.time() - t
t = time.time()


os.makedirs(os.path.join(work, 'build'), exist_ok=True)   # link() leaves the mod-hook record in the layout's build/
exe, written = pb.link(runner, lib, pb.Layout(work), env)
tl = time.time() - t
print(json.dumps({'compiled_modules': lifted, 'compile_s': round(tc_s, 1), 'objcache_entries': hits,
                  'archive_s': round(ta, 1), 'link_s': round(tl, 1), 'library': exe, 'bytes': os.path.getsize(exe)}), flush=True)
