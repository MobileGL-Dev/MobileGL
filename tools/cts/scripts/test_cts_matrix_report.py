import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

try:
    from . import cts_matrix_report as report
except ImportError:  # Allows `python test_cts_matrix_report.py`.
    import cts_matrix_report as report


def qpa_case(case, status):
    return (
        f"#beginTestCaseResult {case}\n"
        f'<Result StatusCode="{status}"/>\n'
        "#endTestCaseResult\n"
    )


class MatrixReportTests(unittest.TestCase):
    def test_utf8_bom_caselist_matches_runner_semantics(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            caselist = root / "cases.txt"
            caselist.write_bytes(b"\xef\xbb\xbfcase.a\n")
            results = root / "results"
            results.mkdir()
            (results / "run.qpa").write_text(
                qpa_case("case.a", "Pass"), encoding="utf-8"
            )

            item = report.build_version_report("gl30", str(caselist), [str(results)])

            self.assertEqual(1, item["expected"])
            self.assertEqual({"case.a": "Pass"}, item["cases"]["results"])
            self.assertEqual("OK", item["validation"]["state"])

    def test_incomplete_qpa_is_unrun_not_a_completed_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            caselist = root / "cases.txt"
            caselist.write_text("case.a\n", encoding="utf-8")
            results = root / "results"
            results.mkdir()
            (results / "run.qpa").write_text(
                "#beginTestCaseResult case.a\n#endTestCaseResult\n",
                encoding="utf-8",
            )
            (results / "unrun.txt").write_text("case.a\n", encoding="utf-8")

            item = report.build_version_report("gl46", str(caselist), [str(results)])

            self.assertEqual(0, item["result"])
            self.assertEqual(1, item["unrun"])
            self.assertEqual(["case.a"], item["cases"]["incomplete_results"])
            self.assertEqual("INCOMPLETE", item["validation"]["state"])

    def test_incomplete_qpa_is_upgraded_by_crash_sidecar(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            caselist = root / "cases.txt"
            caselist.write_text("case.a\n", encoding="utf-8")
            results = root / "results"
            results.mkdir()
            (results / "run.qpa").write_text(
                "#beginTestCaseResult case.a\n", encoding="utf-8"
            )
            (results / "crashed.txt").write_text("case.a\n", encoding="utf-8")

            item = report.build_version_report("gl46", str(caselist), [str(results)])

            self.assertEqual("Crash", item["cases"]["results"]["case.a"])
            self.assertEqual([], item["cases"]["incomplete_results"])
            self.assertEqual("OK", item["validation"]["state"])

    def test_chunk_numbers_above_four_digits_use_numeric_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            caselist = root / "cases.txt"
            caselist.write_text("case.a\n", encoding="utf-8")
            results = root / "results"
            results.mkdir()
            (results / "alpha.qpa").write_text("# no results\n", encoding="utf-8")
            (results / "chunk9999.qpa").write_text(
                qpa_case("case.a", "Fail"), encoding="utf-8"
            )
            (results / "chunk10000.qpa").write_text(
                qpa_case("case.a", "Pass"), encoding="utf-8"
            )
            (results / "zeta.qpa").write_text("# no results\n", encoding="utf-8")

            item = report.build_version_report(
                "gl46", str(caselist), [str(results)]
            )

            self.assertEqual(
                ["alpha.qpa", "chunk9999.qpa", "chunk10000.qpa", "zeta.qpa"],
                [Path(path).name for path in item["inputs"]["qpa_files"]],
            )
            self.assertEqual("Pass", item["cases"]["results"]["case.a"])
            self.assertEqual(1, item["duplicate"])

    def test_qpa_sidecars_duplicates_and_expected_denominator(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            caselist = root / "gl30.txt"
            caselist.write_text("\n".join("abcdefg") + "\n", encoding="utf-8")
            results = root / "results"
            results.mkdir()
            (results / "chunk0000.qpa").write_text(
                qpa_case("a", "Fail") + "#beginTestCaseResult e\n",
                encoding="utf-8",
            )
            (results / "chunk0001.qpa").write_text(
                qpa_case("a", "Pass")
                + qpa_case("b", "NotSupported")
                + qpa_case("c", "QualityWarning")
                + qpa_case("d", "Fail"),
                encoding="utf-8",
            )
            (results / "crashed.txt").write_text("e\n", encoding="utf-8")
            (results / "hung.txt").write_text("f\n", encoding="utf-8")
            (results / "unrun.txt").write_text("g\n", encoding="utf-8")

            item = report.build_version_report(
                "gl30", str(caselist), [str(results)]
            )

            self.assertEqual(item["expected"], 7)
            self.assertEqual(item["result"], 6)
            self.assertEqual(item["pass"], 1)
            self.assertEqual(item["accepted"], 3)
            self.assertEqual(item["crash"], 1)
            self.assertEqual(item["hang"], 1)
            self.assertEqual(item["unrun"], 1)
            self.assertEqual(item["duplicate"], 1)
            self.assertEqual(item["cases"]["results"]["a"], "Pass")
            self.assertEqual(item["cases"]["results"]["e"], "Crash")
            self.assertEqual(item["cases"]["results"]["f"], "DeviceHang")
            self.assertAlmostEqual(item["strict_pass_rate"], 1 / 7)
            self.assertAlmostEqual(item["conformance_accepted_rate"], 3 / 7)
            self.assertAlmostEqual(
                item["rates"]["measured_only_conformance_accepted"], 3 / 6
            )
            self.assertEqual(item["validation"]["state"], "INCOMPLETE")
            self.assertEqual(item["validation"]["errors"], [])
            self.assertTrue(
                item["validation"]["invariant_expected_equals_result_plus_unrun"]
            )

    def test_missing_result_is_inferred_and_rejected_when_not_declared(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            caselist = root / "cases.txt"
            caselist.write_text("a\nb\n", encoding="utf-8")
            results = root / "results"
            results.mkdir()
            (results / "run.qpa").write_text(qpa_case("a", "Pass"), encoding="utf-8")

            item = report.build_version_report(
                "gl31", str(caselist), [str(results)]
            )

            self.assertEqual(item["unrun"], 1)
            self.assertEqual(item["cases"]["unrun"], ["b"])
            self.assertEqual(item["validation"]["state"], "ERROR")
            self.assertEqual(item["validation"]["undeclared_unrun"], ["b"])
            self.assertIn("not declared", item["validation"]["errors"][0])
            self.assertEqual(item["strict_pass_rate"], 0.5)

    def test_cli_emits_markdown_json_and_weighted_overall(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            statuses = {
                "gl30": "Pass",
                "gl31": "Fail",
                "gl32": "NotSupported",
                "gl33": None,
            }
            argv = []
            for version, status in statuses.items():
                caselist = root / f"{version}.txt"
                caselist.write_text(f"{version}.case\n", encoding="utf-8")
                result_dir = root / f"{version}-results"
                result_dir.mkdir()
                qpa = result_dir / "run.qpa"
                qpa.write_text(
                    qpa_case(f"{version}.case", status) if status else "# empty run\n",
                    encoding="utf-8",
                )
                if status is None:
                    (result_dir / "unrun.txt").write_text(
                        f"{version}.case\n", encoding="utf-8"
                    )
                argv.extend(
                    [
                        f"--{version}-caselist",
                        str(caselist),
                        f"--{version}-results",
                        str(result_dir),
                    ]
                )
            json_path = root / "matrix.json"
            argv.extend(["--json", str(json_path)])

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                rc = report.main(argv)

            self.assertEqual(rc, 1)  # GL33 is explicitly incomplete.
            markdown = stdout.getvalue()
            self.assertIn("| Suite | Expected | Result", markdown)
            self.assertIn("| **Overall (weighted)**", markdown)
            payload = json.loads(json_path.read_text(encoding="utf-8"))
            overall = payload["overall"]
            self.assertEqual(overall["expected"], 4)
            self.assertEqual(overall["result"], 3)
            self.assertEqual(overall["pass"], 1)
            self.assertEqual(overall["accepted"], 2)
            self.assertEqual(overall["unrun"], 1)
            self.assertEqual(overall["strict_pass_rate"], 0.25)
            self.assertEqual(overall["conformance_accepted_rate"], 0.5)
            self.assertEqual(overall["aggregation"], "weighted_by_expected_cases")
            self.assertEqual(overall["validation"]["state"], "INCOMPLETE")

    def test_duplicate_caselist_and_unexpected_result_are_validation_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            caselist = root / "cases.txt"
            caselist.write_text("a\na\n", encoding="utf-8")
            results = root / "results"
            results.mkdir()
            (results / "run.qpa").write_text(
                qpa_case("a", "Pass") + qpa_case("outside", "Pass"),
                encoding="utf-8",
            )

            item = report.build_version_report(
                "gl32", str(caselist), [str(results)]
            )

            self.assertEqual(item["cases"]["duplicate_caselist_entries"], {"a": 2})
            self.assertEqual(item["cases"]["unexpected_results"], {"outside": "Pass"})
            self.assertEqual(item["validation"]["state"], "ERROR")
            self.assertEqual(len(item["validation"]["errors"]), 2)


if __name__ == "__main__":
    unittest.main()
