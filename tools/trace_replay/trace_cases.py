#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path


TRACE_CASES_JSON = Path(__file__).with_name("trace_cases.json")
CI_BACKENDS = ("DirectGLES", "DirectVulkan")

# Every key a case or the defaults block may carry. An UNKNOWN key is a hard error rather than a
# silent no-op, which is review finding N-4: `"split": true` mistyped as `"splitt": true` loaded
# clean, the split subset became [], the GitHub matrix became {"include":[]}, and `retrace-split`
# was skipped with no red anywhere. Every other way of getting `split` wrong already raised
# (`"ci": false`, a backend list without DirectGLES, a non-bool value) - the typo was the one hole,
# and it is the shape that makes a whole CI job quietly stop existing.
#
# Adding a key means adding it here, deliberately, in the same commit. That is the point.
KNOWN_CASE_KEYS = frozenset({
    "name",
    "trace_archive",
    "trace_file",
    "golden",
    "alternate_golden",
    "target_call",
    "width",
    "height",
    "ssim_threshold",
    "crop_x",
    "crop_y",
    "crop_width",
    "crop_height",
    "coherent_as_flush",
    "timeout_seconds",
    "ci",
    "ci_backends",
    "verify",
    "split",
    "avoid_angle_llvmpipe_explicit_lod_bias",
})


def load_trace_case_manifest(path=TRACE_CASES_JSON):
    with Path(path).open("r", encoding="utf-8") as file:
        manifest = json.load(file)
    defaults = manifest.get("defaults", {})
    unknown_defaults = sorted(set(defaults) - KNOWN_CASE_KEYS)
    if unknown_defaults:
        raise ValueError(
            f"unknown key(s) in the defaults block: {', '.join(unknown_defaults)}. "
            f"Known keys are {', '.join(sorted(KNOWN_CASE_KEYS))}"
        )
    cases = []
    seen = set()
    for case in manifest.get("cases", []):
        merged = {**defaults, **case}
        name = merged.get("name")
        if not name:
            raise ValueError("trace case is missing name")
        unknown = sorted(set(case) - KNOWN_CASE_KEYS)
        if unknown:
            raise ValueError(
                f"unknown key(s) for {name}: {', '.join(unknown)}. A mistyped flag loads clean and "
                f"turns its whole CI subset into an empty matrix, which GitHub skips with no red. "
                f"Known keys are {', '.join(sorted(KNOWN_CASE_KEYS))}"
            )
        if name in seen:
            raise ValueError(f"duplicate trace case: {name}")
        seen.add(name)
        for key in ("trace_archive", "trace_file", "golden", "target_call", "width", "height"):
            if key not in merged:
                raise ValueError(f"{key} is required for {name}")
        # "verify" opts a case into the MOBILEGL_PIPE_VERIFY retrace subset. It is checked here
        # rather than where the matrix is built so that a typo is a loud manifest error in every
        # consumer (the cmake emitter included) instead of a subset that is quietly one case short.
        verify = merged.get("verify", False)
        if not isinstance(verify, bool):
            raise ValueError(f"verify must be true or false for {name}")
        if verify and not merged.get("ci", True):
            raise ValueError(
                f"{name} is marked verify but excluded from CI, so the verify matrix would drop it"
            )
        # "split" opts a case into the MOBILEGL_TRANSPORT=inproc retrace subset, the same shape
        # and the same reason as "verify" above: a typo has to be a loud manifest error in every
        # consumer, not a subset that is quietly one case short. The split arm additionally runs
        # DirectGLES ONLY - the server the arm exercises is Espryt's - so a case that excluded
        # DirectGLES from CI would leave the split matrix with nothing to run.
        split = merged.get("split", False)
        if not isinstance(split, bool):
            raise ValueError(f"split must be true or false for {name}")
        if split and not merged.get("ci", True):
            raise ValueError(
                f"{name} is marked split but excluded from CI, so the split matrix would drop it"
            )
        if split and "DirectGLES" not in ci_backends(merged):
            raise ValueError(
                f"{name} is marked split but does not run DirectGLES in CI; the split arm is "
                f"DirectGLES-only, so the entry would be registered with no backend"
            )
        cases.append(merged)
    return {"defaults": defaults, "cases": cases}


def load_trace_cases(path=TRACE_CASES_JSON):
    return load_trace_case_manifest(path)["cases"]


def case_with_defaults(case):
    return dict(case)


def trace_case_names(path=TRACE_CASES_JSON):
    return [case["name"] for case in load_trace_cases(path)]


def find_trace_case(name, path=TRACE_CASES_JSON):
    for case in load_trace_cases(path):
        if case["name"] == name:
            return case
    raise KeyError(name)


def fixture_files(case, fixture_root):
    root = fixture_root.rstrip("/\\")
    files = [
        f"{root}/{case['trace_archive']}",
        f"{root}/{case['golden']}",
    ]
    if case.get("alternate_golden"):
        files.append(f"{root}/{case['alternate_golden']}")
    return files


def github_apk_case(case):
    result = dict(case)
    for key in ("trace_archive", "golden", "alternate_golden"):
        if result.get(key):
            result[key] = f"tools/trace_replay/fixtures/{result[key]}"
    return result


def ci_trace_cases(cases):
    return [case for case in cases if case.get("ci", True)]


def verify_trace_cases(cases):
    """The subset the third CI mode retraces.

    The verify build compares two state models at every verb boundary and again at every accessor
    read, which the design budgets at 5-10x, so the per-push lane runs a named subset and the full
    79-case sweep happens at the phase exit and on workflow_dispatch.
    """
    return [case for case in cases if case.get("verify", False)]


def split_trace_cases(cases):
    """The subset the split (MOBILEGL_TRANSPORT=inproc) CI mode retraces.

    P5's phase gate names exactly one: OpenRA, at SSIM >= 0.99. It is also the only fixture that
    is hydrated locally, so keeping the subset explicit in the manifest is what stops a later
    phase from widening the arm into an LFS fetch by accident.
    """
    return [case for case in cases if case.get("split", False)]


def ci_backends(case):
    backends = case.get("ci_backends")
    if backends is None:
        return CI_BACKENDS
    if not isinstance(backends, list) or not backends:
        raise ValueError(f"ci_backends must be a non-empty list for {case['name']}")
    unknown = [backend for backend in backends if backend not in CI_BACKENDS]
    if unknown:
        raise ValueError(
            f"unknown ci_backends for {case['name']}: {', '.join(unknown)}"
        )
    if len(set(backends)) != len(backends):
        raise ValueError(f"ci_backends contains duplicates for {case['name']}")
    return backends


def github_test_matrix(cases):
    return {
        "include": [
            {"backend": backend, "case": case["name"]}
            for case in cases
            for backend in ci_backends(case)
        ]
    }


def github_verify_matrix(cases):
    return github_test_matrix(verify_trace_cases(cases))


def github_split_matrix(cases):
    """{backend, case} for the split arm. DirectGLES only - see split_trace_cases."""
    return {
        "include": [
            {"backend": "DirectGLES", "case": case["name"]}
            for case in split_trace_cases(cases)
        ]
    }


def github_apk_matrix(cases):
    backends = {
        "DirectGLES": {"name": "DirectGLES", "gpu": "software"},
        "DirectVulkan": {"name": "DirectVulkan", "gpu": "lavapipe"},
    }
    return {
        "include": [
            {"backend": backends[backend], "case": github_apk_case(case)}
            for case in cases
            for backend in ci_backends(case)
        ]
    }


def github_transport_matrix(cases, *, apk=False):
    """Run the entire original matrix as both control and inproc acceptance.

    The two transports use the same D/P artifact and each case's existing backend
    list. The legacy split:true subset does not limit this main acceptance matrix.
    Nested APK case/backend metadata is deliberately left untouched.
    """
    build = github_apk_matrix if apk else github_test_matrix
    if not cases:
        raise ValueError("transport matrix needs at least one trace case")
    return {"include": [
        {**row, "lane": lane, "transport": transport}
        for selected, lane, transport in (
            (cases, "monolith-control", "monolith"),
            (cases, "inproc-acceptance", "inproc"),
        )
        for row in build(selected)["include"]
    ]}


def self_test_transport_matrices():
    cases = ci_trace_cases(load_trace_cases())
    for apk in (False, True):
        build = github_apk_matrix if apk else github_test_matrix
        original = build(cases)["include"]
        mixed = github_transport_matrix(cases, apk=apk)["include"]
        controls = [row for row in mixed if row["transport"] == "monolith"]
        acceptance = [row for row in mixed if row["transport"] == "inproc"]
        strip_lane = lambda rows: [{key: value for key, value in row.items()
                                   if key not in ("lane", "transport")} for row in rows]
        assert strip_lane(controls) == original, "control lost original case/backend/parameters"
        assert strip_lane(acceptance) == original, "acceptance lost original case/backend/parameters"
        assert all(row["lane"] == "monolith-control" for row in controls)
        assert all(row["lane"] == "inproc-acceptance" for row in acceptance)
        identities = [(row["case"]["name"] if apk else row["case"],
                       row["backend"]["name"] if apk else row["backend"], row["transport"])
                      for row in mixed]
        assert len(identities) == len(set(identities)), "duplicate lane identity"
        # A restricted case keeps exactly its original backend and golden data.
        restricted = [{**cases[0], "split": False, "ci_backends": ["DirectGLES"]}]
        rows = github_transport_matrix(restricted, apk=apk)["include"]
        assert len(rows) == 2 and strip_lane(rows[:1]) == strip_lane(rows[1:])
        try:
            github_transport_matrix([], apk=apk)
        except ValueError:
            pass
        else:
            raise AssertionError("an empty acceptance lane silently passed")
        print(f"transport matrix {'APK' if apk else 'Linux'}: {len(controls)} unchanged controls, "
              f"{len(acceptance)} full acceptance entries; exact metadata and negative controls OK")


def cmake_quote(value):
    return '"' + str(value).replace("\\", "/").replace('"', '\\"') + '"'


def emit_cmake(cases, fixture_root):
    root = fixture_root.rstrip("/\\")
    lines = ["# Generated from tools/trace_replay/trace_cases.json.", ""]
    keys = [
        ("TRACE_ARCHIVE", "trace_archive", True),
        ("TRACE_FILE", "trace_file", False),
        ("GOLDEN", "golden", True),
        ("ALTERNATE_GOLDEN", "alternate_golden", True),
        ("TARGET_CALL", "target_call", False),
        ("WIDTH", "width", False),
        ("HEIGHT", "height", False),
        ("SSIM_THRESHOLD", "ssim_threshold", False),
        ("CROP_X", "crop_x", False),
        ("CROP_Y", "crop_y", False),
        ("CROP_WIDTH", "crop_width", False),
        ("CROP_HEIGHT", "crop_height", False),
        ("COHERENT_AS_FLUSH", "coherent_as_flush", False),
    ]
    def emit_one(function, case):
        lines.append(f"{function}({cmake_quote(case['name'])}")
        for cmake_key, json_key, fixture_path in keys:
            value = case.get(json_key)
            if value is None or value == "":
                continue
            if fixture_path:
                value = f"{root}/{value}"
            lines.append(f"        {cmake_key} {cmake_quote(value)}")
        lines.append(")")
        lines.append("")

    for case in cases:
        emit_one("add_trace_replay_test_for_backends", case)
        # P5's split arm, emitted beside the monolith pair rather than in a block of its own so
        # that a case and its variant always carry identical parameters. The CMake side is a
        # no-op unless MOBILEGL_BUILD_DISAGGREGATED is ON.
        if case.get("split", False):
            emit_one("add_trace_replay_split_test", case)
    return "\n".join(lines)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", dest="case_name", help="Trace case name.")
    parser.add_argument("--ci", action="store_true", help="Only include cases enabled for CI.")
    parser.add_argument("--self-test-transport-matrices", action="store_true")
    parser.add_argument("--fixture-root", default="tools/trace_replay/fixtures")
    parser.add_argument(
        "--format",
        choices=(
            "names",
            "github-test-matrix",
            "github-test-transport-matrix",
            "github-verify-matrix",
            "github-split-matrix",
            "github-apk",
            "github-apk-matrix",
            "github-apk-transport-matrix",
            "fixture-files",
            "cmake",
        ),
        default="names",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if args.self_test_transport_matrices:
        self_test_transport_matrices()
        return 0
    cases = load_trace_cases()
    if args.ci:
        cases = ci_trace_cases(cases)
    if args.format == "names":
        print(json.dumps([case["name"] for case in cases], separators=(",", ":")))
    elif args.format == "github-test-matrix":
        print(json.dumps(github_test_matrix(cases), separators=(",", ":")))
    elif args.format == "github-test-transport-matrix":
        print(json.dumps(github_transport_matrix(cases), separators=(",", ":")))
    elif args.format == "github-verify-matrix":
        print(json.dumps(github_verify_matrix(cases), separators=(",", ":")))
    elif args.format == "github-split-matrix":
        print(json.dumps(github_split_matrix(cases), separators=(",", ":")))
    elif args.format == "github-apk":
        print(json.dumps([github_apk_case(case) for case in cases], separators=(",", ":")))
    elif args.format == "github-apk-matrix":
        print(json.dumps(github_apk_matrix(cases), separators=(",", ":")))
    elif args.format == "github-apk-transport-matrix":
        print(json.dumps(github_transport_matrix(cases, apk=True), separators=(",", ":")))
    elif args.format == "fixture-files":
        if not args.case_name:
            print("--case is required for --format fixture-files", file=sys.stderr)
            return 2
        try:
            case = find_trace_case(args.case_name)
        except KeyError:
            print(f"unknown trace case: {args.case_name}", file=sys.stderr)
            return 2
        print("\n".join(fixture_files(case, args.fixture_root)))
    elif args.format == "cmake":
        print(emit_cmake(cases, args.fixture_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
