# SPDX-License-Identifier: GPL-3.0-or-later
"""The optimized build's profile (pkp_build.py --optimize): where it lives, when it is reused, how it is made.

Profile-guided optimisation of the lifted code, done on the player's own machine:
  1. the test version: the game built with -fprofile-generate (pkp_build.py builds it in <game>/build/pgo/gen, never over the
     installed game), with profile_writer.cpp linked in;
  2. the play-test session: that game plays session.nxs (the runtime's scripted runs, [test] script) on a COPY of the player's save
     (<game>/build/pgo/save); the writer saves the counts to <game>/build/pgo/raw/pkpgame.profraw. On a PC run_session() runs it;
     on Android the app runs it (pkp_build.py prepares the save copy, exits SESSION_EXIT and names the test version, the script
     and the folders: session_env()); the next build takes the counts when the session ran to its end (Store.session_done()).
     The script checks the game's state before its tour (in the world, a save from after the first house, outdoors); a run that
     fails one of them (off_track()) builds the normal game and tells the player why (wait_after_play_test());
  3. llvm-profdata merge --sparse into <game>/build/pgo/<key>.profdata, plus <key>.order.txt (the hot functions, for the link);
  4. the normal build at -O2 with the profile (pkp_build.py use_options()).

Reuse: the profile is made once per game dump and kept for every later build (updates of the launcher or the app) while the dump is
the same: <key> is a hash of the verified ExeFS (main build ID and every module's sha256). It is made again only when
  - the dump changes (another key: no profile),
  - llvm-profdata of this package can no longer read it (a toolchain change; usable() asks it),
  - a build reports too many profile mismatches (MISMATCH_SHARE of the profiled functions: lifter changes that alter control flow;
    LLVM matches each function by a hash of its control-flow graph, so other lifter changes keep the profile): drop() renames it.
The profile and the order file are made from the player's own game and stay in their build folder: never in git or a release.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SESSION = os.path.join(HERE, 'session.nxs')
RAW_NAME = 'pkpgame.profraw'
SESSION_EXIT = 5                  # pkp_build.py: the test version is built, the session has to run (Android: the app runs it)
MISMATCH_SHARE = 0.10             # more mismatched functions than this share of the profiled ones makes it again
SESSION_SECONDS = 1800            # a session that runs longer is stopped ([boot] max_seconds); the script itself takes about 10 min
ORDER_TOP = 8000                  # hot functions placed together by the link
# clang's IR profile warning (-Wbackend-plugin with -fprofile-use), one line per function whose control-flow hash differs (clang 22):
# "warning: c_105.cpp: function control flow change detected (hash mismatch) f_1 Hash = ... [-Wbackend-plugin]"
MISMATCH_RE = re.compile(r'function control flow change detected', re.I)
# What the player is told when the play-test did not pass (pkp_build.py no_house; the launcher and the app show their own words for
# it, from Store.status): the save is from before the first house (the script's progress check failed), or the run could not get
# where the tour starts (into the world, outdoors). The normal game is built either way.
MESSAGES = {
    'first_house': 'Optimized build: play until you have your first house, then set up again. The normal game is built for now.',
    'play_test': 'Optimized build: the play-test could not finish; the normal game is built. Save outdoors and try Set up again.',
}


def key(want):
    """The dump the profile belongs to: the verified ExeFS (pkp_build.verify())."""
    text = json.dumps({'main_build_id': want['main_build_id'], 'sha256': want['sha256']}, sort_keys=True)
    return hashlib.sha256(text.encode()).hexdigest()[:16]


class Store:
    """<game>/build/pgo: the kept profile of each dump and the scratch space of making one."""

    def __init__(self, game):
        self.root = os.path.join(game, 'build', 'pgo')
        self.gen = os.path.join(self.root, 'gen')        # the test version's build (pkp_build Layout work and dest)
        self.raw = os.path.join(self.root, 'raw')
        self.save = os.path.join(self.root, 'save')
        self.out = os.path.join(self.root, 'session')
        self.status = os.path.join(self.root, 'status.json')   # {"waiting": a MESSAGES key}: read by the launcher and the app

    def profile(self, k):
        return os.path.join(self.root, k + '.profdata')

    def order(self, k):
        return os.path.join(self.root, k + '.order.txt')

    def meta(self, k):
        return os.path.join(self.root, k + '.json')

    def raw_file(self):
        return os.path.join(self.raw, RAW_NAME)

    def session_done(self):
        """A play-test session ran to its end: the script's result says pass (exit code 0, runtime/src/testtools/tt_core.cpp
        result.json) and the counts are there. A session stopped half way (paused, closed, crashed) is not a profile to build with."""
        f = self.raw_file()
        try:
            with open(os.path.join(self.out, 'result.json'), encoding='utf-8') as r:
                passed = json.load(r).get('exit_code') == 0
        except (OSError, ValueError, AttributeError):
            passed = False
        return passed and os.path.isfile(f) and os.path.getsize(f) > 0

    def start_session(self, save_src):
        """A fresh session: no counts, no result, a new copy of the save."""
        for d in (self.raw, self.out):
            shutil.rmtree(d, ignore_errors=True)
        os.makedirs(self.raw)
        copy_save(save_src, self)

    def drop(self, k, why):
        """The profile no longer fits: kept beside as .stale (for a look), the next optimized build makes a new one."""
        for p in (self.profile(k), self.order(k), self.meta(k)):
            if os.path.exists(p):
                os.replace(p, p + '.stale')
        print('optimized build: profile dropped (%s)' % why, flush=True)

    def clean_scratch(self):
        for d in (self.gen, self.raw, self.save, self.out):
            shutil.rmtree(d, ignore_errors=True)
        if os.path.exists(self.status):
            os.remove(self.status)


def house_line():
    """The line of session.nxs that checks the save's progress (`@assert prog`): a failure there means no first house yet."""
    with open(SESSION, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            if line.strip().startswith('@assert prog'):
                return n
    return 0


def off_track(store):
    """Why the last play-test ran and its script failed (exit 1: a step it waits on never came): 'first_house' when it was the
    progress check (house_line()), 'play_test' for any other step (never in the world, never outdoors); None when it passed, did
    not run, or ended otherwise (a crash, a freeze or a stop is not this)."""
    try:
        with open(os.path.join(store.out, 'result.json'), encoding='utf-8') as r:
            res = json.load(r)
    except (OSError, ValueError):
        return None
    if not isinstance(res, dict) or res.get('exit_code') != 1:
        return None
    return 'first_house' if res.get('fail_line') == house_line() else 'play_test'


def wait_after_play_test(store, why):
    """Records why the optimized build built the normal game (Store.status, `why`: a MESSAGES key) and clears the session, so the
    next optimized build plays it again (the test version is kept: then it only links)."""
    for d in (store.raw, store.save, store.out):
        shutil.rmtree(d, ignore_errors=True)
    with open(store.status, 'w', encoding='utf-8') as f:
        json.dump({'waiting': why}, f)


def profile_hash(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()[:16]


def mismatched(done_path, store, k):
    """The finished build (its done.json) was made with this dump's kept profile and its compiler found too many functions that no
    longer match it (profile_mismatched), unless that profile was kept on purpose (accept())."""
    try:
        with open(done_path, encoding='utf-8') as f:
            done = json.load(f)
    except (OSError, ValueError):
        return False
    if not isinstance(done, dict) or not os.path.isfile(store.profile(k)):
        return False
    if accepted(store, k, str((done.get('tools') or {}).get('lifter', ''))):
        return False
    made = (done.get('tools') or {}).get('options') or {}
    return made.get('profile') == profile_hash(store.profile(k)) and too_many(done.get('profile_mismatched', 0), functions(store.meta(k)))


def functions(meta_path):
    """The number of functions with counts in a kept profile (merge() writes it), or 0."""
    try:
        with open(meta_path, encoding='utf-8') as f:
            return int(json.load(f).get('functions', 0))
    except (OSError, ValueError, AttributeError):
        return 0


def usable(store, k, profdata):
    """Whether this dump's profile is there and this package's llvm-profdata reads it (a toolchain change can make it unreadable)."""
    p = store.profile(k)
    if not os.path.isfile(p) or not functions(store.meta(k)):
        return False
    r = subprocess.run([profdata, 'show', p], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    if r.returncode != 0:
        store.drop(k, 'this llvm-profdata cannot read it')
        return False
    return True


def merge(store, k, profdata):
    """The session's raw counts into the kept profile and the order file. Raises RuntimeError with llvm-profdata's words."""
    out = store.profile(k)
    r = subprocess.run([profdata, 'merge', '--sparse', '-o', out + '.tmp', store.raw_file()], capture_output=True, text=True,
                       stdin=subprocess.DEVNULL)
    if r.returncode != 0:
        raise RuntimeError('llvm-profdata merge failed: ' + (r.stderr or r.stdout).strip()[-500:])
    show = subprocess.run([profdata, 'show', '--topn=%d' % ORDER_TOP, out + '.tmp'], capture_output=True, text=True,
                          stdin=subprocess.DEVNULL)
    if show.returncode != 0:
        os.remove(out + '.tmp')
        raise RuntimeError('llvm-profdata show failed: ' + (show.stderr or show.stdout).strip()[-500:])
    names, total = [], 0
    for line in show.stdout.splitlines():
        m = re.match(r'\s+(\S+), max count', line)
        if m:
            names.append(m.group(1))
        t = re.match(r'Total functions:\s*(\d+)', line.strip())
        if t:
            total = int(t.group(1))
    if not total:                    # an empty profile would build the normal code under the optimized stamp
        os.remove(out + '.tmp')
        raise RuntimeError('the play-test session counted no functions')
    os.replace(out + '.tmp', out)
    with open(store.order(k), 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(n + '\n' for n in names))
    with open(store.meta(k), 'w', encoding='utf-8') as f:
        json.dump({'key': k, 'functions': total, 'hot': len(names)}, f)
    return total


def lifter_of(done_path):
    """The lifter hash a finished build was made with (done.json tools.lifter), or ''."""
    try:
        with open(done_path, encoding='utf-8') as f:
            return str(((json.load(f) or {}).get('tools') or {}).get('lifter', ''))
    except (OSError, ValueError, AttributeError):
        return ''


def accept(store, k, done_path):
    """Keeps this profile although the build in done_path found many mismatches: it was made from this very code, so a new one
    would be no better (pkp_build.py build_with_options, mismatched()). Only for that lifter: a later lifter release can still
    make it again."""
    with open(store.meta(k), encoding='utf-8') as f:
        meta = json.load(f)
    meta['accepted'] = lifter_of(done_path)
    with open(store.meta(k), 'w', encoding='utf-8') as f:
        json.dump(meta, f)


def accepted(store, k, lifter):
    try:
        with open(store.meta(k), encoding='utf-8') as f:
            return bool(lifter) and json.load(f).get('accepted') == lifter
    except (OSError, ValueError, AttributeError):
        return False


def count_mismatches(text):
    """How many functions one compile's output names as not matching the profile."""
    return sum(1 for line in text.splitlines() if MISMATCH_RE.search(line))


def too_many(mismatched, profiled):
    return profiled > 0 and mismatched > MISMATCH_SHARE * profiled


def require_save(save_src):
    """The session plays the player's own save (the script goes from the title through Continue into the world); on an empty one
    it would play the start of a new game and learn little. RuntimeError with the player's words when there is none."""
    has = save_src and any(files for _, _, files in os.walk(save_src))
    if not has:
        raise RuntimeError('the play-test needs a save to play a copy of, and there is none yet (%s). Switch "Optimized build" off, '
                           'set up and play the game once, then switch it on and set up again.' % (save_src or 'no save folder'))


def copy_save(save_src, store):
    """A copy of the player's save for the session (never the save itself)."""
    require_save(save_src)
    shutil.rmtree(store.save, ignore_errors=True)
    shutil.copytree(save_src, store.save)


def session_env(store, headless=True):
    """The settings of the play-test session (runtime/docs/CONFIG.md: [test], [boot], [fs]): the script, its output, frames from the
    frame tick, a copy of the save, no sound, a hard time limit; on a PC also no window, beside a running copy of the game (a phone
    shows the session: the app's game window is the only one SDL has there)."""
    env = {'NXRT_TEST_SCRIPT': SESSION, 'NXRT_TEST_OUT': store.out, 'NXRT_TEST_FRAME_SOURCE': 'tick', 'NXRT_FS_SAVE_DIR': store.save,
           'SDL_AUDIODRIVER': 'dummy', 'NXRT_BOOT_MAX_SECONDS': str(SESSION_SECONDS),
           'NXRT_TEST_WALL_TIMEOUT_S': str(SESSION_SECONDS)}   # a script's own limit is 900 s (tt_core.cpp): too short for a slow PC
    if headless:
        env.update({'NXRT_BOOT_GFX': 'headless', 'NXRT_ALLOW_MULTIPLE': '1'})
    return env


def run_session(runner, exe, game, save_src, store):
    """PC: plays the session with the test version `exe` (Runner: the launcher's Pause stops it). The script ends the process
    itself (@pass, exit 0). A session that did not finish (paused, crashed, timed out) leaves no counts behind: a part of the tour
    is not a profile to build with, and the next Set up plays it again."""
    store.start_session(save_src)
    folder = os.path.dirname(exe)
    # Windows: the runtime beside the exe, always the game folder's current one (the launcher puts it there; an older copy left
    # from an earlier release would not match the exe)
    # Linux: libpkp_rt.so the same way (the exe finds it beside itself, RUNPATH $ORIGIN; link() already put the package's
    # sdk copy there, which stays when the game folder has none)
    for name in ('pkp_rt.dll', 'libusb-1.0.dll', 'libpkp_rt.so'):
        if os.path.isfile(os.path.join(game, name)):
            shutil.copyfile(os.path.join(game, name), os.path.join(folder, name))
    env = dict(os.environ, **session_env(store))
    rc, tail = -1, ''
    try:
        rc, tail = runner.run([exe, '--game', game], env=env, cwd=game)
    finally:
        print('optimized build: play-test session ended (exit %d)' % rc, flush=True)
        if rc != 0:
            shutil.rmtree(store.raw, ignore_errors=True)
    if not store.session_done():
        raise RuntimeError('the play-test session did not finish (exit %d):\n%s' % (rc, tail))


if __name__ == '__main__':
    # self-check of the pure parts (no game, no compiler): python -I pgo/pkp_pgo.py
    assert key({'main_build_id': 'ab', 'sha256': {'main': '1', 'sdk': '2'}}) == key({'sha256': {'sdk': '2', 'main': '1'}, 'main_build_id': 'ab'})
    sample = ('warning: c_105.cpp: Function control flow change detected (hash mismatch) f_1 Hash = 1 [-Wbackend-plugin]\n'
              'warning: c_105.cpp: Function control flow change detected (hash mismatch) f_2 Hash = 2 [-Wbackend-plugin]\n'
              'clang: note: something else\n')
    assert count_mismatches(sample) == 2
    assert not too_many(10, 100) and too_many(11, 100) and not too_many(5, 0)
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        for empty in (None, d, os.path.join(d, 'missing')):
            try:
                require_save(empty)
                raise AssertionError('an empty save passed')
            except RuntimeError:
                pass
        os.makedirs(os.path.join(d, 'account'))
        open(os.path.join(d, 'account', 'x'), 'w').close()
        require_save(d)
        st = Store(os.path.join(d, 'g'))
        os.makedirs(st.root)
        with open(st.meta('k'), 'w') as f:
            json.dump({'functions': 3}, f)
        done = os.path.join(d, 'done.json')
        with open(done, 'w') as f:
            json.dump({'tools': {'lifter': 'L1'}}, f)
        assert not accepted(st, 'k', 'L1')
        accept(st, 'k', done)
        assert accepted(st, 'k', 'L1') and not accepted(st, 'k', 'L2') and functions(st.meta('k')) == 3
        os.makedirs(st.out)
        assert house_line() > 0
        for line, why in ((house_line(), 'first_house'), (house_line() + 3, 'play_test')):
            with open(os.path.join(st.out, 'result.json'), 'w') as f:
                json.dump({'exit_code': 1, 'fail_line': line}, f)
            assert off_track(st) == why
        with open(os.path.join(st.out, 'result.json'), 'w') as f:
            json.dump({'exit_code': 4, 'fail_line': 0}, f)
        assert off_track(st) is None
        wait_after_play_test(st, 'first_house')
        assert os.path.isfile(st.status) and off_track(st) is None
        st.clean_scratch()
        assert not os.path.exists(st.status)
    print('ok')
    sys.exit(0)
