#!/usr/bin/env python
"""Summarise dEQP/glcts .qpa logs into a conformance pass rate.

Handles the two ways a case can end in a .qpa: a normal
``#beginTestCaseResult``/``#endTestCaseResult`` pair carrying a
``<Result StatusCode="...">`` element, and ``#terminateTestCaseResult <reason>``,
which is what the log contains when the process died partway through a case.
Cases that were started but never terminated (the run was killed) are reported
separately so a truncated chunk is never silently scored as a pass.

Usage:
    python qpa_report.py <file-or-dir> [<file-or-dir> ...] [--json out.json] [--top N]
"""

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

# Khronos conformance treats these as non-failures: the test either passed or
# the implementation legitimately does not expose the feature under test.
NON_FAILURE = {
    "Pass",
    "NotSupported",
    "QualityWarning",
    "CompatibilityWarning",
    "Waiver",
}

# Statuses that indicate the case did not merely fail but destabilised the run.
HARD = {"Crash", "Timeout", "InternalError", "ResourceError", "DeviceHang"}

CASE_START = re.compile(r"^#beginTestCaseResult\s+(\S+)")
CASE_END = re.compile(r"^#endTestCaseResult")
CASE_TERM = re.compile(r"^#terminateTestCaseResult\s+(.*)")
RESULT = re.compile(r'<Result\s+StatusCode="([^"]+)"')


def parse_qpa(path):
    """Yield (case_name, status) for every case recorded in one .qpa file."""
    current = None
    status = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = CASE_START.match(line)
            if m:
                if current is not None:
                    # A new case started before the previous one closed.
                    yield current, status or "Incomplete"
                current, status = m.group(1), None
                continue
            if current is None:
                continue
            m = RESULT.search(line)
            if m:
                status = m.group(1)
                continue
            m = CASE_TERM.match(line)
            if m:
                reason = m.group(1).strip() or "Terminated"
                # dEQP writes e.g. "Crash" / "Timeout" here.
                yield current, reason if reason in HARD else "Crash"
                current, status = None, None
                continue
            if CASE_END.match(line):
                yield current, status or "Incomplete"
                current, status = None, None
    if current is not None:
        # File ended mid-case: the runner was killed.
        yield current, "Incomplete"


def collect(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, names in os.walk(p):
                files.extend(os.path.join(root, n) for n in sorted(names) if n.endswith(".qpa"))
        else:
            files.append(p)
    return files


def group_of(case):
    """The case's parent group, e.g. KHR-GL33.shaders.arrays for ...arrays.foo."""
    parts = case.split(".")
    return ".".join(parts[:-1]) if len(parts) > 1 else case


def load_sidecar(paths, name):
    """Case names run_cts.py recorded in one of its sidecar lists."""
    out = set()
    for p in paths:
        d = p if os.path.isdir(p) else os.path.dirname(p)
        f = os.path.join(d, name)
        if os.path.isfile(f):
            with open(f, "r", encoding="utf-8") as fh:
                out.update(l.strip() for l in fh if l.strip() and not l.strip().startswith("#"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--json", dest="json_out")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    files = collect(args.paths)
    if not files:
        print("no .qpa files found", file=sys.stderr)
        return 2

    # Later chunks may re-run a case; last result wins.
    results = {}
    for f in files:
        for case, status in parse_qpa(f):
            results[case] = status

    # A case the runner saw take the process down is a Crash, not merely an
    # unterminated log entry - but a real result from a later retry wins.
    for case in load_sidecar(args.paths, "crashed.txt"):
        if results.get(case, "Incomplete") == "Incomplete":
            results[case] = "Crash"
    # Worse than a crash: these rebooted the device.
    for case in load_sidecar(args.paths, "hung.txt"):
        if results.get(case, "Incomplete") in ("Incomplete", "Crash"):
            results[case] = "DeviceHang"

    # Cases excluded up front, and cases the run never reached, are not results.
    # Report them separately so a partial run is never read as a complete one.
    skipped = load_sidecar(args.paths, "skipped.txt")
    unrun = load_sidecar(args.paths, "unrun.txt") - set(results)

    counts = Counter(results.values())
    total = len(results)
    non_fail = sum(counts[s] for s in NON_FAILURE)
    strict_pass = counts["Pass"]
    failures = total - non_fail

    by_group_fail = defaultdict(int)
    by_group_total = defaultdict(int)
    for case, status in results.items():
        g = group_of(case)
        by_group_total[g] += 1
        if status not in NON_FAILURE:
            by_group_fail[g] += 1

    label = f" [{args.label}]" if args.label else ""
    print(f"=== glcts conformance summary{label} ===")
    print(f"files parsed      : {len(files)}")
    print(f"cases with result : {total}")
    print()
    for status, n in counts.most_common():
        mark = "   " if status in NON_FAILURE else " ! "
        print(f"{mark}{status:<22} {n:>7}  {100.0 * n / total:6.2f}%")
    print()
    if total:
        print(f"conformance pass rate (Pass+NotSupported+warnings) : {100.0 * non_fail / total:6.2f}%  ({non_fail}/{total})")
        print(f"strict pass rate      (Pass only)                  : {100.0 * strict_pass / total:6.2f}%  ({strict_pass}/{total})")
        print(f"failures                                           : {failures}")

    if skipped or unrun:
        print("\n--- NOT MEASURED (excluded from the rates above) ---")
        if skipped:
            print(f"  quarantined up front : {len(skipped)}")
        if unrun:
            print(f"  never reached        : {len(unrun)}")
        print("  The rates above cover only cases that produced a result.")

    if failures:
        print(f"\n--- worst groups (of {len(by_group_total)}) ---")
        worst = sorted(by_group_fail.items(), key=lambda kv: -kv[1])[: args.top]
        for g, nf in worst:
            nt = by_group_total[g]
            print(f"  {g:<52} {nf:>6}/{nt:<6} fail  ({100.0 * nf / nt:5.1f}%)")

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "label": args.label,
                    "files": len(files),
                    "total": total,
                    "counts": dict(counts),
                    "non_failure": non_fail,
                    "strict_pass": strict_pass,
                    "failures": failures,
                    "pass_rate": (non_fail / total) if total else 0.0,
                    "strict_pass_rate": (strict_pass / total) if total else 0.0,
                    "results": results,
                },
                fh,
                indent=1,
            )
        print(f"\nwrote {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
