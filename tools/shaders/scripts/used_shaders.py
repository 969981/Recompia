#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the used-shader list the launcher translates first (game-build/release_b/used_shaders.txt).

The list holds only code hashes (the first 16 hex of SHA-1 of a shader's code block, the key of results.tsv and of the
.spv files), never shader code or other game data. The launcher's Set up translates the blocks it names first and turns
Play on as soon as they are done; the rest of the corpus follows in the background (launcher/src-tauri/src/shaders.rs).

Sources, merged in this order (a hash keeps its first place):
  --list FILE      an existing list (keep the shipped one first, so a new run only appends);
  --order FILE     a `shader_order.txt` from a play run (the runtime appends every shader it uses there, in first-use
                   order; [nvn] shader_order). Repeat for each tester's file;
  --manifest FILE  with --source NAME: every block a corpus manifest.tsv (tools/shaders/ampere/extract_corpus.py, column
                   `source`) lists for that source file, e.g. `main` or a small RDB file the game loads at boot.
Lines starting with '#' and anything that is not 16 lowercase hex are ignored on input.

Pokopia 2.0.1a list, as shipped:
  python -I tools/shaders/scripts/used_shaders.py --out game-build/release_b/used_shaders.txt \
      --order <play run 1>/shader_order.txt --order <play run 2>/shader_order.txt \
      --manifest <corpus>/manifest.tsv --source main --source 0x05d525ae.fdata --source 0x2943e4da.fdata --source 0x52482d03.fdata
To add a tester's runs later: --list game-build/release_b/used_shaders.txt --order <their shader_order.txt> --out <same file>.
"""
import argparse, os, re, sys

HEX16 = re.compile(r'^[0-9a-f]{16}$')


def read_hashes(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            h = line.strip().lower()
            if HEX16.match(h):
                yield h


def manifest_hashes(path, sources):
    with open(path, encoding='utf-8') as f:
        header = f.readline().rstrip('\n').split('\t')
        src, code = header.index('source'), header.index('code_sha1_16')
        for line in f:
            c = line.rstrip('\n').split('\t')
            if len(c) > max(src, code) and c[src] in sources and HEX16.match(c[code]):
                yield c[code]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--list', action='append', default=[])
    ap.add_argument('--order', action='append', default=[])
    ap.add_argument('--manifest')
    ap.add_argument('--source', action='append', default=[])
    a = ap.parse_args()
    if a.source and not a.manifest:
        ap.error('--source needs --manifest')
    seen, out, counts = set(), [], []

    def add(label, it):
        n = 0
        for h in it:
            if h not in seen:
                seen.add(h)
                out.append(h)
                n += 1
        counts.append(f'{label}: +{n}')

    for p in a.list:
        if os.path.isfile(p):
            add(f'list {os.path.basename(p)}', read_hashes(p))
    for p in a.order:
        add(f'order {os.path.basename(os.path.dirname(os.path.abspath(p)))}/{os.path.basename(p)}', read_hashes(p))
    if a.manifest:
        for s in a.source:
            add(f'source {s}', manifest_hashes(a.manifest, {s}))
    tmp = a.out + '.tmp'
    with open(tmp, 'w', newline='\n', encoding='utf-8') as f:
        f.write('# Shaders the launcher translates first (hashes only: first 16 hex of SHA-1 of each code block).\n')
        f.write('# Made by tools/shaders/scripts/used_shaders.py from play runs\' shader_order.txt and the boot-time source files.\n')
        f.write(''.join(h + '\n' for h in out))
    os.replace(tmp, a.out)
    print('\n'.join(counts + [f'{len(out)} hashes -> {a.out}']))


if __name__ == '__main__':
    sys.exit(main())
