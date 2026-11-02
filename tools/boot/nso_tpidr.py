"""List MRS/MSR of TPIDR_EL0/TPIDRRO_EL0 sites in an NSO with the next few instructions. usage: nso_tpidr.py <nso> [context]"""
import sys, struct, lz4.block
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
d = open(sys.argv[1], 'rb').read(); ctx = int(sys.argv[2]) if len(sys.argv) > 2 else 5
off, mem, size = struct.unpack_from('<III', d, 0x10); fsz = struct.unpack_from('<I', d, 0x60)[0]
text = lz4.block.decompress(d[off:off + fsz], uncompressed_size=size)
cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM); cs.skipdata = True
names = {0xd53bd060: 'mrs tpidrro_el0', 0xd53bd040: 'mrs tpidr_el0', 0xd51bd040: 'msr tpidr_el0'}
words = struct.unpack('<%dI' % (len(text) // 4), text)
for i, w in enumerate(words):
    k = w & 0xffffffe0 if (w & 0xffe0ffe0) != 0xd51bd040 else 0xd51bd040
    k = (w & 0xffffffe0)
    if (w & 0xfffffe0) and (w & 0xffffffe0) in names or (w & 0xffffffe0) == 0xd51bd040 and False:
        pass
    if (w & 0xffffffe0) in names or (w & 0xffffffe0) == 0xd51bd040:
        print('%x: %s' % (i * 4, names.get(w & 0xffffffe0, 'msr tpidr_el0')))
        for ins in cs.disasm(text[i * 4:i * 4 + 4 * (ctx + 1)], i * 4):
            print('    %x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
