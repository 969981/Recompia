"""Unit tests for regress and find_glitches (synthetic run dirs and PNG frames; stdlib unittest)."""
import json
import os
import shutil
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

import find_glitches as fg  # noqa: E402
import regress  # noqa: E402

REGRESS_CLI = os.path.join(TEST_DIR, "regress.py")
W, H = 96, 54


def gradient(shift=0.0):
    """Smooth mid-range gradient: no near-black pixels, no flat channel."""
    y, x = np.mgrid[0:H, 0:W]
    r = 40 + x * 160 / (W - 1)
    g = 40 + y * 160 / (H - 1)
    b = 60 + (x + y) * 100 / (W + H - 2)
    img = np.stack([r, g, b], axis=2) + shift
    return np.clip(np.round(img), 0, 255).astype(np.uint8)


def write_trace(path, ms, mem=None, labels=None, header=True):
    mem = mem or {}
    labels = labels or {}
    lines = [json.dumps({"trace": 1, "build": "test"})] if header else []
    for i, m in enumerate(ms):
        obj = {"f": i, "ms": m}
        if i in mem:
            obj["mem"] = mem[i]
        lines.append(json.dumps(obj))
        if i in labels:
            lines.append(json.dumps({"label": labels[i], "frame": i}))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def write_run(root, name, ms=None, result=None, mem=None, labels=None, crash=False, freeze=False, pngs=None):
    d = os.path.join(root, name)
    os.makedirs(d, exist_ok=True)
    if ms is not None:
        write_trace(os.path.join(d, "trace.jsonl"), ms, mem, labels)
    if result is not None:
        with open(os.path.join(d, "result.json"), "w", encoding="utf-8") as fh:
            json.dump(result, fh)
    if crash:
        with open(os.path.join(d, "crash.txt"), "w", encoding="utf-8") as fh:
            fh.write("boom\n")
    if freeze:
        with open(os.path.join(d, "freeze.txt"), "w", encoding="utf-8") as fh:
            fh.write("stuck\n")
    if pngs is not None:
        fdir = os.path.join(d, "frames")
        os.makedirs(fdir, exist_ok=True)
        for i, arr in enumerate(pngs):
            Image.fromarray(arr).save(os.path.join(fdir, f"f{i:04d}.png"))
    return d


def result_json(**kw):
    base = {"schema": 1, "verdict": "pass", "exit_code": 0, "message": "", "frames": 600,
            "wall_s": 10.0, "checkpoints": [{"label": "boot", "frame": 100, "hash": "aa"}],
            "final_hash": "aa"}
    base.update(kw)
    return base


def base_summary(**kw):
    d = {
        "frames": 600, "fps_windows": 10, "wall_s": 10.0, "verdict": "pass", "exit_code": 0,
        "crashes": 0, "freezes": 0, "fps_median": 62.5, "fps_p10": 60.0, "frame_ms_p99": 16.0,
        "peak_mem_mb": 1000.0, "vk_errors": 0, "host_exceptions": 0, "glitch_frames": 0,
        "final_hash": "aaa", "deterministic": False, "checkpoint_labels": ["boot", "menu"],
    }
    d.update(kw)
    return d


def status_of(verdict, metric):
    rows = [r for r in verdict.rows if r["metric"] == metric]
    return rows[0]["status"] if rows else None


class SummarizeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = cls._tmp.name

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_constant_16ms_trace_exact(self):
        d = write_run(self.tmp, "const", ms=[16.0] * 600,
                      labels={100: "boot"}, result=result_json(vk_errors=0), mem={0: 200.0})
        s = regress.summarize(d)
        self.assertEqual(s["fps_median"], 62.5)
        self.assertEqual(s["fps_p10"], 62.5)
        self.assertEqual(s["frame_ms_p99"], 16.0)
        self.assertEqual(s["fps_windows"], 10)
        self.assertEqual(s["frames"], 600)
        self.assertEqual(s["checkpoint_labels"], ["boot"])
        self.assertEqual(s["crashes"], 0)
        self.assertEqual(s["freezes"], 0)
        self.assertEqual(s["host_exceptions"], 0)
        self.assertEqual(s["vk_errors"], 0)
        self.assertEqual(s["glitch_frames"], 0)
        self.assertEqual(s["final_hash"], "aa")
        self.assertEqual(s["peak_mem_mb"], 200.0)

    def test_label_lines_not_counted_as_frames_without_result(self):
        d = write_run(self.tmp, "trace_only", ms=[16.0] * 120, labels={5: "boot"})
        s = regress.summarize(d)
        self.assertEqual(s["frames"], 120)
        self.assertIsNone(s["verdict"])
        self.assertEqual(s["fps_median"], 62.5)
        self.assertIsNone(s["checkpoint_labels"])
        self.assertIsNone(s["crashes"])
        self.assertIsNone(s["vk_errors"])

    def test_skip_windows_rule(self):
        # 23 windows is not more than skip+3, so nothing is dropped.
        s23 = regress.summarize(write_run(self.tmp, "w23", ms=[16.0] * (60 * 23)))
        self.assertEqual(s23["fps_windows"], 23)
        # 24 windows is more than skip+3, so the first 20 go.
        s24 = regress.summarize(write_run(self.tmp, "w24", ms=[16.0] * (60 * 24)))
        self.assertEqual(s24["fps_windows"], 4)
        # skip_windows=0 keeps everything.
        s0 = regress.summarize(write_run(self.tmp, "w24b", ms=[16.0] * (60 * 24)), skip_windows=0)
        self.assertEqual(s0["fps_windows"], 24)

    def test_slow_boot_windows_dropped(self):
        ms = [33.0] * (60 * 20) + [16.0] * (60 * 10)  # 30 windows: slow boot, then 62.5 fps
        d = write_run(self.tmp, "boot_slow", ms=ms)
        self.assertEqual(regress.summarize(d)["fps_median"], 62.5)
        self.assertEqual(regress.summarize(d)["fps_p10"], 62.5)
        self.assertAlmostEqual(regress.summarize(d, skip_windows=0)["fps_median"], 1000.0 / 33.0, places=6)

    def test_missing_files_tolerated_and_missing_dir_raises(self):
        empty = os.path.join(self.tmp, "empty")
        os.makedirs(empty)
        s = regress.summarize(empty)
        for key in ("fps_median", "fps_p10", "frame_ms_p99", "peak_mem_mb", "verdict",
                    "crashes", "freezes", "vk_errors", "final_hash", "checkpoint_labels", "frames"):
            self.assertIsNone(s[key], key)
        self.assertEqual(s["glitch_frames"], 0)
        self.assertEqual(s["host_exceptions"], None)
        with self.assertRaises(FileNotFoundError):
            regress.summarize(os.path.join(self.tmp, "does_not_exist"))

    def test_crash_flags(self):
        s1 = regress.summarize(write_run(self.tmp, "crash_verdict", result=result_json(verdict="crash")))
        self.assertEqual(s1["crashes"], 1)
        s2 = regress.summarize(write_run(self.tmp, "crash_file", result=result_json(), crash=True))
        self.assertEqual(s2["crashes"], 1)
        s3 = regress.summarize(write_run(self.tmp, "freeze_file", result=result_json(), freeze=True))
        self.assertEqual(s3["freezes"], 1)
        self.assertEqual(s3["crashes"], 0)

    def test_peak_mem_and_host_exceptions_default(self):
        d = write_run(self.tmp, "mem", ms=[16.0] * 600, mem={0: 100.0, 300: 512.5, 599: 300.0},
                      result=result_json())
        s = regress.summarize(d)
        self.assertEqual(s["peak_mem_mb"], 512.5)
        self.assertEqual(s["host_exceptions"], 0)

    def test_glitch_frames_from_pngs(self):
        normal = [gradient(i * 3.0) for i in range(3)]
        black = gradient(0.0)
        black[: H // 2] = 0
        d = write_run(self.tmp, "glitchy", ms=[16.0] * 600, result=result_json(),
                      pngs=normal + [black])
        self.assertEqual(regress.summarize(d)["glitch_frames"], 1)

    def test_torn_last_line_ignored(self):
        d = write_run(self.tmp, "torn", ms=[16.0] * 120)
        with open(os.path.join(d, "trace.jsonl"), "a", encoding="utf-8") as fh:
            fh.write('{"f": 120, "ms": 1')
        self.assertEqual(regress.summarize(d)["frames"], 120)


class CompareTests(unittest.TestCase):
    def test_identical_no_loss(self):
        b = base_summary()
        v = regress.compare(b, dict(b))
        self.assertEqual(v.overall, "no loss")
        self.assertEqual(v.loss_count, 0)

    def test_fps_median_down_5_is_loss(self):
        v = regress.compare(base_summary(), base_summary(fps_median=62.5 * 0.95))
        self.assertEqual(v.overall, "loss")
        self.assertEqual(status_of(v, "fps_median"), "LOSS")

    def test_fps_median_down_2_is_ok(self):
        v = regress.compare(base_summary(), base_summary(fps_median=62.5 * 0.98))
        self.assertEqual(v.loss_count, 0)
        self.assertEqual(status_of(v, "fps_median"), "OK")

    def test_fps_p10_down_12_is_loss(self):
        v = regress.compare(base_summary(), base_summary(fps_p10=60.0 * 0.88))
        self.assertEqual(status_of(v, "fps_p10"), "LOSS")

    def test_peak_mem_up_15_is_loss(self):
        v = regress.compare(base_summary(), base_summary(peak_mem_mb=1000.0 * 1.15))
        self.assertEqual(status_of(v, "peak_mem_mb"), "LOSS")

    def test_crash_added_is_loss(self):
        v = regress.compare(base_summary(), base_summary(crashes=1, verdict="crash"))
        self.assertEqual(status_of(v, "crashes"), "LOSS")
        self.assertEqual(status_of(v, "verdict"), "LOSS")

    def test_missing_checkpoint_is_loss(self):
        v = regress.compare(base_summary(), base_summary(checkpoint_labels=["boot"]))
        self.assertEqual(status_of(v, "checkpoints"), "LOSS")
        row = [r for r in v.rows if r["metric"] == "checkpoints"][0]
        self.assertEqual(row["missing"], ["menu"])

    def test_glitch_frames_up_is_loss(self):
        v = regress.compare(base_summary(), base_summary(glitch_frames=3))
        self.assertEqual(status_of(v, "glitch_frames"), "LOSS")

    def test_vk_errors_and_host_exceptions_up_is_loss(self):
        v = regress.compare(base_summary(), base_summary(vk_errors=2, host_exceptions=1))
        self.assertEqual(status_of(v, "vk_errors"), "LOSS")
        self.assertEqual(status_of(v, "host_exceptions"), "LOSS")

    def test_hash_difference_on_deterministic_runs_is_note(self):
        b = base_summary(deterministic=True)
        c = base_summary(deterministic=True, final_hash="bbb")
        v = regress.compare(b, c)
        self.assertEqual(v.loss_count, 0)
        self.assertEqual(status_of(v, "final_hash"), "NOTE")

    def test_hash_difference_not_deterministic_is_not_note(self):
        v = regress.compare(base_summary(), base_summary(final_hash="bbb"))
        self.assertEqual(v.loss_count, 0)
        self.assertEqual(status_of(v, "final_hash"), "OK")

    def test_null_metrics_are_skipped(self):
        v = regress.compare(base_summary(), base_summary(fps_median=None, peak_mem_mb=None))
        self.assertIsNone(status_of(v, "fps_median"))
        self.assertIsNone(status_of(v, "peak_mem_mb"))
        self.assertEqual(v.loss_count, 0)

    def test_pass_to_fail_is_loss_and_fail_to_pass_is_not(self):
        self.assertEqual(status_of(regress.compare(base_summary(), base_summary(verdict="fail")), "verdict"), "LOSS")
        self.assertEqual(status_of(regress.compare(base_summary(verdict="fail"), base_summary()), "verdict"), "OK")

    def test_threshold_override(self):
        v = regress.compare(base_summary(), base_summary(fps_median=62.5 * 0.98),
                            {"fps_median_drop": 0.01})
        self.assertEqual(status_of(v, "fps_median"), "LOSS")


class FindGlitchesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = cls._tmp.name

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def make_set(self, name, arrays):
        d = os.path.join(self.tmp, name)
        os.makedirs(d, exist_ok=True)
        for i, arr in enumerate(arrays):
            Image.fromarray(arr).save(os.path.join(d, f"frame_{i:03d}.png"))
        return d

    def test_flags_mostly_black_frame_among_gradients(self):
        black = gradient(0.0)
        black[: H // 2] = 0
        d = self.make_set("black_mid", [gradient(0.0), gradient(3.0), black, gradient(6.0), gradient(9.0)])
        flagged = fg.find_glitches(d)
        self.assertEqual([f["index"] for f in flagged], [2])
        self.assertEqual(flagged[0]["reason"], "near_black")

    def test_normal_gradients_not_flagged(self):
        d = self.make_set("normal", [gradient(i * 3.0) for i in range(5)])
        self.assertEqual(fg.find_glitches(d), [])

    def test_black_frac_threshold(self):
        half = gradient(0.0)
        half[: H // 2] = 0
        d = self.make_set("half_black", [gradient(0.0), gradient(3.0), half, gradient(6.0)])
        self.assertEqual(fg.find_glitches(d, black_frac=0.6), [])
        self.assertEqual(len(fg.find_glitches(d, black_frac=0.02)), 1)

    def test_flat_frame_flagged(self):
        flat = np.full((H, W, 3), (128, 128, 128), dtype=np.uint8)
        d = self.make_set("flat_one", [gradient(0.0), flat, gradient(6.0), gradient(9.0)])
        flagged = fg.find_glitches(d)
        self.assertEqual([f["index"] for f in flagged], [1])
        self.assertEqual(flagged[0]["reason"], "flat")

    def test_all_flat_set_not_flagged(self):
        flat = np.full((H, W, 3), (200, 30, 30), dtype=np.uint8)
        d = self.make_set("all_flat", [flat, flat.copy(), flat.copy()])
        self.assertEqual(fg.find_glitches(d), [])

    def test_empty_dir(self):
        d = os.path.join(self.tmp, "empty_frames")
        os.makedirs(d)
        self.assertEqual(fg.find_glitches(d), [])


class CliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = cls._tmp.name
        cls.base = write_run(cls.tmp, "base", ms=[16.0] * 600, result=result_json())
        cls.same = write_run(cls.tmp, "same", ms=[16.0] * 600, result=result_json())
        cls.crash = write_run(cls.tmp, "crash", ms=[16.0] * 600,
                              result=result_json(verdict="crash", exit_code=1), crash=True)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_cli(self, *args):
        return subprocess.run([sys.executable, REGRESS_CLI, *args], capture_output=True, text=True)

    def test_exit_codes(self):
        self.assertEqual(self.run_cli("compare", self.base, self.same).returncode, 0)
        r = self.run_cli("compare", self.base, self.crash)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("VERDICT: loss", r.stdout)
        self.assertEqual(self.run_cli("compare", self.base, os.path.join(self.tmp, "nope")).returncode, 2)
        self.assertEqual(self.run_cli("summarize", os.path.join(self.tmp, "nope")).returncode, 2)

    def test_compare_accepts_summary_json(self):
        bsum = os.path.join(self.tmp, "base.json")
        csum = os.path.join(self.tmp, "cand.json")
        self.assertEqual(self.run_cli("summarize", self.base, "-o", bsum).returncode, 0)
        self.assertEqual(self.run_cli("summarize", self.same, "-o", csum).returncode, 0)
        with open(bsum, encoding="utf-8") as fh:
            summary = json.load(fh)
        self.assertEqual(summary["fps_median"], 62.5)
        r = self.run_cli("compare", bsum, csum)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("VERDICT: no loss", r.stdout)

        slow = dict(summary, fps_median=62.5 * 0.95)
        with open(csum, "w", encoding="utf-8") as fh:
            json.dump(slow, fh)
        r = self.run_cli("compare", bsum, csum, "--json")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertEqual(json.loads(r.stdout)["loss_count"], 1)


if __name__ == "__main__":
    unittest.main()
