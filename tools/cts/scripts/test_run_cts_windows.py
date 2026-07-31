import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import run_cts_windows as runner


def qpa_closed(case: str, status: str = "Pass") -> str:
    return (
        f"#beginTestCaseResult {case}\n"
        f'<Result StatusCode="{status}">ok</Result>\n'
        "#endTestCaseResult\n"
    )


def command_path(command, option):
    prefix = option + "="
    return Path(next(value[len(prefix) :] for value in command if value.startswith(prefix)))


class AtomicWriteTests(unittest.TestCase):
    def test_access_denied_retries_then_replace_succeeds(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "remaining.txt"
            real_replace = runner.os.replace
            attempts = 0

            def flaky_replace(source, destination):
                nonlocal attempts
                attempts += 1
                if attempts == 1:
                    raise PermissionError(13, "temporarily denied", str(destination))
                if attempts == 2:
                    error = OSError("temporary WinError 5")
                    error.winerror = 5
                    raise error
                real_replace(source, destination)

            with mock.patch.object(runner.os, "replace", side_effect=flaky_replace), mock.patch.object(
                runner.time, "sleep"
            ) as sleep:
                runner.atomic_write_text(target, "case.a\n")

            self.assertEqual(3, attempts)
            self.assertEqual("case.a\n", target.read_text(encoding="utf-8"))
            self.assertEqual(2, sleep.call_count)
            self.assertEqual(
                [
                    mock.call(runner.ATOMIC_REPLACE_INITIAL_BACKOFF_SECONDS),
                    mock.call(runner.ATOMIC_REPLACE_INITIAL_BACKOFF_SECONDS * 2),
                ],
                sleep.call_args_list,
            )
            self.assertEqual([], list(target.parent.glob(".remaining.txt.*.tmp")))

    def test_permanent_access_denied_stops_after_bounded_attempts(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "remaining.txt"

            def always_denied(_source, _destination):
                error = OSError("persistent WinError 5")
                error.winerror = 5
                raise error

            with mock.patch.object(
                runner.os, "replace", side_effect=always_denied
            ) as replace, mock.patch.object(runner.time, "sleep") as sleep:
                with self.assertRaises(OSError) as raised:
                    runner.atomic_write_text(target, "case.a\n")

            self.assertEqual(5, raised.exception.winerror)
            self.assertEqual(runner.ATOMIC_REPLACE_ATTEMPTS, replace.call_count)
            self.assertEqual(runner.ATOMIC_REPLACE_ATTEMPTS - 1, sleep.call_count)
            self.assertFalse(target.exists())
            self.assertEqual([], list(target.parent.glob(".remaining.txt.*.tmp")))

    def test_non_access_error_is_not_retried(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "remaining.txt"
            error = OSError(28, "disk full")
            with mock.patch.object(
                runner.os, "replace", side_effect=error
            ) as replace, mock.patch.object(runner.time, "sleep") as sleep:
                with self.assertRaises(OSError):
                    runner.atomic_write_text(target, "case.a\n")

            self.assertEqual(1, replace.call_count)
            sleep.assert_not_called()


class QpaParsingTests(unittest.TestCase):
    def test_terminate_is_a_completed_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "chunk0000.qpa"
            path.write_text(
                "#beginTestCaseResult KHR-GL30.a\n"
                "#terminateTestCaseResult Crash\n"
                "#beginTestCaseResult KHR-GL30.b\n",
                encoding="utf-8",
            )
            progress = runner.scan_qpa(path)
            self.assertEqual(["KHR-GL30.a"], progress.recorded)
            self.assertEqual("KHR-GL30.b", progress.in_flight)
            self.assertEqual(2, progress.begin_count)

    def test_end_without_result_is_not_accounted(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "chunk0000.qpa"
            path.write_text(
                "#beginTestCaseResult KHR-GL46.incomplete\n"
                "#endTestCaseResult\n"
                + qpa_closed("KHR-GL46.complete"),
                encoding="utf-8",
            )
            progress = runner.scan_qpa(path)
            self.assertEqual(["KHR-GL46.complete"], progress.recorded)
            self.assertIsNone(progress.in_flight)

    def test_result_written_before_truncated_eof_is_recovered(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "chunk0000.qpa"
            path.write_text(
                "#beginTestCaseResult KHR-GL46.complete\n"
                '<Result StatusCode="Pass">ok</Result>\n',
                encoding="utf-8",
            )
            progress = runner.scan_qpa(path)
            self.assertEqual(["KHR-GL46.complete"], progress.recorded)
            self.assertIsNone(progress.in_flight)


class RunIdentityTests(unittest.TestCase):
    def test_non_object_run_state_is_a_controlled_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            outdir = Path(temporary)
            (outdir / "run_state.json").write_text("null\n", encoding="utf-8")
            with self.assertRaises(runner.RunnerError):
                runner.check_run_identity(outdir, "DirectVulkan", ["case.a"])

    def test_controller_identity_prevents_mixed_invocations(self):
        with tempfile.TemporaryDirectory() as temporary:
            outdir = Path(temporary)
            runner.check_run_identity(
                outdir, "DirectVulkan", ["case.a"], invocation_identity="identity-a"
            )
            runner.check_run_identity(
                outdir, "DirectVulkan", ["case.a"], invocation_identity="identity-a"
            )
            with self.assertRaises(runner.RunnerError):
                runner.check_run_identity(
                    outdir, "DirectVulkan", ["case.a"], invocation_identity="identity-b"
                )

    def test_controller_identity_cannot_be_downgraded_by_omission(self):
        with tempfile.TemporaryDirectory() as temporary:
            outdir = Path(temporary)
            runner.check_run_identity(
                outdir, "DirectVulkan", ["case.a"], invocation_identity="identity-a"
            )
            with self.assertRaises(runner.RunnerError):
                runner.check_run_identity(outdir, "DirectVulkan", ["case.a"])

    def test_legacy_artifacts_require_explicit_adoption(self):
        with tempfile.TemporaryDirectory() as temporary:
            outdir = Path(temporary)
            (outdir / "chunk0000.qpa").write_text(
                qpa_closed("case.a"), encoding="utf-8"
            )
            with self.assertRaises(runner.RunnerError):
                runner.check_run_identity(
                    outdir, "DirectVulkan", ["case.a"], invocation_identity="identity-a"
                )
            self.assertFalse((outdir / "run_state.json").exists())

            runner.check_run_identity(
                outdir,
                "DirectVulkan",
                ["case.a"],
                invocation_identity="identity-a",
                adopt_legacy=True,
            )
            state = runner.json.loads(
                (outdir / "run_state.json").read_text(encoding="utf-8")
            )
            self.assertTrue(state["adopted_legacy"])

    def test_foreign_nested_qpa_and_skipped_sidecar_are_legacy_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            outdir = Path(temporary)
            nested = outdir / "old"
            nested.mkdir()
            qpa = nested / "legacy.qpa"
            qpa.write_text(qpa_closed("case.a"), encoding="utf-8")
            skipped = outdir / "skipped.txt"
            skipped.write_text("case.b\n", encoding="utf-8")

            self.assertEqual(
                {qpa, skipped}, set(runner.recovery_artifacts(outdir))
            )
            with self.assertRaises(runner.RunnerError):
                runner.check_run_identity(
                    outdir, "DirectVulkan", ["case.a", "case.b"]
                )
            recorded, crashed, hung = runner.recover_results(
                outdir, {"case.a", "case.b"}
            )
            self.assertEqual({"case.a"}, recorded)
            self.assertEqual(set(), crashed)
            self.assertEqual(set(), hung)

    def test_non_object_or_non_string_meta_classification_is_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            outdir = Path(temporary)
            (outdir / "chunk0000.meta.json").write_text("null\n", encoding="utf-8")
            (outdir / "chunk0001.meta.json").write_text("[]\n", encoding="utf-8")
            (outdir / "chunk0002.meta.json").write_text(
                '{"classified_case": [], "classification": "Crash"}\n', encoding="utf-8"
            )
            self.assertEqual(
                (set(), set()), runner.load_meta_classifications(outdir, {"case.a"})
            )


class RunnerRecoveryTests(unittest.TestCase):
    def run_args(self, root: Path, caselist: Path, outdir: Path, *extra: str):
        return [
            "--exe",
            sys.executable,
            "--workdir",
            str(root),
            "--caselist",
            str(caselist),
            "--outdir",
            str(outdir),
            "--backend",
            "DirectVulkan",
            *extra,
        ]

    def test_crash_tail_is_quarantined_and_next_chunk_resumes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist = root / "cases.txt"
            outdir = root / "results"
            caselist.write_text("KHR-GL30.a\nKHR-GL30.b\nKHR-GL30.c\n", encoding="utf-8")
            seen_remaining = []

            def fake_run(command, workdir, environment, qpa_path, stdout_path, stderr_path, *timeouts):
                del workdir, timeouts
                seen_remaining.append(
                    command_path(command, "--deqp-caselist-file")
                    .read_text(encoding="utf-8")
                    .splitlines()
                )
                stdout_path.write_text("fake stdout\n", encoding="utf-8")
                stderr_path.write_text("fake stderr\n", encoding="utf-8")
                self.assertEqual("DirectVulkan", environment["MOBILEGL_BACKEND_TYPE"])
                if len(seen_remaining) == 1:
                    qpa_path.write_text(
                        qpa_closed("KHR-GL30.a") + "#beginTestCaseResult KHR-GL30.b\n",
                        encoding="utf-8",
                    )
                    return runner.ProcessOutcome(0xC0000005, 0.1)
                qpa_path.write_text(qpa_closed("KHR-GL30.c"), encoding="utf-8")
                return runner.ProcessOutcome(0, 0.1)

            with mock.patch.object(runner, "run_process", side_effect=fake_run):
                result = runner.main(self.run_args(root, caselist, outdir))

            self.assertEqual(0, result)
            self.assertEqual(
                [
                    ["KHR-GL30.a", "KHR-GL30.b", "KHR-GL30.c"],
                    ["KHR-GL30.c"],
                ],
                seen_remaining,
            )
            self.assertEqual("KHR-GL30.b\n", (outdir / "crashed.txt").read_text(encoding="utf-8"))
            self.assertEqual("", (outdir / "hung.txt").read_text(encoding="utf-8"))
            self.assertEqual("", (outdir / "unrun.txt").read_text(encoding="utf-8"))

    def test_existing_qpa_and_sidecar_are_recovered(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist = root / "cases.txt"
            outdir = root / "results"
            outdir.mkdir()
            caselist.write_text("KHR-GL31.a\nKHR-GL31.b\nKHR-GL31.c\n", encoding="utf-8")
            (outdir / "chunk0000.qpa").write_text(qpa_closed("KHR-GL31.a"), encoding="utf-8")
            (outdir / "crashed.txt").write_text("KHR-GL31.b\n", encoding="utf-8")
            seen_remaining = []

            def fake_run(command, workdir, environment, qpa_path, stdout_path, stderr_path, *timeouts):
                del workdir, environment, stdout_path, stderr_path, timeouts
                seen_remaining.extend(
                    command_path(command, "--deqp-caselist-file")
                    .read_text(encoding="utf-8")
                    .splitlines()
                )
                qpa_path.write_text(qpa_closed("KHR-GL31.c"), encoding="utf-8")
                return runner.ProcessOutcome(0, 0.1)

            with mock.patch.object(runner, "run_process", side_effect=fake_run):
                result = runner.main(
                    self.run_args(root, caselist, outdir, "--adopt-legacy")
                )

            self.assertEqual(0, result)
            self.assertEqual(["KHR-GL31.c"], seen_remaining)
            self.assertTrue((outdir / "chunk0001.qpa").is_file())

    def test_repeated_no_output_aborts_without_false_case_blame(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            caselist = root / "cases.txt"
            outdir = root / "results"
            caselist.write_text("KHR-GL32.a\nKHR-GL32.b\n", encoding="utf-8")
            seen_remaining = []

            def fake_run(command, workdir, environment, qpa_path, stdout_path, stderr_path, *timeouts):
                del workdir, environment, stdout_path, stderr_path, timeouts
                seen_remaining.append(
                    command_path(command, "--deqp-caselist-file")
                    .read_text(encoding="utf-8")
                    .splitlines()
                )
                qpa_path.write_text("#sessionInfo releaseName fake\n", encoding="utf-8")
                return runner.ProcessOutcome(
                    1, 0.1, timed_out=True, timeout_reason="qpa-idle"
                )

            with mock.patch.object(runner, "run_process", side_effect=fake_run):
                result = runner.main(
                    self.run_args(root, caselist, outdir, "--max-empty-streak", "2")
                )

            self.assertEqual(4, result)
            self.assertEqual(
                [["KHR-GL32.a", "KHR-GL32.b"], ["KHR-GL32.a", "KHR-GL32.b"]],
                seen_remaining,
            )
            self.assertEqual("", (outdir / "crashed.txt").read_text(encoding="utf-8"))
            self.assertEqual("", (outdir / "hung.txt").read_text(encoding="utf-8"))
            self.assertEqual(
                "KHR-GL32.a\nKHR-GL32.b\n",
                (outdir / "unrun.txt").read_text(encoding="utf-8"),
            )


class ProcessTimeoutTests(unittest.TestCase):
    def test_qpa_activity_prevents_idle_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            qpa = root / "active.qpa"
            helper = (
                "import pathlib,sys,time\n"
                "path=pathlib.Path(sys.argv[1])\n"
                "for size in range(1, 9):\n"
                " path.write_text('x' * size, encoding='utf-8')\n"
                " time.sleep(0.08)\n"
            )
            outcome = runner.run_process(
                [sys.executable, "-c", helper, str(qpa)],
                root,
                dict(runner.os.environ),
                qpa,
                root / "stdout.log",
                root / "stderr.log",
                idle_timeout=0.2,
                max_round_seconds=0,
                poll_seconds=0.03,
            )
            self.assertFalse(outcome.timed_out)
            self.assertEqual(0, outcome.returncode)

    def test_idle_timeout_really_stops_process(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            started = time.monotonic()
            outcome = runner.run_process(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                root,
                dict(runner.os.environ),
                root / "never-created.qpa",
                root / "stdout.log",
                root / "stderr.log",
                idle_timeout=0.2,
                max_round_seconds=0,
                poll_seconds=0.05,
            )
            elapsed = time.monotonic() - started
            self.assertTrue(outcome.timed_out)
            self.assertEqual("qpa-idle", outcome.timeout_reason)
            self.assertLess(elapsed, 10)


if __name__ == "__main__":
    unittest.main()
