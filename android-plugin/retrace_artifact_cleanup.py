#!/usr/bin/env python3
# MobileGL - android-plugin/retrace_artifact_cleanup.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Delete the intermediate artifacts of an APK workflow run, keeping what a failure needs.

The retrace summary embeds every image it shows, so once it is uploaded the per-shard result
artifacts are only worth keeping for cases that did not pass:

- a result artifact stays when its shard-status.json is missing, when its job failed, or when
  it holds a case that did not pass in any shard (so both backends and all transports of a
  failed case stay comparable);
- every result artifact stays when a job failed without leaving a shard-status.json to say
  which cases it was running.

A canceled job retains nothing by itself; a failed verdict inside it still does. The trace
fixtures are not artifacts: the retrace jobs restore them from a cache entry.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import threading
import time
from pathlib import Path

RESULT_PREFIXES = ("MobileGL-android-retrace-result-", "retrace-result-")
FAILED_CONCLUSIONS = ("failure", "timed_out", "action_required")
# GitHub's secondary rate limit allows about 180 mutating REST calls a minute. Requests start
# at most this often, whatever the number of workers.
DELETE_INTERVAL_SECONDS = 60.0 / 170.0
DELETE_WORKERS = 3
MAX_REFUSED_DELETES = 5


def pause(seconds):
    time.sleep(seconds)


def read_rows(text, columns):
    rows = []
    for line in text.splitlines():
        if line.strip():
            fields = line.split("\t")
            rows.append(fields + [""] * (columns - len(fields)))
    return rows


def shard_statuses(download_roots):
    """Artifact name -> its shard-status.json, for every downloaded result artifact that has one."""
    statuses = {}
    for root in download_roots:
        root = Path(root)
        if not root.is_dir():
            continue
        for directory in root.iterdir():
            try:
                status = json.loads((directory / "shard-status.json").read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            if isinstance(status, dict) and isinstance(status.get("cases"), list):
                statuses[directory.name] = status
    return statuses


def plan(artifacts, jobs, statuses):
    """Returns (ids to delete, lines explaining what stays)."""
    failed_job_ids = {job_id for job_id, conclusion, _ in jobs if conclusion in FAILED_CONCLUSIONS}
    status_job_ids = {str(status.get("job_id") or "") for status in statuses.values()} - {""}
    # Failed jobs that left no status file: nothing says which cases they were running.
    unexplained = failed_job_ids - status_job_ids
    if not status_job_ids and failed_job_ids:
        # The statuses carry no job ids to match against. Count instead: each failed job must
        # be explained by an artifact that shows a case without a pass.
        explaining = sum(1 for status in statuses.values()
                         if any(record.get("status") != "passed" for record in status["cases"]))
        unexplained = set() if explaining >= len(failed_job_ids) else failed_job_ids

    failed_cases = set()  # did not pass somewhere: every result that holds one stays
    for status in statuses.values():
        not_passed = [record for record in status["cases"] if record.get("status") != "passed"]
        if status_job_ids:
            job_failed = str(status.get("job_id") or "") in failed_job_ids
        else:
            # Without job ids, a status that shows a case without a pass stands for a failed job.
            job_failed = bool(failed_job_ids) and bool(not_passed)
        for record in not_passed:
            if record.get("status") == "failed" or job_failed:
                failed_cases.add(record.get("case"))

    delete, kept = [], []
    for artifact_id, name in artifacts:
        if not name.startswith(RESULT_PREFIXES):
            continue
        reason = ""
        status = statuses.get(name)
        if unexplained:
            reason = f"{len(unexplained)} failed job(s) left no shard status"
        elif status is None:
            reason = "no shard status was downloaded for it"
        elif str(status.get("job_id") or "") in failed_job_ids:
            reason = "its job failed"
        else:
            cases = sorted(failed_cases.intersection(record.get("case") for record in status["cases"]))
            if cases:
                reason = "holds failed case(s): " + ", ".join(cases)
        if reason:
            kept.append(f"{name} ({artifact_id}): {reason}")
        else:
            delete.append((artifact_id, name))
    return delete, kept


class Deleter:
    """Runs `command` once per artifact, with the artifact id appended to its last word."""

    def __init__(self, command):
        self.command = command
        self.lock = threading.Lock()
        self.next_start = 0.0
        self.refused = []
        self.deleted = 0

    def wait_for_slot(self):
        with self.lock:
            now = time.monotonic()
            start = max(now, self.next_start)
            self.next_start = start + DELETE_INTERVAL_SECONDS
        if start > now:
            pause(start - now)

    def delete(self, artifact_id, name):
        for attempt in (1, 2):
            with self.lock:
                if len(self.refused) >= MAX_REFUSED_DELETES:
                    return
            self.wait_for_slot()
            done = subprocess.run(self.command[:-1] + [self.command[-1] + artifact_id],
                                  capture_output=True, text=True)
            if done.returncode == 0:
                with self.lock:
                    self.deleted += 1
                return
            if attempt == 1:
                # Give a rate limit time to clear before the one retry.
                with self.lock:
                    self.next_start = max(self.next_start, time.monotonic() + 30)
        with self.lock:
            self.refused.append(f"{name} ({artifact_id}): {' '.join(done.stderr.split())[:200]}")

    def run(self, entries):
        pending = list(entries)

        def work():
            while True:
                with self.lock:
                    if not pending:
                        return
                    artifact_id, name = pending.pop(0)
                self.delete(artifact_id, name)

        threads = [threading.Thread(target=work) for _ in range(min(DELETE_WORKERS, len(pending)))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()


def gh_rows(path, query, columns):
    done = subprocess.run(["gh", "api", "--paginate", path, "--jq", query], capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError(f"gh api {path} failed: {done.stderr.strip()}")
    return read_rows(done.stdout, columns)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--downloads", action="append", default=[],
                        help="Directory holding one sub-directory per downloaded result artifact.")
    parser.add_argument("--artifacts-file", help="Tab-separated id and name per line, instead of asking gh.")
    parser.add_argument("--jobs-file", help="Tab-separated id, conclusion and name per line, instead of asking gh.")
    parser.add_argument("--dry-run", action="store_true", help="Print the plan, delete nothing.")
    options = parser.parse_args(argv)

    repository = os.environ.get("GITHUB_REPOSITORY", "")
    run_id = os.environ.get("GITHUB_RUN_ID", "")
    try:
        if options.artifacts_file:
            artifacts = read_rows(Path(options.artifacts_file).read_text(encoding="utf-8"), 2)
        else:
            artifacts = gh_rows(f"repos/{repository}/actions/runs/{run_id}/artifacts?per_page=100",
                                ".artifacts[] | [.id, .name] | @tsv", 2)
        if options.jobs_file:
            jobs = read_rows(Path(options.jobs_file).read_text(encoding="utf-8"), 3)
        else:
            # 50 rows per page: the jobs endpoint answers HTTP 502 for larger pages of a re-run
            # attempt of a run this size.
            jobs = gh_rows(f"repos/{repository}/actions/runs/{run_id}/jobs?per_page=50",
                           '.jobs[] | [.id, .conclusion // "", .name] | @tsv', 3)
    except (OSError, RuntimeError) as error:
        print(f"::error::Could not read the run's artifacts and jobs; nothing was deleted: {error}")
        return 1

    delete, kept = plan([(row[0], row[1]) for row in artifacts], [tuple(row[:3]) for row in jobs],
                        shard_statuses(options.downloads))
    for line in kept:
        print(f"Retaining {line}")
    if options.dry_run:
        for artifact_id, name in delete:
            print(f"Would delete {name} ({artifact_id})")
        return 0

    deleter = Deleter(shlex.split(os.environ.get("MOBILEGL_CLEANUP_DELETE_COMMAND") or
                                  f"gh api --method DELETE repos/{repository}/actions/artifacts/"))
    started = time.monotonic()
    deleter.run(delete)
    for line in deleter.refused:
        print(f"::warning::Could not delete intermediate artifact {line}")
    if len(deleter.refused) >= MAX_REFUSED_DELETES:
        print("::warning::Stopped deleting intermediate artifacts after repeated refusals; the rest stay until they expire.")
    print(f"Deleted {deleter.deleted} intermediate Android artifact(s) in {time.monotonic() - started:.0f}s; "
          f"retained {len(kept)}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
