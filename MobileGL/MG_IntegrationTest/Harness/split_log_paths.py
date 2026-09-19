#!/usr/bin/env python3
"""Validate discovered Split log ownership; read only freshly reset control logs."""
import os
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


# P5e (gl), ID-119: THE MARKER GRAMMAR, PARSED WHERE IT IS WRITTEN.
#
# MG_Backend/MGPipe/PipeInputs.cpp writes exactly two shapes and they differ only in the leading
# tag and the reason, which is what lets every filter written since P5c keep meaning "red":
#
#   MGPipe: Fatal{UnmigratedPipeInput,    "<F>@<V>"} [BARRIER-PULLED, <why>, retires in <phase>]
#   MGPipe: Admitted{UnmigratedPipeInput, "<F>@<V>"} [BARRIER-PULLED, ADMITTED[-ESCALATED], retires in <phase>]
MARKER_RE = re.compile(
    r"(Fatal|Admitted)\{UnmigratedPipeInput,\s*\"([^\"@]+)@([^\"]+)\"\}"
    r"(?:\s*\[BARRIER-PULLED,\s*([^,\]]+))?")


def marker_log_paths(document):
    """{entry: absolute MOBILEGL_LOG_FILE_PATH} for EVERY entry that declares one.

    P5e (gl), ID-119: THE LOG SET COMES FROM ctest, NOT FROM A DIRECTORY. The CI step used to
    grep MobileGL/MG_IntegrationTest/split-logs/, which holds 90 of the lane's entries. The ones
    it missed were the F1. readback block, both NamedBlit pairs, the four Ct. entries and
    PersistentMapArm - i.e. precisely the readback population the allowlist is ABOUT, plus the
    only DirectVulkan entries in the lane. A directory is a guess about where the lane put its
    logs; `ctest --show-only=json-v1` is the lane saying where it put them.

    Unlike paths() above this does not restrict itself to DirectGLES.Split. entries and does not
    validate ownership - that is paths()' job and SplitLogPaths.PrivateAndDistinct runs it. This
    one answers "which files might carry a marker", for every backend prefix the lane has."""
    logs = {}
    for test in document["tests"]:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        values = [v.split("=", 1)[1] for v in props.get("ENVIRONMENT", [])
                  if v.startswith("MOBILEGL_LOG_FILE_PATH=")]
        if len(values) != 1 or not values[0]:
            continue
        logs[test["name"]] = str(Path(values[0]).resolve())
    return logs


def read_pair_set(path):
    """A committed `<field>@<verb>` set, one per line; blank lines and # comments ignored."""
    pairs = set()
    for line in Path(path).read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            pairs.add(line)
    return pairs


def classify_markers(logs):
    """(fatal, admitted, escalated, scanned) - each a {pair: sorted entry names}.

    Counted BY ENTRY and not by line: the runtime dedupes each (field, verb) once per process,
    so a line count would only ever say how many processes ran."""
    fatal, admitted, escalated = {}, {}, {}
    scanned = []
    for name in sorted(logs):
        path = logs[name]
        if not Path(path).is_file():
            continue
        scanned.append(name)
        text = Path(path).read_text(errors="replace")
        for tag, field, verb, why in MARKER_RE.findall(text):
            pair = f"{field}@{verb}"
            if tag == "Fatal":
                fatal.setdefault(pair, set()).add(name)
            elif (why or "").strip() == "ADMITTED-ESCALATED":
                escalated.setdefault(pair, set()).add(name)
            else:
                admitted.setdefault(pair, set()).add(name)
    return ({p: sorted(e) for p, e in fatal.items()},
            {p: sorted(e) for p, e in admitted.items()},
            {p: sorted(e) for p, e in escalated.items()},
            scanned)


def print_marker_table(title, table):
    print(f"  {title}: {len(table)} distinct pair(s)")
    for pair in sorted(table, key=lambda p: (-len(table[p]), p)):
        print(f"    {len(table[pair]):4d}  {pair}")


def expect_fatal(document, junit_path, expected_path):
    """THE DUAL-BLOCK LANE'S CENSUS AND TWO-SIDED RATCHET (P5f f1, P5F §4/§6).

    The markers mode above ratchets the strict lane's ADMITTED pairs; the dual-block lane's
    currency is the FATAL pair, because under MOBILEGL_IPC_ROLE_SPLIT_STATE=1 every
    BARRIER-PULLED read aborts unconditionally (CountBarrierPull's dual-block arm) - the red
    IS the list of fields that still read the other role's memory.

    Three checks, all load-bearing:

      1. EVERY failed entry with a private log carries at least one Fatal{UnmigratedPipeInput}
         marker in it. A red entry without one is an UNNAMED crash - a segfault or an abort
         from something else - which is precisely what the rehearsal must make impossible
         (the O-class fields have no null check; without the Fatal arm they die nameless).
      2. No ADMITTED marker anywhere: under the dual-block arm the Fatal fires before the
         admission question is asked, so an Admitted line means the arm did not fire.
      3. The Fatal pair set equals the committed expected file, BOTH ways - a new pair is a
         debt somebody now owes, a vanished pair means a later f package retired it and the
         file must lose the row in the same commit (the strict file's rule, ID-119).

    A green entry needs no marker and asserts nothing; the lane turning fully green with an
    empty expected file IS the P5f exit state, which this mode then verifies rather than
    obstructs."""
    logs = marker_log_paths(document)
    cases = ET.parse(junit_path).getroot().findall(".//testcase")
    by_name = {}
    for case in cases:
        by_name.setdefault(case.get("name"), []).append(case)

    problems = []
    fatal, admitted, escalated = {}, {}, {}
    scanned = 0
    skipped = 0
    green = 0
    red = 0
    for name, entries in sorted(by_name.items()):
        if any(c.find("skipped") is not None for c in entries):
            skipped += 1
            continue
        failed = any(c.find("failure") is not None or c.get("status") == "fail"
                     for c in entries)
        if not failed:
            green += 1
            continue
        red += 1
        path = logs.get(name)
        if path is None:
            problems.append(f"{name} is RED and declares no MOBILEGL_LOG_FILE_PATH, so the "
                            "red cannot be shown to be a named Fatal - an unnamed crash is "
                            "exactly what this lane exists to forbid")
            continue
        if not Path(path).is_file():
            problems.append(f"{name} is RED and its private log {path} does not exist - the "
                            "process died before it could write one, which is an unnamed crash")
            continue
        scanned += 1
        text = Path(path).read_text(errors="replace")
        entry_fatal = set()
        for tag, field, verb, why in MARKER_RE.findall(text):
            pair = f"{field}@{verb}"
            if tag == "Fatal":
                entry_fatal.add(pair)
                fatal.setdefault(pair, set()).add(name)
            elif (why or "").strip() == "ADMITTED-ESCALATED":
                escalated.setdefault(pair, set()).add(name)
            else:
                admitted.setdefault(pair, set()).add(name)
        if not entry_fatal:
            problems.append(f"{name} is RED but its private log carries no "
                            "Fatal{UnmigratedPipeInput} marker - an unnamed failure, not a "
                            "census entry")

    print(f"SplitLogPaths expect-fatal: {len(by_name)} entrie(s) in the run - "
          f"{green} green, {skipped} skipped, {red} red ({scanned} with a private log scanned)")
    print_marker_table("Fatal{UnmigratedPipeInput", fatal)
    if admitted or escalated:
        problems.append("%d Admitted marker(s) under the dual-block knob - the unconditional "
                        "Fatal arm in CountBarrierPull did not fire first: %s"
                        % (len(admitted) + len(escalated),
                           ", ".join(sorted(set(admitted) | set(escalated)))))

    expected = read_pair_set(expected_path)
    observed = set(fatal)
    appeared = sorted(observed - expected)
    vanished = sorted(expected - observed)
    if appeared:
        problems.append("%d Fatal pair(s) the lane has not seen before: %s. Each is a field "
                        "that reads the other role's memory - add it to %s with the package "
                        "that retires it, or retire it."
                        % (len(appeared), ", ".join(appeared), Path(expected_path).name))
    if vanished:
        problems.append("%d expected pair(s) no longer appear: %s. Remove them from %s in the "
                        "commit that retired them - an expected set that keeps rows nothing "
                        "writes any more is how this lane rots green."
                        % (len(vanished), ", ".join(vanished), Path(expected_path).name))
    if problems:
        raise ValueError("the dual-block lane's fatal ratchet: " + " | ".join(problems))
    print(f"SplitLogPaths expect-fatal: ratchet OK - {len(observed)} fatal pair(s), "
          f"zero admitted, every red entry named")


def markers(document, selector, allowed_path, expected_path, require_no_fatal):
    """THE TWO-SIDED RATCHET (ID-119).

    One side: a marker that is not admitted fails the lane. That half existed in spirit but was
    unreachable code, because strict aborted on every barrier-pulled read and the run's rc took
    the step out before the comparison (ID-117 fixed the knob; this reads the result).

    The other side, which is the new half: an admitted pair that NO LONGER APPEARS must be
    removed from the expected set. Without it the lane rots green - a debt retires, its marker
    stops being written, and the expected list quietly becomes a list of things that used to
    happen, so the next regression to re-introduce one of them reads as "expected"."""
    logs = {name: path for name, path in marker_log_paths(document).items()
            if re.search(selector, name)}
    if not logs:
        raise ValueError(f"no lane entry matching /{selector}/ declares a "
                         "MOBILEGL_LOG_FILE_PATH, so there is nothing to read markers out of")
    fatal, admitted, escalated, scanned = classify_markers(logs)
    print(f"SplitLogPaths markers: {len(scanned)} of {len(logs)} selected entries wrote a log")
    print_marker_table("Fatal{UnmigratedPipeInput", fatal)
    print_marker_table("Admitted{UnmigratedPipeInput", admitted)
    print_marker_table("Admitted{UnmigratedPipeInput (ESCALATED)", escalated)
    if not scanned:
        raise ValueError("not one selected entry wrote its private log: the marker census is "
                         "EMPTY because nothing was read, which is not the same statement as "
                         "'no marker fired' and must never be reported as one")

    problems = []
    if require_no_fatal and fatal:
        problems.append("%d Fatal marker(s) - each is a field an apply still reads out of client "
                        "memory with nothing admitting it: %s"
                        % (len(fatal), "; ".join(f"{p} ({len(e)} entries, e.g. {e[0]})"
                                                 for p, e in sorted(fatal.items()))))

    # An ADMITTED (non-escalated) marker claims the GENERATED table admitted it, so it must be in
    # that table. An ADMITTED-ESCALATED one does not: it is a runtime fact about the record's
    # payload (an open XFB span, a draw with client arrays) that no table indexed by
    # (field, verb) can carry, so looking for it in a static list would mean widening that list
    # with every pair that could ever escalate - which would forgive the ordinary draw path too.
    allowed = read_pair_set(allowed_path)
    outside = sorted(set(admitted) - allowed)
    if outside:
        problems.append("%d marker(s) tagged ADMITTED are not in the generated allowlist, so the "
                        "committed PipeFieldOwnership.inc and the generator disagree: %s"
                        % (len(outside), ", ".join(outside)))

    expected = read_pair_set(expected_path)
    observed = set(admitted) | set(escalated)
    appeared = sorted(observed - expected)
    vanished = sorted(expected - observed)
    if appeared:
        problems.append("%d admitted pair(s) the lane has not seen before: %s. Each is a debt "
                        "somebody now owes - add it to %s with the phase that retires it, or "
                        "retire it."
                        % (len(appeared), ", ".join(appeared), Path(expected_path).name))
    if vanished:
        problems.append("%d expected pair(s) no longer appear: %s. Remove them from %s in the "
                        "commit that retired them - an expected set that keeps rows nothing "
                        "writes any more is how this lane rots green."
                        % (len(vanished), ", ".join(vanished), Path(expected_path).name))
    if problems:
        raise ValueError("the strict lane's marker ratchet: " + " | ".join(problems))
    print(f"SplitLogPaths markers: ratchet OK - {len(observed)} admitted pair(s), "
          f"{len(fatal)} fatal pair(s)")


def main():
    mode = sys.argv[1]
    if mode == "check":
        data = subprocess.check_output([sys.argv[2], "--test-dir", sys.argv[3],
                                        "--show-only=json-v1"], text=True)
        selected = paths(json.loads(data))
        print(f"SplitLogPaths: {len(selected)} entries, {len(set(selected.values()))} distinct private paths")
        return
    if mode == "markers":
        markers(json.loads(Path(sys.argv[2]).read_text()), sys.argv[3], sys.argv[4], sys.argv[5],
                len(sys.argv) > 6 and sys.argv[6] == "no-fatal")
        return
    if mode == "expect-fatal":
        expect_fatal(json.loads(Path(sys.argv[2]).read_text()), sys.argv[3], sys.argv[4])
        return
    selected = paths(json.loads(Path(sys.argv[2]).read_text()))
    selected = {name: path for name, path in selected.items() if re.search(sys.argv[3], name)}
    # The SAME exclusion run_control hands ctest -E. Without it this helper and ctest disagree
    # about the subject set, and the helper reports an entry ctest never ran as "did not run"
    # (P5e, ID-122). ctest -R is POSIX ERE and cannot express the exclusion inline.
    _excl = os.environ.get("SPLIT_LOG_EXCLUDE", "")
    if _excl:
        selected = {n: q for n, q in selected.items() if not re.search(_excl, n)}
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
