"""Make a smaller shader corpus from a big one: all blocks whose id starts with --keep-prefix (e.g. arc) plus a seeded random
sample of the others. Output layout is the same (manifest.tsv, code_/ctrl_ files), so run_oracle.py takes it as is.
usage: python sample_corpus.py <corpus> --out <dir> --n 8000 [--seed 1] [--keep-prefix arc]"""
import argparse, csv, os, random, shutil

ap = argparse.ArgumentParser()
ap.add_argument('corpus')
ap.add_argument('--out', required=True)
ap.add_argument('--n', type=int, default=8000)
ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--keep-prefix', default='arc')
a = ap.parse_args()
rows = list(csv.reader(open(os.path.join(a.corpus, 'manifest.tsv')), delimiter='\t'))
head, body = rows[0], rows[1:]
by_hash = {}
for r in body:
    by_hash.setdefault(r[10], r)
keep = {h for h, r in by_hash.items() if r[0].startswith(a.keep_prefix)}
rest = sorted(h for h in by_hash if h not in keep)
random.Random(a.seed).shuffle(rest)
keep |= set(rest[:a.n])
os.makedirs(a.out, exist_ok=True)
with open(os.path.join(a.out, 'manifest.tsv'), 'w', newline='\n') as m:
    m.write('\t'.join(head) + '\n')
    for r in body:
        if r[10] in keep:
            m.write('\t'.join(r) + '\n')
for h in keep:
    for f in (by_hash[h][11], by_hash[h][12]):
        shutil.copyfile(os.path.join(a.corpus, f), os.path.join(a.out, f))
print(f'{len(keep)} distinct blocks of {len(by_hash)} -> {a.out}')
