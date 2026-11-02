"""Texture pipeline proof on real MPR data.

  python txtr_pipeline.py layout            -> for every distinct (format, kind, size, layers, mips) check that tools/tiling's
                                               computed tiled size equals the TXTR's GPU buffer size (which block height works)
  python txtr_pipeline.py samples [--n 4]   -> per format/kind: LZSS-decompress, untile with tegra_tile, compare with retrotool's
                                               own deswizzle, ASTC -> BC7 with astc2bc7, PSNR/SSIM vs the ASTC decode, timing
  python txtr_pipeline.py bulk --count N    -> convert every mip level of N ASTC textures (random, seeded), measure total time/MPix

Needs: work/survey/textures/txtr_all.tsv (txtr_tally.py), tools/tiling + tools/textures builds, retrotool, numpy/scipy/pillow.
Outputs under work/survey/textures/.
"""
import argparse, collections, concurrent.futures as cf, csv, json, os, random, re, shutil, struct, subprocess, sys, tempfile, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'textures', 'oracle'))
from txtr_tally import fmt_info
import txtr_decode
import metrics

TILE = os.path.join(ROOT, 'tools', 'tiling', 'build', 'release', 'tegra_tile.exe')
A2B = os.path.join(ROOT, 'tools', 'textures', 'build', 'release', 'astc2bc7.exe')
BC7DEC = os.path.join(ROOT, 'tools', 'textures', 'build', 'oracle', 'bc7dec.exe')
RETRO = os.path.join(ROOT, 'toolchain', 'retrotool', 'retrotool.exe')
OUT = os.path.join(ROOT, 'work', 'survey', 'textures')
TSV = os.path.join(OUT, 'txtr_all.tsv')
LEVEL_RE = re.compile(r'level (\d+): (\d+)x(\d+)x(\d+) blocks (\d+)x(\d+) gob_y (\d+) gob_z (\d+) linear@(\d+) tiled@(\d+) \((\d+)\)')


def rows():
    out = []
    with open(TSV) as f:
        for r in csv.DictReader(f, delimiter='\t'):
            for k in ('w', 'h', 'layers', 'mips', 'gpu_size'):
                r[k] = int(r[k])
            out.append(r)
    return out


def distinct(rs):
    d = {}
    for r in rs:
        d.setdefault(r['id'], r)
    return list(d.values())


def gob_swizzle_crate(rows_blocks):
    """block height the `tegra_swizzle` crate (used by retrotool) picks for level 0"""
    hh = rows_blocks + rows_blocks // 2
    return 16 if hh >= 128 else 8 if hh >= 64 else 4 if hh >= 32 else 2 if hh >= 16 else 1


def gob_tiling_default(rows_blocks):
    g = 1
    while g < 16 and g * 8 < rows_blocks:
        g <<= 1
    return g


def tile_args(r, gob_y, tight=True, gob_z=1):
    bw, bh, bpb, fam, srgb = fmt_info(r['fmt'])
    a = ['--width', str(r['w']), '--height', str(r['h']), '--levels', str(r['mips']), '--bpb', str(bpb),
         '--block', f'{bw}x{bh}', '--gob-y', str(gob_y)]
    if r['kind'] == '3D':
        a += ['--depth', str(r['layers']), '--gob-z', str(gob_z)]
    else:
        a += ['--layers', str(r['layers'])]
    return a + (['--tight'] if tight else [])


def run_tile(r, gob_y, inp, outp, gob_z=1):
    """-> (rc, size, stdout, stderr); size = the tiled size, or the layer stride when that is what the GPU buffer holds
    (3D textures are allocated with the last depth block untrimmed)"""
    p = subprocess.run([TILE, 'untile', inp, outp] + tile_args(r, gob_y, gob_z=gob_z), capture_output=True, text=True)
    m = re.search(r'tiled (\d+) bytes, layer stride (\d+)', p.stdout)
    size = None
    if m:
        size = int(m.group(1))
        if r['kind'] == '3D' and size != r['gpu_size'] and int(m.group(2)) == r['gpu_size']:
            size = int(m.group(2))
    return p.returncode, size, p.stdout, p.stderr


# ---------------------------------------------------------------- layout check
def cmd_layout(a):
    rs = distinct(rows())
    combos = {}
    for r in rs:
        combos.setdefault((r['fmt'], r['kind'], r['w'], r['h'], r['layers'], r['mips'], r['gpu_size']), r)
    maxsz = max(r['gpu_size'] for r in rs)
    tmp = tempfile.mkdtemp(dir=OUT)
    dummy = os.path.join(tmp, 'zero.bin')
    with open(dummy, 'wb') as f:
        f.truncate(maxsz)

    def one(item):
        key, r = item
        rb = -(-r['h'] // fmt_info(r['fmt'])[1])
        sizes = {}
        zs = (1, 2, 4, 8, 16, 32) if r['kind'] == '3D' else (1,)
        for g in (1, 2, 4, 8, 16, 32):
            for z in zs:
                rc, tiled, _, _ = run_tile(r, g, dummy, os.path.join(tmp, f'o{os.getpid()}_{id(r)}'), gob_z=z)
                sizes[(g, z) if r['kind'] == '3D' else g] = tiled
        ok = [g for g, t in sizes.items() if t == r['gpu_size']]
        return key, rb, ok

    with cf.ThreadPoolExecutor(16) as ex:
        res = list(ex.map(one, combos.items()))
    shutil.rmtree(tmp, ignore_errors=True)
    n = len(res)
    any_ok = sum(1 for _, _, ok in res if ok)
    res2d = [x for x in res if x[0][1] != '3D']
    a_ok = sum(1 for k, rb, ok in res2d if gob_swizzle_crate(rb) in ok)
    b_ok = sum(1 for k, rb, ok in res2d if gob_tiling_default(rb) in ok)
    both = sum(1 for k, rb, ok in res2d if gob_swizzle_crate(rb) in ok and gob_tiling_default(rb) in ok)
    for k, rb, ok in res:
        if k[1] == '3D':
            print('3D', k, 'valid (gob_y, gob_z):', ok)
    n = len(res2d)
    print(f'{len(rs)} distinct textures, {n} distinct (format, kind, size, layers, mips, gpu_size) combos')
    print(f'(2D/cube/array combos: {n}; 3D combos listed above)')
    print(f'tiled size == GPU buffer size for some block height 1..32: {any_ok}/{len(res)}')
    print(f'  with the tegra_swizzle-crate rule (retrotool):       {a_ok}/{n}')
    print(f'  with tools/tiling default_gob_blocks_y():            {b_ok}/{n}')
    print(f'  both rules agree and match:                          {both}/{n}')
    bad = [(k, rb, ok) for k, rb, ok in res if not ok]
    n = len(res2d)
    for k, rb, ok in bad[:10]:
        print('NO MATCH', k, rb)
    diff = [(k, rb, ok) for k, rb, ok in res2d if ok and gob_tiling_default(rb) not in ok]
    for k, rb, ok in diff[:10]:
        print('tools/tiling default wrong:', k, 'block rows', rb, 'valid', ok, 'crate rule', gob_swizzle_crate(rb))
    json.dump(dict(distinct=len(rs), combos=n, any=any_ok, crate_rule=a_ok, tiling_default=b_ok, both=both),
              open(os.path.join(OUT, 'layout_check.json'), 'w'))


# ---------------------------------------------------------------- samples / bulk
def convert_level(blocks, w, h, bw, bh, srgb, tmp, tag, quality=5, dump=True):
    """ASTC blocks of one level -> BC7 DDS. Returns (seconds, rgba_ref, rgba_bc7)."""
    inp, dds, dump_f = (os.path.join(tmp, f'{tag}.blk'), os.path.join(tmp, f'{tag}.dds'), os.path.join(tmp, f'{tag}.rgba'))
    open(inp, 'wb').write(blocks)
    cmd = [A2B, inp, dds, '--raw', f'{w}x{h}:{bw}x{bh}', '--quality', str(quality)] + (['--srgb'] if srgb else ['--linear'])
    if dump:
        cmd += ['--dump-rgba', dump_f]
    t = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.perf_counter() - t
    if p.returncode != 0:
        raise RuntimeError(f'astc2bc7 failed: {p.stderr.strip() or p.stdout.strip()}')
    if not dump:
        return dt, None, None
    d = open(dump_f, 'rb').read()
    rw, rh, _ = struct.unpack_from('<III', d, 0)
    ref = np.frombuffer(d, np.uint8, offset=12).reshape(rh, rw, 4)
    dec, _ = metrics.decode_dds(dds, BC7DEC)
    return dt, ref, dec


def untile_to_file(r, tiled, tmp, tag):
    inp, outp = os.path.join(tmp, f'{tag}.tiled'), os.path.join(tmp, f'{tag}.lin')
    open(inp, 'wb').write(tiled)
    bw, bh, bpb, fam, srgb = fmt_info(r['fmt'])
    rb = -(-r['h'] // bh)
    zs = (16, 8, 4, 2, 1, 32) if r['kind'] == '3D' else (1,)
    for g in dict.fromkeys([gob_swizzle_crate(rb), gob_tiling_default(rb), 1, 2, 4, 8, 16, 32]):
        for z in zs:
            rc, tiled_sz, out, err = run_tile(r, g, inp, outp, gob_z=z)
            if rc == 0 and tiled_sz == r['gpu_size']:
                levels = [tuple(int(x) for x in m.groups()) for m in LEVEL_RE.finditer(out)]
                return (g, z) if z > 1 or r['kind'] == '3D' else g, open(outp, 'rb').read(), levels
    return None, None, None


def retrotool_check(r, lin, tmp, tag):
    """compare our untiled output with retrotool (tegra_swizzle crate) on the same file"""
    src = os.path.join(tmp, f'{tag}.TXTR')
    shutil.copy(r['path'], src)
    fam = fmt_info(r['fmt'])[3]
    ext = 'astc' if fam == 'ASTC' else 'dds'
    cmd = [RETRO, 'txtr', 'convert'] + (['-a'] if fam == 'ASTC' else []) + [src]
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = os.path.join(tmp, f'{tag}.{ext}')
    if p.returncode != 0 or not os.path.exists(out):
        return 'retrotool-failed'
    b = open(out, 'rb').read()
    if fam == 'ASTC':
        pay = b[16:]
        return 'identical' if lin[:len(pay)] == pay else 'DIFFERENT'
    # DDS: compare the tail (all mips of a single layer are stored back to back in both)
    n = len(lin)
    if len(b) >= n and b[-n:] == lin:
        return 'identical'
    return 'different-or-reordered'


def pick_samples(rs, n_per):
    groups = collections.defaultdict(list)
    for r in rs:
        fam = fmt_info(r['fmt'])[3]
        cls = 'tex' if r['kind'] == '2D' else r['kind']
        groups[(r['fmt'], cls)].append(r)
    picks = []
    for key, g in sorted(groups.items()):
        g = [x for x in g if x['gpu_size'] <= 6 * 1024 * 1024] or g[:1]
        g.sort(key=lambda x: x['gpu_size'])
        k = n_per if fmt_info(key[0])[3] == 'ASTC' and key[1] == 'tex' else 2 if fmt_info(key[0])[3] == 'ASTC' else 1
        idx = sorted(set(round(i * (len(g) - 1) / max(k - 1, 1)) for i in range(k))) if k > 1 else [len(g) // 2]
        picks += [g[i] for i in idx]
    return picks


def cmd_samples(a):
    rs = distinct(rows())
    picks = pick_samples(rs, a.n)
    tmp = os.path.join(OUT, 'samples')
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    results = []
    modes = {}
    t_astc = 0.0
    px_astc = 0
    for i, r in enumerate(picks):
        tag = f's{i:03d}'
        bw, bh, bpb, fam, srgb = fmt_info(r['fmt'])
        row = dict(i=i, path=r['path'].split('paks/')[-1], fmt=r['fmt'], kind=r['kind'], w=r['w'], h=r['h'], layers=r['layers'],
                   mips=r['mips'], gpu_size=r['gpu_size'])
        try:
            t0 = time.perf_counter()
            head, tiled = txtr_decode.load(r['path'], modes)
            row['lzss_ms'] = round((time.perf_counter() - t0) * 1000, 1)
            row['size_ok'] = len(tiled) == r['gpu_size']
            g, lin, levels = untile_to_file(r, tiled, tmp, tag)
            row['gob_y'] = g
            row['untile'] = 'ok' if g else 'FAIL'
            if not g:
                results.append(row)
                continue
            row['crate_rule_gob'] = gob_swizzle_crate(-(-r['h'] // bh))
            if r['layers'] == 1 and r['kind'] == '2D':
                row['vs_retrotool'] = retrotool_check(r, lin, tmp, tag)
            if fam == 'ASTC' and r['kind'] in ('2D', 'Cube', '2DArray'):
                # level 0 of layer 0 starts at linear offset 0 in both layer conventions
                lv = levels[0]
                w, h, nbx, nby, off = lv[1], lv[2], lv[4], lv[5], lv[8]
                blocks = lin[off:off + nbx * nby * 16]
                dt, ref, dec = convert_level(blocks, w, h, bw, bh, srgb, tmp, tag)
                m = metrics.metrics(ref, dec)
                row.update(a2b_ms=round(dt * 1000, 1), mpix=round(w * h / 1e6, 3), psnr_rgb=round(m['psnr_rgb'], 2),
                           psnr_rgba=round(m['psnr_rgba'], 2), ssim=round(m['ssim_luma'], 4),
                           alpha_not255=int((dec[..., 3] != 255).sum()), ref_alpha_not255=int((ref[..., 3] != 255).sum()))
        except Exception as e:
            row['error'] = f'{type(e).__name__}: {e}'
        results.append(row)
        print({k: v for k, v in row.items() if k in ('i', 'fmt', 'kind', 'w', 'h', 'layers', 'mips', 'gob_y', 'vs_retrotool', 'psnr_rgb', 'ssim', 'error')},
              flush=True)
    json.dump(dict(results=results, lzss_modes=modes), open(os.path.join(OUT, 'samples_result.json'), 'w'), indent=1)
    ok = [x for x in results if x.get('untile') == 'ok']
    print(f'\nsamples {len(results)}, untile ok {len(ok)}, size ok {sum(1 for x in results if x.get("size_ok"))}')
    rt = collections.Counter(x.get('vs_retrotool') for x in results if 'vs_retrotool' in x)
    print('vs retrotool:', dict(rt))
    ps = [x for x in results if 'psnr_rgb' in x]
    if ps:
        print(f'ASTC->BC7 on {len(ps)} samples: PSNR RGB min {min(x["psnr_rgb"] for x in ps)} mean {np.mean([x["psnr_rgb"] for x in ps]):.2f}; '
              f'SSIM min {min(x["ssim"] for x in ps)} mean {np.mean([x["ssim"] for x in ps]):.4f}')
        byfmt = collections.defaultdict(list)
        for x in ps:
            byfmt[x['fmt']].append(x['psnr_rgb'])
        for k, v in sorted(byfmt.items()):
            print(f'  {k:14} n={len(v)} mean PSNR {np.mean(v):.2f} min {min(v):.2f}')
    print('LZSS modes seen:', modes)


def cmd_bulk(a):
    """convert every mip level (layer 0) of N random ASTC 2D textures; no quality metrics, timing only"""
    rs = [r for r in distinct(rows()) if fmt_info(r['fmt'])[3] == 'ASTC' and r['kind'] == '2D']
    random.Random(a.seed).shuffle(rs)
    tmp = os.path.join(OUT, 'bulk')
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    tot_t, tot_px, nlev = 0.0, 0, 0
    t_wall = time.perf_counter()
    for i, r in enumerate(rs[:a.count]):
        bw, bh, bpb, fam, srgb = fmt_info(r['fmt'])
        head, tiled = txtr_decode.load(r['path'])
        g, lin, levels = untile_to_file(r, tiled, tmp, 'b')
        for lv in levels:
            w, h, nbx, nby, off = lv[1], lv[2], lv[4], lv[5], lv[8]
            blocks = lin[off:off + nbx * nby * 16]
            dt, _, _ = convert_level(blocks, w, h, bw, bh, srgb, tmp, 'b', dump=False)
            tot_t += dt
            tot_px += w * h
            nlev += 1
    wall = time.perf_counter() - t_wall
    print(f'{a.count} textures, {nlev} levels, {tot_px/1e6:.2f} MPix (all mips); astc2bc7 process time {tot_t:.1f} s '
          f'= {tot_px/1e6/tot_t:.2f} MPix/s (CLI incl. process start + DLL load); wall incl. python decode/untile {wall:.1f} s')
    json.dump(dict(textures=a.count, levels=nlev, mpix=tot_px / 1e6, a2b_seconds=tot_t, mpix_per_s=tot_px / 1e6 / tot_t, wall=wall),
              open(os.path.join(OUT, 'bulk_result.json'), 'w'))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    sp.add_parser('layout')
    s = sp.add_parser('samples')
    s.add_argument('--n', type=int, default=4)
    b = sp.add_parser('bulk')
    b.add_argument('--count', type=int, default=40)
    b.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    {'layout': cmd_layout, 'samples': cmd_samples, 'bulk': cmd_bulk}[args.cmd](args)
