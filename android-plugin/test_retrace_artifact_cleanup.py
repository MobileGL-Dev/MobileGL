#!/usr/bin/env python3
# MobileGL - android-plugin/test_retrace_artifact_cleanup.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Pin which intermediate APK artifacts the cleanup keeps, and how fast it deletes the rest."""

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("retrace_artifact_cleanup", HERE / "retrace_artifact_cleanup.py")
cleanup = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cleanup)

RESULT = "MobileGL-android-retrace-result-"


def status(job_id, **cases):
    return {"job_id": job_id, "cases": [{"case": name, "status": verdict} for name, verdict in cases.items()]}


def names(entries):
    return sorted(name for _, name in entries)


class PlanTest(unittest.TestCase):
    ARTIFACTS = [("1", RESULT + "mono-gles"), ("2", RESULT + "inproc-gles"), ("3", RESULT + "spawn-vk"),
                 ("4", "mobilegl-android-monolith-control.html"), ("5", "MobileGL-plugin-release-abc.apk"),
                 ("6", "MobileGL-plugin-trace-release-abc.apk")]

    def test_a_green_run_deletes_every_intermediate_and_nothing_else(self):
        statuses = {RESULT + "mono-gles": status("11", a="passed", b="passed"),
                    RESULT + "inproc-gles": status("12", a="passed", b="passed"),
                    RESULT + "spawn-vk": status("13", c="passed")}
        jobs = [("11", "success", "x"), ("12", "success", "y"), ("13", "success", "z"), ("14", "", "retrace summary")]
        delete, kept = cleanup.plan(self.ARTIFACTS, jobs, statuses)
        self.assertEqual(kept, [])
        self.assertEqual(names(delete), sorted(name for _, name in self.ARTIFACTS[:3]))

    def test_a_failed_case_keeps_every_shard_that_ran_it(self):
        statuses = {RESULT + "mono-gles": status("11", a="passed", b="passed"),
                    RESULT + "inproc-gles": status("12", a="passed", b="failed"),
                    RESULT + "spawn-vk": status("13", c="passed")}
        jobs = [("11", "success", "x"), ("12", "failure", "y"), ("13", "success", "z")]
        delete, kept = cleanup.plan(self.ARTIFACTS, jobs, statuses)
        # Only b's results are worth comparing, so the monolith shard stays for b and the
        # spawn shard goes.
        self.assertEqual(names(delete), [RESULT + "spawn-vk"])
        self.assertEqual(len(kept), 2)
        self.assertTrue(any("inproc-gles" in line and "its job failed" in line for line in kept))
        self.assertTrue(any("mono-gles" in line and "holds failed case(s): b" in line for line in kept))

    def test_cases_a_failed_job_never_reached_are_kept(self):
        statuses = {RESULT + "mono-gles": status("11", a="passed", b="not-run"),
                    RESULT + "spawn-vk": status("13", c="passed")}
        jobs = [("11", "timed_out", "x"), ("13", "success", "z")]
        delete, kept = cleanup.plan(self.ARTIFACTS[:1] + self.ARTIFACTS[2:], jobs, statuses)
        self.assertEqual(names(delete), [RESULT + "spawn-vk"])
        self.assertEqual(len(kept), 1)

    def test_a_job_that_failed_after_its_cases_passed_keeps_its_results(self):
        statuses = {RESULT + "mono-gles": status("11", a="passed", b="passed"),
                    RESULT + "spawn-vk": status("13", c="passed")}
        jobs = [("11", "failure", "upload failed"), ("13", "success", "z")]
        delete, kept = cleanup.plan(self.ARTIFACTS[:1] + self.ARTIFACTS[2:], jobs, statuses)
        self.assertEqual(names(delete), [RESULT + "spawn-vk"])
        self.assertTrue("mono-gles" in kept[0] and "its job failed" in kept[0])

    def test_a_canceled_job_keeps_nothing_by_itself(self):
        statuses = {RESULT + "mono-gles": status("11", a="passed", b="not-run"),
                    RESULT + "inproc-gles": status("12", a="failed", b="not-run")}
        jobs = [("11", "cancelled", "x"), ("12", "cancelled", "y")]
        delete, kept = cleanup.plan(self.ARTIFACTS[:2], jobs, statuses)
        # Case a failed in the inproc shard, so both shards that ran it stay.
        self.assertEqual(delete, [])
        self.assertEqual(len(kept), 2)
        only_not_run = {RESULT + "mono-gles": status("11", a="passed", b="not-run")}
        delete, kept = cleanup.plan(self.ARTIFACTS[:1], [("11", "cancelled", "x")], only_not_run)
        self.assertEqual((names(delete), kept), ([RESULT + "mono-gles"], []))

    def test_a_result_without_a_status_is_kept(self):
        statuses = {RESULT + "mono-gles": status("11", a="passed")}
        delete, kept = cleanup.plan(self.ARTIFACTS[:2], [("11", "success", "x")], statuses)
        self.assertEqual(names(delete), [RESULT + "mono-gles"])
        self.assertIn("no shard status", kept[0])

    def test_a_failed_job_without_a_status_keeps_everything(self):
        statuses = {RESULT + "mono-gles": status("11", a="passed", b="passed")}
        jobs = [("11", "success", "x"), ("12", "failure", "died before its upload")]
        delete, kept = cleanup.plan(self.ARTIFACTS, jobs, statuses)
        self.assertEqual(delete, [])
        self.assertEqual(len(kept), 3)

    def test_statuses_without_job_ids_fall_back_to_counting(self):
        artifacts = self.ARTIFACTS[:1] + self.ARTIFACTS[2:]
        explained = {RESULT + "mono-gles": status("", a="passed", b="not-run"),
                     RESULT + "spawn-vk": status("", c="passed")}
        delete, _ = cleanup.plan(artifacts, [("12", "failure", "y")], explained)
        # The one status with a case that did not pass stands for the one failed job.
        self.assertEqual(names(delete), [RESULT + "spawn-vk"])
        unexplained = {RESULT + "mono-gles": status("", a="passed", b="passed"),
                       RESULT + "spawn-vk": status("", c="passed")}
        delete, _ = cleanup.plan(artifacts, [("12", "failure", "y")], unexplained)
        self.assertEqual(delete, [])


class RunTest(unittest.TestCase):
    @staticmethod
    def passed(artifacts):
        """A shard status with one passed case for every artifact, each from a job of its own."""
        return {name: status(f"9{artifact_id}", a="passed") for artifact_id, name in artifacts}

    def run_cleanup(self, artifacts, jobs, statuses, exit_code=0, dry_run=False):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name, value in statuses.items():
                (root / "downloads" / "lane" / name).mkdir(parents=True)
                (root / "downloads" / "lane" / name / "shard-status.json").write_text(json.dumps(value))
            (root / "artifacts.tsv").write_text("".join(f"{i}\t{n}\n" for i, n in artifacts))
            (root / "jobs.tsv").write_text("".join("\t".join(job) + "\n" for job in jobs))
            # One file per call: the deletes run in parallel processes.
            (root / "deleted").mkdir()
            script = root / "delete.py"
            script.write_text("import pathlib, sys\n"
                              "(pathlib.Path(sys.argv[1]) / sys.argv[2].replace('/', '_')).write_text('')\n"
                              f"sys.exit({exit_code})\n")
            python = pathlib.Path(sys.executable).as_posix()
            command = f'"{python}" "{script.as_posix()}" "{(root / "deleted").as_posix()}" artifacts/'
            pauses = []
            output = io.StringIO()
            with mock.patch.dict(os.environ, {"MOBILEGL_CLEANUP_DELETE_COMMAND": command}), \
                    mock.patch.object(cleanup, "pause", pauses.append), contextlib.redirect_stdout(output):
                code = cleanup.main(["--downloads", str(root / "downloads" / "lane"),
                                     "--artifacts-file", str(root / "artifacts.tsv"),
                                     "--jobs-file", str(root / "jobs.tsv")] + (["--dry-run"] if dry_run else []))
            deleted = sorted(entry.name.replace("_", "/") for entry in (root / "deleted").iterdir())
            return code, output.getvalue(), deleted, pauses

    def test_deletes_are_spaced_under_the_secondary_rate_limit(self):
        artifacts = [(str(index), f"{RESULT}shard{index}") for index in range(12)]
        code, output, deleted, pauses = self.run_cleanup(artifacts, [], self.passed(artifacts))
        self.assertEqual(code, 0)
        self.assertEqual(deleted, sorted(f"artifacts/{index}" for index in range(12)))
        self.assertIn("Deleted 12 intermediate Android artifact(s)", output)
        # Twelve starts are booked at least 11 intervals apart, so the rate stays under 180 a minute.
        self.assertLess(60 / cleanup.DELETE_INTERVAL_SECONDS, 180)
        self.assertGreater(len(pauses), 0)

    def test_refused_deletes_warn_and_leave_the_run_green(self):
        artifacts = [(str(index), f"{RESULT}shard{index}") for index in range(8)]
        code, output, _, _ = self.run_cleanup(artifacts, [], self.passed(artifacts), exit_code=1)
        self.assertEqual(code, 0)
        self.assertIn("::warning::Could not delete intermediate artifact", output)
        self.assertIn("Stopped deleting", output)
        self.assertIn("Deleted 0 intermediate", output)

    def test_a_dry_run_deletes_nothing(self):
        artifacts = [("1", RESULT + "shard1")]
        code, output, deleted, _ = self.run_cleanup(artifacts, [], self.passed(artifacts), dry_run=True)
        self.assertEqual((code, deleted), (0, []))
        self.assertIn(f"Would delete {RESULT}shard1 (1)", output)


if __name__ == "__main__":
    unittest.main()
