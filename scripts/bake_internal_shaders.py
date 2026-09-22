#!/usr/bin/env python3
# MobileGL - scripts/bake_internal_shaders.py
# Copyright (c) 2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""Regenerate the tree's baked SPIR-V headers from their committed GLSL.

    python3 scripts/bake_internal_shaders.py --build-dir build-split
    python3 scripts/bake_internal_shaders.py --build-dir build-split --check

THIS SCRIPT DOES NOT COMPILE ANYTHING. It runs MG_Test's BakedInternalShadersTest with
MOBILEGL_BAKE_INTERNAL_SHADERS set, takes the words that binary emits, and splices them into
the headers. The compiler is therefore the IN-TREE glslang the gate itself uses - one compiler
and one answer.

The alternative, which is what the four headers in the tree were made with before this script
existed, is a glslangValidator somebody happened to have on their PATH. That is how the
iterationRP witness ended up carrying words its own source no longer produces: nothing named
the compiler, nothing re-ran it, and the header's prose was the only record that it was stale.
A gate whose baker is a different program from the gate has the same failure one level up - the
day the CLI's defaults and the library's diverge, the gate reds on a header nobody can
regenerate, and the fix is to edit the words by hand.

FORMATTING IS PRESERVED PER HEADER, not normalised: the headers in the tree disagree about
words per line and about the `u` suffix, and reformatting one of them would bury a one-word
change in a whole-file diff. The style is read back out of the array being replaced.

--check exits non-zero and names the headers that would change, without writing. It is the
same verdict the ctest entry gives; this mode exists so the regeneration path can be checked
from a shell without reading gtest output.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST_TARGET = "BakedInternalShadersTest"
BAKE_ENV = "MOBILEGL_BAKE_INTERNAL_SHADERS"

# `    0x07230203u, 0x00010000u, ...` - the body of one baked array.
WORD_RE = re.compile(r"0x([0-9a-fA-F]{1,8})u?")


def find_test_binary(build_dir: Path) -> Path:
    """The test executable, wherever this generator put it."""
    candidates = sorted(build_dir.rglob(TEST_TARGET))
    candidates = [c for c in candidates if c.is_file() and os.access(c, os.X_OK)]
    if not candidates:
        raise SystemExit(
            f"{TEST_TARGET} not found under {build_dir}. Build it first:\n"
            f"    cmake --build {build_dir} --target {TEST_TARGET}"
        )
    return candidates[0]


def emit_words(binary: Path, out_dir: Path) -> dict:
    """Run the gate in bake mode; return {symbol: (header, [words])}."""
    env = dict(os.environ)
    env[BAKE_ENV] = str(out_dir)
    result = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"{TEST_TARGET} failed in bake mode (rc={result.returncode})")
    baked = {}
    for path in sorted(out_dir.glob("*.words")):
        lines = path.read_text().splitlines()
        if not lines:
            raise SystemExit(f"{path} is empty")
        header = lines[0].strip()
        words = [int(line, 16) for line in lines[1:] if line.strip()]
        baked[path.stem] = (header, words)
    if not baked:
        raise SystemExit(f"{TEST_TARGET} emitted nothing into {out_dir}")
    return baked


def locate_array(text: str, symbol: str):
    """(body_start, body_end, style) for `symbol`'s array body."""
    opener = re.search(re.escape(symbol) + r"\s*\[\s*\]\s*=\s*\{", text)
    if not opener:
        raise SystemExit(f"cannot find the array `{symbol}` to replace")
    body_start = opener.end()
    closer = text.find("};", body_start)
    if closer < 0:
        raise SystemExit("the array `" + symbol + "` has no closing `};`")
    body = text[body_start:closer]
    rows = [row for row in body.split("\n") if WORD_RE.search(row)]
    if not rows:
        raise SystemExit(f"the array `{symbol}` holds no words to learn its style from")
    style = {
        "indent": re.match(r"[ \t]*", rows[0]).group(0),
        # The LONGEST row, not the first: a one-row array and a ragged last row would both
        # teach the wrong width.
        "per_line": max(len(WORD_RE.findall(row)) for row in rows),
        "suffix": "u" if re.search(r"0x[0-9a-fA-F]+u", body) else "",
        # The whitespace the closing brace sits behind. It is part of the SPAN being
        # replaced, so a body written without it silently outdents every array it
        # touches - a diff on three headers that changed no word at all.
        "close_indent": re.search(r"[ \t]*$", text[:closer]).group(0),
    }
    return body_start, closer, style


def format_body(words, style) -> str:
    per_line = style["per_line"]
    out = ["\n"]
    for at in range(0, len(words), per_line):
        chunk = words[at:at + per_line]
        out.append(style["indent"]
                   + ", ".join(f"0x{word:08x}{style['suffix']}" for word in chunk) + ",\n")
    out.append(style["close_indent"])
    return "".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build-split",
                        help="a configured build tree that has BakedInternalShadersTest")
    parser.add_argument("--build", action="store_true",
                        help="build the test target first")
    parser.add_argument("--check", action="store_true",
                        help="report headers that would change and exit non-zero; write nothing")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    if args.build:
        subprocess.run(["cmake", "--build", str(build_dir), "--target", TEST_TARGET],
                       check=True)

    binary = find_test_binary(build_dir)
    scratch = Path(tempfile.mkdtemp(prefix="mgl-bake-"))
    try:
        baked = emit_words(binary, scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    # One read/write per HEADER, not per symbol: two arrays share
    # WireColorBlitSpirv.h and three share PrimitivesGeneratedNoXfbProbeSpv.h, and
    # rewriting the file once per array would splice into a stale copy.
    by_header = {}
    for symbol, (header, words) in baked.items():
        by_header.setdefault(header, []).append((symbol, words))

    changed = []
    for header, entries in sorted(by_header.items()):
        path = ROOT / header
        if not path.exists():
            raise SystemExit(f"{header} does not exist")
        original = path.read_text()
        text = original
        for symbol, words in sorted(entries):
            start, end, style = locate_array(text, symbol)
            text = text[:start] + format_body(words, style) + text[end:]
        if text == original:
            print(f"bake: {header} is already fresh ({len(entries)} array(s))")
            continue
        changed.append(header)
        if args.check:
            print(f"bake: {header} WOULD CHANGE ({', '.join(s for s, _ in sorted(entries))})")
        else:
            path.write_text(text)
            print(f"bake: {header} rewritten ({', '.join(s for s, _ in sorted(entries))})")

    if args.check and changed:
        print(f"bake: {len(changed)} header(s) are stale against their sources", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
