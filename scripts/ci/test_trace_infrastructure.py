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


if __name__ == "__main__":
    unittest.main()
