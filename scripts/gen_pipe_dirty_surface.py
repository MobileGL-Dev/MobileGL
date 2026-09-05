#!/usr/bin/env python3
# MobileGL - scripts/gen_pipe_dirty_surface.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""The dirty-surface scanner (plan B corollary 4, section 5.2).

MGPipe replaces "the backend rediscovers what changed" with "the frontend says what
changed", which only works if EVERY frontend mutation that a backend can observe bumps an
aggregate generation. The failure mode is silent and one-directional: a mutation that
forgets to bump renders stale, and no purity gate can see it.

So the mutation surface has to be enumerated mechanically rather than by memory. This
script reports every place in MG_Impl/GLImpl where a GL entry point BOTH mutates frontend
state through pGLContext AND reaches the backend in the same function - those are the
publish points, the ones that must map onto an aggregate generation.

P0 is the skeleton: it reports. P1 adds the mapping file and CI regenerates it with
`git diff --exit-code` and zero unmapped mutators, the same shape as gen_pipe.py's G6.

    python3 scripts/gen_pipe_dirty_surface.py            # human-readable report
    python3 scripts/gen_pipe_dirty_surface.py --summary  # counts only
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_ROOT = os.path.join(REPO_ROOT, "MobileGL", "MG_Impl", "GLImpl")

# The mutating half of GLContext's surface. Prefix-matched, per the plan's list.
MUTATOR_PREFIXES = ("Add", "Set", "Mark", "Bump", "Allocate", "Truncate", "Record", "Notify",
                    "Begin", "End")

MUTATOR_RE = re.compile(r"pGLContext->\s*((?:%s)\w*)\s*\(" % "|".join(MUTATOR_PREFIXES))
BACKEND_RE = re.compile(r"gBackendFunctionsTable\.GL\.(\w+)|pActiveBackendObject->\s*(\w+)")
FUNCTION_RE = re.compile(r"(?:^|\n)[ \t]*(?:[A-Za-z_][\w:<>,&*\s]*?)\b(\w+)\s*\([^;{}]*\)\s*"
                         r"(?:const\s*)?(?:noexcept\s*)?\{")


def mask_comments_and_strings(text):
    """Replace comment and string-literal bodies with spaces, keeping every offset and
    newline, so the regexes below cannot match inside a comment or a literal."""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                if i + 1 < n:
                    out[i + 1] = " "
                i += 2
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                if i < n and text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def function_bodies(masked):
    """Yield (name, start_offset, end_offset) for every braced function body."""
    for match in FUNCTION_RE.finditer(masked):
        name = match.group(1)
        start = masked.index("{", match.end() - 1) if masked[match.end() - 1] != "{" else match.end() - 1
        depth = 0
        i = start
        while i < len(masked):
            if masked[i] == "{":
                depth += 1
            elif masked[i] == "}":
                depth -= 1
                if depth == 0:
                    yield name, start, i
                    break
            i += 1


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def scan_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    masked = mask_comments_and_strings(text)
    findings = []
    # Every mutator in the file, whether or not it shares a function with a backend call.
    # The difference between this and the publish points below is the whole point of the
    # report: a mutation that does NOT reach the backend in the same function is published
    # by the NEXT verb, and it is exactly those that need an aggregate generation rather
    # than an inline push.
    all_mutators = [(m.group(1), line_of(masked, m.start())) for m in MUTATOR_RE.finditer(masked)]
    for name, start, end in function_bodies(masked):
        body = masked[start:end]
        mutators = [(m.group(1), line_of(masked, start + m.start())) for m in MUTATOR_RE.finditer(body)]
        if not mutators:
            continue
        backend = sorted(set(m.group(1) or m.group(2) for m in BACKEND_RE.finditer(body)))
        if not backend:
            continue
        findings.append({
            "function": name,
            "line": line_of(masked, start),
            "mutators": mutators,
            "backend": backend,
        })
    return findings, all_mutators


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", action="store_true", help="print the counts only")
    args = parser.parse_args()

    if not os.path.isdir(SCAN_ROOT):
        sys.exit("missing %s" % SCAN_ROOT)

    sources = []
    for root, _, files in os.walk(SCAN_ROOT):
        for name in sorted(files):
            if name.endswith((".cpp", ".h")):
                sources.append(os.path.join(root, name))
    sources.sort()

    total_functions = 0
    total_mutators = 0
    deferred_mutators = 0
    distinct_mutators = {}
    distinct_all = {}
    for path in sources:
        findings, all_mutators = scan_file(path)
        for mutator, _ in all_mutators:
            distinct_all[mutator] = distinct_all.get(mutator, 0) + 1
        deferred_mutators += len(all_mutators)
        if not findings:
            continue
        relative = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
        if not args.summary:
            print("\n%s" % relative)
        for finding in findings:
            total_functions += 1
            total_mutators += len(finding["mutators"])
            for mutator, _ in finding["mutators"]:
                distinct_mutators[mutator] = distinct_mutators.get(mutator, 0) + 1
            if args.summary:
                continue
            print("  %s (line %d) -> backend: %s" % (finding["function"], finding["line"],
                                                     ", ".join(finding["backend"][:4])))
            for mutator, line in finding["mutators"]:
                print("      %-44s :%d  UNMAPPED" % (mutator, line))

    print("\ndirty-surface: %d files scanned under MG_Impl/GLImpl" % len(sources))
    print("dirty-surface: %d mutator calls in total, %d distinct mutators" % (deferred_mutators,
                                                                              len(distinct_all)))
    print("dirty-surface: %d of them sit in %d IMMEDIATE PUBLISH POINTS - functions that also "
          "reach the backend - across %d distinct mutators"
          % (total_mutators, total_functions, len(distinct_mutators)))
    print("dirty-surface: the remaining %d are DEFERRED: nothing reaches the backend in the same "
          "function, so the next verb publishes them, and each one needs an aggregate generation"
          % (deferred_mutators - total_mutators))
    print("dirty-surface: distinct mutators, by call count")
    for mutator in sorted(distinct_all, key=lambda k: (-distinct_all[k], k)):
        print("    %5d  %s%s" % (distinct_all[mutator], mutator,
                                 "  (immediate)" if mutator in distinct_mutators else ""))
    print("dirty-surface: every mutator above is UNMAPPED - the aggregate-generation mapping file "
          "lands in P1, and this report is what it has to cover.")
    print("dirty-surface: known limits of this scanner - it matches braced function bodies "
          "textually, so a mutator inside a lambda is attributed to the enclosing function, and a "
          "mutation published through a helper the entry point calls reads as deferred here.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
