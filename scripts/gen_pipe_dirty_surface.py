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

    python3 scripts/gen_pipe_dirty_surface.py             # human-readable report
    python3 scripts/gen_pipe_dirty_surface.py --summary   # counts only
    python3 scripts/gen_pipe_dirty_surface.py --check     # THE GATE: rc 1 on any hole
    python3 scripts/gen_pipe_dirty_surface.py --self-test # the gate's own negative controls
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


DEF_PATH = os.path.join(REPO_ROOT, "MobileGL", "MG_Pipe", "DirtySurface.def")
TRACKER_PATH = os.path.join(REPO_ROOT, "MobileGL", "MG_Impl", "Pipe", "Tracker.h")

ROW_RE = re.compile(r"^[ \t]*X\((\w+),\s*(\w+)\)\s*\\?\s*$", re.M)
DIRTY_NAME_RE = re.compile(r'^\s*"(NEW_[A-Z0-9_]+)",\s*$', re.M)

# The answers that are not a dirty-bit name. Each one is documented in DirtySurface.def's
# header; a row that uses anything else is a typo, and a typo that read as "mapped" would be
# exactly the silent hole this gate exists to close.
NON_BIT_ANSWERS = ("kImmediate", "kReverseChannel", "kNoBackendRead", "kExplicitDestroy",
                   "kPulledEveryVerb")


def dirty_bit_names():
    """The MGPipeDirty bit names, read out of Tracker.h's kMGPipeDirtyNames so a row cannot
    name a bit that does not exist and a bit cannot be renamed out from under a row. Read
    from the RAW text on purpose: the names are string literals, which is exactly what
    mask_comments_and_strings blanks."""
    with open(TRACKER_PATH, "r", encoding="utf-8", errors="replace") as handle:
        return set(DIRTY_NAME_RE.findall(handle.read()))


def load_mapping(text=None):
    """{mutator: answer} from DirtySurface.def, or from `text` for the self-test."""
    if text is None:
        with open(DEF_PATH, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    rows = {}
    duplicates = []
    for match in ROW_RE.finditer(mask_comments_and_strings(text)):
        mutator, answer = match.group(1), match.group(2)
        if mutator in rows:
            duplicates.append(mutator)
        rows[mutator] = answer
    return rows, duplicates


def check_mapping(mapping, duplicates, scanned, bits):
    """Every problem the gate fails on, as a list of human-readable lines. BOTH directions:
    an unmapped mutator renders stale, and a row naming a mutator the scan no longer finds is
    a stale row that would keep a real hole looking covered."""
    problems = []
    for mutator in sorted(set(scanned) - set(mapping)):
        problems.append("UNMAPPED mutator %s - add a row to MG_Pipe/DirtySurface.def" % mutator)
    for mutator in sorted(set(mapping) - set(scanned)):
        problems.append("STALE row %s - the scan no longer finds this mutator; delete the row"
                        % mutator)
    for mutator in sorted(duplicates):
        problems.append("DUPLICATE row %s" % mutator)
    for mutator in sorted(mapping):
        answer = mapping[mutator]
        if answer in NON_BIT_ANSWERS:
            continue
        if answer in bits:
            continue
        problems.append("BAD answer %s for %s - not a MGPipeDirty bit name and not one of %s"
                        % (answer, mutator, ", ".join(NON_BIT_ANSWERS)))
    return problems


SELF_TEST_WITHHELD = """
#define MGP_DIRTY_SURFACE_LIST(X) \\
    X(RecordError, kReverseChannel)
"""

SELF_TEST_STALE = None  # built from the real def at run time


def scan_all():
    """(findings-per-file, {mutator: call count}) over the whole scan root."""
    sources = []
    for root, _, files in os.walk(SCAN_ROOT):
        for name in sorted(files):
            if name.endswith((".cpp", ".h")):
                sources.append(os.path.join(root, name))
    sources.sort()

    per_file = []
    distinct_all = {}
    for path in sources:
        findings, all_mutators = scan_file(path)
        for mutator, _ in all_mutators:
            distinct_all[mutator] = distinct_all.get(mutator, 0) + 1
        per_file.append((path, findings, all_mutators))
    return sources, per_file, distinct_all


def self_test(scanned, bits):
    """Canned negative controls. Each MUST trip; trips == 0 is an error, which is the shape
    check_include_closure.py and gen_pipe.py --self-test already use."""
    trips = 0
    failures = []

    # 1. a mutator withheld from the def.
    mapping, duplicates = load_mapping(SELF_TEST_WITHHELD)
    problems = check_mapping(mapping, duplicates, scanned, bits)
    if any(p.startswith("UNMAPPED") for p in problems):
        trips += 1
    else:
        failures.append("negative control 1 (a withheld mutator) did NOT trip")

    # 2. a row naming a mutator the scan does not find.
    real, real_duplicates = load_mapping()
    with_ghost = dict(real)
    with_ghost["SetSomethingThatDoesNotExist"] = "kImmediate"
    problems = check_mapping(with_ghost, real_duplicates, scanned, bits)
    if any(p.startswith("STALE") for p in problems):
        trips += 1
    else:
        failures.append("negative control 2 (a stale row) did NOT trip")

    # 3. a row whose answer is neither a dirty bit nor one of the documented non-bit answers.
    with_typo = dict(real)
    with_typo["RecordError"] = "NEW_TYPO_THAT_IS_NOT_A_BIT"
    problems = check_mapping(with_typo, real_duplicates, scanned, bits)
    if any(p.startswith("BAD answer") for p in problems):
        trips += 1
    else:
        failures.append("negative control 3 (a bad answer) did NOT trip")

    for failure in failures:
        print("dirty-surface self-test: %s" % failure)
    if trips == 0:
        print("dirty-surface self-test: NOTHING tripped - the gate cannot fail, which is worse "
              "than a red gate")
        return 1
    if failures:
        return 1
    print("dirty-surface self-test: %d negative controls, all tripped" % trips)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", action="store_true", help="print the counts only")
    parser.add_argument("--check", action="store_true",
                        help="fail when a scanned mutator has no row in DirtySurface.def, or a "
                             "row names a mutator the scan no longer finds")
    parser.add_argument("--self-test", action="store_true",
                        help="run the canned negative controls; each must trip")
    args = parser.parse_args()

    if not os.path.isdir(SCAN_ROOT):
        sys.exit("missing %s" % SCAN_ROOT)
    if not os.path.isfile(DEF_PATH):
        sys.exit("missing %s" % DEF_PATH)

    sources, per_file, distinct_all = scan_all()
    bits = dirty_bit_names()
    if not bits:
        sys.exit("could not read the MGPipeDirty bit names out of %s" % TRACKER_PATH)

    if args.self_test:
        return self_test(distinct_all, bits)

    mapping, duplicates = load_mapping()

    if args.check:
        problems = check_mapping(mapping, duplicates, distinct_all, bits)
        for problem in problems:
            print("dirty-surface: %s" % problem)
        if problems:
            print("dirty-surface: %d problem(s); the mapping must cover every mutator the scan "
                  "finds, in both directions" % len(problems))
            return 1
        print("dirty-surface: %d mutators, all mapped, no stale rows" % len(mapping))
        return 0

    total_functions = 0
    total_mutators = 0
    deferred_mutators = 0
    distinct_mutators = {}
    for path, findings, all_mutators in per_file:
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
                print("      %-44s :%d  %s" % (mutator, line, mapping.get(mutator, "UNMAPPED")))

    print("\ndirty-surface: %d files scanned under MG_Impl/GLImpl" % len(sources))
    print("dirty-surface: %d mutator calls in total, %d distinct mutators" % (deferred_mutators,
                                                                              len(distinct_all)))
    print("dirty-surface: %d of them sit in %d IMMEDIATE PUBLISH POINTS - functions that also "
          "reach the backend - across %d distinct mutators"
          % (total_mutators, total_functions, len(distinct_mutators)))
    print("dirty-surface: the remaining %d are DEFERRED: nothing reaches the backend in the same "
          "function, so the next verb publishes them, and each one needs an aggregate generation"
          % (deferred_mutators - total_mutators))
    print("dirty-surface: distinct mutators, by call count, with what publishes each")
    for mutator in sorted(distinct_all, key=lambda k: (-distinct_all[k], k)):
        print("    %5d  %-42s %s%s" % (distinct_all[mutator], mutator,
                                       mapping.get(mutator, "UNMAPPED"),
                                       "  (immediate)" if mutator in distinct_mutators else ""))
    unmapped = sorted(set(distinct_all) - set(mapping))
    if unmapped:
        print("dirty-surface: %d UNMAPPED - run --check, which is a gate since P2" % len(unmapped))
    else:
        print("dirty-surface: every mutator above is mapped (MG_Pipe/DirtySurface.def); --check "
              "is a gate and --self-test proves it can fail")
    print("dirty-surface: known limits of this scanner - it matches braced function bodies "
          "textually, so a mutator inside a lambda is attributed to the enclosing function, and a "
          "mutation published through a helper the entry point calls reads as deferred here.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
