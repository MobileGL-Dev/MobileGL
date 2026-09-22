#!/usr/bin/env python3
# MobileGL - scripts/link_ratchet.py
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header
"""The link-closure ratchet: which frontend symbols would a server-only image still need?

CONTRACT-P6.md 12.1 records the debt as 184 symbols and says "a ratchet recomputes the 184
in CI and asserts it only ever falls". This is that ratchet. The measurement is a6's
(docs/Disaggregated/notes/p6/a6-link-experiment.md), mechanised:

    (SERVER undefined) - (SERVER defined) - (SHARED defined)  intersect  (FRONTEND defined)

i.e. the undefined-symbol list a real link of a server-only image would print today. There is
no module target to link - `SOURCE_FILES` is one flat list feeding `MobileGL` and `MobileGL_s`
(CMakeLists.txt) and establishing module boundaries is P13's named work - so the partition is
applied to the per-object output of the single `MobileGL` target instead. That is exactly why
the partition lives in PARTITION below as an explicit table with a rationale per row rather
than as a regex someone has to reverse-engineer: it IS the module boundary, written down.

    python3 scripts/link_ratchet.py --build-dir build-split
    python3 scripts/link_ratchet.py --build-dir build-split --bucket
    python3 scripts/link_ratchet.py --build-dir build-split \
        --baseline scripts/data/link_ratchet_baseline.txt --assert-monotone \
        --json link-ratchet.json
    python3 scripts/link_ratchet.py --self-test

Exit codes: 0 green, 1 the ratchet itself went red (new frontend reach), 2 the run could not
be trusted to mean anything (an object no PARTITION row claims, or a build configured without
the three disaggregated options). The two are separate on purpose - a 2 is never "the code got
worse", it is "fix the measurement first".

WHAT THE GATE IS FOR (ID-P7-7). The baseline stores the symbol LIST, not a count. A count-only
ratchet passes on the day one symbol is cleared and another added, which is precisely the
regression this gate exists to catch: new backend code reaching into a frontend object. So
--assert-monotone fails on symbols present now and ABSENT from the baseline, and merely reports
symbols that disappeared, with an instruction to re-baseline in the same commit. Progress is
never a red build; new reach always is.

GUARD RAILS. The build must be a disaggregated one
(-DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON
-DMOBILEGL_PIPE_PUSH=ON): with the option OFF, MG_Remote is not compiled at all, the SERVER set
is missing its transport half and the script would report a cheerfully small number that means
nothing. The run reads CMakeCache.txt and refuses such a build by name unless it is given
--no-require-flags. Two more refusals for the same reason - every way this gate can go green
without having measured anything is an explicit exit 2:
  * an object that matches no PARTITION row (the tree moved and the partition did not), and
  * an empty SERVER, FRONTEND or SHARED set (the target was never built in that directory,
    which otherwise classifies perfectly into three empty sets and reports a beautiful zero).

SYMBOL IDENTITY, and the one place this is looser than a linker. nm is run with -C, so the unit
of the set arithmetic is the DEMANGLED name - what a6 published and what a reviewer can read
and grep for. That is deliberately not the linker's unit. Measured on the base tree, the
FRONTEND set defines 14974 distinct manglings under 14880 distinct demangled spellings: the 94
collisions are overwhelmingly the C1/C2 (complete/base) constructor and D1/D2 destructor pairs,
which demangle to the same text by construction, plus `[abi:cxx11]` tags and `.cold` parts. The
consequence to know about: if a server object ever referenced the C1 form of a constructor that
the frontend defined only as C2, this script would call it satisfied and a real link would not.
Both forms are emitted together in practice, so no such case exists today - but a demangled
ratchet is a readable approximation of a link, not a link. `scripts/symbol_report.py` is the
tool that works in manglings when the mangling is the point.

Only `U` counts as a reference. A weak undefined (`w`) resolves to zero when nothing defines
it, so it obliges nobody; on the base tree no `w` reaches a frontend definition, so this
filter costs nothing today and keeps the number honest the day it does.

THE BASELINE IS TOOLCHAIN-SENSITIVE, and this is the one way this gate goes red without any
code having got worse. Whether a small method is inlined or left as an out-of-line call is a
compiler-version decision, and it moves symbols into and out of the reach set. The stored
baseline was generated with a different clang than CI's `clang++-20` (same libstdc++, same
Release, same three options). If the first CI run is red, read the uploaded JSON before
touching anything: if every entry in `new_since_baseline` is a sibling method of a class
already in the list and brings NO new referring object, it is an inlining difference and the
answer is one deliberate re-baseline commit that says so. A new REFERRING OBJECT is never an
inlining difference - that is the real thing this gate is looking for. An Android NDK build is
a different question again: the NDK is libc++, every `std::` spelling changes, and the whole
baseline would read as new. This gate is for Linux host builds.
"""

import argparse
import json
import os
import re
import subprocess
import sys

PREFIX = "link-ratchet: "

SERVER = "SERVER"
FRONTEND = "FRONTEND"
SHARED = "SHARED"

# ---------------------------------------------------------------------------
# The partition. This table IS the module boundary that the build system does not have.
#
# Rows are (object path prefix, role, why). Paths are relative to
# <build-dir>/CMakeFiles/<target>.dir/ and a row ending in `.o` is an exact object, not a
# prefix. Matching is LONGEST PREFIX WINS, so `MG_Remote/Client/` (FRONTEND) overrides nothing
# here only because no shorter `MG_Remote/` row exists - if one is ever added, the longer row
# still wins and the intent stays readable.
#
# Provenance: a6-link-experiment-data.md lines 6-8 name the three sets. Every row below that is
# not in a6's three lines carries "(post-a6)" in its rationale and is called out in the report,
# because a partition that silently absorbed a new directory would silently change the number.
# ---------------------------------------------------------------------------
PARTITION = (
    # --- SERVER: what a server-only image would be built from ---------------------------
    ("MobileGL/MG_Backend/", SERVER,
     "backend translation - GL/Vulkan command submission is the server's entire job"),
    ("MobileGL/MG_Pipe/", SERVER,
     "the applier and its routing decode into the backend inside the server process"),
    ("MobileGL/MG_Remote/Transport/", SERVER,
     "the transport stack; the server end of every link lives here"),
    ("MobileGL/MG_Remote/Protocol/", SERVER,
     "wire schema helpers used by the server's decode path"),
    ("MobileGL/MG_Remote/Wire/", SERVER,
     "the wire codec the server decodes with"),
    ("MobileGL/MG_Remote/Server/", SERVER,
     "the server loop, session, applier and spawn"),
    ("MobileGL/MG_Remote/CapsCodec.cpp.o", SERVER,
     "the server encodes the caps blob the client mirrors; a6 names this object by hand"),

    # --- FRONTEND: what a server-only image is supposed to be able to drop ---------------
    ("MobileGL/MG_Impl/", FRONTEND,
     "the GL/EGL/GLX entry points and the client-side pipe fill - guest-process code"),
    ("MobileGL/MG_State/", FRONTEND,
     "the GL object model; the server is meant to see records on the wire, not these objects"),
    ("MobileGL/MG_Remote/Client/", FRONTEND,
     "the client half of the session - by definition not in a server image"),

    # --- SHARED: linked into both images, so it satisfies references without being reach --
    ("MobileGL/MG_Util/", SHARED,
     "role-neutral utilities: converters, transpiler, metrics, math"),
    ("MobileGL/MG_Remote/FatalFunnel.cpp.o", SHARED,
     "(post-a6, landed in 5968863f) every session death funnels through it from BOTH roles, "
     "so it is role-neutral infrastructure. a6 predates the file and names no rule for it"),
    ("MobileGL/Init.cpp.o", SHARED,
     "library init, runs in whichever process loaded the library"),
    ("MobileGL/GlobalObjects.cpp.o", SHARED,
     "the process-global singletons both roles reach"),
    ("MobileGL/ConfigLoader.cpp.o", SHARED,
     "environment/config parsing, read by both roles"),
)

# ---------------------------------------------------------------------------
# Referrer families, for --bucket. a6 attributed the debt to P7 (Magma), P3b/P4b (Espryt),
# both, and "P6's own" - which it derived by hand from who REFERS to each symbol. Same table
# shape, same longest-prefix rule; every SERVER object falls in exactly one family.
# ---------------------------------------------------------------------------
FAMILIES = (
    ("MobileGL/MG_Backend/DirectVulkan/", "magma",
     "Magma / DirectVulkan - P7's full migration"),
    ("MobileGL/MG_Backend/DirectGLES/", "espryt",
     "Espryt / DirectGLES - P3b/P4b's deepening"),
    ("", "core",
     "server-side code that is neither backend: MG_Remote transport/wire/server, the MG_Pipe "
     "applier, and the backend-neutral MG_Backend glue. a6 calls this bucket 'P6's own'"),
)

BUCKETS = (
    ("p6-core", "referred to by NO backend object: the transport/applier's own account "
                "(a6: 'P6 self' = 6)"),
    ("p7-magma", "referred to only by DirectVulkan: P7's account (a6: 101)"),
    ("p3b-p4b-espryt", "referred to only by DirectGLES: P3b/P4b's account (a6: 14)"),
    ("both-backends", "referred to by both backends: falls only when both migrate (a6: 60)"),
)

A6_BUCKET_TOTALS = {"p6-core": 6, "p7-magma": 101, "p3b-p4b-espryt": 14, "both-backends": 60}
A6_TOTAL = 184

# `nm -C -o` prints "<path>:<addr-or-blanks> <type> <demangled name>". The demangled name
# contains spaces, `<`, `,` and `::`, so the type letter is located by position after the
# known object path rather than by splitting the whole line.
NM_BODY_RE = re.compile(r"^\s*([0-9a-fA-F]*)\s+([A-Za-z?])\s(.+)$")

# A baseline line may carry a trailing ` # note` annotation (ID-P7-7 asks for `# P13` on the
# symbols that cannot fall before P13). No demangled C++ name contains " #", so splitting on
# the first occurrence is unambiguous.
BASELINE_ANNOTATION = " #"

REQUIRED_FLAGS = (
    ("MOBILEGL_BUILD_DISAGGREGATED", "ON"),
    ("MOBILEGL_BUILD_DISAGGREGATED_INPROC", "ON"),
    ("MOBILEGL_PIPE_PUSH", "ON"),
)


def say(message):
    print(PREFIX + message)


def classify(obj_path, table=PARTITION):
    """Longest-prefix match against the explicit table. None means: no row claims this object."""
    best = None
    for prefix, role, _why in table:
        if obj_path == prefix or (prefix.endswith("/") and obj_path.startswith(prefix)):
            if best is None or len(prefix) > len(best[0]):
                best = (prefix, role)
    return best[1] if best else None


def family_of(obj_path, table=FAMILIES):
    """Which referrer family a SERVER object belongs to. The empty prefix row is the default."""
    best = ("", None)
    for prefix, name, _why in table:
        if prefix == "" or obj_path.startswith(prefix):
            if name is not None and len(prefix) >= len(best[0]):
                best = (prefix, name)
    return best[1]


def find_objects(build_dir, target):
    """Every .o under <build-dir>/CMakeFiles/<target>.dir, keyed by path relative to that dir."""
    root = os.path.join(build_dir, "CMakeFiles", target + ".dir")
    if not os.path.isdir(root):
        raise SystemExit(PREFIX + "no such object directory: {} (is --build-dir a configured "
                                  "CMake build, and is --target right? the disaggregated build "
                                  "has MobileGL.dir, MobileGL_s.dir and MobileGLServer.dir)"
                         .format(root))
    objects = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.endswith(".o"):
                full = os.path.join(dirpath, name)
                objects.append(os.path.relpath(full, root).replace(os.sep, "/"))
    return root, sorted(objects)


def check_flags(build_dir):
    """Refuse a build whose options make the measurement meaningless. Returns the flag lines."""
    cache = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.isfile(cache):
        return None, ["no CMakeCache.txt in {} - cannot confirm the build is disaggregated"
                      .format(build_dir)]
    values = {}
    with open(cache, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line.strip())
            if match:
                values[match.group(1)] = match.group(2)
    problems = []
    for name, wanted in REQUIRED_FLAGS:
        got = values.get(name)
        if got != wanted:
            problems.append(
                "{}={} (need {}). With the option off, MG_Remote is not compiled at all, the "
                "SERVER set has no transport half, and every number below would be a small, "
                "green, meaningless one.".format(name, got if got is not None else "<unset>",
                                                 wanted))
    return values, problems


def run_nm(nm_tool, mode, paths, cwd, chunk=150):
    """`nm -C -o --<mode>-only` over paths, returned as raw text. Chunked for argv limits."""
    out = []
    for i in range(0, len(paths), chunk):
        batch = paths[i:i + chunk]
        completed = subprocess.run([nm_tool, "-C", "-o", "--" + mode + "-only"] + batch,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, cwd=cwd)
        # nm exits non-zero on a file it cannot read; an object it merely has no symbols for is
        # not an error. Anything on stderr is worth surfacing - a silently skipped object is a
        # silently shrunk SERVER set.
        if completed.returncode != 0 and completed.stderr.strip():
            raise SystemExit(PREFIX + "nm failed: " + completed.stderr.strip())
        out.append(completed.stdout)
    return "".join(out)


def parse_nm(text, known_objects):
    """`nm -C -o` transcript -> {object: {name: type-letter}}. Keys come from known_objects."""
    table = {obj: {} for obj in known_objects}
    ordered = sorted(known_objects, key=len, reverse=True)
    for line in text.splitlines():
        if not line.strip():
            continue
        obj = None
        for candidate in ordered:
            if line.startswith(candidate + ":"):
                obj = candidate
                break
        if obj is None:
            continue
        match = NM_BODY_RE.match(line[len(obj) + 1:])
        if not match:
            continue
        table[obj][match.group(3)] = match.group(2)
    return table


def compute(roles, undefined, defined):
    """The set arithmetic, on plain dicts so the self-test can drive it without a build.

    roles     : {object: SERVER|FRONTEND|SHARED}
    undefined : {object: {name: type}}  - only SERVER objects need to be present
    defined   : {object: {name: type}}  - every object

    Returns the sorted symbol list plus the per-symbol referrer/definer attribution.
    """
    server = [o for o, r in roles.items() if r == SERVER]
    frontend = [o for o, r in roles.items() if r == FRONTEND]
    shared = [o for o, r in roles.items() if r == SHARED]

    referrers = {}
    for obj in server:
        for name, kind in undefined.get(obj, {}).items():
            # `U` only. A weak undefined (`w`) resolves to zero when nothing defines it, so it
            # is not a reference that obliges anybody.
            if kind != "U":
                continue
            referrers.setdefault(name, set()).add(obj)

    server_defined = set()
    for obj in server:
        server_defined.update(defined.get(obj, {}))
    shared_defined = set()
    for obj in shared:
        shared_defined.update(defined.get(obj, {}))

    definers = {}
    for obj in frontend:
        for name in defined.get(obj, {}):
            definers.setdefault(name, set()).add(obj)

    server_undefined = set(referrers)
    unsatisfied = server_undefined - server_defined - shared_defined
    reach = sorted(unsatisfied & set(definers))

    return {
        "symbols": reach,
        "referrers": {name: sorted(referrers[name]) for name in reach},
        "definers": {name: sorted(definers[name]) for name in reach},
        "counts": {
            "server_objects": len(server),
            "frontend_objects": len(frontend),
            "shared_objects": len(shared),
            "server_undefined": len(server_undefined),
            "unsatisfied_by_server_or_shared": len(unsatisfied),
            "defined_by_frontend": len(reach),
        },
    }


def bucketise(result):
    """Group the reach by referrer family, reproducing a6's four hand-made buckets."""
    buckets = {name: [] for name, _why in BUCKETS}
    also_core = []
    for name in result["symbols"]:
        families = {family_of(obj) for obj in result["referrers"][name]}
        backends = families & {"magma", "espryt"}
        if not backends:
            buckets["p6-core"].append(name)
        else:
            if backends == {"magma"}:
                buckets["p7-magma"].append(name)
            elif backends == {"espryt"}:
                buckets["p3b-p4b-espryt"].append(name)
            else:
                buckets["both-backends"].append(name)
            if "core" in families:
                also_core.append(name)
    return buckets, also_core


def read_baseline(path):
    """Baseline file -> (symbols, {symbol: annotation}). `#` at line start is a comment."""
    symbols = []
    notes = {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n").rstrip("\r")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if BASELINE_ANNOTATION in line:
                symbol, annotation = line.split(BASELINE_ANNOTATION, 1)
                symbol = symbol.rstrip()
                notes[symbol] = annotation.strip()
            else:
                symbol = line.rstrip()
            symbols.append(symbol)
    return symbols, notes


def monotone(current, baseline):
    """(new, gone). `new` is the red: reach that was not in the baseline."""
    current_set = set(current)
    baseline_set = set(baseline)
    return sorted(current_set - baseline_set), sorted(baseline_set - current_set)


# ---------------------------------------------------------------------------
# Self-test: the classification and the set arithmetic on synthetic nm output, no build.
# ---------------------------------------------------------------------------
CANNED_OBJECTS = {
    # SERVER
    "MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp.o": SERVER,
    "MobileGL/MG_Backend/DirectGLES/Managers.cpp.o": SERVER,
    "MobileGL/MG_Backend/BackendObject.cpp.o": SERVER,
    "MobileGL/MG_Remote/Server/PipeApplier.cpp.o": SERVER,
    "MobileGL/MG_Pipe/PipeApply.cpp.o": SERVER,
    "MobileGL/MG_Remote/CapsCodec.cpp.o": SERVER,
    # FRONTEND
    "MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp.o": FRONTEND,
    "MobileGL/MG_Impl/Pipe/SlotAllocator.cpp.o": FRONTEND,
    "MobileGL/MG_Remote/Client/ClientSession.cpp.o": FRONTEND,
    # SHARED
    "MobileGL/MG_Util/Math/Vec.cpp.o": SHARED,
    "MobileGL/MG_Remote/FatalFunnel.cpp.o": SHARED,
    "MobileGL/ConfigLoader.cpp.o": SHARED,
}

CANNED_UNDEFINED = {
    "MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp.o": {
        "MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const": "U",
        "MobileGL::MG_State::GLState::SamplerObject::GetMinFilter() const": "U",
        "MobileGL::Shared::Helper()": "U",          # satisfied by SHARED
        "MobileGL::MG_Backend::BackendObject::Tick()": "U",   # satisfied by SERVER
        "MobileGL::Nowhere::Absent()": "U",          # satisfied by nobody: not our business
        "MobileGL::MG_State::GLState::WeakOnly()": "w",       # weak undef: not a reference
    },
    "MobileGL/MG_Backend/DirectGLES/Managers.cpp.o": {
        "MobileGL::MG_State::GLState::SamplerObject::GetMinFilter() const": "U",
        "MobileGL::MG_State::GLState::SamplerObject::GetWrapS() const": "U",
    },
    "MobileGL/MG_Remote/Server/PipeApplier.cpp.o": {
        "MobileGL::MG_Remote::Client::ClientSession::NoteApplyThreadEnteredApplier()": "U",
    },
    "MobileGL/MG_Pipe/PipeApply.cpp.o": {
        "MobileGL::MG_Pipe::MGPipeSlots()": "U",
    },
    "MobileGL/MG_Backend/BackendObject.cpp.o": {},
    "MobileGL/MG_Remote/CapsCodec.cpp.o": {},
}

CANNED_DEFINED = {
    "MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp.o": {
        "MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const": "T",
        "MobileGL::MG_State::GLState::SamplerObject::GetMinFilter() const": "T",
        "MobileGL::MG_State::GLState::SamplerObject::GetWrapS() const": "T",
        "MobileGL::MG_State::GLState::WeakOnly()": "W",
    },
    "MobileGL/MG_Impl/Pipe/SlotAllocator.cpp.o": {
        "MobileGL::MG_Pipe::MGPipeSlots()": "T",
    },
    "MobileGL/MG_Remote/Client/ClientSession.cpp.o": {
        "MobileGL::MG_Remote::Client::ClientSession::NoteApplyThreadEnteredApplier()": "T",
    },
    "MobileGL/MG_Util/Math/Vec.cpp.o": {
        "MobileGL::Shared::Helper()": "T",
    },
    "MobileGL/MG_Backend/BackendObject.cpp.o": {
        "MobileGL::MG_Backend::BackendObject::Tick()": "T",
    },
    "MobileGL/MG_Remote/FatalFunnel.cpp.o": {},
    "MobileGL/ConfigLoader.cpp.o": {},
}

CANNED_NM_TRANSCRIPT = """\
MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp.o:                 U MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const
MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp.o:                 w MobileGL::MG_State::GLState::WeakOnly()
MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp.o:0000000000000010 T MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const
MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp.o:0000000000000020 W MobileGL::MG_State::GLState::WeakOnly()
this line belongs to no known object and must be ignored
"""


def self_test():
    problems = []

    # --- the partition table ---------------------------------------------------------
    expected_roles = [
        ("MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp.o", SERVER),
        ("MobileGL/MG_Pipe/PipeRoute.cpp.o", SERVER),
        ("MobileGL/MG_Remote/Transport/ShmLink.cpp.o", SERVER),
        ("MobileGL/MG_Remote/Server/ServerLoop.cpp.o", SERVER),
        ("MobileGL/MG_Remote/CapsCodec.cpp.o", SERVER),
        ("MobileGL/MG_Remote/Client/ClientSession.cpp.o", FRONTEND),
        ("MobileGL/MG_State/GLState/TextureState/TextureObject.cpp.o", FRONTEND),
        ("MobileGL/MG_Impl/GLImpl/Texture/GL_Texture.cpp.o", FRONTEND),
        ("MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/Pass.cpp.o", SHARED),
        ("MobileGL/MG_Remote/FatalFunnel.cpp.o", SHARED),
        ("MobileGL/ConfigLoader.cpp.o", SHARED),
        ("MobileGL/MG_Nowhere/Invented.cpp.o", None),
    ]
    for path, want in expected_roles:
        got = classify(path)
        if got != want:
            problems.append("classify({}) = {}, expected {}".format(path, got, want))
    # Longest prefix wins even when a shorter row would also match.
    longer = PARTITION + (("MobileGL/", SHARED, "hypothetical catch-all"),)
    if classify("MobileGL/MG_Remote/Server/ServerLoop.cpp.o", longer) != SERVER:
        problems.append("longest-prefix match lost to a shorter catch-all row")

    for path, want in (("MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp.o", "magma"),
                       ("MobileGL/MG_Backend/DirectGLES/Utils.cpp.o", "espryt"),
                       ("MobileGL/MG_Backend/BackendObject.cpp.o", "core"),
                       ("MobileGL/MG_Remote/Server/PipeApplier.cpp.o", "core"),
                       ("MobileGL/MG_Pipe/PipeApply.cpp.o", "core")):
        got = family_of(path)
        if got != want:
            problems.append("family_of({}) = {}, expected {}".format(path, got, want))

    # --- the nm parser ---------------------------------------------------------------
    parsed = parse_nm(CANNED_NM_TRANSCRIPT, [
        "MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp.o",
        "MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp.o"])
    uni = parsed["MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp.o"]
    sam = parsed["MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp.o"]
    if uni.get("MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const") != "U":
        problems.append("nm parse: undefined line with a spaced demangled name: {}".format(uni))
    if uni.get("MobileGL::MG_State::GLState::WeakOnly()") != "w":
        problems.append("nm parse: weak-undefined type letter lost: {}".format(uni))
    if sam.get("MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const") != "T":
        problems.append("nm parse: defined line with an address: {}".format(sam))
    if len(uni) != 2 or len(sam) != 2:
        problems.append("nm parse: unowned line was not ignored: {} / {}".format(uni, sam))

    # --- the set arithmetic ----------------------------------------------------------
    result = compute(CANNED_OBJECTS, CANNED_UNDEFINED, CANNED_DEFINED)
    want_symbols = [
        "MobileGL::MG_Pipe::MGPipeSlots()",
        "MobileGL::MG_Remote::Client::ClientSession::NoteApplyThreadEnteredApplier()",
        "MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const",
        "MobileGL::MG_State::GLState::SamplerObject::GetMinFilter() const",
        "MobileGL::MG_State::GLState::SamplerObject::GetWrapS() const",
    ]
    if result["symbols"] != want_symbols:
        problems.append("reach set: {}".format(result["symbols"]))
    # Each exclusion for its own reason, so a regression names which subtraction broke.
    if "MobileGL::Shared::Helper()" in result["symbols"]:
        problems.append("a SHARED-defined symbol survived the subtraction")
    if "MobileGL::MG_Backend::BackendObject::Tick()" in result["symbols"]:
        problems.append("a SERVER-defined symbol survived the subtraction")
    if "MobileGL::Nowhere::Absent()" in result["symbols"]:
        problems.append("a symbol nobody defines was reported as frontend reach")
    if "MobileGL::MG_State::GLState::WeakOnly()" in result["symbols"]:
        problems.append("a weak undefined (`w`) was counted as a reference")
    # 8 distinct `U` names across the four referring server objects (the `w` is not one of
    # them, and GetMinFilter is referred to twice but counted once); minus Tick (SERVER-defined)
    # and Helper (SHARED-defined) leaves 6; of those, Absent is defined by nobody, leaving 5.
    if result["counts"]["server_undefined"] != 8:
        problems.append("server undefined count: {}".format(result["counts"]))
    if result["counts"]["unsatisfied_by_server_or_shared"] != 6:
        problems.append("unsatisfied count: {}".format(result["counts"]))
    if result["counts"]["defined_by_frontend"] != 5:
        problems.append("frontend-defined count: {}".format(result["counts"]))
    if (result["counts"]["server_objects"], result["counts"]["frontend_objects"],
            result["counts"]["shared_objects"]) != (6, 3, 3):
        problems.append("partition counts: {}".format(result["counts"]))

    # --- the buckets ------------------------------------------------------------------
    buckets, also_core = bucketise(result)
    want_buckets = {
        "p6-core": ["MobileGL::MG_Pipe::MGPipeSlots()",
                    "MobileGL::MG_Remote::Client::ClientSession::"
                    "NoteApplyThreadEnteredApplier()"],
        "p7-magma": ["MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const"],
        "p3b-p4b-espryt": ["MobileGL::MG_State::GLState::SamplerObject::GetWrapS() const"],
        "both-backends": ["MobileGL::MG_State::GLState::SamplerObject::GetMinFilter() const"],
    }
    if buckets != want_buckets:
        problems.append("buckets: {}".format(buckets))
    if sum(len(v) for v in buckets.values()) != len(result["symbols"]):
        problems.append("buckets do not partition the reach set")
    if also_core:
        problems.append("unexpected backend+core overlap: {}".format(also_core))

    # --- the ratchet ------------------------------------------------------------------
    base = ["a", "b", "c"]
    new, gone = monotone(["b", "c", "d"], base)
    if new != ["d"] or gone != ["a"]:
        problems.append("monotone(): new={} gone={}".format(new, gone))
    if monotone(["a", "b"], base) != ([], ["c"]):
        problems.append("a shrunk set must be green with a re-baseline note")
    if monotone(base, base) != ([], []):
        problems.append("an identical set must be silent")
    # ID-P7-7's whole point: one cleared, one added, same count -> still red.
    new, gone = monotone(["a", "b", "z"], base)
    if new != ["z"] or gone != ["c"]:
        problems.append("clear-one-add-one at equal count did not name the new symbol")

    # --- the baseline parser ----------------------------------------------------------
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False,
                                     encoding="utf-8", newline="\n") as handle:
        handle.write("# a header comment\n"
                     "#   base: deadbeef\n"
                     "\n"
                     "MobileGL::MG_Pipe::MGPipeSlots() # P13\n"
                     "MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const\n"
                     "MobileGL::Vec4<int>::Vec4(int, int, int, int) # P13 allocator-shaped\n")
        tmp = handle.name
    try:
        symbols, notes = read_baseline(tmp)
        if symbols != ["MobileGL::MG_Pipe::MGPipeSlots()",
                       "MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const",
                       "MobileGL::Vec4<int>::Vec4(int, int, int, int)"]:
            problems.append("baseline parse dropped or mangled a symbol: {}".format(symbols))
        if notes.get("MobileGL::MG_Pipe::MGPipeSlots()") != "P13":
            problems.append("baseline annotation not read: {}".format(notes))
        if len(notes) != 2:
            problems.append("baseline annotations: {}".format(notes))
    finally:
        os.unlink(tmp)

    for problem in problems:
        say("self-test: " + problem)
    say("self-test: " + ("OK (partition, families, nm parser, set arithmetic, buckets, "
                         "ratchet, baseline parser)" if not problems else "FAILED"))
    return 0 if not problems else 1


def render_buckets(result, buckets, also_core, verbose):
    lines = []
    lines.append("")
    lines.append("| bucket | symbols | a6 | delta | meaning |")
    lines.append("|---|---:|---:|---:|---|")
    for name, why in BUCKETS:
        count = len(buckets[name])
        a6 = A6_BUCKET_TOTALS[name]
        lines.append("| `{}` | {} | {} | {:+d} | {} |".format(name, count, a6, count - a6, why))
    total = sum(len(v) for v in buckets.values())
    lines.append("| **total** | **{}** | **{}** | **{:+d}** | |".format(
        total, A6_TOTAL, total - A6_TOTAL))
    lines.append("")
    lines.append("a6's four published totals sum to {} against its own headline of {}; the "
                 "buckets above are a strict partition, so they always sum to the headline."
                 .format(sum(A6_BUCKET_TOTALS.values()), A6_TOTAL))
    if also_core:
        lines.append("")
        lines.append("{} backend-bucket symbol(s) are ALSO referred to by a core object, so "
                     "they do not fall when the backend that owns them migrates:".format(
                         len(also_core)))
        for name in also_core:
            lines.append("  - " + name)
    if verbose:
        for name, _why in BUCKETS:
            lines.append("")
            lines.append("### {} ({})".format(name, len(buckets[name])))
            for symbol in buckets[name]:
                lines.append("  {}".format(symbol))
                lines.append("      defined by: " + ", ".join(result["definers"][symbol]))
                lines.append("      referred by: " + ", ".join(result["referrers"][symbol]))
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", help="a configured, BUILT disaggregated CMake build dir")
    parser.add_argument("--target", default="MobileGL",
                        help="the CMake target whose objects carry the partition "
                             "(default: MobileGL; MobileGL_s is the same sources as a static "
                             "lib, MobileGLServer is the P0 spike stub)")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--baseline", help="the stored symbol list")
    parser.add_argument("--assert-monotone", action="store_true",
                        help="exit non-zero for every symbol present now and absent from the "
                             "baseline. Symbols that vanished are reported and are never a "
                             "failure - progress does not turn the build red.")
    parser.add_argument("--bucket", action="store_true",
                        help="group the reach by referring object family and print the "
                             "P7 / P3b-P4b / both / core totals against a6's")
    parser.add_argument("--verbose-buckets", action="store_true",
                        help="with --bucket, list every symbol with its definer and referrers")
    parser.add_argument("--list", action="store_true",
                        help="print the symbol list itself (implied when no baseline is given)")
    parser.add_argument("--json", help="write the machine-readable result here")
    parser.add_argument("--write-baseline", metavar="FILE",
                        help="write the current list out in baseline format, annotations from "
                             "--baseline carried over")
    parser.add_argument("--no-require-flags", dest="require_flags", action="store_false",
                        help="do not refuse a build whose CMakeCache lacks the three "
                             "disaggregated options (for inspecting an odd build by hand)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.build_dir:
        parser.error("--build-dir is required (or use --self-test)")
    if args.assert_monotone and not args.baseline:
        parser.error("--assert-monotone needs --baseline")
    if args.baseline and not os.path.isfile(args.baseline):
        # Named, not a traceback: in CI this is a path typo or a missing checkout, and the two
        # look identical in a stack trace.
        say("FAIL no baseline file at {} (the ratchet has nothing to compare against; "
            "scripts/data/link_ratchet_baseline.txt is the one CI uses)".format(args.baseline))
        return 2

    cache_values, flag_problems = check_flags(args.build_dir)
    if flag_problems:
        for problem in flag_problems:
            say(("FAIL " if args.require_flags else "warning: ") + problem)
        if args.require_flags:
            return 2

    root, objects = find_objects(args.build_dir, args.target)
    roles = {}
    unclassified = []
    for obj in objects:
        role = classify(obj)
        if role is None:
            unclassified.append(obj)
        else:
            roles[obj] = role
    if unclassified:
        say("FAIL {} object(s) match no PARTITION row. The tree moved and this measurement "
            "no longer measures what it claims to; add a row (with its reason) rather than a "
            "catch-all:".format(len(unclassified)))
        for obj in unclassified:
            say("  unclassified: " + obj)
        return 2

    # An unbuilt (or half-built) directory classifies cleanly into three empty sets and would
    # otherwise report a beautiful zero. The ratchet's happiest possible output must not also be
    # its output for "there was nothing to look at".
    empty = [name for name, members in ((SERVER, [o for o, r in roles.items() if r == SERVER]),
                                        (FRONTEND, [o for o, r in roles.items() if r == FRONTEND]),
                                        (SHARED, [o for o, r in roles.items() if r == SHARED]))
             if not members]
    if empty:
        say("FAIL {} set(s) are empty ({}) in {} - the target was not built, or was built "
            "somewhere else. A ratchet that reports 0 because it found no objects is worse "
            "than no ratchet.".format(len(empty), ", ".join(empty), root))
        return 2

    server = sorted(o for o, r in roles.items() if r == SERVER)
    undefined = parse_nm(run_nm(args.nm, "undefined", server, root), server)
    defined = parse_nm(run_nm(args.nm, "defined", objects, root), objects)
    result = compute(roles, undefined, defined)

    counts = result["counts"]
    say("build-dir {} (target {}, {} objects)".format(args.build_dir, args.target, len(objects)))
    if cache_values:
        say("configure: " + ", ".join("{}={}".format(n, cache_values.get(n, "<unset>"))
                                      for n, _ in REQUIRED_FLAGS)
            + ", CMAKE_BUILD_TYPE=" + cache_values.get("CMAKE_BUILD_TYPE", "<unset>"))
    say("partition: SERVER {} / FRONTEND {} / SHARED {} objects".format(
        counts["server_objects"], counts["frontend_objects"], counts["shared_objects"]))
    say("SERVER undefined symbols: {}".format(counts["server_undefined"]))
    say("  not satisfied by SERVER or SHARED: {}".format(
        counts["unsatisfied_by_server_or_shared"]))
    say("  OF WHICH defined by FRONTEND: {}  (a6 measured {})".format(
        counts["defined_by_frontend"], A6_TOTAL))

    buckets, also_core = bucketise(result)
    if args.bucket:
        print("\n".join(render_buckets(result, buckets, also_core, args.verbose_buckets)))

    if args.list or not args.baseline:
        print("")
        for symbol in result["symbols"]:
            print(symbol)

    exit_code = 0
    new, gone = [], []
    baseline_symbols, baseline_notes = [], {}
    if args.baseline:
        baseline_symbols, baseline_notes = read_baseline(args.baseline)
        new, gone = monotone(result["symbols"], baseline_symbols)
        say("baseline {}: {} symbol(s), {} annotated".format(
            args.baseline, len(baseline_symbols), len(baseline_notes)))
        if gone:
            say("{} symbol(s) in the baseline are GONE. That is the ratchet turning - not a "
                "failure. Re-baseline in the same commit that cleared them "
                "(--write-baseline):".format(len(gone)))
            for symbol in gone:
                say("  cleared: " + symbol
                    + (("   [" + baseline_notes[symbol] + "]") if symbol in baseline_notes
                       else ""))
        if new:
            say("{} symbol(s) are NEW frontend reach: a server object now needs a frontend "
                "definition the baseline did not record. This is what the gate exists for - "
                "either stop reaching, or land the reach with the reason in the commit and "
                "re-baseline deliberately:".format(len(new)))
            for symbol in new:
                say("  NEW: " + symbol)
                say("       defined by:  " + ", ".join(result["definers"][symbol]))
                say("       referred by: " + ", ".join(result["referrers"][symbol]))
        if args.assert_monotone and new:
            exit_code = 1
        if not new and not gone:
            say("ratchet: unchanged at {} symbol(s)".format(len(result["symbols"])))

    if args.write_baseline:
        if not args.baseline:
            say("warning: --write-baseline without --baseline: the ` # P13` annotations of the "
                "old file cannot be carried over and will be lost. Pass --baseline too.")
        write_baseline(args.write_baseline, result["symbols"], baseline_notes, cache_values,
                       args.build_dir)
        say("baseline written to {} ({} symbol(s), {} annotation(s) carried over)".format(
            args.write_baseline, len(result["symbols"]),
            sum(1 for s in result["symbols"] if s in baseline_notes)))

    if args.json:
        payload = {
            "build_dir": args.build_dir,
            "target": args.target,
            "configure": {name: (cache_values or {}).get(name) for name, _ in REQUIRED_FLAGS},
            "counts": counts,
            # The partition itself travels with the artifact: a reviewer reading the JSON six
            # months from now should not have to guess which objects were on which side.
            "roles": roles,
            "a6": {"total": A6_TOTAL, "buckets": A6_BUCKET_TOTALS},
            "symbols": result["symbols"],
            "buckets": {name: buckets[name] for name, _ in BUCKETS},
            "bucket_counts": {name: len(buckets[name]) for name, _ in BUCKETS},
            "backend_symbols_also_referred_by_core": also_core,
            "definers": result["definers"],
            "referrers": result["referrers"],
            "baseline": args.baseline,
            "baseline_count": len(baseline_symbols) if args.baseline else None,
            "new_since_baseline": new,
            "cleared_since_baseline": gone,
            "assert_monotone": args.assert_monotone,
            "exit_code": exit_code,
        }
        with open(args.json, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(payload, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
        say("json written to " + args.json)

    if exit_code:
        say("FAIL --assert-monotone: {} new frontend symbol(s) (named above)".format(len(new)))
    return exit_code


def write_baseline(path, symbols, notes, cache_values, build_dir):
    head = subprocess.run(["git", "rev-parse", "HEAD"], stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, text=True).stdout.strip() or "<unknown>"
    flags = " ".join("-D{}={}".format(n, (cache_values or {}).get(n, "?"))
                     for n, _ in REQUIRED_FLAGS)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "# MobileGL link-closure ratchet baseline (CONTRACT-P6.md 12.1, ID-P7-7).\n"
            "# Regenerate: python3 scripts/link_ratchet.py --build-dir <dir> \\\n"
            "#               --baseline scripts/data/link_ratchet_baseline.txt \\\n"
            "#               --write-baseline scripts/data/link_ratchet_baseline.txt\n"
            "# base commit : {}\n"
            "# configure   : {} -DCMAKE_BUILD_TYPE={}\n"
            "# build dir   : {}\n"
            "# count       : {}\n"
            "#\n"
            "# One demangled symbol per line. A trailing ` # note` is an annotation the parser\n"
            "# ignores; `# P13` marks a symbol that cannot fall before P13 establishes module\n"
            "# target boundaries. The LIST is the baseline, not the count: a ratchet that\n"
            "# stored only a number would go green on the day one symbol is cleared and another\n"
            "# added (ID-P7-7).\n".format(
                head, flags, (cache_values or {}).get("CMAKE_BUILD_TYPE", "?"), build_dir,
                len(symbols)))
        for symbol in symbols:
            note = notes.get(symbol)
            handle.write(symbol + (" # " + note if note else "") + "\n")


if __name__ == "__main__":
    sys.exit(main())
