#!/usr/bin/env python3
"""Tally a ctest --output-junit file as "passed failed skipped".

Split out of .github/workflows/test.yml's negative-control step so that the workflow, the local
gate and the control smoke test all count a run the same way.

WHY THIS EXISTS AT ALL (ID-46 finding 8, second half). The counter this replaces lived inline in
the workflow and counted a case as having "run" when it was merely not <skipped/>:

    if case.find('skipped') is None and case.get('status') not in ('notrun', 'disabled'):
        ran += 1

so a case that RAN AND FAILED armed the negative controls below it. Combined with the `|| true`
that hid the baseline's exit code, a lane in which every split entry was already red reported
itself armed, and a control that turns an already-red entry red then "passed". Passed, failed and
skipped are three different answers and the caller needs all three.
"""

import sys
import xml.etree.ElementTree as ET


def tally(path, split_only=False):
    passed = failed = skipped = 0
    for case in ET.parse(path).getroot().iter('testcase'):
        if split_only and not case.get('name', '').startswith('DirectGLES.Split.'):
            continue
        if case.find('failure') is not None or case.find('error') is not None:
            failed += 1
        elif case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled'):
            skipped += 1
        else:
            passed += 1
    return passed, failed, skipped


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != '--require-split-ran'):
        print("usage: junit_tally.py <junit.xml> [--require-split-ran]", file=sys.stderr)
        return 2
    try:
        passed, failed, skipped = tally(sys.argv[1], split_only=len(sys.argv) == 3)
    except Exception as exc:  # a malformed file is not "zero of everything"
        print(f"junit_tally: cannot parse {sys.argv[1]}: {exc}", file=sys.stderr)
        return 1
    print(f"{passed} {failed} {skipped}")
    if len(sys.argv) == 3 and (passed == 0 or failed):
        print('split baseline FAILED: no successful split runtime entries or an already-red selection', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
