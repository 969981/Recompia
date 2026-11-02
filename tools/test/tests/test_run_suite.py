"""Unit tests for run_suite.py. Stdlib unittest; runs the fake game (fake_game.py) as the exe.

Run from the tools test folder:
    ..\\.venv\\Scripts\\python.exe -I -m unittest discover -s tests -p "test_run_suite.py" -v
"""
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEST_DIR = HERE.parent
if str(TEST_DIR) not in sys.path:
    sys.path.insert(0, str(TEST_DIR))
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import run_suite  # noqa: E402
import fake_game  # noqa: E402
from PIL import Image  # noqa: E402

FAKE = str(HERE / "fake_game.py")
RUNNER = str(TEST_DIR / "run_suite.py")


def tval(v):
    """Render a Python value as a TOML value (JSON string escapes are valid TOML basic strings)."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return json.dumps(v)
    if isinstance(v, list):
        return "[" + ", ".join(tval(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{ " + ", ".join(f"{k} = {tval(x)}" for k, x in v.items()) + " }"
    raise TypeError(v)


class SuiteCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.out_root = self.dir / "out"

    def tearDown(self):
        self._tmp.cleanup()

    def script(self, mode):
        """Create <mode>.nxs whose first line steers the fake game; return its relative name."""
        p = self.dir / f"{mode}.nxs"
        p.write_text(f"# fake: {mode}\n-- body\n", encoding="utf-8")
        return p.name

    def suite(self, runs, defaults=None):
        d = {"exe": [sys.executable, FAKE], "out_root": "out", "timeout": 20}
        d.update(defaults or {})
        lines = ["[defaults]"] + [f"{k} = {tval(v)}" for k, v in d.items()]
        for run in runs:
            lines += ["", "[[run]]"] + [f"{k} = {tval(v)}" for k, v in run.items()]
        path = self.dir / "suite.toml"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = run_suite.main([str(a) for a in argv])
        return rc, out.getvalue(), err.getvalue()

    def results(self):
        with open(self.out_root / "suite_result.json", encoding="utf-8") as fh:
            return {r["name"]: r for r in json.load(fh)}


class Classification(SuiteCase):
    def test_every_classification(self):
        runs = [
            {"name": "ok", "script": self.script("pass")},
            {"name": "fail", "script": self.script("fail")},
            {"name": "fail_expected", "script": self.script("fail"), "expect_exit": 1},
            {"name": "pass_but_expected_fail", "script": self.script("pass"), "expect_exit": 1},
            {"name": "crash", "script": self.script("crash")},
            {"name": "crash_named", "script": self.script("crashfile")},
            {"name": "freeze", "script": self.script("freeze")},
            {"name": "hang", "script": self.script("hang"), "timeout": 2},
            {"name": "deadline", "script": self.script("deadline")},
            {"name": "bad_script", "script": self.script("badscript")},
            {"name": "internal", "script": self.script("internal")},
            {"name": "odd_exit", "script": self.script("odd")},
        ]
        rc, out, _ = self.run_cli(self.suite(runs))
        self.assertEqual(rc, 1)
        res = self.results()
        expect = {
            "ok": "PASS",
            "fail": "FAIL",
            "fail_expected": "PASS",
            "pass_but_expected_fail": "FAIL",
            "crash": "CRASH",
            "crash_named": "CRASH",
            "freeze": "FREEZE",
            "hang": "TIMEOUT",
            "deadline": "TIMEOUT",
            "bad_script": "ERROR",
            "internal": "ERROR",
            "odd_exit": "ERROR",
        }
        for name, status in expect.items():
            with self.subTest(run=name):
                self.assertEqual(res[name]["status"], status, res[name])
        self.assertIn("fake assert failed", res["fail"]["reason"])
        self.assertIn("expected failure but passed", res["pass_but_expected_fail"]["reason"])
        self.assertIn("boot_crash.txt", res["crash_named"]["reason"])
        self.assertIn("killed after 2s", res["hang"]["reason"])

    def test_pass_and_expected_failure_give_exit_zero(self):
        rc, _, _ = self.run_cli(self.suite([
            {"name": "ok", "script": self.script("pass")},
            {"name": "neg", "script": self.script("fail"), "expect_exit": 1},
        ]))
        self.assertEqual(rc, 0)

    def test_hang_is_killed(self):
        rc, _, _ = self.run_cli(self.suite([
            {"name": "hang", "script": self.script("hang"), "timeout": 2},
        ]))
        self.assertEqual(rc, 1)
        res = self.results()["hang"]
        self.assertEqual(res["status"], "TIMEOUT")
        self.assertLess(res["wall_s"], 15)
        pid = int((self.out_root / "hang" / "hang_pid.txt").read_text(encoding="utf-8"))
        if os.name == "nt":
            ps = subprocess.run(["tasklist", "/FI", f"PID eq {pid}"],
                                capture_output=True, text=True).stdout
            self.assertNotIn(str(pid), ps)
        else:
            with self.assertRaises(OSError):
                os.kill(pid, 0)

    def test_env_and_frames_reach_the_game(self):
        self.run_cli(self.suite(
            [{"name": "frames", "script": self.script("pass"), "env": {"FAKE_FRAMES": "7"}}],
            defaults={"env": {"FAKE_FRAMES": "3"}}))
        self.assertEqual(self.results()["frames"]["frames"], 7)
        self.assertEqual(self.results()["frames"]["status"], "PASS")

    def test_harness_env_is_set_for_the_game(self):
        rc, _, _ = self.run_cli(self.suite([
            {"name": "ok", "script": self.script("pass")},
        ]))
        self.assertEqual(rc, 0)
        outdir = self.out_root / "ok"
        self.assertTrue((outdir / "result.json").exists())
        self.assertTrue((outdir / "trace.jsonl").exists())
        self.assertTrue((outdir / "shots" / "a.png").exists())


class Repeat(SuiteCase):
    def test_repeat_deterministic_pass(self):
        rc, _, _ = self.run_cli(self.suite([
            {"name": "rep", "script": self.script("pass"), "repeat": 3},
        ]))
        self.assertEqual(rc, 0)
        for i in (1, 2, 3):
            self.assertTrue((self.out_root / f"rep_{i}" / "result.json").exists())
        self.assertEqual(self.results()["rep"]["status"], "PASS")

    def test_repeat_nondeterministic_fails(self):
        rc, _, _ = self.run_cli(self.suite([
            {"name": "rep", "script": self.script("nondeterministic"), "repeat": 2},
        ]))
        self.assertEqual(rc, 1)
        res = self.results()["rep"]
        self.assertEqual(res["status"], "FAIL")
        self.assertIn("nondeterministic", res["reason"])

    def test_repeat_nondeterministic_allowed_when_not_deterministic(self):
        rc, _, _ = self.run_cli(self.suite([
            {"name": "rep", "script": self.script("nondeterministic"), "repeat": 2,
             "deterministic": False},
        ]))
        self.assertEqual(rc, 0)
        res = self.results()["rep"]
        self.assertEqual(res["status"], "PASS")
        self.assertIn("determinism not checked", " ".join(res["notes"]))


class Selection(SuiteCase):
    def setUp(self):
        super().setUp()
        self.runs = [
            {"name": "a", "script": self.script("pass"), "tags": ["boot"]},
            {"name": "b", "script": self.script("pass"), "tags": ["boot", "slow"]},
            {"name": "c", "script": self.script("pass"), "tags": ["other"]},
        ]
        self.path = self.suite(self.runs)

    def test_list(self):
        rc, out, _ = self.run_cli(self.path, "--list")
        self.assertEqual(rc, 0)
        self.assertEqual(out.split(), ["a", "b", "c"])
        self.assertFalse(self.out_root.exists())  # nothing ran

    def test_only(self):
        rc, _, _ = self.run_cli(self.path, "--only", "c,a")
        self.assertEqual(rc, 0)
        self.assertEqual(sorted(self.results()), ["a", "c"])

    def test_tag(self):
        rc, _, _ = self.run_cli(self.path, "--tag", "boot")
        self.assertEqual(rc, 0)
        self.assertEqual(sorted(self.results()), ["a", "b"])

    def test_unknown_only_name_is_usage_error(self):
        rc, _, err = self.run_cli(self.path, "--only", "nope")
        self.assertEqual(rc, 2)
        self.assertIn("nope", err)

    def test_exe_override_replaces_suite_exe(self):
        cfg = run_suite.load_suite(self.path, exe_override="C:\\games\\other.exe")[1]
        self.assertEqual(cfg[0].exe, ["C:\\games\\other.exe"])

    def test_missing_suite_file_is_usage_error(self):
        rc, _, _ = self.run_cli(self.dir / "missing.toml")
        self.assertEqual(rc, 2)


class Output(SuiteCase):
    def test_table_and_totals(self):
        runs = [
            {"name": "ok", "script": self.script("pass")},
            {"name": "bad", "script": self.script("fail")},
            {"name": "hang", "script": self.script("hang"), "timeout": 2},
        ]
        rc, out, _ = self.run_cli(self.suite(runs))
        self.assertEqual(rc, 1)
        lines = out.splitlines()
        header = lines[0]
        for col in ("NAME", "STATUS", "FRAMES", "WALL(s)", "NOTES"):
            self.assertIn(col, header)
        self.assertEqual(lines[-1], "3 runs: 1 pass, 1 fail, 0 crash, 0 freeze, 1 timeout, 0 error")
        body = "\n".join(lines)
        self.assertLess(body.index("ok"), body.index("bad"))
        self.assertLess(body.index("bad"), body.index("hang"))
        self.assertIn("TIMEOUT", body)
        self.assertIn("fake assert failed", body)

    def test_trim_notes_to_70(self):
        self.assertEqual(len(run_suite._trim("x" * 300)), 70)
        self.assertEqual(run_suite._trim("short"), "short")

    def test_suite_result_json(self):
        self.run_cli(self.suite([
            {"name": "ok", "script": self.script("pass")},
            {"name": "bad", "script": self.script("fail")},
        ]))
        data = json.loads((self.out_root / "suite_result.json").read_text(encoding="utf-8"))
        self.assertIsInstance(data, list)
        self.assertEqual([d["name"] for d in data], ["ok", "bad"])
        for d in data:
            self.assertTrue(Path(d["outdir"]).is_dir())
            self.assertIn(d["status"], run_suite.STATUS_ORDER)
        self.assertEqual(data[0]["frames"], 120)

    def test_parallel_jobs_keep_suite_order(self):
        rc, out, _ = self.run_cli(self.suite([
            {"name": "one", "script": self.script("pass")},
            {"name": "two", "script": self.script("pass")},
            {"name": "three", "script": self.script("pass")},
        ]), "--jobs", "3")
        self.assertEqual(rc, 0)
        names = [l.split()[0] for l in out.splitlines()[2:-2]]
        self.assertEqual(names, ["one", "two", "three"])

    def test_cli_entry_point(self):
        path = self.suite([{"name": "ok", "script": self.script("pass")}])
        proc = subprocess.run([sys.executable, RUNNER, str(path), "--list"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.split(), ["ok"])


class ReferenceComparisons(SuiteCase):
    def setUp(self):
        super().setUp()
        refs = self.dir / "refs"
        refs.mkdir()
        tmp_trace = self.dir / "tmp_trace"
        tmp_trace.mkdir()
        fake_game.write_trace(tmp_trace, 120, 0.0)
        (refs / "trace_0.jsonl").write_bytes((tmp_trace / "trace.jsonl").read_bytes())
        Image.new("RGB", (64, 36), (128, 128, 128)).save(refs / "gray.png")
        Image.new("RGB", (64, 36), (0, 0, 0)).save(refs / "black.png")

    def test_trace_identical_passes(self):
        self.run_cli(self.suite([{"name": "t", "script": self.script("pass"),
                                  "ref_trace": "refs/trace_0.jsonl"}]))
        self.assertEqual(self.results()["t"]["status"], "PASS")

    def test_position_drift_fails(self):
        self.run_cli(self.suite([{"name": "t", "script": self.script("pass"),
                                  "env": {"FAKE_TRACE_SHIFT": "0.5"},
                                  "ref_trace": {"ref": "refs/trace_0.jsonl", "tol": "pos=0.05"}}]))
        res = self.results()["t"]
        self.assertEqual(res["status"], "FAIL")
        self.assertEqual(res["reason"],
                         "trace diverges at frame 0 field pos: ref=[0, 0, 1] run=[0.5, 0, 1] "
                         "(120 mismatches)")

    def test_trace_shift_within_tol_passes(self):
        self.run_cli(self.suite([{"name": "t", "script": self.script("pass"),
                                  "env": {"FAKE_TRACE_SHIFT": "0.01"},
                                  "ref_trace": {"ref": "refs/trace_0.jsonl", "tol": "pos=0.05"}}]))
        self.assertEqual(self.results()["t"]["status"], "PASS")

    def test_default_tol_is_tracediff_default(self):
        # No tol given: tracediff's default pos tolerance (0.05) applies.
        self.run_cli(self.suite([
            {"name": "small", "script": self.script("pass"),
             "env": {"FAKE_TRACE_SHIFT": "0.01"}, "ref_trace": "refs/trace_0.jsonl"},
            {"name": "big", "script": self.script("pass"),
             "env": {"FAKE_TRACE_SHIFT": "0.5"}, "ref_trace": "refs/trace_0.jsonl"},
        ]))
        self.assertEqual(self.results()["small"]["status"], "PASS")
        self.assertEqual(self.results()["big"]["status"], "FAIL")

    def test_align_first_change_offsets_frames(self):
        # Run's trace is renumbered 5 frames later; align on the first change of state (frame 10).
        self.run_cli(self.suite([
            {"name": "aligned", "script": self.script("pass"),
             "env": {"FAKE_FRAME_OFFSET": "5"},
             "ref_trace": {"ref": "refs/trace_0.jsonl", "align": "first-change:state"}},
            {"name": "unaligned", "script": self.script("pass"),
             "env": {"FAKE_FRAME_OFFSET": "5"},
             "ref_trace": "refs/trace_0.jsonl"},
        ]))
        aligned = self.results()["aligned"]
        self.assertEqual(aligned["status"], "PASS", aligned)
        self.assertIn("trace offset 5", " ".join(aligned["notes"]))
        unaligned = self.results()["unaligned"]
        self.assertEqual(unaligned["status"], "FAIL")
        self.assertIn("trace diverges at frame 5 field pos", unaligned["reason"])

    def test_missing_trace_ref_is_skip_not_fail(self):
        rc, _, _ = self.run_cli(self.suite([{"name": "t", "script": self.script("pass"),
                                             "ref_trace": "refs/none.jsonl"}]))
        self.assertEqual(rc, 0)
        res = self.results()["t"]
        self.assertEqual(res["status"], "PASS")
        self.assertIn("SKIP(ref missing)", res["notes"])

    def test_align_unknown_field_fails_loudly(self):
        self.run_cli(self.suite([{"name": "t", "script": self.script("pass"),
                                  "ref_trace": {"ref": "refs/trace_0.jsonl",
                                                "align": "first-change:pos"}}]))
        # pos is not an align field, so tracediff rejects it and the run FAILs with its message.
        res = self.results()["t"]
        self.assertEqual(res["status"], "FAIL")
        self.assertIn("align field must be one of", res["reason"])

    def test_shot_same_colour_passes(self):
        self.run_cli(self.suite([{"name": "s", "script": self.script("pass"),
                                  "ref_shots": {"a": "refs/gray.png"}}]))
        self.assertEqual(self.results()["s"]["status"], "PASS")

    def test_shot_different_colour_fails(self):
        self.run_cli(self.suite([{"name": "s", "script": self.script("pass"),
                                  "env": {"FAKE_SHOT_RGB": "255,255,255"},
                                  "ref_shots": {"a": "refs/black.png"}}]))
        res = self.results()["s"]
        self.assertEqual(res["status"], "FAIL")
        self.assertIn("shot a differs", res["reason"])

    def test_shot_table_form_with_mask_and_min_ssim(self):
        self.run_cli(self.suite([{"name": "s", "script": self.script("pass"),
                                  "ref_shots": {"a": {"ref": "refs/gray.png",
                                                      "min_ssim": 0.85, "mask": "hud"}}}]))
        self.assertEqual(self.results()["s"]["status"], "PASS")

    def test_missing_shot_ref_is_skip(self):
        self.run_cli(self.suite([{"name": "s", "script": self.script("pass"),
                                  "ref_shots": {"a": "refs/none.png"}}]))
        res = self.results()["s"]
        self.assertEqual(res["status"], "PASS")
        self.assertIn("SKIP(ref missing)", res["notes"])


if __name__ == "__main__":
    unittest.main()
