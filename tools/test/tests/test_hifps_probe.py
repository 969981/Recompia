"""Unit tests for hifps_probe (script, windows and metrics on synthetic traces, stdlib unittest only).

The synthetic traces follow the real trace format: one line per frame with f, igt (game play time), pos [x,y,z], vel, yaw, state, and spd
with phys 1 where the game gave its physics speed. A window is the rows between the start and end checkpoint frames.
"""
import math
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(HERE)
if TEST_DIR not in sys.path:
    sys.path.insert(0, TEST_DIR)

import hifps_probe as hp  # noqa: E402

RATES = (60, 120, 144, 240)
BASE_RATE = 60
VMAX = 6.0          # units / s
TAU_UP = 0.25       # s
TAU_DOWN = 0.15     # s
JUMP_V0 = 4.0       # units / s
JUMP_G = 20.0       # units / s^2
GROUND_Y = 10.0
YAW_TARGET = hp.TURN_EXPECT


def walk_window(rate):
    """Start checkpoint at frame 0. Stick forward for 3 game s (v = VMAX*(1-exp(-t/TAU_UP))), released, then v decays with TAU_DOWN for
    1.5 s. Row f is the state at the start of frame f, so row 'hold' (igt 3.0) carries the speed at the release instant.
    Returns (rows, cps)."""
    dt = 1.0 / rate
    hold = hp.frames(hp.WALK_HOLD_S, rate)
    total = hold + hp.frames(hp.WALK_TAIL_S, rate)
    rows = []
    v = 0.0
    z = 0.0
    for f in range(total + 1):
        rows.append({'f': f, 'igt': f * dt, 'pos': [0.0, GROUND_Y, z], 'vel': [0.0, 0.0, -v], 'yaw': 0.0,
                     'state': 0, 'phys': 1, 'spd': v})
        if f < hold:
            v = VMAX * (1.0 - math.exp(-(f + 1) * dt / TAU_UP))
        else:
            v *= math.exp(-dt / TAU_DOWN)
        z -= v * dt
    cps = {'walk_%d_start' % rate: 0, 'walk_%d_end' % rate: total}
    return rows, cps


def jump_window(rate):
    """Start checkpoint at frame 0. z(t) = 4t - 10t^2 for 0 <= t < 0.4 (state 1 in the air), ground height again after landing."""
    n = hp.frames(hp.JUMP_RUN_S, rate) + hp.frames(hp.JUMP_PRESS_S, rate)
    rows = []
    for f in range(n + 1):
        t = f / float(rate)
        y = GROUND_Y
        state = 0
        if t < 0.4:
            y = GROUND_Y + JUMP_V0 * t - 0.5 * JUMP_G * t * t
            state = 1
        rows.append({'f': f, 'igt': t, 'pos': [0.0, y, 0.0], 'vel': [0.0, 0.0, 0.0], 'yaw': 0.0, 'state': state})
    cps = {'jump_%d_start' % rate: 0, 'jump_%d_end' % rate: n}
    return rows, cps


def turn_window(rate):
    """Start checkpoint at frame 0. Yaw linear to YAW_TARGET over 0.2 s, constant after."""
    n = hp.frames(hp.TURN_SPREAD_S, rate) + hp.frames(hp.TURN_RUN_S, rate)
    ramp = hp.frames(hp.TURN_SPREAD_S, rate)
    rows = []
    for f in range(n + 1):
        k = min(f, ramp)
        rows.append({'f': f, 'igt': f / float(rate), 'pos': [0.0, GROUND_Y, 0.0], 'vel': [0.0, 0.0, 0.0],
                     'yaw': YAW_TARGET * k / ramp, 'state': 0})
    cps = {'turn_%d_start' % rate: 0, 'turn_%d_end' % rate: n}
    return rows, cps


WINDOWS = {'walk': walk_window, 'jump': jump_window, 'turn': turn_window}


def metrics(scen, rate, rows=None, cps=None):
    if rows is None:
        rows, cps = WINDOWS[scen](rate)
    pl = hp.window(rows, cps, scen, rate)
    return hp.FUNCS[scen](pl, rate)


def assert_rel(test, value, ref, tol, what):
    rel = abs(value - ref) / abs(ref)
    test.assertLessEqual(
        rel, tol,
        '%s: actual %.6f, expected %.6f, relative error %.4f exceeds %.4f' % (what, value, ref, rel, tol))


class TestScript(unittest.TestCase):
    def setUp(self):
        self.lines = [ln.strip() for ln in hp.script_all() if ln.strip()]

    def test_frames(self):
        self.assertEqual(hp.frames(3.0, 60), 180)
        self.assertEqual(hp.frames(0.4, 144), 58)
        self.assertEqual(hp.frames(0.2, 144), 29)
        self.assertEqual(hp.frames(0.001, 60), 1)

    def test_prologue_uses_600s_mash_timeout(self):
        self.assertEqual(self.lines[0], '# S11 physics probe: one process, @rate per block')
        self.assertIn('@timeout 0', self.lines)
        self.assertIn('@mash A 5000 area frigate_exterior_docking_hangar timeout=600000', self.lines)

    def test_one_block_per_rate_and_scenario(self):
        self.assertEqual(self.lines.count('@warp tallon tallon_canyon_cavern'), 12)
        for rate in RATES:
            for scen in ('walk', 'jump', 'turn'):
                with self.subTest(rate=rate, scen=scen):
                    i_warp = [i for i, ln in enumerate(self.lines) if ln == '@warp tallon tallon_canyon_cavern']
                    i_rate = self.lines.index('@rate %d' % rate)
                    i_start = self.lines.index('@hash %s_%d_start' % (scen, rate))
                    i_end = self.lines.index('@hash %s_%d_end' % (scen, rate))
                    self.assertLess(i_start, i_end)
                    self.assertLess(i_rate, i_start)
                    self.assertEqual(self.lines[i_end + 1], '@rate 0')
                    self.assertTrue(any(w < i_rate for w in i_warp))

    def test_rate_steps_in_order(self):
        rate_steps = [ln for ln in self.lines if ln.startswith('@rate ')]
        expect = []
        for rate in RATES:
            for _ in ('walk', 'jump', 'turn'):
                expect += ['@rate %d' % rate, '@rate 0']
        self.assertEqual(rate_steps, expect)
        self.assertEqual(self.lines[-1], '@pass')

    def test_walk_actions(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                a = hp.actions('walk', rate)
                self.assertEqual(a, ['stick left 0 1.0', '@run 3', 'stick left 0 0', '@run 1.5'])

    def test_jump_actions_scale_with_rate(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                a = hp.actions('jump', rate)
                self.assertEqual(a[0], 'press B for %d' % round(0.4 * rate))
                self.assertEqual(a[1], '@run 2')

    def test_turn_actions_scale_with_rate(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                a = hp.actions('turn', rate)
                n = round(0.2 * rate)
                parts = a[0].split()
                self.assertEqual(parts[0], 'mouse')
                self.assertEqual(parts[2:], ['0', 'for', '%d' % n])
                self.assertLess(abs(float(parts[1]) * n - 1000.0), 1e-3)
                self.assertEqual(a[1], '@run 0.5')


class TestWindow(unittest.TestCase):
    def test_window_is_between_checkpoints(self):
        rows = [{'f': f, 'igt': f / 60.0, 'pos': [0, 0, 0]} for f in range(20)]
        cps = {'walk_60_start': 5, 'walk_60_end': 9}
        pl = hp.window(rows, cps, 'walk', 60)
        self.assertEqual([r['f'] for r in pl], [5, 6, 7, 8, 9])

    def test_missing_checkpoint_gives_empty_window(self):
        rows = [{'f': 1, 'igt': 0.1, 'pos': [0, 0, 0]}]
        self.assertEqual(hp.window(rows, {'walk_60_start': 0}, 'walk', 60), [])

    def test_checkpoints_from_trace_labels(self):
        rows = [{'f': 3, 'h': '0', 'label': 'jump_60_start'}, {'f': 9, 'h': '0', 'label': 'jump_60_end'}]
        cps = hp.checkpoints('no_such_dir', rows)
        self.assertEqual(cps, {'jump_60_start': 3, 'jump_60_end': 9})


class TestMetrics(unittest.TestCase):
    def test_walk_metrics_match_60hz(self):
        base = metrics('walk', BASE_RATE)
        self.assertIsNotNone(base)
        for rate in RATES:
            if rate == BASE_RATE:
                continue
            with self.subTest(rate=rate):
                m = metrics('walk', rate)
                self.assertIsNotNone(m)
                assert_rel(self, m['steady'], base['steady'], 0.01, 'walk steady %d Hz vs 60 Hz' % rate)
                assert_rel(self, m['dist'], base['dist'], 0.02, 'walk distance %d Hz vs 60 Hz' % rate)
                assert_rel(self, m['t95'], base['t95'], 0.06, 'walk t95 %d Hz vs 60 Hz' % rate)
                assert_rel(self, m['slide'], base['slide'], 0.06, 'walk slide %d Hz vs 60 Hz' % rate)

    def test_walk_steady_speed_is_vmax(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                assert_rel(self, metrics('walk', rate)['steady'], VMAX, 0.03, 'walk steady %d Hz vs vmax' % rate)

    def test_walk_distance_is_analytic(self):
        expect = VMAX * (hp.WALK_HOLD_S - TAU_UP)
        for rate in RATES:
            with self.subTest(rate=rate):
                assert_rel(self, metrics('walk', rate)['dist'], expect, 0.02, 'walk distance %d Hz vs analytic' % rate)

    def test_walk_slide_is_analytic(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                assert_rel(self, metrics('walk', rate)['slide'], VMAX * TAU_DOWN, 0.02, 'walk slide %d Hz vs analytic' % rate)

    def test_walk_falls_back_to_position_without_spd(self):
        rows, cps = walk_window(BASE_RATE)
        for r in rows:
            r.pop('spd', None)
        m = metrics('walk', BASE_RATE, rows, cps)
        self.assertIsNotNone(m)
        self.assertGreater(m['fallback'], 0)
        assert_rel(self, m['steady'], VMAX, 0.03, 'walk steady speed from the position difference')

    def test_jump_metrics_match_60hz(self):
        base = metrics('jump', BASE_RATE)
        for rate in RATES:
            if rate == BASE_RATE:
                continue
            with self.subTest(rate=rate):
                m = metrics('jump', rate)
                assert_rel(self, m['peak'], base['peak'], 0.02, 'jump peak %d Hz vs 60 Hz' % rate)
                assert_rel(self, m['air'], base['air'], 0.03, 'jump air %d Hz vs 60 Hz' % rate)

    def test_jump_peak_is_analytic(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                assert_rel(self, metrics('jump', rate)['peak'], 0.4, 0.03, 'jump peak %d Hz vs 0.4' % rate)

    def test_jump_airtime_is_game_time(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                assert_rel(self, metrics('jump', rate)['air'], 0.4, 0.03, 'jump airtime %d Hz vs 0.4 s' % rate)

    def test_turn_yaw_matches_target(self):
        for rate in RATES:
            with self.subTest(rate=rate):
                assert_rel(self, metrics('turn', rate)['yaw'], YAW_TARGET, 0.01, 'turn yaw %d Hz' % rate)

    def test_turn_expected_value(self):
        self.assertAlmostEqual(hp.TURN_EXPECT, -0.767945, places=6)

    def test_too_few_rows_return_none(self):
        short = [{'f': i, 'igt': i / 60.0, 'pos': [0.0, float(i), 0.0], 'vel': [0.0, 0.0, 0.0], 'yaw': 0.0, 'state': 0}
                 for i in range(9)]
        for scen in ('walk', 'jump', 'turn'):
            with self.subTest(scen=scen):
                self.assertIsNone(hp.FUNCS[scen](short, 60))
                self.assertIsNone(hp.FUNCS[scen]([], 60))


class TestSplit(unittest.TestCase):
    def test_split_integrates_both_sides(self):
        series = [(0.0, 2.0), (1.0, 2.0), (2.0, 2.0)]
        before, after = hp._split(series, 1.5)
        self.assertAlmostEqual(before, 3.0)
        self.assertAlmostEqual(after, 1.0)


if __name__ == '__main__':
    unittest.main()
