#!/usr/bin/env python
"""Turn port_candidates.json into (name, addr) pairs where a code-like name directly precedes the address.
usage: kb_pairs_port.py IN.json OUT.json"""
import json, re, sys, collections
c = json.load(open(sys.argv[1]))
PAIR = re.compile(r'(?P<name>(?:[A-Za-z_][A-Za-z0-9_]*::)*[A-Za-z_~][A-Za-z0-9_]*)(?:\(\))?[`\'"]?[ ]*(?:\(|@|at |=)?[ ]*0x(?P<a>[0-9a-fA-F]{5,7})\b')
GOOD = re.compile(r'(::|[a-z][A-Z]|^[A-Z][a-z]|_[a-z]|^[A-Z][A-Za-z0-9]+$)')
BAD = {'ore','Ids','conns','return','const','case','and','at','the','is','of','in','to','from','by','a','pass','offset','addr','mask','value','size','bit','bits','x','if','for'}
pairs = collections.defaultdict(lambda: {'srcs': []})
for o in c:
    for m in PAIR.finditer(o['text']):
        v = int(m.group('a'), 16)
        if not (0x1000 <= v < 0x2800000): continue
        nm = m.group('name')
        if nm in BAD or len(nm) < 4 or not GOOD.search(nm) or re.fullmatch(r'x[0-9a-f]+_\w+', nm): continue
        if re.fullmatch(r'0x[0-9a-fA-F]+', nm): continue
        k = (nm, '0x' + m.group('a').lower())
        pairs[k]['srcs'].append({'path': o['file'], 'line': o['line'], 'evidence': m.group(0)})
out = [{'name': k[0], 'addr': k[1], 'srcs': v['srcs'][:6], 'nsrc': len(v['srcs'])} for k, v in sorted(pairs.items(), key=lambda kv: int(kv[0][1], 16))]
json.dump(out, open(sys.argv[2], 'w'), indent=0)
byaddr = collections.defaultdict(set); byname = collections.defaultdict(set)
for p in out: byaddr[p['addr']].add(p['name']); byname[p['name']].add(p['addr'])
print(len(out), 'pairs;', len(byaddr), 'addrs;', len(byname), 'names')
print('addr with >1 name:', sum(len(v) > 1 for v in byaddr.values()), ' name with >1 addr:', sum(len(v) > 1 for v in byname.values()))
