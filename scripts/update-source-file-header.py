#!/usr/bin/env python3
# MobileGL - scripts/update-source-file-header.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""Add the license header to source files that lack it.

    python3 scripts/update-source-file-header.py             # every tracked source file it covers
    python3 scripts/update-source-file-header.py FILE ...     # only these files
    python3 scripts/update-source-file-header.py --check      # list what needs attention, exit 1 if anything does

Covered under MobileGL/, scripts/ and tools/: C, C++, .inc, Java, JavaScript modules and GLSL with
// comments; Python, shell, CMake modules and CMakeLists.txt with # comments.
An existing header is kept as written, since its copyright years belong to the file; only a first
line that names a different path is corrected. Generated files and files under another license are
left alone. Files with mixed line endings are reported, never rewritten.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = "End of Source File Header"
HEADER = """{c} MobileGL - {path}
{c} Copyright (c) 2025-2026 MobileGL-Dev
{c} Licensed under the GNU Lesser General Public License v3.0:
{c}   https://www.gnu.org/licenses/gpl-3.0.txt
{c}   https://www.gnu.org/licenses/lgpl-3.0.txt
{c} SPDX-License-Identifier: LGPL-3.0-only
{c} End of Source File Header
"""
OWN_SPDX = "SPDX-License-Identifier: LGPL-3.0-only"
DIRS = ("MobileGL/", "scripts/", "tools/")
SLASH_EXT = (".h", ".hpp", ".cpp", ".cc", ".c", ".inc", ".java", ".mjs",
             ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese", ".glsl")
HASH_EXT = (".py", ".sh", ".cmake")
FOREIGN_LICENSE = re.compile(r"Apache License|Permission is hereby granted|MIT License|BSD License|"
                             r"Mozilla Public License|zlib License")
SPDX_RE = re.compile(r"SPDX-License-Identifier:\s*(\S+)")
COPYRIGHT_RE = re.compile(r"Copyright\b.*")
# Built against apitrace's glws/glproc interfaces; whether they are adapted from apitrace's own
# sources is not settled, so they carry no MobileGL header until it is.
EXEMPT = ("tools/trace_replay/apitrace_",)

# plan() statuses: the first three need no action, the last four do.
QUIET = ("ok", "foreign", "empty")
FIXABLE = ("add", "fix-path")
MANUAL = ("mixed-eol", "bad-header")


def is_generated(path):
    return "/generated/" in path or "/gen/" in path or os.path.basename(path).startswith("generated_")


def comment_prefix(path):
    if path.endswith(HASH_EXT) or os.path.basename(path) == "CMakeLists.txt":
        return "#"
    if path.endswith(SLASH_EXT):
        return "//"
    return None


def covered(path):
    return (path.startswith(DIRS) and comment_prefix(path) is not None
            and not is_generated(path) and not path.startswith(EXEMPT))


def plan(path, text):
    """Return (status, new_text). new_text is set only for the FIXABLE statuses."""
    if not text.strip():
        return "empty", None
    bom = "﻿" if text.startswith("﻿") else ""
    text = text[len(bom):]
    if "\r\n" in text and re.search(r"(?<!\r)\n", text):
        return "mixed-eol", None
    c = comment_prefix(path)
    lines = text.splitlines(keepends=True)
    eol = "\r\n" if lines[0].endswith("\r\n") else "\n"
    prefix = []
    while lines and (lines[0].startswith("#!") or (c == "#" and re.match(r"#.*coding[:=]", lines[0]))):
        prefix.append(lines.pop(0))

    head = "".join(lines[:15])
    if MARKER in head:
        first = lines[0].rstrip("\r\n")
        expected = "%s MobileGL - %s" % (c, path)
        if OWN_SPDX not in head or not re.match(r"(//|#|/\*) MobileGL - ", first):
            return "bad-header", None
        if first.startswith(c + " MobileGL - ") and first != expected:
            lines[0] = expected + lines[0][len(first):]
            return "fix-path", bom + "".join(prefix + lines)
        return "ok", None

    top = "".join(lines[:30])
    spdx = [m.group(1) for m in SPDX_RE.finditer(top)]
    copyrights = [m.group(0) for m in COPYRIGHT_RE.finditer(top)]
    if (FOREIGN_LICENSE.search(top) or any(s != "LGPL-3.0-only" for s in spdx)
            or any("MobileGL-Dev" not in line for line in copyrights)):
        return "foreign", None

    if lines and lines[0].strip() == "%s %s" % (c, OWN_SPDX):
        lines.pop(0)  # the full header below says the same
    header = [l + eol for l in HEADER.format(c=c, path=path).rstrip("\n").split("\n")]
    if lines and lines[0].strip():
        lines.insert(0, eol)  # one blank line between the header and the code
    return "add", bom + "".join(prefix + header + lines)


def tracked():
    out = subprocess.run(["git", "-C", ROOT, "-c", "core.quotePath=false", "ls-files"],
                         stdout=subprocess.PIPE).stdout.decode("utf-8", "replace")
    return [p for p in out.splitlines() if covered(p)]


def main(argv):
    check = "--check" in argv
    paths = [os.path.relpath(os.path.abspath(p), ROOT).replace(os.sep, "/") for p in argv if p != "--check"] or tracked()
    needs = 0
    for path in paths:
        if not covered(path):
            continue
        try:
            with open(os.path.join(ROOT, path), encoding="utf-8", newline="") as fh:
                text = fh.read()
        except UnicodeDecodeError:
            print("%s: not UTF-8, skipped" % path)
            needs += 1
            continue
        status, new_text = plan(path, text)
        if status in QUIET:
            continue
        needs += 1
        if status in MANUAL:
            print("%s: %s, fix by hand" % (path, status))
        elif check:
            print("%s: needs %s" % (path, status))
        else:
            with open(os.path.join(ROOT, path), "w", encoding="utf-8", newline="") as fh:
                fh.write(new_text)
            print("%s: %s" % (path, status))
    print("%d file(s) %s" % (needs, "need attention" if check else "changed or reported"))
    return 1 if check and needs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
