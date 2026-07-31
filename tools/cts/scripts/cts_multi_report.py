#!/usr/bin/env python
"""Summarise any number of GL CTS suites and MobileGL backends.

Each repeatable suite specification consists of four values: backend, label,
caselist, and result directory.  For example::

    python cts_multi_report.py \
      --suite DirectGLES gl30 gl30-main.txt runs/gles/gl30 \
      --suite DirectVulkan gl30 gl30-main.txt runs/vulkan/gl30 \
      --markdown cts-summary.md --json cts-summary.json

A compact comma form is accepted as well::

    --suite=DirectGLES,gl31,gl31-main.txt,runs/gles/gl31

Per-suite parsing and validation deliberately delegate to
``cts_matrix_report`` so QPA ordering, sidecar upgrades, accepted statuses,
unrun handling, and duplicate-result semantics cannot drift between reports.
All conformance rates use unique expected caselist cases as their denominator.
Backend subtotals and the overall total are therefore case-weighted, not an
unweighted average of suite percentages.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
import sys
from typing import Iterable, Optional, Sequence

try:  # Direct script execution and package imports are both supported.
    from . import cts_matrix_report
except ImportError:  # pragma: no cover - covered through CLI-style tests
    import cts_matrix_report


SUPPORTED_BACKENDS = ("DirectGLES", "DirectVulkan")
SUM_FIELDS = (
    "expected",
    "result",
    "pass",
    "accepted",
    "crash",
    "hang",
    "unrun",
    "duplicate",
)


class MultiReportInputError(ValueError):
    """Suite specifications cannot produce an unambiguous report."""


@dataclass(frozen=True)
class SuiteSpec:
    backend: str
    label: str
    caselist: str
    result_dir: str

    @property
    def suite_id(self) -> str:
        return f"{self.backend}/{self.label}"


def _ratio(numerator: int, denominator: int) -> float:
    return numerator / denominator if denominator else 0.0


def _validation_state(items: Sequence[dict]) -> str:
    states = {item["validation"]["state"] for item in items}
    if "ERROR" in states:
        return "ERROR"
    if "INCOMPLETE" in states:
        return "INCOMPLETE"
    return "OK"


def aggregate_reports(items: Sequence[dict], suite_state_keys: Sequence[str]) -> dict:
    """Return an expected-case-weighted aggregate for suite reports."""

    if len(items) != len(suite_state_keys):
        raise MultiReportInputError("internal suite/state key count mismatch")

    totals = {
        field: sum(int(item[field]) for item in items)
        for field in SUM_FIELDS
    }
    status_counts: Counter[str] = Counter()
    for item in items:
        status_counts.update(item["counts"])

    state = _validation_state(items)
    expected = totals["expected"]
    result = totals["result"]
    suite_states = {
        key: item["validation"]["state"]
        for key, item in zip(suite_state_keys, items)
    }
    return {
        **totals,
        "suite_count": len(items),
        "counts": dict(sorted(status_counts.items())),
        "aggregation": "weighted_by_expected_cases",
        "coverage": {
            "numerator": result,
            "denominator": expected,
            "rate": _ratio(result, expected),
        },
        "rates": {
            "denominator": "expected",
            "strict_pass_only": _ratio(totals["pass"], expected),
            "conformance_accepted": _ratio(totals["accepted"], expected),
            "measured_only_strict_pass": _ratio(totals["pass"], result),
            "measured_only_conformance_accepted": _ratio(
                totals["accepted"], result
            ),
        },
        # Keep the convenient aliases used by cts_matrix_report consumers.
        "strict_pass_rate": _ratio(totals["pass"], expected),
        "conformance_accepted_rate": _ratio(totals["accepted"], expected),
        "validation": {
            "state": state,
            "ok": state == "OK",
            "suite_states": suite_states,
            "invariant_expected_equals_result_plus_unrun": (
                expected == result + totals["unrun"]
            ),
        },
    }


def _caselist_fingerprint(path: str) -> tuple[str, int]:
    cases, _duplicates = cts_matrix_report.read_caselist(path)
    payload = "\n".join(cases).encode("utf-8") + b"\n"
    return hashlib.sha256(payload).hexdigest(), len(cases)


def _read_provenance(
    spec: SuiteSpec,
    require_run_state: bool,
    expected_run_identity: Optional[str],
) -> dict:
    path = os.path.join(spec.result_dir, "run_state.json")
    if not os.path.isfile(path):
        if require_run_state or expected_run_identity is not None:
            raise MultiReportInputError(
                "suite result directory has no run_state.json; invocation provenance "
                f"cannot be verified: {spec.result_dir}"
            )
        return {"state": "UNVERIFIED", "run_state": None}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            state = json.load(handle)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MultiReportInputError(f"cannot read suite run identity {path}: {exc}") from exc
    if not isinstance(state, dict):
        raise MultiReportInputError(f"suite run identity must be a JSON object: {path}")
    if state.get("backend") != spec.backend:
        raise MultiReportInputError(
            f"suite {spec.suite_id} is labelled {spec.backend}, but run_state.json "
            f"records {state.get('backend')!r}"
        )
    fingerprint, case_count = _caselist_fingerprint(spec.caselist)
    if state.get("caselist_sha256") != fingerprint or state.get("case_count") != case_count:
        raise MultiReportInputError(
            f"suite {spec.suite_id} run_state.json belongs to a different caselist"
        )
    invocation_identity = state.get("invocation_identity")
    if (
        expected_run_identity is not None
        and invocation_identity != expected_run_identity
    ):
        raise MultiReportInputError(
            f"suite {spec.suite_id} run_state.json belongs to a different CTS invocation"
        )
    return {
        "state": "VERIFIED",
        "run_state": os.path.abspath(path),
        "invocation_identity": invocation_identity,
    }


def _validate_specs(
    specs: Sequence[SuiteSpec],
    require_run_state: bool,
    expected_run_identity: Optional[str],
) -> dict[str, dict]:
    if not specs:
        raise MultiReportInputError("at least one --suite specification is required")

    seen: set[tuple[str, str]] = set()
    seen_result_dirs: list[tuple[str, str]] = []
    provenance: dict[str, dict] = {}
    for spec in specs:
        if spec.backend not in SUPPORTED_BACKENDS:
            raise MultiReportInputError(
                f"unsupported backend {spec.backend!r}; expected one of "
                + ", ".join(SUPPORTED_BACKENDS)
            )
        if not spec.label.strip():
            raise MultiReportInputError("suite label cannot be empty")
        identity = (spec.backend, spec.label)
        if identity in seen:
            raise MultiReportInputError(
                f"duplicate suite specification for {spec.backend}/{spec.label}"
            )
        seen.add(identity)
        if not os.path.isdir(spec.result_dir):
            raise MultiReportInputError(
                f"suite result directory does not exist: {spec.result_dir}"
            )
        physical_result_dir = os.path.normcase(
            os.path.realpath(os.path.abspath(spec.result_dir))
        )
        for previous_dir, previous_suite in seen_result_dirs:
            try:
                common_dir = os.path.commonpath(
                    [previous_dir, physical_result_dir]
                )
            except ValueError:
                continue
            if common_dir in (previous_dir, physical_result_dir):
                raise MultiReportInputError(
                    f"suite {spec.suite_id} uses a result directory which overlaps "
                    f"{previous_suite}: {spec.result_dir}"
                )
        seen_result_dirs.append((physical_result_dir, spec.suite_id))
        provenance[spec.suite_id] = _read_provenance(
            spec, require_run_state, expected_run_identity
        )
    return provenance


def build_report(
    specs: Sequence[SuiteSpec],
    require_run_state: bool = True,
    expected_run_identity: Optional[str] = None,
) -> dict:
    """Build suite, per-backend, and overall serialisable reports."""

    provenance = _validate_specs(
        specs, require_run_state, expected_run_identity
    )
    suite_reports: list[dict] = []
    backend_order: list[str] = []

    for spec in specs:
        if spec.backend not in backend_order:
            backend_order.append(spec.backend)
        item = cts_matrix_report.build_version_report(
            spec.label, spec.caselist, [spec.result_dir]
        )
        # ``version`` is the generic label argument in build_version_report;
        # expose explicit multi-report terminology while retaining all of its
        # validation and case-level evidence.
        item.pop("version", None)
        item["backend"] = spec.backend
        item["label"] = spec.label
        item["suite_id"] = spec.suite_id
        item["provenance"] = provenance[spec.suite_id]
        if item["provenance"]["state"] == "UNVERIFIED":
            item["validation"]["warnings"].append(
                "result directory has no run_state.json; backend provenance is unverified"
            )
        suite_reports.append(item)

    backends: dict[str, dict] = {}
    for backend in backend_order:
        backend_items = [
            item for item in suite_reports if item["backend"] == backend
        ]
        labels = [item["label"] for item in backend_items]
        aggregate = aggregate_reports(backend_items, labels)
        aggregate["backend"] = backend
        aggregate["suite_labels"] = labels
        backends[backend] = aggregate

    overall = aggregate_reports(
        suite_reports, [item["suite_id"] for item in suite_reports]
    )
    overall["backend_count"] = len(backends)
    overall["backends"] = backend_order

    return {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "accepted_statuses": list(cts_matrix_report.ACCEPTED_STATUSES),
        "rate_policy": {
            "denominator": "unique expected cases from each suite caselist",
            "unrun_cases": "included in the denominator and never accepted",
            "backend_aggregation": "weighted by expected cases",
            "overall_aggregation": "weighted by expected cases across backend-suite pairs",
            "duplicate_results": "last QPA result wins, matching qpa_report.py",
        },
        "suites": suite_reports,
        "backends": backends,
        "overall": overall,
    }


def _percent(numerator: int, denominator: int) -> str:
    if not denominator:
        return "n/a"
    return f"{100.0 * numerator / denominator:.2f}% ({numerator}/{denominator})"


def _markdown_cell(value: object) -> str:
    return str(value).replace("|", r"\|").replace("\r", " ").replace("\n", " ")


def _table_row(backend: str, label: str, item: dict, bold: bool = False) -> str:
    values = [
        backend,
        label,
        str(item["expected"]),
        str(item["result"]),
        str(item["pass"]),
        str(item["accepted"]),
        str(item["crash"]),
        str(item["hang"]),
        str(item["unrun"]),
        str(item["duplicate"]),
        _percent(item["result"], item["expected"]),
        _percent(item["pass"], item["expected"]),
        _percent(item["accepted"], item["expected"]),
        item["validation"]["state"],
    ]
    values = [_markdown_cell(value) for value in values]
    if bold:
        values = [f"**{value}**" for value in values]
    return "| " + " | ".join(values) + " |"


def render_markdown(report: dict) -> str:
    lines = [
        "# GL CTS multi-suite conformance report",
        "",
        (
            "| Backend | Suite | Expected | Result | Pass | Accepted | Crash | Hang | "
            "Unrun | Duplicate | Coverage | Strict Pass-only | Conformance-accepted | Validation |"
        ),
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---:|",
    ]

    for backend in report["backends"]:
        for item in report["suites"]:
            if item["backend"] == backend:
                lines.append(_table_row(backend, item["label"], item))
        subtotal = report["backends"][backend]
        lines.append(
            _table_row(backend, f"{backend} weighted subtotal", subtotal, bold=True)
        )

    lines.append(
        _table_row(
            "All backends",
            "Overall weighted",
            report["overall"],
            bold=True,
        )
    )
    lines.extend(
        [
            "",
            (
                "Rates use unique **Expected** caselist cases as the denominator. "
                "Unrun cases remain in the denominator and are not accepted."
            ),
            "Accepted statuses: "
            + ", ".join(
                f"`{status}`" for status in report["accepted_statuses"]
            )
            + ".",
            (
                "Duplicate is the number of extra QPA observations; the final "
                "observation wins."
            ),
        ]
    )

    details: list[str] = []
    for item in report["suites"]:
        validation = item["validation"]
        messages = validation["errors"] + validation["warnings"]
        if messages:
            details.append(
                f"- **{_markdown_cell(item['suite_id'])} {validation['state']}**: "
                + "; ".join(_markdown_cell(message) for message in messages)
            )
    if details:
        lines.extend(["", "## Validation details", "", *details])
    return "\n".join(lines) + "\n"


def _write_text(path: str, contents: str) -> None:
    absolute = os.path.abspath(path)
    os.makedirs(os.path.dirname(absolute), exist_ok=True)
    with open(absolute, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(contents)


def _write_json(path: str, report: dict) -> None:
    _write_text(path, json.dumps(report, indent=2, sort_keys=True) + "\n")


def _normalise_compact_suite_args(argv: Sequence[str]) -> list[str]:
    """Expand ``--suite=b,l,c,r`` into the four-value argparse form."""

    result: list[str] = []
    index = 0
    while index < len(argv):
        token = argv[index]
        if token.startswith("--suite="):
            compact = token.split("=", 1)[1]
            parts = compact.split(",", 3)
            if len(parts) != 4:
                raise MultiReportInputError(
                    "compact --suite expects backend,label,caselist,result-dir"
                )
            result.extend(["--suite", *parts])
            index += 1
            continue
        if token == "--suite" and index + 1 < len(argv) and argv[index + 1].count(",") >= 3:
            parts = argv[index + 1].split(",", 3)
            result.extend(["--suite", *parts])
            index += 2
            continue
        result.append(token)
        index += 1
    return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--suite",
        action="append",
        nargs=4,
        required=True,
        metavar=("BACKEND", "LABEL", "CASELIST", "RESULT_DIR"),
        help=(
            "suite specification; repeat for every backend/suite pair "
            f"(backends: {', '.join(SUPPORTED_BACKENDS)})"
        ),
    )
    parser.add_argument(
        "--markdown",
        default="cts_multi_report.md",
        help="Markdown output path (default: ./cts_multi_report.md)",
    )
    parser.add_argument(
        "--json",
        dest="json_out",
        default="cts_multi_report.json",
        help="JSON output path (default: ./cts_multi_report.json)",
    )
    parser.add_argument(
        "--allow-incomplete",
        action="store_true",
        help="return success even when one or more suites are ERROR/INCOMPLETE",
    )
    parser.add_argument(
        "--adopt-legacy",
        dest="allow_unverified_provenance",
        action="store_true",
        help="accept legacy result directories without run_state.json (provenance remains unverified)",
    )
    parser.add_argument(
        "--allow-unverified-provenance",
        dest="allow_unverified_provenance",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--expected-run-identity",
        help="require every suite run_state.json to contain this controller fingerprint",
    )
    return parser


def _specs_from_args(values: Sequence[Sequence[str]]) -> list[SuiteSpec]:
    return [
        SuiteSpec(
            backend=backend.strip(),
            label=label.strip(),
            caselist=caselist,
            result_dir=result_dir,
        )
        for backend, label, caselist, result_dir in values
    ]


def main(argv: Optional[Iterable[str]] = None) -> int:
    raw_argv = list(argv) if argv is not None else sys.argv[1:]
    try:
        normalised = _normalise_compact_suite_args(raw_argv)
    except MultiReportInputError as exc:
        print(f"cts_multi_report: {exc}", file=sys.stderr)
        return 2
    args = _parser().parse_args(normalised)

    if os.path.normcase(os.path.abspath(args.markdown)) == os.path.normcase(
        os.path.abspath(args.json_out)
    ):
        print("cts_multi_report: Markdown and JSON paths must differ", file=sys.stderr)
        return 2

    try:
        report = build_report(
            _specs_from_args(args.suite),
            require_run_state=not args.allow_unverified_provenance,
            expected_run_identity=args.expected_run_identity,
        )
        markdown = render_markdown(report)
        _write_text(args.markdown, markdown)
        _write_json(args.json_out, report)
    except (
        OSError,
        MultiReportInputError,
        cts_matrix_report.ReportInputError,
    ) as exc:
        print(f"cts_multi_report: {exc}", file=sys.stderr)
        return 2

    print(markdown, end="")
    print(f"\nMarkdown: {os.path.abspath(args.markdown)}")
    print(f"JSON: {os.path.abspath(args.json_out)}")
    if not args.allow_incomplete and not report["overall"]["validation"]["ok"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
