#!/usr/bin/env python
"""Harvest 'Name 0xADDR' candidates for MPR 1.0.0 main offsets from MetroidPrimePort notes/code.
usage: kb_harvest_port.py OUT.json   (scans refs/MetroidPrimePort except decomp/extern)"""
import re, sys, json, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = ROOT / 'refs' / 'MetroidPrimePort'
ADDR = re.compile(r'\b0x([0-9a-fA-F]{5,7})\b')
NAME = re.compile(r'((?:[A-Za-z_][A-Za-z0-9_]*::)*[A-Za-z_~][A-Za-z0-9_]*)\s*(?:\(\))?[`\'"]?\s*(?:\(|at|@|:|,)?\s*$')
skip = ('decomp', 'extern', '.git', 'tests/fixtures', 'build', 'assets')
out = []
for p in sorted(BASE.rglob('*')):
    rel = p.relative_to(BASE).as_posix()
    if not p.is_file() or any(rel.startswith(s) or ('/' + s + '/') in '/' + rel for s in skip): continue
    if p.suffix not in ('.cpp', '.h', '.hpp', '.md', '.inc', '.txt', '.py', '.c'): continue
    try: text = p.read_text(encoding='utf-8', errors='replace').splitlines()
    except Exception: continue
    for i, l in enumerate(text, 1):
        for m in ADDR.finditer(l):
            v = int(m.group(1), 16)
            if not (0x1000 <= v < 0x2800000): continue
            pre = l[:m.start()]
            nm = NAME.search(pre)
            out.append({'file': 'refs/MetroidPrimePort/' + rel, 'line': i, 'addr': '0x' + m.group(1).lower(),
                        'guess_name': nm.group(1) if nm else None, 'text': l.strip()[:240]})
json.dump(out, open(sys.argv[1], 'w'), indent=0)
from collections import Counter
print(len(out), 'candidates;', len({o['addr'] for o in out}), 'unique addrs')
print(Counter(o['file'].split('/')[-1] for o in out).most_common(15))
