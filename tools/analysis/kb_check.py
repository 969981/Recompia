#!/usr/bin/env python
"""Mechanical oracle for KB fragments: schema + evidence-in-source check.
usage: kb_check.py frag1.json [frag2.json ...]   (exit 0 = all PASS)
"""
import json, re, sys, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
HEX = re.compile(r'^0x[0-9a-f]+$')
cache = {}
def lines(path):
    if path not in cache:
        p = ROOT / path
        cache[path] = p.read_text(encoding='utf-8', errors='replace').splitlines() if p.is_file() else None
    return cache[path]

def check_src(src, errs, label, need=()):
    if not isinstance(src, dict): errs.append(f'{label}: src missing'); return
    L = lines(src.get('path', ''))
    if L is None: errs.append(f"{label}: file not found {src.get('path')}"); return
    ln, ev = src.get('line'), src.get('evidence')
    if not isinstance(ln, int) or not ev: errs.append(f'{label}: line/evidence missing'); return
    win = '\n'.join(L[max(0, ln - 4): ln + 3])
    if ev not in win and re.sub(r'\s+', ' ', ev) not in re.sub(r'\s+', ' ', win):
        errs.append(f'{label}: evidence not found near line {ln}: {ev[:60]!r}'); return
    evn = ev.lower()
    for k, v in need:
        if v and str(v).lower() not in evn and str(v).lower() not in win.lower() and not (str(v).startswith("0x") and str(v)[2:].lower() in win.lower()):
            errs.append(f'{label}: {k}={v} not in source text near line {ln}')

def check_item(it, errs, label):
    t = it.get('type')
    if t == 'symbol':
        for k in ('name', 'kind', 'addr_form', 'src'):
            if not it.get(k): errs.append(f'{label}: missing {k}')
        a = it.get('address')
        if a is not None and not HEX.match(str(a)): errs.append(f'{label}: bad address {a}')
        if a is not None and it.get('addr_form') == 'unknown': errs.append(f'{label}: address with addr_form unknown')
        if it.get('addr_form') not in ('main_offset', 'va', 'unknown'): errs.append(f'{label}: bad addr_form')
        check_src(it.get('src'), errs, label, need=(('address', a), ('mangled', it.get('mangled'))))
    elif t == 'struct':
        if not it.get('name'): errs.append(f'{label}: missing name')
        check_src(it.get('src'), errs, label)
        for i, f in enumerate(it.get('fields', [])):
            fl = f'{label}.fields[{i}]'
            if not HEX.match(str(f.get('offset', ''))): errs.append(f'{fl}: bad offset {f.get("offset")}')
            if not f.get('name') or not f.get('type'): errs.append(f'{fl}: name/type missing')
            if f.get('confidence') not in ('source-stated', 'inferred'): errs.append(f'{fl}: bad confidence')
            check_src(f.get('src'), errs, fl, need=(('offset', f.get('offset')),) if f.get('confidence') == 'source-stated' else ())
    elif t == 'enum':
        if not it.get('name') or not it.get('values'): errs.append(f'{label}: name/values missing')
        check_src(it.get('src'), errs, label)
    else:
        errs.append(f'{label}: unknown type {t}')

def main(argv):
    bad = 0
    for fn in argv:
        d = json.load(open(fn, encoding='utf-8'))
        errs = []
        items = d.get('items', [])
        for i, it in enumerate(items): check_item(it, errs, f'{pathlib.Path(fn).name}[{i}] {it.get("name")}')
        print(f'{"PASS" if not errs else "FAIL"} {fn}: {len(items)} items, {len(errs)} errors')
        for e in errs[:40]: print('   ', e)
        bad += bool(errs)
    return 1 if bad else 0
if __name__ == '__main__': sys.exit(main(sys.argv[1:]))
