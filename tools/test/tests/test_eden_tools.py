"""Unit tests for eden_config.set_values / apply / HARNESS_SETTINGS and the edeninput key table and _lparam.

Run from the tools test folder:
  ..\\.venv\\Scripts\\python.exe -I -m unittest discover -s tests -p "test_eden_tools.py" -v

Only pure functions and module-level data are used. No window is found and no message is posted.
"""
import os
import sys
import tempfile
import unittest

_TOOLS_TEST = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _TOOLS_TEST not in sys.path:
    sys.path.insert(0, _TOOLS_TEST)

try:
    import eden_config
    _EC_ERR = None
except Exception as e:  # tests are skipped with the reason below
    eden_config = None
    _EC_ERR = e

try:
    import edeninput
    _EI_ERR = None
except Exception as e:  # windll missing or import failure: skip, do not crash discovery
    edeninput = None
    _EI_ERR = e


# A small Qt-style ini in the layout eden_config expects.
SAMPLE = [
    "[Core]",
    "use_speed_limit\\default=false",
    "use_speed_limit=true",
    "",
    "[Debugging]",
    "use_gdbstub\\default=true",
    "use_gdbstub=true",
    "use_gdbstub_extra\\default=true",
    "use_gdbstub_extra=true",
    "gdbstub_port\\default=true",
    "gdbstub_port=1234",
    "",
    "[Audio]",
    "output_engine\\default=true",
    "output_engine=0",
]


@unittest.skipIf(eden_config is None, f"eden_config import failed: {_EC_ERR!r}")
class SetValuesTests(unittest.TestCase):

    def test_replaces_value_and_sets_default_line_false(self):
        out = eden_config.set_values(SAMPLE, {"Debugging.use_gdbstub": "false"})
        expected = list(SAMPLE)
        expected[5] = "use_gdbstub\\default=false"
        expected[6] = "use_gdbstub=false"
        self.assertEqual(out, expected)

    def test_other_section_untouched_when_one_section_changes(self):
        out = eden_config.set_values(SAMPLE, {"Audio.output_engine": "3"})
        expected = list(SAMPLE)
        expected[13] = "output_engine\\default=false"
        expected[14] = "output_engine=3"
        self.assertEqual(out, expected)

    def test_appends_missing_key_to_end_of_its_section(self):
        out = eden_config.set_values(SAMPLE, {"Debugging.new_key": "x"})
        i = out.index("[Audio]")
        self.assertEqual(out[:12], SAMPLE[:12])  # everything up to and including Debugging's blank line
        self.assertEqual(out[12:i], ["new_key\\default=false", "new_key=x"])
        self.assertEqual(out[i:], SAMPLE[12:])

    def test_appends_missing_key_to_last_section(self):
        out = eden_config.set_values(SAMPLE, {"Audio.new_key": "x"})
        self.assertEqual(out, SAMPLE + ["new_key\\default=false", "new_key=x"])

    def test_creates_missing_section_at_end_of_file(self):
        out = eden_config.set_values(SAMPLE, {"Video.fullscreen": "true"})
        self.assertEqual(out, SAMPLE + ["[Video]", "fullscreen\\default=false", "fullscreen=true"])

    def test_two_existing_keys_in_one_section_both_replaced(self):
        out = eden_config.set_values(
            SAMPLE, {"Debugging.use_gdbstub": "false", "Debugging.gdbstub_port": "6543"})
        expected = list(SAMPLE)
        expected[5] = "use_gdbstub\\default=false"
        expected[6] = "use_gdbstub=false"
        expected[9] = "gdbstub_port\\default=false"
        expected[10] = "gdbstub_port=6543"
        self.assertEqual(out, expected)

    def test_two_new_keys_in_one_section_appended_in_order(self):
        out = eden_config.set_values(SAMPLE, {"Debugging.a": "1", "Debugging.b": "2"})
        i = out.index("[Audio]")
        self.assertEqual(out[12:i], ["a\\default=false", "a=1", "b\\default=false", "b=2"])
        self.assertEqual(out[i:], SAMPLE[12:])

    def test_same_key_name_in_other_section_untouched(self):
        lines = [
            "[Core]",
            "use_gdbstub\\default=true",
            "use_gdbstub=true",
            "[Debugging]",
            "use_gdbstub\\default=true",
            "use_gdbstub=true",
        ]
        out = eden_config.set_values(lines, {"Debugging.use_gdbstub": "false"})
        self.assertEqual(out, [
            "[Core]",
            "use_gdbstub\\default=true",
            "use_gdbstub=true",
            "[Debugging]",
            "use_gdbstub\\default=false",
            "use_gdbstub=false",
        ])

    def test_prefix_key_not_touched_longer_key_after(self):
        # use_gdbstub is a prefix of use_gdbstub_extra, which follows it in SAMPLE
        out = eden_config.set_values(SAMPLE, {"Debugging.use_gdbstub": "false"})
        self.assertIn("use_gdbstub_extra\\default=true", out)
        self.assertIn("use_gdbstub_extra=true", out)
        self.assertNotIn("use_gdbstub_extra=false", out)

    def test_prefix_key_not_touched_longer_key_before(self):
        # longer key appears first in the section; shorter key must still hit only its own line
        lines = [
            "[Debugging]",
            "use_gdbstub_extra\\default=true",
            "use_gdbstub_extra=true",
            "use_gdbstub\\default=true",
            "use_gdbstub=true",
        ]
        out = eden_config.set_values(lines, {"Debugging.use_gdbstub": "false"})
        self.assertEqual(out, [
            "[Debugging]",
            "use_gdbstub_extra\\default=true",
            "use_gdbstub_extra=true",
            "use_gdbstub\\default=false",
            "use_gdbstub=false",
        ])

    def test_shorter_key_setting_leaves_longer_key_alone(self):
        out = eden_config.set_values(SAMPLE, {"Debugging.use_gdbstub_extra": "false"})
        self.assertIn("use_gdbstub=true", out)
        self.assertIn("use_gdbstub\\default=true", out)
        self.assertIn("use_gdbstub_extra=false", out)

    def test_empty_input_creates_section(self):
        out = eden_config.set_values([], {"Debugging.use_gdbstub": "false"})
        self.assertEqual(out, ["[Debugging]", "use_gdbstub\\default=false", "use_gdbstub=false"])

    def test_no_settings_returns_lines_unchanged(self):
        self.assertEqual(eden_config.set_values([], {}), [])
        self.assertEqual(eden_config.set_values(SAMPLE, {}), SAMPLE)

    
    def test_default_line_not_duplicated_when_no_value_line_exists(self):
        # BUG (expected failure): a key that has a \default line but no value line gets its
        # \default line written twice, because the default-line branch does not mark the key as seen.
        lines = ["[Debugging]", "use_gdbstub\\default=true"]
        out = eden_config.set_values(lines, {"Debugging.use_gdbstub": "false"})
        self.assertEqual(out, ["[Debugging]", "use_gdbstub\\default=false", "use_gdbstub=false"])


@unittest.skipIf(eden_config is None, f"eden_config import failed: {_EC_ERR!r}")
class ApplyAndHarnessSettingsTests(unittest.TestCase):

    def test_apply_writes_lf_endings_and_single_trailing_newline(self):
        original = b"[Core]\r\nfoo=1\r\n\r\n[Debugging]\r\nuse_gdbstub\\default=true\r\nuse_gdbstub=true\r\n"
        with tempfile.TemporaryDirectory() as d:
            cfg = os.path.join(d, "sdl2-config.ini")
            with open(cfg, "wb") as f:
                f.write(original)
            eden_config.apply(cfg, {"Debugging.use_gdbstub": "false"})
            with open(cfg, "rb") as f:
                data = f.read()
        self.assertNotIn(b"\r", data)
        self.assertTrue(data.endswith(b"\n"))
        self.assertFalse(data.endswith(b"\n\n"))
        self.assertEqual(
            data,
            b"[Core]\nfoo=1\n\n[Debugging]\nuse_gdbstub\\default=false\nuse_gdbstub=false\n",
        )

    def test_apply_is_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            cfg = os.path.join(d, "sdl2-config.ini")
            with open(cfg, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(SAMPLE) + "\n")
            eden_config.apply(cfg, {"Debugging.use_gdbstub": "false", "Video.x": "1"})
            with open(cfg, "rb") as f:
                first = f.read()
            eden_config.apply(cfg, {"Debugging.use_gdbstub": "false", "Video.x": "1"})
            with open(cfg, "rb") as f:
                second = f.read()
        self.assertEqual(first, second)

    def test_harness_settings_values(self):
        self.assertEqual(eden_config.HARNESS_SETTINGS["Debugging.use_gdbstub"], "false")
        self.assertEqual(eden_config.HARNESS_SETTINGS["Audio.output_engine"], "3")


@unittest.skipIf(edeninput is None, f"edeninput import failed: {_EI_ERR!r}")
class EdenInputKeyTableTests(unittest.TestCase):

    SWITCH_BUTTONS = ["A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS"]
    ARROWS = ["LSTICK_UP", "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT"]

    def test_every_entry_is_vk_scan_ext_tuple_in_range(self):
        self.assertTrue(edeninput.KEYS)
        for name, entry in edeninput.KEYS.items():
            with self.subTest(key=name):
                self.assertIsInstance(entry, tuple)
                self.assertEqual(len(entry), 3)
                vk, scan, ext = entry
                self.assertTrue(1 <= vk <= 255, f"vk {vk:#x}")
                self.assertTrue(1 <= scan <= 0x7F, f"scan {scan:#x}")
                self.assertIsInstance(ext, bool)

    def test_switch_buttons_present(self):
        for b in self.SWITCH_BUTTONS:
            with self.subTest(button=b):
                self.assertIn(b, edeninput.KEYS)

    def test_arrow_keys_marked_extended(self):
        for name in self.ARROWS:
            with self.subTest(key=name):
                self.assertTrue(edeninput.KEYS[name][2])

    def test_non_arrow_keys_not_extended(self):
        for name, (_, _, ext) in edeninput.KEYS.items():
            if name not in self.ARROWS:
                with self.subTest(key=name):
                    self.assertFalse(ext)

    def test_virtual_key_values_unique(self):
        vks = [entry[0] for entry in edeninput.KEYS.values()]
        self.assertEqual(len(vks), len(set(vks)))


@unittest.skipIf(edeninput is None, f"edeninput import failed: {_EI_ERR!r}")
class EdenInputLParamTests(unittest.TestCase):

    def test_plain_key_down_lparam(self):
        lp = edeninput._lparam(0x1E, False, False)
        self.assertEqual(lp & 0xFFFF, 1)                  # repeat count 1
        self.assertEqual((lp >> 16) & 0xFF, 0x1E)         # scan code in bits 16..23
        self.assertEqual(lp, 1 | (0x1E << 16))
        for bit in (24, 30, 31):
            self.assertEqual(lp & (1 << bit), 0, f"bit {bit} set")

    def test_extended_key_up_lparam(self):
        lp = edeninput._lparam(0x48, True, True)
        self.assertEqual(lp & 0xFFFF, 1)
        self.assertEqual((lp >> 16) & 0xFF, 0x48)
        self.assertTrue(lp & (1 << 24), "extended bit 24 not set")
        self.assertTrue(lp & (1 << 30), "previous-state bit 30 not set")
        self.assertTrue(lp & (1 << 31), "transition bit 31 not set")

    def test_arrow_lparam_uses_extended_bit(self):
        vk, sc, ext = edeninput.KEYS["LSTICK_UP"]
        lp = edeninput._lparam(sc, ext, False)
        self.assertTrue(lp & (1 << 24))


if __name__ == "__main__":
    unittest.main()
