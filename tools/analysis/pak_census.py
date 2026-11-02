#!/usr/bin/env python
"""Census of asset types in MPR .pak files (reads only the PACK/TOCC/ADIR headers, never the asset bodies).

  pak_census.py game/romfs [--json out.json]
Layout (confirmed on the 1.0.0 dump): RFRM(32 B hdr: magic, size u64, unk u64, form id, reader u32, writer u32) form PACK;
at +32 form TOCC; inside it chunks (id 4, size u64, unk u32, skip u64 => payload at +24+skip): ADIR = u32 count + 52-byte entries
{type 4, uuid 16, reader u32, writer u32, offset u64, decompressed u64, stored u64}.
"""
import json, pathlib, struct, sys, collections

root = pathlib.Path(sys.argv[1])
by = collections.defaultdict(lambda: [0, 0, 0, 0])   # count, stored bytes, decompressed bytes, compressed entries
versions = collections.defaultdict(set)
tx = collections.defaultdict(lambda: [0, 0])
tx_tile = collections.Counter()
bad = []
paks = sorted(root.rglob('*.pak'))
for p in paks:
    with open(p, 'rb') as f:
        h = f.read(64)
        if h[:4] != b'RFRM' or h[20:24] != b'PACK' or h[32:36] != b'RFRM' or h[52:56] != b'TOCC':
            bad.append(str(p)); continue
        tocc_size = struct.unpack_from('<Q', h, 36)[0]
        pos, end = 64, 64 + tocc_size
        while pos < end:
            f.seek(pos); ch = f.read(24)
            if len(ch) < 24: break
            cid = ch[:4]; size = struct.unpack_from('<Q', ch, 4)[0]; skip = struct.unpack_from('<Q', ch, 16)[0]
            if cid == b'ADIR':
                f.seek(pos + 24 + skip)
                n = struct.unpack('<I', f.read(4))[0]
                data = f.read(52 * n)
                for i in range(n):
                    t, uid, rv, wv, off, dec, sto = struct.unpack_from('<4s16sIIQQQ', data, 52 * i)
                    k = t.decode('latin1')
                    if k == 'TXTR':   # HEAD chunk right after the 32-byte RFRM header: kind, format, w, h, layers, tile_mode, swizzle, mips
                        f.seek(off + 32); hc = f.read(24)
                        if hc[:4] == b'HEAD':
                            sk = struct.unpack_from('<Q', hc, 16)[0]
                            f.seek(off + 32 + 24 + sk); hh = f.read(32)
                            kind, fmt, w, hgt, lay, tm, sw, mips = struct.unpack('<8I', hh)
                            tx[(fmt, kind)][0] += 1; tx[(fmt, kind)][1] += sto
                            tx_tile[(tm, sw)] += 1
                    by[k][0] += 1; by[k][1] += sto; by[k][2] += dec; by[k][3] += (sto != dec)
                    versions[k].add((rv, wv))
            pos += 24 + skip + size
rows = sorted(by.items(), key=lambda kv: -kv[1][1])
print(f'{len(paks)} paks ({len(bad)} unreadable)')
print(f'{"type":6} {"count":>8} {"stored MB":>10} {"decomp MB":>10} {"compressed":>10}  (reader,writer) versions')
for k, (c, s, d, z) in rows:
    print(f'{k:6} {c:8d} {s / 1e6:10.1f} {d / 1e6:10.1f} {z:10d}  {sorted(versions[k])[:4]}')
print()
print('TXTR (format code, kind): count, stored MB')
for (fmt, kind), (c, b) in sorted(tx.items(), key=lambda kv: -kv[1][1]): print(f'  format {fmt:3d} kind {kind}: {c:6d}  {b / 1e6:8.1f}')
print('TXTR (tile_mode, swizzle) counts:', dict(tx_tile))
if len(sys.argv) > 3 and sys.argv[2] == '--json':
    json.dump({'paks': len(paks), 'unreadable': bad, 'types': {k: {'count': c, 'stored_bytes': s, 'decompressed_bytes': d, 'compressed_entries': z,
               'versions': sorted(versions[k])} for k, (c, s, d, z) in rows}, 'txtr_formats': {f'{a}/{b}': v for (a, b), v in tx.items()}, 'txtr_tile_swizzle': {f'{a}/{b}': v for (a, b), v in tx_tile.items()}}, open(sys.argv[3], 'w'), indent=1)
