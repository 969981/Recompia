# SPDX-License-Identifier: GPL-3.0-or-later
"""Spin-loop test (nxrecomp/spin.py) on synthetic AArch64 functions.

    python test_spin.py [--build DIR]

Each case is a few instruction words lifted as one function. PASS when
  * the poll loops are found with the right kind, and loops that end by themselves (a counter, a cursor, a call, a CAS
    retry, a store of a changing value) are not;
  * the loads of a found loop are lifted as poll loads (rdp*, rdvp) and every other statement is byte-identical to the
    lift without the pass; with spin_yield the head also calls nx_spin_hint(2);
  * when clang is installed: at -O1 the lifted `L: ldr w8, [x0]; cbz w8, L` keeps a loop with the load inside it, where
    the plain lift lets clang drop the loop (that is the bug this pass fixes).
"""
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, ROOT)
import capstone                    # noqa: E402
from nxrecomp.image import Image   # noqa: E402
from nxrecomp import program, spin  # noqa: E402

BASE = 0x7100000000
RET = 0xd65f03c0

# name: (words with their disassembly, expected kinds of the loops found)
CASES = {
    'poll': ([(0xb9400008, 'ldr w8, [x0]'), (0x34ffffe8, 'cbz w8, #0x7100000000'), (RET, 'ret')], ['poll']),
    'acquire': ([(0x88dffc08, 'ldar w8, [x0]'), (0x34ffffe8, 'cbz w8, #0x7100000000'), (RET, 'ret')], ['poll+acq']),
    # cbnz exits; a second load and a compare decide the back edge (Capstone says cbnz reads NZCV: corrected)
    'two_loads': ([(0xb9400008, 'ldr w8, [x0]'), (0x35000088, 'cbnz w8, #0x7100000014'), (0xb9400029, 'ldr w9, [x1]'),
                   (0x7100053f, 'cmp w9, #1'), (0x54ffff81, 'b.ne #0x7100000000'), (RET, 'ret')], ['poll']),
    'wfe': ([(0x885ffc08, 'ldaxr w8, [x0]'), (0x35000068, 'cbnz w8, #0x7100000010'), (0xd503205f, 'wfe'),
             (0x17fffffd, 'b #0x7100000000'), (RET, 'ret')], ['wfe']),
    'invariant_store': ([(0xb900003f, 'str wzr, [x1]'), (0xb9400008, 'ldr w8, [x0]'), (0x34ffffc8, 'cbz w8, #0x7100000000'),
                         (RET, 'ret')], ['poll_store']),
    'vector': ([(0x3dc00000, 'ldr q0, [x0]'), (0x9e660008, 'fmov x8, d0'), (0xb4ffffc8, 'cbz x8, #0x7100000000'), (RET, 'ret')],
               ['poll']),
    'prologue': ([(0xaa0003e1, 'mov x1, x0'), (0x91002022, 'add x2, x1, #8'), (0xb9400048, 'ldr w8, [x2]'),
                  (0x34ffffe8, 'cbz w8, #0x7100000008'), (0xb9400020, 'ldr w0, [x1]'), (RET, 'ret')], ['poll']),
    # not spins
    'counter': ([(0xb9400008, 'ldr w8, [x0]'), (0x71000529, 'subs w9, w9, #1'), (0x54ffffc1, 'b.ne #0x7100000000'), (RET, 'ret')], []),
    'cursor': ([(0xb8404408, 'ldr w8, [x0], #4'), (0x34ffffe8, 'cbz w8, #0x7100000000'), (RET, 'ret')], []),
    'call': ([(0xb9400008, 'ldr w8, [x0]'), (0x94000003, 'bl #0x7100000010'), (0x34ffffc8, 'cbz w8, #0x7100000000'), (RET, 'ret'),
              (RET, 'ret')], []),
    'cas_retry': ([(0x885ffc08, 'ldaxr w8, [x0]'), (0x11000508, 'add w8, w8, #1'), (0x8809fc08, 'stlxr w9, w8, [x0]'),
                   (0x35ffffa9, 'cbnz w9, #0x7100000000'), (RET, 'ret')], []),
    'varying_store': ([(0xb9400008, 'ldr w8, [x0]'), (0xb9000028, 'str w8, [x1]'), (0x34ffffc8, 'cbz w8, #0x7100000000'),
                       (RET, 'ret')], []),
    'too_far': ([(0xb9400008, 'ldr w8, [x0]')] + [(0xd503201f, 'nop')] * 128 + [(0x34ffefe8, 'cbz w8, #0x7100000000'), (RET, 'ret')], []),
}

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print('FAIL', what)


def lift(words, spin_on, yield_hint=False, ends=None):
    mem = struct.pack('<%dI' % len(words), *words)
    img = Image('main', BASE, mem, (0, len(mem)))
    env = program.Env(img)
    env.ends = ends or {BASE: BASE + len(mem)}
    env.starts = sorted(env.ends)
    env.spin, env.spin_yield = spin_on, yield_hint
    text = program.lift_function(env, BASE)[0]
    return text, env.spin_stats


def kinds(words):
    word = lambda a: words[(a - BASE) // 4]                 # noqa: E731
    return ['%s%s' % (lp.kind, '+acq' if lp.acquire else '') for lp in spin.find(word, BASE, BASE + 4 * len(words))]


def main():
    build = os.path.join(ROOT, 'build', 'spin')
    for i, a in enumerate(sys.argv):
        if a == '--build':
            build = sys.argv[i + 1]
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for name, (code, want) in CASES.items():
        words = [w for w, _ in code]
        for k, (w, asm) in enumerate(code):                 # the words are what the case says they are
            got = ['%s %s' % (i.mnemonic, i.op_str) for i in cs.disasm(struct.pack('<I', w), BASE + 4 * k)]
            check(got and got[0].strip() == asm, '%s: word %d is %s, not %s' % (name, k, got, asm))
        got = kinds(words)
        check(got == want, '%s: loops %s, expected %s' % (name, got, want))
        plain, _ = lift(words, False)
        polled, st = lift(words, True)
        yielded, sty = lift(words, True, True)
        pl, po = plain.split('\n'), polled.split('\n')
        check(len(pl) == len(po), '%s: line counts differ' % name)
        diff = [(a, b) for a, b in zip(pl, po) if a != b]
        if not want:
            check(not diff and plain == yielded, '%s: not a spin, yet the text changed' % name)
            continue
        for a, b in diff:                                   # only loads change, and only into their poll form
            check(spin.poll_text(a)[0] == b, '%s: changed beyond the loads: %r -> %r' % (name, a, b))
        check(re.search(r'\brdp(8|16|32|64)\(|\brdvp\(', polled) or 'acq' in want[0], '%s: no poll load in %s' % (name, polled))
        check(not re.search(r'\brd(8|16|32|64)\(|\brdv\(', polled.split('\n')[2]) if name != 'prologue' else True,
              '%s: a plain load left in the loop head' % name)
        check(not st.unprotected, '%s: unprotected loads %s' % (name, st.unprotected))
        if want[0] in ('wfe',):
            check(yielded == polled, '%s: a WFE loop got another hint' % name)
        else:
            check(sty.yields == 1 and yielded.count('nx_spin_hint(2); ') == 1, '%s: spin_yield head hint missing' % name)
    # the prologue case: the instructions outside the loop (mov, add, the load after it) are untouched
    words = [w for w, _ in CASES['prologue'][0]]
    plain, _ = lift(words, False)
    polled, _ = lift(words, True)
    check('rd32(a)' in polled.split('\n')[6] and 'rdp32' in polled.split('\n')[4], 'prologue: the load after the loop must stay plain:\n' + polled)
    # the poll forms of every lifted load spelling
    check(spin.poll_text('c->x[8] = (uint64_t)(nx::ld<uint32_t>(a));')[0] == 'c->x[8] = (uint64_t)(nx::ld_poll<uint32_t>(a));', 'nx::ld')
    check(spin.poll_text('{ uint64_t a = c->x[0]; c->x[1] = rd64(a); c->v[0] = rdv(a, 16); }')[0]
          == '{ uint64_t a = c->x[0]; c->x[1] = rdp64(a); c->v[0] = rdvp(a, 16); }', 'rd64 / rdv')
    check(spin.poll_text('stxp32(c, a, b, d); ldrd64(x)')[1] is False, 'only whole helper names are rewritten')
    clang_check(build)
    print('FAIL (%d)' % len(FAILS) if FAILS else 'PASS')
    return 1 if FAILS else 0


def clang_check(build):
    """clang -O1 on the lifted poll loop: the plain lift has no loop left, the poll lift keeps the load in its loop."""
    clang = shutil.which('clang++') or (r'C:\Program Files\LLVM\bin\clang++.exe' if os.path.exists(r'C:\Program Files\LLVM\bin\clang++.exe') else None)
    if not clang:
        print('skip: no clang++ for the codegen check')
        return
    os.makedirs(build, exist_ok=True)
    open(os.path.join(build, 'funcs.h'), 'w').close()
    words = [w for w, _ in CASES['poll'][0]]
    for spin_on in (False, True):
        text, _ = lift(words, spin_on)
        src = os.path.join(build, 'poll_%d.cpp' % spin_on)
        with open(src, 'w') as f:
            f.write(program.GEN_HEADER + 'extern "C" {\n' + text + '\n}\n')
        target = ['--target=x86_64-pc-windows-msvc'] if os.name == 'nt' else []
        r = subprocess.run([clang] + target + ['-std=c++20', '-O1', '-w', '-fno-strict-aliasing', '-ffp-contract=off', '-mcx16',
                                               '-I', os.path.join(ROOT, 'include'), '-I', build, '-S', '-o', '-', src],
                           capture_output=True, text=True)
        if r.returncode:
            check(False, 'clang failed: ' + r.stderr[-2000:])
            return
        body = r.stdout.split('f_7100000000:', 1)[-1].split('End function', 1)[0]
        loop = 'Loop Header' in body
        if spin_on:
            m = re.search(r'Loop Header.*?\n(.*?)\n\s*j\w+\s+\.LBB', body, re.S)
            check(loop and m and re.search(r'mov\w*\s+\(%r\w+\)', m.group(1)), 'poll lift: no load inside a loop at -O1:\n' + body)
        else:
            print('plain lift at -O1: %s' % ('loop kept' if loop else 'no loop left (the hazard)'))


if __name__ == '__main__':
    sys.exit(main())
