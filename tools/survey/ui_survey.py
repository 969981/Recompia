"""UI survey: GUIF list, asset-type census + keyword search, Scaleform/SWF byte scan, main.elf strings/symbols,
UI/FrontEnd pak census.
usage: tools/.venv/Scripts/python.exe -u tools/survey/ui_survey.py [index|scan|elf|all]
Read-only on game/ and refs/. Writes under work/survey/ui/. Uses read_adir copied from tools/survey/inventory.py
(inventory.py runs its main() at import time, so it cannot be imported)."""
import os, sys, struct, uuid, re, mmap, csv, collections
from multiprocessing import Pool

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
ROMFS = ROOT + '/game/romfs'
PAKS = ROOT + '/work/survey/paks'
ELF = ROOT + '/work/elf/main.elf'
OUT = ROOT + '/work/survey/ui'

PAT = re.compile(rb'(?:GFX|CFX|FWS)[\x06-\x0c]')
TAGS = [b'GFXL', b'GFXC']


def read_adir(path):
    with open(path, 'rb') as f:
        hdr = f.read(64)
        assert hdr[:4] == b'RFRM' and hdr[20:24] == b'PACK', path
        assert hdr[32:36] == b'RFRM' and hdr[52:56] == b'TOCC', path
        pack_size = struct.unpack_from('<Q', hdr, 4)[0]
        tocc_size = struct.unpack_from('<Q', hdr, 36)[0]
        toc = f.read(tocc_size)
    off = 0; adir = None; strg = {}
    while off + 24 <= len(toc):
        cid = toc[off:off+4]; size = struct.unpack_from('<Q', toc, off+4)[0]; skip = struct.unpack_from('<Q', toc, off+16)[0]
        body = toc[off+24+skip: off+24+skip+size]
        if cid == b'ADIR':
            n = struct.unpack_from('<I', body, 0)[0]
            adir = []
            for i in range(n):
                e = body[4+i*52: 4+(i+1)*52]
                t = e[:4].decode('latin1')
                aid = uuid.UUID(bytes_le=e[4:20])
                ver, ver2 = struct.unpack_from('<II', e, 20)
                o, ds, s = struct.unpack_from('<QQQ', e, 28)
                adir.append((t, aid, o, ds, s))
        elif cid == b'STRG':
            n = struct.unpack_from('<I', body, 0)[0]; p = 4
            for _ in range(n):
                p += 4; aid = uuid.UUID(bytes_le=body[p:p+16]); p += 16
                ln = struct.unpack_from('<I', body, p)[0]; p += 4
                strg.setdefault(aid, []).append(body[p:p+ln].decode('utf8', 'replace')); p += ln
        off += 24 + skip + size
    return adir, strg


def all_paks():
    res = []
    for dp, _, fs in os.walk(ROMFS):
        for fn in fs:
            if fn.endswith('.pak'):
                p = os.path.join(dp, fn)
                res.append((os.path.relpath(p, ROMFS).replace(os.sep, '/'), p.replace(os.sep, '/')))
    return sorted(res)


def write_tsv(path, header, rows):
    os.makedirs(OUT, exist_ok=True)
    with open(path, 'w', newline='', encoding='utf8') as f:
        w = csv.writer(f, delimiter='\t', lineterminator='\n')
        w.writerow(header); w.writerows(rows)


def index():
    # record: (pak_rel, type, uuid, [names], decompressed, stored, adir_offset)
    recs = []
    paks = all_paks()
    print('paks found under game/romfs:', len(paks), flush=True)
    for rel, p in paks:
        adir, strg = read_adir(p)
        for t, aid, o, ds, s in adir:
            recs.append((rel, t, str(aid), strg.get(aid, []), ds, s, o))
    return recs


def step1(recs):
    print('\n== STEP 1: GUIF assets ==')
    g = [r for r in recs if r[1] == 'GUIF']
    g.sort(key=lambda r: (r[0], r[3][0] if r[3] else r[2]))
    rows = [[r[0], r[2], ' | '.join(r[3]) if r[3] else '(no STRG name)', r[4], r[5], r[6]] for r in g]
    write_tsv(OUT + '/guif.tsv', ['pak', 'uuid', 'names', 'decompressed_bytes', 'stored_bytes', 'adir_offset'], rows)
    for row in rows:
        print('\t'.join(map(str, row)))
    print('GUIF count:', len(g))


def step2a(recs):
    print('\n== STEP 2a: asset types across all paks ==')
    cnt = collections.Counter(r[1] for r in recs)
    distinct_keys = set((r[1], r[2]) for r in recs)
    dcnt = collections.Counter(k[0] for k in distinct_keys)
    rows = [[t, cnt[t], dcnt[t]] for t in sorted(cnt, key=lambda t: -cnt[t])]
    write_tsv(OUT + '/types_all_paks.tsv', ['type', 'assets', 'distinct_uuid'], rows)
    print('distinct types:', len(cnt))
    for t, a, d in rows:
        print(f'{t:6} assets={a:6d} distinct={d:6d}')
    print('total assets:', len(recs))

    print('\n-- keyword hits in asset names/STRG (.swf .gfx flash scaleform menu) --')
    KW = re.compile(r'\.swf|\.gfx|flash|scaleform|menu', re.I)
    hits = []
    for r in recs:
        names = r[3]
        if any(KW.search(n) for n in names):
            hits.append([r[1], r[0], r[2], ' | '.join(names)])
    byt = collections.Counter(h[0] for h in hits)
    print('keyword hits per type:', dict(byt))
    write_tsv(OUT + '/keyword_hits.tsv', ['type', 'pak', 'uuid', 'names'], sorted(hits))
    for h in sorted(hits)[:120]:
        print('\t'.join(h))
    print('keyword hit total:', len(hits))


def step4(recs):
    print('\n== STEP 4: UI / FrontEnd paks ==')
    A = [r for r in recs if r[0].startswith('UI/')]
    B = [r for r in recs if r[0].startswith('Worlds/FrontEnd/') and r[0].count('/') == 2]
    C = [r for r in recs if r[0].startswith('Worlds/FrontEnd/')]
    lit = A + B
    print('literal set: UI/**/*.pak =', len(set(r[0] for r in A)), 'paks;  Worlds/FrontEnd/*.pak =', len(set(r[0] for r in B)), 'paks')
    print('recursive set: Worlds/FrontEnd/**/*.pak =', len(set(r[0] for r in C)), 'paks')

    pk = collections.defaultdict(lambda: collections.Counter())
    pkds = collections.Counter()
    for r in lit:
        pk[r[0]][r[1]] += 1
        pkds[r[0]] += r[4]
    rows = []
    for p in sorted(pk):
        parts = ', '.join(f'{t}={c}' for t, c in pk[p].most_common())
        print(f'{p}  ({sum(pk[p].values())} assets, {pkds[p]/1e6:.1f} MB decomp)\n    {parts}')
        for t, c in pk[p].most_common():
            rows.append([p, t, c])
    write_tsv(OUT + '/ui_pak_types.tsv', ['pak', 'type', 'assets'], rows)

    tot = collections.Counter(); totds = collections.Counter()
    for r in lit:
        tot[r[1]] += 1; totds[r[1]] += r[4]
    print('literal set per-type totals:', ', '.join(f'{t}={c}({totds[t]/1e6:.1f}MB)' for t, c in tot.most_common()))

    def name_of(r):
        return r[3][0] if r[3] else '(uuid ' + r[2] + ')'

    print('\n40 biggest assets, literal set (by decompressed size):')
    big = sorted(lit, key=lambda r: -r[4])[:40]
    for r in big:
        print(f'{r[4]:>12d}  {r[1]:5} {r[0]:40}  {name_of(r)}')
    print('\nrecursive set per-type totals:', ', '.join(f'{t}={c}' for t, c in collections.Counter(r[1] for r in C).most_common()))
    print('40 biggest assets, recursive Worlds/FrontEnd set:')
    bigC = sorted(C, key=lambda r: -r[4])[:40]
    for r in bigC:
        print(f'{r[4]:>12d}  {r[1]:5} {r[0]:40}  {name_of(r)}')
    write_tsv(OUT + '/biggest40_literal.tsv', ['decompressed_bytes', 'type', 'pak', 'name'], [[r[4], r[1], r[0], name_of(r)] for r in big])


def scan_one(p):
    out = []
    size = 0
    try:
        size = os.path.getsize(p)
        if size < 4:
            return p, size, out
        with open(p, 'rb') as f:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            try:
                n = len(mm)
                for m in PAT.finditer(mm):
                    o = m.start()
                    ver = mm[o + 3]
                    L = struct.unpack_from('<I', mm, o + 4)[0] if o + 8 <= n else -1
                    out.append(('SWF', o, m.group()[:3].decode('latin1'), ver, L, int(8 <= L <= n - o)))
                for t in TAGS:
                    o = mm.find(t)
                    while o != -1:
                        out.append(('TAG', o, t.decode('latin1'), 0, -1, 0))
                        o = mm.find(t, o + 1)
            finally:
                mm.close()
    except Exception as e:
        out.append(('ERR', 0, repr(e)[:120], 0, -1, 0))
    return p, size, out


def step_scan():
    print('\n== STEP 2b: byte scan of all extracted files ==')
    files = []
    for dp, _, fs in os.walk(PAKS):
        for fn in fs:
            files.append(os.path.join(dp, fn).replace(os.sep, '/'))
    print('files to scan:', len(files), flush=True)
    total = 0
    ext_files = collections.Counter(); swf_by = collections.Counter(); swf_ver = collections.Counter()
    plaus_by = collections.Counter(); tag_by = collections.Counter(); errs = 0
    hits = []
    with Pool(processes=max(1, (os.cpu_count() or 2) - 1)) as pool:
        for i, (p, size, out) in enumerate(pool.imap_unordered(scan_one, files, chunksize=8)):
            total += size
            ext = os.path.splitext(p)[1].lstrip('.') or '(none)'
            ext_files[ext] += 1
            rel = os.path.relpath(p, PAKS).replace(os.sep, '/')
            for kind, o, a, b, L, pl in out:
                if kind == 'SWF':
                    swf_by[ext] += 1; swf_ver[(a, b)] += 1
                    if pl: plaus_by[ext] += 1
                elif kind == 'TAG':
                    tag_by[(a, ext)] += 1
                else:
                    errs += 1
                hits.append((rel, o, kind, a, b, L, pl, ext))
            if (i + 1) % 5000 == 0:
                print('  scanned', i + 1, flush=True)
    print(f'scanned {len(files)} files, {total/1e9:.2f} GB')
    print('scanned files per ext:', ', '.join(f'{k}={v}' for k, v in ext_files.most_common()))
    print('SWF-signature hits (GFX/CFX/FWS + version 6..12):', sum(swf_by.values()))
    print('  by signature+version:', dict(swf_ver))
    print('  by asset type:', dict(swf_by))
    print('  with plausible length field (8 <= L <= remaining bytes):', sum(plaus_by.values()), dict(plaus_by))
    print('TAG hits (GFXL / GFXC):', sum(tag_by.values()), dict(tag_by))
    print('errors:', errs)
    hits.sort()
    write_tsv(OUT + '/scan_hits.tsv', ['file', 'offset', 'kind', 'sig_or_tag', 'version', 'swf_len', 'len_ok', 'asset_type'],
              [[h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]] for h in hits])
    print('first 100 hits (file, offset, kind, sig/tag, version, swf_len, len_ok, type):')
    for h in hits[:100]:
        print('  ', h[0], f'0x{h[1]:x}', h[2], h[3], h[4], h[5], h[6], h[7])
    print('full list in', OUT + '/scan_hits.tsv')


def step_elf():
    from elftools.elf.elffile import ELFFile
    print('\n== STEP 2c: main.elf strings and symbols ==')
    data = open(ELF, 'rb').read()
    print('main.elf bytes:', len(data), 'magic:', data[:4])
    strs = set(s.decode('latin1') for s in re.findall(rb'[\x20-\x7e]{6,}', data))
    print('distinct printable strings >=6 chars:', len(strs))
    KW = ['scaleform', 'gfx', 'flash', '.swf', 'actionscript', 'gfxmovie', 'scaleform::']
    for k in KW:
        print(f'  strings containing {k!r}: {sum(1 for s in strs if k in s.lower())}')
    hits = sorted(s for s in strs if any(k in s.lower() for k in KW))
    write_tsv(OUT + '/elf_string_hits.tsv', ['string'], [[h] for h in hits])
    print('unique matching strings:', len(hits), '(full list in elf_string_hits.tsv)')
    for h in hits[:150]:
        print('  ', h[:160])
    with open(ELF, 'rb') as f:
        ef = ELFFile(f)
        for sn in ('.dynsym', '.symtab'):
            sec = ef.get_section_by_name(sn)
            if sec is None:
                print(sn, 'section absent'); continue
            allsyms = list(sec.iter_symbols())
            defined = [s.name for s in allsyms if s['st_shndx'] != 'SHN_UNDEF' and s.name]
            m = [n for n in defined if re.search(r'GFx|Scaleform|GFXL', n, re.I)]
            print(f'{sn}: total={len(allsyms)} defined={len(defined)} GFx/Scaleform/GFXL defined matches={len(m)}')
            for n in m[:40]:
                print('    ', n[:160])


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    mode = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if mode in ('index', 'all'):
        recs = index()
        step1(recs)
        step2a(recs)
        step4(recs)
    if mode in ('elf', 'all'):
        step_elf()
    if mode in ('scan', 'all'):
        step_scan()
