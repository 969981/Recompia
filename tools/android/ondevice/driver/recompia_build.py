# SPDX-License-Identifier: GPL-3.0-or-later
"""On-device game build: the entry point the app's build service runs (interface: INTERFACE.md, kept by tools/android/ondevice).

    libpython_exec.so -I -u recompia_build.py --dump <dir> --work <dir> --out <libpkpgame.so> --jobs <n>
                      --native-lib-dir <dir> --toolchain-dir <dir>

Wraps the player build (kit/tools/build/pkp_build.py --target android-arm64) with the phone's own clang, lld and llvm-ar, and
turns its output into the app's progress protocol, one event per line on stdout:
    @@STEP <i>/<n> <name>   @@PROGRESS <done>/<total> <text>   @@LOG <text>   @@DONE <path>   @@ERROR <text>
Exit codes: 0 done, 1 error, 2 cancelled, 3 not enough free space.

The tools are ONE executable in the app's native library folder (libllvm_exec.so, LLVM's multicall driver: it picks clang, lld
or llvm-ar by the name it is started under). Android runs code only from that folder, which holds no symlinks, so the names are
symlinks in <toolchain-dir>/bin, made again at every start (the folder moves when the app is updated). clang resolves its own
path through /proc/self/exe, i.e. to the native library folder, so its resource folder and linker are given by
CCC_OVERRIDE_OPTIONS (clang's own environment hook; '#' keeps it quiet).

Resume: pkp_build keeps finished lift modules and objects in <work>/build; running again continues. Cancel: SIGTERM here writes
pkp_build's cancel file (it kills the lifter and the compilers and exits 2); a child still alive after 10 s is killed.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

STEPS = ['check', 'lift', 'compile', 'link', 'finish']
# Work space of a full build, measured: lift output 0.92 GiB + objects 0.30 + module archives 0.30 + library 0.25 + object cache
# 0.30, with margin. What a previous run left in <work> counts as already used.
NEED_WORK = 2600 << 20
NEED_OUT = 300 << 20
TOOLS = {'clang++': 'libllvm_exec.so', 'ld.lld': 'libllvm_exec.so', 'llvm-ar': 'libllvm_exec.so'}


def emit(*parts):
    print(' '.join(str(p) for p in parts).replace('\n', ' / '), flush=True)


def link(target, name):
    """name -> target as a symlink, replaced when it points elsewhere."""
    if os.path.islink(name) and os.readlink(name) == target:
        return
    if os.path.lexists(name):
        os.remove(name)
    os.symlink(target, name)


def tree_size(path):
    total = 0
    for d, _, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(d, f)).st_size
            except OSError:
                pass
    return total


def free(path):
    while not os.path.isdir(path):
        path = os.path.dirname(path)
    return shutil.disk_usage(path).free


def default_jobs():
    # measured on a 12 GB phone: beyond this the big cores throttle and memory runs short (pkp_build lowers it further by free memory)
    return 4


def main():
    ap = argparse.ArgumentParser()
    for opt in ('--dump', '--work', '--out', '--native-lib-dir', '--toolchain-dir'):
        ap.add_argument(opt, required=True)
    ap.add_argument('--jobs', type=int, default=0)
    a = ap.parse_args()
    tc, nl = os.path.abspath(a.toolchain_dir), os.path.abspath(a.native_lib_dir)
    work, out = os.path.abspath(a.work), os.path.abspath(a.out)
    exefs = os.path.join(os.path.abspath(a.dump), 'exefs')
    kit = os.path.join(tc, 'kit', 'tools', 'build')
    jobs = a.jobs if a.jobs > 0 else default_jobs()

    emit('@@STEP', '1/%d' % len(STEPS), STEPS[0])
    for p, what in ((exefs, 'the dump\'s exefs folder'), (os.path.join(kit, 'pkp_build.py'), 'the build kit'),
                    (os.path.join(nl, 'libllvm_exec.so'), 'the compiler'), (os.path.join(nl, 'libpkp_rt.so'), 'the runtime')):
        if not os.path.exists(p):
            emit('@@ERROR', 'Missing %s: %s' % (what, p))
            return 1
    os.makedirs(work, exist_ok=True)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    need = max(0, NEED_WORK - tree_size(os.path.join(work, 'build')))
    for d, n in ((work, need), (os.path.dirname(out), NEED_OUT)):
        if free(d) < n:
            emit('@@ERROR', 'not enough free space: need %d MiB in %s, have %d MiB' % (n >> 20, d, free(d) >> 20))
            return 3
    bindir = os.path.join(tc, 'bin')
    os.makedirs(bindir, exist_ok=True)
    for name, lib in TOOLS.items():
        link(os.path.join(nl, lib), os.path.join(bindir, name))
    link(os.path.join(nl, 'libpkp_rt.so'), os.path.join(kit, 'sdk', 'libpkp_rt.so'))

    env = dict(os.environ)
    env['CCC_OVERRIDE_OPTIONS'] = '# +-resource-dir=%s +--ld-path=%s' % (
        os.path.join(tc, 'lib', 'clang', '19'), os.path.join(bindir, 'ld.lld'))
    env.setdefault('TMPDIR', os.path.join(work, 'tmp'))
    os.makedirs(env['TMPDIR'], exist_ok=True)
    cmd = [sys.executable, '-I', os.path.join(kit, 'pkp_build.py'), '--target', 'android-arm64',
           '--cxx', os.path.join(bindir, 'clang++'), '--ar', os.path.join(bindir, 'llvm-ar'),
           '--sysroot', os.path.join(tc, 'sysroot'), '--exefs', exefs, '--game', work, '--jobs', str(jobs)]
    emit('@@LOG', 'Building with %d jobs' % jobs)
    # own process group: pkp_build then ends the whole group (lifter workers, compilers) when it gets SIGTERM
    child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                             cwd=kit, start_new_session=True)
    cancel_file = os.path.join(work, 'build', 'cancel')

    def on_term(*_):
        open(cancel_file, 'w').close()
        deadline = time.time() + 10
        while child.poll() is None and time.time() < deadline:
            time.sleep(0.2)
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
        sys.exit(2)

    signal.signal(signal.SIGTERM, on_term)
    step, error = 0, None
    for raw in child.stdout:
        line = raw.decode('utf-8', 'replace').rstrip('\r\n')
        if line.startswith('@progress '):
            parts = line.split(' ', 4) + ['']
            phase, done, total, text = parts[1], parts[2], parts[3], parts[4]
            if phase in STEPS and STEPS.index(phase) + 1 > step:
                step = STEPS.index(phase) + 1
                emit('@@STEP', '%d/%d' % (step, len(STEPS)), phase)
            emit('@@PROGRESS', '%s/%s' % (done, total), text)
        elif line.startswith('@error '):
            error = line[7:].replace('\\n', ' / ')
        elif line.startswith('@cancelled'):
            pass
        else:
            emit(line)
    rc = child.wait()
    if rc == 2:
        return 2
    if rc != 0:
        emit('@@ERROR', error or 'the build stopped (exit %d)' % rc)
        return 1
    built = os.path.join(work, 'libpkpgame.so')
    # moved, not kept: the next run relinks from the kept module archives (seconds) if it is asked for again
    try:
        os.replace(built, out)
    except OSError:                      # --out on another file system than --work
        tmp = out + '.tmp'
        shutil.copyfile(built, tmp)
        with open(tmp, 'rb') as f:
            os.fsync(f.fileno())
        os.replace(tmp, out)
        os.remove(built)
    emit('@@DONE', out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
