"""Unit tests for tracediff (synthetic traces in a temp dir, stdlib unittest only)."""
import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(HERE)
if TEST_DIR not in sys.path:
    sys.path.insert(0, TEST_DIR)

import tracediff  # noqa: E402

CLI = os.path.join(TEST_DIR, "tracediff.py")
A_AREA = "0123456789abcdef0123456789abcdef"
B_AREA = "fedcba9876543210fedcba9876543210"
WORLD = "11112222333344445555666677778888"


def frame(f, area=A_AREA, hp=100.0, state=1, **extra):
    d = {
        "f": f, "area": area, "world": WORLD,
        "pos": [float(f), 0.0, 0.0], "vel": [0.0, 0.0, 0.0],
        "hp": hp, "igt": f / 60.0, "state": state,
    }
    d.update(extra)
    return d


def trace(n, **kw):
    return [frame(i, **kw) for i in range(n)]


def write_jsonl(path, objs, header=None):
    with open(path, "w", encoding="utf-8") as fh:
        if header is not None:
            fh.write(json.dumps(header) + "\n")
        for o in objs:
            fh.write(json.dumps(o) + "\n")


class TracediffTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def path(self, name):
        return os.path.join(self.dir, name)

    # --- loader ---------------------------------------------------------

    def test_loader_returns_header_and_skips_blank_lines(self):
        p = self.path("h.jsonl")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(json.dumps({"trace": 1, "source": "nxrt", "seed": 5}) + "\n\n")
            fh.write(json.dumps(frame(0)) + "\n\n")
            fh.write(json.dumps(frame(1)) + "\n")
        header, objs = tracediff.load_trace(p)
        self.assertEqual(header["source"], "nxrt")
        self.assertEqual(header["seed"], 5)
        self.assertEqual([o["f"] for o in objs], [0, 1])

    def test_malformed_line_raises_with_line_number(self):
        p = self.path("bad.jsonl")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(json.dumps({"trace": 1, "source": "eden", "seed": 0}) + "\n")  # line 1
            fh.write("\n")                                                          # line 2
            fh.write(json.dumps(frame(0)) + "\n")                                   # line 3
            fh.write("{not json\n")                                                 # line 4
        with self.assertRaises(ValueError) as ctx:
            tracediff.load_trace(p)
        self.assertIn(":4:", str(ctx.exception))

    def test_header_lines_ignored_by_diff(self):
        a_path, b_path = self.path("a.jsonl"), self.path("b.jsonl")
        write_jsonl(a_path, trace(6), header={"trace": 1, "source": "nxrt", "seed": 1})
        write_jsonl(b_path, trace(6))
        _, fa = tracediff.load_trace(a_path)
        _, fb = tracediff.load_trace(b_path)
        res = tracediff.diff_traces(fa, fb)
        self.assertTrue(res.passed)
        self.assertEqual(res.matched_frames, 6)

    # --- diff core ------------------------------------------------------

    def test_identical_traces_pass(self):
        res = tracediff.diff_traces(trace(20), trace(20))
        self.assertTrue(res.passed)
        self.assertEqual(res.matched_frames, 20)
        self.assertEqual(res.mismatch_count, 0)
        self.assertIsNone(res.first_divergence)
        self.assertGreater(res.compared, 0)

    def test_position_drift_beyond_tolerance_fails(self):
        a = trace(20)
        b = copy.deepcopy(a)
        b[12]["pos"][0] += 0.2
        res = tracediff.diff_traces(a, b)
        self.assertFalse(res.passed)
        self.assertEqual(res.mismatch_count, 1)
        self.assertEqual(res.first_divergence["frame"], 12)
        self.assertEqual(res.first_divergence["field"], "pos")
        self.assertAlmostEqual(res.first_divergence["delta"], 0.2, places=6)

    def test_drift_inside_tolerance_passes(self):
        a = trace(20)
        b = copy.deepcopy(a)
        for fr in b:
            fr["pos"][1] += 0.01
        res = tracediff.diff_traces(a, b)
        self.assertTrue(res.passed)
        self.assertAlmostEqual(res.max_delta["pos"], 0.01, places=6)

    def test_tol_argument_widens_tolerance(self):
        a = trace(20)
        b = copy.deepcopy(a)
        b[12]["pos"][0] += 0.2
        self.assertFalse(tracediff.diff_traces(a, b).passed)
        self.assertTrue(tracediff.diff_traces(a, b, tol={"pos": 0.5}).passed)

    def test_offset_shifted_trace(self):
        a = trace(20)
        b = [dict(fr, f=fr["f"] + 7) for fr in a]
        self.assertTrue(tracediff.diff_traces(a, b, offset=7).passed)
        self.assertFalse(tracediff.diff_traces(a, b, offset=0).passed)

    def test_align_first_change_area_finds_offset(self):
        a = [frame(f, area=A_AREA if f < 40 else B_AREA) for f in range(100)]
        # b starts 30 frames later: a's frame f sits at b's frame f+30; b's
        # pre-roll (frames 0..29) is area A and not part of the comparison.
        b = [frame(f, area=A_AREA) for f in range(30)] + [dict(fr, f=fr["f"] + 30) for fr in a]
        res = tracediff.diff_traces(a, b, align="first-change:area")
        self.assertEqual(res.offset, 30)
        self.assertTrue(res.passed)
        self.assertEqual(res.matched_frames, 100)

    def test_align_first_equals_value(self):
        a = [frame(f, area=A_AREA if f < 40 else B_AREA) for f in range(100)]
        b = [frame(f, area=A_AREA) for f in range(30)] + [dict(fr, f=fr["f"] + 30) for fr in a]
        res = tracediff.diff_traces(a, b, align="first:area=" + B_AREA)
        self.assertEqual(res.offset, 30)
        self.assertTrue(res.passed)

    def test_align_with_offset_is_rejected(self):
        with self.assertRaises(ValueError):
            tracediff.diff_traces(trace(5), trace(5), offset=3, align="first-change:area")

    def test_align_unknown_field_is_rejected(self):
        with self.assertRaises(ValueError):
            tracediff.diff_traces(trace(5), trace(5), align="first-change:pos")

    def test_area_mismatch_reports_both_values(self):
        a = trace(10)
        b = copy.deepcopy(a)
        b[5]["area"] = B_AREA
        res = tracediff.diff_traces(a, b)
        self.assertFalse(res.passed)
        self.assertEqual(res.first_divergence["frame"], 5)
        self.assertEqual(res.first_divergence["field"], "area")
        self.assertEqual(res.first_divergence["a"], A_AREA)
        self.assertEqual(res.first_divergence["b"], B_AREA)

    def test_exact_hash_catches_single_digit(self):
        a = [frame(f, h="0123456789abcdef") for f in range(12)]
        b = copy.deepcopy(a)
        b[9]["h"] = "0123456789abcdee"
        res = tracediff.diff_traces(a, b, exact_hash=True)
        self.assertFalse(res.passed)
        self.assertEqual(res.first_divergence["frame"], 9)
        self.assertEqual(res.first_divergence["field"], "h")

    def test_exact_hash_ignores_other_fields(self):
        a = [frame(f, h="0123456789abcdef") for f in range(12)]
        b = copy.deepcopy(a)
        for fr in b:
            fr["pos"][0] += 5.0
        self.assertTrue(tracediff.diff_traces(a, b, exact_hash=True).passed)

    def test_field_missing_on_one_side_is_skipped(self):
        a = trace(10)
        b = copy.deepcopy(a)
        del b[3]["hp"]
        res = tracediff.diff_traces(a, b)
        self.assertTrue(res.passed)
        self.assertEqual(res.skipped, 1)

    def test_fields_option_limits_comparison(self):
        a = trace(10)
        b = copy.deepcopy(a)
        b[2]["area"] = B_AREA
        self.assertTrue(tracediff.diff_traces(a, b, fields=["pos"]).passed)
        self.assertFalse(tracediff.diff_traces(a, b, fields=["area"]).passed)

    def test_checkpoints_compared_by_label_not_frame(self):
        a = trace(10) + [
            {"f": 2, "label": "enter", "hp": 100.0},
            {"f": 6, "label": "exit", "hp": 50.0},
            {"f": 8, "label": "warp", "hp": 1.0},
        ]
        b = trace(10) + [
            {"f": 99, "label": "enter", "hp": 100.0},   # different f, same label: still compared
            {"f": 6, "label": "exit", "hp": 80.0},      # hp drift beyond 0.5
        ]
        res = tracediff.diff_traces(a, b)
        self.assertFalse(res.passed)
        self.assertEqual(res.matched_frames, 10)
        self.assertEqual(res.checkpoints_matched, 2)
        self.assertEqual(res.checkpoint_only_in_a, 1)
        self.assertEqual(res.first_divergence["label"], "exit")
        self.assertEqual(res.first_divergence["field"], "hp")
        self.assertEqual(res.mismatch_count, 1)

    def test_format_report_names_first_divergence(self):
        a = trace(20)
        b = copy.deepcopy(a)
        b[12]["pos"][0] += 0.2
        text = tracediff.format_report(tracediff.diff_traces(a, b), max_rows=5)
        self.assertIn("FAIL", text)
        self.assertIn("first divergence: frame 12 field pos", text)

    def test_empty_fields_rejected(self):
        with self.assertRaises(ValueError):
            tracediff.diff_traces(trace(3), trace(3), fields=[])

    # --- CLI ------------------------------------------------------------

    def run_cli(self, *args):
        return subprocess.run([sys.executable, CLI, *args], capture_output=True, text=True)

    def test_cli_exit_0_identical(self):
        a, b = self.path("a.jsonl"), self.path("b.jsonl")
        write_jsonl(a, trace(10))
        write_jsonl(b, trace(10))
        r = self.run_cli(a, b)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("PASS", r.stdout)

    def test_cli_exit_1_on_mismatch_and_tol_override(self):
        a, b = self.path("a.jsonl"), self.path("b.jsonl")
        base = trace(10)
        drift = copy.deepcopy(base)
        drift[4]["pos"][0] += 0.2
        write_jsonl(a, base)
        write_jsonl(b, drift)
        r = self.run_cli(a, b)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("first divergence: frame 4 field pos", r.stdout)
        r2 = self.run_cli(a, b, "--tol", "pos=0.5")
        self.assertEqual(r2.returncode, 0, r2.stdout)

    def test_cli_json_output(self):
        a, b = self.path("a.jsonl"), self.path("b.jsonl")
        write_jsonl(a, trace(5))
        write_jsonl(b, trace(5))
        r = self.run_cli(a, b, "--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        data = json.loads(r.stdout)
        self.assertTrue(data["passed"])
        self.assertEqual(data["matched_frames"], 5)

    def test_cli_exit_2_missing_file(self):
        a = self.path("a.jsonl")
        write_jsonl(a, trace(3))
        r = self.run_cli(a, self.path("missing.jsonl"))
        self.assertEqual(r.returncode, 2)

    def test_cli_exit_2_malformed_line(self):
        a, b = self.path("a.jsonl"), self.path("b.jsonl")
        write_jsonl(a, trace(3))
        with open(b, "w", encoding="utf-8") as fh:
            fh.write("{oops\n")
        r = self.run_cli(a, b)
        self.assertEqual(r.returncode, 2)
        self.assertIn(":1:", r.stderr)

    def test_cli_exit_2_bad_tol(self):
        a = self.path("a.jsonl")
        write_jsonl(a, trace(3))
        r = self.run_cli(a, a, "--tol", "pos=abc")
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
