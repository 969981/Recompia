#!/usr/bin/env python
"""Parse the community's MPR C++ headers into KB fragments (symbols, structs with field offsets, enums, vtable slots).

usage: kb_parse_headers.py OUT_DIR

Offsets: IDA-exported field names carry their offset (qword28, dwordE8, gap6C[52]) -> "source-stated".
Fields without such a name are laid out sequentially from the previous known position when every
size is known -> "inferred". GetField<T>(this, 0xNN) accessors -> "source-stated".
Nothing is invented: if a size is unknown the offset stays unknown until the next anchor.
"""
import re, sys, json, pathlib, collections

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCES = {
    'hdr_rando': ('refs/MP1R-Rando-CodeEdits/libs/game', ['**/*.h', '**/*.hpp']),
    'hdr_practice': ('refs/mp1r-practice-mod/src/prime', ['**/*.h', '**/*.hpp']),
    'hdr_exlaunch': ('refs/MP1R-Exlaunch-Base/src/CGame', ['**/*.h', '**/*.hpp']),
    'hdr_nnhid': ('refs/mp1r-practice-mod/src_framework/nn', ['hid.h', 'hid/*.h']),
}
PRIM = {'char': 1, 'uchar': 1, 'bool': 1, 'byte': 1, 'u8': 1, 's8': 1, 'int8_t': 1, 'uint8_t': 1, 'short': 2, 'ushort': 2,
        'u16': 2, 's16': 2, 'int16_t': 2, 'uint16_t': 2, 'int': 4, 'uint': 4, 'unsigned': 4, 'float': 4, 'u32': 4, 's32': 4,
        'int32_t': 4, 'uint32_t': 4, 'long': 8, 'ulong': 8, 'u64': 8, 's64': 8, 'int64_t': 8, 'uint64_t': 8, 'double': 8,
        'size_t': 8, 'CObjectId': 16, 'CGuid': 16, 'CVector3f': 12, 'CTransform4f': 48, 'CFourCC': 4, 'CAABox': 24,
        'CQuaternion': 16, 'CRelAngle': 4, 'CAxisAngle': 12}
COMPOSITE = {'CObjectId': 8, 'CGuid': 8, 'CVector3f': 4, 'CTransform4f': 4, 'CFourCC': 4, 'CAABox': 4, 'CQuaternion': 4, 'CRelAngle': 4, 'CAxisAngle': 4}
ANCHOR = re.compile(r'^[a-z]+(?:[A-Za-z_]*?)([0-9A-F]{1,5})$')
ANCHOR2 = re.compile(r'^(?:qword|dword|word|byte|gap|char|int|short|ptr|pchar|pint|unk|float|double|vtbl|p[a-z]+|[a-z]+)([0-9A-F]{1,7})(?:\[.*\])?$')


def strip_comments(lines):
    """return lines with // and /* */ removed (keeps line count) + dict line->preceding comment text"""
    out, comments, inblk = [], {}, False
    for i, l in enumerate(lines):
        s = l
        if inblk:
            if '*/' in s:
                s = s.split('*/', 1)[1]; inblk = False
            else:
                out.append(''); continue
        while '/*' in s:
            a, b = s.split('/*', 1)
            if '*/' in b: s = a + ' ' + b.split('*/', 1)[1]
            else: s = a; inblk = True; break
        if '//' in s:
            a, c = s.split('//', 1)
            comments[i] = c.strip()
            s = a
        out.append(s)
    return out, comments


def sizeof(t, known):
    t = re.sub(r'\b(const|volatile|struct|class|enum|unsigned|signed)\b', ' ', t).strip()
    if '*' in t or '&' in t: return 8, 8
    t0 = t.split('<')[0].strip().split('::')[-1]
    if t0 in PRIM: s = PRIM[t0]; return s, (min(s, 8) if t0 not in COMPOSITE else COMPOSITE[t0])
    if t0 in known and known[t0].get('size'): return known[t0]['size'], known[t0].get('align', 8)
    return None, None


def parse_enum_body(body_lines, start_line, path):
    vals, cur = [], -1
    text = ' '.join(body_lines)
    for part in text.split(','):
        part = part.strip()
        if not part: continue
        m = re.match(r'^(\w+)\s*(?:=\s*(.+))?$', part)
        if not m: continue
        n, v = m.group(1), m.group(2)
        if v:
            v = v.strip()
            mm = re.match(r'^(-?0x[0-9a-fA-F]+|-?\d+)$', v)
            if mm: cur = int(mm.group(1), 0); vals.append({'name': n, 'value': hex(cur) if cur >= 0 else str(cur)})
            else: vals.append({'name': n, 'value': v}); cur = None
        else:
            cur = (cur + 1) if isinstance(cur, int) else None
            vals.append({'name': n, 'value': hex(cur) if isinstance(cur, int) else None})
    return vals


def parse_file(path, relpath, known, out_items, mang_comments):
    raw = path.read_text(encoding='utf-8', errors='replace').splitlines()
    lines, comments = strip_comments(raw)
    n = len(lines)
    i = 0
    stack = []  # (kind, name, data)
    pending = None
    while i < n:
        l = lines[i]
        s = l.strip()
        if not s:
            i += 1; continue
        # namespace
        m = re.match(r'^namespace\s+(\w+)', s)
        if m and '{' in s and not stack_in_class(stack):
            stack.append(('ns', m.group(1), None)); i += 1; continue
        # enum
        m = re.match(r'^(?:typedef\s+)?enum\s+(?:class\s+|struct\s+)?(\w+)?\s*(?::\s*([\w:]+))?\s*(\{.*)?$', s)
        if m and not s.endswith(';') or (m and '{' in s):
            name, under, rest = m.group(1), m.group(2), m.group(3) or ''
            j, body = i, []
            text = s
            if '{' not in text:
                j += 1
                while j < n and '{' not in lines[j]: j += 1
                text = lines[j]
            after = text.split('{', 1)[1]
            while '}' not in after and j + 1 < n:
                body.append(after); j += 1; after = lines[j]
            body.append(after.split('}')[0])
            vals = parse_enum_body(body, i + 1, relpath) if name else []
            if name and vals:
                full = '::'.join([x[1] for x in stack if x[0] in ('class', 'ns')] + [name])
                out_items.append({'type': 'enum', 'name': full, 'underlying': under, 'values': vals,
                                  'src': {'path': relpath, 'line': i + 1, 'evidence': s[:100]}})
                known.setdefault(name, {'size': 4, 'align': 4})
            i = j + 1; continue
        # class / struct start
        m = re.match(r'^(?:template\s*<[^>]*>\s*)?(class|struct)\s+(\w+)\s*(?:final\s*)?(?::\s*([^{;]+?))?\s*(\{)?\s*$', s)
        if m and not s.endswith(';'):
            kind, name, bases, br = m.groups()
            bl = []
            if bases:
                for b in bases.split(','):
                    b = re.sub(r'\b(public|private|protected|virtual)\b', '', b).strip()
                    if b: bl.append(b.split('<')[0].strip())
            data = {'name': name, 'kind': kind, 'bases': bl, 'line': i + 1, 'ev': s[:100], 'access': 'public' if kind == 'struct' else 'private',
                    'fields': [], 'virt': [], 'methods': [], 'pos': None, 'maxalign': 1, 'vptr': False, 'anyvirt': False}
            data['qual'] = '::'.join([x[1] for x in stack if x[0] in ('class', 'ns')] + [name])
            stack.append(('class', name, data))
            pending = None if br else data
            i += 1; continue
        if pending is not None and s == '{':
            pending = None; i += 1; continue
        if s.startswith('}'):
            if stack:
                kind, name, data = stack.pop()
                if kind == 'class': finish_class(data, relpath, known, out_items)
            i += 1; continue
        # inside a class?
        if stack and stack[-1][0] == 'class':
            data = stack[-1][2]
            m = re.match(r'^(public|private|protected)\s*:', s)
            if m: data['access'] = m.group(1); i += 1; continue
            # gather statement until ';' or end of inline body
            stmt, startline = s, i
            depth = s.count('{') - s.count('}')
            j = i
            while (';' not in stmt.split('{')[0] and depth <= 0 and '{' not in stmt and not stmt.endswith('}')) and j + 1 < n and not stmt.endswith(';'):
                j += 1; stmt += ' ' + lines[j].strip()
                if ';' in stmt: break
            depth = stmt.count('{') - stmt.count('}')
            while depth > 0 and j + 1 < n:
                j += 1; stmt += ' ' + lines[j].strip(); depth = stmt.count('{') - stmt.count('}')
            # nested class/enum/struct are handled at the top of loop only when starting a line, so skip decls ending with ';' only
            handle_member(stmt, startline, relpath, data, known, out_items, comments, mang_comments, raw)
            i = j + 1; continue
        # free function declarations / globals at namespace scope
        if s.endswith(';') and not s.startswith(('using', 'typedef', 'template', '#', 'friend')):
            m = re.match(r'^(?:extern\s+)?([\w:<>\*&\s]+?)\s*[\*&]*\s*(\w+)\s*\(([^)]*)\)\s*;$', s)
            if m:
                out_items.append({'type': 'symbol', 'name': '::'.join([x[1] for x in stack if x[0] == 'ns'] + [m.group(2)]),
                                  'mangled': None, 'kind': 'function', 'address': None, 'addr_form': 'unknown', 'claimed_version': 'unspecified',
                                  'signature': s.rstrip(';'), 'role': 'declared in header; no address given', 'tags': [], 'used_by': 'declaration',
                                  'src': {'path': relpath, 'line': i + 1, 'evidence': s[:100]}})
            else:
                m = re.match(r'^extern\s+([\w:<>\*&\s]+?)\s*[\*&]*\s*(\w+)\s*;$', s)
                if m:
                    out_items.append({'type': 'symbol', 'name': m.group(2), 'mangled': None, 'kind': 'global', 'address': None, 'addr_form': 'unknown',
                                      'claimed_version': 'unspecified', 'signature': s.rstrip(';'), 'role': 'extern global declared in header', 'tags': [],
                                      'used_by': 'declaration', 'src': {'path': relpath, 'line': i + 1, 'evidence': s[:100]}})
        # brace tracking for unknown blocks (functions bodies at ns scope)
        if '{' in s and '}' not in s and not s.startswith('namespace'):
            depth = 1; i += 1
            while i < n and depth > 0:
                depth += lines[i].count('{') - lines[i].count('}'); i += 1
            continue
        i += 1


def stack_in_class(stack):
    return any(x[0] == 'class' for x in stack)


def strip_attrs(t):
    pk = 'packed' in t
    al = re.search(r'aligned\((\d+)\)', t)
    t = re.sub(r'__attribute__\(\(.*?\)\)', ' ', t)
    return t, pk, int(al.group(1)) if al else None


def handle_member(stmt, line0, relpath, data, known, out_items, comments, mang_comments, raw):
    s = stmt.strip()
    ev = raw[line0].strip()[:100]
    src = {'path': relpath, 'line': line0 + 1, 'evidence': ev}
    # mangled-name comments immediately above
    mangs = []
    k = line0 - 1
    while k >= 0 and raw[k].strip().startswith('//'):
        mm = re.search(r'(_ZN?K?\w+)', raw[k])
        if mm: mangs.append(mm.group(1))
        k -= 1
    if re.match(r'^(class|struct)\s+\w+\s*[\*&]', s):
        s = re.sub(r'^(class|struct)\s+', '', s)
    if s.startswith(('using', 'typedef', 'friend', 'static_assert', 'enum', 'class', 'struct', 'template')) and '(' not in s:
        return
    # GetField accessor
    mg = re.search(r'GetField<([\w:\s\*]+)>\s*\(\s*this\s*,\s*(0x[0-9a-fA-F]+)\s*\)', s)
    if mg:
        mn = re.search(r'(\w+)\s*\(\s*\)', s)
        nm = mn.group(1) if mn else 'field'
        deref = '*GetField' in s.replace(' ', '') and 'return*' in s.replace(' ', '')
        ty = mg.group(1).strip()
        data['fields'].append({'offset': mg.group(2).lower(), 'type': ty + ('*' if deref and False else ''), 'name': (nm[3:] if nm.startswith('Get') and len(nm) > 3 else nm) + '_acc', 'note': 'accessor in header reads this offset; class ' + data['name'],
                               'confidence': 'source-stated', 'src': {'path': relpath, 'line': line0 + 1, 'evidence': mg.group(0)}})
        return
    if '(' in s:
        # method
        s0 = re.sub(r'\b(inline|static|constexpr|explicit|ALWAYS_INLINE)\b', ' ', s)
        isvirt = bool(re.match(r'^\s*virtual\b', s0)) or bool(re.search(r'\boverride\b', s0))
        decl = s0.split('{')[0].strip().rstrip(';').strip()
        m = re.match(r'^(?:virtual\s+)?(?:(.+?)\s+)?[&*]*\s*(~?\w+(?:::\w+)?|operator\S+)\s*\((.*)\)\s*(const)?\s*(override|final)?\s*(=\s*0|=\s*default|=\s*delete)?\s*$', decl.replace('virtual ', '', 1) if decl.startswith('virtual') else decl)
        if not m: return
        ret, nm, args, cst, ovr, _ = m.groups()
        name = nm
        if name in (data['name'], '~' + data['name']) and not isvirt:
            data['methods'].append(('ctor/dtor', name, decl, src, mangs)); return
        rec = (('virt' if isvirt else 'method'), name, decl, src, mangs)
        if isvirt and not ovr or (isvirt and name.startswith('~')):
            data['virt'].append(rec)
        elif isvirt and ovr:
            data['virt_over'] = data.get('virt_over', []) + [rec]
        else:
            data['methods'].append(rec)
        if isvirt: data['anyvirt'] = True
        return
    # field
    fl, pk, al = strip_attrs(s)
    fl = fl.rstrip(';').strip()
    fl = re.sub(r'=\s*[^,]+$', '', fl).strip()
    fl = re.sub(r'^\s*(static|mutable)\s+', '', fl)
    if s.startswith('static'): return
    m = re.match(r'^(.+?)\s*([\*&]*)\s*(\w+)\s*(\[[^\]]*\])?\s*(?::\s*\d+)?$', fl)
    if not m: return
    ty, ptr, name, arr = m.group(1).strip(), m.group(2), m.group(3), m.group(4)
    ty = re.sub(r'\b(private|public|protected)\s*:', '', ty).strip()
    if not ty: return
    full_t = ty + ptr + (arr or '')
    sz, algn = sizeof(ty + ptr, known)
    cnt = 1
    if arr:
        a = arr.strip('[]').strip()
        try: cnt = int(a, 0)
        except ValueError: cnt = None
    anchor = None
    ma = ANCHOR2.match(name)
    if ma and re.match(r'^(qword|dword|word|byte|gap|char|int|short|ptr|pchar|pint|unk|pc\w+|p\w+|float|double|vtbl)', name):
        try: anchor = int(ma.group(1), 16)
        except ValueError: anchor = None
    data['fields'].append({'_raw': True, 'type': full_t, 'name': name, 'size': sz, 'align': 1 if pk else (al or algn), 'count': cnt,
                           'anchor': anchor, 'packed': pk, 'src': src})


def finish_class(data, relpath, known, out_items):
    # layout
    pos = None
    if data['bases']:
        b = known.get(data['bases'][0].split('::')[-1])
        if b and b.get('size'): pos = b['size']; data['maxalign'] = b.get('align', 8)
    else:
        pos = 8 if data['anyvirt'] else 0
        if data['anyvirt']: data['maxalign'] = 8
    if data['bases'] and pos is None:
        pos = None
    fields_out = [f for f in data['fields'] if not f.get('_raw')]
    exact_layout = True
    for f in data['fields']:
        if not f.get('_raw'): continue
        sz, algn, cnt = f['size'], f['align'], f['count']
        total = sz * cnt if (sz is not None and cnt is not None) else None
        drift = None
        if f['anchor'] is not None:
            off = f['anchor']
            conf = 'source-stated'
            drift = None
            if pos is not None and off != pos:
                a_ = algn or 1
                if off != (pos + a_ - 1) // a_ * a_: drift = hex((pos + a_ - 1) // a_ * a_)
            if f['name'].startswith('gap') and total is None:
                m = re.search(r'\[(\d+)\]', f['type'])
                total = int(m.group(1)) if m else None
            pos = off + total if total is not None else None
        elif pos is not None and sz is not None:
            a = algn or 1
            off = (pos + a - 1) // a * a
            conf = 'inferred'
            pos = off + total if total is not None else None
        else:
            off = None; conf = None; pos = None
        if off is None: continue
        note_ = '' if conf == 'source-stated' else 'laid out sequentially from the previous field/base; verify'
        if conf == 'source-stated' and drift: note_ = f'name implies {hex(off)} but sequential layout from the previous field predicts {drift}: header is internally inconsistent here'
        fields_out.append({'offset': hex(off), 'type': f['type'], 'name': f['name'], 'note': note_,
                           'confidence': conf, 'src': {**f['src'], 'evidence': f['src']['evidence']}})
        if algn: data['maxalign'] = max(data['maxalign'], algn)
    size = None
    has_anchor = any(f.get('_raw') and f['anchor'] is not None for f in data['fields'])
    if has_anchor and pos is not None and not any(f.get('_raw') and f['anchor'] is None and f['size'] is None for f in data['fields']):
        a = data['maxalign'] or 1
        size = (pos + a - 1) // a * a
    known[data['name']] = {'size': size, 'align': data['maxalign']}
    # vtable slots (own new virtuals only; slot numbering needs the base's count)
    vt = []
    base_slots = 0
    if data['bases']:
        base_slots = known.get('_vt_' + data['bases'][0].split('::')[-1], 0) if known.get('_vt_' + data['bases'][0].split('::')[-1]) is not None else None
    slot = base_slots
    virt_list = []
    for kind, name, decl, src, mangs in data['virt']:
        if name.startswith('~') and data['bases'] and (known.get('_vt_' + data['bases'][0].split('::')[-1]) or 0) > 0:
            virt_list.append({'slot': 0, 'name': name + ' (complete dtor, override)', 'decl': decl, 'src': src}); continue  # reuses the base's two dtor slots
        if name.startswith('~'):
            virt_list.append({'slot': slot, 'name': name + ' (complete dtor)' if slot is not None else name, 'decl': decl, 'src': src}); slot = slot + 2 if slot is not None else None
        else:
            virt_list.append({'slot': slot, 'name': name, 'decl': decl, 'src': src}); slot = slot + 1 if slot is not None else None
    known['_vt_' + data['name']] = slot if slot is not None else (None)
    if data['fields'] or virt_list or data['bases'] or data['methods']:
        item = {'type': 'struct', 'name': data['qual'], 'kind': data['kind'], 'bases': data['bases'], 'size': hex(size) if size else None,
                'fields': sorted(fields_out, key=lambda f: int(f['offset'], 16)),
                'virtuals': [{'slot': v['slot'], 'name': v['name'], 'signature': v['decl'], 'src': v['src'],
                              'slot_confidence': 'inferred from declaration order (Itanium ABI, dtor = 2 slots)'} for v in virt_list],
                'src': {'path': relpath, 'line': data['line'], 'evidence': data['ev']}}
        out_items.append(item)
    # symbols
    for kind, name, decl, src, mangs in data['methods'] + [('virt', v['name'], v['decl'], v['src'], []) for v in virt_list] + [(k, n, d, s, m) for (k, n, d, s, m) in data.get('virt_over', [])]:
        if name.startswith('~') and False: continue
        sym = {'type': 'symbol', 'name': f"{data['qual']}::{name}", 'mangled': mangs[0] if mangs else None,
               'kind': 'function', 'address': None, 'addr_form': 'unknown', 'claimed_version': 'unspecified', 'signature': decl,
               'role': 'method declared in header' + (' (virtual)' if kind == 'virt' else ' (virtual override)' if kind == 'virt' else ''),
               'tags': [], 'used_by': 'declaration', 'src': src}
        if len(mangs) > 1:
            sym['mangled'] = None
            sym['mangled_candidates'] = mangs
            sym['role'] += '; source comments list several mangled candidates (only one is real)'
        out_items.append(sym)


def main(outdir):
    outdir = pathlib.Path(outdir); outdir.mkdir(parents=True, exist_ok=True)
    known = {}
    summary = collections.Counter()
    for sid, (base, pats) in SOURCES.items():
        items, mc = [], {}
        files = sorted({p for pat in pats for p in (ROOT / base).glob(pat) if p.is_file()})
        # parse dependency order: files whose classes are bases first is not known; two passes to stabilise sizes
        for rep in range(2):
            items = []
            for p in files:
                rel = p.relative_to(ROOT).as_posix()
                try: parse_file(p, rel, known, items, mc)
                except Exception as e:
                    print('PARSE FAIL', rel, repr(e))
        # dedupe by (type,name,path,line)
        seen, ded = set(), []
        for it in items:
            k = (it['type'], it['name'], it['src']['path'], it['src']['line'])
            if k in seen: continue
            seen.add(k); ded.append(it)
        json.dump({'source_id': sid, 'items': ded}, open(outdir / f'{sid}.json', 'w', encoding='utf-8'), indent=1)
        c = collections.Counter(i['type'] for i in ded)
        print(sid, dict(c), 'fields', sum(len(i.get('fields', [])) for i in ded if i['type'] == 'struct'))
if __name__ == '__main__':
    main(sys.argv[1])
