# SPDX-License-Identifier: GPL-3.0-or-later
"""Guest registers and NZCV in C locals: a rewrite of one lifted function's statements (emit_function.lift_function).

The instruction lifters write every guest register and flag through the context (c->x[n], c->N ..., adds64(c, ...)), so
each guest instruction is a load and a store of Ctx memory, four stores per flag-setting instruction. The compiler removes
such a store only where it proves that nothing reads the Ctx before the next one, and it keeps no register in a host
register across a branch join. This pass moves them into locals that clang keeps in host registers and syncs with the Ctx
only where something else can see it. Modes (GameConfig.reg_locals, `nxrecomp lift --locals`): 'off' (this pass is not
run: the eager code), 'exact' and 'aapcs' (below).

Registers. Every X register the function names (x0..x30, SP = x[31]) becomes a local `xN`. Before a statement part that
hands the Ctx to other code (a call of a lifted function, call_indirect, an SVC, MRS/MSR to the runtime, a site callout or
hook, a trap: escapes()) the registers that may differ from the Ctx are stored (spill), and a `return` spills first. Which
ones may differ is a forward data flow over the gotos and fall-throughs (dirty sets, may), followed in text order inside a
statement. After a lifted call the Ctx is the truth again: the callee, the runtime it reaches or an unwinder may have
changed any register. Locals are loaded lazily: a statement that reads a register whose local is not valid on every path
to it (valid sets, must) loads it from the Ctx first, which is current there because the register is dirty on no path. A
register dirty on one path and not valid on another (written in one branch only) is "eager": loaded at the entry and right
after every call, so it is always valid. After any other escape every local is loaded again (clang drops the loads nobody
reads). So wherever other code runs, the Ctx holds the registers the eager code would have stored there.

Flags. NZCV lives in a local `NxF F` (include/nxrecomp/locals.h): the packed N Z C V and the operands of the last
flag-setting add, subtract or logic instruction. Every definition writes both; a reader whose reaching definitions are
all of one kind (sub64, add32, logic64, ...) computes its condition from the operands, so `cmp x1, x2; b.lt` becomes
one signed compare; any other reader uses the packed form, and so does a reader that follows a reload of F inside its own
statement (a site hook in front of the instruction may have changed the Ctx's flags). clang drops what nobody reads, which
makes the flags lazy. F starts from the Ctx at the entry and is stored into the Ctx at tail exits (a branch to another
function, the fall into the next listed function, an indirect tail call) and at escapes to the runtime, which then reload
it; only where a definition can reach (F differs from the Ctx's flags otherwise). 'exact' also stores F before every
lifted call and at every return and loads it after the call, so the Ctx's NZCV is what the eager code leaves at every call
and return. 'aapcs' does neither: AAPCS64 neither passes NZCV to a callee nor returns it, so compiled code never reads
flags across a call or a return. A read that can see flags from before its function's entry or from a callee is counted
(Stats entry_reads, after_call; the lift log lists them with their addresses): such reads are where 'aapcs' differs from
'exact'.

What still sees a stale Ctx. Between two sync points the Ctx's x[] and NZCV are behind the locals. Code that runs there
without being called by the lifted function reads old values: another thread that looks at this thread's Ctx (the
runtime's freeze and crash reports print its lr / sp / fp and registers) and a host fault handler on one of the function's
own memory accesses. Both must treat the Ctx of a running thread as a hint. Everything the function calls, and every way
out of it (return, tail exit, an exception or longjmp out of a callee), finds the Ctx exact. The landing pads of a
function are entered through a new call of it (u_<entry>(c, pad)), so they start from the Ctx like the entry does.

Left as they were (apply() returns None and Stats.left counts the reason): functions with an unresolved `br` (their
dispatch can enter any instruction), functions compiled with optnone (no SROA, the locals would only add stack traffic),
any function whose text uses c->x with a computed index or takes a register's address, any with a register write under
control flow other than a mid-function hook's `if (...) return;`, any with an escape that is not a tail exit under
control flow (its sync would run on one path only), and any that reads a register after a lifted call inside one statement
(only the eager registers are loaded there). The lifter emits none of the last two today; they guard the pass against a
new statement form.

Design after valentderah/totk-pc-recomp recomp/regs.py and recomp/flags.py (MIT License, Copyright (c) 2026 Valiantsin
Dzerakh; see recomp/THIRD_PARTY.md): dirty-set spills, conditions from the operands of the reaching definition kind,
AAPCS64 flags. Re-implemented for this lifter's statements; differences: every register in locals instead of the 4-6 most
used, loaded lazily; no trust in callee-saved registers after a call; every flag definition writes both forms instead of
a per-definition packing plan.
"""
import re

REG = re.compile(r'c->x\[(\d+)\]')
WRITE = re.compile(r'c->x\[(\d+)\]\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
# a register used other than by value: its address taken (a unary &: one that follows no operand; `a & c->x[1]` is an AND),
# a computed index, ++ / --
ODD_REG = re.compile(r'(?:^|[(,=;{}?:!~+\-*/%<>|^&])\s*&\s*c->x\b|c->x\[(?!\d+\])|c->x\[\d+\]\s*(?:\+\+|--)|(?:\+\+|--)\s*c->x\[')
GOTO = re.compile(r'goto L_([0-9a-f]+);')
LABEL_DISPATCH = 'L_dispatch'

# flag helpers that take the Ctx (ctx.h, cxx.h); rewritten to take F
FLAG_CALL = re.compile(r'\b(?:nx::)?(adds64|adds32|subs64|subs32|logic64|logic32|set_nzcv|get_nzcv|cond|fcmp)\b(\s*<[^<>()]*>)?\(c\b')
FLAG_RAW = re.compile(r'c->([NZCV])\b')
FLAG_DEF_NAMES = ('adds64', 'adds32', 'subs64', 'subs32', 'logic64', 'logic32', 'set_nzcv', 'fcmp')

# helpers taking the Ctx that touch neither X registers nor NZCV (exclusive monitor, vector registers): no sync needed
SAFE = frozenset(['ldxr', 'stxr', 'clrex', 'ldxp', 'stxp', 'ldxp128', 'stxp32', 'stxp64', 'stx8', 'stx16', 'stx32', 'stx64',
                  'v_tbl', 'v_ldn', 'v_stn', 'stx_diag'])
# a call that passes the Ctx: NAME(c, ... ) or NAME(c)
CALL_C = re.compile(r'\b((?:[A-Za-z_]\w*::)*[A-Za-z_]\w*)\s*(<[^<>()]*>)?\(c\s*[,)]')
# calls that do not pass the Ctx but end in the runtime's crash report
NO_CTX_ESCAPE = re.compile(r'\b(guest_trap|guest_unimpl)\s*\(')
# statements after which control never falls through to the next one (exact forms only: a wrong match would be unsafe)
TERMINAL = re.compile(r'^(?:return;|goto L_[0-9a-f]+;|\{ (?:[fh]_\w+\(c\)|u_[0-9a-f]+\(c, 0\)|call_indirect\(c, [^;{}]*\)); return; \})$')
# calls that leave for good when followed by `return;` (tail exits: publish the flags, no reload)
TAILCALL_NAMES = re.compile(r'^(?:[fhu]_\w+|call_indirect)$')

# the same calls inside a tail exit (`<call>; return;`): what is left of a statement without them has only escapes that come back
TAIL_EXIT = re.compile(r'(?:[fhu]_\w+\(c(?:, 0)?\)|call_indirect\(c, [^;{}]*\));\s*return;')
MODES = ('exact', 'aapcs')

KINDS = ('add64', 'sub64', 'add32', 'sub32', 'logic64', 'logic32')
P = 'p'           # packed flags only (CCMP, FCMP, MSR NZCV, ADCS / SBCS)
CTX = 'ctx'       # F equals the Ctx's flags (a reload after the runtime ran); read packed
ENTRY = 'entry'   # F equals the flags the function was entered with (from the Ctx); read packed


def _close(text, i):
    """Index just past the parenthesis that closes the one at text[i]."""
    depth = 0
    for j in range(i, len(text)):
        ch = text[j]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                return j + 1
    raise ValueError('unbalanced parentheses: ' + text)


def _args(text, i):
    """Top-level argument strings of the call whose '(' is at text[i], and the index past its ')'."""
    end = _close(text, i)
    out, depth, start = [], 0, i + 1
    for j in range(i + 1, end - 1):
        ch = text[j]
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif ch == ',' and depth == 0:
            out.append(text[start:j].strip())
            start = j + 1
    out.append(text[start:end - 1].strip())
    return out, end


def flag_effect(text):
    """(reads flags, kind written or None, written on every path) of one statement's original text."""
    reads = bool(FLAG_RAW.search(text))
    defs = []
    for m in FLAG_CALL.finditer(text):
        name = m.group(1)
        if name in ('cond', 'get_nzcv'):
            reads = True
            continue
        if name in ('adds64', 'adds32'):
            args, _ = _args(text, m.end() - 2)
            cin = args[3] if len(args) == 4 else ''
            if cin == '0':
                defs.append('add' + name[4:])
            elif cin == '1':
                defs.append('sub' + name[4:])
            else:                                  # ADCS / SBCS: carry in from the flags
                reads = True
                defs.append(P)
        elif name in ('subs64', 'subs32'):
            defs.append('sub' + name[4:])
        elif name in ('logic64', 'logic32'):
            defs.append('logic' + name[5:])
        else:                                      # set_nzcv, fcmp
            defs.append(P)
    if not defs:
        return reads, None, False
    kind = defs[0] if len(defs) == 1 else P
    always = len(defs) == 1 and 'if (' not in text and '?' not in text
    return reads, kind, always


def calls_out(text):
    """Where the statement calls code that can see the Ctx: the text offsets of every call that takes it and is not a
    helper known to touch neither registers nor flags, and of the traps."""
    out = [m.start() for m in NO_CTX_ESCAPE.finditer(text)]
    for m in CALL_C.finditer(text):
        name = m.group(1).split('::')[-1]
        if not (name in SAFE or name in FLAG_DEF_NAMES or name in ('cond', 'get_nzcv')):
            out.append(m.start())
    return out


def escapes(text):
    """Whether the statement hands the Ctx to other code (and so needs a sync)."""
    return bool(calls_out(text)) or 'return;' in text


def conditional_escape(text):
    """Whether an escape that comes back (not a tail exit) may sit under control flow (some comes before it in the
    statement): its sync would run on one path only, which the per-statement data flow does not model. A mid-function
    hook's call is the condition itself: it always runs."""
    rest = TAIL_EXIT.sub('', MIDHOOK.sub('nx_site_midhook(c, 0);', text))
    return any(CONTROL.search(rest[:i]) or '?' in rest[:i] for i in calls_out(rest))


class Odd(Exception):
    """A statement form the rewrite does not handle: the function stays as it is (the text is the reason)."""


def plan(stmts, pads=(), flags='exact'):
    """-> (texts, decl, Stats) with the locals rewrite applied; raises Odd when the function must stay as it is.
    stmts: [(pc or None, text)] in order; pads: landing pad addresses (entered from the resume switch)."""
    texts = [t for _, t in stmts]
    for t in texts:
        if LABEL_DISPATCH in t:
            raise Odd('unresolved br')
        if ODD_REG.search(t):
            raise Odd('register by address or computed index')
    n = len(texts)
    index = {pc: i for i, (pc, _) in enumerate(stmts) if pc is not None}
    succ = []
    for i, t in enumerate(texts):
        s = [index[int(m.group(1), 16)] for m in GOTO.finditer(t) if int(m.group(1), 16) in index]
        if i + 1 < n and not TERMINAL.match(t.strip()):
            s.append(i + 1)
        succ.append(s)
    roots = [0] + [index[p] for p in pads if p in index]

    regs = sorted({int(m.group(1)) for t in texts for m in REG.finditer(t)})
    writes = [{int(m.group(1)) for m in WRITE.finditer(t)} for t in texts]
    # registers a statement reads: every mention that is not the target of a plain `=` (a compound one reads too)
    reads = [{int(m.group(1)) for m in REG.finditer(t)} - plain_targets(t) for t in texts]
    esc = [escapes(t) for t in texts]
    fx = [flag_effect(t) for t in texts]
    for t, w in zip(texts, writes):
        # writes are taken as done on every path through their statement; the only control flow the lifter puts in
        # front of a register write is a mid-function hook's `if (...) return;`
        if w and CONTROL.search(MIDHOOK.sub('', t)):
            raise Odd('register write under control flow')
    for t, e in zip(texts, esc):
        if e and conditional_escape(t):
            raise Odd('escape under control flow')
    # what each statement's syncs leave (rewrite's `after`): None, or ('all' | 'eager', registers written after them);
    # reload_f: its rewrite reloads F from the Ctx (an escape other than a tail exit; in 'aapcs' also not a lifted call),
    # def_last: ... and a flag definition follows the last such reload in its text
    probe = [rewrite(t, regs, (), (), P, True, True, flags) if e else ('', None) for t, e in zip(texts, esc)]
    reload_f = [RELOAD_F in p for p, _ in probe]
    def_last = [bool(FX_DEF.search(p[p.rfind(RELOAD_F):])) for p, _ in probe]
    info = [a for _, a in probe]

    # dirty registers on entry to each statement (may: union over predecessors). Not reached in the flow: none.
    dirty = [None] * n
    work = []
    for r in roots:
        if dirty[r] is None:
            dirty[r] = set()
            work.append(r)
    while work:
        i = work.pop()
        out = (dirty[i] | writes[i]) if info[i] is None else set(info[i][1])
        for s in succ[i]:
            if dirty[s] is None or not out <= dirty[s]:
                dirty[s] = (dirty[s] or set()) | out
                work.append(s)
    dirty = [d if d is not None else set() for d in dirty]

    # Registers in locals are loaded lazily: before a statement that reads one whose local is not valid on every path
    # (must: intersection over predecessors), the local is loaded from the Ctx, which is then current because the
    # register is not dirty on any path. A register that is dirty on one path and not valid on another (written in
    # one branch, untouched in the other, then read or spilled) is "eager": loaded at the entry and after every call,
    # so its local is always valid. Iterate until no such conflict is left.
    preds = [[] for _ in range(n)]
    for i, s in enumerate(succ):
        for j in s:
            preds[j].append(i)
    allr = set(regs)
    eager = set()
    while True:
        valid = [set(allr) for _ in range(n)]
        for r in roots:
            valid[r] = set(eager)
        loads = [set() for _ in range(n)]
        changed = True
        while changed:
            changed = False
            for i in range(n):
                if i in roots:
                    vin = set(eager)
                elif preds[i]:
                    vin = set.intersection(*[vout(j, valid, loads, writes, info, eager, allr) for j in preds[i]])
                else:
                    vin = set(eager)                   # not reached in the flow
                if vin != valid[i]:
                    valid[i] = vin
                    changed = True
                ld = reads[i] - vin
                if ld != loads[i]:
                    loads[i] = ld
                    changed = True
        conflict = set()
        for i in range(n):
            conflict |= dirty[i] - valid[i]
        if not conflict - eager:
            break
        eager |= conflict

    # kinds of the flag definitions reaching each statement ('call': a call since then, for stats only)
    reach = [None] * n
    work = []
    for r in roots:
        if reach[r] is None:
            reach[r] = {ENTRY}
            work.append(r)
    while work:
        i = work.pop()
        reads, kind, always = fx[i]
        out = set(reach[i])
        if reload_f[i]:
            out = {CTX}                            # the runtime ran: F reloaded from the Ctx
        if kind and (def_last[i] or not reload_f[i]):  # (a definition before the reload went to the Ctx and came back)
            out = {kind} if always else (out | {kind})
        elif info[i] is not None and info[i][0] == 'eager':
            out = out | {'call'}
        for s in succ[i]:
            if reach[s] is None or not out <= reach[s]:
                reach[s] = (reach[s] or set()) | out
                work.append(s)
    for i in range(n):
        if reach[i] is None:
            reach[i] = {P}                         # not reached in the flow

    st = Stats()
    out_texts = []
    for i, t in enumerate(texts):
        reads, kind, _ = fx[i]
        mode = None
        if reads:
            ks = reach[i] - {'call'}
            mode = next(iter(ks)) if len(ks) == 1 and ks <= set(KINDS) else P
            if reload_f[i]:
                mode = P                           # F was reloaded inside this statement: the operands are not its flags
            st.reads += 1
            st.operand += mode != P
            st.after_call += 'call' in reach[i]
            st.entry_reads += ENTRY in reach[i]
            if ENTRY in reach[i] or 'call' in reach[i]:
                st.where.append((stmts[i][0], 'entry' if ENTRY in reach[i] else 'call'))
        # F differs from the Ctx's flags only after a definition: publish at an exit only then
        publish = bool(kind) or bool(reach[i] - {CTX, ENTRY, 'call'})
        pre = ''.join('x%d = c->x[%d]; ' % (r, r) for r in sorted(loads[i]))
        out_texts.append(pre + rewrite(t, regs, dirty[i], sorted(eager), mode, esc[i], publish, flags)[0])
    decl = ''
    if eager:
        decl += 'uint64_t ' + ', '.join('x%d = c->x[%d]' % (r, r) for r in sorted(eager)) + '; '
    lazy = [r for r in regs if r not in eager]
    if lazy:
        decl += 'uint64_t ' + ', '.join('x%d' % r for r in lazy) + '; '
    decl += 'NxF F = {c->N, c->Z, c->C, c->V, 0, 0};'
    st.eager = len(eager)
    st.lazy = len(lazy)
    st.funcs = 1
    return out_texts, decl, st


class Stats:
    def __init__(self):
        self.funcs = self.reads = self.operand = self.after_call = self.entry_reads = 0
        self.where = []               # (pc, 'entry' | 'call') of those reads
        self.eager = self.lazy = 0     # registers loaded at the entry / on first use
        self.left = {}                # functions left as they were: reason -> count

    def leave(self, reason):
        self.left[reason] = self.left.get(reason, 0) + 1

    def add(self, o):
        self.funcs += o.funcs
        self.reads += o.reads
        self.operand += o.operand
        self.after_call += o.after_call
        self.entry_reads += o.entry_reads
        self.where += o.where
        self.eager += o.eager
        self.lazy += o.lazy
        for k, v in o.left.items():
            self.left[k] = self.left.get(k, 0) + v

    def lines(self, name, mode):
        """The lift log's lines for one module."""
        out = ['%s: locals (%s) in %d functions; flag reads %d: from operands %d, may see the entry flags %d, may see a callee flags %d'
               % (name, mode, self.funcs, self.reads, self.operand, self.entry_reads, self.after_call),
               '  registers in locals: %d loaded at the entry, %d on first use' % (self.eager, self.lazy),
               '  left as they were: %d functions (%s)' % (sum(self.left.values()),
                                                           ', '.join('%s %d' % kv for kv in sorted(self.left.items())) or 'none')]
        if self.where:
            out.append('  those reads: ' + ' '.join('%x(%s)' % (pc or 0, k) for pc, k in sorted(self.where, key=lambda w: w[0] or 0)[:40]))
        return out


def apply(lines, trailer, pads, mode, stats, skip=None):
    """The statements [(pc, text)] of one function and its closing statement on the locals: (lines, trailer, declarations),
    or None when the function stays as it is (`skip`: the caller's reason, else plan's). stats: Stats, updated."""
    if skip:
        stats.leave(skip)
        return None
    try:
        texts, decl, st = plan(lines + [(None, trailer)], pads, mode)
    except Odd as e:
        stats.leave(str(e))
        return None
    stats.add(st)
    return [(pc, t) for (pc, _), t in zip(lines, texts)], texts[-1], decl


PUBLISH = 'c->N = F.N; c->Z = F.Z; c->C = F.C; c->V = F.V; '
RELOAD_F = ' F.N = c->N; F.Z = c->Z; F.C = c->C; F.V = c->V;'


FX_DEF = re.compile(r'\bfx_(?:%s)\b' % '|'.join(FLAG_DEF_NAMES))
LOCAL_READ = re.compile(r'(?<![\w.>\[])x\d+\b(?!\s*=(?!=))')
LOCAL_WRITE = re.compile(r'(?<![\w.>\[])x(\d+)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
PLAIN_TARGET = re.compile(r'c->x\[(\d+)\]\s*=(?!=)')
CONTROL = re.compile(r'\bif \(|\belse\b|\bswitch \(|\bfor \(|\bwhile \(')
MIDHOOK = re.compile(r'if \(nx_site_midhook\(c, 0x[0-9a-f]+ULL\)\) return;')


def plain_targets(text):
    """Registers whose every mention in text is the target of a plain `=` (written, not read)."""
    targets = {int(m.group(1)) for m in PLAIN_TARGET.finditer(text)}
    counts = {}
    for m in REG.finditer(text):
        counts[int(m.group(1))] = counts.get(int(m.group(1)), 0) + 1
    plain = {}
    for m in PLAIN_TARGET.finditer(text):
        plain[int(m.group(1))] = plain.get(int(m.group(1)), 0) + 1
    return {r for r in targets if plain[r] == counts[r]}


def vout(j, valid, loads, writes, info, eager, allr):
    """Registers whose local is valid after statement j."""
    after = info[j]
    if after is None:
        return valid[j] | loads[j] | writes[j]
    kind, wr = after
    return (allr if kind == 'all' else eager) | wr


def rewrite(text, regs, dirty, eager, mode, escape, publish=True, flags='exact'):
    """-> (text, after): the statement on the locals (registers and flags renamed, syncs around every escaping call and
    return) and what its last non-tail sync leaves: None (no sync), or ('all' | 'eager', registers written after it).
    dirty: registers that may differ from the Ctx on entry; eager: the registers reloaded after a lifted call (every
    register is reloaded after other escapes)."""
    # 1. flags: helper calls take F; reads use the chosen mode
    def flag_sub(m):
        name, targs = m.group(1), m.group(2) or ''
        if name == 'cond':
            return ('fx_cc_%s(F' % mode) if mode != P else 'fx_cond(F'
        return 'fx_%s%s(F' % (name, targs)
    text = FLAG_CALL.sub(flag_sub, text)
    text = FLAG_RAW.sub(lambda m: 'F.' + m.group(1), text)
    # 2. registers
    text = REG.sub(lambda m: 'x' + m.group(1), text)
    if not escape:
        return text, None
    pub = PUBLISH if publish else ''
    rl_all = ''.join(' x%d = c->x[%d];' % (r, r) for r in regs)
    rl_eager = ''.join(' x%d = c->x[%d];' % (r, r) for r in eager)
    # 3. syncs, in text order: every escaping call and every return; register writes in between grow the dirty set
    sites = []
    for m in CALL_C.finditer(text):
        name = m.group(1).split('::')[-1]
        if name in SAFE or name.startswith('fx_'):
            continue
        sites.append((m.start(), 1, name))
    for m in NO_CTX_ESCAPE.finditer(text):
        sites.append((m.start(), 1, m.group(1)))
    for m in re.finditer(r'\breturn;', text):
        sites.append((m.start(), 1, 'return'))
    for m in LOCAL_WRITE.finditer(text):
        # the write happens after its right-hand side: `x0 = (({ ...; auto r_ = guest_mrs(c, ...); reload; r_; }))`
        # stores x0 after the escape's sync, so it is dirty after it (placed at the assignment's start, the reload
        # dropped it from the dirty set and the next call reloaded the stale Ctx value)
        sites.append((_assign_end(text, m.end()), 0, int(m.group(1))))
    sites.sort()
    d = set(dirty)
    after = None
    out, pos = [], 0
    for start, is_site, name in sites:
        if not is_site:
            d.add(name)
            if after is not None:
                after[1].add(name)
            continue
        if start < pos:                            # the return of a tail exit, consumed with its call
            continue
        sp = ''.join('c->x[%d] = x%d; ' % (r, r) for r in sorted(d))
        out.append(text[pos:start])
        if name == 'return':
            out.append('{ %s%sreturn; }' % (sp, '' if flags == 'aapcs' else pub))
            pos = start + len('return;')
            continue
        end = _close(text, text.index('(', start))
        call = text[start:end]
        tail = re.match(r'\s*;\s*return;', text[end:])
        stmt = re.match(r'\s*;', text[end:]) and _stmt_start(text, start)
        lifted = bool(TAILCALL_NAMES.match(name))
        if tail and lifted:                        # tail exit: spill, publish the flags, leave
            out.append('{ %s%s%s; return; }' % (sp, pub, call))
            pos = end + tail.end()
            continue
        if stmt and lifted:                        # a call of a lifted function: only it can change registers
            exact = flags != 'aapcs'               # AAPCS64: a callee neither reads nor leaves flags
            out.append('{ %s%s%s;%s%s }' % (sp, pub if exact else '', call, rl_eager, RELOAD_F if exact else ''))
            after = ('eager', set())
        elif stmt:                                 # the runtime: it may read or change anything
            out.append('{ %s%s%s;%s%s }' % (sp, pub, call, rl_all, RELOAD_F))
            after = ('all', set())
        else:                                      # the call's value is used: a statement expression (clang)
            out.append('({ %s%sauto r_ = %s;%s%s r_; })' % (sp, pub, call, rl_all, RELOAD_F))
            after = ('all', set())
        pos = end + (re.match(r'\s*;', text[end:]).end() if stmt else 0)
        d = set()
        if after[0] == 'eager' and LOCAL_READ.search(text[pos:]):
            raise Odd('register read after a call in one statement')   # only the eager registers were loaded again
    out.append(text[pos:])
    return ''.join(out), after



def _assign_end(text, i):
    """Index of the end of the assignment whose right-hand side starts at text[i]: the first ; or , at its own
    bracket depth, or the bracket that closes around it."""
    depth = 0
    for j in range(i, len(text)):
        ch = text[j]
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            if depth == 0:
                return j
            depth -= 1
        elif ch in ';,' and depth == 0:
            return j
    return len(text)


def _stmt_start(text, i):
    """Whether the call at text[i] starts a statement (its value is not used): after ; { } else, or after the condition
    of an if (not after a cast)."""
    j = i - 1
    while j >= 0 and text[j] == ' ':
        j -= 1
    if j < 0 or text[j] in ';{}' or text[max(0, j - 3):j + 1] == 'else':
        return True
    if text[j] != ')':
        return False
    depth = 0
    for k in range(j, -1, -1):
        if text[k] == ')':
            depth += 1
        elif text[k] == '(':
            depth -= 1
            if depth == 0:
                return text[max(0, k - 3):k].rstrip().endswith('if')
    return False
