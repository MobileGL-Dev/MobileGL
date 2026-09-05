#!/usr/bin/env python3
# MobileGL - scripts/check_doc_citations.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""Resolve every `path:line` citation in a set of markdown documents.

A design document that cites the code is only as good as its line numbers, and a wrong one
is worse than none: it sends the next reader to a function that does something else. The
disaggregation plan's first draft cited SamplerObject.h:468-492 for a struct that lives at
:72-96 in a 160-line file, and nothing caught it.

So every `File.h:123` and `File.cpp:123-456` in the given documents is resolved against a
git revision - the file must exist there and must have at least that many lines. Bare file
names are resolved by basename, which is how the plan spells most of its citations; an
ambiguous basename is reported rather than guessed.

    python3 scripts/check_doc_citations.py docs/Disaggregated/*.md
    python3 scripts/check_doc_citations.py --rev 81b17c0b --strict docs/Disaggregated/*.md

Exits non-zero only with --strict, so it can be wired into CI as a warning first.
"""

import argparse
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# `Managers.cpp:4340-4390`, `MobileGL/MG_State/.../RenderState.h:263`, `:12-13` is NOT
# matched on purpose: a citation with no file name cannot be checked, only guessed.
CITATION_RE = re.compile(
    r"(?<![\w/.-])((?:[\w.-]+/)*[\w.-]+\.(?:h|hpp|cpp|cc|c|py|def|inc|md|json|yml|yaml|txt|fbs))"
    r":(\d+)(?:-(\d+))?")


def git(args, rev_root=REPO_ROOT):
    return subprocess.run(["git", "-C", rev_root] + args, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False).stdout.decode("utf-8", "replace")


class Tree(object):
    def __init__(self, rev):
        self.Rev = rev
        listing = git(["ls-tree", "-r", "--name-only", rev]).splitlines()
        if not listing:
            sys.exit("check_doc_citations: revision %s has no files (bad revision?)" % rev)
        self.Paths = set(listing)
        self.ByBasename = {}
        for path in listing:
            self.ByBasename.setdefault(os.path.basename(path), []).append(path)
        self.LineCounts = {}

    def Resolve(self, cited):
        if cited in self.Paths:
            return [cited]
        candidates = self.ByBasename.get(os.path.basename(cited), [])
        if len(candidates) > 1 and "/" in cited:
            candidates = [p for p in candidates if p.endswith(cited)] or candidates
        return candidates

    def LineCount(self, path):
        if path not in self.LineCounts:
            blob = git(["show", "%s:%s" % (self.Rev, path)])
            # A file with no trailing newline still has that last line.
            count = blob.count("\n") + (1 if blob and not blob.endswith("\n") else 0)
            self.LineCounts[path] = count
        return self.LineCounts[path]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", nargs="+", help="markdown files to check")
    parser.add_argument("--rev", default="HEAD", help="git revision the citations point into")
    parser.add_argument("--strict", action="store_true", help="exit 1 when a citation does not resolve")
    args = parser.parse_args()

    tree = Tree(args.rev)
    checked = 0
    problems = []
    for document in args.documents:
        if not os.path.exists(document):
            problems.append("%s: no such document" % document)
            continue
        with open(document, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
        for number, line in enumerate(lines, start=1):
            for match in CITATION_RE.finditer(line):
                cited, first, last = match.group(1), int(match.group(2)), match.group(3)
                last = int(last) if last else first
                checked += 1
                where = "%s:%d: `%s`" % (document, number, match.group(0))
                candidates = tree.Resolve(cited)
                if not candidates:
                    problems.append("%s -> no such file at %s" % (where, args.rev))
                    continue
                if len(candidates) > 1:
                    problems.append("%s -> ambiguous: %s" % (where, ", ".join(sorted(candidates))))
                    continue
                if last < first:
                    problems.append("%s -> inverted line range" % where)
                    continue
                count = tree.LineCount(candidates[0])
                if last > count:
                    problems.append("%s -> %s has %d lines at %s"
                                    % (where, candidates[0], count, args.rev))

    print("check_doc_citations: %d citations in %d document(s) against %s, %d problem(s)"
          % (checked, len(args.documents), args.rev, len(problems)))
    for problem in problems:
        print("  %s" % problem)
    if problems and args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
