"""Tiny RFRM reader/lister. usage: python rfrm.py <file> -- prints the form tree (chunks with sizes)."""
import struct, sys

def parse(buf, off=0, end=None, depth=0, out=None, maxdepth=6):
    out = out if out is not None else []
    end = len(buf) if end is None else end
    while off + 24 <= end:
        cid = buf[off:off+4]
        if cid == b'RFRM':
            size = struct.unpack_from('<Q', buf, off+4)[0]
            fid = buf[off+20:off+24].decode('latin1'); rv, wv = struct.unpack_from('<II', buf, off+24)
            out.append((depth, 'RFRM', fid, off, size, f'rv={rv} wv={wv}'))
            if depth < maxdepth: parse(buf, off+32, off+32+size, depth+1, out, maxdepth)
            off += 32 + size
        else:
            size = struct.unpack_from('<Q', buf, off+4)[0]; unk = struct.unpack_from('<I', buf, off+12)[0]; skip = struct.unpack_from('<Q', buf, off+16)[0]
            out.append((depth, 'CHUNK', cid.decode('latin1'), off + 24 + skip, size, f'unk={unk} skip={skip}'))
            off += 24 + skip + size
    return out

if __name__ == '__main__':
    b = open(sys.argv[1], 'rb').read()
    for d, k, i, o, s, x in parse(b):
        print('  ' * d + f'{k} {i} data@0x{o:x} size=0x{s:x} {x}')
