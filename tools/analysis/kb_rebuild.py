#!/usr/bin/env python
"""Rebuild the whole knowledge base in order: headers -> patches -> fragment check -> merge -> (optional) validate against main.elf.

  kb_rebuild.py                                  # rebuild docs/mpr from the fragments, no binary
  kb_rebuild.py --elf work/elf/main.elf          # ... and validate + write verification blocks back into the JSON
Fragments written by hand (port_pairs_*, rando_*, practice_mod) are inputs and are never touched here.
"""
import argparse, pathlib, subprocess, sys

A = pathlib.Path(__file__).resolve().parent
ROOT = A.parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('--elf')
a = ap.parse_args()
steps = [[str(A / 'kb_parse_headers.py'), str(ROOT / 'docs/mpr/frag')],
         [str(A / 'kb_build_patches.py')]]
for s in steps:
    subprocess.run([sys.executable] + s, check=True)
frags = sorted(str(p) for p in (ROOT / 'docs/mpr/frag').glob('*.json'))
subprocess.run([sys.executable, str(A / 'kb_check.py')] + frags, check=True)
subprocess.run([sys.executable, str(A / 'kb_merge.py')], check=True)
if a.elf:
    r = subprocess.run([sys.executable, str(A / 'validate_kb.py'), '--elf', a.elf, '--quiet', '--write-back', '--report', str(ROOT / 'work/kb/validate_report.json')])
    sys.exit(0)  # FAIL entries are findings, not a build error
