#!/usr/bin/env python
"""Apply the MPR knowledge base (docs/mpr) to an IDA database: names, function/site comments, C types.

Headless (idalib, no GUI):
    tools\\.venv\\Scripts\\python.exe tools\\analysis\\ida_kb_apply.py main.elf [--kb docs/mpr] [--out work/ida/main.i64] [--force] [--include-unverified]
Inside IDA / the ida-mcp execute_python tool:
    exec(open(r'D:\\...\\tools\\analysis\\ida_kb_apply.py').read()); apply_kb(r'D:\\...\\docs\\mpr')

What it does (addresses in the KB are main-relative offsets; ea = IDA image base + offset):
  * types      parses docs/mpr/structs.h (flattened struct layouts + enums) into the local type library
  * names      address-bearing entries: a verified Itanium-mangled name (validate_kb --write-back) or the source's mangled name
               is set when the address has no real name yet (IDA then shows the demangled form); source names that are only
               descriptive labels are never used as names
  * comments   every entry with an address gets a one-line comment (role, tags, source, verification); function entries get a
               repeatable function comment, patch sites a normal comment; patch-set entries get the patch description
  * report     counts of what changed; nothing is deleted or overwritten unless --force
Unverified or FAILed entries are skipped unless --include-unverified; FAIL entries are never applied.
"""
import json, os, pathlib, re, sys, tempfile, warnings
warnings.simplefilter('ignore', DeprecationWarning)

IDA_DIR = os.environ.get('IDADIR', r'C:\Program Files\IDA Professional 9.4')


def _ida_modules():
    import ida_nalt, ida_name, ida_funcs, ida_bytes, ida_segment, ida_typeinf, idc, idaapi
    return ida_nalt, ida_name, ida_funcs, ida_bytes, ida_segment, ida_typeinf, idc, idaapi


def sanitize(n):
    return re.sub(r'[^A-Za-z0-9_]', '_', n)


def apply_kb(kb_dir, force=False, include_unverified=False, apply_types=True, log=print):
    ida_nalt, ida_name, ida_funcs, ida_bytes, ida_segment, ida_typeinf, idc, idaapi = _ida_modules()
    kb = pathlib.Path(kb_dir)
    base = ida_nalt.get_imagebase()
    stats = dict(types_ok=None, types_errors=None, renamed=0, kept_existing=0, func_comments=0, site_comments=0, patch_comments=0,
                 skipped_fail=0, skipped_unverified=0, skipped_nosegment=0, labels_not_named=0)

    # ---- types -----------------------------------------------------------------------
    if apply_types and (kb / 'structs.h').exists():
        text = (kb / 'structs.h').read_text(encoding='utf-8')
        errs = idc.parse_decls(text, 0)       # returns number of errors, prints them to the output window
        stats['types_errors'] = errs
        stats['types_ok'] = errs == 0
        log(f'types: parsed structs.h with {errs} error(s)')

    # ---- names + comments ---------------------------------------------------------------
    def has_real_name(ea):
        n = ida_name.get_name(ea)
        return bool(n) and not re.match(r'^(sub_|loc_|unk_|off_|byte_|word_|dword_|qword_|flt_|dbl_|nullsub_|j_)[0-9A-Fa-f]+$', n)

    def put_cmt(ea, text, is_func):
        if is_func:
            f = ida_funcs.get_func(ea)
            if f and f.start_ea == ea:
                ida_funcs.set_func_cmt(f, text, True)
                stats['func_comments'] += 1
                return
        ida_bytes.set_cmt(ea, text, False)
        stats['site_comments'] += 1

    sj = json.load(open(kb / 'symbols.json', encoding='utf-8'))['symbols']
    for e in sj:
        v = e.get('verification') or {}
        st = v.get('status')
        if st == 'FAIL':
            stats['skipped_fail'] += 1
            continue
        if st is None and not include_unverified:
            stats['skipped_unverified'] += 1
            continue
        off = v.get('binary_address') or e.get('address')
        if off is None:
            continue
        ea = base + int(off, 16)
        if ida_segment.getseg(ea) is None:
            stats['skipped_nosegment'] += 1
            continue
        label = (' ' in e['name']) or ('_at_0x' in e['name']) or ('::' not in e['name'] and len(e['name']) <= 6 and e['name'].isupper())
        mangled = v.get('binary_mangled') or e.get('mangled')
        is_func_entry = e['kind'] in ('function', 'vtable') and (v.get('method') in ('mangled', 'name', 'name+address', 'name+arity', 'address', 'plain name') or e['kind'] == 'vtable')
        if mangled and e['kind'] != 'patch_site' and v.get('method') != 'import' and not (v.get('note', '') or '').startswith('mid-function'):
            if has_real_name(ea) and not force:
                stats['kept_existing'] += 1
            else:
                if ida_name.set_name(ea, mangled, ida_name.SN_NOWARN | ida_name.SN_NOCHECK | ida_name.SN_FORCE):
                    stats['renamed'] += 1
        elif label:
            stats['labels_not_named'] += 1
        elif e['kind'] in ('function', 'data') and not has_real_name(ea) and '::' not in e['name'] and not (v.get('note', '') or '').startswith('mid-function'):
            if ida_name.set_name(ea, sanitize(e['name']), ida_name.SN_NOWARN | ida_name.SN_FORCE):
                stats['renamed'] += 1
        parts = [e['name'] if label else None, e.get('role'), ('tags: ' + ','.join(e['tags'])) if e.get('tags') else None,
                 'src: ' + ', '.join(sorted({c['source'] for c in e['claims'] if c.get('source')})),
                 'KB: ' + (st or 'unverified') + ((' ' + v['note']) if v.get('note') else '')]
        put_cmt(ea, ' | '.join(p for p in parts if p)[:500], is_func_entry)

    # ---- patch sets ----------------------------------------------------------------------
    pj = kb / 'patches.json'
    if pj.exists():
        for s in json.load(open(pj, encoding='utf-8'))['sets']:
            for en in s['entries']:
                v = en.get('verification') or {}
                if v.get('status') == 'FAIL' or (v.get('status') is None and not include_unverified):
                    continue
                ea = base + int(en['offset'], 16)
                if ida_segment.getseg(ea) is None:
                    continue
                ida_bytes.set_cmt(ea, f"community patch [{s['id']}] new bytes {en.get('bytes')}: {en.get('comment') or ''}"[:300], False)
                stats['patch_comments'] += 1
    log('apply_kb: ' + json.dumps(stats))
    return stats


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('binary', help='ELF / .i64 to open headlessly')
    ap.add_argument('--kb', default=str(pathlib.Path(__file__).resolve().parents[2] / 'docs' / 'mpr'))
    ap.add_argument('--out', default=None, help='save the database here (default: next to the binary, IDA decides)')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--include-unverified', action='store_true')
    ap.add_argument('--ida-dir', default=IDA_DIR)
    a = ap.parse_args(argv)
    os.environ['IDADIR'] = a.ida_dir
    sys.path.insert(0, os.path.join(a.ida_dir, 'idalib', 'python'))
    import idapro
    idapro.enable_console_messages(False)
    if idapro.open_database(a.binary, True) != 0:
        sys.exit('open_database failed')
    try:
        st = apply_kb(a.kb, force=a.force, include_unverified=a.include_unverified)
        import ida_loader
        if a.out:
            ida_loader.save_database(a.out, 0)
            print('saved', a.out)
    finally:
        idapro.close_database(True)
    return 0 if st.get('types_errors') in (0, None) else 2


if __name__ == '__main__':
    if 'idaapi' in sys.modules or 'ida_idaapi' in sys.modules:
        pass  # loaded inside IDA: call apply_kb(...) yourself
    else:
        sys.exit(main(sys.argv[1:]))
