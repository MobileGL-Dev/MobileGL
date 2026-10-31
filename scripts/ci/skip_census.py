#!/usr/bin/env python3
# MobileGL - scripts/ci/skip_census.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""The skip census: across every gtest lane of the Test workflow, skips may only shrink.

P13 deletes compile-time macros (MOBILEGL_PIPE_PUSH, MOBILEGL_PIPE_LEGACY_MEMOS) that dozens of
tests guard with GTEST_SKIP or a pull stub. Deleting a macro before its guard turns those tests
"green by skipping", and nothing in a per-lane pass/fail count notices. This gate reads every
lane's ctest JUnit, keys each case by lane and name, and compares the run with a baseline checked
into the tree (scripts/data/skip_census_baseline.json):

  * a case skipped now that the baseline does not list as skipped is RED - including the
    pass -> skip flip, which is the trap the gate exists for;
  * a case the baseline knows that is gone from its lane is RED (names only grow, G14), unless
    the lane is missing entirely, which is reported on its own line;
  * a skip that turned into a pass is reported, never red - rewrite the baseline in the commit
    that earns it so the improvement is locked in;
  * new lanes and new names are reported, never red;
  * a lane whose row carries "optional": "<reason>" runs only on some paths (for example only when
    a build-equality guard falls back); its absence is a note, and when it runs it is checked
    like any other lane;
  * an optional row that also carries "instead_of": "<lane>" is that lane's whole-run fallback:
    when it ran, the lane it stands in for may be absent (a note), and is required otherwise;
  * `check --require LANE` names an optional lane that the run took the path for, so its
    absence is red in that run.

A deliberate retirement edits the baseline in the same commit and names the cases in the commit
message (PLAN-P13 §4.2).

Usage:
  skip_census.py check  <junit-dir> --baseline <json> [--report <json>] [--require LANE]...
  skip_census.py write  <junit-dir> --baseline <json>
  skip_census.py --self-test
<junit-dir> holds one sub-directory per lane (actions/download-artifact with a pattern lays
them out like that); every *.xml under a lane directory counts toward that lane, keyed
<lane>/<xml file stem>.
"""
import argparse
import json
import pathlib
import re
import sys
import tempfile
import xml.etree.ElementTree as ET


# gtest_discover_tests names a value-parameterised case `<name>  # GetParam() = <printed value>`,
# and a printed struct of pointers is raw bytes that move with every build. The census keys a
# case by what it IS, not by where its parameter happened to land.
_PARAM_SUFFIX = re.compile(r'\s+#\s+(?:GetParam|TypeParam)\(\) = .*$', re.S)


def normalize_name(name):
    return _PARAM_SUFFIX.sub('', name)


def case_status(case):
    if case.find('failure') is not None or case.find('error') is not None:
        return 'fail'
    if case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled'):
        return 'skip'
    return 'pass'


def collect(root):
    """{lane: {name: status}} from <root>/<lane>/**/*.xml."""
    root = pathlib.Path(root)
    lanes = {}
    for xml in sorted(root.rglob('*.xml')):
        rel = xml.relative_to(root)
        lane = f'{rel.parts[0]}/{xml.stem}' if len(rel.parts) > 1 else xml.stem
        try:
            cases = ET.parse(xml).getroot().iter('testcase')
        except ET.ParseError as error:
            raise SystemExit(f'skip census: {xml} is not valid JUnit ({error})')
        entries = lanes.setdefault(lane, {})
        for case in cases:
            name = case.get('name')
            if not name:
                continue
            name = normalize_name(name)
            status = case_status(case)
            # A name reported twice in one lane (a rerun) keeps its worst outcome.
            order = {'pass': 0, 'skip': 1, 'fail': 2}
            if name not in entries or order[status] > order[entries[name]]:
                entries[name] = status
    return lanes


def to_baseline(lanes):
    """Lanes whose name set equals an earlier lane's say so (`names_from`) instead of repeating
    it: most gtest lanes run the same few thousand names, and the baseline stays reviewable."""
    out, seen = {}, {}
    for lane, entries in sorted(lanes.items()):
        names = sorted(entries)
        key = tuple(names)
        row = {'skipped': sorted(n for n, s in entries.items() if s == 'skip')}
        if key in seen:
            row['names_from'] = seen[key]
        else:
            row['names'] = names
            seen[key] = lane
        out[lane] = row
    return out


def resolve_baseline(baseline):
    """The inverse of to_baseline's dedupe: every lane gets its own `names`."""
    resolved = {}
    for lane, row in baseline.items():
        names = row['names'] if 'names' in row else baseline[row['names_from']]['names']
        resolved[lane] = {'names': sorted({normalize_name(n) for n in names}),
                          'skipped': sorted({normalize_name(n) for n in row['skipped']})}
        for key in ('optional', 'instead_of'):
            if row.get(key):
                resolved[lane][key] = row[key]
    return resolved


def keep_optional_rows(written, previous):
    """`write` from a run that did not run an optional lane keeps that lane's row, with its names
    resolved, and every optional row keeps its reason. A run where an `instead_of` row stood in
    did not run the lane it replaces, so that lane keeps its previous row too."""
    resolved = resolve_baseline(previous)

    def keep(lane):
        written[lane] = {'skipped': resolved[lane]['skipped'], 'names': resolved[lane]['names']}
    for lane, row in previous.items():
        if not row.get('optional'):
            continue
        if lane not in written:
            keep(lane)
        for key in ('optional', 'instead_of'):
            if row.get(key):
                written[lane][key] = row[key]
    for lane, row in list(written.items()):
        stood_in = row.get('instead_of')
        if stood_in and stood_in not in written and stood_in in previous:
            keep(stood_in)
    return dict(sorted(written.items()))


def compare(lanes, baseline, required=()):
    red, info = [], []
    for lane in sorted(set(required) - set(baseline)):
        red.append(f'lane {lane}: required by this run, but the baseline has no such row')
    stood_in = {row['instead_of']: lane for lane, row in baseline.items()
                if row.get('optional') and row.get('instead_of') and lane in lanes}
    for lane, known in sorted(baseline.items()):
        current = lanes.get(lane)
        if current is None:
            if lane in stood_in:
                info.append(f'lane {lane}: not run ({stood_in[lane]} ran instead)')
                continue
            if known.get('optional') and lane not in required:
                info.append(f"lane {lane}: not run (optional: {known['optional']})")
                continue
            if known.get('optional'):
                red.append(f"lane {lane}: no JUnit in this run, which took the path that runs it "
                           f"({known['optional']})")
                continue
            red.append(f'lane {lane}: no JUnit at all in this run (the job did not run or did not upload)')
            continue
        allowed = set(known['skipped'])
        for name in sorted(n for n, s in current.items() if s == 'skip' and n not in allowed):
            was = 'new' if name not in set(known['names']) else 'pass'
            red.append(f'lane {lane}: {name} SKIPPED (baseline: {was})')
        missing = sorted(set(known['names']) - set(current))
        for name in missing:
            red.append(f'lane {lane}: {name} disappeared (names only grow; retire it in the baseline by name)')
        for name in sorted(n for n in allowed if current.get(n) == 'pass'):
            info.append(f'lane {lane}: {name} now PASSES (was skipped) - tighten the baseline')
        added = sorted(set(current) - set(known['names']))
        if added:
            info.append(f'lane {lane}: {len(added)} new case(s), e.g. {added[0]}')
    for lane in sorted(set(lanes) - set(baseline)):
        info.append(f'lane {lane}: new lane ({len(lanes[lane])} cases) - add it to the baseline')
    return red, info


def summary(lanes):
    total = sum(len(e) for e in lanes.values())
    skipped = sum(1 for e in lanes.values() for s in e.values() if s == 'skip')
    return f'{len(lanes)} lane(s), {total} case(s), {skipped} skipped'


def run(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('check', 'write'))
    parser.add_argument('junit_dir')
    parser.add_argument('--baseline', required=True, type=pathlib.Path)
    parser.add_argument('--report', type=pathlib.Path)
    parser.add_argument('--require', action='append', default=[], metavar='LANE',
                        help='check: an optional lane this run took the path for, so it must be present')
    args = parser.parse_args(argv)
    lanes = collect(args.junit_dir)
    if not lanes:
        print(f'::error::skip census: no JUnit under {args.junit_dir}')
        return 1
    print(f'skip census: {summary(lanes)}')
    if args.report:
        args.report.write_text(json.dumps(to_baseline(lanes), indent=1) + '\n')
    if args.command == 'write':
        written = to_baseline(lanes)
        if args.baseline.exists():
            written = keep_optional_rows(written, json.loads(args.baseline.read_text()))
        args.baseline.parent.mkdir(parents=True, exist_ok=True)
        args.baseline.write_text(json.dumps(written, indent=1) + '\n')
        print(f'wrote {args.baseline}')
        return 0
    if not args.baseline.exists():
        print(f'::error::skip census: no baseline at {args.baseline}; write one from a full run')
        return 1
    red, info = compare(lanes, resolve_baseline(json.loads(args.baseline.read_text())), args.require)
    for line in info:
        print(f'note: {line}')
    for line in red:
        print(f'::error::skip census: {line}')
    print(f'skip census: {"RED" if red else "green"} ({len(red)} finding(s))')
    return 1 if red else 0


def self_test():
    import contextlib
    import io

    def quiet(argv):
        # The negative controls print ::error:: lines on purpose; on a runner those would show
        # as error annotations on a GREEN self-test, so their output is swallowed here.
        with contextlib.redirect_stdout(io.StringIO()):
            return run(argv)

    def junit(path, cases):
        path.parent.mkdir(parents=True, exist_ok=True)
        body = ''.join(
            f'<testcase name="{n}">' + ('<skipped/>' if s == 'skip' else '<failure message="x"/>' if s == 'fail' else '')
            + '</testcase>' for n, s in cases)
        path.write_text(f'<testsuite>{body}</testsuite>')

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        base = tmp / 'base.json'
        good = tmp / 'good'
        junit(good / 'junit-unit' / 'unit.xml', [('A', 'pass'), ('B', 'skip'), ('C', 'pass')])
        assert quiet(['write', str(good), '--baseline', str(base)]) == 0
        assert quiet(['check', str(good), '--baseline', str(base)]) == 0, 'identical run must be green'
        cases = {
            'pass -> skip': ([('A', 'skip'), ('B', 'skip'), ('C', 'pass')], 1),
            'new skipped case': ([('A', 'pass'), ('B', 'skip'), ('C', 'pass'), ('D', 'skip')], 1),
            'case disappeared': ([('A', 'pass'), ('B', 'skip')], 1),
            'skip -> pass': ([('A', 'pass'), ('B', 'pass'), ('C', 'pass')], 0),
            'new passing case': ([('A', 'pass'), ('B', 'skip'), ('C', 'pass'), ('D', 'pass')], 0),
            'failure is not a skip': ([('A', 'fail'), ('B', 'skip'), ('C', 'pass')], 0),
        }
        for label, (run_cases, expected) in cases.items():
            d = tmp / label.replace(' ', '_').replace('>', '')
            junit(d / 'junit-unit' / 'unit.xml', run_cases)
            got = quiet(['check', str(d), '--baseline', str(base)])
            assert got == expected, f'{label}: exit {got}, expected {expected}'
        lost = tmp / 'lost'
        junit(lost / 'junit-other' / 'other.xml', [('A', 'pass')])
        assert quiet(['check', str(lost), '--baseline', str(base)]) == 1, 'a lane that vanished must be red'
        assert quiet(['check', str(tmp / 'empty'), '--baseline', str(base)]) == 1, 'no JUnit must be red'
        # An optional lane: absent is a note, present is checked, and a plain lane stays required.
        optional = tmp / 'optional.json'
        rows = json.loads(base.read_text())
        rows['junit-other/other'] = {'skipped': [], 'names': ['A'], 'optional': 'runs on the fallback only'}
        optional.write_text(json.dumps(rows))
        assert quiet(['check', str(good), '--baseline', str(optional)]) == 0, 'an absent optional lane is a note'
        both = tmp / 'both'
        junit(both / 'junit-unit' / 'unit.xml', [('A', 'pass'), ('B', 'skip'), ('C', 'pass')])
        junit(both / 'junit-other' / 'other.xml', [('A', 'skip')])
        assert quiet(['check', str(both), '--baseline', str(optional)]) == 1, 'a present optional lane is checked'
        assert quiet(['check', str(lost), '--baseline', str(optional)]) == 1, 'a plain lane is still required'
        assert quiet(['check', str(good), '--baseline', str(optional), '--require', 'junit-other/other']) == 1, \
            'an optional lane the run took the path for is required'
        present = tmp / 'present'
        junit(present / 'junit-unit' / 'unit.xml', [('A', 'pass'), ('B', 'skip'), ('C', 'pass')])
        junit(present / 'junit-other' / 'other.xml', [('A', 'pass')])
        assert quiet(['check', str(present), '--baseline', str(optional), '--require', 'junit-other/other']) == 0, \
            'a required optional lane that ran is checked like any other'
        assert quiet(['check', str(good), '--baseline', str(optional), '--require', 'junit-no/such']) == 1, \
            'a required lane the baseline does not know is red'
        kept = keep_optional_rows(to_baseline(collect(good)), rows)
        assert kept['junit-other/other'] == rows['junit-other/other'], 'write keeps an absent optional row'
        # A whole-run fallback lane stands in for a trimmed one: the leg ran everything under its
        # own stem, so the trimmed lane is absent and the full one is held to the full names.
        trimmed = tmp / 'trimmed.json'
        rows = json.loads(base.read_text())
        rows['junit-leg/full'] = {'skipped': ['B'], 'names_from': 'junit-unit/unit'}
        rows['junit-leg/trimmed'] = {'skipped': [], 'names': ['A']}
        rows['junit-leg/trimmed-all'] = {'skipped': ['B'], 'names_from': 'junit-unit/unit',
                                         'optional': 'the fallback', 'instead_of': 'junit-leg/trimmed'}
        trimmed.write_text(json.dumps(rows))

        def leg(name, trimmed_cases=None, all_cases=None):
            d = tmp / name
            junit(d / 'junit-unit' / 'unit.xml', [('A', 'pass'), ('B', 'skip'), ('C', 'pass')])
            junit(d / 'junit-leg' / 'full.xml', [('A', 'pass'), ('B', 'skip'), ('C', 'pass')])
            if trimmed_cases is not None:
                junit(d / 'junit-leg' / 'trimmed.xml', trimmed_cases)
            if all_cases is not None:
                junit(d / 'junit-leg' / 'trimmed-all.xml', all_cases)
            return str(d)
        full_run = [('A', 'pass'), ('B', 'skip'), ('C', 'pass')]
        assert quiet(['check', leg('trim-ok', [('A', 'pass')]), '--baseline', str(trimmed)]) == 0, \
            'the trimmed lane alone is green'
        assert quiet(['check', leg('fallback-ok', None, full_run), '--baseline', str(trimmed)]) == 0, \
            'the fallback lane in place of the trimmed one is green'
        assert quiet(['check', leg('fallback-lost', None, full_run[:2]), '--baseline', str(trimmed)]) == 1, \
            'a name that disappeared from the fallback lane is red'
        assert quiet(['check', leg('fallback-skip', None, [('A', 'skip')] + full_run[1:]),
                      '--baseline', str(trimmed)]) == 1, 'a new skip in the fallback lane is red'
        assert quiet(['check', leg('neither'), '--baseline', str(trimmed)]) == 1, \
            'with neither lane the trimmed one is still required'
        assert keep_optional_rows({}, rows)['junit-leg/trimmed-all']['instead_of'] == 'junit-leg/trimmed', \
            'write keeps instead_of'
        rewritten = tmp / 'rewritten.json'
        rewritten.write_text(trimmed.read_text())
        assert quiet(['write', leg('rewrite-fallback', None, full_run), '--baseline', str(rewritten)]) == 0
        kept = json.loads(rewritten.read_text())
        assert kept.get('junit-leg/trimmed') == {'skipped': [], 'names': ['A']}, \
            'write from a fallback run keeps the row of the lane the fallback stood in for'
        assert kept['junit-leg/trimmed-all']['instead_of'] == 'junit-leg/trimmed'
        assert quiet(['check', leg('rewrite-trim-ok', [('A', 'pass')]), '--baseline', str(rewritten)]) == 0, \
            'the rewritten baseline still passes a normal run'
        assert quiet(['check', leg('rewrite-trim-lost', [('B', 'skip')]), '--baseline', str(rewritten)]) == 1, \
            'the rewritten baseline still reds a name gone from the trimmed lane'
        assert quiet(['check', str(good), '--baseline', str(tmp / 'none.json')]) == 1, 'no baseline must be red'
    assert normalize_name('S/T.Case/Leaf  # GetParam() = 64-byte object <46-CA 08-EB>') == 'S/T.Case/Leaf'
    assert normalize_name('T.Plain') == 'T.Plain'
    twins = {'a/x': {'p': 'pass', 'q': 'skip'}, 'b/x': {'p': 'pass', 'q': 'pass'}}
    packed = to_baseline(twins)
    assert 'names_from' in packed['b/x'] and resolve_baseline(packed)['b/x']['names'] == ['p', 'q']
    print('skip_census self-test: ok')
    return 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        sys.exit(self_test())
    sys.exit(run(sys.argv[1:]))
