#!/usr/bin/env python
"""Prove validate_kb.py on a synthetic AArch64 ELF (clang --target=aarch64-linux-gnu + lld).

Builds two ELFs (ET_DYN with .dynsym and imports; ET_EXEC at image base 0x7100000000 with .symtab), generates a
knowledge base with known-good and deliberately wrong claims from the ELF itself, runs validate_kb.py and asserts every
expected status.   usage: run_synth.py   (exit 0 = all expectations met)
"""
import json, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import validate_kb as V

LLVM = pathlib.Path(r'C:\Program Files\LLVM\bin')
S = HERE / 'synth'
PY = sys.executable
fail = 0


def sh(*a):
    r = subprocess.run([str(x) for x in a], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        raise SystemExit('command failed: %s' % a[0])
    return r.stdout


def build():
    cc = [LLVM / 'clang++.exe', '--target=aarch64-linux-gnu', '-fPIC', '-ffreestanding', '-fno-exceptions', '-fno-rtti',
          '-fno-threadsafe-statics', '-O1', '-c']
    sh(*cc, S / 'synth.cpp', '-o', S / 'synth.o')
    sh(*cc, S / 'stubs.cpp', '-o', S / 'stubs.o')
    sh(LLVM / 'ld.lld.exe', '-shared', '-Bsymbolic', '-o', S / 'synth_dyn.elf', S / 'synth.o')
    sh(LLVM / 'ld.lld.exe', '-pie', '--image-base=0x7100000000', '-e', '0', '-o', S / 'synth_exec.elf', S / 'synth.o', S / 'stubs.o')


def make_kb(elf, outdir, bad=False):
    B = V.Binary(str(elf))

    def off(m):
        return B.syms[m][0] - B.base

    def sym(name, kind='function', mangled=None, address=None, contains=None, cands=None, hooked=False):
        return {'name': name, 'mangled': mangled, 'kind': kind, 'address': hex(address) if address is not None else None, 'signature': None,
                'claims': [{'mangled': mangled}] if hooked else [], 'contains': contains or [], 'mangled_candidates': cands or [],
                'role': None, 'tags': [], 'used_by': [], 'aliases': []}

    PI = '_ZN10CPlayerMP112ProcessInputERK11CFinalInputR13CStateManager'
    GW = '_ZN10COsContext14GetScreenWidthEv'
    GH = '_ZN10COsContext15GetScreenHeightEv'
    pi, gw, gh = off(PI), off(GW), off(GH)
    syms, exp = [], {}

    def add(key, e, status):
        e['_k'] = key
        syms.append(e)
        exp[key] = status

    add('good_mangled', sym('CPlayerMP1::ProcessInput', mangled=PI, address=pi), 'PASS')
    add('wrong_addr', sym('CPlayerMP1::ProcessInput', mangled=PI, address=pi + 0x1000), 'FAIL')
    add('missing_mangled', sym('CPlayerMP1::NoSuchFn', mangled='_ZN10CPlayerMP19NoSuchFnEv', hooked=True), 'FAIL')
    add('by_name', sym('NTonemap::build_tonemap_eval_params'), 'PASS')
    add('mid_function', sym('hook site in ProcessInput', kind='patch_site', address=pi + 8), 'WARN')
    add('addr_only', sym('COsContext::GetScreenHeight', address=gh), 'PASS')
    add('addr_other_name', sym('CTotally::Different', address=gh), 'FAIL')
    add('contains_ok', sym('CPlayerMP1::ProcessInput', mangled=PI, address=pi, contains=[hex(pi + 8)]), 'PASS')
    add('contains_bad', sym('COsContext::GetScreenWidth', mangled=GW, address=gw, contains=[hex(pi + 8)]), 'FAIL')
    add('plain_c', sym('nvnBootstrapLoader'), 'PASS')
    add('unknown', sym('CNoSuch::Thing'), 'WARN')
    add('import_or_def', sym('operator delete', mangled='_ZdlPvm', hooked=True), 'PASS')
    add('candidates', sym('CPlayerMP1::ProcessInput', cands=['_ZN10CPlayerMP19WrongOneEv', PI]), 'PASS')

    ins = B.disasm(B.va(pi), 40)
    bls = [i for i in ins if i.mnemonic == 'bl']
    bw = [i for i in bls if int(i.op_str.lstrip('#'), 16) == B.va(gw)][0]
    bh = [i for i in bls if int(i.op_str.lstrip('#'), 16) == B.va(gh)][0]
    orig = B.read(bw.address, 4).hex().upper()
    ent, pexp = [], {}

    def padd(key, offs, byt, comment, status, **kw):
        e = {'offset': hex(offs), 'bytes': byt, 'comment': comment, **kw}
        e['_k'] = key
        ent.append(e)
        pexp[key] = status

    mov = '00C88052'  # mov w0,#0x640
    padd('bl_ok', bw.address - B.base, mov, 'mov w0, #0x640 // GetScreenWidth CPlayerMP1::ProcessInput', 'PASS')
    padd('bl_wrong_callee', bw.address - B.base, mov, 'mov w0, #0x384 // GetScreenHeight CPlayerMP1::ProcessInput', 'FAIL')
    padd('bl_wrong_func', bh.address - B.base, '80708052', 'mov w0, #0x384 // GetScreenHeight CTotally::Else', 'FAIL')
    padd('orig_ok', bw.address - B.base, 'D503201F', 'nop', 'PASS', orig_bytes=orig)
    padd('orig_bad', bw.address - B.base, 'D503201F', 'nop', 'FAIL', orig_bytes='DEADBEEF')
    padd('outside', 0x7fffff00, 'D503201F', 'nop', 'FAIL')
    padd('stale_comment', gw, mov, 'mov w0, #0x777 // width', 'WARN')
    padd('const_getter', gw, mov, 'mov w0, #0x640 | mov w0, #0x780 | docked resX', 'PASS')

    def fld(o, n):
        return {'offset': o, 'name': n, 'type': 'float', 'confidence': 'source-stated', 'note': '', 'src': {}, 'alternatives': []}

    good = [{'name': 'CEntityMP1', 'bases': [], 'size': None, 'sources': [], 'fields': [],
             'virtuals': [{'slot': 0, 'name': '~CEntityMP1', 'signature': ''}, {'slot': 2, 'name': 'Think', 'signature': ''}]},
            {'name': 'CPlayerMP1', 'bases': ['CEntityMP1'], 'size': None, 'sources': [], 'fields': [fld('0x5cc', 'pitch'), fld('0x7e0', 'bogus')],
             'virtuals': [{'slot': 0, 'name': '~CPlayerMP1', 'signature': ''}, {'slot': 3, 'name': 'Extra', 'signature': ''}]}]
    badst = json.loads(json.dumps(good))
    badst[1]['virtuals'][1]['slot'] = 2
    outdir.mkdir(parents=True, exist_ok=True)
    json.dump({'symbols': syms}, open(outdir / 'symbols.json', 'w'))
    json.dump({'sets': [{'id': 'synth', 'entries': ent}]}, open(outdir / 'patches.json', 'w'))
    json.dump({'structs': badst if bad else good}, open(outdir / 'structs.json', 'w'))
    return exp, pexp


def run(elf, label, bad=False):
    global fail
    tmp = pathlib.Path(tempfile.mkdtemp())
    exp, pexp = make_kb(elf, tmp, bad)
    r = subprocess.run([PY, str(HERE.parent / 'validate_kb.py'), '--elf', str(elf), '--kb', str(tmp), '--quiet', '--write-back'],
                       capture_output=True, text=True)
    print(f'--- {label}: validate_kb exit {r.returncode}')
    print(r.stdout.strip().split('==== SUMMARY ====')[-1].strip())
    sy = json.load(open(tmp / 'symbols.json'))['symbols']
    pa = json.load(open(tmp / 'patches.json'))['sets'][0]['entries']
    st = json.load(open(tmp / 'structs.json'))['structs']

    def chk(what, got, want):
        global fail
        ok = got == want
        if not ok:
            fail += 1
        print(('ok   ' if ok else 'BAD  ') + f'{what}: got {got}, expected {want}')

    for e in sy:
        chk('symbol ' + e['_k'], e['verification']['status'], exp[e['_k']])
    for e in pa:
        chk('patch  ' + e['_k'], e['verification']['status'], pexp[e['_k']])
    cp = [s for s in st if s['name'] == 'CPlayerMP1'][0]
    chk('vtable CEntityMP1', [s for s in st if s['name'] == 'CEntityMP1'][0].get('verification_vtable', {}).get('status'), 'PASS')
    chk('vtable CPlayerMP1', cp.get('verification_vtable', {}).get('status'), 'FAIL' if bad else 'PASS')
    chk('field pitch@0x5cc', cp['fields'][0]['verification']['status'], 'PASS')
    chk('field bogus@0x7e0', cp['fields'][1]['verification']['status'], 'WARN')
    chk('exit code (FAIL entries present => 1)', r.returncode, 1)



def main():
    build()
    run(S / 'synth_dyn.elf', 'ET_DYN (.dynsym, base 0, imports)')
    run(S / 'synth_exec.elf', 'ET_EXEC (.symtab, image base 0x7100000000)')
    run(S / 'synth_dyn.elf', 'ET_DYN with a wrong vtable slot in the KB', bad=True)
    print('ALL SYNTHETIC EXPECTATIONS MET' if not fail else f'{fail} EXPECTATION(S) FAILED')
    sys.exit(1 if fail else 0)


if __name__ == '__main__':
    main()
