#!/usr/bin/env python3
# MobileGL - android-plugin/retrace_shard_budget.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Give every emulator shard the deadline and the job timeout its own cases need.

The cases of a shard share one job, so one case that hangs spends time the cases after it
need. This reads a shard matrix of trace_cases.py (--format github-apk-shard-matrix or
github-apk-transport-shard-matrix) on standard input, or from --matrix-file, and prints it
with two fields added to every shard: deadline_seconds, which the retrace job passes to
trace-replay-shard.sh, and timeout_minutes, the limit of that job. The deadline is what the
shard needs for every case to use its whole time budget (trace_replay_shard.needed_seconds),
up to --cap-seconds. A capped shard still replays every case; only when several of its
replays hang to the end of their budgets are the cases after them recorded as not-run.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import trace_replay_shard  # noqa: E402  (a sibling file, found through the line above)


def shard_label(row):
    backend = row.get("backend")
    backend = backend.get("name", "") if isinstance(backend, dict) else str(backend or "")
    return f"{backend} / {row.get('transport') or 'monolith'}, shard {row.get('shard')}/{row.get('shards')}"


def shard_needs(matrix):
    """(label, case count, seconds needed) for every shard of the matrix."""
    rows = matrix.get("include") if isinstance(matrix, dict) else None
    if not isinstance(rows, list) or not rows:
        raise ValueError("the matrix has no shard rows under 'include'")
    needs = []
    for row in rows:
        cases = row.get("cases") if isinstance(row, dict) else None
        if not isinstance(cases, list) or not cases:
            raise ValueError(f"a shard row has no 'cases' list: {str(row)[:200]}")
        needs.append((shard_label(row), len(cases), trace_replay_shard.needed_seconds(cases)))
    return needs


# Minutes a retrace job has around its shard runner: the setup before the replays and the
# uploads after them.
JOB_OVERHEAD_MINUTES = 15

# A hosted job may run six hours.
HOSTED_JOB_LIMIT_MINUTES = 360


def with_limits(matrix, cap_seconds):
    """The matrix with deadline_seconds and timeout_minutes on every shard, and the report."""
    needs = shard_needs(matrix)
    rows = []
    report = []
    for row, (label, cases, seconds) in zip(matrix["include"], needs):
        deadline = min(seconds, cap_seconds)
        rows.append({**row, "deadline_seconds": deadline,
                     "timeout_minutes": -(-deadline // 60) + JOB_OVERHEAD_MINUTES})
        if seconds > cap_seconds:
            report.append(f"retrace shard budget: {label} would need {seconds} s for all of its "
                          f"{cases} case(s) to use their whole budgets; capped at {cap_seconds} s")
    largest = max(needs, key=lambda entry: entry[2])
    report.append(f"retrace shard budget: {len(needs)} shard(s), {len(report)} capped at "
                  f"{cap_seconds} s; the most a shard needs is {largest[2]} s ({largest[0]}, "
                  f"{largest[1]} case(s))")
    return {**matrix, "include": rows}, report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cap-seconds", type=int, required=True,
                        help="The longest deadline a shard is given.")
    parser.add_argument("--matrix-file", help="The shard matrix; standard input when not given.")
    options = parser.parse_args(argv)
    limit = (HOSTED_JOB_LIMIT_MINUTES - JOB_OVERHEAD_MINUTES) * 60
    if not 0 < options.cap_seconds <= limit:
        print(f"retrace shard budget: --cap-seconds must be between 1 and {limit}", file=sys.stderr)
        return 2
    if options.matrix_file:
        text = Path(options.matrix_file).read_text(encoding="utf-8")
    else:
        text = sys.stdin.read()
    try:
        matrix, report = with_limits(json.loads(text), options.cap_seconds)
    except (ValueError, KeyError, TypeError) as error:
        print(f"retrace shard budget: the shard matrix cannot be read: {error}", file=sys.stderr)
        return 2
    for line in report:
        print(line, file=sys.stderr)
    print(json.dumps(matrix, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
