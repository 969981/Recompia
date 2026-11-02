"""List the SVC numbers used by each NSO (svc #n sites) and which of them the runtime has no handler for.
  tools\.venv\Scripts\python tools\boot\svc_sites.py   (reads game\exefs\*, runtime\src\kernel\svc_*.cpp for the registered numbers)"""
import os, re, struct, lz4.block, collections, glob
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
reg = set()
for f in glob.glob(os.path.join(ROOT, 'runtime', 'src', 'kernel', 'svc_*.cpp')) + glob.glob(os.path.join(ROOT, 'runtime', 'src', 'services', '*.cpp')):
    for m in re.finditer(r'svc_register\(\s*(0x[0-9A-Fa-f]+)', open(f, errors='replace').read()): reg.add(int(m.group(1), 16))
names = {}
inc = os.path.join(ROOT, 'runtime', 'src', 'kernel', 'svc_names.inc')
for m in re.finditer(r'NXRT_SVC\(\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*"(\w+)"', open(inc).read()): names[int(m.group(1), 0)] = m.group(2)
for nso in ('rtld', 'main', 'sdk', 'subsdk0'):
    d = open(os.path.join(ROOT, 'game', 'exefs', nso), 'rb').read()
    off, mem, size = struct.unpack_from('<III', d, 0x10); fsz = struct.unpack_from('<I', d, 0x60)[0]
    t = lz4.block.decompress(d[off:off + fsz], uncompressed_size=size)
    c = collections.Counter()
    for i in range(0, len(t) - 3, 4):
        w = struct.unpack_from('<I', t, i)[0]
        if (w & 0xffe0001f) == 0xd4000001: c[(w >> 5) & 0xffff] += 1
    print('%s: %d sites, %d distinct' % (nso, sum(c.values()), len(c)))
    for n in sorted(c):
        print('   0x%02x %-34s x%-3d %s' % (n, names.get(n, '?'), c[n], '' if n in reg else 'NO HANDLER'))
