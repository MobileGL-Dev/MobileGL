#!/usr/bin/env python3
# MobileGL - scripts/symbol_report.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""Per-symbol nm/.text attribution between two libMobileGL.so builds.

P0.5's acceptance gate says every `nm --defined-only -S` delta has to be explainable per
symbol (ROADMAP.md:16). The awkward part is that de-nesting a type renames every mangled
name that mentions it - `ProgramObject::LinkArtifacts` -> `LinkArtifacts` shows up inside
every `std::vector<...>` instantiation too - so a raw nm diff of a pure move looks
catastrophic. `--strip-scope` folds those into a "renamed-only" bucket: same normalised
demangled name, byte-identical size. What is left over is the report's real content.

    python3 scripts/symbol_report.py --before old.so --after new.so
    python3 scripts/symbol_report.py --before old.so --after new.so \\
        --strip-scope 'MobileGL::MG_State::GLState::ProgramObject::' \\
        --only-names 'TypeFacts,ResourceReflection,XfbVarying,LinkArtifacts,SpirvArtifacts' \\
        --markdown ~/w7/p05-symbol-report.md

GUARD RAILS - the comparison is meaningless unless both .so files were built with:
  * the same CMAKE_BUILD_TYPE (the visibility presets differ between configurations,
    CMakeLists.txt:578-598, so a Debug/Release pair "adds" thousands of symbols),
  * MOBILEGL_ENABLE_LTO OFF on both sides (CMakeLists.txt:108,137-146 - LTO merges and
    renames at will and nothing here is attributable afterwards),
  * the same compiler and standard library.
The header of every report prints both paths and their byte sizes so a mismatched pair is
visible in the output rather than only in the reader's assumptions.

This tool is informational by default (ARCHITECTURE.md:507, job name `monolith-symbol-report`
ARCHITECTURE.md:568): with no gate flag it prints its report and exits 0, whatever it found.
Three flags turn it into a hard gate, and P1's G1 uses the first two - the strangler's pull build
has to stay byte-identical, and G1 spells that out as `added == removed == resized == renamed == 0`
with a `.text` delta of zero:

    python3 scripts/symbol_report.py --before base.so --after head.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0

  * `--fail-on-symbol-set-change` covers all FOUR buckets, not just added/removed: a resize at zero
    net delta and a rename at zero net delta are both source changes, and both are what a guard
    rewrite produces. It always compares at threshold 0, whatever `--threshold` is set to.
  * `--fail-on-added-bytes 0` means byte-identical, so a SHRUNK .text fails it too; a positive
    budget keeps the one-sided "must not grow by more than N" meaning. `--fail-on-text-delta` is
    the explicit spelling of the zero case for a run that wants no byte budget at all.

A gate that fires still writes its Markdown and JSON first: the report IS the diagnosis, and a
CI job that failed before uploading its artifact is a job nobody can act on.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

PREFIX = "symbol-report: "

# `nm --defined-only -S` prints "<addr> <size> <type> <name>", and "<addr> <type> <name>"
# for a defined symbol whose size the object file does not carry (absolute symbols,
# assembler labels).
NM_SIZED_RE = re.compile(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\S)\s+(.+)$")
NM_UNSIZED_RE = re.compile(r"^([0-9a-fA-F]+)\s+(\S)\s+(.+)$")

# `size --format=sysv` prints "section  size  addr" rows plus a Total row.
SIZE_ROW_RE = re.compile(r"^(\S+)\s+(\d+)(?:\s+(\d+))?\s*$")

INTERESTING_SECTIONS = (".text", ".data", ".bss", ".rodata", "Total")


def say(message):
    print(PREFIX + message)


def parse_nm(text):
    """nm --defined-only -S transcript -> {mangled: (type, size_or_None)}."""
    symbols = {}
    for line in text.splitlines():
        line = line.rstrip()
        if not line:
            continue
        match = NM_SIZED_RE.match(line)
        if match:
            symbols[match.group(4)] = (match.group(3), int(match.group(2), 16))
            continue
        match = NM_UNSIZED_RE.match(line)
        if match:
            symbols[match.group(3)] = (match.group(2), None)
    return symbols


def parse_size(text):
    """size --format=sysv transcript -> {section: bytes}."""
    sections = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("section"):
            continue
        match = SIZE_ROW_RE.match(stripped)
        if not match:
            continue
        sections[match.group(1)] = int(match.group(2))
    return sections


def demangle(names, cxxfilt):
    """Batch-demangle through c++filt; a plain identifier passes through unchanged."""
    ordered = list(names)
    if not ordered:
        return {}
    if not cxxfilt or not (shutil.which(cxxfilt) or os.path.isfile(cxxfilt)):
        return {name: name for name in ordered}
    completed = subprocess.run([cxxfilt], input="\n".join(ordered) + "\n",
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    lines = completed.stdout.splitlines()
    if len(lines) != len(ordered):
        return {name: name for name in ordered}
    return dict(zip(ordered, lines))


def parent_scope(prefix):
    """`A::B::C::` -> `A::B::`  (drop the last named component, keep the enclosing scope).

    A de-nesting moves `A::B::C::X` to `A::B::X`, so folding the two spellings together
    means deleting the `C::` component, NOT the whole prefix - deleting the whole prefix
    would turn the before side into a bare `X` that no after-side name matches.
    """
    body = prefix[:-2] if prefix.endswith("::") else prefix
    cut = body.rfind("::")
    return body[:cut + 2] if cut >= 0 else ""


def normalise(name, strip_scopes, rename_map):
    """Textual folding of the demangled name: this is what makes a de-nesting a rename."""
    for scope in strip_scopes:
        name = name.replace(scope, parent_scope(scope))
    for old, new in rename_map:
        name = name.replace(old, new)
    return name


def build_side(path, nm_tool, size_tool, cxxfilt, strip_scopes, rename_map, canned=None):
    if canned is None:
        nm_text = subprocess.run([nm_tool, "--defined-only", "-S", path],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True, check=True).stdout
        size_text = subprocess.run([size_tool, "--format=sysv", path],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, check=True).stdout
    else:
        nm_text, size_text = canned

    raw = parse_nm(nm_text)
    demangled = demangle(raw.keys(), cxxfilt)
    table = {}
    for mangled, (kind, size) in raw.items():
        key = normalise(demangled.get(mangled, mangled), strip_scopes, rename_map)
        table.setdefault(key, []).append({"mangled": mangled, "type": kind,
                                          "size": size or 0,
                                          "demangled": demangled.get(mangled, mangled)})
    folded = {}
    for key, entries in table.items():
        folded[key] = {
            "size": sum(e["size"] for e in entries),
            "count": len(entries),
            "mangled": sorted(e["mangled"] for e in entries),
            "type": entries[0]["type"],
        }
    return folded, parse_size(size_text), len(raw)


def bucket(before, after, only_names, threshold):
    def wanted(name):
        return not only_names or any(n in name for n in only_names)

    removed, added, resized, renamed, unchanged = [], [], [], [], 0
    for name in sorted(set(before) | set(after)):
        b = before.get(name)
        a = after.get(name)
        if b is None:
            if wanted(name):
                added.append({"name": name, "delta": a["size"], "before": 0, "after": a["size"]})
            continue
        if a is None:
            if wanted(name):
                removed.append({"name": name, "delta": -b["size"], "before": b["size"], "after": 0})
            continue
        if a["size"] != b["size"] and abs(a["size"] - b["size"]) > threshold:
            if wanted(name):
                resized.append({"name": name, "delta": a["size"] - b["size"],
                                "before": b["size"], "after": a["size"]})
            continue
        if a["mangled"] != b["mangled"]:
            # same normalised name, byte-identical size: the pure-move signature
            if wanted(name):
                renamed.append({"name": name, "delta": 0,
                                "before": b["mangled"][0], "after": a["mangled"][0]})
            continue
        unchanged += 1
    for group in (removed, added, resized):
        group.sort(key=lambda row: (-abs(row["delta"]), row["name"]))
    renamed.sort(key=lambda row: row["name"])
    return removed, added, resized, renamed, unchanged


def markdown_table(title, rows, columns):
    lines = ["", "### {} ({})".format(title, len(rows)), ""]
    if not rows:
        lines.append("_none_")
        lines.append("")
        return lines
    lines.append("| " + " | ".join(columns) + " |")
    lines.append("|" + "|".join(["---"] * len(columns)) + "|")
    for row in rows:
        lines.append("| " + " | ".join(str(cell).replace("|", "\\|") for cell in row) + " |")
    lines.append("")
    return lines


CANNED_BEFORE = ("""0000000000001000 0000000000000010 T MobileGL::MG_State::GLState::ProgramObject::LinkArtifacts::Reset()
0000000000002000 0000000000000020 T MobileGL::MG_State::GLState::ProgramObject::Link()
0000000000003000 0000000000000030 T MobileGL::Gone()
0000000000004000 0000000000000040 T MobileGL::Grew()
0000000000005000 T MobileGL::NoSize()
""", """libBefore.so  :
section        size   addr
.text          1000   100
.data           200   2000
.bss            300   3000
Total          1500
""")

CANNED_AFTER = ("""0000000000001000 0000000000000010 T MobileGL::MG_State::GLState::LinkArtifacts::Reset()
0000000000002000 0000000000000020 T MobileGL::MG_State::GLState::ProgramObject::Link()
0000000000004000 0000000000000050 T MobileGL::Grew()
0000000000006000 0000000000000060 T MobileGL::BrandNew()
0000000000005000 T MobileGL::NoSize()
""", """libAfter.so  :
section        size   addr
.text          1100   100
.data           200   2000
.bss            300   3000
Total          1600
""")


def gate_failures(added, removed, resized, renamed, text_delta, fail_on_added_bytes,
                  fail_on_symbol_set_change, fail_on_text_delta=False):
    """The reasons this run should fail, in the order they are reported. Empty means green.

    Pure, and takes the buckets rather than the file paths, so the self-test can drive it from the
    canned transcripts: a gate whose only test is a real build is a gate nobody re-tests.

    P1's G1 is spelled `added == removed == resized == renamed == 0` and `.text` delta 0, so all
    FOUR buckets are part of --fail-on-symbol-set-change and a budget of 0 bytes means zero
    movement in EITHER direction. A shrunk .text and a resize with zero net delta are exactly the
    shapes a guard rewrite produces, and both used to walk straight through this function.
    """
    reasons = []
    if fail_on_symbol_set_change and (added or removed or resized or renamed):
        reasons.append(
            "--fail-on-symbol-set-change: {} symbol(s) added, {} removed, {} resized, {} renamed. "
            "None of that is a property of the build machine, so any change here is a source "
            "change - name each one in the commit message or fix it.".format(
                len(added), len(removed), len(resized), len(renamed)))
    if fail_on_text_delta and text_delta != 0:
        reasons.append(
            "--fail-on-text-delta: .text moved by {:+d} bytes.".format(text_delta))
    if fail_on_added_bytes is not None:
        # A budget of zero is not "must not grow", it is "must not move": the pull build of a
        # strangler that got smaller is just as much a code change as one that got bigger, and G1
        # names the delta, not its sign. A positive budget keeps the older, one-sided meaning.
        if fail_on_added_bytes == 0 and text_delta != 0 and not fail_on_text_delta:
            reasons.append(
                "--fail-on-added-bytes 0: .text moved by {:+d} bytes (a budget of 0 means the "
                "section must be byte-identical, in either direction).".format(text_delta))
        elif fail_on_added_bytes > 0 and text_delta > fail_on_added_bytes:
            reasons.append(
                "--fail-on-added-bytes {}: .text grew by {} bytes.".format(
                    fail_on_added_bytes, text_delta))
    return reasons


def self_test():
    strip = ["MobileGL::MG_State::GLState::ProgramObject::"]
    before, before_sections, before_count = build_side(None, None, None, None, strip, [],
                                                       canned=CANNED_BEFORE)
    after, after_sections, after_count = build_side(None, None, None, None, strip, [],
                                                    canned=CANNED_AFTER)
    removed, added, resized, renamed, unchanged = bucket(before, after, [], 0)
    problems = []
    if before_count != 5 or after_count != 5:
        problems.append("nm parse counts: {} / {} (expected 5 / 5)".format(before_count, after_count))
    if [r["name"] for r in removed] != ["MobileGL::Gone()"]:
        problems.append("removed bucket: {}".format([r["name"] for r in removed]))
    if [r["name"] for r in added] != ["MobileGL::BrandNew()"]:
        problems.append("added bucket: {}".format([r["name"] for r in added]))
    if [(r["name"], r["delta"]) for r in resized] != [("MobileGL::Grew()", 0x10)]:
        problems.append("resized bucket: {}".format([(r["name"], r["delta"]) for r in resized]))
    # `ProgramObject::LinkArtifacts::Reset` -> `LinkArtifacts::Reset` folds to the same
    # normalised name at the same size: renamed-only, not added+removed.
    if [r["name"] for r in renamed] != ["MobileGL::MG_State::GLState::LinkArtifacts::Reset()"]:
        problems.append("renamed bucket: {}".format([r["name"] for r in renamed]))
    if unchanged != 2:  # ProgramObject::Link() and NoSize()
        problems.append("unchanged count: {} (expected 2)".format(unchanged))
    if before_sections.get(".text") != 1000 or after_sections.get("Total") != 1600:
        problems.append("size --format=sysv parse: {} / {}".format(before_sections, after_sections))
    # The three gates, driven from the same canned transcripts: the after side adds one symbol,
    # removes one, resizes one, renames one and grows .text by 100, so each flag must fire, each
    # must stay quiet when it is not asked for, and --fail-on-added-bytes must accept a delta it
    # was told to tolerate.
    text_delta = after_sections.get(".text", 0) - before_sections.get(".text", 0)
    if gate_failures(added, removed, resized, renamed, text_delta, None, False):
        problems.append("gates fired with no flag set")
    if len(gate_failures(added, removed, resized, renamed, text_delta, None, True)) != 1:
        problems.append("--fail-on-symbol-set-change did not fire on 1 added + 1 removed")
    if len(gate_failures(added, removed, resized, renamed, text_delta, 0, False)) != 1:
        problems.append("--fail-on-added-bytes 0 did not fire on a +100 .text delta")
    if gate_failures(added, removed, resized, renamed, text_delta, 100, False):
        problems.append("--fail-on-added-bytes 100 fired on a +100 .text delta")
    if len(gate_failures([], [], [], [], text_delta, 0, True)) != 1:
        problems.append("an unchanged symbol set still tripped --fail-on-symbol-set-change")
    # The three shapes that used to walk through: a SHRUNK .text under a zero budget, a resize with
    # no net delta, and a rename with no net delta. G1 forbids all three by name.
    if not gate_failures([], [], [], [], -4096, 0, False):
        problems.append("--fail-on-added-bytes 0 passed a .text that SHRANK by 4096 bytes")
    if not gate_failures([], [], resized, [], 0, None, True):
        problems.append("--fail-on-symbol-set-change passed a resized symbol at zero net delta")
    if not gate_failures([], [], [], renamed, 0, None, True):
        problems.append("--fail-on-symbol-set-change passed a renamed symbol at zero net delta")
    if not gate_failures([], [], [], [], -4096, None, False, True):
        problems.append("--fail-on-text-delta passed a .text that SHRANK by 4096 bytes")
    if gate_failures([], [], [], [], 0, None, False, True):
        problems.append("--fail-on-text-delta fired on a zero .text delta")
    for problem in problems:
        say("self-test: " + problem)
    say("self-test: " + ("OK (2 canned transcripts, 5 buckets, 3 gates)" if not problems else "FAILED"))
    return 0 if not problems else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--before", help="the baseline libMobileGL.so")
    parser.add_argument("--after", help="the libMobileGL.so under test")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--size", default="size")
    parser.add_argument("--cxxfilt", default="c++filt")
    parser.add_argument("--markdown", default=None, help="write the Markdown report here")
    parser.add_argument("--json", default=None, help="write the machine-readable result here")
    parser.add_argument("--strip-scope", action="append", default=[], metavar="PREFIX",
                        help="de-nest this scope in demangled names before comparing: PREFIX "
                             "'A::B::C::' rewrites every 'A::B::C::X' to 'A::B::X' (repeatable). "
                             "This is what folds a de-nesting into a rename instead of an "
                             "added+removed pair, including inside template arguments.")
    parser.add_argument("--rename-map", action="append", default=[], metavar="OLD=NEW",
                        help="textual OLD=NEW substitution on demangled names (repeatable)")
    parser.add_argument("--only-names", default=None,
                        help="comma-separated: list only symbols whose demangled text contains one")
    parser.add_argument("--threshold", type=int, default=0,
                        help="ignore size deltas of at most this many bytes")
    parser.add_argument("--fail-on-added-bytes", type=int, default=None,
                        help="exit non-zero when .text grew by more than this many bytes; 0 is "
                             "special and means the section must be BYTE-IDENTICAL, so a shrink "
                             "fails it too (that is what P1's G1 asks for)")
    parser.add_argument("--fail-on-text-delta", action="store_true",
                        help="exit non-zero when .text moved at all, in either direction. The "
                             "explicit spelling of what --fail-on-added-bytes 0 also does.")
    parser.add_argument("--fail-on-symbol-set-change", action="store_true",
                        help="exit non-zero when any defined symbol was added, removed, resized or "
                             "renamed (the four buckets of the report; --strip-scope decides which "
                             "of added+removed vs renamed a de-nesting lands in, it does not "
                             "excuse it). Compared at threshold 0 whatever --threshold says.")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if not args.before or not args.after:
        parser.error("--before and --after are required (or use --self-test)")

    if args.fail_on_added_bytes is not None and args.fail_on_added_bytes < 0:
        parser.error("--fail-on-added-bytes takes a byte budget of 0 or more")

    rename_map = []
    for entry in args.rename_map:
        if "=" not in entry:
            parser.error("--rename-map expects OLD=NEW, got " + entry)
        old, new = entry.split("=", 1)
        rename_map.append((old, new))
    only_names = [n.strip() for n in args.only_names.split(",")] if args.only_names else []

    before, before_sections, before_count = build_side(
        args.before, args.nm, args.size, args.cxxfilt, args.strip_scope, rename_map)
    after, after_sections, after_count = build_side(
        args.after, args.nm, args.size, args.cxxfilt, args.strip_scope, rename_map)

    removed, added, resized, renamed, unchanged = bucket(before, after, only_names, args.threshold)
    # --threshold is a REPORT control - "do not list resizes smaller than this" - and a gate that
    # read the thresholded buckets would quietly weaken itself the day someone raised it. The gates
    # always see the threshold-0 buckets.
    if args.threshold:
        gate_removed, gate_added, gate_resized, gate_renamed, _ = bucket(before, after, only_names, 0)
    else:
        gate_removed, gate_added, gate_resized, gate_renamed = removed, added, resized, renamed

    before_text = before_sections.get(".text", 0)
    after_text = after_sections.get(".text", 0)
    delta = after_text - before_text
    percent = (100.0 * delta / before_text) if before_text else 0.0

    say("before: {} ({} bytes on disk)".format(args.before, os.path.getsize(args.before)))
    say("after : {} ({} bytes on disk)".format(args.after, os.path.getsize(args.after)))
    if args.strip_scope:
        say("strip-scope: " + " ; ".join(args.strip_scope))
    if rename_map:
        say("rename-map: " + " ; ".join("{}={}".format(o, n) for o, n in rename_map))
    say(".text {} -> {} ({:+d}, {:+.3f}%)".format(before_text, after_text, delta, percent))
    for section in INTERESTING_SECTIONS:
        if section == ".text":
            continue
        b = before_sections.get(section)
        a = after_sections.get(section)
        if b is None and a is None:
            continue
        say("{} {} -> {} ({:+d})".format(section, b or 0, a or 0, (a or 0) - (b or 0)))
    say("{} -> {} defined symbols: {} added, {} removed, {} resized, {} renamed".format(
        before_count, after_count, len(added), len(removed), len(resized), len(renamed)))
    # The two counts differ by the folding: several mangled symbols can share one
    # normalised demangled name (local aliases, `.cold` parts, identical-COMDAT clones).
    say("{} -> {} normalised names, {} unchanged (name, size and mangling all identical)".format(
        len(before), len(after), unchanged))
    if only_names:
        say("listing restricted to names containing: " + ", ".join(only_names))
    gates = gate_failures(gate_added, gate_removed, gate_resized, gate_renamed, delta,
                          args.fail_on_added_bytes, args.fail_on_symbol_set_change,
                          args.fail_on_text_delta)
    if (args.fail_on_added_bytes is None and not args.fail_on_symbol_set_change
            and not args.fail_on_text_delta):
        say("no gate flag: informational run")
    elif args.threshold:
        say("gates compare at threshold 0 ({} added, {} removed, {} resized, {} renamed there); "
            "--threshold {} only shortens the report".format(
                len(gate_added), len(gate_removed), len(gate_resized), len(gate_renamed),
                args.threshold))

    lines = ["# MobileGL symbol report", "",
             "| side | path | file bytes | .text |",
             "|---|---|---|---|",
             "| before | `{}` | {} | {} |".format(args.before, os.path.getsize(args.before), before_text),
             "| after | `{}` | {} | {} |".format(args.after, os.path.getsize(args.after), after_text),
             "",
             "`.text` {} -> {} ({:+d}, {:+.3f}%). {} -> {} defined symbols: "
             "{} added, {} removed, {} resized, {} renamed, {} unchanged.".format(
                 before_text, after_text, delta, percent, before_count, after_count,
                 len(added), len(removed), len(resized), len(renamed), unchanged)]
    lines += markdown_table("Removed", [(r["name"], r["before"]) for r in removed],
                            ["symbol", "bytes"])
    lines += markdown_table("Added", [(r["name"], r["after"]) for r in added],
                            ["symbol", "bytes"])
    lines += markdown_table("Resized", [(r["name"], r["before"], r["after"], "{:+d}".format(r["delta"]))
                                        for r in resized],
                            ["symbol", "before", "after", "delta"])
    lines += markdown_table("Renamed only (same size)",
                            [(r["name"], r["before"], r["after"]) for r in renamed],
                            ["normalised symbol", "before mangling", "after mangling"])
    report = "\n".join(lines) + "\n"

    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(report)
        say("markdown written to " + args.markdown)
    else:
        print(report)

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as handle:
            json.dump({"before": args.before, "after": args.after,
                       "sections_before": before_sections, "sections_after": after_sections,
                       "removed": removed, "added": added, "resized": resized,
                       "renamed": renamed, "unchanged": unchanged}, handle, indent=2)
            handle.write("\n")
        say("json written to " + args.json)

    # After the report is written, never before: a gate that fired is exactly when someone needs
    # the Markdown and the JSON.
    for reason in gates:
        say("FAIL " + reason)
    return 1 if gates else 0


if __name__ == "__main__":
    sys.exit(main())
