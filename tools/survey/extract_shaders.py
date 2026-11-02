"""Extract every NVN shader program (code block + control section) into the layout the tools/shaders harness reads
(manifest.tsv, code_<h>.bin, ctrl_<h>.bin), and report format statistics.

Two places hold shaders, both zlib-wrapped NVN GLSLC output ("program sets"):

1. game/romfs/MaterialArchive.arc = RFRM 'MTRL' (reader version 22) with two backend chunks:
     SDX  (D3D shader set: 1168 records of DXBC, not used on Switch)      SNVN (NVN: 583 records)
   Chunk body: u32 flags, u32 tocSize, u32 bufferCount, bufferCount x (u32 offset, u32 size), u32 materialCount, materials ...,
   (at body+8+tocSize) u32 sourceCount, then sourceCount records:
     u8 state, u32 strLen, char[strLen], u32 dataLen, data[dataLen]
   data = u8 0, u32 N (flags in the high bits), u32 kind, u32 zlibLen, zlib stream, then a trailer
          (u32 stageCount, u32 1, ... per-stage offsets and the control section size).
2. every pak MTRL asset (reader version 168): zlib body (META footer chunk) = 'EXPT' blob that embeds the same records
   (u8 0, u32 N, u32 kind in {0x11, 0x19, 0x08}, u32 zlibLen, zlib work); found by pattern, validated by decompression.

The zlib output is the NVN program layout: stageCount code blocks (0x100-aligned, each starts 0x12345678 and has the SPH
at +0x30) followed by stageCount control sections (magic 0x98761234, +0x704 = code block size). The control section is
0x880 bytes in most records but 0x88c, 0x890, 0x940 ... in some, so the layout is found from the magics.

usage: python extract_shaders.py <MaterialArchive.arc> --out <corpus dir> [--paks work/survey/paks]
"""
import argparse, collections, glob, hashlib, json, os, re, struct, sys, zlib

STAGE = {0: 'vertex', 1: 'vertex', 2: 'tess_control', 3: 'tess_eval', 4: 'geometry', 5: 'fragment'}
CODE_MAGICS = (b'\x78\x56\x34\x12', b'\x79\x56\x34\x12')
CTRL_MAGIC = b'\x34\x12\x76\x98'
EMBED_RE = re.compile(rb'\x00(.{4})([\x11\x19\x08]\x00\x00\x00)(.{4})\x78[\x01\x5e\x9c\xda]', re.S)


def chunks(b):
    assert b[:4] == b'RFRM' and b[20:24] == b'MTRL'
    off = 32
    while off + 24 <= len(b):
        cid = b[off:off + 4]
        size = struct.unpack_from('<Q', b, off + 4)[0]
        skip = struct.unpack_from('<Q', b, off + 16)[0]
        yield cid.decode('latin1').strip(), off + 24 + skip, size
        off += 24 + skip + size


def read_sources(b, base):
    flags, toc, nbuf = struct.unpack_from('<III', b, base)
    p = base + 8 + toc
    n = struct.unpack_from('<I', b, p)[0]
    p += 4
    out = []
    for _ in range(n):
        state = b[p]
        sl = struct.unpack_from('<I', b, p + 1)[0]
        p += 5
        name = b[p:p + sl].decode('latin1')
        p += sl
        dl = struct.unpack_from('<I', b, p)[0]
        p += 4
        out.append((state, name, b[p:p + dl]))
        p += dl
    return dict(flags=flags, toc=toc, nbuf=nbuf, end=p - base), out


def split_raw(raw):
    """decompressed program set -> [(code block, control section)...] found from the magics"""
    codes = [j for j in range(0, len(raw) - 3, 4) if raw[j:j + 4] in CODE_MAGICS]
    ctrls = [j for j in range(0, len(raw) - 3, 4) if raw[j:j + 4] == CTRL_MAGIC]
    stages = len(codes)
    assert stages == len(ctrls) and stages > 0, (codes, ctrls)
    blocks = []
    for k in range(stages):
        c_end = codes[k + 1] if k + 1 < stages else ctrls[0]
        t_end = ctrls[k + 1] if k + 1 < stages else len(raw)
        code, ctrl = raw[codes[k]:c_end], raw[ctrls[k]:t_end]
        assert struct.unpack_from('<I', ctrl, 0x704)[0] == len(code), 'ctrl+0x704 != code block size'
        blocks.append((code, ctrl))
    return blocks


def split_source(d):
    """SShaderSource data -> (n256, kind, [(code, ctrl)...], trailer words)"""
    zero, n256, kind, zl = struct.unpack_from('<BIII', d, 0)
    raw = zlib.decompressobj().decompress(d[13:13 + zl])
    trailer = d[13 + zl:]
    tw = struct.unpack_from('<%dI' % (len(trailer) // 4), trailer, 0)
    blocks = split_raw(raw)
    assert tw[0] == len(blocks), (tw[0], len(blocks))
    return n256, kind, blocks, tw


def mtrl_blob(path):
    """pak MTRL asset (reader version 168): decompress the zlib body named by the META footer chunk"""
    b = open(path, 'rb').read()
    size = struct.unpack_from('<Q', b, 4)[0]
    off = 32 + size
    fsz = struct.unpack_from('<Q', b, off + 4)[0]
    o = off + 32
    while o + 24 <= off + 32 + fsz:
        cid = b[o:o + 4]
        csz = struct.unpack_from('<Q', b, o + 4)[0]
        sk = struct.unpack_from('<Q', b, o + 16)[0]
        if cid == b'META':
            cnt, ver, cs, ds, fo = struct.unpack_from('<5I', b, o + 24 + sk)
            return zlib.decompress(b[fo:fo + cs])
        o += 24 + sk + csz
    raise ValueError('no META')


def embedded_sets(d):
    """program sets embedded in a decompressed pak MTRL -> list of (offset, [(code, ctrl)...])"""
    out = []
    for m in EMBED_RE.finditer(d):
        zl = struct.unpack_from('<I', m.group(3))[0]
        st = m.start() + 13
        try:
            zo = zlib.decompressobj()
            raw = zo.decompress(d[st:st + zl])
            if len(zo.unused_data) or not zo.eof or raw[:4] not in CODE_MAGICS:
                continue
            out.append((m.start(), split_raw(raw)))
        except (zlib.error, AssertionError, struct.error):
            continue
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('arc')
    ap.add_argument('--out', required=True)
    ap.add_argument('--paks', help='extracted pak folder (work/survey/paks): also pull the program sets embedded in pak MTRL assets')
    a = ap.parse_args()
    b = open(a.arc, 'rb').read()
    ch = {n: (o, s) for n, o, s in chunks(b)}
    print('chunks:', {k: hex(v[1]) for k, v in ch.items()})
    info, srcs = read_sources(b, ch['SNVN'][0])
    print('SNVN header', info, 'sources', len(srcs))
    os.makedirs(a.out, exist_ok=True)
    seen, rows = {}, []
    stats = collections.Counter()
    kinds = collections.Counter()
    ctrl_sizes = collections.Counter()
    combos = {'arc': collections.Counter(), 'pak': collections.Counter()}
    arc_hashes, pak_hashes = set(), set()

    def add(ident, blocks, origin):
        combo = []
        for k, (code, ctrl) in enumerate(blocks):
            comp = struct.unpack_from('<I', code, 0)[0] == 0x12345679
            start = struct.unpack_from('<I', ctrl, 0x708)[0]
            plen = struct.unpack_from('<I', ctrl, 0x6f8)[0]
            csz, coff = struct.unpack_from('<II', ctrl, 0x6fc)
            stype = 'compute' if comp else STAGE.get((struct.unpack_from('<I', code, start)[0] >> 10) & 15, '?')
            combo.append(stype)
            ctrl_sizes[len(ctrl)] += 1
            h = hashlib.sha1(code + ctrl).hexdigest()[:16]
            cf, tf = f'code_{h}.bin', f'ctrl_{h}.bin'
            stats['stages_' + origin] += 1
            (arc_hashes if origin == 'arc' else pak_hashes).add(h)
            if h not in seen:
                seen[h] = stype
                open(os.path.join(a.out, cf), 'wb').write(code)
                open(os.path.join(a.out, tf), 'wb').write(ctrl)
            rows.append((f'{ident}_s{k}', stype, 0, len(code), 0, len(ctrl), start, plen, coff, csz, h, cf, tf))
        combos[origin]['+'.join(combo)] += 1

    for i, (state, name, d) in enumerate(srcs):
        n256, kind, blocks, tw = split_source(d)
        kinds[kind] += 1
        add(f'arc{i:04d}', blocks, 'arc')
    if a.paks:
        files = sorted(glob.glob(os.path.join(a.paks, '**', '*.MTRL'), recursive=True))
        for fi, f in enumerate(files):
            sets = embedded_sets(mtrl_blob(f))
            stats['mtrl_files'] += 1
            stats['mtrl_with_sets'] += bool(sets)
            for off, blocks in sets:
                stats['pak_sets'] += 1
                add(f'mtrl{fi:03d}_{off:x}', blocks, 'pak')
    with open(os.path.join(a.out, 'manifest.tsv'), 'w', newline='\n') as m:
        m.write('id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\tconst_size\t'
                'code_sha1_16\tcode_file\tctrl_file\n')
        for r in rows:
            m.write('\t'.join(f'{x:#x}' if isinstance(x, int) and j in (2, 3, 4, 5, 6, 7, 8, 9) else str(x)
                              for j, x in enumerate(r)) + '\n')
    byst = collections.Counter(seen.values())
    rep = dict(arc_sources=len(srcs), pak_program_sets=stats['pak_sets'], mtrl_files=stats['mtrl_files'],
               mtrl_files_with_sets=stats['mtrl_with_sets'], stage_blocks_arc=stats['stages_arc'],
               stage_blocks_pak=stats['stages_pak'], distinct_blocks=len(seen), distinct_in_arc=len(arc_hashes),
               distinct_in_pak=len(pak_hashes), distinct_in_both=len(arc_hashes & pak_hashes),
               distinct_by_stage=dict(byst), stage_combos_arc=dict(combos['arc']), stage_combos_pak=dict(combos['pak']),
               kind_field_arc=dict(kinds), control_sizes={hex(k): v for k, v in ctrl_sizes.items()})
    json.dump(rep, open(os.path.join(a.out, 'extract_report.json'), 'w'), indent=1)
    print(json.dumps(rep, indent=1))


if __name__ == '__main__':
    sys.exit(main())
