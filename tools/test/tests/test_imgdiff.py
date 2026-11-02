"""Unit tests for imgdiff (synthetic images, stdlib unittest + numpy + Pillow)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(HERE)
if TEST_DIR not in sys.path:
    sys.path.insert(0, TEST_DIR)

import imgdiff  # noqa: E402

CLI = os.path.join(TEST_DIR, "imgdiff.py")
W, H = 320, 180


def base_image():
    """Smooth gradients plus a few hard-edged rectangles, mid-range values."""
    y, x = np.mgrid[0:H, 0:W]
    r = (40 + x * 160 / (W - 1)).astype(np.uint8)
    g = (40 + y * 160 / (H - 1)).astype(np.uint8)
    b = (60 + (x + y) * 100 / (W + H - 2)).astype(np.uint8)  # diagonal gradient, no single-bin spike
    img = np.stack([r, g, b], axis=2)
    img[20:60, 30:110] = (220, 30, 30)
    img[100:150, 200:290] = (30, 30, 220)
    img[40:70, 240:300] = (250, 250, 250)
    return img


def add_noise(img, std=3.0, seed=0):
    rng = np.random.default_rng(seed)
    n = rng.normal(0.0, std, img.shape)
    return np.clip(np.round(img.astype(np.float64) + n), 0, 255).astype(np.uint8)


def block_change(img, frac, colour):
    x0, y0, x1, y1 = frac
    out = img.copy()
    out[int(round(y0 * H)):int(round(y1 * H)), int(round(x0 * W)):int(round(x1 * W))] = colour
    return out


class ImgDiffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = cls._tmp.name
        cls.a = base_image()

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def save(self, name, arr):
        path = os.path.join(self.tmp, name)
        Image.fromarray(arr).save(path)
        return path

    def test_identical(self):
        r = imgdiff.compare(self.a, self.a.copy())
        self.assertAlmostEqual(r.ssim, 1.0, places=6)
        self.assertEqual(r.mae, 0.0)
        self.assertAlmostEqual(r.hist_corr, 1.0, places=6)
        self.assertEqual(r.changed_frac, 0.0)
        self.assertTrue(r.passed())

    def test_double_resolution_scores_high(self):
        big = np.asarray(Image.fromarray(self.a).resize((2 * W, 2 * H), Image.Resampling.BICUBIC))
        r = imgdiff.compare(self.a, big)
        self.assertGreaterEqual(r.ssim, 0.97)
        self.assertNotIn("aspect mismatch", r.notes)
        self.assertTrue(r.passed())

    def test_small_noise_passes_default_thresholds(self):
        r = imgdiff.compare(self.a, add_noise(self.a, std=3.0))
        self.assertTrue(r.passed(), f"ssim={r.ssim} mae={r.mae} hist={r.hist_corr} changed={r.changed_frac}")

    def test_large_block_change_fails(self):
        b = block_change(self.a, (0.0, 0.0, 1.0, 0.3), (255, 0, 255))
        r = imgdiff.compare(self.a, b)
        self.assertGreater(r.changed_frac, 0.15)
        self.assertFalse(r.passed())

    def test_block_change_inside_rect_mask_passes(self):
        b = block_change(self.a, (0.0, 0.0, 1.0, 0.3), (255, 0, 255))
        r = imgdiff.compare(self.a, b, masks=[(0.0, 0.0, 1.0, 0.3)])
        self.assertTrue(r.passed())
        self.assertAlmostEqual(r.mae, 0.0, places=6)

    def test_mask_array_matches_rect_mask(self):
        b = block_change(self.a, (0.0, 0.0, 1.0, 0.3), (255, 0, 255))
        rect_res = imgdiff.compare(self.a, b, masks=[(0.0, 0.0, 1.0, 0.3)])

        arr = np.zeros((H, W), dtype=np.uint8)
        arr[0:54, :] = 255
        arr_res = imgdiff.compare(self.a, b, masks=[arr])

        dict_res = imgdiff.compare(self.a, b, masks=[{"rect": [0.0, 0.0, 1.0, 0.3]}])

        path = self.save("mask_rect.png", arr)
        file_res = imgdiff.compare(self.a, b, masks=[path])

        for other in (arr_res, dict_res, file_res):
            self.assertAlmostEqual(other.ssim, rect_res.ssim, places=9)
            self.assertAlmostEqual(other.mae, rect_res.mae, places=9)
            self.assertAlmostEqual(other.hist_corr, rect_res.hist_corr, places=9)
            self.assertAlmostEqual(other.changed_frac, rect_res.changed_frac, places=9)

    def test_hud_preset_accepted(self):
        b = self.a.copy()
        b[0:25, :] = 255  # only the top 14% strip changes
        self.assertTrue(imgdiff.compare(self.a, b, masks=["hud"]).passed())
        self.assertFalse(imgdiff.compare(self.a, b).passed())

    def test_fully_masked_gives_ssim_one(self):
        other = np.random.default_rng(1).integers(0, 256, self.a.shape, dtype=np.uint8)
        r = imgdiff.compare(self.a, other, masks=[(0.0, 0.0, 1.0, 1.0)])
        self.assertEqual(r.ssim, 1.0)
        self.assertTrue(r.passed())
        self.assertIn("fully masked", r.notes)

    def test_brightness_shift_fails_mae(self):
        b = np.clip(self.a.astype(np.int16) + 60, 0, 255).astype(np.uint8)
        r = imgdiff.compare(self.a, b)
        self.assertGreater(r.mae, imgdiff.DEFAULT_THRESHOLDS["max_mae"])
        self.assertFalse(r.passed())

    def test_horizontal_shift_high_contrast_low_ssim(self):
        cols = (np.arange(W) // 8) % 2                     # 8 px stripes, period 16
        stripes = np.repeat((cols * 255).astype(np.uint8)[None, :], H, axis=0)
        a = np.stack([stripes] * 3, axis=2)
        b = np.roll(a, 20, axis=1)
        r = imgdiff.compare(a, b)
        self.assertLess(r.ssim, 0.9)

    def test_rgba_input(self):
        rgba = np.dstack([self.a, np.full((H, W), 255, dtype=np.uint8)])
        r = imgdiff.compare(rgba, self.a)
        self.assertAlmostEqual(r.mae, 0.0, places=9)

        # Transparent top half composites to black, so it must match a black top half.
        rng = np.random.default_rng(2)
        rgba2 = rgba.copy()
        rgba2[:90, :, :3] = rng.integers(0, 256, (90, W, 3), dtype=np.uint8)
        rgba2[:90, :, 3] = 0
        expected = self.a.copy()
        expected[:90] = 0
        r2 = imgdiff.compare(rgba2, expected)
        self.assertAlmostEqual(r2.mae, 0.0, places=9)
        self.assertTrue(r2.passed())

    def test_diff_image_shape_and_mask_tint(self):
        b = block_change(self.a, (0.0, 0.0, 1.0, 0.3), (255, 0, 255))
        d = imgdiff.diff_image(self.a, b, size=(200, 100))
        self.assertEqual(d.shape, (100, 200, 3))
        self.assertEqual(d.dtype, np.uint8)
        d_masked = imgdiff.diff_image(self.a, b, masks=[(0.0, 0.0, 0.5, 0.5)])
        self.assertEqual(d_masked.shape, (H, W, 3))
        self.assertEqual(tuple(d_masked[0, 0]), (0, 0, 90))

    def test_aspect_mismatch_note(self):
        b = np.asarray(Image.fromarray(self.a).resize((W, 240), Image.Resampling.BICUBIC))
        r = imgdiff.compare(self.a, b)
        self.assertIn("aspect mismatch", r.notes)

    def test_load_masks_json(self):
        path = os.path.join(self.tmp, "masks.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"top": [[0.0, 0.0, 1.0, 0.3]], "unused": [[0.0, 0.5, 0.5, 1.0]]}, f)
        masks = imgdiff.load_masks_json(path)
        self.assertEqual(len(masks), 2)
        only_top = imgdiff.load_masks_json(path, names=["top"])
        self.assertEqual(only_top, [(0.0, 0.0, 1.0, 0.3)])
        b = block_change(self.a, (0.0, 0.0, 1.0, 0.3), (255, 0, 255))
        self.assertTrue(imgdiff.compare(self.a, b, masks=only_top).passed())

    def test_bad_mask_spec_raises(self):
        with self.assertRaises(ValueError):
            imgdiff.compare(self.a, self.a, masks=["no_such_preset_or_file"])


class CliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = cls._tmp.name
        cls.a = base_image()
        cls.ref = os.path.join(cls.tmp, "ref.png")
        Image.fromarray(cls.a).save(cls.ref)
        cls.same = os.path.join(cls.tmp, "same.png")
        Image.fromarray(cls.a).save(cls.same)
        cls.bright = os.path.join(cls.tmp, "bright.png")
        Image.fromarray(np.clip(cls.a.astype(np.int16) + 60, 0, 255).astype(np.uint8)).save(cls.bright)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_cli(self, *args):
        return subprocess.run([sys.executable, CLI, *args], capture_output=True, text=True, timeout=120)

    def test_exit_0_on_identical(self):
        p = self.run_cli(self.ref, self.same)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertTrue(p.stdout.startswith("PASS "), p.stdout)

    def test_exit_1_on_failure(self):
        p = self.run_cli(self.ref, self.bright)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertTrue(p.stdout.startswith("FAIL "), p.stdout)

    def test_exit_2_on_missing_file(self):
        p = self.run_cli(self.ref, os.path.join(self.tmp, "missing.png"))
        self.assertEqual(p.returncode, 2)

    def test_exit_2_on_bad_mask_spec(self):
        p = self.run_cli(self.ref, self.same, "--mask", "not_a_preset_or_file")
        self.assertEqual(p.returncode, 2)

    def test_exit_2_on_bad_size(self):
        p = self.run_cli(self.ref, self.same, "--size", "huge")
        self.assertEqual(p.returncode, 2)

    def test_json_and_diff_out(self):
        diff_path = os.path.join(self.tmp, "diff.png")
        p = self.run_cli(self.ref, self.bright, "--json", "--diff-out", diff_path)
        self.assertEqual(p.returncode, 1, p.stderr)
        payload = json.loads(p.stdout)
        for key in ("ssim", "mae", "hist_corr", "changed_frac", "notes", "passed", "thresholds"):
            self.assertIn(key, payload)
        self.assertFalse(payload["passed"])
        with Image.open(diff_path) as im:
            self.assertEqual(im.size, (W, H))


if __name__ == "__main__":
    unittest.main()
