#!/usr/bin/env python3
# MobileGL - tools/piglit-android/compare_results.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""Compare two run_piglit_android.py results.json files (e.g. DirectGLES vs
DirectVulkan) and write a markdown report of totals plus categorized diffs."""

import argparse
import json
from collections import Counter
from pathlib import Path

BAD = ('crash', 'timeout', 'fail', 'missing', 'notrun', 'warn')


def load(path):
    d = json.loads(Path(path).read_text())
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('a', help='first results.json')
    ap.add_argument('b', help='second results.json')
    ap.add_argument('-o', '--out', help='markdown output path')
    args = ap.parse_args()

    da, db = load(args.a), load(args.b)
    na, nb = da['backend'], db['backend']
    ta, tb = da['tests'], db['tests']
    names = sorted(set(ta) | set(tb))

    lines = [f'# piglit: {na} vs {nb}', '']
    lines.append(f'| | {na} | {nb} |')
    lines.append('|---|---|---|')
    ca = Counter(o["status"] for o in ta.values())
    cb = Counter(o["status"] for o in tb.values())
    for k in sorted(set(ca) | set(cb)):
        lines.append(f'| {k} | {ca.get(k, 0)} | {cb.get(k, 0)} |')
    lines.append(f'| total | {len(ta)} | {len(tb)} |')
    lines.append(f'| elapsed | {da.get("elapsed_sec")}s | {db.get("elapsed_sec")}s |')
    lines.append('')

    def bucket(pred, title):
        rows = [n for n in names
                if pred(ta.get(n, {}).get('status', 'absent'),
                        tb.get(n, {}).get('status', 'absent'))]
        if rows:
            lines.append(f'## {title} ({len(rows)})')
            lines.append('')
            for n in rows:
                sa = ta.get(n, {}).get('status', 'absent')
                sb = tb.get(n, {}).get('status', 'absent')
                lines.append(f'- `{n}` — {na}: {sa}, {nb}: {sb}')
            lines.append('')

    bucket(lambda a, b: a in BAD and b in BAD, 'Bad on both (likely frontend/state-tracker)')
    bucket(lambda a, b: a in BAD and b == 'pass', f'Bad only on {na}')
    bucket(lambda a, b: a == 'pass' and b in BAD, f'Bad only on {nb}')
    bucket(lambda a, b: a == 'skip' and b == 'pass' or a == 'pass' and b == 'skip',
           'Skip on one side only')

    text = '\n'.join(lines) + '\n'
    if args.out:
        Path(args.out).write_text(text)
        print(f'wrote {args.out}')
    else:
        print(text)


if __name__ == '__main__':
    main()
