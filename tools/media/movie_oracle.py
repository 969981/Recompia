"""Movie oracle: play every FMV0 movie of the game through the HLE movie:: path (nxrt_tests media_movie) and compare with the container and ffmpeg.

  tools\.venv\Scripts\python tools\media\movie_oracle.py [--runtime-wt DIR] [--romfs DIR] [--work DIR] [--only ID_PREFIX] [--frame-step N] [--threads N]

Steps: fmv_manifest.py (extract the 25 movies from the RomFS paks), fmv_reference.py (ffmpeg frame md5 + reference PCM), write expected.tsv and the
per-movie .pts files, run `nxrt_tests media_movie` with NXRT_MOVIE_DIR, then compare the PCM the HLE path produced with ffmpeg's (compare_pcm.py).
Game data is read at run time from the RomFS and lives only in --work (default work\media_fmv, ignored by git).
"""
import argparse, json, os, subprocess, sys, time

def _project_root():
    """The project folder: NXRT_ROOT, else the nearest parent of this file that holds both tools/ and work/."""
    import os, pathlib
    env = os.environ.get('NXRT_ROOT')
    if env:
        return pathlib.Path(env).as_posix()
    here = pathlib.Path(__file__).resolve()
    for p in here.parents:
        if (p / 'tools').is_dir() and (p / 'work').is_dir():
            return p.as_posix()
    return here.parents[2].as_posix()


ROOT = _project_root()
HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable


def run(cmd, **kw):
    print('>', ' '.join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runtime-wt', default=ROOT + '/runtime-wt/media')
    ap.add_argument('--romfs', default=ROOT + '/game/romfs')
    ap.add_argument('--work', default=ROOT + '/work/media_fmv')
    ap.add_argument('--only', default='')
    ap.add_argument('--frame-step', default='1')
    ap.add_argument('--threads', default='8')
    a = ap.parse_args()
    t0 = time.time()
    if run([PY, '-I', HERE + '/fmv_manifest.py', '--romfs', a.romfs, '--out', a.work]).returncode:
        return 2
    if run([PY, '-I', HERE + '/fmv_reference.py', '--dir', a.work] + (['--only', a.only] if a.only else [])).returncode:
        return 2
    man = json.load(open(a.work + '/manifest.json'))
    lines = []
    for m in man['movies']:
        mid = m['file'][:-4]
        v, au = m['video'], m['audio']
        lines.append('\t'.join(str(x) for x in [mid, v['frame_count'], v['width'], v['height'], au['channels'] if au else 0, au['sample_rate'] if au else 0, au['duration_us'] if au else 0]))
        open('%s/%s.pts' % (a.work, mid), 'w').write('\n'.join(str(x) for x in v['pts_us']) + '\n')
    open(a.work + '/expected.tsv', 'w').write('\n'.join(lines) + '\n')
    out = a.work + '/out'
    os.makedirs(out, exist_ok=True)
    exe = a.runtime_wt + '/build/release/nxrt_tests.exe'
    env = dict(os.environ, NXRT_MOVIE_DIR=a.work, NXRT_MOVIE_OUT=out, NXRT_MOVIE_THREADS=a.threads, NXRT_MOVIE_FRAME_STEP=a.frame_step)
    if a.only:
        env['NXRT_MOVIE_ONLY'] = a.only
    t1 = time.time()
    rc = run([exe, 'media_movie'], env=env, cwd=os.path.dirname(exe)).returncode
    print('test run: %.1f s, exit %d' % (time.time() - t1, rc))
    bad = 0
    for m in man['movies']:
        mid = m['file'][:-4]
        if a.only and not mid.startswith(a.only):
            continue
        au = m['audio']
        ours, ref = '%s/%s.pcm' % (out, mid), '%s/%s.ref.pcm' % (a.work, mid)
        if au and os.path.exists(ours) and os.path.exists(ref):
            r = run([PY, '-I', HERE + '/compare_pcm.py', ref, ours, '--channels', str(au['channels']), '--rate', str(au['sample_rate'])], capture_output=True, text=True)
            print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip(), '  [%s]' % mid[:8])
            bad += r.returncode != 0
        elif au:
            print('missing PCM for', mid[:8]); bad += 1
    print('audio comparisons failed: %d; total %.1f s' % (bad, time.time() - t0))
    return 1 if (rc or bad) else 0


if __name__ == '__main__':
    sys.exit(main())
