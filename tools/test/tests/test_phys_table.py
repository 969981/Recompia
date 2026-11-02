"""Unit tests for phys_table (analysis half: speeds, walk/jump metrics, crossings, windows, checkpoints, table; stdlib unittest only).

The synthetic traces follow the real trace format: one row per frame with f, igt (game play time), pos [x, y, z] and state. Walk and jump
positions are the analytic integral of the speed or height, sampled at the rate under test, so the expected numbers are known exactly.
"""
import contextlib
import io
import json
import math
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(HERE)
if TEST_DIR not in sys.path:
    sys.path.insert(0, TEST_DIR)

import phys_table as pt  # noqa: E402

RATES = (60, 120, 144, 240)
BASE_RATE = 60
VMAX_WALK = 10.8     # units / s, full stick
ACCEL_TAU = 0.09     # s, speed rises as VMAX * (1 - exp(-t / ACCEL_TAU))
DECEL_TAU = 0.15     # s, speed decays as v * exp(-t / DECEL_TAU) after the stick goes to zero
JUMP_V0 = 10.0       # units / s
JUMP_G = 35.0        # units / s^2
JUMP_OBS_S = 2.0     # s of observation after the jump input
JUMP_HSPEED = 4.0    # units / s, sideways speed for the horizontal jump


def stick_x(t, vmax, hold):
    """x position at igt t for a speed that rises with ACCEL_TAU while the stick is down (for `hold` s), then decays with DECEL_TAU."""
    if t <= hold:
        return vmax * (t - ACCEL_TAU * (1.0 - math.exp(-t / ACCEL_TAU)))
    x_hold = vmax * (hold - ACCEL_TAU * (1.0 - math.exp(-hold / ACCEL_TAU)))
    v_hold = vmax * (1.0 - math.exp(-hold / ACCEL_TAU))
    return x_hold + v_hold * DECEL_TAU * (1.0 - math.exp(-(t - hold) / DECEL_TAU))


def stick_rows(rate, vmax, hold, tail):
    """one row per frame from igt 0 to hold + tail, moving along +x"""
    n = int(round((hold + tail) * rate))
    return [{'f': f, 'igt': f / float(rate), 'pos': [stick_x(f / float(rate), vmax, hold), 0.0, 0.0], 'state': 0}
            for f in range(n + 1)]


def jump_rows(rate, hspeed=0.0):
    """ballistic jump from ground y=0 with JUMP_V0 and JUMP_G; height is clamped to 0 after landing"""
    n = int(round(JUMP_OBS_S * rate))
    rows = []
    for f in range(n + 1):
        t = f / float(rate)
        y = max(0.0, JUMP_V0 * t - 0.5 * JUMP_G * t * t)
        rows.append({'f': f, 'igt': t, 'pos': [hspeed * t, y, 0.0], 'state': 1 if y > 0 else 0})
    return rows


def assert_rel(test, value, ref, tol, what):
    rel = abs(value - ref) / abs(ref)
    test.assertLessEqual(
        rel, tol,
        '%s: actual %.6f, expected %.6f, relative error %.4f exceeds %.4f' % (what, value, ref, rel, tol))


class TestPosSpeeds(unittest.TestCase):
    def test_horizontal_speed_from_position_difference(self):
        rows = [{'f': 0, 'igt': 0.0, 'pos': [0.0, 5.0, 0.0]},
                {'f': 1, 'igt': 0.5, 'pos': [1.0, 9.0, 0.0]},
                {'f': 2, 'igt': 1.0, 'pos': [3.0, 0.0, 4.0]}]
        sp = pt.pos_speeds(rows)
        self.assertEqual([t for t, _ in sp], [0.5, 1.0])
        self.assertAlmostEqual(sp[0][1], 2.0)
        self.assertAlmostEqual(sp[1][1], math.hypot(2.0, 4.0) / 0.5)


class TestWalk(unittest.TestCase):
    def check_scenario(self, scen, vmax):
        hold, tail = pt.HOLD[scen], pt.TAIL[scen]
        base = pt.walk_metrics(stick_rows(BASE_RATE, vmax, hold, tail), scen)
        self.assertIsNotNone(base)
        for rate in RATES:
            with self.subTest(scen=scen, rate=rate):
                m = pt.walk_metrics(stick_rows(rate, vmax, hold, tail), scen)
                self.assertIsNotNone(m)
                assert_rel(self, m['steady'], vmax, 0.005, '%s steady %d Hz vs %g' % (scen, rate, vmax))
                assert_rel(self, m['fall'], base['fall'], 0.03, '%s fall %d Hz vs 60 Hz' % (scen, rate))

    def test_walk_full_stick(self):
        self.check_scenario('walk', VMAX_WALK)

    def test_walk_fall_is_decay_time(self):
        # 75 % to 25 % of an exponential decay takes DECEL_TAU * ln 3; the measured value must sit close to that
        m = pt.walk_metrics(stick_rows(BASE_RATE, VMAX_WALK, pt.HOLD['walk'], pt.TAIL['walk']), 'walk')
        assert_rel(self, m['fall'], DECEL_TAU * math.log(3.0), 0.03, 'walk fall vs decay time')

    def test_half_stick(self):
        # half stick: 0.5 of full speed, uses HOLD['half'] and TAIL['half']
        self.check_scenario('half', 0.5 * VMAX_WALK)

    def test_ball(self):
        # morph ball: synthetic top speed 7.2 units / s, uses HOLD['ball'] and TAIL['ball']
        self.check_scenario('ball', 7.2)

    def test_too_few_rows_give_none(self):
        self.assertIsNone(pt.walk_metrics(stick_rows(BASE_RATE, VMAX_WALK, 0.1, 0.0)[:5], 'walk'))


class TestJump(unittest.TestCase):
    def test_peak_and_air_at_every_rate(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                m = pt.jump_metrics(jump_rows(rate))
                self.assertIsNotNone(m)
                assert_rel(self, m['peak'], JUMP_V0 ** 2 / (2 * JUMP_G), 0.01, 'jump peak %d Hz' % rate)
                assert_rel(self, m['air'], 2 * JUMP_V0 / JUMP_G, 0.03, 'jump air %d Hz' % rate)
                self.assertNotIn('hdist', m)

    def test_horizontal_distance_is_speed_times_air(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                m = pt.jump_metrics(jump_rows(rate, JUMP_HSPEED), horizontal=True)
                self.assertIsNotNone(m)
                assert_rel(self, m['hdist'], JUMP_HSPEED * m['air'], 0.03, 'jump hdist %d Hz' % rate)


class TestCrossing(unittest.TestCase):
    def test_rising_crossing_is_interpolated(self):
        series = [(0.0, 0.0), (0.5, 1.0), (1.0, 2.0), (1.5, 3.0)]
        self.assertAlmostEqual(pt.crossing(series, 0.75, True), 0.375)
        self.assertAlmostEqual(pt.crossing(series, 2.5, True), 1.25)

    def test_falling_crossing_is_interpolated(self):
        series = [(0.0, 3.0), (0.5, 2.0), (1.0, 1.0), (1.5, 0.0)]
        self.assertAlmostEqual(pt.crossing(series, 1.5, False), 0.75)

    def test_no_crossing_returns_none(self):
        series = [(0.0, 0.0), (1.0, 1.0), (2.0, 2.0)]
        self.assertIsNone(pt.crossing(series, 10.0, True))
        self.assertIsNone(pt.crossing(series, 0.5, False))


class TestWindow(unittest.TestCase):
    def test_rows_outside_checkpoints_are_dropped(self):
        rows = [{'f': f, 'igt': f / 60.0, 'pos': [0.0, 0.0, 0.0]} for f in range(20)]
        cps = {'walk_60_start': 5, 'walk_60_end': 12}
        pl = pt.window(rows, cps, 'walk', 60)
        self.assertEqual([r['f'] for r in pl], list(range(5, 13)))

    def test_rows_without_igt_advance_are_dropped(self):
        rows = [{'f': f, 'igt': f / 60.0, 'pos': [0.0, 0.0, 0.0]} for f in range(20)]
        rows[8]['igt'] = rows[7]['igt']          # the game clock did not move on frame 8 (a load)
        cps = {'walk_60_start': 5, 'walk_60_end': 12}
        pl = pt.window(rows, cps, 'walk', 60)
        self.assertEqual([r['f'] for r in pl], [5, 6, 7, 9, 10, 11, 12])

    def test_missing_checkpoint_gives_empty_window(self):
        rows = [{'f': 1, 'igt': 0.1, 'pos': [0.0, 0.0, 0.0]}]
        self.assertEqual(pt.window(rows, {'walk_60_start': 0}, 'walk', 60), [])


class TestCheckpoints(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='phys_table_test_')
        os.makedirs(os.path.join(self.dir, 'out'))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def write_trace(self, rows, junk=()):
        with open(os.path.join(self.dir, 'out', 'trace.jsonl'), 'w') as fh:
            for r in rows:
                fh.write(json.dumps(r) + '\n')
            for line in junk:
                fh.write(line + '\n')

    def write_result(self, checkpoints):
        with open(os.path.join(self.dir, 'out', 'result.json'), 'w') as fh:
            json.dump({'checkpoints': [{'label': lb, 'frame': f} for lb, f in checkpoints]}, fh)

    def test_load_trace_skips_truncated_and_broken_lines(self):
        self.write_trace([{'f': 0, 'igt': 0.0, 'pos': [0, 0, 0]}, {'f': 1, 'igt': 0.1, 'pos': [0, 0, 0]}],
                         junk=['{"f": 2, "igt"', '{not json}'])
        rows = pt.load_trace(self.dir)
        self.assertEqual([r['f'] for r in rows], [0, 1])

    def test_result_json_checkpoints_win_and_trace_labels_fill_the_rest(self):
        self.write_result([('walk_60_start', 4), ('walk_60_end', 10)])
        self.write_trace([{'f': 5, 'igt': 0.1, 'pos': [0, 0, 0], 'label': 'walk_60_start'},
                          {'f': 10, 'igt': 0.2, 'pos': [0, 0, 0], 'label': 'walk_60_end'},
                          {'f': 30, 'igt': 0.5, 'pos': [0, 0, 0], 'label': 'jump_60_start'},
                          {'f': 40, 'igt': 0.7, 'pos': [0, 0, 0], 'label': 'jump_60_end'}])
        cps = pt.checkpoints(self.dir, pt.load_trace(self.dir))
        self.assertEqual(cps, {'walk_60_start': 4, 'walk_60_end': 10, 'jump_60_start': 30, 'jump_60_end': 40})

    def test_trace_labels_used_when_no_result_json(self):
        self.write_trace([{'f': 3, 'igt': 0.05, 'pos': [0, 0, 0], 'label': 'jump_60_start'},
                          {'f': 9, 'igt': 0.15, 'pos': [0, 0, 0], 'label': 'jump_60_end'}])
        cps = pt.checkpoints(self.dir, pt.load_trace(self.dir))
        self.assertEqual(cps, {'jump_60_start': 3, 'jump_60_end': 9})


class TestTable(unittest.TestCase):
    def metrics(self, **over):
        m = {'steady': 10.8, 'rise': 0.10, 'fall': 0.16, 'd1': 0.50, 'total': 4.0}
        m.update(over)
        return m

    def run_table(self, res):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ok = pt.table(res, RATES, ['walk'])
        return ok, buf.getvalue()

    def test_within_one_percent_is_true(self):
        res = {('walk', r): self.metrics() for r in RATES}
        res[('walk', 144)] = self.metrics(steady=10.8 * 1.005)
        ok, out = self.run_table(res)
        self.assertTrue(ok)
        self.assertNotIn(')!', out)

    def test_beyond_one_percent_is_false(self):
        res = {('walk', r): self.metrics() for r in RATES}
        res[('walk', 240)] = self.metrics(fall=0.16 * 1.02)
        ok, out = self.run_table(res)
        self.assertFalse(ok)
        self.assertIn(')!', out)

    def test_info_keys_beyond_one_percent_do_not_fail(self):
        res = {('walk', r): self.metrics() for r in RATES}
        res[('walk', 240)] = self.metrics(d1=0.50 * 1.05)
        res[('walk', 120)] = self.metrics(rise=0.10 * 0.9)
        ok, out = self.run_table(res)
        self.assertTrue(ok)
        self.assertIn(')~', out)
        self.assertNotIn(')!', out)


if __name__ == '__main__':
    unittest.main()
