import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent.parent
GENERATOR = TEST_DIR / "gen_regions.py"

GUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
WORLD_KEYS = {"tallon", "chozo", "magmoor", "phendrana", "mines", "crater", "frigate"}
ALIASES = {"frigate_start", "tallon_start"}
FRIGATE_GUID = "4b5e8635-0f43-4ffb-9427-2e62186e517d"


class GenRegionsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        out = Path(cls._tmp.name) / "regions.tsv"
        proc = subprocess.run(
            [sys.executable, str(GENERATOR), "--out", str(out)],
            capture_output=True, text=True, encoding="utf-8",
        )
        cls.returncode = proc.returncode
        cls.stderr = proc.stderr
        cls.lines = out.read_text(encoding="utf-8").splitlines() if out.exists() else []
        cls.rows = [l.split("\t") for l in cls.lines if l and not l.startswith("#")]

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def setUp(self):
        self.assertEqual(self.returncode, 0, self.stderr)
        self.assertTrue(self.rows, "generator produced no rows")

    def world_rows(self):
        return [r for r in self.rows if len(r) == 3 and r[2] == "" and r[0] in WORLD_KEYS]

    def area_rows(self):
        return [r for r in self.rows if len(r) == 3 and r[2] != "" and r[0] not in ALIASES]

    def test_seven_world_lines(self):
        worlds = self.world_rows()
        self.assertEqual(len(worlds), 7)
        self.assertEqual({r[0] for r in worlds}, WORLD_KEYS)

    def test_every_area_has_existing_world(self):
        world_guids = {r[1] for r in self.world_rows()}
        world_keys = {r[0] for r in self.world_rows()}
        for r in self.area_rows():
            self.assertIn(r[1], world_guids, f"area {r[0]} has unknown world guid")
            world_key = r[0].split("_", 1)[0]
            self.assertIn(world_key, world_keys, f"area {r[0]} has no world prefix")
        for r in self.rows:
            if r[0] in ALIASES:
                self.assertIn(r[1], world_guids, f"alias {r[0]} has unknown world guid")

    def test_all_guids_well_formed(self):
        for r in self.rows:
            self.assertRegex(r[1], GUID_RE, f"bad world guid on {r[0]}")
            if r[2]:
                self.assertRegex(r[2], GUID_RE, f"bad area guid on {r[0]}")

    def test_keys_unique(self):
        keys = [r[0] for r in self.rows]
        self.assertEqual(len(keys), len(set(keys)), "duplicate keys")

    def test_frigate_has_at_least_ten_areas(self):
        frigate = [r for r in self.area_rows() if r[0].startswith("frigate_")]
        self.assertGreaterEqual(len(frigate), 10)

    def test_frigate_world_line_present(self):
        self.assertIn(f"frigate\t{FRIGATE_GUID}\t", self.lines)


if __name__ == "__main__":
    unittest.main()
