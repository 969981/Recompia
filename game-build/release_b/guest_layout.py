# SPDX-License-Identifier: GPL-3.0-or-later
"""The lifter's inputs moved to another guest base, for a target whose guest address space does not start at the canonical
0x7000000000 (runtime/include/nxrt/memory.h kCanonicalBase; runtime/cmake/guest_layout.cmake is where a target chooses another).

The game config's module bases and its site lists are written for the canonical layout. The lifter takes the actual module base
(lift --base) and the site lists (lift --sites) on its command line, and every other address it reads is module-relative, so a
build at another base needs no change to the lifter: this script gives the moved module table and writes moved copies of the
site lists. A build at the canonical base never runs it, so its stamps, its lift and its module header stay as they are.

    python guest_layout.py build-info --game G --arch A --guest-base 0x.. --out-dir DIR    nxrecomp build-info, moved; the site
                                                                                          lists are DIR/<module>.sites
    python guest_layout.py module-header --game G --guest-base 0x.. --out FILE           nxrecomp module-header, moved
Run from recomp/, like `python -m nxrecomp` (recomp/cmake/NxRecomp.cmake does, when NX_GUEST_BASE is set). pkp_build.py loads it
when its kit has sdk/guest_base.txt.
"""
import argparse
import copy
import json
import os
import sys

CANONICAL_BASE = 0x7000000000     # nxrt/memory.h kCanonicalBase: the base every config value and site list is written for


def read_base(path):
    """The guest base in a guest_base.txt (one hex number, written by game-build/CMakeLists.txt)."""
    with open(path, encoding='utf-8') as f:
        return int(f.read().strip(), 16)


def slide(cfg, base):
    """How far a build at guest base `base` moves the config's addresses (memory.h kGuestSlide)."""
    if base % 0x100000000:
        raise ValueError('guest base %#x is not on a 4 GiB boundary (nxrt/memory.h)' % base)
    # Limit: overrides are absolute guest addresses that only the lifter itself could move (the game config has none)
    if cfg.overrides:
        raise ValueError('game config %s has overrides: they are not moved to guest base %#x' % (cfg.name, base))
    return base - CANONICAL_BASE


def placed(cfg, by):
    """A copy of the game config with every module base moved by `by` (the config itself is left as it is)."""
    out = copy.copy(cfg)
    out.modules = []
    for m in cfg.modules:
        c = copy.copy(m)
        c.base = m.base + by
        out.modules.append(c)
    return out


def move_sites(src, dst, by):
    """Write the site list src (nxrecomp/sites.py: `<kind> <address> ...` per line, `#` comments) to dst with every address moved
    by `by`; lines and comments stay where they were, so the lifter's line numbers still point at the right site. Returns dst."""
    out = []
    with open(src, encoding='utf-8') as f:
        for line in f:
            code, sep, note = line.rstrip('\n').partition('#')
            t = code.split()
            if len(t) >= 2:
                t[1] = '%#x' % (int(t[1], 16) + by)
                code = ' '.join(t) + (' ' if sep else '')
            out.append(code + sep + note + '\n')
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(out)
    return dst


def main(argv=None):
    sys.path.insert(0, os.getcwd())       # recomp/, as for `python -m nxrecomp`
    from nxrecomp import modheader
    from nxrecomp.__main__ import build_info
    from nxrecomp.config import load_game
    ap = argparse.ArgumentParser(prog='guest_layout.py')
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('build-info', 'module-header'):
        p = sub.add_parser(name)
        p.add_argument('--game', required=True)
        p.add_argument('--guest-base', required=True, type=lambda s: int(s, 16))
        if name == 'build-info':
            p.add_argument('--arch', default='')
            p.add_argument('--out-dir', required=True)
        else:
            p.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    cfg = load_game(a.game)
    by = slide(cfg, a.guest_base)
    if a.cmd == 'module-header':
        modheader.write(placed(cfg, by).modules, a.out)
        return 0
    info = build_info(a.game, a.arch)
    for m in info['modules']:
        m['base'] = '0x%x' % (int(m['base'], 16) + by)
        if m['sites']:
            dst = os.path.join(os.path.abspath(a.out_dir), m['name'] + '.sites')
            m['sites'] = move_sites(os.path.join('games', a.game, m['sites']), dst, by)
    print(json.dumps(info))
    return 0


if __name__ == '__main__':
    sys.exit(main())
