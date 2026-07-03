#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path


TRACE_CASES_JSON = Path(__file__).with_name("trace_cases.json")


def load_trace_case_manifest(path=TRACE_CASES_JSON):
    with Path(path).open("r", encoding="utf-8") as file:
        manifest = json.load(file)
    defaults = manifest.get("defaults", {})
    cases = []
    seen = set()
    for case in manifest.get("cases", []):
        merged = {**defaults, **case}
        name = merged.get("name")
        if not name:
            raise ValueError("trace case is missing name")
        if name in seen:
            raise ValueError(f"duplicate trace case: {name}")
        seen.add(name)
        for key in ("trace_archive", "trace_file", "golden", "target_call", "width", "height"):
            if key not in merged:
                raise ValueError(f"{key} is required for {name}")
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
    ]
    for case in cases:
        lines.append(f"add_trace_replay_test_for_backends({cmake_quote(case['name'])}")
        for cmake_key, json_key, fixture_path in keys:
            value = case.get(json_key)
            if value is None or value == "":
                continue
            if fixture_path:
                value = f"{root}/{value}"
            lines.append(f"        {cmake_key} {cmake_quote(value)}")
        lines.append(")")
        lines.append("")
    return "\n".join(lines)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", dest="case_name", help="Trace case name.")
    parser.add_argument("--ci", action="store_true", help="Only include cases enabled for CI.")
    parser.add_argument("--fixture-root", default="tools/trace_replay/fixtures")
    parser.add_argument(
        "--format",
        choices=("names", "github-apk", "fixture-files", "cmake"),
        default="names",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    cases = load_trace_cases()
    if args.ci:
        cases = ci_trace_cases(cases)
    if args.format == "names":
        print(json.dumps([case["name"] for case in cases], separators=(",", ":")))
    elif args.format == "github-apk":
        print(json.dumps([github_apk_case(case) for case in cases], separators=(",", ":")))
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
