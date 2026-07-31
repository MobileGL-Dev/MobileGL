import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

try:
    from . import cts_multi_report as report
except ImportError:  # Allows `python test_cts_multi_report.py`.
    import cts_multi_report as report


def qpa_case(case: str, status: str) -> str:
    return (
        f"#beginTestCaseResult {case}\n"
        f'<Result StatusCode="{status}"/>\n'
        "#endTestCaseResult\n"
    )


def write_run_state(
    caselist: Path,
    result_dir: Path,
    backend: str,
    invocation_identity=None,
) -> None:
    fingerprint, case_count = report._caselist_fingerprint(str(caselist))
    (result_dir / "run_state.json").write_text(
        json.dumps(
            {
                "version": 1,
                "backend": backend,
                "case_count": case_count,
                "caselist_sha256": fingerprint,
                "invocation_identity": invocation_identity,
            }
        ),
        encoding="utf-8",
    )


def make_inputs(
    root: Path,
    name: str,
    cases: list[str],
    qpa: str,
    backend: str = "DirectGLES",
):
    caselist = root / f"{name}.txt"
    caselist.write_text("\n".join(cases) + "\n", encoding="utf-8")
    result_dir = root / f"{name}-results"
    result_dir.mkdir()
    (result_dir / "chunk0000.qpa").write_text(qpa, encoding="utf-8")
    write_run_state(caselist, result_dir, backend)
    return caselist, result_dir


class MultiReportTests(unittest.TestCase):
    def test_backend_aggregate_is_weighted_by_expected_cases(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            small_cases, small_results = make_inputs(
                root, "small", ["small.pass"], qpa_case("small.pass", "Pass")
            )
            large_cases, large_results = make_inputs(
                root,
                "large",
                ["large.pass", "large.crash", "large.hang"],
                qpa_case("large.pass", "Pass"),
            )
            (large_results / "crashed.txt").write_text(
                "large.crash\n", encoding="utf-8"
            )
            (large_results / "hung.txt").write_text(
                "large.hang\n", encoding="utf-8"
            )

            payload = report.build_report(
                [
                    report.SuiteSpec(
                        "DirectGLES", "small", str(small_cases), str(small_results)
                    ),
                    report.SuiteSpec(
                        "DirectGLES", "large", str(large_cases), str(large_results)
                    ),
                ]
            )

            aggregate = payload["backends"]["DirectGLES"]
            self.assertEqual(4, aggregate["expected"])
            self.assertEqual(4, aggregate["result"])
            self.assertEqual(2, aggregate["pass"])
            self.assertEqual(2, aggregate["accepted"])
            self.assertEqual(1, aggregate["crash"])
            self.assertEqual(1, aggregate["hang"])
            self.assertEqual(0, aggregate["unrun"])
            # (1 accepted + 1 accepted) / (1 expected + 3 expected), not
            # the unweighted mean of 100% and 33.3%.
            self.assertEqual(0.5, aggregate["conformance_accepted_rate"])
            self.assertEqual("weighted_by_expected_cases", aggregate["aggregation"])
            self.assertEqual("OK", aggregate["validation"]["state"])

    def test_declared_unrun_is_incomplete_and_cli_returns_nonzero(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist, result_dir = make_inputs(
                root, "missing", ["case.a", "case.b"], qpa_case("case.a", "Pass")
            )
            (result_dir / "unrun.txt").write_text("case.b\n", encoding="utf-8")
            markdown_path = root / "report.md"
            json_path = root / "report.json"
            argv = [
                "--suite",
                "DirectGLES",
                "gl30",
                str(caselist),
                str(result_dir),
                "--markdown",
                str(markdown_path),
                "--json",
                str(json_path),
            ]

            with contextlib.redirect_stdout(io.StringIO()):
                returncode = report.main(argv)

            self.assertEqual(1, returncode)
            self.assertTrue(markdown_path.is_file())
            payload = json.loads(json_path.read_text(encoding="utf-8"))
            suite = payload["suites"][0]
            self.assertEqual(2, suite["expected"])
            self.assertEqual(1, suite["result"])
            self.assertEqual(1, suite["unrun"])
            self.assertEqual("INCOMPLETE", suite["validation"]["state"])
            self.assertEqual("INCOMPLETE", payload["overall"]["validation"]["state"])
            self.assertEqual(0.5, payload["overall"]["conformance_accepted_rate"])

    def test_dual_backend_cli_outputs_markdown_and_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist = root / "gl30.txt"
            caselist.write_text("gl30.case\n", encoding="utf-8")
            gles = root / "gles"
            vulkan = root / "vulkan"
            gles.mkdir()
            vulkan.mkdir()
            (gles / "run.qpa").write_text(
                qpa_case("gl30.case", "Pass"), encoding="utf-8"
            )
            (vulkan / "run.qpa").write_text(
                qpa_case("gl30.case", "Fail"), encoding="utf-8"
            )
            write_run_state(caselist, gles, "DirectGLES")
            write_run_state(caselist, vulkan, "DirectVulkan")
            markdown_path = root / "dual.md"
            json_path = root / "dual.json"
            argv = [
                f"--suite=DirectGLES,gl30,{caselist},{gles}",
                "--suite",
                "DirectVulkan",
                "gl30",
                str(caselist),
                str(vulkan),
                "--markdown",
                str(markdown_path),
                "--json",
                str(json_path),
            ]

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                returncode = report.main(argv)

            self.assertEqual(0, returncode)
            payload = json.loads(json_path.read_text(encoding="utf-8"))
            self.assertEqual({"DirectGLES", "DirectVulkan"}, set(payload["backends"]))
            self.assertEqual(1, payload["backends"]["DirectGLES"]["accepted"])
            self.assertEqual(0, payload["backends"]["DirectVulkan"]["accepted"])
            self.assertEqual(2, payload["overall"]["expected"])
            self.assertEqual(1, payload["overall"]["accepted"])
            self.assertEqual(0.5, payload["overall"]["conformance_accepted_rate"])
            markdown = markdown_path.read_text(encoding="utf-8")
            self.assertIn("DirectGLES weighted subtotal", markdown)
            self.assertIn("DirectVulkan weighted subtotal", markdown)
            self.assertIn("Overall weighted", markdown)
            self.assertIn("Markdown:", stdout.getvalue())

    def test_duplicate_qpa_result_uses_last_observation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist, result_dir = make_inputs(
                root,
                "duplicate",
                ["case.a"],
                qpa_case("case.a", "Fail"),
                backend="DirectVulkan",
            )
            (result_dir / "chunk0001.qpa").write_text(
                qpa_case("case.a", "Pass"), encoding="utf-8"
            )

            payload = report.build_report(
                [
                    report.SuiteSpec(
                        "DirectVulkan", "gl33", str(caselist), str(result_dir)
                    )
                ]
            )

            suite = payload["suites"][0]
            self.assertEqual("Pass", suite["cases"]["results"]["case.a"])
            self.assertEqual(1, suite["duplicate"])
            self.assertEqual(1, payload["overall"]["duplicate"])
            self.assertEqual(1.0, payload["overall"]["strict_pass_rate"])
            self.assertEqual("OK", suite["validation"]["state"])
            self.assertIn("last result wins", suite["validation"]["warnings"][0])

    def test_backend_provenance_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist, result_dir = make_inputs(
                root,
                "provenance",
                ["case.a"],
                qpa_case("case.a", "Pass"),
                backend="DirectVulkan",
            )
            with self.assertRaises(report.MultiReportInputError):
                report.build_report(
                    [report.SuiteSpec("DirectGLES", "gl30", str(caselist), str(result_dir))]
                )

    def test_missing_provenance_requires_explicit_legacy_opt_in(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist, result_dir = make_inputs(
                root, "legacy", ["case.a"], qpa_case("case.a", "Pass")
            )
            (result_dir / "run_state.json").unlink()
            spec = report.SuiteSpec("DirectGLES", "gl30", str(caselist), str(result_dir))
            with self.assertRaises(report.MultiReportInputError):
                report.build_report([spec])
            payload = report.build_report([spec], require_run_state=False)
            self.assertEqual("UNVERIFIED", payload["suites"][0]["provenance"]["state"])

    def test_expected_run_identity_accepts_match_and_rejects_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist, result_dir = make_inputs(
                root, "identity", ["case.a"], qpa_case("case.a", "Pass")
            )
            write_run_state(
                caselist, result_dir, "DirectGLES", invocation_identity="identity-a"
            )
            spec = report.SuiteSpec(
                "DirectGLES", "gl30", str(caselist), str(result_dir)
            )

            payload = report.build_report(
                [spec], expected_run_identity="identity-a"
            )
            self.assertEqual(
                "identity-a",
                payload["suites"][0]["provenance"]["invocation_identity"],
            )
            with self.assertRaises(report.MultiReportInputError):
                report.build_report([spec], expected_run_identity="identity-b")

    def test_expected_identity_rejects_legacy_state_and_missing_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist, result_dir = make_inputs(
                root, "legacy-identity", ["case.a"], qpa_case("case.a", "Pass")
            )
            spec = report.SuiteSpec(
                "DirectGLES", "gl30", str(caselist), str(result_dir)
            )
            with self.assertRaises(report.MultiReportInputError):
                report.build_report([spec], expected_run_identity="identity-a")

            (result_dir / "run_state.json").unlink()
            with self.assertRaises(report.MultiReportInputError):
                report.build_report(
                    [spec],
                    require_run_state=False,
                    expected_run_identity="identity-a",
                )

    def test_duplicate_physical_result_directory_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist, result_dir = make_inputs(
                root, "duplicate-dir", ["case.a"], qpa_case("case.a", "Pass")
            )
            with self.assertRaises(report.MultiReportInputError):
                report.build_report(
                    [
                        report.SuiteSpec(
                            "DirectGLES", "gl30", str(caselist), str(result_dir)
                        ),
                        report.SuiteSpec(
                            "DirectGLES",
                            "gl31",
                            str(caselist),
                            str(result_dir / "."),
                        ),
                    ]
                )

    def test_ancestor_and_descendant_result_directories_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist = root / "cases.txt"
            caselist.write_text("case.a\n", encoding="utf-8")
            parent = root / "results"
            child = parent / "nested"
            child.mkdir(parents=True)
            (parent / "chunk0000.qpa").write_text(
                qpa_case("case.a", "Pass"), encoding="utf-8"
            )
            (child / "chunk0000.qpa").write_text(
                qpa_case("case.a", "Fail"), encoding="utf-8"
            )
            write_run_state(caselist, parent, "DirectGLES")
            write_run_state(caselist, child, "DirectVulkan")

            with self.assertRaises(report.MultiReportInputError):
                report.build_report(
                    [
                        report.SuiteSpec(
                            "DirectGLES", "gl30", str(caselist), str(parent)
                        ),
                        report.SuiteSpec(
                            "DirectVulkan", "gl30", str(caselist), str(child)
                        ),
                    ]
                )


if __name__ == "__main__":
    unittest.main()
