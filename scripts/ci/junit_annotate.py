#!/usr/bin/env python3
"""Turn the failed entries of ctest --output-junit files into GitHub error annotations.

A red lane's log needs a signed-in reader, but its annotations do not, so the names of the
failed entries - and the lines of their output that say why - are what a reader of the run
page (or of the check-runs API) can see without downloading anything. GitHub keeps at most
ten error annotations per step, so this prints one summary naming every failed entry and then
one excerpt for each of the first few.

Usage: junit_annotate.py [--excerpts N] FILE_OR_DIR...   (a directory means every *.xml in it)
Exit status is always 0: this reports a failure, it is never one.
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

INTERESTING = re.compile(
    r"Failure|FAILED|Failed|error|Error|Fatal|Refuse|Expected|Which is|Value of|Actual|"
    r"Abort|abort|Segmentation|SIGSEGV|signal|timed out|Timeout|assert", re.ASCII)


def escape(text):
    return text.replace("%", "%25").replace("\r", "").replace("\n", "%0A")


def failed_cases(path):
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as error:
        print(f"junit_annotate: {path}: {error}", file=sys.stderr)
        return []
    out = []
    for case in root.iter("testcase"):
        status = case.get("status", "")
        failure = case.find("failure")
        error = case.find("error")
        if status not in ("fail", "failed") and failure is None and error is None:
            continue
        text = case.findtext("system-out") or ""
        if failure is not None:
            text = (failure.get("message") or "") + "\n" + (failure.text or "") + "\n" + text
        out.append((case.get("name", "?"), text))
    return out


def excerpt(text, limit=40):
    lines = [line.rstrip() for line in text.splitlines() if line.strip()]
    keep = set()
    for index, line in enumerate(lines):
        if INTERESTING.search(line):
            keep.update(range(index, min(index + 4, len(lines))))
    picked = [lines[index] for index in sorted(keep)] or lines[-limit:]
    return "\n".join(line[:400] for line in picked[:limit])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--excerpts", type=int, default=8)
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()
    files = []
    for name in args.paths:
        path = Path(name)
        files.extend(sorted(path.glob("*.xml")) if path.is_dir() else [path])
    failed = []
    for path in files:
        if path.is_file():
            failed.extend((path.name, name, text) for name, text in failed_cases(path))
    if not failed:
        print("junit_annotate: no failed entries in " + ", ".join(str(f) for f in files))
        return 0
    names = "\n".join(f"{name} ({source})" for source, name, _ in failed)
    print(f"::error title={len(failed)} failed ctest entr(y/ies)::{escape(names[:60000])}")
    for source, name, text in failed[:max(args.excerpts, 0)]:
        print(f"::error title={name[:200]}::{escape(excerpt(text))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
