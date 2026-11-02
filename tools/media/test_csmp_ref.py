"""Unit tests for csmp_ref.py: a hand-computed synthetic stream plus two real CSMP files when present."""
import os
import struct
import tempfile
import unittest

import numpy as np

import csmp_ref as cr

def _project_root():
    """The project folder: NXRT_ROOT, else the nearest parent of this file that holds both tools/ and work/."""
    import os, pathlib
    env = os.environ.get('NXRT_ROOT')
    if env:
        return pathlib.Path(env).as_posix()
    here = pathlib.Path(__file__).resolve()
    for p in here.parents:
        if (p / 'tools').is_dir() and (p / 'work').is_dir():
            return p.as_posix()
    return here.parents[2].as_posix()


REAL_DIR = _project_root() + "/work/survey/paks/Preload/DuplicateData"

# Synthetic stream: 3 frames (42 samples).
# coef pair 0 = (0, 0); pair 1 = (2048, 0); pair 2 = (0, 2048); rest 0.
#  frame 1: header 0x13 (pair 1, scale 1<<3 = 8), nibbles 1 2 3 4 -1 -2 0 -8 7 -1 -8 0 0 1
#           sample = nibble*8 + previous sample  -> 8 24 48 80 72 56 56 -8 48 40 -24 -24 -24 -16
#  frame 2: header 0x22 (pair 2, scale 4), nibbles 7 7 -8 -8 0 0 0 0 1 1 -7 -7 7 -1
#           sample = nibble*4 + sample two back  -> 4 12 -28 -20 -28 -20 -28 -20 -24 -16 -52 -44 -24 -48
#  frame 3: header 0x0F (pair 0, scale 1<<15), nibbles 7 -1 -8 0 ... 0
#           sample = clamp16(nibble*32768)       -> 32767 -32768 -32768 0 x 11
EXPECTED = (
    [8, 24, 48, 80, 72, 56, 56, -8, 48, 40, -24, -24, -24, -16]
    + [4, 12, -28, -20, -28, -20, -28, -20, -24, -16, -52, -44, -24, -48]
    + [32767, -32768, -32768] + [0] * 11
)
COEFS = [0, 0, 2048, 0, 0, 2048] + [0] * 10
SYNTH_FRAMES = bytes([0x13, 0x12, 0x34, 0xFE, 0x08, 0x7F, 0x80, 0x01]) \
    if False else None


def synth_adpcm():
    frames = [
        [0x13, 0x12, 0x34, 0xFE, 0x08, 0x7F, 0x80, 0x01],
        [0x22, 0x77, 0x88, 0x00, 0x00, 0x11, 0x99, 0x7F],
        [0x0F, 0x7F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00],
    ]
    return bytes(b for fr in frames for b in fr)


def build_csmp(path, sample_count, adpcm, coefs, rate=48000, hist1=0, hist2=0):
    """Write a minimal CSMP file laid out like the real ones (FMTA, DATA with 16-byte prefix)."""
    prefix = bytes([0, 0, 0, 0, 1, 0, 0, 0]) + bytes(8)
    hdr = bytearray(0x60)
    nib = sample_count * 16 // 14 + 1
    struct.pack_into("<IIIHHIII", hdr, 0, sample_count, nib, rate, 0, 0, 2, sample_count * 16 // 14, 2)
    struct.pack_into("<16h", hdr, 0x1C, *coefs)
    struct.pack_into("<HHhhHhh", hdr, 0x3C, 0, 0, hist1, hist2, 0, 0, 0)
    payload = prefix + bytes(hdr) + adpcm
    fmta = b"FMTA" + struct.pack("<I", 5) + bytes([0, 0, 0, 0, 1])
    data = b"DATA" + struct.pack("<I", len(payload)) + payload
    body = b"CSMP" + bytes(12) + fmta + bytes(11) + bytes([1, 4, 0, 0, 0]) + data   # +0x18 of the chunk: channel count 1, layout mask 4 (centre)
    with open(path, "wb") as f:
        f.write(b"RFRM" + struct.pack("<I", len(body)) + bytes(12) + body)


class SyntheticDecodeTests(unittest.TestCase):
    def test_hand_computed_values(self):
        out = cr.decode_dsp_adpcm(synth_adpcm(), COEFS, 42, 0, 0)
        self.assertEqual(out.dtype, np.int16)
        self.assertEqual(out.tolist(), EXPECTED)

    def test_truncates_to_sample_count(self):
        out = cr.decode_dsp_adpcm(synth_adpcm(), COEFS, 40, 0, 0)
        self.assertEqual(out.tolist(), EXPECTED[:40])

    def test_short_input_rejected(self):
        with self.assertRaises(cr.CsmpError):
            cr.decode_dsp_adpcm(synth_adpcm()[:16], COEFS, 42, 0, 0)

    def test_parse_synthetic_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "syn.csmp")
            build_csmp(p, 42, synth_adpcm(), COEFS, rate=32000, hist1=0, hist2=0)
            info = cr.parse_csmp(p)
            self.assertEqual(info["channels"], 1)
            self.assertEqual(info["rate"], 32000)
            self.assertEqual(info["sample_count"], 42)
            self.assertEqual(info["coefs"], COEFS)
            self.assertTrue(info["nibble_ok"])
            self.assertEqual(info["adpcm_bytes"][:24], synth_adpcm())
            pcm = cr.decode_dsp_adpcm(info["adpcm_bytes"], info["coefs"], info["sample_count"],
                                      info["hist1"], info["hist2"])
            self.assertEqual(pcm.tolist(), EXPECTED)

    def test_bad_format_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "bad.csmp")
            build_csmp(p, 42, synth_adpcm(), COEFS)
            with open(p, "rb") as f:
                raw = bytearray(f.read())
            # DSP format field: DATA payload start + 0x10 + 0x0E; payload starts after 'DATA'+size
            at = raw.find(b"DATA") + 8 + 0x10 + 0x0E
            raw[at] = 1
            with open(p, "wb") as f:
                f.write(bytes(raw))
            with self.assertRaises(cr.CsmpError):
                cr.parse_csmp(p)


def real_files(n=2):
    if not os.path.isdir(REAL_DIR):
        return []
    names = sorted(x for x in os.listdir(REAL_DIR) if x.upper().endswith(".CSMP"))
    return [os.path.join(REAL_DIR, x) for x in names[:n]]


class RealFileTests(unittest.TestCase):
    def test_two_real_files_parse_and_are_consistent(self):
        files = real_files(2)
        if not files:
            self.skipTest("real CSMP folder not present: %s" % REAL_DIR)
        for p in files:
            with self.subTest(file=os.path.basename(p)):
                info = cr.parse_csmp(p)
                self.assertEqual(info["channels"], 1)
                self.assertTrue(info["nibble_ok"],
                                "nibble count %d vs samples %d" % (info["nibble_count"], info["sample_count"]))
                frames = -(-info["sample_count"] // 14)
                self.assertGreaterEqual(len(info["adpcm_bytes"]), frames * 8)
                self.assertEqual(info["data_payload"][:16].hex(), "00000000010000000000000000000000")
                pcm = cr.decode_dsp_adpcm(info["adpcm_bytes"], info["coefs"], info["sample_count"],
                                          info["hist1"], info["hist2"])
                self.assertEqual(len(pcm), info["sample_count"])
                self.assertEqual(pcm.dtype, np.int16)


if __name__ == "__main__":
    unittest.main()
