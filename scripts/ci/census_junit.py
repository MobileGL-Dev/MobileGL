#!/usr/bin/env python3
"""Summarize the indebted broad lane without turning debt into a hard gate."""
import collections
import sys
import xml.etree.ElementTree as ET

cases = ET.parse(sys.argv[1]).getroot().findall('.//testcase')
if not cases:
    sys.exit('Census FAILED: no executed test records')
counts = collections.Counter()
for case in cases:
    failure = case.find('failure')
    if failure is not None:
        reason = failure.get('message', '')
        status = 'aborted' if 'aborted' in reason.lower() else 'failed'
    elif case.find('skipped') is not None:
        status = 'skipped'
    else:
        status = 'passed'
    counts[status] += 1
print('### Broad inproc census (ID-65: recorded debt, not a gate)')
print(f'CTest exit: {sys.argv[2]}; total: {len(cases)}')
print('\n| passed | skipped | aborted | failed |\n|---:|---:|---:|---:|')
print('| ' + ' | '.join(str(counts[k]) for k in ('passed', 'skipped', 'aborted', 'failed')) + ' |')
