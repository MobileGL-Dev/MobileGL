#!/usr/bin/env python3
# MobileGL - scripts/ci/retrace_verify_reason.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

"""Whether retrace-verify's corrupted-snapshot control went red for its own reason.

The control replays OpenRA with MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters and must go
red with the comparator's divergence on that field: a red without it (another Fatal, a divergence
on another field) proves nothing about the comparator. The check requires that divergence in the
transcript; it does not rule out other failures beside it. run_trace_case.cmake prints the library
log through message(STATUS), which never re-wraps, so each log line stays one transcript line.

Usage: retrace_verify_reason.py TRANSCRIPT    (the control run's ctest -V output)
Exit 0 when the divergence is there, 1 when it is not, 2 when the transcript cannot be read.
"""
import sys

REASON = 'Fatal{PipeVerifyDiffer, "GetRenderStateParameters@'


def carries_reason(text):
    return REASON in text


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__)
        return 2
    try:
        text = open(argv[0], encoding='utf-8', errors='replace').read()
    except OSError as error:
        print(f'::error::retrace_verify_reason: cannot read {argv[0]}: {error}')
        return 2
    if carries_reason(text):
        print(f'retrace_verify_reason: the red carries {REASON}...')
        return 0
    print(f'::error::the corrupted-snapshot retrace went red without {REASON}...: '
          'something other than the comparator failed it')
    return 1


if __name__ == '__main__':
    sys.exit(main())
