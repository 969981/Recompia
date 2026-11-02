#!/usr/bin/env python
"""Find symbols in an ELF by regex on the demangled name (or mangled name), with address, size and optional callers.

  sym_find.py work/elf/main.elf "CFirstPersonCamera|UpdateFOV" [--imports] [--callers] [--limit 60]
Callers = functions containing a `bl` to the symbol (scans .text once, takes ~20 s on main).
"""
import argparse, os, re, sys, collections
sys.path.insert(0, os.path.dirname(__file__))
import validate_kb as V

ap = argparse.ArgumentParser()
ap.add_argument('elf'); ap.add_argument('pattern')
ap.add_argument('--imports', action='store_true', help='search main\'s imports (undefined symbols) instead')
ap.add_argument('--callers', action='store_true')
ap.add_argument('--limit', type=int, default=80)
a = ap.parse_args()
B = V.Binary(a.elf)
rx = re.compile(a.pattern)
if a.imports:
    allimp = sorted(B.imports)
    dm = dict(zip(allimp, B._demangle(allimp)))
    names = [n for n in allimp if rx.search(dm[n]) or rx.search(n)]
    for n in names[: a.limit]:
        print(f'{n}  =  {dm[n]}  {"(defined in " + B.extra[n] + ")" if n in B.extra else ""}')
    print(len(names), 'matches'); sys.exit(0)
hits = [(B.syms[m][0], B.syms[m][1], m) for m in B.syms if rx.search(B.dem[m]) or rx.search(m)]
hits.sort()
callers = collections.defaultdict(set)
if a.callers and hits:
    targets = {h[0] for h in hits}
    text = [s for s in B.loads if s['p_flags'] & 1][0]
    data = B.read(text['p_vaddr'], text['p_filesz'])
    import struct
    for i in range(0, len(data) - 3, 4):
        w = struct.unpack_from('<I', data, i)[0]
        if (w & 0xFC000000) in (0x94000000,):
            imm = w & 0x3FFFFFF
            if imm & 0x2000000: imm -= 0x4000000
            t = text['p_vaddr'] + i + imm * 4
            if t in targets: callers[t].add(text['p_vaddr'] + i)
for v, sz, m in hits[: a.limit]:
    line = f'{v - B.base:#09x} {sz:6d}  {B.dem[m][:110]}'
    if a.callers:
        cs = sorted(callers.get(v, []))
        line += f'   callers={len(cs)}' + (' e.g. ' + ', '.join(f'{c - B.base:#x}({(B.dem.get(B.symbol_containing(c) or "", "?"))[:40]})' for c in cs[:3]) if cs else '')
    print(line)
print(len(hits), 'matches')
