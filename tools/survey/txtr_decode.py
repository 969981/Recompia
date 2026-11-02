"""TXTR reader: header + META + LZSS-decompressed *tiled* (block-linear) GPU buffer.

Used by txtr_pipeline.py. Layout knowledge from refs/retrotool/lib/src/format/txtr.rs and lzss.rs and
refs/PrimeRemasterStructs/NTextureFormat.bt (re-implemented, nothing copied).
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from txtr_tally import FORMATS, KINDS, fmt_info


def lzss(inp, out_len, mode):
    g = 1 << (mode - 1)
    out = bytearray(out_len)
    o = i = 0
    n = len(inp)
    hdr = grp = 0
    while i < n:
        if grp == 0:
            hdr = inp[i]
            i += 1
            grp = 8
        if not hdr & 0x80:
            out[o:o + g] = inp[i:i + g]
            i += g
            o += g
        else:
            b0 = inp[i]
            count = (b0 >> 4) + (4 - mode)
            length = (((b0 & 0xF) << 8) | inp[i + 1]) << (mode - 1)
            i += 2
            nb = count * g
            s = o - length
            if length >= nb:
                out[o:o + nb] = out[s:s + nb]
            else:
                pat = bytes(out[s:o])
                out[o:o + nb] = (pat * ((nb + length - 1) // length))[:nb]
            o += nb
        hdr = (hdr << 1) & 0xFF
        grp -= 1
    if o != out_len:
        raise ValueError(f'lzss produced {o} of {out_len} bytes')
    return out


def decompress(buf, out_len, modes):
    mode = struct.unpack_from('<I', buf, 0)[0]
    modes[mode] = modes.get(mode, 0) + 1
    data = memoryview(buf)[4:]
    if mode == 0:
        if len(data) != out_len:
            raise ValueError('raw buffer size mismatch')
        return bytes(data)
    if mode in (1, 2, 3):
        return lzss(bytes(data), out_len, mode)
    raise ValueError(f'unknown compression mode {mode}')


def load(path, modes=None):
    """-> (header dict, tiled GPU buffer bytes)"""
    modes = modes if modes is not None else {}
    b = open(path, 'rb').read()
    size = struct.unpack_from('<Q', b, 4)[0]
    h = 56
    kind, fmt, w, hh, layers, tile_mode, swz, nmip = struct.unpack_from('<8I', b, h)
    mips = struct.unpack_from('<%dI' % nmip, b, h + 32)
    head = dict(kind=KINDS[kind], fmt=FORMATS[fmt], w=w, h=hh, layers=layers, mips=nmip, mip_sizes=list(mips))
    foot = 32 + size
    fsize = struct.unpack_from('<Q', b, foot + 4)[0]
    o = foot + 32
    meta = None
    while o + 24 <= foot + 32 + fsize:
        cid = b[o:o + 4]
        csz = struct.unpack_from('<Q', b, o + 4)[0]
        sk = struct.unpack_from('<Q', b, o + 16)[0]
        if cid == b'META':
            meta = b[o + 24 + sk:o + 24 + sk + csz]
        o += 24 + sk + csz
    dec_size = struct.unpack_from('<I', meta, 20)[0]
    ninfo = struct.unpack_from('<I', meta, 24)[0]
    p = 28
    infos = []
    for _ in range(ninfo):
        idx, off, sz = struct.unpack_from('<BII', meta, p)
        infos.append((idx, off, sz))
        p += 9
    nbuf = struct.unpack_from('<I', meta, p)[0]
    p += 4
    out = bytearray(dec_size)
    for _ in range(nbuf):
        index, off, sz, doff, dsz = struct.unpack_from('<5I', meta, p)
        p += 20
        rd = next(i for i in infos if i[0] == index)
        comp = b[rd[1] + off: rd[1] + off + sz]
        out[doff:doff + dsz] = decompress(comp, dsz, modes)
    head['gpu_size'] = dec_size
    return head, bytes(out)
