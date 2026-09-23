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

# THE LOOPBACK SUPERVISOR SERVES ONE SESSION AT A TIME (ServerMain.cpp answers a second Hello
# Refuse{Busy}; CONTRACT-P65.md), so every entry that reaches it has to hold the lane's lock.
# P7's D2 TextureUploadShape Tcp entry required the fixture but locked only its private log name:
# under `ctest -L integration-gpu -j 4` it ran beside another Tcp case and whichever client
# connected second died Refuse{Busy} -> Fatal{CapsBeforeFirstSnapshot}. `-L '^integration-tcp$'`
# happened to schedule it next to SupervisorProtocolControls (which runs its own supervisor), so
# the lane that exists to exercise the fixture could not see it. Asserted on the registration, not
# on a lucky schedule.
TCP_FIXTURE = 'mobilegl-tcp'
TCP_LANE_LABEL = 'integration-tcp'


def _list_property(test, name):
    """A json-v1 list property as a list of strings (a ';'-joined string counts as a list)."""
    for prop in test.get('properties', []):
        if prop.get('name') != name:
            continue
        value = prop.get('value')
        if value is None:
            return []
        if isinstance(value, list):
            return [str(item) for item in value]
        return [item for item in str(value).split(';') if item]
    return []


def tcp_lock_violations(document):
    """Entries of a ctest --show-only=json-v1 document that reach the loopback TCP supervisor -
    FIXTURES_REQUIRED names mobilegl-tcp, or the entry is labelled exactly integration-tcp - and do
    not hold RESOURCE_LOCK mobilegl-tcp. Sorted names; empty when the registration is sound."""
    bad = []
    for test in document.get('tests', []):
        reaches = (TCP_FIXTURE in _list_property(test, 'FIXTURES_REQUIRED')
                   or TCP_LANE_LABEL in _list_property(test, 'LABELS'))
        if reaches and TCP_FIXTURE not in _list_property(test, 'RESOURCE_LOCK'):
            bad.append(test.get('name', '<unnamed>'))
    return sorted(bad)


def require_tcp_locks(document):
    bad = tcp_lock_violations(document)
    if bad:
        raise ValueError(f'{len(bad)} entrie(s) reach the one-session TCP supervisor without '
                         f'RESOURCE_LOCK {TCP_FIXTURE}, so `ctest -j` can run them beside another '
                         f'Tcp case and the second client dies Refuse{{Busy}}: ' + ', '.join(bad))


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


def require_entries_passed(path, names):
    """P7 wave 2-F, PH-7 (1). Named entries must have RUN and passed - a SKIP is a failure here.

    `TcpLane.SupervisorProtocolControls` is registered with SKIP_RETURN_CODE 77 because the
    script it runs needs flatc, which is deliberately not in the build graph. That is the right
    behaviour for a developer who has not built one; it is the wrong behaviour for CI, where the
    lane's whole job is to run the supervisor's protocol controls and "skipped" is
    indistinguishable from the three commits the entry did not exist. The workflow builds flatc
    and then names the entry here, so a CI run in which the tool went missing reds by name
    instead of reporting a green lane that measured less than it says.
    """
    seen = {}
    for case in ET.parse(path).getroot().iter('testcase'):
        name = case.get('name')
        if name not in names:
            continue
        if case.find('failure') is not None or case.find('error') is not None:
            seen[name] = 'failed'
        elif case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled'):
            seen[name] = 'skipped'
        else:
            seen[name] = 'passed'
    for name in names:
        state = seen.get(name, 'absent')
        if state != 'passed':
            raise ValueError(f'{name}: required to pass, was {state}')


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
    parser.add_argument('--discovery', help='CTest --show-only=json-v1 output (required for TCP proof); '
                                            'every entry in it that reaches the TCP supervisor must '
                                            'hold RESOURCE_LOCK mobilegl-tcp')
    parser.add_argument('--require-run-ahead', action='store_true',
                        help='Additionally prove actual run-ahead, without fallback or demotion, on each TCP case')
    parser.add_argument('--require-entry-passed', action='append', default=[], metavar='NAME',
                        help='This ctest entry must be present and passed; a SKIP fails (repeatable)')
    args = parser.parse_args()
    if args.prefix in ('DirectGLES.Tcp.', 'DirectGLES.TcpDevice.') and not args.discovery:
        parser.error('TCP arm proof requires --discovery to locate each private role log')
    if args.require_run_ahead and args.prefix not in ('DirectGLES.Tcp.', 'DirectGLES.TcpDevice.'):
        parser.error('--require-run-ahead requires a TCP arm selection')
    try:
        prefix = args.prefix
        if args.discovery:
            require_tcp_locks(json.loads(Path(args.discovery).read_text()))
        passed, failed, skipped = tally(args.junit, prefix=prefix)
        if prefix in ('DirectGLES.Tcp.', 'DirectGLES.TcpDevice.'):
            require_tcp_proof(args.junit, prefix, args.discovery, args.require_run_ahead)
        if args.require_entry_passed:
            require_entries_passed(args.junit, args.require_entry_passed)
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
