"""Unit tests for compare_pcm.py. Synthetic data only; nothing is read from the project."""
import os
import tempfile
import unittest

import numpy as np

import compare_pcm as cp

RATE = 48000
CH = 6
N = 2 * RATE  # 2 seconds per channel


def write_pcm(path, arr):
    np.clip(np.round(arr), -32768, 32767).astype("<i2").tofile(path)


class CompareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rng = np.random.default_rng(1234)
        cls.A = rng.normal(0.0, 3000.0, (N, CH))  # independent noise per channel
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = cls.tmp.name
        cls.pa = os.path.join(cls.dir, "a.pcm")
        write_pcm(cls.pa, cls.A)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _path(self, name):
        return os.path.join(self.dir, name)

    def _run(self, b_arr, name, min_corr=0.99):
        pb = self._path(name)
        write_pcm(pb, b_arr)
        return cp.compare(self.pa, pb, CH, RATE, 4096, min_corr)

    def test_identical_pass(self):
        res = self._run(self.A, "same.pcm")
        self.assertTrue(res["passed"], res["failures"])
        for c, _, _, lag, corr, rel, status in res["rows"]:
            self.assertEqual(lag, 0)
            self.assertGreater(corr, 0.9999)
            self.assertEqual(status, "PASS")

    def test_shift_100_pass_with_lag_100(self):
        # B is A delayed by 100 samples: B[i+100] == A[i]
        B = np.vstack([np.zeros((100, CH)), self.A[:-100]])
        res = self._run(B, "shift100.pcm")
        self.assertTrue(res["passed"], res["failures"])
        for c, _, _, lag, corr, _, _ in res["rows"]:
            self.assertEqual(abs(lag), 100, "channel %d lag %s" % (c, lag))

    def test_swapped_channel_fail(self):
        B = self.A.copy()
        B[:, 0] = self.A[:, 1]
        res = self._run(B, "swap.pcm")
        self.assertFalse(res["passed"])
        self.assertTrue(any(f.startswith("ch 0:") and "corr" in f for f in res["failures"]),
                        res["failures"])

    def test_noise_minus_20db_pass(self):
        noise = np.zeros_like(self.A)
        rng = np.random.default_rng(99)
        for c in range(CH):
            sig_rms = np.sqrt(np.mean(self.A[:, c] ** 2))
            noise[:, c] = rng.normal(0.0, 1.0, N) * (0.1 * sig_rms)  # -20 dB noise power
        res = self._run(self.A + noise, "noise20.pcm", min_corr=0.98)
        self.assertTrue(res["passed"], res["failures"])
        for row in res["rows"]:
            self.assertGreater(row[4], 0.98)

    def test_length_diff_10000_fail(self):
        B = np.vstack([self.A, np.zeros((10000, CH))])
        res = self._run(B, "longer.pcm")
        self.assertFalse(res["passed"])
        self.assertTrue(any("length difference" in f for f in res["failures"]), res["failures"])

    def test_silent_channel_reported_not_failing(self):
        A = self.A.copy()
        A[:, 5] = 0.0
        pa = self._path("silent_a.pcm")
        write_pcm(pa, A)
        pb = self._path("silent_b.pcm")
        write_pcm(pb, A)
        res = cp.compare(pa, pb, CH, RATE, 4096, 0.99)
        self.assertTrue(res["passed"], res["failures"])
        self.assertEqual(res["rows"][5][6], "silent")


if __name__ == "__main__":
    unittest.main()
