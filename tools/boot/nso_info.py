"""NSO header dump + optional disassembly of a range. usage: nso_info.py <nso> [--dis off count] (uses capstone, lz4 from tools/.venv)"""
import sys, struct, lz4.block
p = sys.argv[1]
d = open(p, 'rb').read()
magic, ver, _, flags = struct.unpack_from('<4sIII', d, 0)
segs = []
for i in range(3):
    off, mem, size = struct.unpack_from('<III', d, 0x10 + i * 16)
    segs.append((off, mem, size))
bss = struct.unpack_from('<I', d, 0x3c)[0]
fsz = struct.unpack_from('<III', d, 0x60)
print(p, 'flags', hex(flags), 'segs(fileoff,memoff,decomp)', [(hex(a), hex(b), hex(c)) for a, b, c in segs], 'bss', hex(bss), 'filesizes', [hex(x) for x in fsz])
img = bytearray()
for i, (off, mem, size) in enumerate(segs):
    raw = d[off:off + fsz[i]]
    if flags & (1 << i): raw = lz4.block.decompress(raw, uncompressed_size=size)
    img.extend(b'\0' * (mem - len(img))); img.extend(raw)
mod0 = struct.unpack_from('<I', img, 4)[0]
print('mod0 at', hex(mod0), img[mod0:mod0 + 4], [hex(x) for x in struct.unpack_from('<6I', img, mod0 + 4)])
if len(sys.argv) > 2 and sys.argv[2] == '--dis':
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    def _cs():
        c = Cs(CS_ARCH_ARM64, CS_MODE_ARM); c.skipdata = True; return c
    off = int(sys.argv[3], 0); n = int(sys.argv[4], 0)
    for ins in _cs().disasm(bytes(img[off:off + n * 4]), off):
        print('%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
