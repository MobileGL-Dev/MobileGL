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
import re
import argparse
import importlib.util
import json
from pathlib import Path
import xml.etree.ElementTree as ET


# P6 `t6`: ONE FLAG PER ARM, because the prefix IS the arm and a shared flag would let a lane
# tally the other lane's entries and call itself run. Exit gate 9.3 compares the two.
ARM_PREFIXES = {
    '--require-split-ran': 'DirectGLES.Split.',
    '--require-spawn-ran': 'DirectGLES.Spawn.',
    '--require-tcp-ran': 'DirectGLES.Tcp.',
    '--require-tcp-device-ran': 'DirectGLES.TcpDevice.',
}

TCP_ARM = re.compile(r'control=tcp data=stream server=\S+ pid=[1-9][0-9]*\b')


def require_tcp_proof(path, prefix, discovery, require_run_ahead=False):
    helper_path = Path(__file__).resolve().parents[2] / 'MobileGL/MG_IntegrationTest/Harness/split_log_paths.py'
    spec = importlib.util.spec_from_file_location('split_log_paths', helper_path)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    logs = helper.marker_log_paths(json.loads(Path(discovery).read_text()))
    for case in ET.parse(path).getroot().iter('testcase'):
        if not case.get('name', '').startswith(prefix):
            continue
        if case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled'):
            continue
        # The console sink is compiled out of CI builds, so system-out cannot
        # establish the transport. Read this entry's private role logs instead.
        name = case.get('name')
        if name not in logs:
            raise ValueError(f'{name}: discovery has no private log path')
        output = helper.read_role_logs(logs[name])
        if not TCP_ARM.search(output):
            raise ValueError(f"{case.get('name')}: missing TCP/stream endpoint and server pid arm proof")
        if require_run_ahead and ('run-ahead ARMED' not in output or 'running lockstep' in output
                                  or 'run-ahead DISARMED' in output):
            raise ValueError(f"{case.get('name')}: TCP runtime did not keep requested run-ahead armed")


def tally(path, prefix=None):
    passed = failed = skipped = 0
    for case in ET.parse(path).getroot().iter('testcase'):
        if prefix is not None and not case.get('name', '').startswith(prefix):
            continue
        if case.find('failure') is not None or case.find('error') is not None:
            failed += 1
        elif case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled'):
            skipped += 1
        else:
            passed += 1
    return passed, failed, skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('junit')
    arms = parser.add_mutually_exclusive_group()
    for flag, prefix in ARM_PREFIXES.items():
        arms.add_argument(flag, dest='prefix', action='store_const', const=prefix)
    parser.add_argument('--discovery', help='CTest --show-only=json-v1 output (required for TCP proof)')
    parser.add_argument('--require-run-ahead', action='store_true',
                        help='Additionally prove actual run-ahead, without fallback or demotion, on each TCP case')
    args = parser.parse_args()
    if args.prefix in ('DirectGLES.Tcp.', 'DirectGLES.TcpDevice.') and not args.discovery:
        parser.error('TCP arm proof requires --discovery to locate each private role log')
    if args.require_run_ahead and args.prefix not in ('DirectGLES.Tcp.', 'DirectGLES.TcpDevice.'):
        parser.error('--require-run-ahead requires a TCP arm selection')
    try:
        prefix = args.prefix
        passed, failed, skipped = tally(args.junit, prefix=prefix)
        if prefix in ('DirectGLES.Tcp.', 'DirectGLES.TcpDevice.'):
            require_tcp_proof(args.junit, prefix, args.discovery, args.require_run_ahead)
    except Exception as exc:  # a malformed file is not "zero of everything"
        print(f"junit_tally: cannot prove {args.junit}: {exc}", file=sys.stderr)
        return 1
    print(f"{passed} {failed} {skipped}")
    if prefix is not None and (passed == 0 or failed):
        print(f'{prefix} baseline FAILED: no successful entries with that arm prefix, or an '
              f'already-red selection', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
