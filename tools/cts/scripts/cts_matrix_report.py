#!/usr/bin/env python
"""Build a GL 3.0--3.3 CTS conformance matrix from dEQP QPA logs.

The report deliberately scores against the unique cases in each supplied
caselist.  A case that has not produced a result therefore cannot disappear
from the denominator and make a partial run look conformant.

QPA parsing and crash/hang sidecar handling follow :mod:`qpa_report`:

* a later QPA observation of a case wins;
* ``crashed.txt`` upgrades a missing/incomplete result to ``Crash``;
* ``hung.txt`` upgrades a missing/incomplete/crash result to ``DeviceHang``.

Example::

    python cts_matrix_report.py \
      --gl30-caselist gl30-main.txt --gl30-results runs/gl30 \
      --gl31-caselist gl31-main.txt --gl31-results runs/gl31 \
      --gl32-caselist gl32-main.txt --gl32-results runs/gl32 \
      --gl33-caselist gl33-main.txt --gl33-results runs/gl33 \
      --json runs/cts-matrix.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from typing import Iterable, Optional, Sequence

try:  # Works both as a directly executed script and as a package import.
    from . import qpa_report
except ImportError:  # pragma: no cover - exercised by the command-line tests
    import qpa_report


VERSIONS = ("gl30", "gl31", "gl32", "gl33")
ACCEPTED_STATUSES = (
    "Pass",
    "NotSupported",
    "QualityWarning",
    "CompatibilityWarning",
    "Waiver",
)
ACCEPTED = frozenset(ACCEPTED_STATUSES)
CHUNK_QPA = re.compile(r"^chunk(\d+)\.qpa$", re.IGNORECASE)


class ReportInputError(ValueError):
    """An input path cannot be used to construct a meaningful report."""


def _read_non_comment_lines(path: str) -> list[str]:
    try:
        # Match run_cts_windows.py: Khronos lists are UTF-8 and may carry a BOM.
        with open(path, "r", encoding="utf-8-sig", errors="strict") as fh:
            return [
                line.strip()
                for line in fh
                if line.strip() and not line.lstrip().startswith("#")
            ]
    except (OSError, UnicodeError) as exc:
        raise ReportInputError(f"cannot read {path}: {exc}") from exc


def read_caselist(path: str) -> tuple[list[str], dict[str, int]]:
    """Return unique cases in file order and repeated caselist entries.

    The mustpass files consumed by glcts and ``run_cts.py`` are one case per
    non-empty, non-comment line, so this intentionally uses the same syntax.
    """

    entries = _read_non_comment_lines(path)
    counts = Counter(entries)
    unique = list(dict.fromkeys(entries))
    duplicates = {case: count for case, count in counts.items() if count > 1}
    return unique, duplicates


def _collect_qpa_files(paths: Sequence[str]) -> list[str]:
    missing = [path for path in paths if not os.path.exists(path)]
    if missing:
        raise ReportInputError(
            "result path(s) do not exist: " + ", ".join(sorted(missing))
        )

    # qpa_report.collect provides the established directory-recursion rules.
    # De-duplicate aliases so specifying the same directory twice does not
    # manufacture duplicate observations.
    files = qpa_report.collect(paths)
    by_identity: dict[str, str] = {}
    for path in files:
        if not os.path.isfile(path):
            raise ReportInputError(f"QPA input is not a file: {path}")
        absolute = os.path.abspath(path)
        by_identity.setdefault(os.path.normcase(absolute), absolute)
    def order_key(value: str) -> tuple[str, str, int, str]:
        absolute = os.path.abspath(value)
        directory = os.path.normcase(os.path.dirname(absolute))
        filename = os.path.normcase(os.path.basename(absolute))
        match = CHUNK_QPA.fullmatch(filename)
        if match:
            # run_cts_windows.py uses a minimum width of four digits, not a
            # fixed width.  Numeric ordering is therefore required once a run
            # reaches chunk10000; lexical ordering would put it before
            # chunk9999 and break the later-observation-wins rule.
            return directory, "chunk", int(match.group(1)), filename
        # Preserve a deterministic, name-based position for foreign/legacy
        # QPA files while grouping numeric runner chunks at the lexical
        # position occupied by the "chunk" basename.
        return directory, filename, -1, filename

    return sorted(by_identity.values(), key=order_key)


def _ratio(numerator: int, denominator: int) -> float:
    return numerator / denominator if denominator else 0.0


def _sidecar(paths: Sequence[str], name: str) -> set[str]:
    """Load a run_cts.py sidecar with qpa_report-compatible lookup rules."""

    return qpa_report.load_sidecar(paths, name)


def build_version_report(
    version: str, caselist: str, result_paths: Sequence[str]
) -> dict:
    """Build the serialisable report for one GL mustpass version."""

    expected_cases, expected_duplicates = read_caselist(caselist)
    expected = set(expected_cases)
    qpa_files = _collect_qpa_files(result_paths)

    results: dict[str, str] = {}
    observation_history: dict[str, list[dict[str, str]]] = defaultdict(list)
    for qpa_file in qpa_files:
        for case, status in qpa_report.parse_qpa(qpa_file):
            observation_history[case].append(
                {"file": qpa_file, "status": status}
            )
            results[case] = status

    crashed = _sidecar(result_paths, "crashed.txt")
    hung = _sidecar(result_paths, "hung.txt")
    explicit_unrun = _sidecar(result_paths, "unrun.txt")
    skipped = _sidecar(result_paths, "skipped.txt")

    # Keep this order and these guards in lock-step with qpa_report.py.
    for case in crashed:
        if results.get(case, "Incomplete") == "Incomplete":
            results[case] = "Crash"
    for case in hung:
        if results.get(case, "Incomplete") in ("Incomplete", "Crash"):
            results[case] = "DeviceHang"

    # A begin/end pair without <Result>, or a QPA truncated mid-case, is not a
    # completed observation.  Sidecars above may upgrade it to Crash/Hang;
    # anything still Incomplete must stay in the expected denominator as unrun.
    incomplete_results = {
        case for case, status in results.items() if status == "Incomplete"
    }
    for case in incomplete_results:
        del results[case]

    expected_results = {
        case: status for case, status in results.items() if case in expected
    }
    unexpected_results = {
        case: status for case, status in results.items() if case not in expected
    }

    # Missing cases are inferred from the caselist even if unrun.txt itself is
    # missing or stale.  This is the invariant that prevents partial-run rate
    # inflation.
    unrun_cases = expected - set(expected_results)
    declared_not_measured = explicit_unrun | skipped
    undeclared_unrun = unrun_cases - declared_not_measured
    stale_unrun = (explicit_unrun | skipped) & set(expected_results)

    counts = Counter(expected_results.values())
    strict_pass = counts["Pass"]
    accepted = sum(counts[status] for status in ACCEPTED)
    result_count = len(expected_results)
    expected_count = len(expected)
    crash_count = counts["Crash"]
    hang_count = counts["DeviceHang"]

    duplicate_cases = {
        case: {
            "observations": len(history),
            "extra_observations": len(history) - 1,
            "final_status": results.get(case, "Incomplete"),
            "history": history,
        }
        for case, history in sorted(observation_history.items())
        if len(history) > 1
    }
    duplicate_observations = sum(
        item["extra_observations"] for item in duplicate_cases.values()
    )

    sidecar_unknown = {
        name: sorted(cases - expected)
        for name, cases in (
            ("crashed.txt", crashed),
            ("hung.txt", hung),
            ("unrun.txt", explicit_unrun),
            ("skipped.txt", skipped),
        )
        if cases - expected
    }

    errors: list[str] = []
    warnings: list[str] = []
    if not expected_count:
        errors.append("caselist has no cases")
    if expected_duplicates:
        errors.append(
            f"caselist has {sum(n - 1 for n in expected_duplicates.values())} "
            "duplicate entry/entries"
        )
    if not qpa_files:
        errors.append("no .qpa files found")
    if unexpected_results:
        errors.append(
            f"{len(unexpected_results)} result case(s) are absent from the caselist"
        )
    if sidecar_unknown:
        errors.append("one or more sidecars name cases absent from the caselist")
    if undeclared_unrun:
        errors.append(
            f"{len(undeclared_unrun)} missing result case(s) are not declared by "
            "unrun.txt/skipped.txt"
        )
    if stale_unrun:
        warnings.append(
            f"{len(stale_unrun)} case(s) declared unrun/skipped also have a result"
        )
    if duplicate_observations:
        warnings.append(
            f"{duplicate_observations} duplicate QPA observation(s); last result wins"
        )
    if incomplete_results:
        warnings.append(
            f"{len(incomplete_results)} QPA case(s) ended without a final result and were treated as unrun"
        )

    if errors:
        state = "ERROR"
    elif unrun_cases:
        state = "INCOMPLETE"
    else:
        state = "OK"

    return {
        "version": version,
        "inputs": {
            "caselist": os.path.abspath(caselist),
            "result_paths": [os.path.abspath(path) for path in result_paths],
            "qpa_files": qpa_files,
        },
        "expected": expected_count,
        "result": result_count,
        "pass": strict_pass,
        "accepted": accepted,
        "crash": crash_count,
        "hang": hang_count,
        "unrun": len(unrun_cases),
        "duplicate": duplicate_observations,
        "counts": dict(sorted(counts.items())),
        "coverage": {
            "numerator": result_count,
            "denominator": expected_count,
            "rate": _ratio(result_count, expected_count),
        },
        "rates": {
            # These are the report's conformance rates.  Expected, not merely
            # measured results, is the denominator.
            "denominator": "expected",
            "strict_pass_only": _ratio(strict_pass, expected_count),
            "conformance_accepted": _ratio(accepted, expected_count),
            # Useful for comparison with qpa_report.py, whose denominator is
            # cases with a result.  Never presented as the conformance rate.
            "measured_only_strict_pass": _ratio(strict_pass, result_count),
            "measured_only_conformance_accepted": _ratio(
                accepted, result_count
            ),
        },
        "strict_pass_rate": _ratio(strict_pass, expected_count),
        "conformance_accepted_rate": _ratio(accepted, expected_count),
        "validation": {
            "state": state,
            "ok": state == "OK",
            "errors": errors,
            "warnings": warnings,
            "invariant_expected_equals_result_plus_unrun": (
                expected_count == result_count + len(unrun_cases)
            ),
            "undeclared_unrun": sorted(undeclared_unrun),
            "stale_unrun_or_skipped": sorted(stale_unrun),
            "sidecar_cases_absent_from_caselist": sidecar_unknown,
        },
        "cases": {
            "results": dict(sorted(expected_results.items())),
            "unrun": sorted(unrun_cases),
            "unexpected_results": dict(sorted(unexpected_results.items())),
            "incomplete_results": sorted(incomplete_results),
            "duplicate_results": duplicate_cases,
            "duplicate_caselist_entries": dict(sorted(expected_duplicates.items())),
        },
    }


def build_matrix(suites: dict[str, tuple[str, Sequence[str]]]) -> dict:
    """Build all four version reports and their case-weighted aggregate."""

    version_reports = {
        version: build_version_report(version, *suites[version])
        for version in VERSIONS
    }

    totals = {
        key: sum(report[key] for report in version_reports.values())
        for key in (
            "expected",
            "result",
            "pass",
            "accepted",
            "crash",
            "hang",
            "unrun",
            "duplicate",
        )
    }
    status_counts: Counter[str] = Counter()
    for report in version_reports.values():
        status_counts.update(report["counts"])

    states = {report["validation"]["state"] for report in version_reports.values()}
    if "ERROR" in states:
        overall_state = "ERROR"
    elif "INCOMPLETE" in states:
        overall_state = "INCOMPLETE"
    else:
        overall_state = "OK"

    overall = {
        **totals,
        "counts": dict(sorted(status_counts.items())),
        "aggregation": "weighted_by_expected_cases",
        "coverage": {
            "numerator": totals["result"],
            "denominator": totals["expected"],
            "rate": _ratio(totals["result"], totals["expected"]),
        },
        "rates": {
            "denominator": "expected",
            "strict_pass_only": _ratio(totals["pass"], totals["expected"]),
            "conformance_accepted": _ratio(
                totals["accepted"], totals["expected"]
            ),
            "measured_only_strict_pass": _ratio(
                totals["pass"], totals["result"]
            ),
            "measured_only_conformance_accepted": _ratio(
                totals["accepted"], totals["result"]
            ),
        },
        "strict_pass_rate": _ratio(totals["pass"], totals["expected"]),
        "conformance_accepted_rate": _ratio(
            totals["accepted"], totals["expected"]
        ),
        "validation": {
            "state": overall_state,
            "ok": overall_state == "OK",
            "invariant_expected_equals_result_plus_unrun": (
                totals["expected"] == totals["result"] + totals["unrun"]
            ),
        },
    }

    return {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "accepted_statuses": list(ACCEPTED_STATUSES),
        "rate_policy": {
            "denominator": "unique expected cases from each caselist",
            "unrun_cases": "included in the denominator and never accepted",
            "duplicate_results": "last QPA result wins, matching qpa_report.py",
        },
        "versions": version_reports,
        "overall": overall,
    }


def _percent(numerator: int, denominator: int) -> str:
    if not denominator:
        return "n/a"
    return f"{100.0 * numerator / denominator:.2f}% ({numerator}/{denominator})"


def render_markdown(report: dict) -> str:
    """Render the compact terminal-facing conformance table."""

    header = (
        "| Suite | Expected | Result | Pass | Accepted | Crash | Hang | Unrun | "
        "Duplicate | Coverage | Strict Pass-only | Conformance-accepted | Validation |"
    )
    separator = (
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---:|"
    )
    rows = [header, separator]
    for version in VERSIONS:
        item = report["versions"][version]
        rows.append(
            "| {version} | {expected} | {result} | {pass_count} | {accepted} | "
            "{crash} | {hang} | {unrun} | {duplicate} | {coverage} | {strict} | "
            "{accepted_rate} | {state} |".format(
                version=version.upper(),
                expected=item["expected"],
                result=item["result"],
                pass_count=item["pass"],
                accepted=item["accepted"],
                crash=item["crash"],
                hang=item["hang"],
                unrun=item["unrun"],
                duplicate=item["duplicate"],
                coverage=_percent(item["result"], item["expected"]),
                strict=_percent(item["pass"], item["expected"]),
                accepted_rate=_percent(item["accepted"], item["expected"]),
                state=item["validation"]["state"],
            )
        )

    overall = report["overall"]
    rows.append(
        "| **Overall (weighted)** | **{expected}** | **{result}** | **{pass_count}** | "
        "**{accepted}** | **{crash}** | **{hang}** | **{unrun}** | **{duplicate}** | "
        "**{coverage}** | **{strict}** | **{accepted_rate}** | **{state}** |".format(
            expected=overall["expected"],
            result=overall["result"],
            pass_count=overall["pass"],
            accepted=overall["accepted"],
            crash=overall["crash"],
            hang=overall["hang"],
            unrun=overall["unrun"],
            duplicate=overall["duplicate"],
            coverage=_percent(overall["result"], overall["expected"]),
            strict=_percent(overall["pass"], overall["expected"]),
            accepted_rate=_percent(overall["accepted"], overall["expected"]),
            state=overall["validation"]["state"],
        )
    )
    rows.extend(
        (
            "",
            "Rates use unique **Expected** caselist cases as the denominator; unrun cases "
            "remain in that denominator and are not accepted.",
            "Accepted statuses: " + ", ".join(f"`{s}`" for s in ACCEPTED_STATUSES) + ".",
            "Duplicate is the number of extra QPA observations; the last observation wins.",
        )
    )

    details: list[str] = []
    for version in VERSIONS:
        validation = report["versions"][version]["validation"]
        messages = validation["errors"] + validation["warnings"]
        if messages:
            details.append(
                f"- **{version.upper()} {validation['state']}**: " + "; ".join(messages)
            )
    if details:
        rows.extend(("", "Validation details:", "", *details))
    return "\n".join(rows)


def _write_json(path: str, report: dict) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    os.makedirs(parent, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(report, fh, indent=2, sort_keys=True)
        fh.write("\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    for version in VERSIONS:
        parser.add_argument(
            f"--{version}-caselist",
            f"--{version}-case-list",
            required=True,
            help=f"{version.upper()} mustpass caselist",
        )
        parser.add_argument(
            f"--{version}-results",
            f"--{version}-result-dir",
            f"--{version}-results-dir",
            action="append",
            required=True,
            help=f"{version.upper()} result directory or QPA file (repeatable)",
        )
    parser.add_argument(
        "--json",
        dest="json_out",
        default="cts_matrix_report.json",
        help="JSON output path (default: ./cts_matrix_report.json)",
    )
    parser.add_argument(
        "--allow-incomplete",
        action="store_true",
        help="return success even when validation is ERROR/INCOMPLETE",
    )
    return parser


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = _parser().parse_args(argv)
    suites = {
        version: (
            getattr(args, f"{version}_caselist"),
            getattr(args, f"{version}_results"),
        )
        for version in VERSIONS
    }
    try:
        report = build_matrix(suites)
        _write_json(args.json_out, report)
    except (OSError, ReportInputError) as exc:
        print(f"cts_matrix_report: {exc}", file=sys.stderr)
        return 2

    print(render_markdown(report))
    print(f"\nJSON: {os.path.abspath(args.json_out)}")
    if not args.allow_incomplete and not report["overall"]["validation"]["ok"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
