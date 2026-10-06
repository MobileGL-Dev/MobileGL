#!/usr/bin/env python3
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
  * new lanes and new names are reported, never red.

A deliberate retirement edits the baseline in the same commit and names the cases in the commit
message (PLAN-P13 §4.2).

Usage:
  skip_census.py check  <junit-dir> --baseline <json> [--report <json>]
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
    return resolved


def compare(lanes, baseline):
    red, info = [], []
    for lane, known in sorted(baseline.items()):
        current = lanes.get(lane)
        if current is None:
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
    args = parser.parse_args(argv)
    lanes = collect(args.junit_dir)
    if not lanes:
        print(f'::error::skip census: no JUnit under {args.junit_dir}')
        return 1
    print(f'skip census: {summary(lanes)}')
    if args.report:
        args.report.write_text(json.dumps(to_baseline(lanes), indent=1) + '\n')
    if args.command == 'write':
        args.baseline.parent.mkdir(parents=True, exist_ok=True)
        args.baseline.write_text(json.dumps(to_baseline(lanes), indent=1) + '\n')
        print(f'wrote {args.baseline}')
        return 0
    if not args.baseline.exists():
        print(f'::error::skip census: no baseline at {args.baseline}; write one from a full run')
        return 1
    red, info = compare(lanes, resolve_baseline(json.loads(args.baseline.read_text())))
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
