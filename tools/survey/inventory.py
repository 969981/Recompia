"""Per-asset-type inventory across every .pak in game/romfs, read straight from the PACK/TOCC/ADIR directory (no extraction).
usage: python tools/survey/inventory.py [--romfs game/romfs] [--out work/survey/inventory.json]
Prints a table; writes json with per-type totals, per-pak totals, and distinct (by asset id) totals."""
import argparse, json, os, struct, sys, uuid, collections

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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', default='game/romfs'); ap.add_argument('--out', default='work/survey/inventory.json')
    a = ap.parse_args()
    per_type = collections.defaultdict(lambda: [0, 0, 0]); distinct = {}; per_pak = {}; names = {}
    for dp, _, fs in os.walk(a.romfs):
        for fn in sorted(fs):
            if not fn.endswith('.pak'): continue
            p = os.path.join(dp, fn); rel = os.path.relpath(p, a.romfs).replace(os.sep, '/')
            adir, strg = read_adir(p)
            tot = [0, 0, 0]
            for t, aid, o, ds, s in adir:
                pt = per_type[t]; pt[0] += 1; pt[1] += ds; pt[2] += s
                tot[0] += 1; tot[1] += ds; tot[2] += s
                distinct.setdefault((t, aid), (ds, s))
                if aid in strg: names[str(aid)] = strg[aid][0]
            per_pak[rel] = {'file_bytes': os.path.getsize(p), 'assets': tot[0], 'decompressed': tot[1], 'stored': tot[2]}
    dt = collections.defaultdict(lambda: [0, 0, 0])
    for (t, aid), (ds, s) in distinct.items():
        d = dt[t]; d[0] += 1; d[1] += ds; d[2] += s
    out = {'per_type': {k: {'count': v[0], 'decompressed': v[1], 'stored': v[2]} for k, v in per_type.items()},
           'distinct_per_type': {k: {'count': v[0], 'decompressed': v[1], 'stored': v[2]} for k, v in dt.items()},
           'per_pak': per_pak}
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(out, open(a.out, 'w'), indent=1)
    print(f"{'type':5} {'count':>7} {'distinct':>8} {'stored MB':>10} {'decomp MB':>10} {'dist.decomp MB':>14}")
    for t, v in sorted(per_type.items(), key=lambda kv: -kv[1][2]):
        d = dt[t]
        print(f"{t:5} {v[0]:7d} {d[0]:8d} {v[2]/1e6:10.1f} {v[1]/1e6:10.1f} {d[1]/1e6:14.1f}")
    T = [sum(v[i] for v in per_type.values()) for i in range(3)]; D = [sum(v[i] for v in dt.values()) for i in range(3)]
    print(f"TOTAL {T[0]:7d} {D[0]:8d} {T[2]/1e6:10.1f} {T[1]/1e6:10.1f} {D[1]/1e6:14.1f}")
    print(f"paks: {len(per_pak)}  file bytes: {sum(v['file_bytes'] for v in per_pak.values())/1e6:.1f} MB")
main()
