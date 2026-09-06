#!/usr/bin/env python3
# MobileGL - scripts/check_include_closure.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""The P0.5 interface-purity gate: what the extracted headers are allowed to include.

ROADMAP.md P0.5 asks for a CI assertion on the include CLOSURE of the two headers the
phase extracts, and for a negative control that goes red when an `MG_State` include is
added back (ROADMAP.md:7 - every gate must be able to fail for the reason it exists).
`nm --undefined-only` cannot do that job: a header that is included but whose types are
never named leaves no symbol behind, and "included at all" is exactly the coupling P1
and P7 have to be able to sever. So the arbiter here is the preprocessor's own
`-H` transcript, plus a literal `#include` walk that needs no compiler at all.

    probe            header                                        forbidden in its closure
    ---------------- --------------------------------------------- -------------------------------
    value-header     MG_Pipe/MGPipeValueTypes.h                    MG_State/ MG_Impl/ MG_Backend/ MG_Remote/
    artifacts-header MG_State/.../ProgramState/ProgramArtifacts.h  ShaderObject.h, ShaderTranspiler/,
                                                                   Config.h, MG_Backend/, BufferState/,
                                                                   ProgramState/Shader*  (+ <= 2 `glslang::`)
    wire-header      MG_Remote/Transport/ITransport.h              MobileGL/Includes.h

The forbidden sets deliberately say nothing about glslang, spirv-cross or vulkan:
MobileGL/Includes.h pulls all three unconditionally (:53,:56,:59,:80-84,:130) and both
new headers are allowed `<Includes.h>`, so a "textually glslang-free closure" assertion
would be unsatisfiable by construction. P7 measures that with `nm -D | grep glslang` on
the server binary instead - a symbol gate, not a text gate.

Two modes, and they check different things:

    --mode text     transitive walk of literal `#include` lines. No compiler, no
                    submodules, blind to `#if`. This is what the ctest runs, because the
                    CI `test` job's runner has no submodules checked out.
    --mode clang    `-H -E` on a one-line probe TU, which is the ground truth, plus a
                    `-fsyntax-only` pass that proves the header is self-contained.
    --mode both     run both and additionally fail if they disagree about the violations.

D9 (brief section B.0): a probe whose header does not exist yet prints SKIP and is
counted; `--require-all` turns every SKIP into a failure. That is what lets this package
land BEFORE the two headers do, and what stops an all-SKIP run from passing for free
once they exist - the integrator flips `--require-all` on as the ratchet.

    python3 scripts/check_include_closure.py --mode text --self-test
    python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test
    python3 scripts/check_include_closure.py --mode both --self-test --require-all
"""

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PREFIX = "include-closure: "

# The probe manifest. Header/Tu/Forbidden are the fixed contract of brief section B.0:
# the gate is coded against these paths before the headers they name exist.
#
#   Allow entries are deliberately awkward: each one needs a Reason that is printed in
#   the summary of every run, so that a whitelist entry has to be argued for in the pull
#   request instead of appearing quietly (test.yml:831-835 house rule).
PROBES = [
    {
        "Name": "value-header",
        "Header": "MobileGL/MG_Pipe/MGPipeValueTypes.h",
        "Tu": "#include <MG_Pipe/MGPipeValueTypes.h>\n",
        "Forbidden": [
            "MobileGL/MG_State/",
            "MobileGL/MG_Impl/",
            "MobileGL/MG_Backend/",
            "MobileGL/MG_Remote/",
        ],
        "Allow": [],
        "TextLimits": {},
        "Why": "MG_Pipe is below MG_State (ARCHITECTURE.md:550-561): the value types P1 hands "
               "the backend must not drag the frontend state tree back in.",
    },
    {
        "Name": "artifacts-header",
        "Header": "MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h",
        "Tu": "#include <MG_State/GLState/ProgramState/ProgramArtifacts.h>\n",
        "Forbidden": [
            "MobileGL/MG_State/GLState/ProgramState/ShaderObject.h",
            "MobileGL/MG_Util/ShaderTranspiler/",
            "MobileGL/Config.h",
            "MobileGL/MG_Backend/",
            "MobileGL/MG_State/GLState/BufferState/",
            # prefix, on purpose: ShaderStage.h, ShaderCompileTask.h,
            # ShaderCompileAdoptionMap.h, ShaderPreprocessCache.h, ShaderSourceKey.h
            "MobileGL/MG_State/GLState/ProgramState/Shader",
        ],
        "Allow": [],
        # D5: LinkArtifacts keeps exactly two glslang-typed members and no more.
        "TextLimits": {"glslang::": 2},
        "Why": "P7 ships the reflection artifacts over the wire; the archive header must not "
               "depend on the compiler front end that produced them.",
    },
    {
        "Name": "wire-header",
        "Header": "MobileGL/MG_Remote/Transport/ITransport.h",
        "Tu": "#include <MG_Remote/Transport/ITransport.h>\n",
        "Forbidden": ["MobileGL/Includes.h"],
        "Allow": [],
        "TextLimits": {},
        "Why": "WireLog.h:9-24 states the rule: nothing about a byte pipe needs the GL "
               "frontend's umbrella header. Green today - it is the gate's own canary.",
    },
]

# Deleting a probe must be red, not quietly green.
REQUIRED_PROBE_NAMES = ("value-header", "artifacts-header")

# Measured to reproduce the closure the real build sees (666 lines for the negative
# control TU), without a CMake configure.
DEFAULT_CLANG_FLAGS = [
    "-std=gnu++23",
    "-Iinclude",
    "-IMobileGL",
    "-IMobileGL/MG_Pipe",
    "-I3rdparty/xxHash",
    "-I3rdparty/Vulkan-Headers/include",
]

# The only submodule headers Includes.h reaches; glslang and spirv_cross are vendored.
CLANG_MODE_PREREQS = [
    "include/ska/flat_hash_map.hpp",
    "3rdparty/xxHash/xxhash.h",
    "3rdparty/Vulkan-Headers/include/vulkan/vulkan.h",
]

COMPILER_CANDIDATES = ["clang++-20", "clang++", "c++", "g++"]

# Text-mode angle-bracket search path, in the order MOBILEGL_INCLUDE_DIR lists it
# (CMakeLists.txt:529-541); MG_Pipe is there twice on purpose (CMakeLists.txt:531,535).
SEARCH_DIRS = [
    "include",
    "MobileGL",
    "MobileGL/MG_Pipe",
    "3rdparty/xxHash",
    "3rdparty/Vulkan-Headers/include",
]

# `-H` writes to stderr, one line per file actually opened: dots for depth, then the path
# as spelled on the search path. g++ appends a "Multiple include guards may be useful
# for:" paragraph of bare paths, which must not be mistaken for depth-0 includes.
H_LINE_RE = re.compile(r"^(\.+) (.*)$")

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]')


def say(message):
    print(PREFIX + message)


def error(message):
    print("::error::" + message)


def repo_relative(path, cwd):
    """normpath the -H spelling, then express it repo-relative with forward slashes.

    Today's transcripts contain e.g. TextureState/../SamplerState/SamplerObject.h, which
    only matches a forbidden prefix after normalisation. Anything outside the repo (the
    toolchain's own headers, /usr/include) is not our business and returns None.
    """
    absolute = os.path.normpath(os.path.join(cwd, path))
    try:
        relative = os.path.relpath(absolute, REPO_ROOT)
    except ValueError:  # different drive on Windows
        return None
    if relative == os.pardir or relative.startswith(os.pardir + os.sep):
        return None
    return relative.replace(os.sep, "/")


def parse_h_output(text, cwd):
    """-H transcript -> [(depth, repo_relative_or_None, raw_spelling)] in file order."""
    entries = []
    for line in text.splitlines():
        match = H_LINE_RE.match(line)
        if not match:
            continue
        entries.append((len(match.group(1)), repo_relative(match.group(2), cwd), match.group(2)))
    return entries


def resolve_include(spec, quoted, from_dir):
    candidates = []
    if quoted:
        candidates.append(os.path.join(from_dir, spec))
    for directory in SEARCH_DIRS:
        candidates.append(os.path.join(REPO_ROOT, directory, spec))
    for candidate in candidates:
        candidate = os.path.normpath(candidate)
        if os.path.isfile(candidate):
            return candidate
    return None


def text_closure(tu_text, tu_dir):
    """Transitive walk of literal #include lines.

    There are no macro-driven includes under MobileGL/ (3377 include lines, all literal),
    so a literal walk is exact apart from `#if`, which this mode is blind to by design -
    the compiler mode is the arbiter and --mode both cross-checks the two.
    A file is walked once, which is what an include guard does to the -H transcript too.
    """
    entries = []
    seen = set()

    def walk(text, from_dir, depth):
        for line in text.splitlines():
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            path = resolve_include(match.group(2), match.group(1) == '"', from_dir)
            if path is None:  # unresolved (libstdc++, EGL/, GL/, glslang/, ...) = leaf
                continue
            if path in seen:
                continue
            seen.add(path)
            entries.append((depth, repo_relative(path, REPO_ROOT), path))
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    nested = handle.read()
            except OSError:
                continue
            walk(nested, os.path.dirname(path), depth + 1)

    walk(tu_text, tu_dir, 1)
    return entries


def find_violations(entries, forbidden, allow):
    violations = []
    for index, (depth, relative, _raw) in enumerate(entries):
        if relative is None:
            continue
        hit = next((f for f in forbidden if relative.startswith(f)), None)
        if hit is None:
            continue
        if any(relative.startswith(a["Path"]) for a in allow):
            continue
        violations.append({"index": index, "depth": depth, "path": relative, "rule": hit})
    return violations


def chain_for(entries, index):
    """Walk back to depth-1, depth-2, ... 1: the include chain that pulled the violation in."""
    chain = []
    wanted = entries[index][0]
    cursor = index
    while cursor >= 0 and wanted >= 1:
        depth, relative, raw = entries[cursor]
        if depth == wanted:
            chain.append((depth, relative or raw))
            wanted -= 1
        cursor -= 1
    return list(reversed(chain))


def count_text_limits(header_path, limits):
    """Token budgets on the header's own text (D5 pins `glslang::` at 2)."""
    problems = []
    if not limits:
        return problems
    with open(header_path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    for token, budget in sorted(limits.items()):
        found = text.count(token)
        if found > budget:
            problems.append("{} occurs {} times, budget is {}".format(token, found, budget))
    return problems


def pick_compiler(explicit):
    if explicit:
        found = shutil.which(explicit) or (explicit if os.path.isfile(explicit) else None)
        if not found:
            error("compiler not found: " + explicit)
            sys.exit(1)
        return found
    env_cxx = os.environ.get("CXX")
    if env_cxx:
        found = shutil.which(env_cxx) or (env_cxx if os.path.isfile(env_cxx) else None)
        if found:
            return found
    for candidate in COMPILER_CANDIDATES:
        found = shutil.which(candidate)
        if found:
            return found
    error("no C++ preprocessor found (tried $CXX, " + ", ".join(COMPILER_CANDIDATES) + ")")
    sys.exit(1)


def flags_from_compile_commands(path):
    """Any entry whose `file` is under MobileGL/ carries the flags the real build uses."""
    with open(path, "r", encoding="utf-8") as handle:
        database = json.load(handle)
    keep_prefixes = ("-D", "-I", "-isystem", "-std")
    for entry in database:
        source = entry.get("file", "")
        if "MobileGL/" not in source.replace(os.sep, "/"):
            continue
        tokens = entry.get("arguments")
        if tokens is None:
            tokens = shlex.split(entry.get("command", ""))
        flags = []
        skip_next = False
        for token in tokens[1:]:
            if skip_next:
                skip_next = False
                continue
            if token in ("-o", "-c", "-MD", "-MF", "-MT", "-MMD", "-MQ"):
                skip_next = token in ("-o", "-MF", "-MT", "-MQ")
                continue
            if token == "-isystem":
                skip_next = False
                flags.append(token)
                continue
            if token.startswith(keep_prefixes):
                flags.append(token)
                continue
            # the input file and everything else is dropped
        return flags, entry.get("directory", REPO_ROOT)
    error("no MobileGL/ entry in " + path)
    sys.exit(1)


def check_clang_prereqs():
    missing = [p for p in CLANG_MODE_PREREQS if not os.path.isfile(os.path.join(REPO_ROOT, p))]
    if missing:
        error("clang mode needs " + ", ".join(missing) + " - run from a full checkout: "
              "git submodule update --init include/ska 3rdparty/xxHash 3rdparty/Vulkan-Headers")
        sys.exit(1)


def run_preprocessor(compiler, flags, tu_path, cwd):
    command = [compiler] + list(flags) + ["-H", "-E", "-o", os.devnull, tu_path]
    completed = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    return completed


def run_syntax_only(compiler, flags, tu_path, cwd):
    command = [compiler] + list(flags) + ["-fsyntax-only", tu_path]
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def closure_for_tu(mode, tu_text, context):
    """Return (entries, note). `context` carries compiler/flags/cwd/tmpdir."""
    if mode == "text":
        return text_closure(tu_text, REPO_ROOT), None
    tu_path = os.path.join(context["tmpdir"], "probe_%d.cpp" % context["counter"][0])
    context["counter"][0] += 1
    with open(tu_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(tu_text)
    completed = run_preprocessor(context["compiler"], context["flags"], tu_path, context["cwd"])
    if completed.returncode != 0:
        return parse_h_output(completed.stderr, context["cwd"]), completed.stderr.strip()
    return parse_h_output(completed.stderr, context["cwd"]), None


def print_chain(entries, violation):
    for depth, path in chain_for(entries, violation["index"]):
        say("    " + ("  " * (depth - 1)) + path)


def run_probe(probe, mode, context, results):
    header_abs = os.path.join(REPO_ROOT, probe["Header"])
    entries, note = closure_for_tu(mode, probe["Tu"], context)
    if note:
        error("{} preprocessing failed:\n{}".format(probe["Name"], note))
        results.append({"probe": probe["Name"], "mode": mode, "status": "ERROR",
                        "headers": len(entries), "forbidden": 0})
        return "ERROR", set()

    violations = find_violations(entries, probe["Forbidden"], probe["Allow"])
    text_problems = count_text_limits(header_abs, probe["TextLimits"])
    status = "OK" if not violations and not text_problems else "FORBIDDEN"
    say("{} {} {} headers in closure, {} forbidden (mode={}, cxx={})".format(
        probe["Name"], status, len(entries), len(violations), mode,
        context["compiler_label"] if mode == "clang" else "-"))
    for allowed in probe["Allow"]:
        say("    ALLOW {} - {}".format(allowed["Path"], allowed["Reason"]))
    for violation in violations:
        say("    forbidden: {} (rule {}) reached by:".format(violation["path"], violation["rule"]))
        print_chain(entries, violation)
    for problem in text_problems:
        say("    text limit: " + problem)

    results.append({"probe": probe["Name"], "mode": mode, "status": status,
                    "headers": len(entries), "forbidden": len(violations),
                    "violations": [v["path"] for v in violations],
                    "text_problems": text_problems})
    return status, {v["path"] for v in violations}


def run_self_contained_check(probe, context):
    """Second assertion in clang mode: the header compiles on its own."""
    tu_path = os.path.join(context["tmpdir"], "syntax_%s.cpp" % probe["Name"].replace("-", "_"))
    with open(tu_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(probe["Tu"])
    completed = run_syntax_only(context["compiler"], context["flags"], tu_path, context["cwd"])
    if completed.returncode == 0:
        say("{} SELF-CONTAINED OK (-fsyntax-only, cxx={})".format(
            probe["Name"], context["compiler_label"]))
        return True
    say("{} SELF-CONTAINED FAILED (-fsyntax-only, cxx={})".format(
        probe["Name"], context["compiler_label"]))
    for line in completed.stderr.strip().splitlines()[:20]:
        say("    " + line)
    return False


# --------------------------------------------------------------------------------------
# self-test: the always-on proof that the gate is capable of failing
# --------------------------------------------------------------------------------------

PARSER_CANNED_GXX = """. MobileGL/MG_Pipe/MGPipeHandles.h
.. MobileGL/Includes.h
... MobileGL/MG_State/GLState/TextureState/../SamplerState/SamplerObject.h
Multiple include guards may be useful for:
/usr/include/bits/byteswap.h
/usr/include/bits/confname.h
"""


def self_test_parser():
    """Check 4: the trailer paragraph is discarded and a `../` spelling normalises."""
    problems = []
    entries = parse_h_output(PARSER_CANNED_GXX, REPO_ROOT)
    if len(entries) != 3:
        problems.append("g++ -H trailer not discarded: parsed {} lines, expected 3".format(len(entries)))
    depths = [e[0] for e in entries]
    if depths != [1, 2, 3]:
        problems.append("depths misparsed: {}".format(depths))
    normalised = entries[-1][1] if entries else None
    if normalised != "MobileGL/MG_State/GLState/SamplerState/SamplerObject.h":
        problems.append("`../` spelling did not normalise: {}".format(normalised))
    elif not find_violations(entries, PROBES[0]["Forbidden"], []):
        problems.append("normalised path did not match the value-header forbidden list")
    if problems:
        for problem in problems:
            say("    self-test parser: " + problem)
        return False
    say("    self-test parser: trailer discarded, `../` normalised and matched")
    return True


def negative_controls():
    """The three canned TUs of B.3, gated on which headers exist yet."""
    controls = [
        {
            "Name": "control-1 (independent of P0.5)",
            "Tu": "#include <MG_Pipe/MGPipeHandles.h>\n"
                  "#include <MG_State/GLState/RenderState/RenderState.h>\n",
            "Forbidden": PROBES[0]["Forbidden"],
            "ExpectDepth1": "MobileGL/MG_State/GLState/RenderState/RenderState.h",
        },
    ]
    value_header = os.path.join(REPO_ROOT, PROBES[0]["Header"])
    if os.path.isfile(value_header):
        controls.append({
            "Name": "control-2 (MG_State include added back to the value header's TU)",
            "Tu": PROBES[0]["Tu"] + "#include <MG_State/GLState/RenderState/RenderState.h>\n",
            "Forbidden": PROBES[0]["Forbidden"],
            "ExpectDepth1": "MobileGL/MG_State/GLState/RenderState/RenderState.h",
        })
    artifacts_header = os.path.join(REPO_ROOT, PROBES[1]["Header"])
    if os.path.isfile(artifacts_header):
        controls.append({
            "Name": "control-3 (ShaderObject.h added back to the artifacts header's TU)",
            "Tu": PROBES[1]["Tu"] + "#include <MG_State/GLState/ProgramState/ShaderObject.h>\n",
            "Forbidden": PROBES[1]["Forbidden"],
            "ExpectDepth1": "MobileGL/MG_State/GLState/ProgramState/ShaderObject.h",
        })
    return controls


def self_test_text_limit(context):
    """Check 3b: a third `glslang::` token in a copy of the artifacts header must trip."""
    header = os.path.join(REPO_ROOT, PROBES[1]["Header"])
    if not os.path.isfile(header):
        return None
    copy = os.path.join(context["tmpdir"], "ProgramArtifacts_with_a_third_glslang.h")
    with open(header, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    with open(copy, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text + "\n// synthesized third token: glslang::TIntermediate\n")
    problems = count_text_limits(copy, PROBES[1]["TextLimits"])
    if problems:
        say("    self-test text limit: tripped as expected ({})".format(problems[0]))
        return True
    say("    self-test text limit: a third `glslang::` token did NOT trip the budget")
    return False


def self_test(modes, context):
    say("self-test: negative controls (must trip) in mode(s) " + ", ".join(modes))
    ok = self_test_parser()
    trips = 0
    for control in negative_controls():
        for mode in modes:
            entries, note = closure_for_tu(mode, control["Tu"], context)
            if note:
                say("    self-test {} [{}]: preprocessing failed".format(control["Name"], mode))
                ok = False
                continue
            violations = find_violations(entries, control["Forbidden"], [])
            at_depth_1 = [v for v in violations
                          if v["depth"] == 1 and v["path"] == control["ExpectDepth1"]]
            if violations and at_depth_1:
                trips += 1
                say("    self-test {} [{}]: tripped, {} violation(s), {} at depth 1".format(
                    control["Name"], mode, len(violations), control["ExpectDepth1"]))
            else:
                ok = False
                say("    self-test {} [{}]: DID NOT TRIP ({} violation(s), depth-1 hit: {})".format(
                    control["Name"], mode, len(violations), bool(at_depth_1)))
    limit_result = self_test_text_limit(context)
    if limit_result is False:
        ok = False
    if trips == 0:
        error("negative control did not trip: the closure gate is not checking anything")
        return False
    say("self-test: {} negative-control trip(s), parser checks {}".format(
        trips, "OK" if ok else "FAILED"))
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mode", choices=["text", "clang", "both"], default="text",
                        help="literal #include walk, the preprocessor's -H transcript, or both")
    parser.add_argument("--compiler", default=None,
                        help="preprocessor to use; default $CXX, then " + ", ".join(COMPILER_CANDIDATES))
    parser.add_argument("--compile-commands", default=None,
                        help="take -D/-I/-isystem/-std from a compile_commands.json entry under MobileGL/")
    parser.add_argument("--probe", action="append", default=None,
                        help="restrict to this probe (repeatable)")
    parser.add_argument("--self-test", action="store_true",
                        help="run the negative controls and the parser checks (always on in CI)")
    parser.add_argument("--require-all", action="store_true",
                        help="a SKIP (header not present yet) is a failure")
    parser.add_argument("--json", default=None, help="write the machine-readable result here")
    args = parser.parse_args()

    names = [p["Name"] for p in PROBES]
    for required in REQUIRED_PROBE_NAMES:
        if required not in names:
            error("probe `{}` is missing from the manifest in {}".format(
                required, os.path.basename(__file__)))
            return 1

    modes = ["text", "clang"] if args.mode == "both" else [args.mode]
    if "clang" in modes:
        check_clang_prereqs()

    tmpdir = tempfile.mkdtemp(prefix="mgl-include-closure-")
    context = {
        "tmpdir": tmpdir,
        "counter": [0],
        "cwd": REPO_ROOT,
        "flags": list(DEFAULT_CLANG_FLAGS),
        "compiler": None,
        "compiler_label": "-",
    }
    if "clang" in modes:
        compiler = pick_compiler(args.compiler)
        context["compiler"] = compiler
        context["compiler_label"] = compiler
        if args.compile_commands:
            flags, directory = flags_from_compile_commands(args.compile_commands)
            context["flags"] = flags
            context["cwd"] = directory
            say("flags from {} ({} tokens), cwd={}".format(
                args.compile_commands, len(flags), directory))
        say("compiler: " + compiler)

    selected = [p for p in PROBES if args.probe is None or p["Name"] in args.probe]
    if args.probe:
        unknown = sorted(set(args.probe) - set(names))
        if unknown:
            error("unknown probe(s): " + ", ".join(unknown))
            return 1

    results = []
    problems = 0
    skipped = 0
    for probe in selected:
        # D9: a header that does not exist yet is one SKIP line, counted once, and a
        # failure only under --require-all (the ratchet the integrator flips).
        if not os.path.isfile(os.path.join(REPO_ROOT, probe["Header"])):
            say("{} SKIP  header not present yet: {}".format(probe["Name"], probe["Header"]))
            results.append({"probe": probe["Name"], "mode": "+".join(modes), "status": "SKIP",
                            "headers": 0, "forbidden": 0})
            skipped += 1
            if args.require_all:
                problems += 1
                say("{} SKIP is a failure under --require-all: {} does not exist".format(
                    probe["Name"], probe["Header"]))
            continue
        per_mode_status = {}
        per_mode_violations = {}
        for mode in modes:
            status, violations = run_probe(probe, mode, context, results)
            per_mode_status[mode] = status
            per_mode_violations[mode] = violations
        if any(s not in ("OK",) for s in per_mode_status.values()):
            problems += 1
        if len(modes) > 1:
            sets = list(per_mode_violations.values())
            if sets[0] != sets[1]:
                problems += 1
                say("{} MODE DISAGREEMENT - text and clang do not see the same violations".format(
                    probe["Name"]))
                say("    text : " + (", ".join(sorted(per_mode_violations["text"])) or "(none)"))
                say("    clang: " + (", ".join(sorted(per_mode_violations["clang"])) or "(none)"))
        if "clang" in modes:
            if not run_self_contained_check(probe, context):
                problems += 1

    self_test_ok = True
    if args.self_test:
        self_test_ok = self_test(modes, context)
        if not self_test_ok:
            problems += 1

    say("{} probes, {} skipped, {} problem(s)".format(len(selected), skipped, problems))

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as handle:
            json.dump({"modes": modes, "results": results, "skipped": skipped,
                       "problems": problems, "self_test": self_test_ok,
                       "require_all": args.require_all}, handle, indent=2)
            handle.write("\n")

    if problems:
        error("include-closure gate failed: {} problem(s); see the chains above".format(problems))
        return 1
    return 0


if __name__ == "__main__":
    sys.setrecursionlimit(10000)
    sys.exit(main())
