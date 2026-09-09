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

P3a widens the scan root to MG_State/GLState as well, and with it reads the OTHER publish
mechanism: MGP_NOTE_MUTATION, which a state object spells when it moves a pushed PipeInputs
field from inside a backend's own verb. Those sites carry a FIELD name rather than a mutator
name and none of them is a `pGLContext->` call, so the scan attributes each to its ENCLOSING
function - which is the name a DirtySurface.def row is written against. Until this, the four
in TextureState.h were outside the gate entirely, which is the one of the three blind spots
P2 recorded that a scan can actually close.

P0 is the skeleton: it reports. P1 adds the mapping file and CI regenerates it with
`git diff --exit-code` and zero unmapped mutators, the same shape as gen_pipe.py's G6.

P2 adds the half a completeness gate cannot have: for the RenderState family the ANSWER is
derived from RenderState.cpp rather than believed, so a row that names a publisher which
fires on only some paths through the setter (or omits one that always fires) is red. Without
it a row could be wrong in exactly the direction ARCHITECTURE.md 13.2 calls dangerous while
--check stayed green, which is how two rows in this mapping were wrong for a whole review.

Every other bit answer is derived too, against the shutter Tracker.h builds for that bit.
That derivation makes an ABSENCE claim ("this mutator writes nothing that shutter reads"),
so it is only as good as the code it reads, and twice it was not: it missed the writes that
go through a member's field or through the preprocessor, and then - once it read those - it
resolved the reader side to a whole struct while the writer side resolved to the struct's
fields, so every setter of RenderState.cpp "supported" the patch-state bit. Both halves now
resolve to the SAME two-level token, MEM:<member> plus FIELD:<member>.<leaf>; a reference or
pointer bound to a member-rooted lvalue is followed; and a write whose root the analysis
cannot resolve to a member TAINTS the function, so every answer that depends on it comes
out UNDECIDED - printed, tallied, never a verdict. A match at the member level with no
field information on one side is COARSE, reported separately and never counted as derived.

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
# P3a WIDENS THE SCAN ROOT, and this is the one blind spot of the three the P2 review
# recorded that a scan can actually close (DirtySurface.def's header keeps the other two).
# `pGLContext->Set*` is not the only way a frontend mutation becomes observable: a state
# object that moves a PUSHED PipeInputs field from inside a backend's own verb publishes it
# with MGP_NOTE_MUTATION instead, and every one of those sites lives in MG_State/GLState -
# which the scan did not read at all, so the four in TextureState.h were outside the gate
# entirely. Reading both roots and both mechanisms is what makes "every mutation has an
# answer" a claim over the whole surface rather than over one directory of it.
SCAN_ROOTS = (os.path.join(REPO_ROOT, "MobileGL", "MG_Impl", "GLImpl"),
              os.path.join(REPO_ROOT, "MobileGL", "MG_State", "GLState"))

# The mutating half of GLContext's surface. Prefix-matched, per the plan's list.
#
# P4a WIDENS IT BY EXACTLY TWO WORDS, `Use` and `Bind`, and the hole they close is a coverage
# hole in this heuristic rather than a red gate that was being ignored: `UseProgram` begins
# with "Use" and `BindVertexArray`, `BindProgramPipelineObject` and `BindTransformFeedbackObject`
# begin with "Bind", so none of the four was ever visible to this scan - and each of them moves
# a field P3a or P4a pushes. The complete set the widening surfaces was enumerated by grep at
# the phase's base ref before the change landed, so it is four names on seven call sites and
# not a discovery.
#
# `Create` and `Pop` are DELIBERATELY NOT ADDED; DirtySurface.def's header carries the reason,
# which is that they create or destroy objects rather than move a pushed field, and each
# object class's creation and destruction is already answered by its own Mark*ForDeletion row
# plus the constructor-time resource_create.
MUTATOR_PREFIXES = ("Add", "Set", "Mark", "Bump", "Allocate", "Truncate", "Record", "Notify",
                    "Begin", "End", "Use", "Bind")

MUTATOR_RE = re.compile(r"pGLContext->\s*((?:%s)\w*)\s*\(" % "|".join(MUTATOR_PREFIXES))
# The SECOND publish mechanism (MG_Pipe/PipeMutation.h). It carries the FIELD, not a mutator
# name, and it sits inside the state object rather than at a GL entry point - so the mutator
# it belongs to is the ENCLOSING function, which is what needs a row here. Reading it is what
# the widened root buys: MUTATOR_RE alone finds nothing at all under MG_State/GLState.
NOTE_MUTATION_RE = re.compile(r"MGP_NOTE_MUTATION\(\s*(\w+)\s*\)")
BACKEND_RE = re.compile(r"gBackendFunctionsTable\.GL\.(\w+)|pActiveBackendObject->\s*(\w+)")
FUNCTION_RE = re.compile(r"(?:^|\n)[ \t]*(?:[A-Za-z_][\w:<>,&*\s]*?)\b(\w+)\s*\(([^;{}]*)\)\s*"
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


def function_signatures(masked):
    """Yield (name, parameter text, start_offset, end_offset) for every braced function
    body. The parameter text is what the write analysis needs to tell a reference
    parameter (a write through it may land anywhere) from a by-value one (it cannot)."""
    for match in FUNCTION_RE.finditer(masked):
        name = match.group(1)
        if name in CONTROL_KEYWORDS:
            continue
        start = masked.index("{", match.end() - 1) if masked[match.end() - 1] != "{" else match.end() - 1
        depth = 0
        i = start
        while i < len(masked):
            if masked[i] == "{":
                depth += 1
            elif masked[i] == "}":
                depth -= 1
                if depth == 0:
                    yield name, match.group(2), start, i
                    break
            i += 1


def function_bodies(masked):
    """Yield (name, start_offset, end_offset) for every braced function body."""
    for name, _, start, end in function_signatures(masked):
        yield name, start, end


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
        # The push-on-mutation notice: the enclosing function is the mutator, because that is
        # what a row in DirtySurface.def names and what a reader of this report has to look
        # up. It is DEFERRED-shaped by construction (a state object does not reach the
        # backend), so it joins all_mutators and never the immediate publish points below.
        for note in NOTE_MUTATION_RE.finditer(body):
            all_mutators.append((name, line_of(masked, start + note.start())))
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
RENDER_STATE_PATH = os.path.join(REPO_ROOT, "MobileGL", "MG_State", "GLState", "RenderState",
                                 "RenderState.cpp")

# An answer is one or more publishers joined with "|" - every publisher that fires on EVERY
# path through the mutator (DirtySurface.def's header states the rule).
ROW_RE = re.compile(r"^[ \t]*X\((\w+),\s*([\w|]+)\)\s*\\?\s*$", re.M)
DIRTY_NAME_RE = re.compile(r'^\s*"(NEW_[A-Z0-9_]+)",\s*$', re.M)
# The second list of DirtySurface.def: the (mutator, bit) pairs the derivation is KNOWN to
# leave UNDECIDED, each with the reason --check prints. Every bit answer not listed here is
# marked derived, and --check fails when the derivation cannot decide it.
UNDECIDED_LIST_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+MGP_DIRTY_SURFACE_UNDECIDED_LIST\s*\(", re.M)

# The answers that are not a dirty-bit name. Each one is documented in DirtySurface.def's
# header; a row that uses anything else is a typo, and a typo that read as "mapped" would be
# exactly the silent hole this gate exists to close.
NON_BIT_ANSWERS = ("kImmediate", "kReverseChannel", "kNoBackendRead", "kExplicitDestroy",
                   "kUnpublishedDestroy", "kPulledEveryVerb", "kPulledPartialShutter")

# The one non-bit answer that must NOT stand alone. kPulledEveryVerb says "no shutter exists";
# kPulledPartialShutter says "the pull is what holds on every mutating path, and these bits DO
# move on some of them" - so it carries those bits, and the derivation below checks that each
# one really is movable by that mutator. Without the distinction, a reader of this file (P3a
# builds its shutters from it) is told that the mutator behind a bit P2 already emits a call
# for has no shutter at all, which is exactly what X(SetPixelStoreParam, kPulledEveryVerb)
# said about NEW_PIXEL_PACK.
PARTIAL_ANSWERS = ("kPulledPartialShutter",)

# ---- the render-state answers, DERIVED rather than believed -----------------------------
# The two RenderState counters are the one place in the mapping where "what publishes this"
# has a mechanical answer, and where getting it wrong is not a documentation slip: P3a builds
# its narrow shutters from this file, so a row that claims NEW_PIPELINE_STATE for a setter
# whose pipeline bump is conditional (SetStencilFunc) or absent (SetCapability's
# ClipDistance0..7 arms) encodes exactly the under-firing ARCHITECTURE.md 13.2 calls the
# dangerous direction. So the gate derives the answer from RenderState.cpp:
#
#   BumpVersions() moves m_version AND m_pipelineStateVersion (RenderState.h);
#   a bare ++m_version moves only the first;
#   a setter that has BOTH kinds of path always-fires only NEW_RENDER_STATE.
#
# A setter whose body has no bump at all is resolved through the RenderState setter it
# delegates to (SetPolygonOffset -> SetPolygonOffsetClamped).
RENDER_STATE_BIT = "NEW_RENDER_STATE"
PIPELINE_STATE_BIT = "NEW_PIPELINE_STATE"
BUMP_VERSIONS_RE = re.compile(r"\bBumpVersions\s*\(\s*\)")
BARE_VERSION_RE = re.compile(r"\+\+\s*m_version\b")
BARE_PIPELINE_RE = re.compile(r"\+\+\s*m_pipelineStateVersion\b")
SETTER_CALL_RE = re.compile(r"\b(Set\w+)\s*\(")


def render_state_publishers():
    """{setter: set of always-firing render-state publishers} read out of RenderState.cpp.

    A setter absent from the result is not a RenderState setter at all; a setter mapped to an
    EMPTY set moves neither counter (SetPixelStoreParam)."""
    with open(RENDER_STATE_PATH, "r", encoding="utf-8", errors="replace") as handle:
        masked = mask_comments_and_strings(handle.read())

    bodies = {}
    for name, start, end in function_bodies(masked):
        if name.startswith("Set"):
            bodies.setdefault(name, []).append(masked[start:end])

    def direct(body):
        publishers = set()
        bump = BUMP_VERSIONS_RE.search(body) is not None
        bare_version = BARE_VERSION_RE.search(body) is not None
        bare_pipeline = BARE_PIPELINE_RE.search(body) is not None
        if bump or bare_version:
            publishers.add(RENDER_STATE_BIT)
        if bump and not bare_version and not bare_pipeline:
            publishers.add(PIPELINE_STATE_BIT)
        return publishers

    def resolve_body(name, body, seen):
        publishers = direct(body)
        if publishers:
            return publishers
        # No bump of its own: whatever the setter it delegates to publishes.
        for match in SETTER_CALL_RE.finditer(body):
            callee = match.group(1)
            if callee != name and callee in bodies:
                publishers |= resolve(callee, seen)
        return publishers

    def resolve(name, seen):
        if name in seen:
            return set()
        seen.add(name)
        # INTERSECTION, not union, across the bodies of one name. A union would let two
        # overloads - one calling BumpVersions(), one bumping m_version alone - derive as
        # "both counters always fire" and bless the exact under-firing row this derivation
        # exists to catch. Every Set* name in RenderState.cpp has exactly one body today, so
        # this changes no answer; it is the fold that stays right when one does not.
        answers = [resolve_body(name, body, seen) for body in bodies[name]]
        return set.intersection(*answers) if answers else set()

    return {name: resolve(name, set()) for name in bodies}


# ---- the OTHER answers, derived from the shutter each bit is built out of ---------------
# The render-state derivation above covers 45 of the 73 rows. For the rest, "does this
# mutator move the shutter it names" is still a mechanical question, just one asked of a
# different pair of files: MG_Impl/Pipe/Tracker.h says which counters and which bytes each
# MGPipeDirty bit compares, and MG_State says who moves those. So:
#
#   1. read Tracker.h's Update() and, per bit, collect what its shutter READS - ctx.GetXxx()
#      accessors, and `local.Field` reads of a walk local that holds an accessor's result;
#   2. resolve each read, through MG_State's one-line getters, to the same TWO-LEVEL TOKEN
#      the writer side uses: MEM:<member>, and FIELD:<member>.<leaf> - `render.PatchVertices`
#      is FIELD:m_parameters.PatchVertices, and an accessor that returns a whole member reads
#      every field of it, FIELD:<member>.*;
#   3. walk every function body under MG_State/GLState and MG_Impl/Pipe and compute, as a
#      fixed point over call names, which two-level tokens each one transitively WRITES -
#      including through MGP_NOTE_AGGREGATE, whose per-aggregate hop is read out of
#      MGPipeNoteAggregate's own switch rather than assumed;
#   4. match: a writer SUPPORTS a bit iff it writes a member the shutter reads AND, both sides
#      being field-resolved for that member, their FIELD sets intersect (a whole-member write
#      or read is every field). A member in common with no field information on one side is
#      COARSE. UNDER-FIRING - the red verdict - only when every member the shutter reads is
#      either unwritten by the mutator or written in disjoint fields, both sides resolved.
#
# STEP 4 IS AN ABSENCE CLAIM, so step 3 must not MISS a write and must not CREDIT one it did
# not read. What it models is written down here, and what it does not model taints the
# function it is in, which turns every answer depending on that function into UNDECIDED:
#
#   modelled  ++m_x / m_x = / m_x op= (a whole-member write: MEM:m_x, FIELD:m_x.*); a write
#             through a member-rooted lvalue (m_x.f, m_x[i].f, m_x->f, nested), which records
#             MEM:m_x and FIELD:m_x.f - the first field below the member, deeper paths
#             collapse to it, because that is the granularity the shutter reads at; a
#             reference or pointer bound to a member-rooted lvalue (auto& r = m_x.f;
#             for (auto& e : m_x.arr); T* p = &m_x.f; a pointer REBOUND by p = &m_x.g, every
#             binding counting; an alias of an alias), whose writes record the root member
#             and the path they were bound to; a write through a call that returns a
#             reference into an lvalue (m_x.f() = v), a member-function call on a
#             member-rooted lvalue (m_x.push_back(), m_x.reset(), m_x.f.clear()) and a
#             memcpy/memset/memmove/swap whose destination is one, all credited as a whole
#             write of that lvalue - a read-only call is over-credited, which only WIDENS the
#             writer side; a write to a value local, a by-value parameter or an aggregate
#             initialiser's `.f = v`, which touch no member; a write through a const alias
#             with `.` or through a const raw pointer, which the language forbids - but `->`
#             through a const REFERENCE may be a smart pointer's and is NOT taken as
#             read-only; MGP_NOTE_AGGREGATE; a call to any function whose body is under the
#             two roots, resolved BY NAME (every body of that name); and any function-like
#             macro defined under the two roots, EXPANDED first, token pasting performed.
#   tainted   a write, or a non-read-only method call, through a reference or pointer
#             PARAMETER; through a local reference, pointer or iterator bound to something
#             that is not a member-rooted lvalue (a call result, a ternary, an arithmetic
#             expression); to or on a name the body never declares (a member without the m_
#             prefix, a global, an unexpanded token); an assignment operator the analysis
#             could not attribute to any lvalue at all; a `##` left after macro expansion;
#             and a call to a token-pasting macro it refused to expand. Nothing is trusted
#             by name except the standard library's size()/begin()/find() family. The taint
#             is a token like every other, so it travels the same call-graph fixed point the
#             writes do: a mutator that REACHES a tainted body gets no verdict either.
#   undecided also when the shutter itself reads something this script cannot resolve to a
#             member, when the mutator has no body under the two roots, and - for the absence
#             direction only - when a member the shutter reads has a write-shaped occurrence
#             OUTSIDE the two roots. --check prints every UNDECIDED pair with its reason, and
#             fails on one that DirtySurface.def does not list as known-undecided.
#
# What remains one-directional is the OTHER direction: a call name resolves to every body of
# that name, a write inside an `if` counts, a reader that resolves through a ternary yields
# both members, a FIELD token is not scoped to a type - so "M does write something B reads"
# is not proof that it does so on every path and cannot become a MISSING check without false
# reds. That is why a mutator whose bit moves on half its arms answers kPulledPartialShutter
# rather than the bit. "M writes NOTHING B reads" is the direction the gate fails on, and it
# is the under-firing one ARCHITECTURE.md 13.2 calls dangerous - which is what was wrong in
# this file: X(SetNamedTransformFeedbackBinding, NEW_SO_TARGETS) named a shutter that moves
# on NO path through that mutator.
STATE_ROOTS = (os.path.join(REPO_ROOT, "MobileGL", "MG_State", "GLState"),
               os.path.join(REPO_ROOT, "MobileGL", "MG_Impl", "Pipe"))
# Everything the absence claim has to be checked against, which is wider than what it reads:
# a write to a shutter member from outside STATE_ROOTS is a writer this analysis never looks
# at, and the only honest answer to a row whose shutter has one is "undecided".
MOBILEGL_ROOT = os.path.join(REPO_ROOT, "MobileGL")
UPDATE_RE = re.compile(r"Uint32\s+Update\s*\(")
NOW_RE = re.compile(r"now\[Index\(MGPipeDirty::(\w+)\)\]\s*=\s*([^;]*);")
DIRTY_OR_RE = re.compile(r"dirty\s*\|=\s*MGPipeDirtyBit\(MGPipeDirty::(\w+)\)")
DIRTY_ANY_RE = re.compile(r"dirty\s*\|=")
LOCAL_RE = re.compile(r"(\w+)\s*=\s*([^;]*);")
CTX_READ_RE = re.compile(r"\bctx\.(\w+)\s*\(")
ARROW_READ_RE = re.compile(r"\b\w+\s*->\s*(\w+)\s*\(")
# `local.Field` / `local->Field` that is NOT a call: a read of one field of whatever the
# local holds. This is what keeps `render.PatchVertices` from reading as the whole of
# m_parameters, which is what disarmed the patch-state check for 30 rows.
LOCAL_FIELD_RE = re.compile(r"\b(\w+)\s*(?:\.|->)\s*(\w+)\b(?!\s*\()")
RETURN_RE = re.compile(r"\breturn\s+([^;]*);")
MEMBER_RE = re.compile(r"\b(m_\w+)\b")
MEMBER_CALL_RE = re.compile(r"\b(m_\w+)\s*\.\s*(\w+)\s*\(")
CALL_RE = re.compile(r"\b(\w+)\s*\(")
WORD_RE = re.compile(r"\b(\w+)\b")
AGGREGATE_RE = re.compile(r"MGP_NOTE_AGGREGATE\(\s*(\w+)\s*\)")
AGGREGATE_CASE_RE = re.compile(r"case\s+MGPipeAggregate::(\w+)\s*:\s*([^;]*);")

# The assignment operators, as a suffix every write pattern below shares. `=(?!=)` keeps ==
# out; !=, <= and >= cannot match at all, because the character where the operator must start
# is then `!`, `<` or `>` and no alternative here begins with one except <<= / >>=.
ASSIGN = r"(?:\+\+|--|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|=(?!=))"
# A write to the member itself: ++m_x, m_x = v, m_x += v. Used by the OUTSIDE scan only; the
# analysed bodies go through extract_writes below.
MEMBER_WRITE_RE = re.compile(r"\+\+\s*(m_\w+)|\b(m_\w+)\s*%s" % ASSIGN)
# A write THROUGH a member: m_x.f = v, m_x[i].f = v, m_x->f = v, and nested. Outside scan only.
MEMBER_ROOTED_WRITE_RE = re.compile(
    r"\b(m_\w+)\s*(?:\[[^;\n]*?\]|\.\s*\w+|->\s*\w+)+\s*%s" % ASSIGN)

# ---- the write analysis: two-level tokens, aliases, taint --------------------------------
# An lvalue is a root name followed by a path of `.f`, `->f` and `[i]` steps. The root is a
# member (m_x), a local, a parameter, `this`, or nothing this script can name.
INDEX = r"\[(?:[^\[\]]|\[[^\[\]]*\])*\]"
PATH = r"((?:\s*(?:\.|->)\s*\w+|\s*%s)*)" % INDEX
LVALUE_RE = re.compile(r"(\w+)%s" % PATH)
PATH_NAME_RE = re.compile(r"(?:\.|->)\s*(\w+)")
# `lvalue op= rhs`, `lvalue++`, and `*p = v`.
ASSIGN_RE = re.compile(r"(\*?)\s*\b(\w+)%s\s*(%s)" % (PATH, ASSIGN))
# `++lvalue`, `++(*p)`.
PRE_INC_RE = re.compile(r"(\+\+|--)\s*\(?\s*(\*?)\s*(\w+)%s" % PATH)
# `lvalue.method(...) = v`: a write through a call that returns a reference into the lvalue
# (m_levelRange.x() = level). Credited as a whole write of the lvalue.
CALL_LVALUE_RE = re.compile(r"\b(\w+)%s\s*(\.|->)\s*(\w+)\s*\(" % PATH)
ASSIGN_AFTER_RE = re.compile(r"\s*(%s)" % ASSIGN)
# `lvalue.method(`: credited as a whole write of the lvalue when its root is a member or an
# alias of one, unless the method is one of the few that can only read.
METHOD_RE = re.compile(r"\b(\w+)%s\s*(?:\.|->)\s*(\w+)\s*\(" % PATH)
# The standard-library calls that cannot write their object. Nothing else is trusted by
# name: a call to any other method on something this script cannot place is a taint, which
# is what a helper that reads through a `GLContext&` costs whatever reaches it - an
# UNDECIDED answer, not a wrong one.
READ_ONLY_METHODS = frozenset(("size", "empty", "begin", "end", "cbegin", "cend", "rbegin",
                               "rend", "capacity", "max_size", "length", "c_str", "count",
                               "contains", "find"))
# memcpy(&dst, ...), memset(&dst, ...), memmove(&dst, ...), swap(a, b): whole writes.
BULK_RE = re.compile(r"\b(?:std\s*::\s*)?(memcpy|memmove|memset|swap)\s*\(")
# A local declaration: [qualifiers] Type[<...>][::More] [const] [& | && | *] name, then the
# initialiser's opener. A `&`/`*` mark (or an initialiser that takes an address) makes the
# local an ALIAS whose initialiser must resolve to a member-rooted lvalue; anything else is
# a value local, which cannot alias a member - unless the body later writes through it with
# `->` or `*`, in which case it is a pointer or an iterator and is treated as an alias. A
# pointer alias is REBOUND by `p = expr;` (every binding counts); a reference alias is
# written through by it. A const-qualified alias cannot be written through with `.`, and a
# const raw pointer cannot be written through at all - but `->` on a const REFERENCE may be
# a smart pointer's, whose pointee is not const, so that one is not read-only.
TYPE_WORD = r"[A-Za-z_]\w*"
DECL_RE = re.compile(
    r"(?:^|[;{}(,])[ \t\n]*"
    r"((?:(?:const|constexpr|static|volatile|mutable)\s+)*"
    r"(?:%s\s*::\s*)*%s(?:\s*<[^<>;{}()]*>)?(?:\s*::\s*%s)*)"
    r"(\s+const)?(?:\s*(&&|&|\*(?:\s*const)?)\s*|\s+)"
    r"(\w+)\s*(=(?!=)|\{|;|\(|\[|:(?!:))" % (TYPE_WORD, TYPE_WORD, TYPE_WORD))
KEYWORDS = frozenset((
    "return", "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "goto", "throw", "delete", "new", "sizeof", "alignof", "typedef", "using",
    "namespace", "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast", "struct",
    "class", "enum", "union", "template", "typename", "operator", "co_return", "co_await",
    "co_yield", "extern", "friend", "explicit", "inline", "virtual", "override", "final",
    "public", "private", "protected", "try", "catch", "asm", "this"))
# `if (...) {` and friends look exactly like a function definition to FUNCTION_RE. Their
# blocks are inside the enclosing body already, so as "functions" they would only be read a
# second time without their enclosing declarations - and every body with a loop would
# "call" a function named `for`.
CONTROL_KEYWORDS = frozenset(("if", "for", "while", "switch", "catch", "else"))
USING_RE = re.compile(r"\busing\s+\w+\s*(=)")
PARAM_NAME_RE = re.compile(r"(\w+)\s*(\[[^\]]*\])?\s*(?:=[^=].*)?$")
# Every assignment operator in a body. Each one must be attributed to an lvalue by the
# patterns above, or it taints the body: an unread write is the whole failure mode.
ASSIGN_OP_RE = re.compile(r"(\+\+|--|<<=|>>=|[-+*/%&|^]=|(?<![=!<>\[\-+*/%&|^])=(?![=\]]))")

# ---- the preprocessor's half of the write analysis --------------------------------------
# Sixteen of RenderState.cpp's writes exist only after the preprocessor has run: SET_PIXEL_
# STORE_PARAM (:829) pastes `m_pixelStore##paramNameHead##Parameters`, and SET_CAPABILITY
# (:309) pastes `m_parameters.capability##Enabled`. Read raw, neither is a write to any token
# this script can name, so it saw none of them and reported their absence as a fact. So the
# function-like macros defined under the two roots are expanded first, and the ones that
# cannot be expanded are recorded so that a body reaching one is undecided rather than
# answered.
MACRO_DIRECTIVE_RE = re.compile(
    r"^[ \t]*#[ \t]*(define|undef)[ \t]+(\w+)(\([^()\n]*\))?((?:\\\n|[^\n])*)", re.M)
# MGP_NOTE_AGGREGATE is modelled directly (its hop is read out of MGPipeNoteAggregate's own
# switch), so expanding it would delete the very token AGGREGATE_RE looks for.
NEVER_EXPAND = frozenset(("MGP_NOTE_AGGREGATE", "MGP_NOTE_MUTATION"))
MACRO_BODY_LIMIT = 4000
PASTE_RE = re.compile(r"##")
# A member DECLARATION - a type, then the name, then the end of the statement. Used to keep
# the containment check below from confusing two classes that spell a member the same way:
# MG_Backend/MGPipe/PipeInputs.h has its own m_transformFeedbackGeneration, and a write to
# THAT one says nothing about who writes GLContext's.
MEMBER_DECL_RE = re.compile(
    r"^[ \t]*[A-Za-z_][\w:<>,&*\s]*?[\s&*](m_\w+)\s*(?:\[[^\]\n]*\])?"
    r"\s*(?:=[^;\n]*|\{[^}\n]*\})?;", re.M)


def source_files(root):
    """Every .h/.cpp under `root`, in a stable order."""
    out = []
    for directory, _, files in os.walk(root):
        for name in sorted(files):
            if name.endswith((".h", ".cpp")):
                out.append(os.path.join(directory, name))
    return sorted(out)


def macro_definitions(masked):
    """(kind, name, params-or-None, body, start, end) for every #define / #undef, with line
    continuations joined."""
    for match in MACRO_DIRECTIVE_RE.finditer(masked):
        yield (match.group(1), match.group(2), match.group(3),
               match.group(4).replace("\\\n", " "), match.start(), match.end())


def blank_directives(masked):
    """`masked` with every #define / #undef blanked (newlines kept), so a macro BODY is never
    read as code belonging to whatever function encloses the directive - RenderState.cpp
    defines SET_PIXEL_STORE_PARAM *inside* SetPixelStoreParam, and read raw its body
    contributed a field literally named `paramNameTail`."""
    out = list(masked)
    for _, _, _, _, start, end in macro_definitions(masked):
        for index in range(start, end):
            if out[index] != "\n":
                out[index] = " "
    return "".join(out)


def balanced(text):
    counts = {"(": 0, "[": 0, "{": 0}
    closing = {")": "(", "]": "[", "}": "{"}
    for char in text:
        if char in counts:
            counts[char] += 1
        elif char in closing:
            counts[closing[char]] -= 1
            if counts[closing[char]] < 0:
                return False
    return not any(counts.values())


def macro_table(paths):
    """({name: (params, body)} this script will expand, {name: body} it saw and refused).

    Function-like macros only - an object-like macro is a constant and expanding it buys the
    write analysis nothing. A macro is REFUSED when expanding it could corrupt the brace
    matching every function body here is found by (an unbalanced body), when it is variadic,
    over-long or defined more than once with different bodies, or when the analysis models it
    directly. A refused macro is not silently ignored: if its body pastes tokens, every body
    that invokes it is tainted."""
    seen = {}
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            masked = mask_comments_and_strings(handle.read())
        for kind, name, params, body, _, _ in macro_definitions(masked):
            if kind == "undef" or params is None:
                continue
            seen.setdefault(name, []).append((params, body))
    expandable = {}
    refused = {}
    for name, definitions in seen.items():
        bodies = set(body for _, body in definitions)
        params = [part.strip() for part in definitions[0][0][1:-1].split(",") if part.strip()]
        body = definitions[0][1]
        if (name in NEVER_EXPAND or len(bodies) > 1 or len(body) > MACRO_BODY_LIMIT
                or not balanced(body) or any(not part.isidentifier() for part in params)):
            refused[name] = " ".join(sorted(bodies))
            continue
        expandable[name] = (params, body)
    return expandable, refused


def matching_paren(text, open_index):
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    return None


def matching_close(text, open_index, opener, closer):
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == opener:
            depth += 1
        elif text[index] == closer:
            depth -= 1
            if depth == 0:
                return index
    return None


def split_arguments(text):
    args = []
    current = []
    depth = 0
    for char in text:
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        if char == "," and depth == 0:
            args.append("".join(current))
            current = []
            continue
        current.append(char)
    if current or args:
        args.append("".join(current))
    return args


def substitute(body, params, args):
    """One macro expansion: parameters replaced, then `##` pasted away - which is the step
    that turns `m_pixelStore##paramNameHead##Parameters` into a member this script can name."""
    text = body
    for param, arg in sorted(zip(params, args), key=lambda pair: -len(pair[0])):
        text = re.sub(r"\b%s\b" % re.escape(param), lambda _match, value=arg: value, text)
    return re.sub(r"\s*##\s*", "", text)


def expand_macros(masked, expandable, rounds=4):
    """`masked` with its directives blanked and every invocation of an expandable
    function-like macro replaced by its expansion."""
    text = blank_directives(masked)
    if not expandable:
        return text
    pattern = re.compile(r"\b(%s)\s*\(" % "|".join(sorted((re.escape(name) for name in expandable),
                                                          key=len, reverse=True)))
    for _ in range(rounds):
        out = []
        index = 0
        changed = False
        while True:
            match = pattern.search(text, index)
            if match is None:
                out.append(text[index:])
                break
            close = matching_paren(text, match.end() - 1)
            params, body = expandable[match.group(1)]
            args = split_arguments(text[match.end():close]) if close is not None else None
            if args is None or len(args) != len(params):
                out.append(text[index:match.end()])
                index = match.end()
                continue
            out.append(text[index:match.start()])
            out.append(" %s " % substitute(body, params, args))
            index = close + 1
            changed = True
        text = "".join(out)
        if not changed:
            break
    return text


def unmodelled_sites(body, relative, pasting):
    """The preprocessor constructs in `body` this script does NOT model, as taint reasons:
    a token paste it could not expand, and a call to a pasting macro it refused."""
    sites = set()
    if PASTE_RE.search(body):
        sites.add("%s: a `##` token paste no visible macro definition expands" % relative)
    for match in CALL_RE.finditer(body):
        if match.group(1) in pasting:
            sites.add("%s: %s(), a token-pasting macro this script refused to expand"
                      % (relative, match.group(1)))
    return sites


def parse_parameters(params_text):
    """{parameter name: "ref" | "value"}. A reference, pointer or array parameter can alias
    any member of any object; a by-value one cannot."""
    kinds = {}
    for part in split_arguments(params_text or ""):
        part = part.strip()
        if not part or part in ("void", "..."):
            continue
        match = PARAM_NAME_RE.search(part)
        if not match:
            continue
        kinds[match.group(1)] = ("ref" if ("&" in part or "*" in part or match.group(2))
                                 else "value")
    return kinds


def strip_parens(text):
    text = text.strip()
    while text.startswith("(") and text.endswith(")") and balanced(text[1:-1]):
        text = text[1:-1].strip()
    return text


def lvalue_parts(expression):
    """(root, [path names]) when `expression` is a plain lvalue - a name followed by
    `.f` / `->f` / `[i]` steps - and None for anything else (a call, a ternary, arithmetic).
    `this->m_x.f` is folded to root m_x."""
    text = strip_parens(expression)
    match = LVALUE_RE.fullmatch(text)
    if not match:
        return None
    root, path = match.group(1), match.group(2)
    names = PATH_NAME_RE.findall(path)
    if root == "this":
        if not names:
            return None
        root, names = names[0], names[1:]
    return root, names


def field_token(member, names):
    """The two-level FIELD token for a write or read of member `member` at path `names`:
    the first name below the member, or `*` (every field) when the path is empty."""
    return "FIELD:%s.%s" % (member, names[0] if names else "*")


LOCAL = ("", [])  # an alias that resolves to a value local: writes through it touch no member


def extract_writes(body, params_text, site):
    """(tokens, taints) for ONE function body: every write it makes, as MEM:/FIELD: two-level
    tokens, and every reason the analysis could not attribute one of its writes.

    `site` names the body in the taint text so --check can print where the gate lost its
    footing."""
    tokens = set()
    taints = set()
    params = parse_parameters(params_text)
    declarations = {}
    covered = set()

    for match in DECL_RE.finditer(body):
        type_text, post_const, mark, name, opener = match.groups()
        type_words = re.findall(r"\w+", type_text)
        if any(word in KEYWORDS for word in type_words) or name in KEYWORDS:
            continue
        opener_at = match.end() - 1
        init = None
        if opener == "=":
            end = body.find(";", opener_at)
            init = body[opener_at + 1:end if end >= 0 else len(body)]
            covered.add(opener_at)
        elif opener == ":":
            close = matching_paren(body, body.rfind("(", 0, opener_at))
            init = body[opener_at + 1:close if close is not None else len(body)]
        elif opener == "(":
            close = matching_paren(body, opener_at)
            init = body[opener_at + 1:close if close is not None else len(body)]
        elif opener == "{":
            close = matching_close(body, opener_at, "{", "}")
            init = body[opener_at + 1:close if close is not None else len(body)]
        mark = mark or ""
        pointer = "*" in mark or (not mark and (init or "").strip().startswith("&"))
        readonly = "const" in type_words[:-1] or bool(post_const)
        declarations.setdefault(name, []).append({
            "alias": bool(mark) or pointer, "pointer": pointer, "readonly": readonly,
            "init": init})
    for match in USING_RE.finditer(body):
        covered.add(match.start(1))

    def kind_of(name):
        if name in declarations:
            return "alias" if any(b["alias"] for b in declarations[name]) else "value"
        return params.get(name, "unknown")

    # A pointer alias is rebound by `p = expr;` - every binding is a place its later writes
    # may land, so the rebinds are collected before any write is classified.
    rebinds = set()
    for match in ASSIGN_RE.finditer(body):
        root, path, op = match.group(2), match.group(3), match.group(4)
        if (op == "=" and not match.group(1) and not path.strip() and root in declarations
                and any(b["pointer"] for b in declarations[root])):
            end = body.find(";", match.end())
            declarations[root].append({
                "alias": True, "pointer": True, "init": body[match.end():end if end >= 0 else len(body)],
                "readonly": any(b["readonly"] for b in declarations[root])})
            rebinds.add(match.start())
            covered.add(match.start(4))

    def resolve_alias(name, through, seen):
        """[(member, path)] for every binding of alias `name` a write can land through; LOCAL
        for a binding to a value local; None in the list for a binding this script cannot
        resolve. A binding no write can go through (const) contributes nothing."""
        if name in seen:
            return [None]
        seen = seen | {name}
        out = []
        for binding in declarations.get(name, ()):
            if binding["readonly"] and (not through or binding["pointer"]):
                continue
            text = (binding["init"] or "").strip()
            while text[:1] in ("&", "*"):
                text = text[1:].strip()
            if strip_parens(text) in ("", "nullptr", "NULL", "0", "{}"):
                continue  # a null binding has no target; the rebind that gives it one counts
            parts = lvalue_parts(text)
            if parts is None:
                out.append(None)
                continue
            root, names = parts
            if root.startswith("m_"):
                out.append((root, names))
            elif kind_of(root) == "value":
                out.append(LOCAL)
            elif kind_of(root) == "alias":
                for resolved in resolve_alias(root, through, seen):
                    out.append(None if resolved is None else (resolved[0], resolved[1] + names))
            else:
                out.append(None)
        return out

    def record(root, names, through, what="writes"):
        """A write (or a mutating call, `what`) to lvalue root+names. `through` says the
        access went through `->` or `*`, which makes even a value local a pointer."""
        if root == "this":
            if not names:
                return
            root, names = names[0], names[1:]
        if root.startswith("m_"):
            tokens.add("MEM:" + root)
            tokens.add(field_token(root, names))
            return
        kind = kind_of(root)
        if kind == "value" and not through:
            return
        if kind in ("value", "alias"):
            if kind == "value" and not declarations[root]:
                return
            for resolved in resolve_alias(root, through, set()):
                if resolved is None:
                    taints.add("%s %s through '%s', which is bound to something this script "
                               "cannot resolve to a member" % (site, what, root))
                elif resolved is not LOCAL:
                    tokens.add("MEM:" + resolved[0])
                    tokens.add(field_token(resolved[0], resolved[1] + names))
            return
        if kind == "ref":
            taints.add("%s %s through its reference parameter '%s'" % (site, what, root))
            return
        taints.add("%s %s '%s', which it never declares - a member without the m_ prefix, a "
                   "global, or a call result; none of which this script can place"
                   % (site, what, root))

    def designated(text, root_at):
        """`{.f = v, .g = w}`: an aggregate initialiser, not a write to a field named f."""
        before = text[:root_at].rstrip()
        if not before.endswith("."):
            return False
        return before[:-1].rstrip()[-1:] in ("{", ",")

    def collect(text, base):
        """Every write in `text` (an absolute offset `base` into the body), recursing into
        index expressions so that `m_a[m_count++] = v` credits m_count as well as m_a."""
        for match in ASSIGN_RE.finditer(text):
            covered.add(base + match.start(4))
            if designated(text, match.start(2)) or base + match.start() in rebinds:
                continue
            path = match.group(3)
            record(match.group(2), PATH_NAME_RE.findall(path),
                   bool(match.group(1)) or path.lstrip().startswith("->"))
            if "[" in path:
                collect(path, base + match.start(3))
        for match in PRE_INC_RE.finditer(text):
            covered.add(base + match.start(1))
            path = match.group(4)
            record(match.group(3), PATH_NAME_RE.findall(path),
                   bool(match.group(2)) or path.lstrip().startswith("->"))
            if "[" in path:
                collect(path, base + match.start(4))
        for match in METHOD_RE.finditer(text):
            root, path, method = match.group(1), match.group(2), match.group(3)
            if method in READ_ONLY_METHODS:
                continue
            if root in KEYWORDS and root != "this":
                continue
            # A call through `.` on a value local mutates the local; through `->` it
            # mutates whatever the pointer points at. A call on a member-rooted lvalue is
            # a whole write of it; on anything this script cannot place, a taint - the
            # callee's own writes are still inherited by name, but the OBJECT they land
            # in is unattributed, and for a callee with no body under the roots
            # (push_back, reset) that is the whole write.
            record(root, PATH_NAME_RE.findall(path), path.lstrip().startswith("->"),
                   what="calls %s()" % method)
        for match in CALL_LVALUE_RE.finditer(text):
            close = matching_paren(text, match.end() - 1)
            if close is None:
                continue
            after = ASSIGN_AFTER_RE.match(text, close + 1)
            if after is None:
                continue
            covered.add(base + after.start(1))
            root, path, separator, method = match.groups()
            record(root, PATH_NAME_RE.findall(path), separator == "->",
                   what="writes through %s()" % method)
        for match in BULK_RE.finditer(text):
            close = matching_paren(text, match.end() - 1)
            args = split_arguments(text[match.end():close]) if close is not None else []
            targets = args[:2] if match.group(1) == "swap" else args[:1]
            for target in targets:
                arg = strip_parens(target)
                while arg[:1] in ("&", "*"):
                    arg = arg[1:].strip()
                parts = lvalue_parts(arg)
                if parts is None:
                    taints.add("%s passes '%s' to %s(), which this script cannot resolve to "
                               "an lvalue" % (site, arg.strip()[:40], match.group(1)))
                    continue
                record(parts[0], parts[1], True, what="passes to %s()" % match.group(1))

    collect(body, 0)
    for match in ASSIGN_OP_RE.finditer(body):
        if match.start() not in covered:
            taints.add("%s has an assignment at offset %d this script could not attribute to "
                       "any lvalue (%s)" % (site, match.start(),
                                           body[max(0, match.start() - 24):match.start() + 8]
                                           .replace("\n", " ").strip()))
            break
    return tokens, taints


def state_bodies():
    """({function name: [body text]}, {function name: [parameter text]},
    {function name: {taint reason}}, [analysed paths]) over MG_State/GLState and
    MG_Impl/Pipe, macros expanded first."""
    paths = []
    for root in STATE_ROOTS:
        paths += source_files(root)
    expandable, refused = macro_table(paths)
    # A macro this script MODELS is not an unread construct even though it is not expanded.
    pasting = frozenset(name for name, body in refused.items()
                        if name not in NEVER_EXPAND and PASTE_RE.search(body))
    bodies = {}
    signatures = {}
    taints = {}
    for path in paths:
        relative = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            expanded = expand_macros(mask_comments_and_strings(handle.read()), expandable)
        for fn, params, start, end in function_signatures(expanded):
            body = expanded[start:end]
            bodies.setdefault(fn, []).append(body)
            signatures.setdefault(fn, []).append(params)
            sites = unmodelled_sites(body, relative, pasting)
            if sites:
                taints.setdefault(fn, set())
                taints[fn] |= sites
    return bodies, signatures, taints, paths


def writers_outside(analysed):
    """{MEM token: a file OUTSIDE the analysed roots that writes it}.

    The absence claim is only as good as the set of writers the analysis reads. Every .h/.cpp
    under MobileGL/ that is not one of the analysed files is scanned with the direct write
    patterns, and a shutter member that turns up here is undecided rather than answered.

    A file that DECLARES a member of that name is writing its own, not the frontend's - the
    backend's PipeInputs mirrors half of GLState's member names - so its writes do not count.
    That is the one judgement here, and it is the conservative way round only for names the
    two sides share; a genuine outside writer of a frontend member does not declare it.

    This scan is textual and file-wide: it sees a direct member write and a member-rooted
    one, not a write through a reference or a mutating call. It is the containment check for
    code the analysis does not read, and that is the limit of what it can say."""
    seen = set(analysed)
    out = {}
    for path in source_files(MOBILEGL_ROOT):
        if path in seen:
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            masked = mask_comments_and_strings(handle.read())
        relative = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
        own = set(MEMBER_DECL_RE.findall(masked))
        written = set(match.group(1) or match.group(2) for match in MEMBER_WRITE_RE.finditer(masked))
        written |= set(match.group(1) for match in MEMBER_ROOTED_WRITE_RE.finditer(masked))
        for member in written - own:
            out.setdefault("MEM:" + member, relative)
    return out


def written_tokens(bodies, signatures, taints=None, relative_names=None):
    """{function name: set of tokens it transitively WRITES}, a fixed point over call names.

    A token is MEM:<member>, FIELD:<member>.<leaf>, AGG:<MGPipeAggregate enumerator> or
    TAINT:<reason>. The last is not a write: it is "this body contains a write the analysis
    could not attribute", and it rides the same fixed point so that a mutator which REACHES
    such a body inherits it and is undecided rather than answered."""
    taints = taints or {}
    reach = {}
    own_taints = {}
    for name, bodylist in bodies.items():
        tokens = set("TAINT:" + site for site in taints.get(name, ()))
        for index, body in enumerate(bodylist):
            tokens |= set("AGG:" + m.group(1) for m in AGGREGATE_RE.finditer(body))
            params = signatures.get(name, [""] * len(bodylist))[index]
            site = "%s()" % name if relative_names is None else relative_names.get(name, name)
            written, tainted = extract_writes(body, params, site)
            tokens |= written
            tokens |= set("TAINT:" + reason for reason in tainted)
        if any(token.startswith("TAINT:") for token in tokens):
            own_taints[name] = set(token[6:] for token in tokens if token.startswith("TAINT:"))
        reach[name] = tokens
    changed = True
    rounds = 0
    while changed and rounds < 16:
        changed = False
        rounds += 1
        for name, bodylist in bodies.items():
            before = len(reach[name])
            for body in bodylist:
                for match in CALL_RE.finditer(body):
                    callee = match.group(1)
                    if callee != name and callee in reach:
                        reach[name] |= reach[callee]
            if len(reach[name]) != before:
                changed = True
    return reach, own_taints


def aggregate_tokens(bodies, reach):
    """{MGPipeAggregate enumerator: the tokens its notice writes}, read out of
    MGPipeNoteAggregate's own switch rather than assumed."""
    out = {}
    for body in bodies.get("MGPipeNoteAggregate", []):
        for match in AGGREGATE_CASE_RE.finditer(body):
            aggregate, statement = match.group(1), match.group(2)
            tokens = set()
            for call in CALL_RE.finditer(statement):
                tokens |= reach.get(call.group(1), set())
            out.setdefault(aggregate, set())
            out[aggregate] |= tokens
    return out


def expand_aggregates(tokens, aggregates):
    """AGG:X stands for whatever X's notice writes."""
    out = set()
    for token in tokens:
        if token.startswith("AGG:"):
            out |= aggregates.get(token[4:], set())
        else:
            out.add(token)
    return out


def ternary_parts(expression):
    """The arms of `c ? a : b` at depth 0 (the condition is dropped), or [expression]."""
    depth = 0
    question = None
    colon = None
    i = 0
    while i < len(expression):
        char = expression[i]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif depth == 0 and char == "?" and question is None:
            question = i
        elif (depth == 0 and char == ":" and question is not None and colon is None
              and expression[i - 1:i] != ":" and expression[i + 1:i + 2] != ":"):
            colon = i
        i += 1
    if question is not None and colon is not None:
        return [expression[question + 1:colon], expression[colon + 1:]]
    return [expression]


def resolve_reader(name, bodies, seen=None):
    """The two-level tokens an accessor returns, through however many one-line getters it
    delegates to: MEM:m and FIELD:m.<leaf> for `return m.leaf;`, FIELD:m.* for `return m;`
    (every field), and a bare MEM:m - COARSE, no field information - for a member that is
    read in some other shape (`return Mix(m_a, m_b);`). An empty answer means the derivation
    could not follow it, which is reported as UNDECIDED rather than treated as "moves
    nothing"."""
    seen = seen if seen is not None else set()
    if name in seen or name not in bodies:
        return set()
    seen.add(name)
    tokens = set()
    for body in bodies[name]:
        for match in RETURN_RE.finditer(body):
            for arm in ternary_parts(match.group(1)):
                text = strip_parens(arm)
                parts = lvalue_parts(text)
                if parts is not None and parts[0].startswith("m_"):
                    tokens.add("MEM:" + parts[0])
                    tokens.add(field_token(parts[0], parts[1]))
                    continue
                delegated = set()
                for call in MEMBER_CALL_RE.finditer(text):
                    delegated.add(call.group(1))
                    tokens |= resolve_reader(call.group(2), bodies, seen)
                for member in MEMBER_RE.finditer(text):
                    if member.group(1) not in delegated:
                        tokens.add("MEM:" + member.group(1))
    return tokens


def narrow_tokens(tokens, field):
    """`local.Field` where `local` holds an accessor's result: a whole-member read becomes a
    read of that one field; a read already narrower than the member stays as it is (a deeper
    path collapses to the member's first field, the granularity every token here has)."""
    out = set()
    for token in tokens:
        if token.startswith("FIELD:") and token.endswith(".*"):
            out.add("%s%s" % (token[:-1], field))
        else:
            out.add(token)
    return out


def shutter_readers():
    """{MGPipeDirty bit name: set of reader tokens} out of Tracker.h's Update(): CTX:<accessor>
    for a whole read of what the accessor returns, CTXFIELD:<accessor>.<Field> for a read of
    one field of it through a walk local."""
    with open(TRACKER_PATH, "r", encoding="utf-8", errors="replace") as handle:
        masked = mask_comments_and_strings(handle.read())
    body = None
    for name, start, end in function_bodies(masked):
        if name == "Update" and UPDATE_RE.search(masked[max(0, start - 200):start]):
            body = masked[start:end]
            break
    if body is None:
        return {}

    assignments = {}
    for match in LOCAL_RE.finditer(body):
        assignments.setdefault(match.group(1), set()).add(match.group(2))

    def readers_of(expression, depth=0):
        found = set()
        if depth > 4:
            return found
        found |= set("CTX:" + m.group(1) for m in CTX_READ_RE.finditer(expression))
        found |= set("CTX:" + m.group(1) for m in ARROW_READ_RE.finditer(expression))
        narrowed = set()
        for match in LOCAL_FIELD_RE.finditer(expression):
            local, field = match.group(1), match.group(2)
            if local not in assignments or local in ("now", "dirty"):
                continue
            narrowed.add(match.start(1))
            for assigned in assignments[local]:
                if assigned != expression:
                    for token in readers_of(assigned, depth + 1):
                        if token.startswith("CTX:"):
                            found.add("CTXFIELD:%s.%s" % (token[4:], field))
                        else:
                            found.add(token)
        for match in WORD_RE.finditer(expression):
            word = match.group(1)
            if match.start(1) in narrowed or word in ("now", "dirty") or word not in assignments:
                continue
            for assigned in assignments[word]:
                if assigned != expression:
                    found |= readers_of(assigned, depth + 1)
        return found

    out = {}
    for match in NOW_RE.finditer(body):
        out.setdefault(match.group(1), set())
        out[match.group(1)] |= readers_of(match.group(2))
    # The two BitwiseEqual bits have no `now[]` entry: their shutter is the byte compare
    # itself. The window is the text since the previous `dirty |=`, which is the block that
    # builds the value being compared.
    for match in DIRTY_OR_RE.finditer(body):
        # Since the previous `dirty |=` of ANY form - the counter loop's included, or the
        # window would start at the top of the walk and inherit every other bit's readers -
        # AND since the last `}` before this one, whichever is later. The second boundary is
        # what keeps the pack block's trailing `m_pack = pack;` out of the PATCH bit's
        # window: `pack` is a local of the walk, so it expands to ctx.GetPixelStore
        # Parameters and the patch shutter would read as though it compared the pixel store.
        previous = 0
        for boundary in DIRTY_ANY_RE.finditer(body, 0, match.start()):
            previous = boundary.end()
        closing = body.rfind("}", 0, match.start())
        window = body[max(previous, closing + 1):match.start()]
        out.setdefault(match.group(1), set())
        out[match.group(1)] |= readers_of(window)
    return out


ENUM_BODY_RE = re.compile(r"enum\s+class\s+MGPipeDirty\s*:\s*Uint32\s*\{([^}]*)\}")
ENUMERATOR_RE = re.compile(r"^\s*(\w+)\s*(?:=\s*\d+\s*)?,", re.M)


def dirty_bit_aliases():
    """{MGPipeDirty enumerator: the NEW_* name a row spells}, paired BY POSITION with
    kMGPipeDirtyNames. Tracker.h's Update() names the enumerators and DirtySurface.def names
    the strings, so the two spellings have to be tied together somewhere; doing it by
    position also checks that the enum and its name table have not drifted apart."""
    with open(TRACKER_PATH, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    match = ENUM_BODY_RE.search(mask_comments_and_strings(text))
    if not match:
        return {}
    enumerators = [name for name in ENUMERATOR_RE.findall(match.group(1)) if name != "Count"]
    names = DIRTY_NAME_RE.findall(text)
    if len(enumerators) != len(names):
        return {}
    return dict(zip(enumerators, names))


def shutter_movers(readers, bodies, aliases):
    """{NEW_* bit name: (two-level tokens the shutter reads, every reader resolved?)}"""
    out = {}
    for enumerator, tokens in readers.items():
        bit = aliases.get(enumerator)
        if bit is None:
            continue
        movers = set()
        resolved = True
        for token in tokens:
            if token.startswith("CTXFIELD:"):
                accessor, field = token[9:].rsplit(".", 1)
                members = narrow_tokens(resolve_reader(accessor, bodies), field)
            else:
                members = resolve_reader(token[4:], bodies)
            if not members:
                resolved = False
            movers |= members
        out[bit] = (movers, resolved and bool(movers))
    return out


def dirty_bit_names():
    """The MGPipeDirty bit names, read out of Tracker.h's kMGPipeDirtyNames so a row cannot
    name a bit that does not exist and a bit cannot be renamed out from under a row. Read
    from the RAW text on purpose: the names are string literals, which is exactly what
    mask_comments_and_strings blanks."""
    with open(TRACKER_PATH, "r", encoding="utf-8", errors="replace") as handle:
        return set(DIRTY_NAME_RE.findall(handle.read()))


def load_mapping(text=None):
    """({mutator: answer}, [duplicate mutators], {mutator: {bit}} marked known-undecided) from
    DirtySurface.def, or from `text` for the self-test. An answer keeps its "|"-joined
    spelling; answer_set() below is what compares them. The rows after the
    MGP_DIRTY_SURFACE_UNDECIDED_LIST define are the marks; everything before it is the map."""
    if text is None:
        with open(DEF_PATH, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    masked = mask_comments_and_strings(text)
    split = UNDECIDED_LIST_RE.search(masked)
    main_text = masked if split is None else masked[:split.start()]
    mark_text = "" if split is None else masked[split.start():]
    rows = {}
    duplicates = []
    for match in ROW_RE.finditer(main_text):
        mutator, answer = match.group(1), match.group(2)
        if mutator in rows:
            duplicates.append(mutator)
        rows[mutator] = answer
    undecided = {}
    for match in ROW_RE.finditer(mark_text):
        undecided.setdefault(match.group(1), set()).update(answer_set(match.group(2)))
    return rows, duplicates, undecided


def answer_set(answer):
    return {part.strip() for part in answer.split("|") if part.strip()}


# ---- the match rule -----------------------------------------------------------------------
SUPPORTED = "SUPPORTED"
UNDER_FIRING = "UNDER-FIRING"
COARSE = "COARSE"
UNDECIDED = "UNDECIDED"


def split_tokens(tokens):
    """({member}, {member: {leaf}}) out of a set of two-level tokens."""
    members = set()
    fields = {}
    for token in tokens:
        if token.startswith("MEM:"):
            members.add(token[4:])
        elif token.startswith("FIELD:"):
            member, leaf = token[6:].split(".", 1)
            fields.setdefault(member, set()).add(leaf)
    return members, fields


def match_writes(writer, shutter):
    """SUPPORTED, COARSE or UNDER_FIRING for one (writer tokens, shutter tokens) pair.

    A member in common whose FIELD sets intersect (either side's `*` is every field) is
    support. A member in common with no field information on one side is COARSE - the
    analysis cannot say whether the bytes the shutter compares are the ones the writer
    moved. No member in common, or every common member disjoint at field level with both
    sides resolved, is UNDER_FIRING."""
    writer_members, writer_fields = split_tokens(writer)
    shutter_members, shutter_fields = split_tokens(shutter)
    coarse = False
    for member in writer_members & shutter_members:
        written = writer_fields.get(member)
        read = shutter_fields.get(member)
        if not written or not read:
            coarse = True
            continue
        if "*" in written or "*" in read or written & read:
            return SUPPORTED
    return COARSE if coarse else UNDER_FIRING


def describe(tokens):
    _, fields = split_tokens(tokens)
    members, _ = split_tokens(tokens)
    parts = []
    for member in sorted(members):
        leaves = sorted(fields.get(member, ()))
        parts.append("%s{%s}" % (member, ",".join(leaves)) if leaves else "%s{?}" % member)
    return ", ".join(parts)


def derive_bit_answers(mapping, bits, movers, moved, outside=None):
    """{(mutator, bit): (verdict, detail)} for every non-render bit answer in `mapping`.

    Every branch that does not end in SUPPORTED or UNDER_FIRING ends in COARSE or UNDECIDED,
    because the alternative is what this gate did to NEW_PIXEL_PACK and to NEW_PATCH_STATE:
    turn "this script cannot read that construct" into a verdict."""
    outside = outside or {}
    out = {}
    render_bits = {RENDER_STATE_BIT, PIPELINE_STATE_BIT}
    for mutator in sorted(mapping):
        claimed = (answer_set(mapping[mutator]) & bits) - render_bits
        if not claimed:
            continue
        for bit in sorted(claimed):
            if mutator not in moved:
                out[(mutator, bit)] = (UNDECIDED, "no body found under MG_State/GLState or "
                                                  "MG_Impl/Pipe to derive from")
                continue
            shutter, resolved = movers.get(bit, (set(), False))
            if not resolved:
                out[(mutator, bit)] = (UNDECIDED, "Tracker.h's shutter for that bit reads "
                                                  "something this script cannot resolve to a member")
                continue
            blind = sorted(token[6:] for token in moved[mutator] if token.startswith("TAINT:"))
            if blind:
                out[(mutator, bit)] = (UNDECIDED, "the write analysis is not complete for this "
                                                  "mutator: %s" % blind[0])
                continue
            verdict = match_writes(moved[mutator], shutter)
            if verdict == SUPPORTED:
                out[(mutator, bit)] = (SUPPORTED, "")
                continue
            if verdict == COARSE:
                out[(mutator, bit)] = (COARSE, "a member in common, but no field information on "
                                               "one side (shutter: %s; written: %s)"
                                       % (describe(shutter), describe(moved[mutator])))
                continue
            # An ABSENCE claim: the gate first has to be able to say it read every writer of
            # that shutter.
            unread = sorted(set(outside[token] for token in shutter if token in outside))
            if unread:
                out[(mutator, bit)] = (UNDECIDED, "that shutter's members are written outside "
                                                  "the analysed roots too, e.g. %s, so 'it writes "
                                                  "nothing that shutter reads' is not a fact this "
                                                  "script has" % unread[0])
                continue
            out[(mutator, bit)] = (UNDER_FIRING, "it writes nothing %s's shutter reads (shutter: "
                                                 "%s; it writes: %s), so a mutation through it "
                                                 "publishes nothing"
                                   % (bit, describe(shutter), describe(moved[mutator]) or "no member"))
    return out


def object_class_problems(mapping, bits, movers, moved, outside=None, undecided_marks=None):
    """The under-firing check for every answer the RenderState derivation cannot reach.

    Returns (problems, supported count, coarse count, [undecided lines]). A row that derives
    UNDECIDED is a problem unless DirtySurface.def marks it so; a mark on a row that DOES
    derive is stale and a problem too, so the marks cannot silently outlive their reason."""
    undecided_marks = undecided_marks or {}
    verdicts = derive_bit_answers(mapping, bits, movers, moved, outside)
    problems = []
    supported = 0
    coarse = 0
    undecided = []
    for (mutator, bit), (verdict, detail) in sorted(verdicts.items()):
        marked = bit in undecided_marks.get(mutator, set())
        if verdict == SUPPORTED:
            supported += 1
        elif verdict == COARSE:
            coarse += 1
        elif verdict == UNDECIDED:
            undecided.append("%s <- %s (%s)" % (mutator, bit, detail))
            if not marked:
                problems.append("UNDECIDED answer %s for %s - %s; a bit answer the derivation "
                                "cannot decide is not a derived answer, so either widen the "
                                "analysis or list the row in MGP_DIRTY_SURFACE_UNDECIDED_LIST "
                                "with this reason" % (bit, mutator, detail))
        else:
            problems.append("UNDER-FIRING answer %s for %s - %s" % (bit, mutator, detail))
        if marked and verdict != UNDECIDED:
            problems.append("STALE undecided mark %s for %s - the derivation now decides it "
                            "(%s); delete the mark" % (bit, mutator, verdict))
    for mutator, marks in sorted(undecided_marks.items()):
        for bit in sorted(marks):
            if (mutator, bit) not in verdicts:
                problems.append("STALE undecided mark %s for %s - no row claims that bit for "
                                "that mutator" % (bit, mutator))
    return problems, supported, coarse, undecided


def check_mapping(mapping, duplicates, scanned, bits, publishers=None, movers=None, moved=None,
                  outside=None, undecided_marks=None):
    """Every problem the gate fails on, as a list of human-readable lines. BOTH directions:
    an unmapped mutator renders stale, and a row naming a mutator the scan no longer finds is
    a stale row that would keep a real hole looking covered. `publishers` is
    render_state_publishers()'s table; passing None checks only existence and vocabulary,
    which is what the mutator-level negative controls want."""
    problems = []
    for mutator in sorted(set(scanned) - set(mapping)):
        problems.append("UNMAPPED mutator %s - add a row to MG_Pipe/DirtySurface.def" % mutator)
    for mutator in sorted(set(mapping) - set(scanned)):
        problems.append("STALE row %s - the scan no longer finds this mutator; delete the row"
                        % mutator)
    for mutator in sorted(duplicates):
        problems.append("DUPLICATE row %s" % mutator)
    for mutator in sorted(mapping):
        answers = answer_set(mapping[mutator])
        if not answers:
            problems.append("BAD answer for %s - empty" % mutator)
            continue
        for answer in sorted(answers):
            if answer in NON_BIT_ANSWERS or answer in bits:
                continue
            problems.append("BAD answer %s for %s - not a MGPipeDirty bit name and not one of %s"
                            % (answer, mutator, ", ".join(NON_BIT_ANSWERS)))
        partial = answers & set(PARTIAL_ANSWERS)
        if len(answers) > 1 and (answers & set(NON_BIT_ANSWERS)) - partial:
            problems.append("BAD answer %s for %s - a non-bit answer stands alone"
                            % (mapping[mutator], mutator))
        if partial and not answers & bits:
            problems.append("BAD answer %s for %s - %s has to NAME the bits that move on some "
                            "of the paths that mutate; kPulledEveryVerb is the answer when "
                            "none does" % (mapping[mutator], mutator, "|".join(sorted(partial))))

    if movers is not None and moved is not None:
        object_problems, _, _, _ = object_class_problems(mapping, bits, movers, moved, outside,
                                                         undecided_marks)
        problems += object_problems

    if publishers is None:
        return problems

    # THE TRUTH HALF, and it is the half a row can be green and wrong without. For every
    # mutator that is a RenderState setter, the render-state publishers the row claims must
    # be exactly the ones RenderState.cpp always moves - a claimed publisher that does not
    # always fire is an under-firing shutter waiting to be built from this file, and a
    # publisher that always fires but is not claimed hides one.
    render_bits = {RENDER_STATE_BIT, PIPELINE_STATE_BIT}
    for mutator in sorted(mapping):
        claimed = answer_set(mapping[mutator]) & render_bits
        if mutator not in publishers:
            if claimed:
                problems.append(
                    "UNVERIFIABLE answer %s for %s - it claims a render-state publisher but "
                    "RenderState.cpp has no such setter to derive it from"
                    % (mapping[mutator], mutator))
            continue
        derived = publishers[mutator] & render_bits
        if claimed == derived:
            continue
        for missing in sorted(derived - claimed):
            problems.append(
                "MISSING publisher %s for %s - RenderState.cpp moves it on every path, so the "
                "row must name it (derived: %s)"
                % (missing, mutator, "|".join(sorted(derived)) or "none"))
        for extra in sorted(claimed - derived):
            problems.append(
                "UNDER-FIRING answer %s for %s - RenderState.cpp does NOT move it on every "
                "path, so a shutter built on it would miss a mutation (derived: %s)"
                % (extra, mutator, "|".join(sorted(derived)) or "none"))
    return problems


SELF_TEST_WITHHELD = """
#define MGP_DIRTY_SURFACE_LIST(X) \\
    X(RecordError, kReverseChannel)
"""

# The synthetic bodies the write-analysis controls run through the REAL extractor. Each is
# one shape the review found the analysis blind to, spelled the way RenderState.cpp spells it.
SELF_TEST_BODIES = """
void Fixture::WriteAField() {
    if (m_parameters.ClearColor == color) return;
    m_parameters.ClearColor = color;
    ++m_version;
}
void Fixture::WriteThroughRangeFor(BlendEquation color, BlendEquation alpha) {
    Bool stateChanged = false;
    for (auto& blendState : m_parameters.BlendStates) {
        if (blendState.ColorEquation == color && blendState.AlphaEquation == alpha) continue;
        blendState.ColorEquation = color;
        blendState.AlphaEquation = alpha;
        stateChanged = true;
    }
    if (!stateChanged) return;
    BumpVersions();
}
void Fixture::WriteThroughNamedReference(StencilFace face, Uint32 mask) {
    StencilFaceState& state = m_parameters.StencilStates[GetStencilFaceIndex(face)];
    if (state.WriteMask == mask) return;
    state.WriteMask = mask;
    ++m_version;
}
void Fixture::WriteThroughUnresolvableReference(StencilFace face, Uint32 mask) {
    auto& state = LookUpSomewhere(face);
    state.WriteMask = mask;
    ++m_version;
}
void Fixture::WriteThroughReferenceParameter(RenderStateParameters& target, Uint32 mask) {
    target.ScissorBoxWrittenMask = mask;
}
void Fixture::WriteWholeMember(const RenderStateParameters& fresh) {
    m_parameters = fresh;
}
"""


def analyse_snippet(text):
    """{function name: (tokens, taints)} for a synthetic source text, through the same
    extractor the real tree goes through (no macros: the text has none)."""
    masked = mask_comments_and_strings(text)
    out = {}
    for name, params, start, end in function_signatures(masked):
        out[name] = extract_writes(masked[start:end], params, "%s()" % name)
    return out


def scan_all():
    """(findings-per-file, {mutator: call count}) over every scan root."""
    sources = []
    for scan_root in SCAN_ROOTS:
        for root, _, files in os.walk(scan_root):
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


def self_test(scanned, bits, publishers, movers, moved, outside=None, undecided_marks=None):
    """Canned negative controls. Each MUST trip; trips == 0 is an error, which is the shape
    check_include_closure.py and gen_pipe.py --self-test already use."""
    trips = 0
    failures = []

    def tripped(condition, label):
        nonlocal trips
        if condition:
            trips += 1
        else:
            failures.append("negative control %s did NOT trip" % label)

    # 1. a mutator withheld from the def.
    mapping, duplicates, _ = load_mapping(SELF_TEST_WITHHELD)
    problems = check_mapping(mapping, duplicates, scanned, bits)
    tripped(any(p.startswith("UNMAPPED") for p in problems), "1 (a withheld mutator)")

    # 2. a row naming a mutator the scan does not find.
    real, real_duplicates, real_marks = load_mapping()
    with_ghost = dict(real)
    with_ghost["SetSomethingThatDoesNotExist"] = "kImmediate"
    problems = check_mapping(with_ghost, real_duplicates, scanned, bits)
    tripped(any(p.startswith("STALE row") for p in problems), "2 (a stale row)")

    # 3. a row whose answer is neither a dirty bit nor one of the documented non-bit answers.
    with_typo = dict(real)
    with_typo["RecordError"] = "NEW_TYPO_THAT_IS_NOT_A_BIT"
    problems = check_mapping(with_typo, real_duplicates, scanned, bits)
    tripped(any(p.startswith("BAD answer") for p in problems), "3 (a bad answer)")

    # 4. THE CONTROL FOR THE TRUTH HALF, and it is the shape of the defect that was actually
    #    in this file: a row claiming a publisher that fires on only some paths through the
    #    setter. SetCapability's ClipDistance0..7 arms move m_version alone, so
    #    NEW_PIPELINE_STATE here must read as under-firing rather than as a valid answer.
    with_under_firing = dict(real)
    with_under_firing["SetCapability"] = PIPELINE_STATE_BIT
    problems = check_mapping(with_under_firing, real_duplicates, scanned, bits, publishers)
    tripped(any(p.startswith("UNDER-FIRING") for p in problems),
            "4 (an under-firing render-state answer)")

    # 5. the other direction: a row that drops a publisher which DOES always fire. Silent
    #    today, load-bearing the moment P3a builds a shutter from the file.
    with_missing = dict(real)
    with_missing["SetBlendEquation"] = RENDER_STATE_BIT
    problems = check_mapping(with_missing, real_duplicates, scanned, bits, publishers)
    tripped(any(p.startswith("MISSING publisher") for p in problems),
            "5 (a dropped render-state publisher)")

    # 6. THE CONTROL FOR THE OBJECT-CLASS HALF, and it is again the shape of a defect that
    #    was actually in this file: X(SetNamedTransformFeedbackBinding, NEW_SO_TARGETS)
    #    named a shutter (the buffer-content aggregate mixed with the transform-feedback
    #    generation) that GLContext::SetNamedTransformFeedbackBinding moves on no path - it
    #    binds a BufferState binding point or writes a saved-bindings entry, and neither is
    #    a buffer CONTENT write or a BeginTransformFeedback. The row has to be RED - and
    #    the analysis reaches (by name, through `Bind(`) a body it cannot fully read, so the
    #    red it can honestly print is "UNDECIDED and unmarked", never "supported".
    with_dead_shutter = dict(real)
    with_dead_shutter["SetNamedTransformFeedbackBinding"] = "NEW_SO_TARGETS"
    problems = check_mapping(with_dead_shutter, real_duplicates, scanned, bits, publishers,
                             movers, moved, outside, real_marks)
    verdicts = derive_bit_answers(with_dead_shutter, bits, movers, moved, outside)
    tripped(any(("NEW_SO_TARGETS for SetNamedTransformFeedbackBinding" in p
                 and p.startswith(("UNDER-FIRING", "UNDECIDED"))) for p in problems)
            and verdicts[("SetNamedTransformFeedbackBinding", "NEW_SO_TARGETS")][0] != SUPPORTED,
            "6 (an object-class answer whose shutter the mutator never moves)")
    # 6b. the same family, on a mutator the analysis reads completely, so the red is the
    #     verdict itself: a texture-bind bump moves nothing NEW_GLOBAL_CONSTANTS compares.
    with_dead_object = dict(real)
    with_dead_object["BumpTextureBindGeneration"] = "NEW_GLOBAL_CONSTANTS"
    problems = check_mapping(with_dead_object, real_duplicates, scanned, bits, publishers,
                             movers, moved, outside, real_marks)
    tripped(any("UNDER-FIRING answer NEW_GLOBAL_CONSTANTS for BumpTextureBindGeneration" in p
                for p in problems),
            "6b (an object-class answer that is UNDER-FIRING outright)")

    # 7. the same check pointed at a value-class bit, so one passing control cannot stand in
    #    for the whole family: a vertex-attribute default does not move the pixel-store bytes.
    with_wrong_bit = dict(real)
    with_wrong_bit["SetCurrentVertexAttributeInt"] = "NEW_PIXEL_PACK"
    problems = check_mapping(with_wrong_bit, real_duplicates, scanned, bits, publishers,
                             movers, moved, outside, real_marks)
    tripped(any("UNDER-FIRING answer NEW_PIXEL_PACK for SetCurrentVertexAttributeInt" in p
                for p in problems),
            "7 (a value-class answer whose shutter the mutator never moves)")

    # 8. A row that says kPulledPartialShutter and names no bit. The whole point of that
    #    answer is to carry the bits that DO move, so an empty one is kPulledEveryVerb with
    #    a different spelling - and kPulledEveryVerb is the claim that got NEW_PIXEL_PACK
    #    wrong in the first place.
    with_empty_partial = dict(real)
    with_empty_partial["SetPixelStoreParam"] = "kPulledPartialShutter"
    problems = check_mapping(with_empty_partial, real_duplicates, scanned, bits)
    tripped(any("has to NAME the bits" in p for p in problems),
            "8 (kPulledPartialShutter naming no bit)")

    # 9. THE CONTROL FOR THE TAINT PATH. A mutator whose reachable text contains a write the
    #    analysis could not attribute has to come out UNDECIDED - never UNDER-FIRING, never
    #    SUPPORTED - and (9b) --check must refuse it as a derived answer unless the file
    #    marks it, because an unmarked UNDECIDED is a row the file claims and the gate
    #    cannot back.
    blinded = dict(moved)
    blinded["SetCurrentVertexAttributeInt"] = {"TAINT:a canned unattributable write"}
    verdicts = derive_bit_answers(with_wrong_bit, bits, movers, blinded, outside)
    tripped(verdicts.get(("SetCurrentVertexAttributeInt", "NEW_PIXEL_PACK"), ("", ""))[0]
            == UNDECIDED, "9a (a tainted mutator is UNDECIDED, not a verdict)")
    problems, _, _, undecided = object_class_problems(with_wrong_bit, bits, movers, blinded,
                                                      outside, {})
    tripped(any(p.startswith("UNDECIDED answer NEW_PIXEL_PACK for SetCurrentVertexAttributeInt")
                for p in problems)
            and not any("UNDER-FIRING" in p and "SetCurrentVertexAttributeInt" in p for p in problems)
            and any(u.startswith("SetCurrentVertexAttributeInt <- NEW_PIXEL_PACK") for u in undecided),
            "9b (an unmarked UNDECIDED row fails --check)")
    problems, _, _, _ = object_class_problems(with_wrong_bit, bits, movers, blinded, outside,
                                              {"SetCurrentVertexAttributeInt": {"NEW_PIXEL_PACK"}})
    tripped(not any("SetCurrentVertexAttributeInt" in p for p in problems),
            "9c (a marked UNDECIDED row passes --check without a verdict)")

    # 10. The other absence blocker: a shutter whose members have a writer OUTSIDE the roots
    #     this analysis reads. "Nobody writes it" is not a claim about code the script never
    #     opened.
    hidden = dict(outside or {})
    for token in movers.get("NEW_PIXEL_PACK", (set(), False))[0]:
        hidden[token] = "MobileGL/MG_Backend/a-file-this-analysis-never-reads.cpp"
    verdicts = derive_bit_answers(with_wrong_bit, bits, movers, moved, hidden)
    verdict, detail = verdicts.get(("SetCurrentVertexAttributeInt", "NEW_PIXEL_PACK"), ("", ""))
    tripped(verdict == UNDECIDED and "outside the analysed roots" in detail,
            "10 (a shutter member written outside the analysed roots)")

    # 11-14. THE WRITE ANALYSIS ITSELF, on synthetic bodies through the real extractor. The
    #        review's false verdict was "SetBlendEquation writes nothing NEW_PATCH_STATE's
    #        shutter reads", said about a body that writes m_parameters through a range-for
    #        reference; the collapse that followed was every setter "supporting" the patch
    #        bit because the shutter resolved to the whole struct.
    snippet = analyse_snippet(SELF_TEST_BODIES)
    patch_shutter = {"MEM:m_parameters", "FIELD:m_parameters.PatchVertices",
                     "FIELD:m_parameters.PatchDefaultOuterLevel",
                     "FIELD:m_parameters.PatchDefaultInnerLevel"}
    blend_shutter = {"MEM:m_parameters", "FIELD:m_parameters.BlendStates"}
    stencil_shutter = {"MEM:m_parameters", "FIELD:m_parameters.StencilStates"}

    # 11. a bit whose shutter fields are disjoint from a setter's writes -> UNDER-FIRING.
    tokens, taints = snippet["WriteAField"]
    tripped(not taints and "FIELD:m_parameters.ClearColor" in tokens
            and match_writes(tokens, patch_shutter) == UNDER_FIRING,
            "11 (a field write disjoint from the shutter's fields is UNDER-FIRING)")

    # 12. a write through a reference alias - both the range-for form and the named form -
    #     is credited to the member and the field it was bound to, and supports the bit.
    tokens, taints = snippet["WriteThroughRangeFor"]
    tokens2, taints2 = snippet["WriteThroughNamedReference"]
    tripped(not taints and not taints2
            and "FIELD:m_parameters.BlendStates" in tokens and "MEM:m_parameters" in tokens
            and "FIELD:m_parameters.StencilStates" in tokens2
            and match_writes(tokens, blend_shutter) == SUPPORTED
            and match_writes(tokens2, stencil_shutter) == SUPPORTED
            and match_writes(tokens, patch_shutter) == UNDER_FIRING,
            "12 (a write through a reference alias is credited to its member and field)")

    # 13. an alias whose root cannot be resolved -> the body is tainted; and so is a write
    #     through a reference parameter. Neither may yield a verdict.
    tokens, taints = snippet["WriteThroughUnresolvableReference"]
    tokens2, taints2 = snippet["WriteThroughReferenceParameter"]
    tainted_moved = {"Alias": tokens | set("TAINT:" + t for t in taints),
                     "Param": tokens2 | set("TAINT:" + t for t in taints2)}
    verdicts = derive_bit_answers({"Alias": "NEW_PATCH_STATE", "Param": "NEW_PATCH_STATE"},
                                  {"NEW_PATCH_STATE"}, {"NEW_PATCH_STATE": (patch_shutter, True)},
                                  tainted_moved, {})
    tripped(taints and taints2
            and verdicts[("Alias", "NEW_PATCH_STATE")][0] == UNDECIDED
            and verdicts[("Param", "NEW_PATCH_STATE")][0] == UNDECIDED,
            "13 (an unresolvable alias or a reference parameter is UNDECIDED, never a verdict)")

    # 14. a whole-member write supports every field of that member.
    tokens, taints = snippet["WriteWholeMember"]
    tripped(not taints and "FIELD:m_parameters.*" in tokens
            and match_writes(tokens, patch_shutter) == SUPPORTED
            and match_writes(tokens, blend_shutter) == SUPPORTED
            and match_writes(tokens, {"MEM:m_parameters", "FIELD:m_parameters.LineWidth"}) == SUPPORTED,
            "14 (a whole-member write supports every field of the member)")

    # 15. the pixel-store token-paste setter: SetPixelStoreParam's sixteen writes are either
    #     expanded to FIELD:m_pixelStore{Pack,Unpack}Parameters.<leaf> or the mutator is
    #     tainted; it is never UNDER-FIRING for the bit it moves.
    pixel = moved.get("SetPixelStoreParam", set())
    expanded = {"FIELD:m_pixelStorePackParameters.Alignment",
                "FIELD:m_pixelStoreUnpackParameters.Alignment",
                "FIELD:m_pixelStorePackParameters.LSBFirst",
                "FIELD:m_pixelStoreUnpackParameters.SwapBytes"} <= pixel
    tainted = any(token.startswith("TAINT:") for token in pixel)
    with_the_bit = dict(real)
    with_the_bit["SetPixelStoreParam"] = "NEW_PIXEL_PACK"
    verdicts = derive_bit_answers(with_the_bit, bits, movers, moved, outside)
    verdict = verdicts.get(("SetPixelStoreParam", "NEW_PIXEL_PACK"), ("", ""))[0]
    tripped((expanded or tainted) and verdict != UNDER_FIRING
            and "FIELD:paramNameTail" not in pixel and "FIELD:m_parameters.paramNameTail" not in pixel,
            "15 (the token-pasted pixel-store writes are expanded to their fields, or undecided)")

    # 16. THE NEW_PATCH_STATE ANALOGUE OF CONTROL 7, the row that was green at 9ff6061c:
    #     glClearColor does not move the patch trio, so a row that says it does has to be
    #     red - not "supported because both touch m_parameters".
    with_patch = dict(real)
    with_patch["SetClearColor"] = "NEW_RENDER_STATE|NEW_PATCH_STATE"
    problems = check_mapping(with_patch, real_duplicates, scanned, bits, publishers, movers,
                             moved, outside, real_marks)
    tripped(any("UNDER-FIRING answer NEW_PATCH_STATE for SetClearColor" in p for p in problems),
            "16 (a whole-struct reader no longer makes every setter support NEW_PATCH_STATE)")

    # 17. a member in common with no field information on one side is COARSE: reported, not
    #     counted as derived, and not a problem.
    coarse_movers = {"NEW_PATCH_STATE": ({"MEM:m_parameters"}, True)}
    verdicts = derive_bit_answers({"WriteAField": "NEW_PATCH_STATE"}, {"NEW_PATCH_STATE"},
                                  coarse_movers, {"WriteAField": snippet["WriteAField"][0]}, {})
    problems, supported, coarse, _ = object_class_problems(
        {"WriteAField": "NEW_PATCH_STATE"}, {"NEW_PATCH_STATE"}, coarse_movers,
        {"WriteAField": snippet["WriteAField"][0]}, {}, {})
    tripped(verdicts[("WriteAField", "NEW_PATCH_STATE")][0] == COARSE and supported == 0
            and coarse == 1 and not problems,
            "17 (a member-level match without fields is COARSE, never derived)")

    # 18. a stale undecided mark - a row the derivation DOES decide - is a problem, so the
    #     marks cannot outlive their reason.
    problems, _, _, _ = object_class_problems(with_the_bit, bits, movers, moved, outside,
                                              {"SetPixelStoreParam": {"NEW_PIXEL_PACK"}})
    tripped(any(p.startswith("STALE undecided mark NEW_PIXEL_PACK for SetPixelStoreParam")
                for p in problems), "18 (a stale undecided mark)")

    # 19. THE PREFIX WIDENING ITSELF (P4a). `Use` and `Bind` are what make the four new rows
    #     visible at all, and the control asserts BOTH halves of that - P3a's ten-word set
    #     matches none of the four, and the current set matches exactly the four - because
    #     "the pattern matches now" and "the pattern did not match before" are different
    #     claims, and only the pair says the widening bought anything.
    p3a_prefixes = ("Add", "Set", "Mark", "Bump", "Allocate", "Truncate", "Record", "Notify",
                    "Begin", "End")
    p3a_re = re.compile(r"pGLContext->\s*((?:%s)\w*)\s*\(" % "|".join(p3a_prefixes))
    widened_names = ("UseProgram", "BindVertexArray", "BindProgramPipelineObject",
                     "BindTransformFeedbackObject")
    sample = " ".join("pGLContext->%s(x);" % name for name in widened_names)
    tripped(not p3a_re.findall(sample)
            and sorted(MUTATOR_RE.findall(sample)) == sorted(widened_names),
            "19 (P3a's prefix set is blind to the four mutators `Use` and `Bind` add)")

    # 20a-20d. ONE CONTROL PER NEW ROW, and each is the row's own: with that ONE mutator gone
    #     from what the scan finds - which is what a narrowed prefix set, a renamed entry point
    #     or a deleted call site would produce - its row has to come out as a STALE row rather
    #     than sitting in the file describing a mutator that no longer exists. The other three
    #     rows must not trip on it, or one control would be standing in for four.
    for index, name in enumerate(widened_names):
        without = {m: c for m, c in scanned.items() if m != name} if isinstance(scanned, dict) \
            else set(scanned) - {name}
        problems = check_mapping(real, real_duplicates, without, bits)
        stale = [p for p in problems if p.startswith("STALE row")]
        tripped(len(stale) == 1 and name in stale[0],
                "20%s (the %s row is STALE the moment the scan stops finding it)"
                % ("abcd"[index], name))

    # 21. EVERY UNDECIDED MARK IS STILL LOAD-BEARING. Dropping them all has to make --check
    #     refuse EVERY marked (mutator, bit) as an unmarked UNDECIDED, and nothing else - which
    #     is what says each mark is covering a real blind spot rather than a verdict the
    #     analysis could give today. Control 18 is the other direction: a mark the derivation
    #     DOES decide is itself a problem, so no mark can outlive its reason. Read from the
    #     file's own list rather than spelled here, so a row that gains a bit - UseProgram
    #     gained NEW_SAMPLER_VIEWS and NEW_SHADER_IMAGES at the P4a fable seam round - cannot
    #     silently turn this control into one that counts the wrong number.
    problems, _, _, undecided_rows = object_class_problems(real, bits, movers, moved, outside, {})
    marked_pairs = sorted((mutator, bit) for mutator, marks in real_marks.items() for bit in marks)
    tripped(marked_pairs
            and all(any(p.startswith("UNDECIDED answer %s for %s" % (bit, mutator)) for p in problems)
                    for mutator, bit in marked_pairs)
            and len(undecided_rows) == len(marked_pairs),
            "21 (every P4a undecided mark - %d of them - is still needed)" % len(marked_pairs))

    # THE POSITIVE CONTROLS. (a) The row that was wrong in round 3: SetPixelStoreParam writes
    # NEW_PIXEL_PACK's shutter member sixteen times, through a token-pasting macro; it has
    # to be SUPPORTED at field level. (b) The seven setters round 4's review named, which
    # write m_parameters only through a reference alias: each has to carry the member and
    # the field the alias was bound to, with no taint.
    problems, _, _, undecided = object_class_problems(with_the_bit, bits, movers, moved, outside, {})
    if any("SetPixelStoreParam" in line for line in problems + undecided):
        failures.append("the positive control (SetPixelStoreParam DOES write NEW_PIXEL_PACK's "
                        "shutter) did not pass: %s"
                        % "; ".join(line for line in problems + undecided
                                    if "SetPixelStoreParam" in line))
    alias_setters = {"SetBlendFunc": "BlendStates", "SetBlendFuncIndexed": "BlendStates",
                     "SetBlendEquation": "BlendStates", "SetBlendEquationIndexed": "BlendStates",
                     "SetStencilFunc": "StencilStates", "SetStencilMask": "StencilStates",
                     "SetStencilOp": "StencilStates"}
    for setter, field in sorted(alias_setters.items()):
        tokens = moved.get(setter, set())
        if ("MEM:m_parameters" not in tokens or "FIELD:m_parameters.%s" % field not in tokens
                or any(token.startswith("TAINT:") for token in tokens)):
            failures.append("the positive control (%s writes m_parameters.%s through a "
                            "reference alias) did not pass: %s"
                            % (setter, field, sorted(t for t in tokens
                                                     if t.startswith(("TAINT:", "FIELD:m_parameters")))))

    for failure in failures:
        print("dirty-surface self-test: %s" % failure)
    if trips == 0:
        print("dirty-surface self-test: NOTHING tripped - the gate cannot fail, which is worse "
              "than a red gate")
        return 1
    if failures:
        return 1
    print("dirty-surface self-test: %d negative controls, all tripped; positive controls OK "
          "(SetPixelStoreParam's sixteen token-pasted writes are read at field level, and the "
          "seven reference-alias setters of RenderState.cpp resolve to m_parameters' fields)"
          % trips)
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

    for scan_root in SCAN_ROOTS:
        if not os.path.isdir(scan_root):
            sys.exit("missing %s" % scan_root)
    if not os.path.isfile(DEF_PATH):
        sys.exit("missing %s" % DEF_PATH)
    if not os.path.isfile(RENDER_STATE_PATH):
        sys.exit("missing %s" % RENDER_STATE_PATH)

    sources, per_file, distinct_all = scan_all()
    bits = dirty_bit_names()
    if not bits:
        sys.exit("could not read the MGPipeDirty bit names out of %s" % TRACKER_PATH)
    publishers = render_state_publishers()
    if not publishers:
        sys.exit("could not derive any RenderState setter out of %s" % RENDER_STATE_PATH)
    bodies, signatures, taints, analysed = state_bodies()
    reach, own_taints = written_tokens(bodies, signatures, taints)
    outside = writers_outside(analysed)
    aggregates = aggregate_tokens(bodies, reach)
    moved = {name: expand_aggregates(tokens, aggregates) for name, tokens in reach.items()}
    readers = shutter_readers()
    if not readers:
        sys.exit("could not read the dirty shutters out of %s - has MGPipeTracker::Update been "
                 "renamed?" % TRACKER_PATH)
    aliases = dirty_bit_aliases()
    if not aliases:
        sys.exit("could not pair MGPipeDirty's enumerators with kMGPipeDirtyNames in %s - the "
                 "enum and its name table have drifted apart" % TRACKER_PATH)
    movers = shutter_movers(readers, bodies, aliases)

    mapping, duplicates, undecided_marks = load_mapping()

    if args.self_test:
        return self_test(distinct_all, bits, publishers, movers, moved, outside, undecided_marks)

    if args.check:
        problems = check_mapping(mapping, duplicates, distinct_all, bits, publishers, movers,
                                 moved, outside, undecided_marks)
        for problem in problems:
            print("dirty-surface: %s" % problem)
        if problems:
            print("dirty-surface: %d problem(s); the mapping must cover every mutator the scan "
                  "finds, in both directions, every render-state answer must be the one "
                  "RenderState.cpp actually publishes, and every other bit answer must be one "
                  "the derivation can decide" % len(problems))
            return 1
        derived = sum(1 for m in mapping if m in publishers)
        _, supported, coarse, undecided = object_class_problems(mapping, bits, movers, moved,
                                                                outside, undecided_marks)
        prose = sorted(m for m in mapping if not (answer_set(mapping[m]) & bits))
        print("dirty-surface: %d mutators, all mapped, no stale rows; %d render-state answers "
              "derived from RenderState.cpp and matching" % (len(mapping), derived))
        # What the gate did NOT check is part of its output, or "all mapped" reads as "all
        # verified" - which it is not, and was not for two rows through a whole review.
        print("dirty-surface: %d other (mutator, bit) answers derived from their shutter in "
              "Tracker.h - supported at field level, under-firing only; %d COARSE (a member in "
              "common, no field information, not counted); %d UNDECIDED (listed in "
              "MGP_DIRTY_SURFACE_UNDECIDED_LIST, not counted); %d rows carry a prose answer "
              "(%s) that no derivation checks"
              % (supported, coarse, len(undecided), len(prose),
                 ", ".join(sorted(set(a for m in prose for a in answer_set(mapping[m]))))))
        for row in undecided:
            print("dirty-surface:   UNDECIDED, no verdict: %s" % row)
        # The absence claim's own footprint, printed rather than assumed: what the write
        # analysis read, and where it is still coarse.
        print("dirty-surface: the under-firing half read %d function bodies across %d files "
              "under %s, macros expanded first, both sides resolved to MEM:<member> + "
              "FIELD:<member>.<leaf>; %d of those bodies carry a write it could not attribute "
              "and taint whatever reaches them, %d of the %d mutators reach one; %d members are "
              "written outside those roots and any absence claim over one is undecided; a "
              "mutating call on a member-rooted lvalue and a call resolved by name both only "
              "WIDEN what a mutator is credited with, and a FIELD token is not scoped to a type"
              % (sum(len(v) for v in bodies.values()), len(analysed),
                 " + ".join(os.path.relpath(root, REPO_ROOT).replace(os.sep, "/")
                            for root in STATE_ROOTS),
                 len(own_taints),
                 sum(1 for m in mapping if any(t.startswith("TAINT:") for t in moved.get(m, ()))),
                 len(mapping), len(outside)))
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

    print("\ndirty-surface: %d files scanned under %s"
          % (len(sources), " + ".join(os.path.relpath(root, REPO_ROOT).replace(os.sep, "/")
                                      for root in SCAN_ROOTS)))
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
