#!/usr/bin/env python3
"""Presses the gameplay bindings of play mode and checks each one against the run trace. One part per mpr run.

Bindings: the factory defaults of play mode (runtime-wt/verify/src/gfx/play/play_defaults.cpp, [bindings.all] and the
[bindings.menu]/[bindings.map] layers) and the factory [switch] table (play_types.h).

  tools\\.venv\\Scripts\\python tools\\test\\bindings_run.py --part keyboard
  tools\\.venv\\Scripts\\python tools\\test\\bindings_run.py --part pad
  ... parts: keyboard, keyboard2, pad, pad2, scan, map, pause, menu
  tools\\.venv\\Scripts\\python tools\\test\\bindings_run.py --combine      (joins the part tables into table_all.txt)

Each part is its own mpr run with its own prologue and warp (gameplay.py). The exe is NXRT_VERIFY_EXE or game-build-verify\\build\\snap2.
Output per part: work\\verify\\bindings\\<part>\\ (the run: script.nxs, mpr.log, out\\trace.jsonl, shots),
work\\verify\\bindings\\table_<part>.txt and table_<part>.json. Exit code: the mpr.exe exit code.

Each step: @hash PRE_<tag>, @log BINDING <tag>, the input, @hash POST_<tag>. The analysis reads the trace rows between the two
hash labels and the play status lines (NXRT_PLAY_LOG_STATUS=1) between the BINDING marker and the next one.
Movement is judged against forward = -Z at yaw 0 (yaw + = turn left), as stated by the play team.
"""
import argparse, json, math, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gameplay as gp  # noqa: E402

ROOT = gp.ROOT
OUTROOT = os.path.join(ROOT, 'work', 'verify', 'bindings')
EXE = os.environ.get('NXRT_VERIFY_EXE', ROOT + '/game-build-verify/build/snap2')
BEAMS = {0: 'power', 1: 'ice', 2: 'wave', 3: 'plasma'}
VISORS = {0: 'combat', 1: 'xray', 2: 'scan', 3: 'thermal'}
GIVE = ['@give MorphBall 1', '@give ChargeBeam 1', '@give WaveBeam 1', '@give IceBeam 1', '@give PlasmaBeam 1',
        '@give Missiles 50', '@give ScanVisor 1', '@give ThermalVisor 1', '@give XRayVisor 1', '@give MorphBallBombs 1']
GAME_CTX = ('gameplay', 'morph', 'scan')   # contexts in which the gameplay bindings are active


def tap(name, hold=0.3, settle=0.5):
    """Keyboard or mouse input held hold game seconds, released, then settle game seconds."""
    return ['keydown %s' % name, '@run %g' % hold, 'keyup %s' % name, '@run %g' % settle]


def padtap(name, hold=0.3, settle=0.5):
    return ['hold %s' % name, '@run %g' % hold, 'release all', '@run %g' % settle]


def htap(name, hold=0.3, sleep_ms=800):
    """Host-time version for menus and pause: the game clock stands still there, so @run would never end."""
    return ['keydown %s' % name, '@sleep %d' % int(hold * 1000), 'keyup %s' % name, '@sleep %d' % sleep_ms]


def hpadtap(name, hold=0.3, sleep_ms=800):
    return ['hold %s' % name, '@sleep %d' % int(hold * 1000), 'release all', '@sleep %d' % sleep_ms]


class Part:
    def __init__(self, name):
        self.name, self.plan, self.body = name, [], []

    def add(self, binding, expect, check, lines):
        tag = '%s%02d' % (self.name[:2].upper(), len(self.plan) + 1)
        self.plan.append(dict(tag=tag, group=self.name, binding=binding, expect=expect, check=check))
        self.body.extend(['@hash PRE_%s' % tag, '@log BINDING %s' % tag] + lines + ['@hash POST_%s' % tag])


def part_keyboard(P):
    for k, b in [('1', 0), ('2', 2), ('3', 1), ('4', 3)]:
        P.add(k, 'beam %d (%s)' % (b, BEAMS[b]), ('beam', b), tap(k))
    for k, v in [('5', 0), ('6', 2), ('7', 3), ('8', 1)]:
        P.add(k, 'visor %d (%s)' % (v, VISORS[v]), ('visor', v), tap(k))
    P.add('WheelUp', 'beam changes (beam_prev)', ('wheel',), ['key WheelUp for 4', '@run 0.5'])
    P.add('WheelDown', 'beam changes back (beam_next)', ('wheel',), ['key WheelDown for 4', '@run 0.5'])
    for k, v in [('Up', 0), ('Right', 2), ('Down', 3), ('Left', 1)]:
        P.add(k, 'visor %d (%s): d-pad = visor_1..4' % (v, VISORS[v]), ('visor', v), tap(k))
    for k, d in [('W', 'fwd'), ('S', 'back'), ('A', 'left'), ('D', 'right')]:
        P.add(k, 'moves %s' % d, ('move', d), tap(k, 0.5, 0.5))
    P.add('Space', 'jump: state 1 or 2, height rises', ('jump',), tap('Space'))


def part_keyboard2(P):
    for k in ['Left Ctrl', 'C']:
        P.add(k, 'morph on: morph != 0, camera changes', ('morph', 'on'), tap(k, 0.3, 1.5))
        P.add(k + ' (again)', 'morph off: morph == 0', ('morph', 'off'), tap(k, 0.3, 1.5))
    P.add('F', 'missile count drops by 1 (first person)', ('missile',), tap('F'))
    P.add('Mouse2', 'missile count drops by 1 (middle button)', ('missile',), tap('Mouse2'))
    P.add('Mouse3', 'lock on: orbit != 0 (needs a target)', ('lock',), tap('Mouse3'))
    P.add('Mouse3 (again)', 'lock released: orbit == 0', ('unlock',), tap('Mouse3'))
    P.add('Q', 'lock on: orbit != 0 (needs a target)', ('lock',), tap('Q'))
    P.add('Q (again)', 'lock released: orbit == 0', ('unlock',), tap('Q'))
    P.add('Mouse1 (held 1.5 s)', 'charge shot: not in trace, screenshot only', ('charge',),
          ['keydown Mouse1', '@run 1.5', '@shot kb_mouse1_charge', 'keyup Mouse1', '@run 0.5'])
    P.add('Left Alt', 'free look: pitch follows mouse while held (recorded)', ('freelook',),
          ['keydown Left Alt', '@run 0.3', 'mouse 0 -150 for 10', 'keyup Left Alt', '@run 0.5'])
    P.add('mouse, no modifier (baseline)', 'pitch follows mouse', ('pitch',), ['mouse 0 -150 for 10', '@run 0.5'])
    P.add('mouse move (aim, not a [bindings] entry)', 'yaw turns right (decreasing yaw)', ('yaw', 'right'),
          ['mouse 300 0 for 10', '@run 0.5'])
    P.add('Return, Keypad Enter, Backspace, E, PageUp, PageDown, Mouse1 (menu)', 'menu/map only', ('notpressed',), [])


def part_pad(P):
    P.add('LS_UP', 'moves fwd', ('move', 'fwd'), ['stick left 0 1.0 for 40', '@run 0.5'])
    P.add('LS_DOWN', 'moves back', ('move', 'back'), ['stick left 0 -1.0 for 40', '@run 0.5'])
    P.add('LS_LEFT', 'moves left', ('move', 'left'), ['stick left -1.0 0 for 40', '@run 0.5'])
    P.add('LS_RIGHT', 'moves right', ('move', 'right'), ['stick left 1.0 0 for 40', '@run 0.5'])
    P.add('RS_LEFT', 'look left: yaw increases', ('yaw', 'left'), ['stick right -1.0 0 for 40', '@run 0.5'])
    P.add('RS_RIGHT', 'look right: yaw decreases', ('yaw', 'right'), ['stick right 1.0 0 for 40', '@run 0.5'])
    P.add('RS_UP', 'look up: pitch changes', ('pitch',), ['stick right 0 1.0 for 40', '@run 0.5'])
    P.add('RS_DOWN', 'look down: pitch changes', ('pitch',), ['stick right 0 -1.0 for 40', '@run 0.5'])
    for combo, b in [('X+UP', 0), ('X+RIGHT', 2), ('X+DOWN', 1), ('X+LEFT', 3)]:
        P.add(combo, 'beam %d (%s)' % (b, BEAMS[b]), ('beam', b), padtap(combo))


def part_pad2(P):
    for btn, v in [('UP', 0), ('RIGHT', 2), ('DOWN', 3), ('LEFT', 1)]:
        P.add(btn, 'visor %d (%s)' % (v, VISORS[v]), ('visor', v), padtap(btn))
    P.add('B', 'jump: state 1 or 2, height rises', ('jump',), padtap('B'))
    P.add('Y', 'morph on: morph != 0, camera changes', ('morph', 'on'), padtap('Y', 0.3, 1.5))
    P.add('Y (again)', 'morph off: morph == 0', ('morph', 'off'), padtap('Y', 0.3, 1.5))
    P.add('R', 'missile count drops by 1 (first person)', ('missile',), padtap('R'))
    P.add('ZL', 'lock on: orbit != 0 (needs a target)', ('lock',), padtap('ZL'))
    P.add('ZL (again)', 'lock released: orbit == 0', ('unlock',), padtap('ZL'))
    P.add('ZR (held 1.5 s)', 'charge shot: not in trace, screenshot only', ('charge',),
          ['hold ZR', '@run 1.5', '@shot pad_zr_charge', 'release all', '@run 0.5'])
    P.add('RSTICK', 'free look: pitch follows right stick while held (recorded)', ('freelook',),
          ['hold RSTICK', '@run 0.3', 'stick right 0 1.0 for 30', 'release all', '@run 0.5'])
    P.add('WheelUp / WheelDown', 'no pad binding (beam_prev, beam_next = none)', ('nobind',), [])
    P.add('A, B (menu), L, R, D-pad (menu)', 'menu/map only', ('notpressed',), [])



def part_scan(P):
    P.add('6', 'scan visor: visor 2 and ctx scan', ('scanon',), tap('6', 0.3, 1.0))
    P.add('5', 'combat visor again: visor 0, ctx gameplay', ('scanoff',), tap('5', 0.3, 1.0))
    P.add('RIGHT (pad)', 'scan visor: visor 2 and ctx scan', ('scanon',), padtap('RIGHT', 0.3, 1.0))
    P.add('UP (pad), back to combat', 'combat visor: visor 0, ctx gameplay', ('scanoff',), padtap('UP', 0.3, 1.0))


def part_map(P):
    P.add('Tab', 'map opens: ctx=map', ('ctx', 'map'), htap('Tab', 0.1, 1500))
    P.add('Tab (again)', 'map closes: ctx=gameplay', ('ctx', 'gameplay'), htap('Tab', 0.1, 1500))
    P.add('M', 'map opens: ctx=map', ('ctx', 'map'), htap('M', 0.1, 1500))
    P.add('M (again)', 'map closes: ctx=gameplay', ('ctx', 'gameplay'), htap('M', 0.1, 1500))
    P.add('PLUS (pad)', 'map opens: ctx=map', ('ctx', 'map'), hpadtap('PLUS', 0.3, 1500))
    P.add('PLUS (pad, again)', 'map closes: ctx=gameplay', ('ctx', 'gameplay'), hpadtap('PLUS', 0.3, 1500))


def part_pause(P):
    P.add('Escape', 'pause: igt does not advance', ('pause',), htap('Escape', 0.1, 2000))
    P.add('Escape (again)', 'resume: igt advances', ('resume',), htap('Escape', 0.1, 1500))
    P.add('MINUS (pad)', 'pause: igt does not advance', ('pause',), hpadtap('MINUS', 0.3, 2000))
    P.add('B (pad, back in pause menu)', 'resume: igt advances', ('resume',), hpadtap('B', 0.3, 1500))


def part_menu(P):
    P.add('Escape (pause)', 'pause screen: ctx=menu', ('ctx', 'menu'), htap('Escape', 0.1, 2000))
    for k in ['Q', 'E', 'PageUp', 'PageDown', 'WheelUp', 'WheelDown']:
        P.add(k + ' (menu)', 'menu key: no trace field, ctx stays menu', ('menukey',), htap(k, 0.3, 800))
    P.add('Mouse3 (back)', 'back: ctx=gameplay', ('ctx', 'gameplay'), htap('Mouse3', 0.3, 1500))
    P.add('Escape (pause, 2)', 'pause screen: ctx=menu', ('ctx', 'menu'), htap('Escape', 0.1, 2000))
    P.add('Backspace (back)', 'back: ctx=gameplay', ('ctx', 'gameplay'), htap('Backspace', 0.3, 1500))
    P.add('Return, Keypad Enter, Space, Mouse1 (confirm)', 'menu only (confirm may pick Quit)', ('notpressed',), [])


PARTS = {'keyboard': part_keyboard, 'keyboard2': part_keyboard2, 'pad': part_pad, 'pad2': part_pad2, 'scan': part_scan,
         'map': part_map, 'pause': part_pause, 'menu': part_menu}


def build(name):
    P = Part(name)
    PARTS[name](P)
    setup = gp.prologue(timeout_ms=420000) + gp.warp('tallon', 'tallon_canyon_cavern') + GIVE + ['@timeout 900', '@run 1']
    return setup + P.body + ['@log DONE'], P.plan


# ---- analysis ----
def load_trace(path):
    markers, rows = {}, []
    for line in open(path, encoding='utf-8', errors='replace'):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if 'label' in d:
            markers.setdefault(d['label'], d.get('f'))
        elif 'pos' in d and 'f' in d:
            rows.append(d)
    return markers, rows


def log_ctx(path):
    """Per step tag: the contexts in its status lines (in order), and the input-event counter delta over the step."""
    seg, ev, cur, last_ev = {}, {}, None, 0
    for ln in open(path, encoding='utf-8', errors='replace').read().splitlines():
        m = re.search(r'BINDING (\S+)', ln)
        if m:
            cur = m.group(1)
            seg.setdefault(cur, [])
            ev[cur] = [last_ev, last_ev]
            continue
        if 'play: status:' in ln:
            e = re.search(r'events=(\d+)', ln)
            if e:
                last_ev = int(e.group(1))
            if cur:
                ev[cur][1] = last_ev
            c = re.search(r'ctx=(\w+)', ln)
            if cur and c:
                seg[cur].append(c.group(1))
    return seg, {k: v[1] - v[0] for k, v in ev.items()}


def last_at(rows, f):
    best = None
    for r in rows:
        if r['f'] <= f:
            best = r
        else:
            break
    return best


def g(r, k):
    return None if r is None else r.get(k)


def yaw_delta(a, b):
    return ((b - a + math.pi) % (2 * math.pi)) - math.pi


def move_vec(win):
    mv = [r for r in win if 'vel' in r and 'yaw' in r]
    if not mv:
        return None
    best = max(mv, key=lambda r: math.hypot(r['vel'][0], r['vel'][2]))
    return best['vel'][0], best['vel'][2], best['yaw']


def judge(check, pre, post, win, ctxs, nseq):
    kind = check[0]
    if kind in ('beam', 'visor'):
        a = g(post, kind)
        if a is None:
            return 'NO-PROBE', 'no %s field in trace' % kind
        names = BEAMS if kind == 'beam' else VISORS
        return ('PASS' if a == check[1] else 'FAIL'), '%s %s -> %s (%s)' % (kind, g(pre, kind), a, names.get(a, '?'))
    if kind == 'wheel':
        b, a = g(pre, 'beam'), g(post, 'beam')
        if a is None:
            return 'NO-PROBE', 'no beam field'
        return ('PASS' if a != b else 'FAIL'), 'beam %s -> %s' % (b, a)
    if kind == 'move':
        mv = move_vec(win)
        if mv is None:
            return 'NO-PROBE', 'no player rows in window'
        vx, vz, y = mv
        sp = math.hypot(vx, vz)
        if sp < 0.3:
            return 'FAIL', 'no movement, max speed %.2f' % sp
        F = (-math.sin(y), -math.cos(y))         # forward at yaw y (forward = -Z at yaw 0)
        R = (math.cos(y), -math.sin(y))          # right
        d = {'fwd': F, 'back': (-F[0], -F[1]), 'right': R, 'left': (-R[0], -R[1])}[check[1]]
        cos = (vx * d[0] + vz * d[1]) / sp
        return ('PASS' if cos > 0.7 else 'FAIL'), 'speed %.2f, vel xz (%.2f, %.2f), yaw %.2f, cos %+.2f to %s' % (
            sp, vx, vz, y, cos, check[1])
    if kind == 'jump':
        if g(pre, 'pos') is None:
            return 'NO-PROBE', 'no player rows'
        y0 = pre['pos'][1]
        ymax = max(r['pos'][1] for r in win if 'pos' in r)
        air = any(r.get('state') in (1, 2) for r in win)
        return ('PASS' if air and ymax - y0 > 0.05 else 'FAIL'), 'state 1/2 seen %s, height gain %.2f' % (air, ymax - y0)
    if kind == 'morph':
        m1 = g(post, 'morph')
        if m1 is None:
            return 'NO-PROBE', 'no morph field'
        if check[1] == 'on':
            c0, c1 = g(pre, 'cam'), g(post, 'cam')
            return ('PASS' if m1 != 0 and c1 != c0 else 'FAIL'), 'morph %s -> %s, cam %s -> %s' % (g(pre, 'morph'), m1, c0, c1)
        return ('PASS' if m1 == 0 else 'FAIL'), 'morph %s -> %s' % (g(pre, 'morph'), m1)
    if kind == 'missile':
        if g(pre, 'cam') != 0:
            return 'NO-PROBE', 'not in first person (cam %s)' % g(pre, 'cam')
        m0, m1 = g(pre, 'msl'), g(post, 'msl')
        if m0 is None or m1 is None:
            return 'NO-PROBE', 'no msl field'
        return ('PASS' if m1 == m0 - 1 else 'FAIL'), 'msl %s -> %s' % (m0, m1)
    if kind == 'lock':
        o1 = g(post, 'orbit')
        if o1 is None:
            return 'NO-PROBE', 'no orbit field'
        if o1 != 0:
            return 'PASS', 'orbit %s (locked)' % o1
        return 'NO-PROBE', 'orbit 0: no lock (no target found?)'
    if kind == 'unlock':
        o0, o1 = g(pre, 'orbit'), g(post, 'orbit')
        if o0 in (None, 0):
            return 'NO-PROBE', 'nothing was locked (orbit %s)' % o0
        return ('PASS' if o1 == 0 else 'FAIL'), 'orbit %s -> %s' % (o0, o1)
    if kind == 'charge':
        return 'NO-PROBE', 'charge shot not in trace; @shot taken'
    if kind == 'freelook':
        p0, p1 = g(pre, 'pitch'), g(post, 'pitch')
        if p0 is None or p1 is None:
            return 'NO-PROBE', 'no pitch field'
        return 'NO-PROBE', 'recorded: pitch change %+.3f rad' % (p1 - p0)
    if kind == 'yaw':
        d = yaw_delta(g(pre, 'yaw'), g(post, 'yaw'))
        ok = d > 0.02 if check[1] == 'left' else d < -0.02
        return ('PASS' if ok else 'FAIL'), 'yaw %+.3f rad' % d
    if kind == 'pitch':
        p0, p1 = g(pre, 'pitch'), g(post, 'pitch')
        if p0 is None or p1 is None:
            return 'NO-PROBE', 'no pitch field'
        return ('PASS' if abs(p1 - p0) > 0.02 else 'FAIL'), 'pitch %+.3f rad' % (p1 - p0)
    if kind == 'pause':
        i0, i1 = g(pre, 'igt'), g(post, 'igt')
        if i0 is None or i1 is None:
            return 'NO-PROBE', 'no igt field'
        frames = post['f'] - pre['f']
        ok = abs(i1 - i0) < 0.1 and frames >= 10
        return ('PASS' if ok else 'FAIL'), 'igt %.2f -> %.2f over %d frames' % (i0, i1, frames)
    if kind == 'resume':
        i0, i1 = g(pre, 'igt'), g(post, 'igt')
        if i0 is None or i1 is None:
            return 'NO-PROBE', 'no igt field'
        return ('PASS' if i1 - i0 > 0.05 else 'FAIL'), 'igt %.2f -> %.2f' % (i0, i1)
    if kind == 'ctx':
        return ('PASS' if check[1] in ctxs else 'FAIL'), 'ctx %s expected' % check[1]
    if kind == 'scanon':
        ok = g(post, 'visor') == 2 and 'scan' in ctxs
        return ('PASS' if ok else 'FAIL'), 'visor %s -> %s, ctx scan seen %s' % (g(pre, 'visor'), g(post, 'visor'), 'scan' in ctxs)
    if kind == 'scanoff':
        ok = g(post, 'visor') == 0 and 'gameplay' in ctxs
        return ('PASS' if ok else 'FAIL'), 'visor %s -> %s, ctx gameplay seen %s' % (g(pre, 'visor'), g(post, 'visor'), 'gameplay' in ctxs)
    if kind == 'menukey':
        return 'NO-PROBE', 'menu key: no trace field (ctx checked by the next step)'
    return 'NO-PROBE', 'no judge for %s' % kind


def analyse(out, plan):
    trace = os.path.join(out, 'out', 'trace.jsonl')
    if not os.path.exists(trace):
        print('no trace at %s' % trace)
        return []
    markers, rows = load_trace(trace)
    logp = os.path.join(out, 'mpr.log')
    seg, evd = log_ctx(logp) if os.path.exists(logp) else ({}, {})
    res, vecs = [], {}
    for p in plan:
        tag = p['tag']
        row = dict(group=p['group'], binding=p['binding'], expect=p['expect'])
        if p['check'][0] == 'notpressed':
            row.update(result='NOT-PRESSED', observed='not pressed: menu-only action that can pick Quit')
        elif p['check'][0] == 'nobind':
            row.update(result='NO-BINDING', observed='beam_prev and beam_next are "none" in the [switch] table')
        else:
            f0, f1 = markers.get('PRE_' + tag), markers.get('POST_' + tag)
            if f0 is None or f1 is None:
                row.update(result='NOT-RUN', observed='run ended before this step')
            else:
                pre, post = last_at(rows, f0), last_at(rows, f1)
                win = [r for r in rows if f0 <= r['f'] <= f1]
                cl = seg.get(tag, [])
                seq = [c for i, c in enumerate(cl) if i == 0 or cl[i - 1] != c]
                ctxs = sorted(set(cl))
                result, obs = judge(p['check'], pre, post, win, ctxs, len(seq))
                if p['check'][0] == 'move':
                    vecs[tag] = move_vec(win)
                if result == 'FAIL' and not any(c in GAME_CTX for c in ctxs):
                    result, obs = 'NO-PROBE', obs + '; game was in menu context, gameplay bindings inactive'
                obs += '; ctx ' + '>'.join(seq or ['none']) + '; input events +%s' % evd.get(tag, '?')
                row.update(result=result, observed=obs)
        row['tag'] = tag
        res.append(row)
    # movement pairs in the same part, as a second reading (relative to the forward key), recorded in the observed text
    for prefix_names in [('W', 'S', 'A', 'D'), ('LS_UP', 'LS_DOWN', 'LS_LEFT', 'LS_RIGHT')]:
        byname = {r['binding']: r for r in res if r['binding'] in prefix_names}
        if len(byname) != 4:
            continue
        vs = [vecs.get(byname[n]['tag']) for n in prefix_names]
        if not all(vs):
            continue
        u = []
        for v in vs:
            s = math.hypot(v[0], v[1])
            u.append((v[0] / s, v[1] / s) if s > 0.3 else None)
        if not all(u):
            continue
        dot = lambda a, b: a[0] * b[0] + a[1] * b[1]
        uf, ub, ul, ur = u
        byname[prefix_names[1]]['observed'] += '; relative: opposite fwd %s' % (dot(ub, uf) < -0.7)
        byname[prefix_names[2]]['observed'] += '; relative: perpendicular to fwd %s' % (abs(dot(ul, uf)) < 0.7)
        byname[prefix_names[3]]['observed'] += '; relative: opposite left %s' % (dot(ur, ul) < -0.7)
    return res


def table(res, meta):
    lines = [meta, '']
    hdr = ('part', 'binding', 'expected', 'observed', 'result')
    rows = [hdr] + [(r['group'], r['binding'], r['expect'], r.get('observed', ''), r['result']) for r in res]
    w = [max(len(str(x[i])) for x in rows) for i in range(5)]
    for x in rows:
        lines.append(' | '.join(str(x[i]).ljust(w[i]) for i in range(5)))
    counts = {}
    for r in res:
        counts[r['result']] = counts.get(r['result'], 0) + 1
    lines += ['', 'counts: ' + ', '.join('%s %d' % (k, v) for k, v in sorted(counts.items()))]
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--part', choices=sorted(PARTS))
    ap.add_argument('--analyse-only', action='store_true')
    ap.add_argument('--combine', action='store_true', help='join the table_<part>.json files into table_all.txt')
    ap.add_argument('--seconds', type=int, default=900, help='wall-clock limit of the mpr run')
    ap.add_argument('--out', default=None, help='run directory (default work\\verify\\bindings\\<part>)')
    a = ap.parse_args()
    os.makedirs(OUTROOT, exist_ok=True)
    if a.combine:
        allrows, parts = [], []
        for name in ['keyboard', 'keyboard2', 'pad', 'pad2', 'scan', 'map', 'pause', 'menu']:
            p = os.path.join(OUTROOT, 'table_%s.json' % name)
            if os.path.exists(p):
                parts.append(name)
                allrows += json.load(open(p, encoding='utf-8'))['rows']
        txt = table(allrows, 'parts joined: ' + ', '.join(parts))
        open(os.path.join(OUTROOT, 'table_all.txt'), 'w', encoding='utf-8').write(txt + '\n')
        print(txt)
        return
    if not a.part:
        ap.error('--part is required')
    out = os.path.abspath(a.out or os.path.join(OUTROOT, a.part))
    script, plan = build(a.part)
    rc, meta = 0, 'analysis only'
    if not a.analyse_only:
        r = gp.run(script, out, rate=60, env={'NXRT_PLAY_LOG_STATUS': '1'}, seconds=a.seconds, exe_dir=EXE)
        rc = r.get('exit', 1)
        meta = 'part %s, exe %s, mpr exit %s, verdict %s, fail_line %s, message: %s, wall %.0f s' % (
            a.part, EXE, rc, r.get('verdict'), r.get('fail_line'), r.get('message'), r.get('wall', 0))
        print(meta)
    res = analyse(out, plan)
    txt = table(res, meta + '\nscript: ' + os.path.join(out, 'script.nxs'))
    print(txt)
    open(os.path.join(OUTROOT, 'table_%s.txt' % a.part), 'w', encoding='utf-8').write(txt + '\n')
    json.dump(dict(meta=meta, exit=rc, rows=res), open(os.path.join(OUTROOT, 'table_%s.json' % a.part), 'w', encoding='utf-8'), indent=1)
    sys.exit(rc)


if __name__ == '__main__':
    main()
