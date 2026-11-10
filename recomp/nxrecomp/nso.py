# SPDX-License-Identifier: GPL-3.0-or-later
"""Switch NSO reading (LZ4 segments); adapted from smo-recomp tools/extract/nso.py (GPL-3.0-or-later), which in turn is adapted from ssbu-recomp tools/extract (nso2elf.py).
"""
import hashlib
import struct


def nso_info(d):
    """Segments (decompressed), build id and integrity of an NSO."""
    import lz4.block
    if d[:4] != b'NSO0':
        raise ValueError('not an NSO executable')
    flags = struct.unpack_from('<I', d, 12)[0]
    segs = []
    for i, (ho, cso) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
        foff, moff, size = struct.unpack_from('<III', d, ho)
        csz = struct.unpack_from('<I', d, cso)[0]
        raw = d[foff:foff + csz]
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        assert len(raw) == size
        ok = hashlib.sha256(raw).digest() == d[0xA0 + i * 0x20:0xC0 + i * 0x20]
        segs.append({'vaddr': moff, 'size': size, 'data': raw, 'sha_ok': ok})
    return {'segments': segs, 'bss': struct.unpack_from('<I', d, 0x3c)[0], 'build_id': d[0x40:0x60].hex()}
