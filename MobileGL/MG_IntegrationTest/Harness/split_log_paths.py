#!/usr/bin/env python3
"""Validate discovered Split log ownership; read only freshly reset control logs."""
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def paths(document):
    owners = {}
    split = {}
    for test in document["tests"]:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        values = [v.split("=", 1)[1] for v in props.get("ENVIRONMENT", [])
                  if v.startswith("MOBILEGL_LOG_FILE_PATH=")]
        is_split = test["name"].startswith("DirectGLES.Split.")
        if is_split and (len(values) != 1 or not values[0]):
            raise ValueError(f"{test['name']}: requires exactly one nonempty MOBILEGL_LOG_FILE_PATH")
        for value in values:
            path = str(Path(value).resolve())
            owners.setdefault(path, []).append(test["name"])
            if is_split:
                if not Path(value).is_absolute():
                    raise ValueError(f"{test['name']}: MOBILEGL_LOG_FILE_PATH must be absolute: {value}")
                split[test["name"]] = path
    if not split:
        raise ValueError("integration-split: no entries discovered")
    for name, path in split.items():
        if len(owners[path]) != 1:
            raise ValueError(f"{name}: duplicate MOBILEGL_LOG_FILE_PATH {path}: {owners[path]}")
    return split


def main():
    mode = sys.argv[1]
    if mode == "check":
        data = subprocess.check_output([sys.argv[2], "--test-dir", sys.argv[3],
                                        "--show-only=json-v1"], text=True)
        selected = paths(json.loads(data))
        print(f"SplitLogPaths: {len(selected)} entries, {len(set(selected.values()))} distinct private paths")
        return
    selected = paths(json.loads(Path(sys.argv[2]).read_text()))
    selected = {name: path for name, path in selected.items() if re.search(sys.argv[3], name)}
    if not selected:
        raise ValueError(f"integration-split: empty selection for {sys.argv[3]}")
    if mode == "reset":
        for path in selected.values():
            Path(path).unlink(missing_ok=True)
    elif mode == "results":
        cases = ET.parse(sys.argv[4]).getroot().findall(".//testcase")
        label = sys.argv[5]
        by_name = {}
        for case in cases:
            by_name.setdefault(case.get("name"), []).append(case)
        skipped = sum(any(c.find("skipped") is not None for c in by_name.get(n, []))
                      for n in selected)
        # ID-62: pre-flight Fatal is not evidence that a selected entry ran.
        if skipped:
            raise ValueError(f"{label} control: the knob killed the pre-flight, not the entry - "
                             f"{skipped} selected entries skipped")
        missing = sum(len(by_name.get(n, [])) != 1 or
                      by_name[n][0].get("status") in ("notrun", "disabled") for n in selected)
        if missing:
            raise ValueError(f"{label} control: {missing} selected entries did not run")
        not_failed = sum(c.find("failure") is None or c.get("status") != "fail"
                         for n in selected for c in by_name[n])
        if not_failed:
            raise ValueError(f"{label} control: {not_failed} selected entries did not fail")
    elif mode == "assertion":
        cases = ET.parse(sys.argv[4]).getroot().findall(".//testcase")
        by_name = {case.get("name"): case for case in cases}
        for name in selected:
            case = by_name.get(name)
            output = "" if case is None else " ".join(" ".join(case.itertext()).split())
            if not re.search(sys.argv[5], output):
                raise ValueError(f"E3(a) FAILED: {name} red lacks its persistent-map push diagnostic")
    elif mode == "evidence":
        missing = []
        label = sys.argv[5] if len(sys.argv) > 5 else ""
        for name, path in selected.items():
            if Path(path).is_file() and re.search(sys.argv[4], Path(path).read_text(errors="replace")):
                print(f"private-log evidence: {name}: {path}")
            else:
                missing.append(f"{name} ({path})")
        if missing:
            if label:
                raise ValueError(f"{label} FAILED: no selected private log carries /{sys.argv[4]}/. "
                                 "The library's own line is the only channel for this: ctest's "
                                 "transcript is a FALSE ZERO for library output, because the console "
                                 "sink is compiled out of the configurations these lanes run. "
                                 + ", ".join(missing))
            raise ValueError("E1 FAILED: selected private logs lack expected Fatal{BarrierViolation, \"<slot>\"} line: "
                             + ", ".join(missing))
    else:
        raise ValueError(f"unknown mode: {mode}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, ET.ParseError, subprocess.CalledProcessError) as error:
        sys.exit(f"SplitLogPaths FAILED: {error}")
