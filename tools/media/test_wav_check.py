"""Unit tests for wav_check.py. Run from tools\\media with:

    ..\\.venv\\Scripts\\python -I -m unittest

Synthetic WAV files are written to a temporary directory.
"""

import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wav_check  # noqa: E402

RATE = 48000
SECONDS = 12
INT_FULL = 32767


def tone(rate, seconds, freq, amp, channels):
    """Float sine (frames, channels), same tone on every channel."""
    t = np.arange(int(rate * seconds)) / rate
    col = amp * np.sin(2.0 * np.pi * freq * t)
    return np.tile(col[:, None], (1, channels))


def to_int16(x):
    return np.clip(np.round(x * INT_FULL), -32768, 32767).astype("<i2")


def write_wav(path, int_samples, rate):
    """Plain WAVE_FORMAT_PCM file written by the wave module (correct header sizes)."""
    frames, channels = int_samples.shape
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.ascontiguousarray(int_samples, dtype="<i2").tobytes())


def write_extensible(path, int_samples, rate):
    """Hand-built WAVE_FORMAT_EXTENSIBLE file (16-bit PCM sub-format)."""
    frames, channels = int_samples.shape
    data = np.ascontiguousarray(int_samples, dtype="<i2").tobytes()
    block_align = channels * 2
    fmt_base = struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * block_align,
                           block_align, 16, 22, 16, 0x3F)
    # KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00AA00389B71}
    subformat = b"\x01\x00\x00\x00" + b"\x00\x00" + b"\x10\x00" + b"\x80\x00\x00\xaa\x00\x38\x9b\x71"
    fmt = fmt_base + subformat
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
            + b"data" + struct.pack("<I", len(data)) + data)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def run_cli(*args):
    """Run wav_check.main, return (exit code, stdout text)."""
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = wav_check.main(list(args))
    return code, out.getvalue()


class WavCheckTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.dir = cls._tmp.name

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def path(self, name):
        return os.path.join(self.dir, name)

    def check(self, path, **kw):
        return wav_check.check_file(path, **kw)

    # (a) clean stereo sine
    def test_a_clean_sine_passes(self):
        p = self.path("a_clean.wav")
        write_wav(p, to_int16(tone(RATE, SECONDS, 440.0, 0.5, 2)), RATE)
        res = self.check(p, rate=RATE, channels=2)
        self.assertEqual(res["verdict"], "PASS", res["failed"])
        self.assertEqual(res["failed"], [])
        self.assertAlmostEqual(res["whole"]["peak"], 0.5, delta=0.01)
        self.assertEqual(res["whole"]["active_fraction"], 1.0)
        self.assertEqual(res["whole"]["clicks"]["count"], 0)
        self.assertEqual(res["whole"]["dropouts"]["count"], 0)
        code, out = run_cli(p, "--rate", str(RATE), "--channels", "2")
        self.assertEqual(code, 0)
        self.assertTrue(out.startswith("PASS  a_clean.wav"), out)

    # (b) all-zero file
    def test_b_silence_fails_active_and_peak(self):
        p = self.path("b_silent.wav")
        write_wav(p, np.zeros((RATE * SECONDS, 2), dtype="<i2"), RATE)
        res = self.check(p, rate=RATE, channels=2)
        self.assertEqual(res["verdict"], "FAIL")
        self.assertTrue(any(f.startswith("active fraction") for f in res["failed"]), res["failed"])
        self.assertTrue(any(f.startswith("peak") for f in res["failed"]), res["failed"])
        code, _ = run_cli(p)
        self.assertEqual(code, 1)

    # (c) 20 hard clicks: one sample stepped by 0.9 of full scale against its sign
    def test_c_clicks_fail(self):
        p = self.path("c_clicks.wav")
        x = tone(RATE, SECONDS, 440.0, 0.5, 2)
        for i in range(20):
            n = int((0.25 + 0.5 * i) * RATE)
            v = x[n, 0]
            x[n, 0] = v + (-0.9 if v >= 0 else 0.9)
        write_wav(p, to_int16(x), RATE)
        res = self.check(p, rate=RATE, channels=2)
        self.assertEqual(res["verdict"], "FAIL")
        self.assertGreaterEqual(res["whole"]["clicks"]["count"], 20)
        self.assertTrue(any(f.startswith("clicks") for f in res["failed"]), res["failed"])
        first = res["whole"]["clicks"]["first"]
        self.assertLessEqual(len(first), 5)
        self.assertAlmostEqual(first[0]["time_s"], 0.25, delta=0.001)

    # (d) 5 gaps of 20 ms exact zeros inside a sine
    def test_d_dropouts_fail(self):
        p = self.path("d_gaps.wav")
        x = tone(RATE, SECONDS, 440.0, 0.5, 2)
        gap = int(0.020 * RATE)
        for t in (1.0, 3.0, 5.0, 7.0, 9.0):
            s = int(t * RATE)
            x[s:s + gap, :] = 0.0
        write_wav(p, to_int16(x), RATE)
        res = self.check(p, rate=RATE, channels=2)
        self.assertEqual(res["verdict"], "FAIL")
        self.assertEqual(res["whole"]["dropouts"]["count"], 5)
        self.assertTrue(any(f.startswith("dropouts") for f in res["failed"]), res["failed"])

    # (e) wrong sample rate
    def test_e_wrong_rate_fails(self):
        p = self.path("e_rate.wav")
        write_wav(p, to_int16(tone(44100, SECONDS, 440.0, 0.5, 2)), 44100)
        res = self.check(p, rate=RATE, channels=2)
        self.assertEqual(res["verdict"], "FAIL")
        self.assertEqual(res["sample_rate"], 44100)
        self.assertTrue(any(f.startswith("sample rate 44100") for f in res["failed"]), res["failed"])
        code, out = run_cli(p, "--rate", str(RATE))
        self.assertEqual(code, 1)
        self.assertIn("sample rate 44100 Hz, expected 48000 Hz", out)

    # (f) header data size is 0, real data follows
    def test_f_zero_data_size_is_analysed_with_warning(self):
        p = self.path("f_zero_size.wav")
        write_wav(p, to_int16(tone(RATE, SECONDS, 440.0, 0.5, 2)), RATE)
        with open(p, "r+b") as f:
            f.seek(40)                       # 'data' chunk size field in a 44-byte header
            f.write(struct.pack("<I", 0))
        res = self.check(p, rate=RATE, channels=2)
        self.assertEqual(res["verdict"], "PASS", res["failed"])
        self.assertEqual(res["frames"], RATE * SECONDS)
        self.assertAlmostEqual(res["duration_s"], SECONDS, places=6)
        self.assertAlmostEqual(res["whole"]["peak"], 0.5, delta=0.01)
        self.assertTrue(any("data chunk size is 0" in w for w in res["warnings"]), res["warnings"])

    # (g) 6-channel WAVE_FORMAT_EXTENSIBLE, different tone per channel
    def test_g_six_channel_extensible_per_channel_metrics(self):
        p = self.path("g_six.wav")
        channels = 6
        freqs = [220.0 * (c + 1) for c in range(channels)]
        amps = [0.1 + 0.15 * c for c in range(channels)]
        x = np.zeros((RATE * SECONDS, channels))
        for c in range(channels):
            x[:, c] = tone(RATE, SECONDS, freqs[c], amps[c], 1)[:, 0]
        write_extensible(p, to_int16(x), RATE)
        res = self.check(p, rate=RATE, channels=channels)
        self.assertEqual(res["format"]["tag"], "EXTENSIBLE")
        self.assertEqual(res["channels"], channels)
        self.assertEqual(len(res["per_channel"]), channels)
        self.assertEqual(res["verdict"], "PASS", res["failed"])
        for c, blk in enumerate(res["per_channel"]):
            self.assertAlmostEqual(blk["peak"], amps[c], delta=0.01, msg=f"channel {c} peak")
            expected_db = 20.0 * np.log10(amps[c] / np.sqrt(2.0))
            self.assertAlmostEqual(blk["rms_dbfs"], expected_db, delta=0.2, msg=f"channel {c} rms")
            self.assertEqual(blk["active_fraction"], 1.0, msg=f"channel {c} active")
            self.assertLess(abs(blk["dc_offset"]), 1e-3, msg=f"channel {c} dc")
            self.assertEqual(blk["clicks"]["count"], 0, msg=f"channel {c} clicks")

    # extra: unreadable input exits 2
    def test_unreadable_file_exits_2(self):
        p = self.path("junk.wav")
        with open(p, "wb") as f:
            f.write(b"hello world, not a wav")
        code, _ = run_cli(p)
        self.assertEqual(code, 2)
        code, _ = run_cli(self.path("missing.wav"))
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
