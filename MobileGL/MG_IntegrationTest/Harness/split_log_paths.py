#!/usr/bin/env python3
"""Validate discovered Split log ownership; read only freshly reset control logs."""
import json
from pathlib import Path
import re
import subprocess
import sys


def paths(document):
    owners = {}
    split = {}
    for test in document["tests"]:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        values = [v.split("=", 1)[1] for v in props.get("ENVIRONMENT", [])
                  if v.startswith("MOBILEGL_LOG_FILE_PATH=")]
        is_split = "integration-split" in props.get("LABELS", [])
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
    elif mode == "evidence":
        # argv[5], optional: the control's name, for the failure message. Without it the message
        # is E1's, word for word - scripts/ci/testdata/split_private_log_smoke.sh greps for that
        # sentence, and E1 was the only caller until E3(a) gained a library diagnostic of its own
        # (ID-65: "no library diagnostic exists for block size 0 - x2 adds one").
        label = sys.argv[5] if len(sys.argv) > 5 else ""
        for name, path in selected.items():
            if Path(path).is_file() and re.search(sys.argv[4], Path(path).read_text(errors="replace")):
                print(f"private-log evidence: {name}: {path}")
                return
        if label:
            raise ValueError(f"{label} FAILED: no selected private log carries /{sys.argv[4]}/. "
                             "The library's own line is the only channel for this: ctest's "
                             "transcript is a FALSE ZERO for library output, because the console "
                             "sink is compiled out of the configurations these lanes run. "
                             + ", ".join(f"{n} ({p})" for n, p in selected.items()))
        raise ValueError("E1 FAILED: selected private logs lack expected Fatal{BarrierViolation, \"<slot>\"} line: "
                         + ", ".join(f"{n} ({p})" for n, p in selected.items()))
    else:
        raise ValueError(f"unknown mode: {mode}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f"SplitLogPaths FAILED: {error}")
