# SPDX-License-Identifier: GPL-3.0-or-later
"""Android build only: a rewrite of the lifter's C++ output after the lift, before the compile. Outside the stamped lifter trees
(recomp/nxrecomp, recomp/include): desktop builds never run it and their lift is unchanged. Needs the forced include
recomp/native/nx_native.h (nx_fcmp_keep, nx_cc_fcmp) on every unit it rewrote.

    python postlift.py <lift dir of one module, or c_*.cpp files> ...   rewrites in place (idempotent: a rewritten file is skipped)

Two rewrites per lifted function, both on the statements as the lifter wrote them (one statement per line, `L_<pc>: ` labels):

1. Vector registers in locals. Every V register the function names becomes a V128 local, so clang keeps guest vector registers in host
   vector registers instead of Ctx memory. The locals are current at every point where other code can look at the Ctx: a register
   named since the last sync on some path (dirty_in) is stored into the Ctx before every statement that hands the Ctx to other code
   (a lifted call, the runtime, a site hook, a trap, v_tbl / v_ldn / v_stn, a return) and before the closing statement; registers
   are loaded lazily before the first statement that names them where they are not valid on every path (valid_in), and a register
   that would be loaded where it may be dirty, or stored where it is not valid on every path, is loaded at the entry and after every
   escape instead (eager). Left alone: no V register, an unresolved br (its dispatch is outside the statements), an optnone function,
   c->v not indexed by a literal, an escaping statement that names a V register, or too few references for the syncs (references <
   dirty stores + escapes x registers / 2).
2. FCMP conditions from the operands. A flag reader `fx_cond(F, cc)` whose reaching flag definitions are all plain FCMPs (fx_fcmp
   on every path through its statement, nothing else writing F) reads `nx_cc_fcmp(F, cc)`, one IEEE comparison of the operands the
   FCMP keeps (`nx_fcmp_keep`), instead of rebuilding NZCV. Any other definition (integer flags, a conditional FCMP of an FCCMP, a
   reload of F from the Ctx, the entry) keeps the packed form.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE)))          # recomp/: the lifter's own statement analysis (read only)
from nxrecomp import locals as L                                 # noqa: E402

MARK = '/* postlift 2 */'
SIG = re.compile(r'^(?:NX_OPTNONE |__attribute__\(\(noinline\)\) )?void ([fu])_([0-9a-f]+)\(Ctx \*__restrict c(, uint64_t rp)?\) \{$')
LABEL = re.compile(r'^L_([0-9a-f]+): (.*)$')
VREG = re.compile(r'c->v\[(\d+)\]')
ODD = re.compile(r'c->v(?!\[\d+\])')
V_CTX = re.compile(r'\b(?:v_tbl|v_ldn|v_stn)\(c\b')
FX_DEF = re.compile(r'\bfx_(?:adds64|adds32|subs64|subs32|logic64|logic32|set_nzcv)\(F\b|\bF\.[NZCV]\s*=(?!=)|\bF = ')
FCMP = re.compile(r'\bfx_fcmp(?:<[^<>()]*>)?\(F, ')
READ = re.compile(r'\bfx_cond\(F, ')


def escapes(s):
    return bool(L.calls_out(s)) or 'return;' in s or bool(V_CTX.search(s))


def flow(stmts, pads, transfer, init):
    """Forward data flow over the statements (gotos, fall-throughs, landing pads as roots): in-sets, None where not reached.
    transfer(i, in) -> out; init: the set at every root. The join is union for may-sets (merge='or') or intersection."""
    texts, index, succ, falls, roots = L._cfg(stmts, pads)
    din = [None] * len(texts)
    work = []
    for r in roots:
        din[r] = set(init[0])
        work.append(r)
    while work:
        i = work.pop()
        out = transfer(i, din[i])
        for j in succ[i]:
            new = set(out) if din[j] is None else (din[j] | out if init[1] == 'or' else din[j] & out)
            if din[j] is None or new != din[j]:
                din[j] = new
                work.append(j)
    return din


def vec_locals(lines, trailer, pads):
    """-> (lines, trailer, decl) or None. lines: [(pc or None, statement)]."""
    texts = [s for _, s in lines] + [trailer]
    used = sorted({int(n) for t in texts for n in VREG.findall(t)})
    if not used or any(ODD.search(t) for t in texts) or any(L.LABEL_DISPATCH in t for t in texts):
        return None
    esc = [escapes(t) for t in texts]
    if any(e and VREG.search(t) for t, e in zip(texts, esc)):
        return None
    refs = [{int(n) for n in VREG.findall(t)} for t in texts]
    stmts = lines + [(None, trailer)]
    din = flow(stmts, pads, lambda i, d: set() if esc[i] else d | refs[i], (set(), 'or'))
    stores = [sorted(d) if d is not None else used for d in din]
    nrefs = sum(len(VREG.findall(t)) for t in texts)
    if nrefs < sum(len(stores[i]) for i, e in enumerate(esc) if e) + sum(esc) * len(used) // 2:
        return None
    eager = set()
    while True:
        vin = flow(stmts, pads, lambda i, v: set(eager) if esc[i] else v | refs[i], (eager, 'and'))
        conflict = {r for i in range(len(texts)) if vin[i] is not None and din[i] is not None
                    for r in ((refs[i] | (din[i] if esc[i] else set())) - vin[i]) & din[i]}
        if not conflict - eager:
            break
        eager |= conflict
    load = ' '.join('v%d = c->v[%d];' % (n, n) for n in sorted(eager))
    out = []
    for i, (pc, s) in enumerate(lines):
        if esc[i]:
            s = '{ %s %s %s }' % (' '.join('c->v[%d] = v%d;' % (n, n) for n in stores[i]), s, load)
        else:
            need = sorted(refs[i] - vin[i]) if vin[i] is not None else sorted(refs[i])
            s = VREG.sub(lambda m: 'v' + m.group(1), s)
            if need:
                s = '{ %s %s }' % (' '.join('v%d = c->v[%d];' % (n, n) for n in need), s)
        out.append((pc, s))
    trailer = '%s %s' % (' '.join('c->v[%d] = v%d;' % (n, n) for n in stores[-1]), trailer)
    decl = 'V128 ' + ', '.join(('v%d = c->v[%d]' % (n, n)) if n in eager else 'v%d' % n for n in used) + ';'
    return out, trailer, decl


def fcmp_defs(t):
    """'fcmp' when the statement's only flag definition is an FCMP on every path through it, 'other' for any other definition,
    None when it defines no flags."""
    f = FCMP.findall(t)
    if FX_DEF.search(t):
        return 'other'
    if not f:
        return None
    first = FCMP.search(t).start()
    if len(f) > 1 or L.CONTROL.search(t[:first]) or '?' in t[:first]:
        return 'other'
    return 'fcmp'


def fcmp_readers(lines, trailer, pads):
    """-> lines with the readers rewritten, or None when nothing changes."""
    texts = [s for _, s in lines] + [trailer]
    if not any(READ.search(t) for t in texts) or not any(FCMP.search(t) for t in texts):
        return None
    kind = [fcmp_defs(t) for t in texts]
    stmts = lines + [(None, trailer)]
    reach = flow(stmts, pads, lambda i, r: {kind[i]} if kind[i] else r, ({'other'}, 'or'))
    keep = set()                                     # FCMP statements whose operands a rewritten reader needs
    out, changed = [], False
    for i, (pc, s) in enumerate(lines):
        if reach[i] == {'fcmp'} and READ.search(s):
            d = FX_DEF.search(s)
            f = FCMP.search(s)
            stop = min(m.start() for m in (d, f) if m) if (d or f) else len(s)
            head = READ.sub('nx_cc_fcmp(F, ', s[:stop])
            if head != s[:stop]:
                s = head + s[stop:]
                changed = True
        out.append((pc, s))
    if not changed:
        return None
    # every FCMP that reaches a rewritten reader keeps its operands; simplest exact rule: all of them do (cheap: two register moves)
    return [(pc, FCMP.sub('nx_fcmp_keep(F, ', s)) for pc, s in out]


def split(fn_lines):
    """One function's lines -> (head, stmts [(pc, text)], trailer, tail) or None for a form this pass does not take."""
    if not SIG.match(fn_lines[0]) or fn_lines[-1] != '}':
        return None
    body = fn_lines[1:-1]
    if any(l.startswith('L_dispatch:') for l in body):
        return None
    k = 0
    head = []
    while k < len(body) and (body[k].startswith('  c->fn = ') or body[k].startswith('  if (nx_hook_entry(') or
                             body[k].startswith('  uint64_t ') or body[k].startswith('  NxF ')):
        head.append(body[k]); k += 1
    pads = []
    nhead = len(head)                                # the declarations go here, in front of the resume switch's gotos
    if k < len(body) and body[k].startswith('  if (rp) switch (rp) {'):
        while True:
            head.append(body[k])
            m = re.match(r'^  case 0x([0-9a-f]+)ULL: goto L_', body[k])
            if m:
                pads.append(int(m.group(1), 16))
            k += 1
            if head[-1].startswith('  default: '):
                break
    stmts = []
    for l in body[k:]:
        m = LABEL.match(l)
        if m:
            stmts.append((int(m.group(1), 16), m.group(2)))
        elif l.startswith('  '):
            stmts.append((None, l[2:]))
        else:
            return None
    if not stmts:
        return None
    return head, nhead, stmts[:-1], stmts[-1][1], pads


def rewrite_function(fn_lines, cold):
    sp = split(fn_lines)
    if sp is None or cold:
        return fn_lines
    head, nhead, lines, trailer, pads = sp
    labels = {pc for pc, _ in lines if pc is not None}
    decl = None
    r = fcmp_readers(lines, trailer, pads)
    if r:
        lines = r
    v = vec_locals(lines, trailer, pads)
    if v:
        lines, trailer, decl = v
    if not r and not v:
        return fn_lines
    out = [fn_lines[0]] + head[:nhead] + (['  ' + decl] if decl else []) + head[nhead:]
    for pc, s in lines:
        out.append(('L_%x: %s' % (pc, s)) if pc is not None and pc in labels else '  ' + s)
    out.append('  ' + trailer)
    out.append('}')
    return out


def rewrite_file(path):
    text = open(path, encoding='utf-8').read()
    if text.startswith(MARK):
        return False
    lines = text.split('\n')
    out, i = [], 0
    while i < len(lines):
        if SIG.match(lines[i]):
            j = i
            while lines[j] != '}':
                j += 1
            out.extend(rewrite_function(lines[i:j + 1], lines[i].startswith('NX_OPTNONE')))
            i = j + 1
        else:
            out.append(lines[i]); i += 1
    with open(path + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
        f.write(MARK + '\n' + '\n'.join(out))
    os.replace(path + '.tmp', path)
    return True


if __name__ == '__main__':
    n = 0
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            n += sum(rewrite_file(os.path.join(arg, x)) for x in sorted(os.listdir(arg)) if re.match(r'^c_\d+\.cpp$', x))
        else:
            n += rewrite_file(arg)
    print('postlift: %d files rewritten' % n)
