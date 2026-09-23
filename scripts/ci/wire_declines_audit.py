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
WireDeclineTally::Count(WireDeclineSite::X) is allowed ONLY when an MGLOG_W/E statement stands
just above it IN THE SAME BLOCK: a line that BEGINS with the log call, at the Count line's own
indentation, with no line of lesser indentation (an enclosing `if (...) {`, a `} else {`, a
`#else`) and no same-indent `return`/`break`/`case`/... between them. A log in a sibling branch
or a nested one is not this site's line, and neither is a guarded one-liner (`if (g) MGLOG_W`).

Everything is matched on the text with comments and string/character literals blanked out
(newlines kept, so every report cites the real line): a site or a log that is only a comment,
or only a string, is not one.

usage: wire_declines_audit.py [repo-root]
       wire_declines_audit.py --self-test     (negative controls on inline fixtures)
"""
import contextlib
import io
import os
import re
import sys
import tempfile

RENDERER = "MobileGL/MG_Backend/DirectVulkan/Renderer"
DEF = os.path.join(RENDERER, "WireDeclines.def")
SOURCES = (".cpp", ".inc", ".h")
# How far above a bare Count() an MGLOG_ may stand and still be "the line for this site" -
# a cap on top of the block walk, so a log far up the same block does not count either.
LOG_WINDOW = 8

# Matched on comment-stripped text, so a row with a trailing `// note` is still a row.
ROW = re.compile(r"^\s*MGL_WIRE_DECLINE\(\s*([A-Za-z][A-Za-z0-9]*)\s*\)\s*$")
AT_SITE = re.compile(r"MGL_WIRE_DECLINE_AT\(\s*([A-Za-z][A-Za-z0-9]*)\s*,")
COUNT_SITE = re.compile(r"WireDeclineTally::Count\(\s*WireDeclineSite::([A-Za-z][A-Za-z0-9]*)\s*\)")
# Warning or error only: an MGLOG_D / MGLOG_V is compiled away in the Release builds that ship,
# so it is not a line anybody can read off a device.
#
# ANCHORED TO THE STATEMENT START. The log must be the statement at the Count's indentation,
# not a line that mentions one: `if (g) MGLOG_W("x");` and `g ? MGLOG_W("x") : (void)0;` at
# the Count's own indentation are logs that run only sometimes, and a decline that is counted
# every time but logged only sometimes is the unlogged site this check exists to refuse.
LOGGED = re.compile(r"^\s*MGLOG_[WE](_ONCE)?\s*\(")
RAW_PREFIX = re.compile(r"(?:^|[^A-Za-z0-9_])(?:u8|u|U|L)?R$")
# The same shape for a character literal: `L'"'` is a wide char, not the identifier `L` followed
# by a digit separator - and read the other way the `"` inside it opens a phantom string that
# blanks the rest of the line, sites included.
CHAR_PREFIX = re.compile(r"(?:^|[^A-Za-z0-9_])(?:u8|u|U|L)$")
# A statement control cannot flow past. A log ABOVE one of these at the Count's indentation is
# on a path that ends there, so it is not the Count's log.
FLOW_STOP = re.compile(r"^\s*(?:case\b|default\s*:|break\s*;|return\b|continue\s*;|goto\b|throw\b)")
# Conditional-compilation lines the block walk steps over: a log and its Count that straddle a
# `#if MOBILEGL_BUILD_DISAGGREGATED` are in one block on both sides of the preprocessor. `#else`
# and `#elif` are NOT here - a log on the other side of one is in a different build, not this one.
PP_SKIP = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|endif)\b")


def strip_code(text: str) -> str:
    """Blank out //, /* */ comments and string / character literals, keeping every newline.

    What remains lines up with the source line for line, so a match's index is a real line
    number. Raw strings R"d(...)d", prefixed character literals (L'x', u8'x') and C++14 digit
    separators (1'000) are handled."""
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
        elif c == '"' or (c == "'" and (CHAR_PREFIX.search(text[max(0, i - 3):i])
                                        or not (i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_")))):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            blank(i, j)
            i = j
        else:
            i += 1
    return "".join(out)


def indent_of(line: str) -> int:
    return len(line.expandtabs(4)) - len(line.expandtabs(4).lstrip())


def has_own_log(lines, i: int) -> bool:
    """Is there an MGLOG_W/E statement above line i in the same block, within LOG_WINDOW?

    Walk upwards; blank lines, `#if`/`#ifdef`/`#ifndef`/`#endif` lines and deeper lines (a
    log's continuation lines, a nested block) are passed over, but only a line at exactly the
    Count's indentation can be its log. The first line indented LESS than the Count is the
    block's opening (or a sibling branch's `} else {`, or a `#else`) and ends the walk; so does
    a same-indent `case`/`default`/`break`/`return`/`continue`/`goto`/`throw`, past which
    control cannot reach the Count."""
    own = indent_of(lines[i])
    for k in range(i - 1, max(-1, i - 1 - LOG_WINDOW), -1):
        line = lines[k]
        if not line.strip() or PP_SKIP.match(line):
            continue
        ind = indent_of(line)
        if ind < own:
            return False
        if ind == own:
            if LOGGED.match(line):
                return True
            if FLOW_STOP.match(line):
                return False
    return False


def audit(root: str) -> int:
    def_path = os.path.join(root, DEF)
    if not os.path.isfile(def_path):
        print("wire-declines: %s not found" % DEF, file=sys.stderr)
        return 2

    rows = []
    with open(def_path, encoding="utf-8") as f:
        def_text = strip_code(f.read())
    for line in def_text.splitlines():
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
                if not has_own_log(lines, i):
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
        print("wire-declines: %s:%d counts %s with no MGLOG_W/E in its own block within %d lines "
              "above it - a decline must be readable from the log, not only from a counter"
              % (entry, line, name, LOG_WINDOW), file=sys.stderr)

    bad = len(orphan_rows) + len(unknown_sites) + len(unlogged_counts)
    print("wire-declines: %d rows, %d with sites, %d site(s) total, %d unlogged, %d unknown"
          % (len(rows), len(rows) - len(orphan_rows), sum(len(v) for v in counted.values()),
             len(unlogged_counts), len(unknown_sites)))
    return 1 if bad else 0


# --self-test: each fixture is (name, def rows text, Fixture.cpp text, expected rc, text the
# audit must print - so a red is red for the reason the fixture is about). The red ones are
# the holes the B3 fix rounds closed; the green ones prove the audit still accepts the shapes
# the tree uses, and the two shapes (a prefixed char literal, a `#if` between log and Count)
# that a fail-closed strip or walk would have reddened for no defect.
NO_SITE = "has NO SITE"
NO_LOG = "with no MGLOG_W/E in its own block"
_DEF_GHOST = "MGL_WIRE_DECLINE(GhostRow)\n"
SELF_TEST_FIXTURES = (
    ("comment-only site (//)", _DEF_GHOST,
     "void F() {\n"
     "    // MGL_WIRE_DECLINE_AT(GhostRow, \"never counted\");\n"
     "}\n", 1, NO_SITE),
    ("comment-only site (/* */)", _DEF_GHOST,
     "void F() {\n"
     "    /* retired:\n"
     "       MGL_WIRE_DECLINE_AT(GhostRow, \"never counted\"); */\n"
     "}\n", 1, NO_SITE),
    ("string-literal site", _DEF_GHOST,
     "const char* kDoc = \"MGL_WIRE_DECLINE_AT(GhostRow, x);\";\n", 1, NO_SITE),
    ("other-branch log", _DEF_GHOST,
     "void F(int x) {\n"
     "    if (x == 1) {\n"
     "        MGLOG_W(\"x is one\");\n"
     "    }\n"
     "    if (x == 2) {\n"
     "        WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "    }\n"
     "}\n", 1, NO_LOG),
    ("nested-branch log", _DEF_GHOST,
     "void F(int x) {\n"
     "    if (x) {\n"
     "        if (x == 1) {\n"
     "            MGLOG_W(\"x is one\");\n"
     "        }\n"
     "        WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "    }\n"
     "}\n", 1, NO_LOG),
    ("log only in a /* */ comment above", _DEF_GHOST,
     "void F(int x) {\n"
     "    if (x) {\n"
     "        /* MGLOG_W(\"x\"); */\n"
     "        WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "    }\n"
     "}\n", 1, NO_LOG),
    ("nearest log is MGLOG_D", _DEF_GHOST,
     "void F(int x) {\n"
     "    if (x) {\n"
     "        MGLOG_D(\"compiled away in Release\");\n"
     "        WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "    }\n"
     "}\n", 1, NO_LOG),
    ("guarded one-liner log at the Count's indentation", _DEF_GHOST,
     "void F(int g) {\n"
     "    if (g) MGLOG_W(\"logged only when g\");\n"
     "    g ? MGLOG_E(\"or only then\") : (void)0;\n"
     "    WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "}\n", 1, NO_LOG),
    ("log on the far side of a same-indent return", _DEF_GHOST,
     "bool F(int x) {\n"
     "    if (x == 1) goto declined;\n"
     "    MGLOG_W(\"x is not one\");\n"
     "    return true;\n"
     "    declined:\n"
     "    WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "    return false;\n"
     "}\n", 1, NO_LOG),
    ("log in the previous case of a same-indent switch", _DEF_GHOST,
     "void F(int x) {\n"
     "    switch (x) {\n"
     "    case 1:\n"
     "    MGLOG_W(\"one\");\n"
     "    break;\n"
     "    case 2:\n"
     "    WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "    break;\n"
     "    }\n"
     "}\n", 1, NO_LOG),
    ("log only on the other side of a #else", _DEF_GHOST,
     "void F(int x) {\n"
     "#if MOBILEGL_BUILD_DISAGGREGATED\n"
     "    MGLOG_W(\"x\");\n"
     "#else\n"
     "    MGLOG_D(\"compiled away\");\n"
     "#endif\n"
     "    WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "}\n", 1, NO_LOG),
    ("prefixed char literals (L'\"', u8'\"') before a site on the same line",
     "MGL_WIRE_DECLINE(WideQuote)\nMGL_WIRE_DECLINE(Utf8Quote)\n",
     "bool F(wchar_t w, char8_t c) {\n"
     "    if (w == L'\"') MGL_WIRE_DECLINE_AT(WideQuote, \"a wide double quote\");\n"
     "    if (c == u8'\"') MGL_WIRE_DECLINE_AT(Utf8Quote, \"a utf-8 double quote\");\n"
     "    return true;\n"
     "}\n", 0, "0 unlogged, 0 unknown"),
    ("count under its log across a column-0 #if/#endif", _DEF_GHOST,
     "void F(int x) {\n"
     "    if (x) {\n"
     "        MGLOG_W(\"x\");\n"
     "#if MOBILEGL_BUILD_DISAGGREGATED\n"
     "        WireDeclineTally::Count(WireDeclineSite::GhostRow);\n"
     "#endif\n"
     "    }\n"
     "}\n", 0, "0 unlogged, 0 unknown"),
    ("good sites (AT form, bare Count under its own W/E, def row with trailing comment)",
     "MGL_WIRE_DECLINE(GoodAt) // the macro form\nMGL_WIRE_DECLINE(GoodBare)\n",
     "bool F(int x) {\n"
     "    if (x == 1) {\n"
     "        MGL_WIRE_DECLINE_AT(GoodAt, \"x=%d\", x);\n"
     "        return false;\n"
     "    }\n"
     "    if (x == 2) {\n"
     "        MGLOG_E_ONCE(\"x is two: %d, \"\n"
     "                     \"declined\", x);\n"
     "        WireDeclineTally::Count(WireDeclineSite::GoodBare);\n"
     "        return false;\n"
     "    }\n"
     "    return true;\n"
     "}\n", 0, "0 unlogged, 0 unknown"),
)


def self_test() -> int:
    failures = 0
    for name, def_text, src_text, want, why in SELF_TEST_FIXTURES:
        with tempfile.TemporaryDirectory(prefix="wire-declines-selftest-") as root:
            src_dir = os.path.join(root, RENDERER)
            os.makedirs(src_dir)
            with open(os.path.join(src_dir, "WireDeclines.def"), "w", encoding="utf-8") as f:
                f.write(def_text)
            with open(os.path.join(src_dir, "Fixture.cpp"), "w", encoding="utf-8") as f:
                f.write(src_text)
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured), contextlib.redirect_stderr(captured):
                got = audit(root)
        ok = got == want and why in captured.getvalue()
        failures += 0 if ok else 1
        print("wire-declines self-test: %-4s %s -> rc %d (want %d, printing \"%s\")"
              % ("ok" if ok else "FAIL", name, got, want, why))
        if not ok:
            sys.stdout.write("".join("    " + l + "\n" for l in captured.getvalue().splitlines()))
    print("wire-declines self-test: %d/%d fixtures as expected"
          % (len(SELF_TEST_FIXTURES) - failures, len(SELF_TEST_FIXTURES)))
    return 1 if failures else 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        return self_test()
    return audit(os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "."))


if __name__ == "__main__":
    sys.exit(main())
