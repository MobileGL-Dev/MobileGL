#!/usr/bin/env python3
# MobileGL - scripts/check_structure.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""Check the source layout against CONTRIBUTING.md: module layering and file and function size.

    python3 scripts/check_structure.py tree [--update-baseline]
    python3 scripts/check_structure.py report [layering|files|functions]   # list what the totals count
    python3 scripts/check_structure.py ci                                  # what CI runs
    python3 scripts/check_structure.py self-test

Each total is a ratchet against scripts/data/structure_baseline.json, the same way the text
hygiene ratchets work: CI fails when a total grows or when the baseline was raised. Totals are
global, so moving code between files does not trip them.
"""

import argparse
import importlib.util
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE_PATH = "scripts/data/structure_baseline.json"

_spec = importlib.util.spec_from_file_location("text_hygiene", os.path.join(ROOT, "scripts", "check_text_hygiene.py"))
HYGIENE = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(HYGIENE)

FILE_MAX = 3000
FUNCTION_MAX = 300
SOURCE_EXT = (".h", ".hpp", ".cpp", ".cc", ".c", ".inc", ".def")
RATCHETS = ("layering_violations", "file_excess_lines", "function_excess_lines")

# Modules from the bottom up. A file may include files of its own layer and of the layers below.
# The root headers (Includes.h, Config.h) belong to the base; the root .cpp files start the library
# and sit above everything they start.
LAYERS = (
    ("base", ("MobileGL/*.h", "MG_Util", "MG_State")),
    ("pipe", ("MG_Pipe",)),
    ("backend", ("MG_Backend", "MG_Remote", "MG_Impl")),
    ("top", ("MobileGL/*.cpp", "MG_Gbm")),
    ("tests", ("MG_Test", "MG_IntegrationTest", "MG_Benchmark")),
)
# Each backend is isolated: its files are included only by the same backend, by the backend
# registry directly in MG_Backend/, by the top layer and by tests.
BACKENDS = ("MobileGL/MG_Backend/DirectGLES/", "MobileGL/MG_Backend/DirectVulkan/")

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


# ------------------------------------------------------------------------------------ layering

def layer_of(path):
    """The index of the path's layer in LAYERS, or None for a file outside the layered modules."""
    parts = path.split("/")
    if parts[0] != "MobileGL":
        return None
    if len(parts) == 2:
        key = "MobileGL/*.h" if path.endswith((".h", ".hpp")) else "MobileGL/*.cpp"
    else:
        key = parts[1]
    for index, (_name, modules) in enumerate(LAYERS):
        if key in modules:
            return index
    return None


def backend_of(path):
    return next((b for b in BACKENDS if path.startswith(b)), None)


def resolve(source, include, fileset):
    """The tracked file an include names: relative to the including file, then to MobileGL/."""
    for candidate in (os.path.normpath(os.path.join(os.path.dirname(source), include)).replace("\\", "/"),
                      "MobileGL/" + include):
        if candidate in fileset:
            return candidate
    return None


def layering_problems(source, target):
    """Why source may not include target, or None."""
    a, b = layer_of(source), layer_of(target)
    if a is not None and b is not None and b > a:
        return "%s layer includes the %s layer" % (LAYERS[a][0], LAYERS[b][0])
    backend = backend_of(target)
    if backend and backend_of(source) != backend:
        registry = source.count("/") == 2 and source.startswith("MobileGL/MG_Backend/")
        if not registry and a is not None and LAYERS[a][0] not in ("top", "tests"):
            return "includes the %s backend from outside it" % backend.split("/")[2]
    return None


def layering_violations(files, read):
    fileset = set(files)
    found = []
    for path in files:
        if layer_of(path) is None:
            continue
        for lineno, line in enumerate(read(path).split("\n"), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            target = resolve(path, m.group(1), fileset)
            why = target and layering_problems(path, target)
            if why:
                found.append((path, lineno, "%s (%s)" % (why, target)))
    return found


# ---------------------------------------------------------------------------------------- size

def strip_code(text):
    """Blank out comments, string and character literals, preprocessor lines and every conditional
    branch after the first (an #if and its #else may each open the same brace), keeping newlines."""
    out = []
    i, n = 0, len(text)
    line_start = True
    skipped = []  # one entry per open #if: whether its current branch is blanked
    while i < n:
        c = text[i]
        if line_start and c in " \t":
            out.append(c)
            i += 1
            continue
        if line_start and c == "#":
            j = i
            while True:  # a directive runs to the first newline not escaped by a backslash
                j = text.find("\n", j)
                if j < 0:
                    j = n
                    break
                if text[j - 1] != "\\":
                    break
                j += 1
            keyword = re.match(r"#\s*(\w*)", text[i:j]).group(1)
            if keyword in ("if", "ifdef", "ifndef"):
                skipped.append(False)
            elif keyword in ("else", "elif", "elifdef", "elifndef") and skipped:
                skipped[-1] = True
            elif keyword == "endif" and skipped:
                skipped.pop()
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        if line_start and any(skipped) and c != "\n":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        line_start = c == "\n"
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == '"' and i > 0 and text[i - 1] == "R":
            m = re.match(r'"([^(\s]{0,16})\(', text[i:])
            end = text.find(")" + m.group(1) + '"', i) if m else -1
            j = n if end < 0 else end + len(m.group(1)) + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == "'" and i > 0 and text[i - 1].isalnum() and i + 1 < n and text[i + 1].isalnum():
            out.append(" ")  # a digit separator: 1'000'000
            i += 1
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] not in (c, "\n"):
                j += 2 if text[j] == "\\" else 1
            out.append(" " * (min(j, n - 1) - i + 1))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


FUNCTION_TAIL_RE = re.compile(
    r"\)\s*(?:const|volatile|noexcept(?:\s*\([^()]*\))?|override|final|mutable|&&?|->\s*[\w:<>,*&\s]+?|\s)*$")


def block_kind(header):
    """What a brace opened after this header starts: namespace, class, function or other."""
    header = " ".join(header.split())
    if re.search(r"\bnamespace\b", header) or header == "extern":  # extern "C", its literal blanked
        return "namespace"
    if FUNCTION_TAIL_RE.search(header):
        return "function"
    if re.search(r"\b(class|struct|union|enum)\b", header) and "=" not in header:
        return "class"
    return "other"


def functions(text):
    """(first_line, last_line) of each function body outside other functions."""
    code = strip_code(text)
    found = []
    stack = []  # (kind, line)
    header = []
    line = 1
    for ch in code:
        if ch == "\n":
            line += 1
            header.append(" ")
        elif ch == "{":
            if stack and stack[-1][0] in ("function", "other"):
                stack.append(("other", line))
            else:
                stack.append((block_kind("".join(header)), line))
            header = []
        elif ch == "}":
            if stack:
                kind, start = stack.pop()
                if kind == "function":
                    found.append((start, line))
            header = []
        elif ch == ";":
            header = []
        else:
            header.append(ch)
    return found


def size_counts(files, read):
    """(file_excess, function_excess, per_file_rows, per_function_rows)."""
    file_rows, function_rows = [], []
    for path in files:
        text = read(path)
        lines = text.count("\n") + (0 if text.endswith("\n") else 1)
        if lines > FILE_MAX:
            file_rows.append((lines - FILE_MAX, path, lines))
        for start, end in functions(text):
            length = end - start + 1
            if length > FUNCTION_MAX:
                function_rows.append((length - FUNCTION_MAX, path, start, length))
    return (sum(r[0] for r in file_rows), sum(r[0] for r in function_rows),
            sorted(file_rows, reverse=True), sorted(function_rows, reverse=True))


# ---------------------------------------------------------------------------------------- tree

def source_files(repo=ROOT):
    files = HYGIENE.git("ls-files", "MobileGL", repo=repo).splitlines()
    return [f for f in files if f.endswith(SOURCE_EXT) and not HYGIENE.is_generated(f)]


def read_file(path):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as fh:
        return fh.read().replace("\r\n", "\n")


def measure(files, read=read_file):
    layering = layering_violations(files, read)
    file_excess, function_excess, file_rows, function_rows = size_counts(files, read)
    totals = {"layering_violations": len(layering), "file_excess_lines": file_excess,
              "function_excess_lines": function_excess}
    return totals, layering, file_rows, function_rows


def check_tree(update_baseline=False, base=None):
    totals, layering, file_rows, function_rows = measure(source_files())
    path = os.path.join(ROOT, BASELINE_PATH)
    try:
        with open(path, encoding="utf-8") as fh:
            baseline = json.load(fh)
    except OSError:
        baseline = {}
    if update_baseline:
        lowered = {k: min(baseline.get(k, v), v) for k, v in totals.items()}
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(lowered, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("baseline written (it can only go down): %s" % lowered)
        return True
    previous = None
    if base:
        previous = HYGIENE.load_baseline(HYGIENE.git("show", "%s:%s" % (base, BASELINE_PATH), check=False))
    largest = {
        "layering_violations": ["%s:%d %s" % v for v in layering[:5]],
        "file_excess_lines": ["%s (%d lines)" % (r[1], r[2]) for r in file_rows[:5]],
        "function_excess_lines": ["%s:%d (%d lines)" % (r[1], r[2], r[3]) for r in function_rows[:5]],
    }
    ok = True
    for key in RATCHETS:
        now, allowed = totals[key], baseline.get(key, 0)
        if previous and key in previous and allowed > previous[key]:
            ok = False
            HYGIENE.report(BASELINE_PATH, 1, "the %s baseline was raised from %d to %d; it can only go down"
                           % (key, previous[key], allowed))
        if now > allowed:
            ok = False
            HYGIENE.report(None, None, "%s grew from %d to %d; largest: %s; see: python3 %s report"
                           % (key, allowed, now, ", ".join(largest[key]), "scripts/check_structure.py"))
        elif now < allowed:
            HYGIENE.notice("%s fell from %d to %d: lower the baseline with --update-baseline" % (key, allowed, now))
    print("structure: totals %s" % totals)
    return ok


def print_report(what):
    totals, layering, file_rows, function_rows = measure(source_files())
    if what in (None, "layering"):
        print("layering violations: %d" % totals["layering_violations"])
        for path, lineno, why in layering:
            print("  %s:%d %s" % (path, lineno, why))
    if what in (None, "files"):
        print("files over %d lines: %d excess lines" % (FILE_MAX, totals["file_excess_lines"]))
        for _excess, path, lines in file_rows:
            print("  %6d %s" % (lines, path))
    if what in (None, "functions"):
        print("functions over %d lines: %d excess lines" % (FUNCTION_MAX, totals["function_excess_lines"]))
        for _excess, path, start, length in function_rows:
            print("  %6d %s:%d" % (length, path, start))
    return True


# ----------------------------------------------------------------------------------- self-test

def self_test():
    failures = []

    def expect(name, cond):
        if not cond:
            failures.append(name)

    expect("layer: root header", layer_of("MobileGL/Config.h") == 0)
    expect("layer: root source", LAYERS[layer_of("MobileGL/Init.cpp")][0] == "top")
    expect("layer: module", LAYERS[layer_of("MobileGL/MG_Remote/Client/A.h")][0] == "backend")
    expect("layer: outside", layer_of("tools/x.cpp") is None)
    expect("layering: down is fine", layering_problems("MobileGL/MG_Impl/A.cpp", "MobileGL/MG_Util/B.h") is None)
    expect("layering: same layer is fine",
           layering_problems("MobileGL/MG_Remote/A.cpp", "MobileGL/MG_Impl/B.h") is None)
    expect("layering: up fails", layering_problems("MobileGL/MG_State/A.cpp", "MobileGL/MG_Pipe/B.h") is not None)
    expect("layering: product into tests fails",
           layering_problems("MobileGL/MG_Util/A.cpp", "MobileGL/MG_Benchmark/B.inc") is not None)
    gles, vk = "MobileGL/MG_Backend/DirectGLES/U.h", "MobileGL/MG_Backend/DirectVulkan/R.cpp"
    expect("isolation: other backend fails", layering_problems(vk, gles) is not None)
    expect("isolation: same backend", layering_problems("MobileGL/MG_Backend/DirectGLES/A.cpp", gles) is None)
    expect("isolation: registry", layering_problems("MobileGL/MG_Backend/BackendObjects.h", gles) is None)
    expect("isolation: elsewhere in MG_Backend fails",
           layering_problems("MobileGL/MG_Backend/Record/A.cpp", gles) is not None)
    expect("isolation: tests", layering_problems("MobileGL/MG_Test/A.cpp", gles) is None)

    files = {
        "MobileGL/MG_State/S.h": '#include "MG_Pipe/P.h"\n#include <MG_Util/U.h>\n',
        "MobileGL/MG_Pipe/P.h": "int p;\n",
        "MobileGL/MG_Util/U.h": '#include "../MG_Backend/DirectVulkan/R.h"\n',
        "MobileGL/MG_Backend/DirectVulkan/R.h": "int r;\n",
    }
    found = layering_violations(list(files), files.__getitem__)
    expect("layering: include resolution", [(p, n) for p, n, _ in found]
           == [("MobileGL/MG_State/S.h", 1), ("MobileGL/MG_Util/U.h", 1)])

    expect("block: extern C", block_kind('extern    ') == "namespace")
    src = ("namespace a {\n"                                          # 1
           "struct S {\n  int f() const { return 1; }\n};\n"           # 2-4
           "static const int k[] = {\n 1, 2 };\n"                      # 5-6
           "int g(int x)\n{\n  auto l = [] { return '}'; };\n"         # 7-9
           "  const char* s = \"{{\"; // {\n"                          # 10
           "#define OPEN {\n"                                          # 11
           "  if (x) {\n  }\n  return 1'000;\n}\n"                     # 12-15
           "auto h() -> int {\n  return R\"x({)x\"[0];\n}\n"           # 16-18
           "TEST_F(Suite, Case) {\n}\n"                                # 19-20
           "}\n")                                                      # 21
    expect("functions: found", functions(src) == [(3, 3), (8, 15), (16, 18), (19, 20)])
    branches = "#if A\nvoid f() {\n#elif B\nvoid f(int) {\n#else\nvoid g() {\n#endif\n}\n"
    expect("functions: first conditional branch", functions(branches) == [(2, 8)])
    long_src = "void f() {\n" + "  x();\n" * (FUNCTION_MAX + 5) + "}\n"
    long_files = {"MobileGL/MG_Util/L.cpp": long_src * 2}
    sizes = size_counts(list(long_files), long_files.__getitem__)
    expect("size: function excess", sizes[1] == 2 * 7)
    big = {"MobileGL/MG_Util/B.cpp": "int x;\n" * (FILE_MAX + 10)}
    expect("size: file excess", size_counts(list(big), big.__getitem__)[0] == 10)

    for name in failures:
        print("self-test FAILED: %s" % name)
    print("self-test: %d checks failed" % len(failures) if failures else "self-test: all checks pass")
    return not failures


# ---------------------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("tree")
    p.add_argument("--update-baseline", action="store_true")
    p = sub.add_parser("report")
    p.add_argument("what", nargs="?", choices=("layering", "files", "functions"))
    sub.add_parser("ci")
    sub.add_parser("self-test")
    args = ap.parse_args()
    ok = {"tree": lambda: check_tree(args.update_baseline),
          "report": lambda: print_report(args.what),
          "ci": lambda: check_tree(base=HYGIENE.ci_range()[0]),
          "self-test": self_test}[args.cmd]()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
