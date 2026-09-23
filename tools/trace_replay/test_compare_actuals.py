#!/usr/bin/env python3
"""Synthetic-only regressions for the actual-vs-actual SSIM and the archive summary.

No device, no PNG the retrace produced and no golden: every raster here is written by the test
so that each expected SSIM is a closed form (see compare_actuals.self_test for the derivations).
The summary half runs on a fabricated --archive-dir tree, because the failure it has to catch -
"three repeats were called identical while one of them had no actual image at all" - is a shape
of the DIRECTORY, not of the images.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_actuals as ca
import run_android_retrace_local as retrace
import trace_cases


# The two closed forms the self-test derives. Written out here as literals rather than
# recomputed from C1/C2, so a typo in the constants cannot agree with itself.
BLACK_VS_WHITE = 9.999000099990002e-05
INVERTED_HALVES = -0.9964064683569576


def solid(width, height, value, alpha=255):
    return bytes([value, value, value, alpha] * (width * height))


def write_png(path, width, height, rgba):
    Path(path).write_bytes(ca._png_bytes(width, height, rgba))


class Ssim(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="compare-actuals-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_known_pairs(self):
        black, white = self.root / "a.png", self.root / "b.png"
        write_png(black, 8, 4, solid(8, 4, 0))
        write_png(white, 8, 4, solid(8, 4, 255))
        self.assertEqual(ca.compare_files(black, black)["ssim"], 1.0)
        self.assertAlmostEqual(ca.compare_files(black, white)["ssim"], BLACK_VS_WHITE, places=15)
        self.assertEqual(ca.compare_files(black, white)["mismatch_pixels"], 32)

    def test_alpha_is_ignored_and_rgb_is_averaged(self):
        """Alpha must not enter the score: the gate averages three channels, not four."""
        opaque, transparent = self.root / "o.png", self.root / "t.png"
        write_png(opaque, 4, 4, solid(4, 4, 200, alpha=255))
        write_png(transparent, 4, 4, solid(4, 4, 200, alpha=0))
        self.assertEqual(ca.compare_files(opaque, transparent)["ssim"], 1.0)
        self.assertEqual(ca.compare_files(opaque, transparent)["mismatch_pixels"], 0)

    def test_crop_selects_the_window_and_a_size_mismatch_is_refused(self):
        halves, inverse = self.root / "h.png", self.root / "i.png"
        left, right = bytearray(), bytearray()
        for _ in range(4):
            for x in range(8):
                value = 0 if x < 4 else 255
                left += bytes([value, value, value, 255])
                right += bytes([255 - value] * 3 + [255])
        write_png(halves, 8, 4, bytes(left))
        write_png(inverse, 8, 4, bytes(right))
        self.assertAlmostEqual(ca.compare_files(halves, inverse)["ssim"], INVERTED_HALVES, places=15)
        self.assertAlmostEqual(ca.compare_files(halves, inverse, 0, 0, 4, 4)["ssim"],
                               BLACK_VS_WHITE, places=15)
        small = self.root / "s.png"
        write_png(small, 4, 4, solid(4, 4, 0))
        with self.assertRaises(ca.ImageError):
            ca.compare_files(halves, small)

    def test_eight_and_sixteen_bit_and_rgb_decode_to_the_same_raster(self):
        """The three encodings the fixtures and the device actually produce agree."""
        def chunk(kind, body):
            import zlib
            return (len(body).to_bytes(4, "big") + kind + body
                    + zlib.crc32(kind + body).to_bytes(4, "big"))

        def encode(path, width, height, depth, color_type, samples):
            import zlib
            raw = bytearray()
            per_row = len(samples) // height
            for y in range(height):
                raw.append(0)
                raw += samples[y * per_row:(y + 1) * per_row]
            Path(path).write_bytes(
                ca._PNG_MAGIC
                + chunk(b"IHDR", width.to_bytes(4, "big") + height.to_bytes(4, "big")
                        + bytes([depth, color_type, 0, 0, 0]))
                + chunk(b"IDAT", zlib.compress(bytes(raw)))
                + chunk(b"IEND", b""))

        rgba = self.root / "rgba.png"
        rgb = self.root / "rgb.png"
        rgba16 = self.root / "rgba16.png"
        write_png(rgba, 2, 2, solid(2, 2, 96))
        encode(rgb, 2, 2, 8, 2, bytes([96, 96, 96] * 4))
        # 16-bit big-endian samples; png_set_strip_16 keeps the high byte, so RGB reads 96
        # and alpha reads 255 - the same raster the 8-bit files above carry.
        encode(rgba16, 2, 2, 16, 6, bytes([96, 0, 96, 0, 96, 0, 255, 0] * 4))
        first = ca.read_png_rgba(rgba)
        for other in (rgb, rgba16):
            decoded = ca.read_png_rgba(other)
            self.assertEqual((decoded.width, decoded.height), (first.width, first.height))
            self.assertEqual(decoded.pixels, first.pixels, other.name)


class Summary(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="compare-actuals-summary-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def repeat(self, arm, index, rgba=None, ssim=1.0, passed=True, crop=(0, 0, 0, 0),
               write_result=True, write_actual=True):
        directory = self.root / arm / ("repeat-%02d" % index)
        directory.mkdir(parents=True)
        if write_actual:
            write_png(directory / (arm + "-actual.png"), 8, 4,
                      rgba if rgba is not None else solid(8, 4, 64))
        if write_result:
            (directory / "result.json").write_text(json.dumps({
                "passed": passed, "statusCode": 0 if passed else 1, "backend": "DirectVulkan",
                "ssim": ssim, "ssimThreshold": 0.99, "matchedGoldenPath": "/x/y-golden.png",
                "mismatchPixels": 0, "cropX": crop[0], "cropY": crop[1],
                "cropWidth": crop[2], "cropHeight": crop[3]}), encoding="utf-8")
        return directory

    def test_identical_repeats_are_reported_bitwise_identical(self):
        arm = "Example-DirectVulkan"
        for index in (1, 2, 3):
            self.repeat(arm, index)
        report = ca.summarize(self.root)
        self.assertEqual([row["repeat"] for row in report["rows"]], [1, 2, 3])
        self.assertTrue(all(row["identical_to_first"] for row in report["rows"]))
        self.assertTrue(all(row["ssim_vs_first"] == 1.0 for row in report["rows"]))
        self.assertEqual(report["arms"][0]["all_bitwise_identical"], True)
        self.assertEqual(report["arms"][0]["min_ssim_vs_golden"], 1.0)

    def test_a_diverging_repeat_is_not_hidden_by_an_equal_ssim_against_the_golden(self):
        """The whole point of the column: same score, different picture.

        Both repeats can score the same SSIM against the golden while differing from each
        other, and result.json alone can never say so.
        """
        arm = "Example-DirectVulkan"
        self.repeat(arm, 1, rgba=solid(8, 4, 0), ssim=0.98)
        self.repeat(arm, 2, rgba=solid(8, 4, 255), ssim=0.98)
        report = ca.summarize(self.root)
        second = report["rows"][1]
        self.assertFalse(second["identical_to_first"])
        self.assertAlmostEqual(second["ssim_vs_first"], BLACK_VS_WHITE, places=15)
        self.assertEqual(second["mismatch_pixels_vs_first"], 32)
        self.assertFalse(report["arms"][0]["all_bitwise_identical"])
        self.assertAlmostEqual(report["arms"][0]["min_ssim_vs_first"], BLACK_VS_WHITE, places=15)

    def test_the_result_json_crop_is_the_window_used(self):
        arm = "Example-DirectVulkan"
        left, right = bytearray(), bytearray()
        for _ in range(4):
            for x in range(8):
                value = 0 if x < 4 else 255
                left += bytes([value, value, value, 255])
                right += bytes([255 - value] * 3 + [255])
        self.repeat(arm, 1, rgba=bytes(left), crop=(0, 0, 4, 4))
        self.repeat(arm, 2, rgba=bytes(right), crop=(0, 0, 4, 4))
        report = ca.summarize(self.root)
        self.assertEqual(report["rows"][1]["crop"], {"x": 0, "y": 0, "width": 4, "height": 4})
        self.assertAlmostEqual(report["rows"][1]["ssim_vs_first"], BLACK_VS_WHITE, places=15)

    def test_a_missing_actual_or_result_is_an_error_row_not_a_silent_pass(self):
        arm = "Example-DirectVulkan"
        self.repeat(arm, 1)
        self.repeat(arm, 2, write_actual=False)
        self.repeat(arm, 3, write_result=False)
        report = ca.summarize(self.root)
        self.assertEqual(report["rows"][1]["error"], "no *-actual.png")
        self.assertEqual(report["rows"][2]["error"], "no result.json")
        self.assertIsNone(report["rows"][2].get("ssim_vs_golden"))
        self.assertEqual(report["arms"][0]["errors"], 2)
        self.assertFalse(report["arms"][0]["all_bitwise_identical"])
        # The renderer must survive a row with no numbers at all.
        import io
        buffer = io.StringIO()
        ca.render_summary(report, buffer)
        self.assertIn("no *-actual.png", buffer.getvalue())

    def test_a_failed_repeat_is_carried_into_the_rollup(self):
        arm = "Example-DirectVulkan"
        self.repeat(arm, 1, ssim=1.0)
        self.repeat(arm, 2, ssim=0.97, passed=False)
        report = ca.summarize(self.root)
        self.assertFalse(report["arms"][0]["passed_all"])
        self.assertEqual(report["arms"][0]["min_ssim_vs_golden"], 0.97)

    def test_self_test_of_the_transcribed_ssim_passes(self):
        self.assertEqual(ca.self_test(), 0)


class Archive(unittest.TestCase):
    """The other half: run_android_retrace_local writes the tree summarize() reads.

    Tested together because the failure this pair exists to prevent spans both - the result
    root is keyed by case and backend and by NOTHING else, so a second repeat overwrites the
    first, and the archive is the only thing standing between "three repeats" and "one
    surviving picture". A test of either half alone would not see a key mismatch between them.
    """

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="retrace-archive-")
        self.root = Path(self.temp.name)
        self.previous_result_root = retrace.RESULT_ROOT
        retrace.RESULT_ROOT = self.root / "result-root"
        self.case = {"name": "Example/case:1", "backend_overrides": None}
        self.arm = retrace.safe_case(self.case["name"]) + "-DirectVulkan"
        self.result_dir = retrace.RESULT_ROOT / self.arm
        self.result_dir.mkdir(parents=True)

    def tearDown(self):
        retrace.RESULT_ROOT = self.previous_result_root
        self.temp.cleanup()

    def write_run(self, value, ssim):
        """Overwrite the result root the way a real repeat does."""
        write_png(self.result_dir / (self.arm + "-actual.png"), 8, 4, solid(8, 4, value))
        (self.result_dir / "result.json").write_text(json.dumps(
            {"passed": True, "statusCode": 0, "backend": "DirectVulkan", "ssim": ssim,
             "ssimThreshold": 0.99, "matchedGoldenPath": "/x/g.png", "mismatchPixels": 0,
             "cropX": 0, "cropY": 0, "cropWidth": 0, "cropHeight": 0}), encoding="utf-8")
        (self.result_dir / "mobilegl.client.log").write_text("run-ahead ARMED\n", encoding="utf-8")
        (self.result_dir / "transport-proof.json").write_text('{"passed": true}', encoding="utf-8")
        write_png(self.result_dir / (self.arm + "-golden.png"), 8, 4, solid(8, 4, 1))

    def test_each_repeat_survives_the_next_one_and_summarizes(self):
        archive = self.root / "archive"
        for index, (value, ssim) in enumerate(((10, 1.0), (10, 1.0), (200, 0.97)), start=1):
            self.write_run(value, ssim)
            kept = retrace.archive_repeat(archive, self.case, "DirectVulkan", index)
            self.assertTrue((kept / "result.json").is_file())
            self.assertTrue((kept / (self.arm + "-actual.png")).is_file())
            self.assertTrue((kept / "mobilegl.client.log").is_file())
            self.assertTrue((kept / "transport-proof.json").is_file())
        # The golden is copied ONCE per arm, beside the repeats rather than inside each.
        self.assertTrue((archive / self.arm / (self.arm + "-golden.png")).is_file())
        self.assertFalse((archive / self.arm / "repeat-01" / (self.arm + "-golden.png")).is_file())
        report = ca.summarize(archive)
        self.assertEqual([row["repeat"] for row in report["rows"]], [1, 2, 3])
        self.assertTrue(report["rows"][1]["identical_to_first"])
        self.assertFalse(report["rows"][2]["identical_to_first"])
        self.assertEqual(report["rows"][2]["ssim_vs_golden"], 0.97)
        self.assertLess(report["rows"][2]["ssim_vs_first"], 1.0)
        # The directory name the archive chose is the one the summary reports.
        self.assertEqual({row["arm"] for row in report["rows"]}, {self.arm})

    def test_a_repeat_that_produced_nothing_leaves_an_empty_directory_not_the_previous_one(self):
        """A failed repeat must not inherit the previous repeat's artefacts.

        This is the concrete shape tools/device_bench/p6/README.md records for benchmark.json:
        the result root is not cleared between runs, so a run that produced nothing leaves the
        LAST one's files in place. Archiving copies whatever is there, so the guarantee this
        test pins is the weaker, honest one - the archive reports what the result root held -
        and the summary must still be able to say the two repeats are not the same evidence.
        """
        archive = self.root / "archive"
        self.write_run(10, 1.0)
        retrace.archive_repeat(archive, self.case, "DirectVulkan", 1)
        (self.result_dir / "result.json").unlink()
        (self.result_dir / (self.arm + "-actual.png")).unlink()
        kept = retrace.archive_repeat(archive, self.case, "DirectVulkan", 2)
        self.assertFalse((kept / "result.json").exists())
        report = ca.summarize(archive)
        # Both artefacts are gone; the first missing one is the one named.
        self.assertEqual(report["rows"][1]["error"], "no result.json")
        self.assertNotIn("ssim_vs_first", report["rows"][1])
        self.assertNotIn("actual_sha256", report["rows"][1])
        self.assertEqual(report["arms"][0]["errors"], 1)
        self.assertFalse(report["arms"][0]["all_bitwise_identical"])


class RepeatIsolation(unittest.TestCase):
    """The repeat loop itself: a repeat that rendered nothing must archive nothing.

    The Archive class above pins what archive_repeat copies. This pins what main() leaves in the
    result root for it to copy: trace-replay-ci.sh writes result.json and the actual PNG only
    when the app produced them, so without a clear between repeats the second repeat of a case
    whose app died (bsl-esc-menu's scudo abort) archives the FIRST repeat's picture, and the
    summary calls it a passing, bit-identical repeat - the false green gate 3 reads.
    """

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="retrace-repeat-")
        self.root = Path(self.temp.name)
        apk = self.root / "pinned.apk"
        apk.write_bytes(b"not really an apk")
        self.patches = [
            mock.patch.object(retrace, "RESULT_ROOT", self.root / "result-root"),
            mock.patch.object(retrace, "render_summary", lambda: None),
            mock.patch.dict(os.environ, {"MOBILEGL_TRACE_APK": str(apk)}),
        ]
        for patch in self.patches:
            patch.start()

    def tearDown(self):
        for patch in reversed(self.patches):
            patch.stop()
        self.temp.cleanup()

    def test_a_repeat_whose_app_died_archives_no_stale_picture(self):
        calls = []

        def fake_run_case(case, backend, extra_args=None, timeout_seconds=None,
                          env_overrides=None, use_pbuffer=False):
            calls.append(list(extra_args or []))
            arm = retrace.RESULT_ROOT / (retrace.safe_case(case["name"]) + "-" + backend)
            arm.mkdir(parents=True, exist_ok=True)
            # trace-replay-ci.sh refreshes the logs on every run, success or not ...
            (arm / "logcat.txt").write_text("run %d\n" % len(calls), encoding="utf-8")
            if len(calls) == 1:
                # ... but only a run whose app lived writes result.json and the actual.
                write_png(arm / (arm.name + "-actual.png"), 8, 4, solid(8, 4, 10))
                (arm / "result.json").write_text(json.dumps(
                    {"passed": True, "statusCode": 0, "backend": backend, "ssim": 1.0,
                     "ssimThreshold": 0.99, "matchedGoldenPath": "/x/g.png",
                     "mismatchPixels": 0, "cropX": 0, "cropY": 0, "cropWidth": 0,
                     "cropHeight": 0}), encoding="utf-8")
                return 0
            return 1

        archive = self.root / "archive"
        argv = ["run_android_retrace_local.py", "--case", "OpenRA", "--backend", "DirectVulkan",
                "--repeat", "2", "--archive-dir", str(archive)]
        with mock.patch.object(retrace, "run_case", fake_run_case), \
                mock.patch.object(sys, "argv", argv):
            self.assertEqual(retrace.main(), 1)
        self.assertEqual(len(calls), 2)
        arm = archive / "OpenRA-DirectVulkan"
        self.assertTrue((arm / "repeat-01" / "result.json").is_file())
        self.assertTrue((arm / "repeat-01" / "OpenRA-DirectVulkan-actual.png").is_file())
        self.assertFalse((arm / "repeat-02" / "result.json").exists())
        self.assertFalse((arm / "repeat-02" / "OpenRA-DirectVulkan-actual.png").exists())
        report = ca.summarize(archive)
        self.assertEqual(report["rows"][1]["error"], "no result.json")
        self.assertFalse(report["arms"][0]["all_bitwise_identical"])
        self.assertFalse(report["arms"][0]["passed_all"])


@unittest.skipIf(os.name == "nt", "the Git Bash spelling is the Windows branch of the same code")
class PosixHost(unittest.TestCase):
    """The device window runs detached from WSL, so the runner has to start on a POSIX host.

    It used to hardcode Git Bash's `C:/Program Files/Git/bin/bash.exe` and spell every host path
    /c/...: on Linux every case died with FileNotFoundError before its first adb call.
    """

    def test_run_case_starts_a_posix_bash_with_native_paths(self):
        with tempfile.TemporaryDirectory(prefix="retrace-posix-") as directory:
            root = Path(directory)
            case = retrace.case_with_defaults(trace_cases.find_trace_case("OpenRA"))
            fixtures = root / "fixtures"
            fixtures.mkdir()
            (fixtures / case["trace_archive"]).write_bytes(b"a real archive, not an LFS pointer")
            write_png(fixtures / case["golden"], 8, 4, solid(8, 4, 0))
            apk = root / "pinned.apk"
            apk.write_bytes(b"apk")
            captured = {}

            def fake_run(command, cwd=None, env=None, **_):
                captured.update(command=command, env=env)
                return subprocess.CompletedProcess(command, 0)

            with mock.patch.object(retrace, "FIXTURES", fixtures), \
                    mock.patch.object(retrace, "RESULT_ROOT", root / "result-root"), \
                    mock.patch.object(retrace, "FIXTURE_ROOT", root / "fixture-root"), \
                    mock.patch.dict(os.environ, {"MOBILEGL_TRACE_APK": str(apk)}), \
                    mock.patch.object(retrace.subprocess, "run", fake_run):
                self.assertEqual(retrace.run_case(case, "DirectVulkan"), 0)
            command = captured["command"]
            self.assertNotIn("Program Files", command[0])
            self.assertTrue(Path(command[0]).is_file(), command[0])
            self.assertEqual(command[command.index("--apk-file") + 1], str(apk.resolve()))
            self.assertEqual(command[command.index("--golden") + 1],
                             str((fixtures / case["golden"]).resolve()))
            self.assertEqual(captured["env"]["PYTHON"], sys.executable)


if __name__ == "__main__":
    unittest.main(verbosity=2)
