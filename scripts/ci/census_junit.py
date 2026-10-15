#!/usr/bin/env python3
# MobileGL - scripts/ci/census_junit.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

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
