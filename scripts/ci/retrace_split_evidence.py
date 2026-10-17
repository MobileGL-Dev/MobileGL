#!/usr/bin/env python3
# MobileGL - scripts/ci/retrace_split_evidence.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Check the evidence file a split retrace leg leaves behind.

tools/trace_replay/run_trace_case.cmake writes output/split-evidence.txt only after every
assertion of its MOBILEGL_TRANSPORT block passed: the client log carried the transport marker
(and, under spawn, the separate server pid), a server log existed, no Fatal{ and no MGWIRE-FLOOR
line, and, on the legs that asked for it, draw records on the wire and draws on the server. The
leg's ctest exit status alone cannot tell "the assertions passed" from "the assertions never ran",
so the workflow runs this after ctest: a missing file means the transport block was skipped.

Usage: retrace_split_evidence.py FILE --transport T [--case C] [--backend B] [--require-wire]
"""
import argparse
from pathlib import Path
import sys


def parse(text):
    values = {}
    for line in text.splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values


def count(values, key):
    try:
        return int(values.get(key, ''))
    except ValueError:
        return None


def problems_in(values, transport, case=None, backend=None, require_wire=False):
    problems = []
    if values.get('transport') != transport:
        problems.append(f"transport is '{values.get('transport')}', expected '{transport}'")
    if case is not None and values.get('case') != case:
        problems.append(f"case is '{values.get('case')}', expected '{case}'")
    if backend is not None and values.get('backend') != backend:
        problems.append(f"backend is '{values.get('backend')}', expected '{backend}'")
    if not values.get('marker'):
        problems.append('no transport marker recorded')
    if values.get('server_log') != '1':
        problems.append('no server log recorded')
    for key in ('fatal_lines', 'floor_lines'):
        if count(values, key) != 0:
            problems.append(f"{key} is '{values.get(key)}', expected 0")
    tcp = values.get('control', '-').startswith('tcp://')
    if transport == 'spawn' and values.get('second', '-') == '-':
        problems.append('spawn leg recorded no server-process sentence')
    wire = values.get('wire') == 'ON'
    if require_wire and not wire:
        problems.append('wire evidence was required and the leg did not collect it')
    if wire:
        records = count(values, 'wire_draw_records')
        if records is None or records < 1:
            problems.append(f"wire_draw_records is '{values.get('wire_draw_records')}', expected at least 1")
        draws = values.get('server_draws')
        if not (tcp and draws == 'n/a'):
            if count(values, 'server_draws') is None or count(values, 'server_draws') < 1:
                problems.append(f"server_draws is '{draws}', expected at least 1")
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--transport', required=True)
    parser.add_argument('--case')
    parser.add_argument('--backend')
    parser.add_argument('--require-wire', action='store_true')
    args = parser.parse_args(argv)
    if not args.evidence.is_file():
        print(f'::error::{args.evidence} does not exist: run_trace_case.cmake writes it only after its '
              f'MOBILEGL_TRANSPORT assertions passed, so on this leg they did not run or did not finish')
        return 1
    values = parse(args.evidence.read_text(errors='replace'))
    problems = problems_in(values, args.transport, args.case, args.backend, args.require_wire)
    if problems:
        print(f'::error::{args.evidence}: ' + '; '.join(problems))
        return 1
    print(f"split evidence: {values.get('case')} {values.get('backend')} transport={values.get('transport')} "
          f"second='{values.get('second')}' wire={values.get('wire')} "
          f"wire_draw_records={values.get('wire_draw_records')} server_draws={values.get('server_draws')}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
