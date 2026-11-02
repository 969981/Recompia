#!/usr/bin/env python
"""Merge docs/mpr/frag/*.json into docs/mpr/symbols.json, structs.json, enums.json, conflicts.json and structs.h.
usage: kb_merge.py [--frag DIR] [--out DIR]
Deterministic; rerun after any fragment change. Never invents data: every merged claim keeps its source list.
"""
import json, re, sys, pathlib, collections, argparse

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE_VA = 0x7100000000
TAGMAP = {'player': 'player_state'}


def norm_addr(a, form):
    v = int(a, 16)
    if form == 'va' or v >= BASE_VA: v -= BASE_VA
    return hex(v)


def bare(name):
    return re.sub(r'\s+', '', name.split('(')[0])


def merge_symbols(items, conflicts):
    groups = collections.OrderedDict()
    def gkey(it):
        if it.get('mangled'): return ('m', it['mangled'])
        if it.get('address'): return ('a', norm_addr(it['address'], it['addr_form']), bare(it['name']))
        return ('n', bare(it['name']), re.sub(r'\s+', '', it.get('signature') or ''))
    for it in items:
        groups.setdefault(gkey(it), []).append(it)
    # second pass: attach address-less groups to a unique address group of the same bare name
    by_name = collections.defaultdict(list)
    for k, v in groups.items():
        if k[0] in ('m', 'a'): by_name[bare(v[0]['name'])].append(k)
    out = []
    consumed = set()
    for k, v in list(groups.items()):
        if k[0] == 'n' and len(by_name.get(k[1], [])) == 1:
            groups[by_name[k[1]][0]].extend(v); consumed.add(k)
    # mangled groups that share an address group name: leave separate
    for k, v in groups.items():
        if k in consumed: continue
        first = v[0]
        ent = {'name': first['name'], 'mangled': None, 'kind': first['kind'], 'address': None, 'addr_form_original': None,
               'signature': None, 'role': [], 'tags': set(), 'used_by': [], 'claims': [], 'aliases': set(), 'contains': set(), 'mangled_candidates': set()}
        addrs = collections.OrderedDict()
        for it in v:
            if it.get('mangled') and not ent['mangled']: ent['mangled'] = it['mangled']
            if it.get('signature') and not ent['signature'] or (it.get('signature') and it.get('mangled')): ent['signature'] = it['signature']
            if it.get('role') and it['role'] not in ent['role'] and it['role'] != 'method declared in header' and not it['role'].startswith('declared in header'):
                ent['role'].append(it['role'])
            ent['tags'].update(it.get('tags', []))
            if it.get('used_by') and it['used_by'] not in ent['used_by'] and it['used_by'] != 'declaration': ent['used_by'].append(it['used_by'])
            if bare(it['name']) != bare(ent['name']): ent['aliases'].add(it['name'])
            if it['kind'] in ('function', 'global', 'vtable', 'data') and ent['kind'] == 'patch_site': ent['kind'] = it['kind']
            for c in it.get('contains', []): ent['contains'].add(c)
            for c in it.get('mangled_candidates', []): ent['mangled_candidates'].add(c)
            claim = {'source': it.get('_sid'), **it['src'], 'name': it['name']}
            if it.get('address'):
                a = norm_addr(it['address'], it['addr_form'])
                claim.update(address=a, addr_form=it['addr_form'], claimed_version=it['claimed_version'])
                addrs.setdefault(a, []).append(claim)
            if it.get('mangled'): claim['mangled'] = it['mangled']
            ent['claims'].append(claim)
        if len(addrs) == 1:
            ent['address'] = next(iter(addrs))
        elif len(addrs) > 1:
            conflicts.append({'kind': 'symbol_multiple_addresses', 'name': ent['name'], 'addresses': {a: [c['source'] + ':' + c['path'] + ':' + str(c['line']) for c in cs] for a, cs in addrs.items()}})
            ent['address'] = None
            ent['address_candidates'] = list(addrs)
        ent['tags'] = sorted(ent['tags']); ent['aliases'] = sorted(ent['aliases']); ent['contains'] = sorted(ent['contains']); ent['mangled_candidates'] = sorted(ent['mangled_candidates'])
        ent['role'] = ' | '.join(ent['role']) if ent['role'] else None
        out.append(ent)
    # same address, different bare names that are not aliases already merged -> conflict note
    byaddr = collections.defaultdict(list)
    for e in out:
        if e['address']: byaddr[e['address']].append(e)
    for a, es in byaddr.items():
        if len(es) > 1:
            conflicts.append({'kind': 'symbol_address_shared', 'address': a, 'names': [e['name'] for e in es]})
    for i, e in enumerate(sorted(out, key=lambda e: (e['address'] is None, int(e['address'], 16) if e['address'] else 0, e['name']))):
        e['id'] = i
    return sorted(out, key=lambda e: e['id'])


def merge_structs(items, conflicts):
    structs = collections.OrderedDict()
    for it in items:
        s = structs.setdefault(it['name'], {'name': it['name'], 'bases': [], 'size': None, 'sources': [], 'fields': collections.OrderedDict(), 'virtuals': []})
        for b in it.get('bases', []):
            if b not in s['bases']: s['bases'].append(b)
        if it.get('size') and not s['size']: s['size'] = it['size']
        elif it.get('size') and s['size'] != it['size']:
            conflicts.append({'kind': 'struct_size', 'struct': it['name'], 'sizes': [s['size'], it['size']], 'sources': [it['_sid']]})
        s['sources'].append({'source': it['_sid'], **it['src']})
        for f in it.get('fields', []):
            cand = {'name': f['name'], 'type': f['type'], 'confidence': f['confidence'], 'note': f.get('note', ''), 'source': it['_sid'], 'src': f['src']}
            s['fields'].setdefault(f['offset'], []).append(cand)
        for v in it.get('virtuals', []):
            s['virtuals'].append({**v, 'source': it['_sid']})
    out = []
    for name, s in structs.items():
        fl = []
        for off, cands in sorted(s['fields'].items(), key=lambda kv: int(kv[0], 16)):
            cands.sort(key=lambda c: (c['confidence'] != 'source-stated'))
            prim = cands[0]
            alts = [c for c in cands[1:]]
            kinds = {(c['name'].replace('via_', '').lower(), c['type'].replace(' ', '')) for c in cands}
            names = {c['name'] for c in cands}
            # a real conflict: different source-stated names at the same offset from different sources
            def nrm(n): return re.sub(r'^(via_|get|m(?=[A-Z]))', '', re.sub(r'^via_', '', n), flags=re.I).lower().replace('get', '', 1) if False else re.sub(r'^m(?=[A-Z])', '', re.sub(r'^via_(get)?', '', n, flags=re.I)).lower()
            def same(a, b): return a == b or a in b or b in a
            srcset = {c['source'] for c in cands}
            if len(srcset) > 1:
                ns = [(nrm(c['name']), c) for c in cands if not nrm(c['name']).startswith(('gap', 'qword', 'dword', 'byte', 'word', 'unk', 'pad'))]
                bad = any(not same(x[0], y[0]) and x[1]['source'] != y[1]['source'] for i, x in enumerate(ns) for y in ns[i + 1:])
                if bad:
                    conflicts.append({'kind': 'struct_field_name', 'struct': name, 'offset': off,
                                      'candidates': [{'name': c['name'], 'type': c['type'], 'confidence': c['confidence'], 'source': c['source'], 'note': c['note']} for c in cands]})
            fl.append({'offset': off, 'name': prim['name'], 'type': prim['type'], 'confidence': prim['confidence'], 'note': prim['note'], 'src': prim['src'], 'source': prim['source'],
                       'alternatives': [{'name': c['name'], 'type': c['type'], 'confidence': c['confidence'], 'source': c['source'], 'note': c['note']} for c in alts]})
        # same field name at two different offsets (sources disagree)
        nm = collections.defaultdict(set)
        for f in fl:
            if not f['name'].startswith(('gap', 'via_', 'qword', 'dword', 'byte', 'word', 'unk', 'pad')): nm[f['name']].add(f['offset'])
        for n, offs in nm.items():
            if len(offs) > 1: conflicts.append({'kind': 'struct_field_offsets', 'struct': name, 'field': n, 'offsets': sorted(offs)})
        # overlap check among source-stated fields
        out.append({'name': name, 'bases': s['bases'], 'size': s['size'], 'sources': s['sources'], 'fields': fl,
                    'virtuals': s['virtuals']})
    return out


def merge_enums(items, conflicts):
    en = collections.OrderedDict()
    for it in items:
        e = en.setdefault(it['name'], {'name': it['name'], 'underlying': it.get('underlying'), 'values': collections.OrderedDict(), 'sources': []})
        e['sources'].append({'source': it['_sid'], **it['src']})
        for v in it['values']:
            old = e['values'].get(v['name'])
            if old is not None and old['value'] != v['value'] and old['value'] is not None and v['value'] is not None:
                conflicts.append({'kind': 'enum_value', 'enum': it['name'], 'name': v['name'], 'values': [old['value'], v['value']]})
            if old is None: e['values'][v['name']] = {'value': v['value'], 'source': it['_sid']}
    return [{**e, 'values': [{'name': k, **v} for k, v in e['values'].items()]} for e in en.values()]


CT = {'char': 'char', 'uchar': 'unsigned char', 'bool': 'unsigned char', 'byte': 'unsigned char', 'u8': 'unsigned char', 's8': 'signed char', 'short': 'short',
      'ushort': 'unsigned short', 'u16': 'unsigned short', 's16': 'short', 'int': 'int', 'uint': 'unsigned int', 'unsigned': 'unsigned int', 'float': 'float',
      'u32': 'unsigned int', 's32': 'int', 'long': 'long long', 'u64': 'unsigned long long', 's64': 'long long', 'double': 'double', 'size_t': 'unsigned long long',
      'int8_t': 'signed char', 'uint8_t': 'unsigned char', 'int16_t': 'short', 'uint16_t': 'unsigned short', 'int32_t': 'int', 'uint32_t': 'unsigned int',
      'int64_t': 'long long', 'uint64_t': 'unsigned long long'}
SZ = {'char': 1, 'unsigned char': 1, 'signed char': 1, 'short': 2, 'unsigned short': 2, 'int': 4, 'unsigned int': 4, 'float': 4, 'long long': 8,
      'unsigned long long': 8, 'double': 8, 'void*': 8}
KNOWN_SIZES = {'CObjectId': 16, 'CGuid': 16, 'CVector3f': 12, 'CTransform4f': 48, 'CFourCC': 4, 'CAABox': 24, 'CQuaternion': 16}


def cname(n): return re.sub(r'\W', '_', n)


def emit_header(structs, enums, outpath):
    byname = {s['name']: s for s in structs}
    L = ['// GENERATED by tools/analysis/kb_merge.py from docs/mpr/frag/*.json. Do not edit.',
         '// Offsets are claims from community sources (see structs.json for provenance and conflicts), not verified against the binary unless validate_kb says so.',
         '// Flattened layout: every struct lists its base-class fields at absolute offsets; unknown bytes are padding.', '']
    for e in enums:
        vs = [v for v in e['values'] if v['value'] is not None and re.fullmatch(r'-?(0x[0-9a-fA-F]+|\d+)', str(v['value']))]
        if not vs or e['name'].count('::') > 2: continue
        L.append(f'enum {cname(e["name"])} {{ ' + ', '.join(f'{cname(e["name"])}_{v["name"]} = {int(v["value"], 0)}' for v in vs) + ' };')
    L.append('')
    for s in structs: L.append(f'struct {cname(s["name"])};')
    L.append('')
    def flat(name, seen=()):
        s = byname.get(name)
        if not s or name in seen: return {}
        fields = {}
        for b in s['bases']:
            fields.update(flat(b.split('::')[-1] if b.split('::')[-1] in byname else b, seen + (name,)))
        for f in s['fields']:
            fields[int(f['offset'], 16)] = f
        return fields
    def ctype(t):
        t0 = re.sub(r'\b(const|volatile|struct|class|enum)\b', ' ', t).strip()
        arr = re.search(r'\[(\d+)\]$', t0)
        n = None
        if arr: t0 = t0[:arr.start()].strip(); n = int(arr.group(1))
        ptr = t0.endswith('*') or '&' in t0
        base = t0.rstrip('*& ').strip()
        if ptr:
            tn = base.split('<')[0].strip()
            return ('struct ' + cname(tn) + ' *' if tn in byname else 'void *'), 8, n
        if base in CT: return CT[base], SZ[CT[base]], n
        if base in KNOWN_SIZES: return None, KNOWN_SIZES[base], n  # opaque value type -> bytes
        return None, None, n
    for s in structs:
        fl = flat(s['name'])
        if not fl and not s['size']:
            L.append(f'struct {cname(s["name"])} {{ char _opaque[1]; }}; // no layout known'); continue
        L.append(f'#pragma pack(push, 1)')
        L.append(f'struct {cname(s["name"])} {{' + (f'  // size {s["size"]} (inferred)' if s['size'] else ''))
        pos = 0
        for off in sorted(fl):
            f = fl[off]
            t, sz, n = ctype(f['type'])
            if sz is None: L.append(f'  // 0x{off:x} {f["type"]} {f["name"]}: size unknown, skipped'); continue
            tot = sz * (n or 1)
            if off < pos: L.append(f'  // 0x{off:x} {f["type"]} {f["name"]}: overlaps previous field (header inconsistency), skipped'); continue
            if off > pos: L.append(f'  unsigned char _pad_{pos:x}[0x{off - pos:x}];')
            nm = re.sub(r'\W', '_', f['name'])
            if t: L.append(f'  {t} {nm}' + (f'[{n}]' if n else '') + f'; // 0x{off:x} {f["confidence"]}')
            else: L.append(f'  unsigned char {nm}[{tot}]; // 0x{off:x} {f["type"]} {f["confidence"]}')
            pos = off + tot
        if s['size'] and int(s['size'], 16) > pos: L.append(f'  unsigned char _tail_{pos:x}[0x{int(s["size"], 16) - pos:x}];')
        L.append('};'); L.append('#pragma pack(pop)')
    pathlib.Path(outpath).write_text('\n'.join(L) + '\n', encoding='utf-8')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frag', default=str(ROOT / 'docs/mpr/frag'))
    ap.add_argument('--out', default=str(ROOT / 'docs/mpr'))
    a = ap.parse_args()
    items = {'symbol': [], 'struct': [], 'enum': []}
    srcs = []
    for p in sorted(pathlib.Path(a.frag).glob('*.json')):
        d = json.load(open(p, encoding='utf-8'))
        srcs.append(d['source_id'])
        for it in d['items']:
            it['_sid'] = d['source_id']
            items[it['type']].append(it)
    conflicts = []
    syms = merge_symbols(items['symbol'], conflicts)
    structs = merge_structs(items['struct'], conflicts)
    enums = merge_enums(items['enum'], conflicts)
    out = pathlib.Path(a.out)
    rp = out / 'resolutions.json'
    if rp.exists():
        for r in json.load(open(rp, encoding='utf-8'))['resolutions']:
            for c in conflicts:
                if c['kind'] == r['kind'] and c.get('struct') == r.get('struct') and (r.get('offset') is None or c.get('offset') == r['offset']):
                    c['resolution'] = {k: v for k, v in r.items() if k not in ('kind', 'struct', 'offset')}
    json.dump({'meta': {'title': 'MPR 1.0.0 known symbols', 'build_id': '67C9CA1428222752F951021464A7B3597EB98FEC', 'title_id': '010012101468C000',
                        'address_note': 'address = offset from start of main .text (== ELF vaddr of nx2elf main.elf); VA in IDA = image base + address',
                        'sources': srcs, 'count': len(syms)}, 'symbols': syms}, open(out / 'symbols.json', 'w', encoding='utf-8'), indent=1)
    json.dump({'meta': {'sources': srcs}, 'structs': structs}, open(out / 'structs.json', 'w', encoding='utf-8'), indent=1)
    json.dump({'enums': enums}, open(out / 'enums.json', 'w', encoding='utf-8'), indent=1)
    json.dump({'conflicts': conflicts}, open(out / 'conflicts.json', 'w', encoding='utf-8'), indent=1)
    emit_header(structs, enums, out / 'structs.h')
    print(f'symbols {len(syms)} (with address {sum(1 for s in syms if s["address"])}, mangled {sum(1 for s in syms if s["mangled"])}), structs {len(structs)}, '
          f'fields {sum(len(s["fields"]) for s in structs)}, enums {len(enums)}, conflicts {len(conflicts)}')
    print(collections.Counter(c['kind'] for c in conflicts))
if __name__ == '__main__': main()
