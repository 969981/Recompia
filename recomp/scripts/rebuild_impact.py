# SPDX-License-Identifier: GPL-3.0-or-later
"""How much of the game a player compiles again when they update from one release to this tree.

    python recomp/scripts/rebuild_impact.py --exefs <dump>/exefs --zig <zig> [--base v0.12.0] [--max N] [--keep]

The player's Set up (game-build/release_b/pkp_build.py) keeps every compiled object under a key (ObjCache). An update compiles
a unit again exactly when the key this tree gives it is not one the base release made. So this lifts the game twice, with the
base release's lifter and with this tree's, keys every unit with each side's own pkp_build.py, and counts the keys the base
side does not have. A full compile is 15 minutes on a fast PC and an hour on a slow one: run this before merging a change to
recomp/ or to the compile flags, and ship a change that costs many units with others that do.

--base   the release players update from (default: the newest tag that is not a prerelease)
--zig    the Zig the release bundles (BUNDLED_TOOLS.txt), for both sides: its file hash is in the keys, so a Zig bump is all units
--max    exit 1 when more units than this compile again
--keep   keep the work folder (printed) with both lifts

Needs the lifter's Python packages (recomp/requirements.txt) and git. Nothing is compiled; a run is a few minutes.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD_SCRIPT = 'game-build/release_b/pkp_build.py'

# Runs in a staged copy of one side, in the package layout pkp_build.py expects (recomp/ next to it): lifts every module and
# prints {"module/unit.cpp": key}. Written against pkp_build.py's own functions, so each side keys with its own formula.
SIDE = r'''
import json, os, sys
here, exefs, zig, out, jobs = sys.argv[1:6]
sys.path.insert(0, here)
import pkp_build as b
b.ZIG = zig
b.COMPILER = [zig, 'c++']
os.makedirs(out, exist_ok=True)
runner = b.Runner(os.path.join(out, 'cancel'))
gen = os.path.join(out, 'lift')
b.lift(runner, exefs, gen, int(jobs), {})
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(out, 'zig-cache'), ZIG_LOCAL_CACHE_DIR=os.path.join(out, 'zig-cache'))
folder = os.path.join(out, 'objcache')
try:
    cache = b.make_cache(folder, 1, b.compiler_id(), env)
except TypeError:                       # a release before IncludeView
    cache = b.make_cache(folder, 1, b.compiler_id())
keys = {}
for m in b.MODULES:
    d = os.path.join(gen, m[0])
    for f in sorted(os.listdir(d)):
        if f.endswith('.cpp'):
            keys['%s/%s' % (m[0], f)] = cache.key(os.path.join(d, f))
with open(os.path.join(out, 'keys.json'), 'w') as f:
    json.dump(keys, f)
'''


def git(*args):
    return subprocess.run(['git', '-C', REPO] + list(args), check=True, capture_output=True, text=True).stdout.strip()


def newest_release():
    for tag in git('tag', '--sort=-creatordate', '--merged', 'HEAD').splitlines():
        if tag.startswith('v') and '-' not in tag:
            return tag
    sys.exit('no release tag reaches HEAD: name one with --base')


def stage(side, ref):
    """One side in the package layout: <side>/pkp_build.py and <side>/recomp. ref None = this working tree."""
    os.makedirs(side)
    if ref is None:
        shutil.copy2(os.path.join(REPO, *BUILD_SCRIPT.split('/')), side)
        shutil.copytree(os.path.join(REPO, 'recomp'), os.path.join(side, 'recomp'),
                        ignore=shutil.ignore_patterns('__pycache__', 'build', 'oracle'))
        return
    tar = os.path.join(side, 'src.tar')
    subprocess.run(['git', '-C', REPO, 'archive', '-o', tar, ref, 'recomp', BUILD_SCRIPT], check=True)
    with tarfile.open(tar) as t:
        t.extractall(os.path.join(side, 'src'), filter='data')
    os.remove(tar)
    shutil.move(os.path.join(side, 'src', *BUILD_SCRIPT.split('/')), side)
    shutil.move(os.path.join(side, 'src', 'recomp'), side)
    shutil.rmtree(os.path.join(side, 'src'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--exefs', required=True)
    ap.add_argument('--zig', required=True)
    ap.add_argument('--base')
    ap.add_argument('--max', type=int)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2 - 1))
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    base = a.base or newest_release()
    exefs, zig = os.path.abspath(a.exefs), os.path.abspath(a.zig)
    for p in (exefs, zig):
        if not os.path.exists(p):
            sys.exit('not found: %s' % p)
    work = tempfile.mkdtemp(prefix='rebuild-impact-')
    try:
        runs = {}
        for name, ref in (('base', base), ('head', None)):
            side = os.path.join(work, name)
            stage(side, ref)
            out = os.path.join(side, 'out')
            os.makedirs(out)
            log = open(os.path.join(side, 'log.txt'), 'w')
            runs[name] = (subprocess.Popen([sys.executable, '-I', '-c', SIDE, side, exefs, zig, out, str(a.jobs)],
                                           stdout=log, stderr=subprocess.STDOUT), side)
        print('lifting with %s and with this tree (%d jobs each) ...' % (base, a.jobs), flush=True)
        keys = {}
        for name, (proc, side) in runs.items():
            if proc.wait() != 0:
                with open(os.path.join(side, 'log.txt')) as f:
                    sys.exit('the %s side failed:\n%s' % (name, ''.join(f.readlines()[-25:])))
            with open(os.path.join(side, 'out', 'keys.json')) as f:
                keys[name] = json.load(f)
        have = set(keys['base'].values())
        again = sorted(u for u, k in keys['head'].items() if k not in have)
        total = len(keys['head'])
        for module in dict.fromkeys(u.split('/')[0] for u in keys['head']):
            units = [u for u in keys['head'] if u.startswith(module + '/')]
            n = sum(1 for u in units if u in again)
            print('  %-8s %3d of %3d units compile again' % (module, n, len(units)))
        print('%s -> this tree: %d of %d units compile again (%d%% of a full Set up compile)'
              % (base, len(again), total, round(100.0 * len(again) / max(1, total))))
        if again and len(again) <= 60:
            print('  ' + ' '.join(again))
        if a.max is not None and len(again) > a.max:
            print('more than --max %d' % a.max)
            return 1
        return 0
    finally:
        if a.keep:
            print('kept: %s' % work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
