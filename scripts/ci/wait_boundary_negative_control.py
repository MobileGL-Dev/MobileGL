#!/usr/bin/env python3
"""E1: green/red/restored-green proof of real ClientSession wait boundaries.

The peer holds apply until the actual transport parks or the emit call returns.
VERB_BARRIER=0 must fail only the two owed waits; ordinary run-ahead draws need
not fail. Each arm is checked from its own CTest JUnit testcase, never a global
grep, stale library log, arbitrary nonzero exit, or timing-dependent pixel.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

PREFIX = "RemoteWaitBoundaryControl."
CASES = {
    "AppliedClassWaitsWithRunAheadCap": ("GenerateMipmap", 1, True,
        "E1 wait boundary: GenerateMipmap returned before apply"),
    "MissingServerCapKeepsClearLockstep": ("Clear", 0, True,
        "E1 wait boundary: Clear without server cap returned before apply"),
    "ServerCapAllowsClearToRunAhead": ("Clear", 1, False, ""),
}


def own_negative(text, expected_observation, diagnostic):
    return expected_observation in text and diagnostic in text


def inspect(path, selected, barrier, negative, rc):
    root = ET.parse(path).getroot()
    cases = root.findall(".//testcase")
    names = [case.get("name") for case in cases]
    if len(names) != len(set(names)) or set(names) != set(selected):
        raise ValueError("selected testcase names are missing, duplicated or unexpected")
    for case in cases:
        name = case.get("name")
        if case.find("skipped") is not None or case.get("status") in {"notrun", "disabled"}:
            raise ValueError(f"{name}: selected testcase skipped or did not run")
        failed = case.find("failure") is not None or case.find("error") is not None
        op, cap, waits, diagnostic = CASES[name.removeprefix(PREFIX)]
        waited = int(waits and barrier == 1)
        observation = (f"E1 observation: op={op} cap={cap} barrier={barrier} parked={waited} "
                       f"applied_before_return={waited} emitted=1 peer_seen=1")
        text = "\n".join(case.itertext())
        if negative:
            if not failed or rc == 0:
                raise ValueError(f"{name}: disabled barrier did not fail the owed wait")
            if not own_negative(text, observation, diagnostic):
                raise ValueError(f"{name}: red lacks its own observed wait-boundary failure")
        elif failed or rc != 0 or observation not in text:
            raise ValueError(f"{name}: baseline/restored arm did not execute its green boundary proof")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctest", default=os.environ.get("CTEST", "ctest"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    all_cases = [PREFIX + name for name in CASES]
    negative_cases = [PREFIX + name for name, (_, _, waits, _) in CASES.items() if waits]
    ok = True
    for arm, barrier, negative, selected in (
        ("baseline", 1, False, all_cases),
        ("control", 0, True, negative_cases),
        ("restored", 1, False, all_cases),
    ):
        # Never run a negative arm against a failed baseline. A failed negative
        # still proceeds through restoration and must not consume stale XML.
        if arm == "control" and not ok:
            continue
        result = args.out / f"e1-{arm}.xml"
        result.unlink(missing_ok=True)
        env = os.environ.copy()
        env["MOBILEGL_IPC_VERB_BARRIER"] = str(barrier)
        selector = "^(" + "|".join(re.escape(name) for name in selected) + ")$"
        command = [args.ctest, "-L", "unit", "-R", selector, "--no-tests=error",
                   "--output-on-failure", "--output-junit", str(result)]
        try:
            run = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=90)
            (args.out / f"e1-{arm}.out").write_text(run.stdout, encoding="utf-8")
            print(run.stdout, end="", flush=True)
            inspect(result, selected, barrier, negative, run.returncode)
            print(f"E1 {arm}: {'EXPECTED_RED' if negative else 'GREEN'} ({len(selected)} cases)", flush=True)
        except (OSError, ValueError, ET.ParseError, subprocess.TimeoutExpired) as exc:
            print(f"E1 FAILED: {arm}: {exc}", file=sys.stderr, flush=True)
            ok = False
    if ok:
        print("negative control E1: owed waits failed by their own observed boundary assertions; baseline and restoration green")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
