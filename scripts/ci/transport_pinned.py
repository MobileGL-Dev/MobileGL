#!/usr/bin/env python3
# MobileGL - scripts/ci/transport_pinned.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""The integration-gpu entries that pin their own transport, which the two integration legs share.

The integration job runs integration-gpu once per leg, and the legs differ only in the
MOBILEGL_TRANSPORT the job exports. An entry whose own ctest ENVIRONMENT sets MOBILEGL_TRANSPORT
(the Split/Spawn/Tcp families and a few monolith-pinned ones) overrides that one difference, so it
runs identically in both legs. The inproc leg leaves those entries, and the fixture entries only
they require, to the monolith-control leg, which every branch runs.

  transport_pinned.py select ALL.json --out PINNED.txt
  transport_pinned.py assert-exclusion ALL.json TRIMMED.json [--out SUMMARY.json]
  transport_pinned.py require-ran ALL.json JUNIT.xml
  transport_pinned.py --self-test

ALL.json and TRIMMED.json are `ctest --show-only=json-v1` without and with `--exclude-from-file`.
select writes the exact names (P, the pinned entries, plus F, their own fixtures) one per line.
assert-exclusion proves the trimmed selection lost exactly P and F, with P non-empty and no pin
left; it prints the counts on every run. require-ran proves the monolith leg ran every one of them.
select and assert-exclusion report a problem as a warning, because their caller then runs every
entry and stays green; require-ran's are errors.
"""
import argparse
import contextlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

ARM_NAME = re.compile(r'^(DirectGLES|DirectVulkan)\.(Split|Spawn|Tcp|TcpDevice)\.')


def props(test):
    return {p['name']: p.get('value') for p in test.get('properties', [])}


def as_list(value):
    if value is None:
        return []
    if isinstance(value, list):
        return [str(v) for v in value]
    return [v for v in str(value).split(';') if v]


def pinned_transport(test):
    for item in as_list(props(test).get('ENVIRONMENT')):
        if item.startswith('MOBILEGL_TRANSPORT='):
            return item.split('=', 1)[1]
    return None


def partition(document, strict=True):
    """(pinned {name: transport}, fixtures {name}) for a json-v1 document. strict raises when an arm
    name and its pin disagree, so the selection is exactly the population the census lists by name."""
    tests = document.get('tests', [])
    pinned = {t['name']: pinned_transport(t) for t in tests if pinned_transport(t) is not None}
    problems = []
    for test in tests if strict else []:
        name, transport = test['name'], pinned.get(test['name'])
        split_pin = transport is not None and transport != 'monolith'
        if bool(ARM_NAME.match(name)) != split_pin:
            problems.append(f'{name}: name and pin disagree (pin {transport!r})')
    if problems:
        raise ValueError('transport pins and arm names disagree: ' + '; '.join(problems[:10]))
    requirers = {}
    for test in tests:
        for fixture in as_list(props(test).get('FIXTURES_REQUIRED')):
            requirers.setdefault(fixture, set()).add(test['name'])
    fixtures = set()
    for test in tests:
        p = props(test)
        provided = as_list(p.get('FIXTURES_SETUP')) + as_list(p.get('FIXTURES_CLEANUP'))
        if not provided:
            continue
        users = set().union(*(requirers.get(f, set()) for f in provided))
        if users and users <= set(pinned):
            fixtures.add(test['name'])
    return pinned, fixtures


def names(document):
    return [t['name'] for t in document.get('tests', [])]


def select(document, strict=True):
    pinned, fixtures = partition(document, strict)
    return sorted(set(pinned) | fixtures)


def assert_exclusion(full, trimmed):
    pinned, fixtures = partition(full)
    expected = set(pinned) | fixtures
    lost = set(names(full)) - set(names(trimmed))
    added = set(names(trimmed)) - set(names(full))
    problems = []
    if not pinned:
        problems.append('no entry pins its transport, so there is nothing to leave to the other leg')
    if not names(trimmed):
        problems.append('the trimmed selection is empty')
    if added:
        problems.append(f'{len(added)} name(s) appeared only in the trimmed selection, e.g. {sorted(added)[0]}')
    if lost != expected:
        extra, kept = sorted(lost - expected), sorted(expected - lost)
        problems.append(f'excluded {len(lost)} name(s), expected {len(expected)}: '
                        f'{len(extra)} unexpected (e.g. {extra[:3]}), {len(kept)} still selected (e.g. {kept[:3]})')
    left = [t['name'] for t in trimmed.get('tests', []) if pinned_transport(t) is not None]
    if left:
        problems.append(f'{len(left)} pinned entr(ies) left in the trimmed selection, e.g. {left[0]}')
    by_pin = {}
    for transport in pinned.values():
        by_pin[transport] = by_pin.get(transport, 0) + 1
    summary = {'pinned': len(pinned), 'by_transport': by_pin, 'fixtures': sorted(fixtures),
               'remaining': len(names(trimmed)), 'problems': problems}
    return summary


def require_ran(document, junit_path):
    expected = set(select(document, strict=False))
    seen = {case.get('name') for case in ET.parse(junit_path).getroot().iter('testcase')}
    return sorted(expected - seen), len(expected)


def self_test():
    def test(name, transport=None, setup=None, cleanup=None, requires=None):
        properties = []
        if transport:
            properties.append({'name': 'ENVIRONMENT', 'value': ['A=1', f'MOBILEGL_TRANSPORT={transport}']})
        if setup:
            properties.append({'name': 'FIXTURES_SETUP', 'value': [setup]})
        if cleanup:
            properties.append({'name': 'FIXTURES_CLEANUP', 'value': [cleanup]})
        if requires:
            properties.append({'name': 'FIXTURES_REQUIRED', 'value': [requires]})
        return {'name': name, 'properties': properties}

    full = {'tests': [
        test('DirectGLES.Plain.A'), test('DirectGLES.Split.A', 'inproc'),
        test('DirectVulkan.Spawn.A', 'spawn'), test('DirectGLES.Tcp.A', 'spawn', requires='tcp'),
        test('DirectGLES.HandleRecycle.A', 'monolith'),
        test('TcpServer.Start', setup='tcp'), test('TcpServer.Stop', cleanup='tcp'),
        test('Shared.Start', setup='shared'), test('DirectGLES.Plain.B', requires='shared'),
    ]}
    expected = ['DirectGLES.HandleRecycle.A', 'DirectGLES.Split.A', 'DirectGLES.Tcp.A',
                'DirectVulkan.Spawn.A', 'TcpServer.Start', 'TcpServer.Stop']
    trimmed = {'tests': [t for t in full['tests'] if t['name'] not in expected]}
    checks = [
        ('select is the pinned entries and their own fixtures', select(full) == expected),
        ('the exact exclusion passes', not assert_exclusion(full, trimmed)['problems']),
        ('a pinned entry left behind fails',
         bool(assert_exclusion(full, {'tests': trimmed['tests'] + [full['tests'][1]]})['problems'])),
        ('an unpinned entry excluded too fails',
         bool(assert_exclusion(full, {'tests': trimmed['tests'][1:]})['problems'])),
        ('an empty trimmed selection fails', bool(assert_exclusion(full, {'tests': []})['problems'])),
        ('no pinned entry at all fails', bool(assert_exclusion(trimmed, trimmed)['problems'])),
    ]
    try:
        partition({'tests': [test('DirectGLES.Split.B')]})
        checks.append(('an arm name without its pin is refused', False))
    except ValueError:
        checks.append(('an arm name without its pin is refused', True))
    try:
        partition({'tests': [test('DirectGLES.Plain.C', 'inproc')]})
        checks.append(('an inproc pin outside the arm names is refused', False))
    except ValueError:
        checks.append(('an inproc pin outside the arm names is refused', True))
    with tempfile.TemporaryDirectory() as tmp:
        junit = Path(tmp) / 'run.xml'
        junit.write_text('<testsuite>' + ''.join(f'<testcase name="{n}"/>' for n in names(full)) + '</testsuite>')
        checks.append(('require-ran passes when the leg ran them all', require_ran(full, junit)[0] == []))
        junit.write_text('<testsuite>' + ''.join(f'<testcase name="{n}"/>' for n in names(trimmed)) + '</testsuite>')
        checks.append(('require-ran fails when the leg lost them', require_ran(full, junit)[0] == expected))
        # The annotation level: the inproc leg falls back and stays green on an exclusion problem.
        (Path(tmp) / 'all.json').write_text(json.dumps(full))
        (Path(tmp) / 'bad.json').write_text(json.dumps({'tests': []}))

        def annotations(argv):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = main([str(Path(tmp) / a) if a.endswith(('.json', '.xml')) else a for a in argv])
            return rc, out.getvalue()
        rc, text = annotations(['assert-exclusion', 'all.json', 'bad.json'])
        checks.append(('an exclusion problem is a warning', rc == 1 and '::warning::' in text and '::error::' not in text))
        rc, text = annotations(['require-ran', 'all.json', 'run.xml'])
        checks.append(('an entry the monolith leg did not run is an error', rc == 1 and '::error::' in text))
        rc, text = annotations(['require-ran', 'all.json', 'missing.xml'])
        checks.append(('a monolith-leg JUnit that cannot be read is an error', rc == 1 and '::error::' in text))
    failed = 0
    for label, ok in checks:
        failed += not ok
        print(f"{'ok' if ok else 'NOT OK':7} {label}")
    print(f"transport_pinned self-test: {'FAILED' if failed else 'ok'}")
    return 1 if failed else 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv == ['--self-test']:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('select')
    p.add_argument('all')
    p.add_argument('--out', required=True)
    p = sub.add_parser('assert-exclusion')
    p.add_argument('all')
    p.add_argument('trimmed')
    p.add_argument('--out')
    p = sub.add_parser('require-ran')
    p.add_argument('all')
    p.add_argument('junit')
    args = parser.parse_args(argv)
    level = 'error' if args.command == 'require-ran' else 'warning'
    try:
        full = json.loads(Path(args.all).read_text())
        if args.command == 'select':
            chosen = select(full)
            Path(args.out).write_text(''.join(name + '\n' for name in chosen))
            print(f'transport_pinned: {len(chosen)} name(s) left to the monolith-control leg')
            return 0
        if args.command == 'assert-exclusion':
            summary = assert_exclusion(full, json.loads(Path(args.trimmed).read_text()))
            if args.out:
                Path(args.out).write_text(json.dumps(summary, indent=1) + '\n')
            print(f"transport_pinned: excluded {summary['pinned']} pinned entr(ies) {summary['by_transport']} "
                  f"and {len(summary['fixtures'])} fixture entr(ies); {summary['remaining']} remain")
            for problem in summary['problems']:
                print(f'::{level}::transport_pinned: {problem}')
            return 1 if summary['problems'] else 0
        missing, total = require_ran(full, args.junit)
        if missing:
            print(f'::error::transport_pinned: the monolith-control leg ran {total - len(missing)} of the '
                  f'{total} entries the inproc leg leaves to it; missing e.g. {missing[:5]}')
            return 1
        print(f'transport_pinned: the monolith-control leg ran all {total} entries the inproc leg leaves to it')
        return 0
    except (OSError, ValueError, ET.ParseError) as error:
        print(f'::{level}::transport_pinned: {error}')
        return 1


if __name__ == '__main__':
    sys.exit(main())
