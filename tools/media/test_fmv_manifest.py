"""Unit test for the manifest builder in fmv_manifest.py, on a synthetic MP4 made with ffmpeg.

Run from tools\\media:  python -I -m unittest
"""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fmv_manifest  # noqa: E402

FFMPEG = fmv_manifest.find_tool("ffmpeg")
FFPROBE = fmv_manifest.find_tool("ffprobe")


@unittest.skipUnless(FFMPEG and FFPROBE, "ffmpeg/ffprobe not found")
class BuildMovieEntryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.mp4 = os.path.join(cls._tmp.name, "clip.mp4")
        r = subprocess.run([
            FFMPEG, "-v", "error", "-y",
            "-f", "lavfi", "-i", "testsrc=size=64x64:rate=30:duration=2",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=2",
            "-map", "0:v", "-map", "1:a",
            "-c:v", "mpeg4", "-q:v", "5", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-ac", "1", "-ar", "44100",
            "-t", "2", cls.mp4,
        ], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("ffmpeg failed: " + r.stderr.strip())
        cls.entry = fmv_manifest.build_movie_entry(cls.mp4, "clip.mp4", "test/clip.pak", 1234, FFPROBE)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_video_frames_and_size(self):
        v = self.entry["video"]
        self.assertEqual(v["frame_count"], 60)
        self.assertEqual(v["width"], 64)
        self.assertEqual(v["height"], 64)
        self.assertEqual((v["fps_num"], v["fps_den"]), (30, 1))
        self.assertEqual(len(v["pts_us"]), 60)
        self.assertEqual(v["pts_us"], sorted(v["pts_us"]))
        self.assertTrue(all(isinstance(t, int) for t in v["pts_us"]))

    def test_audio_stream(self):
        a = self.entry["audio"]
        self.assertIsNotNone(a)
        self.assertEqual(a["channels"], 1)
        self.assertEqual(a["sample_rate"], 44100)
        self.assertEqual(a["codec"], "aac")
        self.assertGreater(a["packet_count"], 0)
        self.assertAlmostEqual(a["duration_us"] / 1e6, 2.0, delta=0.1)

    def test_record_fields(self):
        self.assertEqual(self.entry["file"], "clip.mp4")
        self.assertEqual(self.entry["pak"], "test/clip.pak")
        self.assertEqual(self.entry["container_bytes"], 1234)
        self.assertEqual(self.entry["mp4_bytes"], os.path.getsize(self.mp4))


if __name__ == "__main__":
    unittest.main()
