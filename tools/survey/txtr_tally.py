"""Tally every extracted TXTR (work/survey/paks/**/*.TXTR) by format, type, size, mips and layers.

Reads only the HEAD chunk (header) and the FOOT/META chunk (GPU buffer sizes), never the pixel data.
usage: python txtr_tally.py [--paks work/survey/paks] [--out work/survey/textures]
writes <out>/txtr_all.tsv (one row per file) and <out>/txtr_summary.json, prints the tables.
"""
import argparse, collections, concurrent.futures as cf, glob, json, os, struct, uuid

FORMATS = ('R8Unorm R8Snorm R8Uint R8Sint R16Unorm R16Snorm R16Uint R16Sint R16Float R32Uint R32Sint Rgb8Unorm Rgba8Unorm '
           'Rgba8Srgb Rgba16Float Rgba32Float Depth16Unorm Depth16Unorm2 Depth24S8Unorm Depth32Float Bc1Unorm Bc1Srgb Bc2Unorm '
           'Bc2Srgb Bc3Unorm Bc3Srgb Bc4Unorm Bc4Snorm Bc5Unorm Bc5Snorm Rg11B10Float R32Float Rg8Unorm Rg8Snorm Rg8Uint Rg8Sint '
           'Rg16Float Rg16Unorm Rg16Snorm Rg16Uint Rg16Sint Rgb10A2Unorm Rgb10A2Uint Rg32Uint Rg32Sint Rg32Float Rgba16Unorm '
           'Rgba16Snorm Rgba16Uint Rgba16Sint Rgba32Uint Rgba32Sint None Astc4x4 Astc5x4 Astc5x5 Astc6x5 Astc6x6 Astc8x5 Astc8x6 '
           'Astc8x8 Astc10x5 Astc10x6 Astc10x8 Astc10x10 Astc12x10 Astc12x12 Astc4x4Srgb Astc5x4Srgb Astc5x5Srgb Astc6x5Srgb '
           'Astc6x6Srgb Astc8x5Srgb Astc8x6Srgb Astc8x8Srgb Astc10x5Srgb Astc10x6Srgb Astc10x8Srgb Astc10x10Srgb Astc12x10Srgb '
           'Astc12x12Srgb BptcUfloat BptcSfloat BptcUnorm BptcUnormSrgb').split()
KINDS = ('1D', '2D', '3D', 'Cube', '1DArray', '2DArray', '2DMS', '2DMSArray', 'CubeArray')


def fmt_info(name):
    """(block_w, block_h, bytes_per_block, family, srgb)"""
    srgb = name.endswith('Srgb')
    if name.startswith('Astc'):
        d = name[4:].replace('Srgb', '')
        w, h = d.split('x')
        return int(w), int(h), 16, 'ASTC', srgb
    if name.startswith('Bc') or name.startswith('Bptc'):
        bpb = 8 if name[:3] in ('Bc1', 'Bc4') else 16
        return 4, 4, bpb, 'BCn', srgb
    bpp = {'R8': 1, 'Rg8': 2, 'Rgb8': 3, 'R16': 2, 'Rg16': 4, 'Rgba8': 4, 'Rgba16': 8, 'Rgba32': 16, 'R32': 4, 'Rg32': 8,
           'Rg11B10': 4, 'Rgb10A2': 4, 'Depth16': 2, 'Depth24S8': 4, 'Depth32': 4}
    for k in sorted(bpp, key=len, reverse=True):
        if name.startswith(k):
            return 1, 1, bpp[k], 'uncompressed', srgb
    return 1, 1, 0, 'other', srgb


def parse(path):
    with open(path, 'rb') as f:
        b = f.read()
    assert b[:4] == b'RFRM' and b[20:24] == b'TXTR'
    size = struct.unpack_from('<Q', b, 4)[0]
    assert b[32:36] == b'HEAD'
    hsize = struct.unpack_from('<Q', b, 36)[0]
    h = 32 + 24
    kind, fmt, w, hh, layers, tile_mode, swz, nmip = struct.unpack_from('<8I', b, h)
    mips = struct.unpack_from('<%dI' % nmip, b, h + 32)
    p = h + 32 + 4 * nmip
    sunk, filt, mfilt, wx, wy, wz, an = struct.unpack_from('<IBBBBBb', b, p)
    # FOOT
    foot = 32 + size
    assert b[foot:foot + 4] == b'RFRM'
    fsize = struct.unpack_from('<Q', b, foot + 4)[0]
    o = foot + 32
    aid = meta = None
    while o + 24 <= foot + 32 + fsize:
        cid = b[o:o + 4]
        csz = struct.unpack_from('<Q', b, o + 4)[0]
        sk = struct.unpack_from('<Q', b, o + 16)[0]
        body = b[o + 24 + sk:o + 24 + sk + csz]
        if cid == b'AINF':
            aid = str(uuid.UUID(bytes_le=body[:16]))
        elif cid == b'META':
            meta = body
        o += 24 + sk + csz
    m = struct.unpack_from('<6I', meta, 0)   # unk, unk, alloc category, gpu offset, align, decompressed size
    nbuf = struct.unpack_from('<I', meta, 24)[0]
    return dict(id=aid, kind=KINDS[kind], fmt=FORMATS[fmt], w=w, h=hh, layers=layers, tile_mode=tile_mode, swizzle=swz,
                mips=nmip, mip_sizes=list(mips), filt=filt, mfilt=mfilt, wrap=f'{wx}{wy}{wz}', aniso=an,
                alloc=m[2], gpu_size=m[5], file_size=len(b), n_read=nbuf)


def work(path):
    try:
        r = parse(path)
        r['path'] = path
        return r
    except Exception as e:
        return dict(path=path, error=f'{type(e).__name__}: {e}')


def pixels(r):
    """pixels over all mip levels and layers/slices (what a transcoder must process)"""
    w, h, d = r['w'], r['h'], r['layers']
    tot = 0
    for i in range(r['mips']):
        mw, mh = max(w >> i, 1), max(h >> i, 1)
        tot += mw * mh * (max(d >> i, 1) if r['kind'] == '3D' else d)
    return tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--paks', default='work/survey/paks')
    ap.add_argument('--out', default='work/survey/textures')
    a = ap.parse_args()
    files = glob.glob(os.path.join(a.paks, '**', '*.TXTR'), recursive=True)
    with cf.ProcessPoolExecutor() as ex:
        rows = list(ex.map(work, files, chunksize=64))
    err = [r for r in rows if 'error' in r]
    rows = [r for r in rows if 'error' not in r]
    os.makedirs(a.out, exist_ok=True)
    cols = ['path', 'id', 'kind', 'fmt', 'w', 'h', 'layers', 'mips', 'tile_mode', 'swizzle', 'filt', 'mfilt', 'wrap', 'aniso',
            'alloc', 'gpu_size', 'file_size', 'n_read']
    with open(os.path.join(a.out, 'txtr_all.tsv'), 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for r in rows:
            f.write('\t'.join(str(r[c]).replace(os.sep, '/') for c in cols) + '\n')
    # distinct by asset id
    dist = {}
    for r in rows:
        dist.setdefault(r['id'], r)
    summ = {}
    for label, rs in (('all_files', rows), ('distinct_assets', list(dist.values()))):
        by = collections.defaultdict(lambda: dict(count=0, mpix_mip0=0.0, mpix_all=0.0, gpu_mb=0.0))
        for r in rs:
            bw, bh, bpb, fam, srgb = fmt_info(r['fmt'])
            k = r['fmt']
            e = by[k]
            e['count'] += 1
            lay = r['layers']
            e['mpix_mip0'] += r['w'] * r['h'] * (lay if r['kind'] != '3D' else lay) / 1e6
            e['mpix_all'] += pixels(r) / 1e6
            e['gpu_mb'] += r['gpu_size'] / 1e6
        summ[label] = by
    print(f'{len(rows)} TXTR parsed, {len(err)} errors, {len(dist)} distinct asset ids')
    for e in err[:5]:
        print('ERR', e)
    d = summ['distinct_assets']
    print(f'\nDISTINCT assets by format ({len(dist)})')
    print(f'{"format":16} {"count":>6} {"Mpix(mip0)":>11} {"Mpix(all)":>10} {"GPU MB":>9}')
    fam = collections.defaultdict(lambda: [0, 0.0, 0.0, 0.0])
    for k, e in sorted(d.items(), key=lambda kv: -kv[1]['gpu_mb']):
        print(f'{k:16} {e["count"]:6d} {e["mpix_mip0"]:11.2f} {e["mpix_all"]:10.2f} {e["gpu_mb"]:9.1f}')
        fm = fmt_info(k)[3] + (' sRGB' if fmt_info(k)[4] else '')
        for i, key in enumerate(('count', 'mpix_mip0', 'mpix_all', 'gpu_mb')):
            fam[fm][i] += e[key]
    print('\nBy family')
    for k, v in sorted(fam.items(), key=lambda kv: -kv[1][3]):
        print(f'{k:20} {v[0]:6d} {v[1]:11.2f} {v[2]:10.2f} {v[3]:9.1f}')
    for name, key in (('kind', lambda r: r['kind']), ('layers', lambda r: r['layers']), ('mips', lambda r: r['mips']),
                      ('tile_mode', lambda r: r['tile_mode']), ('swizzle', lambda r: r['swizzle']),
                      ('alloc', lambda r: r['alloc']), ('size', lambda r: f"{r['w']}x{r['h']}")):
        c = collections.Counter(key(r) for r in dist.values())
        print(f'\n{name}: ' + ', '.join(f'{k}:{v}' for k, v in c.most_common(25)))
    json.dump(dict(parsed=len(rows), errors=len(err), distinct=len(dist), by_format=summ), open(os.path.join(a.out, 'txtr_summary.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
