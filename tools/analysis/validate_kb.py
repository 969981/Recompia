#!/usr/bin/env python
"""Validate the MPR knowledge base (docs/mpr/*.json) against the real main binary.

  validate_kb.py --elf work/elf/main.elf                 # check everything, print PASS/FAIL/WARN per entry + summary
  validate_kb.py --elf main.elf --write-back             # also store a `verification` block in symbols.json / patches.json
  validate_kb.py --elf main.elf --quiet --report out.json

ELF = nx2elf output of main (or any AArch64 ELF with .dynsym/.symtab). Addresses in the KB are main-relative offsets; the ELF's
lowest PT_LOAD vaddr is taken as the image base (0 for nx2elf, 0x7100000000 if rebased), override with --base.

Statuses:  PASS  claim confirmed by the binary
           FAIL  claim contradicted (wrong address / bytes / name missing for a hooked symbol)
           WARN  cannot be confirmed (stripped, internal, inlined) but not contradicted
Exit code = 1 if any FAIL.  Needs pyelftools + capstone (tools/.venv) and llvm-cxxfilt (LLVM bin) for demangling.
"""
import argparse, json, os, pathlib, re, shutil, struct, subprocess, sys, collections
from elftools.elf.elffile import ELFFile
import capstone

ROOT = pathlib.Path(__file__).resolve().parents[2]
CXXFILT = shutil.which('llvm-cxxfilt') or r'C:\Program Files\LLVM\bin\llvm-cxxfilt.exe'


def split_top(s, open_='(', ):
    """return (base, params) of a demangled C++ signature; params None if there is no '(' at template depth 0"""
    depth = 0
    for i, ch in enumerate(s):
        if ch == '<': depth += 1
        elif ch == '>': depth -= 1
        elif ch == '(' and depth == 0 and not s.startswith('(anonymous', i):
            d2, j = 1, i + 1
            while j < len(s) and d2:
                if s[j] == '(': d2 += 1
                elif s[j] == ')': d2 -= 1
                j += 1
            return s[:i].strip(), s[i + 1:j - 1]
    return s.strip(), None


def count_params(p):
    if p is None: return None
    p = p.strip()
    if not p or p == 'void': return 0
    d, n = 0, 1
    for ch in p:
        if ch in '<(': d += 1
        elif ch in '>)': d -= 1
        elif ch == ',' and d == 0: n += 1
    return n


class Binary:
    def __init__(self, path, base=None):
        self.f = ELFFile(open(path, 'rb'))
        self.path = path
        self.loads = [s for s in self.f.iter_segments() if s['p_type'] == 'PT_LOAD']
        self.base = min(s['p_vaddr'] for s in self.loads) if base is None else base
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.md.detail = True
        self.syms = {}      # mangled -> (value, size)
        self.imports = set()  # undefined (imported from sdk/subsdk/rtld) symbol names
        for secname in ('.dynsym', '.symtab'):
            sec = self.f.get_section_by_name(secname)
            if not sec: continue
            for s in sec.iter_symbols():
                if s['st_shndx'] != 'SHN_UNDEF' and s.name and s['st_value']:
                    self.syms.setdefault(s.name, (s['st_value'], s['st_size']))
                elif s['st_shndx'] == 'SHN_UNDEF' and s.name:
                    self.imports.add(s.name)
        self.extra = {}     # name -> file label, for other modules (sdk.elf ...)
        names = list(self.syms)
        self.dem = dict(zip(names, self._demangle(names)))
        self.by_base = collections.defaultdict(list)
        for m in names:
            b, p = split_top(self.dem[m])
            self.by_base[b].append((m, self.syms[m][0], self.syms[m][1], count_params(p)))
        self.by_addr = collections.defaultdict(list)
        for m, (v, sz) in self.syms.items(): self.by_addr[v].append(m)
        fn = sorted(((v, sz, m) for m, (v, sz) in self.syms.items() if sz and not m.startswith('_ZT')), key=lambda t: t[0])
        self._fn_starts = [t[0] for t in fn]; self._fn = fn
        self._rela = None

    def add_extra(self, path):
        try:
            f = ELFFile(open(path, 'rb'))
            for secname in ('.dynsym', '.symtab'):
                sec = f.get_section_by_name(secname)
                if sec:
                    for s in sec.iter_symbols():
                        if s['st_shndx'] != 'SHN_UNDEF' and s.name: self.extra.setdefault(s.name, os.path.basename(path))
        except Exception as e:
            print('WARN: cannot read', path, e, file=sys.stderr)

    def _demangle(self, names):
        try:
            r = subprocess.run([CXXFILT], input='\n'.join(names), capture_output=True, text=True, check=True)
            out = r.stdout.split('\n')
            if len(out) >= len(names): return out[:len(names)]
        except Exception as e:
            print('WARN: llvm-cxxfilt failed (%s); names will not be demangled' % e, file=sys.stderr)
        return list(names)

    def va(self, off): return self.base + off

    def seg_of(self, va):
        for s in self.loads:
            if s['p_vaddr'] <= va < s['p_vaddr'] + s['p_memsz']: return s
        return None

    def is_exec(self, va):
        s = self.seg_of(va)
        return bool(s and s['p_flags'] & 1)

    def read(self, va, n):
        s = self.seg_of(va)
        if not s: return None
        o = va - s['p_vaddr']
        if o + n > s['p_filesz']:  # bss tail
            if o >= s['p_filesz']: return b'\0' * n if o + n <= s['p_memsz'] else None
            return None
        s.stream.seek(s['p_offset'] + o)
        return s.stream.read(n)

    def disasm(self, va, n=1):
        b = self.read(va, 4 * n)
        if not b: return []
        return list(self.md.disasm(b, va))

    def symbol_containing(self, va):
        import bisect
        i = bisect.bisect_right(self._fn_starts, va) - 1
        while i >= 0:
            v, sz, m = self._fn[i]
            if v <= va < v + sz: return m
            if va - v > 0x100000: break
            i -= 1
        return None

    def rela(self):
        if self._rela is None:
            self._rela = {}
            for secname in ('.rela.dyn', '.rela.plt'):
                sec = self.f.get_section_by_name(secname)
                if not sec: continue
                for r in sec.iter_relocations():
                    t = r['r_info_type']
                    if t == 1027: self._rela[r['r_offset']] = r['r_addend']           # R_AARCH64_RELATIVE
                    elif t in (257, 1025, 1026):                                      # ABS64 / GLOB_DAT / JUMP_SLOT
                        symtab = self.f.get_section(sec['sh_link'])
                        s = symtab.get_symbol(r['r_info_sym']) if symtab else None
                        if s is not None and s['st_value']: self._rela[r['r_offset']] = s['st_value'] + r['r_addend']
        return self._rela

    def ptr_at(self, va):
        r = self.rela().get(va)
        if r is not None: return r
        b = self.read(va, 8)
        return struct.unpack('<Q', b)[0] if b else None


import difflib
def is_label(name):
    return (' ' in name) or ('_at_0x' in name) or ('::' not in name and len(name) <= 6 and name.isupper())


def similar(a, b):
    ca = split_top(a)[0]; cb = split_top(b)[0]
    if cb.startswith(ca + '::'): return True   # source names only the class
    a = re.split(r'::', split_top(a)[0])[-1].lower(); b = re.split(r'::', split_top(b)[0])[-1].lower()
    return a == b or a in b or b in a or difflib.SequenceMatcher(None, a, b).ratio() >= 0.7


def norm_asm(t):
    t = t.lower().replace('#', '').replace(',', ' ')
    t = re.sub(r'0x0+([0-9a-f])', r'0x\1', t)
    return ' '.join(t.split())


def ev_hex(a): return int(a, 16)


class Report:
    def __init__(self, quiet): self.rows, self.quiet, self.c = [], quiet, collections.Counter()
    def add(self, status, cat, name, addr, msg):
        self.rows.append((status, cat, name, addr, msg)); self.c[(cat, status)] += 1
        if not self.quiet or status == 'FAIL':
            print(f'{status:4} {cat:7} {name[:70]:70} {addr or "-":>12}  {msg}')


def check_symbols(B, kb, rep):
    for e in kb['symbols']:
        name, kind = e['name'], e['kind']
        addr = ev_hex(e['address']) if e.get('address') else None
        mang = e.get('mangled')
        cands_m = ([mang] if mang else []) + list(e.get('mangled_candidates') or [])
        hooked = bool(mang) or any(c.get('mangled') for c in e['claims'])
        found, method, v = None, None, None
        # 1. mangled names (a mod hooked these, so they must exist)
        hit = next((m for m in cands_m if m in B.syms), None)
        if hit:
            found = (hit,) + B.syms[hit]
            method = 'mangled' if hit == mang else 'mangled (one of several candidates in the source)'
        elif any(m in B.imports or m in B.extra for m in cands_m):
            m = next(m for m in cands_m if m in B.imports or m in B.extra)
            v = {'status': 'PASS', 'method': 'import', 'binary_mangled': m,
                 'note': 'imported by main' + (f', defined in {B.extra[m]}' if m in B.extra else '')}
        elif cands_m:
            alt = B.by_base.get(split_top(name)[0], [])
            v = {'status': 'FAIL', 'note': 'mangled name not in symbol table' + ('; binary has ' + ', '.join(c[0] for c in alt[:2]) if alt else ''),
                 'suggest': [c[0] for c in alt[:5]]}
        # 2. plain C names (nvnBootstrapLoader ...)
        if v is None and found is None and not cands_m and '::' not in name and (name in B.syms or name in B.imports or name in B.extra):
            if name in B.syms:
                found = (name,) + B.syms[name]; method = 'plain name'
            else:
                v = {'status': 'PASS', 'method': 'import', 'note': 'imported by main' + (f', defined in {B.extra[name]}' if name in B.extra else '')}
        # 3. qualified name via the demangled index
        if v is None and found is None and not cands_m and '::' in name and not is_label(name):
            base = split_top(name)[0]
            cands = B.by_base.get(base, [])
            if len(cands) == 1:
                found = cands[0][:3]; method = 'name'
            elif len(cands) > 1:
                pick = [c for c in cands if addr is not None and c[1] == B.va(addr)]
                if pick:
                    found = pick[0][:3]; method = 'name+address'
                else:
                    n = count_params(split_top(e.get('signature') or '')[1]) if e.get('signature') else None
                    pick = [c for c in cands if n is not None and c[3] == n]
                    if len(pick) == 1:
                        found = pick[0][:3]; method = 'name+arity'
                    else:
                        v = {'status': 'WARN', 'note': f'{len(cands)} overloads in binary, cannot pick one', 'candidates': [c[0] for c in cands[:6]]}
        if found:
            m, val, sz = found
            off = val - B.base
            v = {'status': 'PASS', 'method': method, 'binary_mangled': m, 'binary_demangled': B.dem.get(m, m), 'binary_address': hex(off), 'binary_size': sz}
            if addr is not None and off != addr:
                lo = B.va(addr)
                if val <= lo < val + max(sz, 1):
                    v.update(status='WARN', note=f'claimed address is inside {B.dem.get(m, m)} (+{hex(addr - off)}), not its entry {hex(off)}')
                else:
                    v = {'status': 'FAIL', 'note': f'claimed address {hex(addr)} but binary has {B.dem.get(m, m)} at {hex(off)}',
                         'binary_mangled': m, 'binary_address': hex(off)}
        elif v is None and addr is not None:
            va = B.va(addr)
            if B.seg_of(va) is None:
                v = {'status': 'FAIL', 'note': 'address outside every segment'}
            elif kind in ('function', 'patch_site') and not B.is_exec(va):
                v = {'status': 'FAIL', 'note': 'address is not in an executable segment'}
            elif kind in ('function', 'patch_site'):
                ins = B.disasm(va, 1)
                here = B.by_addr.get(va, [])
                cont = B.symbol_containing(va)
                if not ins:
                    v = {'status': 'FAIL', 'note': 'address does not decode as an AArch64 instruction'}
                elif here:
                    dem = B.dem.get(here[0], here[0])
                    info = {'binary_mangled': here[0], 'binary_demangled': dem, 'binary_address': hex(addr), 'binary_size': B.syms[here[0]][1]}
                    if is_label(name): v = {'status': 'WARN', 'method': 'address', 'note': f'source name is a label; binary function here is {dem}', **info}
                    elif similar(name, dem): v = {'status': 'PASS', 'method': 'address', **info}
                    else: v = {'status': 'FAIL', 'note': f'binary function at this address is {dem}, source calls it {name}', **info}
                elif cont:
                    cd = B.dem.get(cont, cont)
                    v = {'status': 'WARN', 'note': f'mid-function: inside {cd} (+{hex(va - B.syms[cont][0])}); insn {ins[0].mnemonic} {ins[0].op_str}',
                         'binary_mangled': cont, 'binary_demangled': cd}
                else:
                    v = {'status': 'WARN', 'note': f'no symbol here (stripped/internal); decodes as {ins[0].mnemonic} {ins[0].op_str}'}
            else:
                here = B.by_addr.get(va, [])
                same = [h for h in here if similar(name, B.dem.get(h, h))]
                if same:
                    v = {'status': 'PASS', 'method': 'address', 'binary_mangled': same[0], 'binary_demangled': B.dem.get(same[0], same[0]), 'binary_address': hex(addr)}
                elif here:
                    v = {'status': 'WARN', 'note': f'data symbol here is {B.dem.get(here[0], here[0])}', 'binary_mangled': here[0]}
                else:
                    other = B.by_base.get(split_top(name)[0], []) if '::' in name else []
                    if other and not is_label(name):
                        v = {'status': 'FAIL', 'note': f'binary has {B.dem.get(other[0][0], other[0][0])} at {hex(other[0][1] - B.base)}, not {hex(addr)}',
                             'binary_mangled': other[0][0], 'binary_address': hex(other[0][1] - B.base)}
                    else:
                        v = {'status': 'WARN', 'note': 'data address inside a segment; no symbol, contents not checked'}
        if v is None:
            v = {'status': 'FAIL' if hooked else 'WARN', 'note': 'no address and name not found in symbol tables (inlined, internal, or renamed)'}
        if v['status'] == 'PASS' and e.get('contains') and found:
            m, val, sz = found
            bad = [c for c in e['contains'] if not (val <= B.va(ev_hex(c)) < val + sz)]
            if bad: v = {**v, 'status': 'FAIL', 'note': f'claimed sites outside the function: {bad[:4]}'}
            else: v['note'] = f'{len(e["contains"])} claimed patch sites all inside the function'
        e['verification'] = v
        e['verified'] = {'PASS': 'verified', 'FAIL': 'contradicted'}.get(v['status'], 'unverified')
        e['address_binary'] = v.get('binary_address') if v['status'] in ('PASS', 'FAIL') or v.get('binary_address') else None
        e['mangled_binary'] = v.get('binary_mangled')
        rep.add(v['status'], 'symbol', name, e.get('address'), v.get('note') or v.get('method', ''))


def check_patches(B, pk, rep):
    for st in pk['sets']:
        for en in st['entries']:
            off = ev_hex(en['offset'])
            va = B.va(off)
            hexb = en.get('bytes')
            nm = f'{st["id"]} {en["offset"]}'
            v = None
            if not hexb or not re.fullmatch(r'[0-9A-Fa-f]+', hexb):
                v = {'status': 'WARN', 'note': 'no new bytes'}
            else:
                new = bytes.fromhex(hexb)
                seg = B.seg_of(va)
                if seg is None or B.read(va, len(new)) is None:
                    v = {'status': 'FAIL', 'note': 'offset outside the binary image'}
                elif not B.is_exec(va):
                    v = {'status': 'PASS', 'note': f'data-side write inside segment ({len(new)} bytes)'}
                else:
                    orig = B.read(va, len(new))
                    notes, bad, warn = [], False, False
                    if len(new) == 4 and va % 4: bad = True; notes.append('unaligned instruction patch')
                    oi = B.disasm(va, 1)
                    ni = list(B.md.disasm(new[:4], va)) if len(new) >= 4 else []
                    if en.get('orig_bytes') and bytes.fromhex(en['orig_bytes']) != orig[:len(en['orig_bytes']) // 2]:
                        bad = True; notes.append(f'binary has {orig.hex()} but source says original {en["orig_bytes"]}')
                    com = en.get('comment') or ''
                    parts = [p.strip() for p in com.split('|')]
                    if len(parts) >= 3 and oi:   # cucholix style: original | new | what
                        o_txt = norm_asm(f'{oi[0].mnemonic} {oi[0].op_str}')
                        if parts[0].split()[0] != oi[0].mnemonic or (parts[0].split()[0] not in ('bl', 'b', 'cbz') and norm_asm(parts[0]) != o_txt):
                            warn = True; notes.append(f'source comment says original "{parts[0]}" but binary has "{oi[0].mnemonic} {oi[0].op_str}" (stale source comment)')
                    if not oi: notes.append('original bytes do not decode')
                    if ni:
                        # does the new instruction match its own comment? (catches transcription typos)
                        m = re.match(r'\s*([a-z]+[a-z0-9.]*)\s+([^/|]*?)\s*(?://|\||$)', com)
                        if m and m.group(1) in ('mov', 'movk', 'movz', 'fmov', 'ldr', 'fmul', 'ret', 'nop', 'sub', 'add'):
                            if norm_asm(f'{ni[0].mnemonic} {ni[0].op_str}') != norm_asm(f'{m.group(1)} {m.group(2)}'):
                                notes.append(f'new bytes decode as "{ni[0].mnemonic} {ni[0].op_str}" but comment says "{m.group(1)} {m.group(2)}" (stale comment or typo)')
                    # call-site claims: "GetScreenWidth <Class::Func>" must be a bl to that function, inside that function
                    mm = re.search(r'(GetScreenWidth|GetScreenHeight)\s+(\w+::\w+)\s*$', com)
                    if mm and oi:
                        if oi[0].mnemonic == 'bl':
                            tgt = int(oi[0].op_str.lstrip('#'), 16)
                            ts = B.by_addr.get(tgt, [])
                            if not any(mm.group(1) in B.dem[t] for t in ts): bad = True; notes.append(f'bl target {hex(tgt - B.base)} is not {mm.group(1)}')
                            else: notes.append(f'bl -> {B.dem[ts[0]]}')
                        else: bad = True; notes.append(f'site is "{oi[0].mnemonic}", not a bl to {mm.group(1)}')
                        cont = B.symbol_containing(va)
                        if cont and mm.group(2) not in B.dem[cont]: bad = True; notes.append(f'site is inside {B.dem[cont]}, source says {mm.group(2)}')
                        elif cont: notes.append(f'inside {B.dem[cont]}')
                    v = {'status': 'FAIL' if bad else ('WARN' if (warn or any('stale' in n for n in notes)) else 'PASS'), 'note': '; '.join(notes) or (f'orig {oi[0].mnemonic} {oi[0].op_str}' if oi else '')}
                    if oi: v['binary_orig'] = f'{oi[0].mnemonic} {oi[0].op_str}'
                    cont = B.symbol_containing(va)
                    if cont: v['in_function'] = B.dem[cont]
            en['verification'] = v
            rep.add(v['status'], 'patch', nm, en['offset'], v.get('note', ''))


def check_structs(B, sk, rep, symbols):
    """vtable slot order + field-offset corroboration"""
    # class -> function symbols
    for s in sk['structs']:
        cls = s['name']
        vt = B.syms.get('_ZTV%d%s' % (len(cls), cls)) if '::' not in cls else None
        if vt and vt[1] > 16:
            slots = []
            for i in range((vt[1] - 16) // 8):
                p_ = B.ptr_at(vt[0] + 16 + 8 * i)
                nm_ = B.by_addr.get(p_, []) if p_ else []
                slots.append({'slot': i, 'target': hex(p_ - B.base) if p_ else None, 'name': B.dem.get(nm_[0], nm_[0]) if nm_ else None})
            s['vtable_binary'] = {'address': hex(vt[0] - B.base), 'size': vt[1], 'slots': slots}
        if s.get('virtuals') and vt:
            ok = bad = unk = 0
            detail = []
            for vtl in s['virtuals']:
                if vtl.get('slot') is None: unk += 1; continue
                p = B.ptr_at(vt[0] + 16 + 8 * vtl['slot'])
                nm = vtl['name'].split(' ')[0]
                if not p: unk += 1; continue
                syms = B.by_addr.get(p, [])
                dems = [B.dem[x] for x in syms]
                if any(d.startswith('__cxa_pure_virtual') for d in dems): unk += 1; continue
                hit = any(('::' + nm + '(') in d or d.startswith(nm + '(') for d in dems) or (nm.startswith('~') and any('::~' in d for d in dems))
                if hit: ok += 1
                elif dems: bad += 1; detail.append(f'slot {vtl["slot"]} {nm} -> {dems[0][:60]}')
                else: unk += 1
            st = 'PASS' if ok and not bad else 'FAIL' if bad else 'WARN'
            s['verification_vtable'] = {'status': st, 'matched': ok, 'mismatched': bad, 'unresolved': unk, 'detail': detail[:6]}
            rep.add(st, 'vtable', cls, hex(vt[0] - B.base), f'{ok} slots match, {bad} mismatch, {unk} unresolved' + (' | ' + '; '.join(detail[:2]) if detail else ''))
        # field corroboration: any instruction in the class's functions using [reg,#off]
        funcs = [(m, v, sz) for m, (v, sz) in B.syms.items() if sz and B.dem[m].startswith(cls + '::') and sz < 0x4000][:400]
        if not funcs: continue
        targets = {}
        for f in s['fields']:
            off = int(f['offset'], 16)
            if f['confidence'] == 'source-stated' and 0x40 <= off <= 0x7ff8 and not f['name'].startswith(('gap', 'pad')): targets[off] = f
        if not targets: continue
        hits = collections.defaultdict(set)
        for m, v, sz in funcs:
            b = B.read(v, sz)
            if not b: continue
            for ins in B.md.disasm(b, v):
                if ins.mnemonic in ('ldr', 'str', 'ldrb', 'strb', 'ldrh', 'strh', 'ldrsw', 'ldur', 'stur') or ins.mnemonic.startswith(('ld', 'st')):
                    for op in ins.operands:
                        if op.type == capstone.arm64.ARM64_OP_MEM and op.mem.disp in targets: hits[op.mem.disp].add(m)
        for off, f in sorted(targets.items()):
            h = hits.get(off)
            f['verification'] = {'status': 'PASS' if h else 'WARN', 'functions_using_offset': len(h or [])}
            rep.add('PASS' if h else 'WARN', 'field', f'{cls}+{hex(off)} {f["name"]}', f['offset'], f'{len(h or [])} {cls}:: functions access this offset')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--kb', default=str(ROOT / 'docs/mpr'))
    ap.add_argument('--base', type=lambda x: int(x, 0), default=None)
    ap.add_argument('--write-back', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--report', default=None)
    ap.add_argument('--extra-elf', action='append', default=None, help='more modules whose symbols satisfy imports (default: sdk.elf/subsdk0.elf/rtld.elf beside --elf)')
    ap.add_argument('--skip', default='', help='comma list: symbols,patches,structs')
    a = ap.parse_args()
    kbd = pathlib.Path(a.kb)
    B = Binary(a.elf, a.base)
    extras = a.extra_elf if a.extra_elf is not None else [str(pathlib.Path(a.elf).with_name(n)) for n in ('sdk.elf', 'subsdk0.elf', 'rtld.elf') if pathlib.Path(a.elf).with_name(n).exists() and pathlib.Path(a.elf).name != n]
    for x in extras: B.add_extra(x)
    print(f'binary: {a.elf}  base=0x{B.base:x}  symbols={len(B.syms)}  segments={len(B.loads)}')
    rep = Report(a.quiet)
    skip = set(a.skip.split(','))
    sk = json.load(open(kbd / 'symbols.json', encoding='utf-8')) if (kbd / 'symbols.json').exists() else {'symbols': []}
    pk = json.load(open(kbd / 'patches.json', encoding='utf-8')) if (kbd / 'patches.json').exists() else {'sets': []}
    st = json.load(open(kbd / 'structs.json', encoding='utf-8')) if (kbd / 'structs.json').exists() else {'structs': []}
    if 'symbols' not in skip: check_symbols(B, sk, rep)
    if 'patches' not in skip: check_patches(B, pk, rep)
    if 'structs' not in skip: check_structs(B, st, rep, sk['symbols'])
    print('\n==== SUMMARY ====')
    for cat in sorted({k[0] for k in rep.c}):
        print(f'{cat:8} PASS {rep.c[(cat, "PASS")]:5}  FAIL {rep.c[(cat, "FAIL")]:5}  WARN {rep.c[(cat, "WARN")]:5}')
    tf = sum(v for k, v in rep.c.items() if k[1] == 'FAIL')
    print(f'TOTAL    PASS {sum(v for k, v in rep.c.items() if k[1] == "PASS")}  FAIL {tf}  WARN {sum(v for k, v in rep.c.items() if k[1] == "WARN")}')
    if a.report: json.dump([list(r) for r in rep.rows], open(a.report, 'w'), indent=0)
    if a.write_back:
        for p, d in (('symbols.json', sk), ('patches.json', pk), ('structs.json', st)):
            if (kbd / p).exists(): json.dump(d, open(kbd / p, 'w', encoding='utf-8'), indent=1)
        tot = collections.Counter()
        for (cat, stt), n in rep.c.items(): tot[f'{cat}.{stt}'] = n
        warn_reasons = collections.Counter()
        for r in rep.rows:
            if r[0] == 'WARN': warn_reasons[(r[4].split(':')[0].split('(')[0].split(';')[0][:60])] += 1
        json.dump({'binary': os.path.basename(a.elf), 'counts': dict(tot),
                   'failures': [{'category': r[1], 'name': r[2], 'address': r[3], 'finding': r[4]} for r in rep.rows if r[0] == 'FAIL'],
                   'warn_reasons': warn_reasons.most_common(12)}, open(kbd / 'validation.json', 'w', encoding='utf-8'), indent=1)
        print('verification blocks written back to', kbd)
    sys.exit(1 if tf else 0)


if __name__ == '__main__': main()
