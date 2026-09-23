#!/usr/bin/env python3
"""P7 wave 2 package B3: WireDeclines.def rows and their sites must agree.

CONTRACT-P7 §0 rule I admits two shapes for a wire-arm refusal, a decline and a named Fatal.
The decline half is only real if:

  (1) every row in WireDeclines.def is actually counted somewhere - a row with no site is a
      name that can never move, which reads in a report as "this cannot happen here" when it
      only means "nobody wired it up"; and
  (2) every site emits a line - a counter with no log puts the next reader back on a bisect,
      which is the thing the package exists to remove.

Rows are APPEND-ONLY (the lanes assert specific counters), so (1) cannot be fixed later by
deleting a row once a release has shipped it. This check is why it cannot rot.

Sites reached through MGL_WIRE_DECLINE_AT satisfy both at once. A bare
WireDeclineTally::Count(WireDeclineSite::X) is allowed ONLY when an MGLOG_ statement carrying
the same reason already stands just above it.

Sources are matched on their text with comments and string/character literals blanked out
(newlines kept, so every report cites the real line): a site or a log that is only a comment,
or only a string, is not one.

usage: wire_declines_audit.py [repo-root]
"""
import os
import re
import sys

RENDERER = "MobileGL/MG_Backend/DirectVulkan/Renderer"
DEF = os.path.join(RENDERER, "WireDeclines.def")
SOURCES = (".cpp", ".inc", ".h")
# How far above a bare Count() an MGLOG_ may stand and still be "the line for this site".
LOG_WINDOW = 8

ROW = re.compile(r"^\s*MGL_WIRE_DECLINE\(([A-Za-z][A-Za-z0-9]*)\)\s*$")
AT_SITE = re.compile(r"MGL_WIRE_DECLINE_AT\(\s*([A-Za-z][A-Za-z0-9]*)\s*,")
COUNT_SITE = re.compile(r"WireDeclineTally::Count\(\s*WireDeclineSite::([A-Za-z][A-Za-z0-9]*)\s*\)")
# Warning or error only: an MGLOG_D / MGLOG_V is compiled away in the Release builds that ship,
# so it is not a line anybody can read off a device.
LOGGED = re.compile(r"MGLOG_[WE](_ONCE)?\s*\(")
RAW_PREFIX = re.compile(r"(?:^|[^A-Za-z0-9_])(?:u8|u|U|L)?R$")


def strip_code(text: str) -> str:
    """Blank out //, /* */ comments and string / character literals, keeping every newline.

    What remains lines up with the source line for line, so a match's index is a real line
    number. Raw strings R"d(...)d" and C++14 digit separators (1'000) are handled."""
    out = list(text)
    n = len(text)
    i = 0

    def blank(a: int, b: int) -> None:
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = i
            while j < n and text[j] != "\n":
                # A backslash-newline continues a // comment onto the next line.
                if text[j] == "\\" and j + 1 < n and text[j + 1] == "\n":
                    j += 2
                    continue
                j += 1
            blank(i, j)
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c == '"' and RAW_PREFIX.search(text[max(0, i - 3):i]):
            open_paren = text.find("(", i + 1)
            if open_paren < 0:
                blank(i, n)
                break
            delim = text[i + 1:open_paren]
            close = text.find(")" + delim + '"', open_paren + 1)
            j = n if close < 0 else close + len(delim) + 2
            blank(i, j)
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_"))):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            blank(i, j)
            i = j
        else:
            i += 1
    return "".join(out)


def main() -> int:
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    def_path = os.path.join(root, DEF)
    if not os.path.isfile(def_path):
        print("wire-declines: %s not found" % DEF, file=sys.stderr)
        return 2

    rows = []
    for line in open(def_path, encoding="utf-8"):
        m = ROW.match(line)
        if m:
            rows.append(m.group(1))
    if len(rows) != len(set(rows)):
        dupes = sorted({r for r in rows if rows.count(r) > 1})
        print("wire-declines: duplicate rows: %s" % ", ".join(dupes), file=sys.stderr)
        return 1

    counted = {}          # name -> [(file, line)]
    unlogged_counts = []  # (file, line, name)
    src_dir = os.path.join(root, RENDERER)
    for entry in sorted(os.listdir(src_dir)):
        if not entry.endswith(SOURCES):
            continue
        # The header that DEFINES the macro and the enum names them all without being a site.
        if entry == "WireDeclineTally.h":
            continue
        path = os.path.join(src_dir, entry)
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = strip_code(f.read()).splitlines()
        for i, line in enumerate(lines):
            for m in AT_SITE.finditer(line):
                counted.setdefault(m.group(1), []).append((entry, i + 1))
            for m in COUNT_SITE.finditer(line):
                name = m.group(1)
                counted.setdefault(name, []).append((entry, i + 1))
                window = lines[max(0, i - LOG_WINDOW):i]
                if not any(LOGGED.search(w) for w in window):
                    unlogged_counts.append((entry, i + 1, name))

    orphan_rows = [r for r in rows if r not in counted]
    unknown_sites = sorted(set(counted) - set(rows))

    for name in orphan_rows:
        print("wire-declines: row %s has NO SITE - it can never be counted" % name, file=sys.stderr)
    for name in unknown_sites:
        for where in counted[name]:
            print("wire-declines: %s:%d counts %s, which is not a row in WireDeclines.def"
                  % (where[0], where[1], name), file=sys.stderr)
    for entry, line, name in unlogged_counts:
        print("wire-declines: %s:%d counts %s with no MGLOG_ within %d lines above it - a decline "
              "must be readable from the log, not only from a counter"
              % (entry, line, name, LOG_WINDOW), file=sys.stderr)

    bad = len(orphan_rows) + len(unknown_sites) + len(unlogged_counts)
    print("wire-declines: %d rows, %d with sites, %d site(s) total, %d unlogged, %d unknown"
          % (len(rows), len(rows) - len(orphan_rows), sum(len(v) for v in counted.values()),
             len(unlogged_counts), len(unknown_sites)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
