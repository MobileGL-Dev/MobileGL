#!/usr/bin/env python3
# MobileGL - scripts/ci/lane_selection.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""One ctest run for several lanes: the exact names of every lane, as one --tests-from-file list.

A lane is a `ctest -L <label regex>` selection. Running the lanes of one job as one ctest
invocation lets a lane that holds a resource lock (the TCP arm runs one entry at a time) share
the run with the others. The lanes keep their own selections: each is discovered with its own
label regex, this script writes the union of the discovered names, ctest runs that list, and
junit_tally.py --select-from/--write-selected carves each lane's JUnit back out, which fails
when a lane's name is missing from the run or reported twice.

  lane_selection.py union OUT.txt LANE=DISCOVERY.json [LANE=DISCOVERY.json ...]
  lane_selection.py --self-test

DISCOVERY.json is `ctest -L <regex> --show-only=json-v1`. union fails when a lane selects
nothing (what --no-tests=error says for a lane run on its own), names an entry twice, or holds a
name a tests-from-file line cannot carry. It prints each lane's count and the size of the union.
"""
import json
from pathlib import Path
import sys
import tempfile


def lane_names(label, path):
    """The names one discovery selects, in order; raises ValueError when the lane is unusable."""
    names = [test['name'] for test in json.loads(Path(path).read_text()).get('tests', [])]
    if not names:
        raise ValueError(f'lane {label}: {path} selects no test')
    if len(names) != len(set(names)):
        twice = sorted({name for name in names if names.count(name) > 1})
        raise ValueError(f'lane {label}: {path} names {len(twice)} entr(ies) twice, e.g. {twice[0]}')
    # ctest reads the list one name per line and trims each line.
    bad = [name for name in names if name != name.strip() or '\n' in name or '\r' in name]
    if bad:
        raise ValueError(f'lane {label}: {len(bad)} name(s) cannot be written one per line, e.g. {bad[0]!r}')
    return names


def union(lanes):
    """(sorted union of names, {label: count}, number of names more than one lane selects)."""
    counts, seen, shared = {}, set(), set()
    for label, path in lanes:
        if label in counts:
            raise ValueError(f'lane {label} is given twice')
        names = lane_names(label, path)
        counts[label] = len(names)
        shared |= seen & set(names)
        seen |= set(names)
    return sorted(seen), counts, len(shared)


def parse_lanes(arguments):
    lanes = []
    for argument in arguments:
        label, separator, path = argument.partition('=')
        if not separator or not label or not path:
            raise ValueError(f'expected LANE=DISCOVERY.json, got {argument!r}')
        lanes.append((label, path))
    if not lanes:
        raise ValueError('no lane given')
    return lanes


def run(argv):
    if len(argv) < 3 or argv[0] != 'union':
        print(__doc__, file=sys.stderr)
        return 2
    try:
        names, counts, shared = union(parse_lanes(argv[2:]))
    except (ValueError, OSError, json.JSONDecodeError) as error:
        print(f'::error::lane_selection.py: {error}')
        return 1
    Path(argv[1]).write_text(''.join(name + '\n' for name in names))
    summary = ', '.join(f'{label} {count}' for label, count in counts.items())
    print(f'lane_selection.py: {len(names)} entr(ies) in one run ({summary}; {shared} in more than one lane)')
    return 0


def self_test():
    import contextlib
    import io

    def quiet(argv):
        # The negative controls print ::error:: lines on purpose; keep them off a green run.
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return run(argv)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        def discovery(name, names):
            path = tmp / name
            path.write_text(json.dumps({'tests': [{'name': n} for n in names]}))
            return str(path)

        out = tmp / 'tests.txt'
        split = discovery('split.json', ['DirectGLES.Split.A', 'DirectGLES.Split.B'])
        spawn = discovery('spawn.json', ['DirectGLES.Spawn.A', 'SpawnLane.Control'])
        tcp = discovery('tcp.json', ['TcpServer.Start', 'DirectGLES.Tcp.A', 'TcpServer.Stop'])
        assert quiet(['union', str(out), f'split={split}', f'spawn={spawn}', f'tcp={tcp}']) == 0
        assert out.read_text().splitlines() == sorted(
            ['DirectGLES.Split.A', 'DirectGLES.Split.B', 'DirectGLES.Spawn.A', 'SpawnLane.Control',
             'TcpServer.Start', 'DirectGLES.Tcp.A', 'TcpServer.Stop']), 'the union is every lane, each name once'
        # A name two lanes select is written once; each lane still carves its own copy.
        shared = discovery('shared.json', ['DirectGLES.Split.A', 'Shared.Fixture'])
        assert quiet(['union', str(out), f'split={split}', f'other={shared}']) == 0
        assert out.read_text().splitlines() == ['DirectGLES.Split.A', 'DirectGLES.Split.B', 'Shared.Fixture']
        out.unlink()
        controls = {
            'a lane that selects nothing': [f'split={split}', f"spawn={discovery('empty.json', [])}"],
            'a lane that names an entry twice': [f"split={discovery('twice.json', ['A', 'A'])}"],
            'a name with a trailing blank': [f"split={discovery('blank.json', ['A '])}"],
            'a name with a line break': [f"split={discovery('break.json', ['A' + chr(10) + 'B'])}"],
            'a lane given twice': [f'split={split}', f'split={spawn}'],
            'a missing discovery file': [f"split={tmp / 'absent.json'}"],
            'an argument without a lane name': [split],
        }
        for label, arguments in controls.items():
            assert quiet(['union', str(out)] + arguments) == 1, f'{label} must fail'
            assert not out.exists(), f'{label} must not leave a list behind'
        assert quiet(['union', str(out)]) == 2, 'no lane is a usage error'
        assert quiet(['nonsense', str(out), f'split={split}']) == 2, 'an unknown command is a usage error'
    print('lane_selection self-test: ok')
    return 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        sys.exit(self_test())
    sys.exit(run(sys.argv[1:]))
