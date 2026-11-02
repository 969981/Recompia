#!/usr/bin/env python
"""Prove ida_kb_apply.py headlessly (idalib) on the stripped synthetic ELF.

1. build synth_exec.elf (image base 0x7100000000), generate a KB from it, run validate_kb --write-back (adds verification blocks)
2. strip the ELF (no symbols left), run ida_kb_apply.py on the stripped copy and save the .i64
3. reopen the saved .i64 with idalib and assert names / comments / types are present
usage: run_ida_synth.py   (exit 0 = proven)
"""
import json, os, pathlib, shutil, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent))
import run_synth as R
import validate_kb as V

PY = sys.executable
ROOT = HERE.parents[2]
IDA = r'C:\Program Files\IDA Professional 9.4'
tmp = pathlib.Path(tempfile.mkdtemp())

R.build()
elf = R.S / 'synth_exec.elf'
kb = tmp / 'kb'
exp, pexp = R.make_kb(elf, kb)
shutil.copy(ROOT / 'docs' / 'mpr' / 'structs.h', kb / 'structs.h')
r = subprocess.run([PY, str(HERE.parent / 'validate_kb.py'), '--elf', str(elf), '--kb', str(kb), '--quiet', '--write-back'], capture_output=True, text=True)
print('validate_kb exit', r.returncode)
stripped = tmp / 'synth_stripped.elf'
shutil.copy(elf, stripped)
subprocess.run([str(R.LLVM / 'llvm-strip.exe'), '--strip-all', str(stripped)], check=True)
nm = subprocess.run([str(R.LLVM / 'llvm-nm.exe'), str(stripped)], capture_output=True, text=True)
print('symbols left in stripped ELF:', len([l for l in nm.stdout.splitlines() if l.strip()]))

out_db = tmp / 'synth.i64'
r = subprocess.run([PY, str(HERE.parent / 'ida_kb_apply.py'), str(stripped), '--kb', str(kb), '--out', str(out_db)], capture_output=True, text=True)
print(r.stdout[-1500:], r.stderr[-800:])
print('ida_kb_apply exit', r.returncode)

check = r'''
import os, sys, json
os.environ["IDADIR"] = r"%s"
sys.path.insert(0, os.path.join(os.environ["IDADIR"], "idalib", "python"))
import idapro
idapro.enable_console_messages(False)
assert idapro.open_database(r"%s", False) == 0
import ida_nalt, ida_name, ida_bytes, ida_funcs, ida_typeinf, idc
base = ida_nalt.get_imagebase()
res = {"base": hex(base)}
kb = json.load(open(r"%s"))["symbols"]
want = [e for e in kb if e["name"] == "CPlayerMP1::ProcessInput" and e.get("verification", {}).get("status") == "PASS" and e["_k"] == "good_mangled"][0]
ea = base + int(want["address"], 16)
res["name_at_ProcessInput"] = ida_name.get_name(ea)
res["demangled"] = ida_name.get_demangled_name(ea, 0, 0, 0)
f = ida_funcs.get_func(ea)
res["func_cmt"] = ida_funcs.get_func_cmt(f, True) if f else None
til = ida_typeinf.get_idati()
for tn in ("CPlayerMP1", "CActorMP1", "EControl", "CStateManager"):
    t = ida_typeinf.tinfo_t()
    res["type_" + tn] = bool(t.get_named_type(til, tn)) and t.get_size()
t = ida_typeinf.tinfo_t(); t.get_named_type(til, "CPlayerMP1")
u = ida_typeinf.udt_type_data_t()
t.get_udt_details(u)
res["CPlayerMP1_members_at_0x5cc"] = [m.name for m in u if m.offset == 0x5cc * 8]
print("RESULT " + json.dumps(res))
idapro.close_database(False)
''' % (IDA, str(out_db), str(kb / 'symbols.json'))
(tmp / 'check.py').write_text(check)
r = subprocess.run([PY, str(tmp / 'check.py')], capture_output=True, text=True)
print(r.stdout[-1500:], r.stderr[-1500:])
line = [l for l in r.stdout.splitlines() if l.startswith('RESULT ')]
ok = False
if line:
    res = json.loads(line[0][7:])
    print(json.dumps(res, indent=1))
    ok = (res['name_at_ProcessInput'].startswith('_ZN10CPlayerMP112ProcessInput') and res['demangled'] and 'CPlayerMP1::ProcessInput' in res['demangled']
          and res['func_cmt'] and 'KB:' in res['func_cmt'] and res.get('type_CPlayerMP1') and res.get('type_EControl') is not None and res.get('CPlayerMP1_members_at_0x5cc'))
print('IDA APPLY PROVEN' if ok else 'IDA APPLY NOT PROVEN')
sys.exit(0 if ok else 1)
