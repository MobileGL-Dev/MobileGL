#!/usr/bin/env python3
"""Exercise the actual APK retry classifier with saved and live adb states."""
import pathlib
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "android-plugin/trace-replay-ci.sh"


class TraceInfrastructureTest(unittest.TestCase):
    def classify(self, saved="device", live="device", disconnected=False, logcat=""):
        source = SCRIPT.read_text()
        functions = source[source.index("record_infrastructure_reason() {"):source.index("copy_app_artifact() {")]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "adb-state.txt").write_text(saved)
            (root / "logcat.txt").write_text(logcat)
            if disconnected:
                (root / "adb-disconnected.txt").write_text("offline")
            shell = functions + '\nadb_device_path() { printf "%s\\n" "$LIVE"; }\n' + \
                'result_root="$1"; LIVE="$2"; is_infrastructure_failure "$1"\n'
            result = subprocess.run(["sh", "-c", shell, "classifier", directory, live], capture_output=True)
            reason = root / "infrastructure-failure-reason.txt"
            return result.returncode, reason.read_text().strip() if reason.exists() else ""

    def test_device_disappears_during_diagnostics(self):
        self.assertEqual(self.classify(live="offline"), (0, "device-unavailable"))

    def test_polling_disconnect_survives_reconnect(self):
        self.assertEqual(self.classify(disconnected=True), (0, "device-unavailable"))

    def test_saved_disconnect_survives_reconnect(self):
        self.assertEqual(self.classify(saved=""), (0, "device-unavailable"))

    def test_online_application_failure_stays_failure(self):
        self.assertEqual(self.classify(logcat="Fatal signal 11 (trace_replay)\n"), (1, ""))

    def test_system_server_crash_remains_retryable(self):
        self.assertEqual(self.classify(logcat="Fatal signal 11 (system_server)\n"), (0, "system-server-crash"))

    def lost_logs(self, live="device", log="", needs_log=1):
        """passed_replay_lost_its_logs over a result dir whose replay PASSED (the caller has
        already checked result.json) with the given mobilegl.log content and live adb state."""
        source = SCRIPT.read_text()
        functions = source[source.index("record_infrastructure_reason() {"):source.index("copy_app_artifact() {")]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "adb-state.txt").write_text("device")
            (root / "logcat.txt").write_text("")
            (root / "mobilegl.log").write_text(log)
            shell = functions + '\nadb_device_path() { printf "%s\\n" "$LIVE"; }\n' + \
                'result_root="$1"; LIVE="$2"; passed_replay_lost_its_logs "$1" "$3"\n'
            result = subprocess.run(["sh", "-c", shell, "lost-logs", directory, live, str(needs_log)],
                                    capture_output=True)
            reason = root / "infrastructure-failure-reason.txt"
            return result.returncode, reason.read_text().strip() if reason.exists() else ""

    def test_a_passed_replay_whose_device_went_offline_before_its_logs_is_retried(self):
        # APK workflow: result.json passed, then "error: device offline" before collect_role_logs,
        # and the inproc proof failed the leg with exit 1 on an empty mobilegl.log.
        self.assertEqual(self.lost_logs(live="offline"), (0, "device-unavailable"))

    def test_an_empty_log_on_a_live_device_stays_the_proofs_failure(self):
        self.assertEqual(self.lost_logs(live="device"), (1, ""))

    def test_a_log_that_arrived_is_judged_by_the_proof_not_retried(self):
        self.assertEqual(self.lost_logs(live="offline", log="Config: IPC run-ahead=1\n"), (1, ""))

    def test_a_leg_that_needs_no_proof_is_not_retried_for_its_log(self):
        self.assertEqual(self.lost_logs(live="offline", needs_log=0), (1, ""))

    def test_the_lost_log_check_runs_before_the_transport_proofs(self):
        source = SCRIPT.read_text()
        call = source.index('if passed_replay_lost_its_logs "${result_dir}"')
        self.assertLess(source.index('sys.exit(0 if result.get("passed")'), call)
        self.assertLess(call, source.index('if [ "${require_inproc}" -eq 1 ]; then\n    "${PYTHON}"'))


if __name__ == "__main__":
    unittest.main()
