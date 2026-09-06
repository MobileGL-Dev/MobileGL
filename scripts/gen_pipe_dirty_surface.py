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

P0 is the skeleton: it reports. P1 adds the mapping file and CI regenerates it with
`git diff --exit-code` and zero unmapped mutators, the same shape as gen_pipe.py's G6.

P2 adds the half a completeness gate cannot have: for the RenderState family the ANSWER is
derived from RenderState.cpp rather than believed, so a row that names a publisher which
fires on only some paths through the setter (or omits one that always fires) is red. Without
it a row could be wrong in exactly the direction ARCHITECTURE.md 13.2 calls dangerous while
--check stayed green, which is how two rows in this mapping were wrong for a whole review.

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
SCAN_ROOT = os.path.join(REPO_ROOT, "MobileGL", "MG_Impl", "GLImpl")

# The mutating half of GLContext's surface. Prefix-matched, per the plan's list.
MUTATOR_PREFIXES = ("Add", "Set", "Mark", "Bump", "Allocate", "Truncate", "Record", "Notify",
                    "Begin", "End")

MUTATOR_RE = re.compile(r"pGLContext->\s*((?:%s)\w*)\s*\(" % "|".join(MUTATOR_PREFIXES))
BACKEND_RE = re.compile(r"gBackendFunctionsTable\.GL\.(\w+)|pActiveBackendObject->\s*(\w+)")
FUNCTION_RE = re.compile(r"(?:^|\n)[ \t]*(?:[A-Za-z_][\w:<>,&*\s]*?)\b(\w+)\s*\([^;{}]*\)\s*"
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


def function_bodies(masked):
    """Yield (name, start_offset, end_offset) for every braced function body."""
    for match in FUNCTION_RE.finditer(masked):
        name = match.group(1)
        start = masked.index("{", match.end() - 1) if masked[match.end() - 1] != "{" else match.end() - 1
        depth = 0
        i = start
        while i < len(masked):
            if masked[i] == "{":
                depth += 1
            elif masked[i] == "}":
                depth -= 1
                if depth == 0:
                    yield name, start, i
                    break
            i += 1


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

# The answers that are not a dirty-bit name. Each one is documented in DirtySurface.def's
# header; a row that uses anything else is a typo, and a typo that read as "mapped" would be
# exactly the silent hole this gate exists to close.
NON_BIT_ANSWERS = ("kImmediate", "kReverseChannel", "kNoBackendRead", "kExplicitDestroy",
                   "kUnpublishedDestroy", "kPulledEveryVerb")

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
#   1. read Tracker.h's Update() and, per bit, collect what its shutter READS -
#      ctx.GetXxx() accessors and `render.Field` reads, with the walk's own locals expanded;
#   2. resolve each accessor, through MG_State's one-line getters, to the MEMBER it returns;
#   3. walk every function body under MG_State/GLState and MG_Impl/Pipe and compute, as a
#      fixed point over call names, which members and struct fields each one transitively
#      WRITES - including through MGP_NOTE_AGGREGATE, whose per-aggregate hop is read out of
#      MGPipeNoteAggregate's own switch rather than assumed;
#   4. a row claiming bit B for mutator M is UNDER-FIRING when M writes nothing B reads.
#
# It is deliberately ONE-DIRECTIONAL. Step 3 is an over-approximation (a call name resolves
# to every body of that name, and a write inside an `if` counts), so "M does write something
# B reads" is not proof that it does so on every path and cannot be turned into a MISSING
# check without false reds. "M writes NOTHING B reads" needs no such assumption, and it is
# the under-firing direction ARCHITECTURE.md 13.2 calls the dangerous one - which is what
# was wrong in this file: X(SetNamedTransformFeedbackBinding, NEW_SO_TARGETS) named a
# shutter that moves on NO path through that mutator.
STATE_ROOTS = (os.path.join(REPO_ROOT, "MobileGL", "MG_State", "GLState"),
               os.path.join(REPO_ROOT, "MobileGL", "MG_Impl", "Pipe"))
UPDATE_RE = re.compile(r"Uint32\s+Update\s*\(")
NOW_RE = re.compile(r"now\[Index\(MGPipeDirty::(\w+)\)\]\s*=\s*([^;]*);")
DIRTY_OR_RE = re.compile(r"dirty\s*\|=\s*MGPipeDirtyBit\(MGPipeDirty::(\w+)\)")
DIRTY_ANY_RE = re.compile(r"dirty\s*\|=")
LOCAL_RE = re.compile(r"(\w+)\s*=\s*([^;]*);")
CTX_READ_RE = re.compile(r"\bctx\.(\w+)\s*\(")
ARROW_READ_RE = re.compile(r"\b\w+\s*->\s*(\w+)\s*\(")
FIELD_READ_RE = re.compile(r"\brender\.(\w+)")
RETURN_RE = re.compile(r"\breturn\s+([^;]*);")
MEMBER_RE = re.compile(r"\b(m_\w+)\b")
MEMBER_CALL_RE = re.compile(r"\b(m_\w+)\s*\.\s*(\w+)\s*\(")
CALL_RE = re.compile(r"\b(\w+)\s*\(")
WORD_RE = re.compile(r"\b(\w+)\b")
AGGREGATE_RE = re.compile(r"MGP_NOTE_AGGREGATE\(\s*(\w+)\s*\)")
MEMBER_WRITE_RE = re.compile(r"\+\+\s*(m_\w+)|\b(m_\w+)\s*(?:\+\+|\+=|=(?!=))")
FIELD_WRITE_RE = re.compile(r"\.\s*(\w+)\s*(?:\[[^\]]*\])?\s*(?:\+\+|\+=|=(?!=))")
AGGREGATE_CASE_RE = re.compile(r"case\s+MGPipeAggregate::(\w+)\s*:\s*([^;]*);")


def state_bodies():
    """{function name: [body text]} over MG_State/GLState and MG_Impl/Pipe."""
    bodies = {}
    for root in STATE_ROOTS:
        for directory, _, files in os.walk(root):
            for name in sorted(files):
                if not name.endswith((".h", ".cpp")):
                    continue
                path = os.path.join(directory, name)
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    masked = mask_comments_and_strings(handle.read())
                for fn, start, end in function_bodies(masked):
                    bodies.setdefault(fn, []).append(masked[start:end])
    return bodies


def written_tokens(bodies):
    """{function name: set of tokens it transitively WRITES}, a fixed point over call names.

    A token is MEM:<member>, FIELD:<struct field> or AGG:<MGPipeAggregate enumerator>."""
    reach = {}
    for name, bodylist in bodies.items():
        tokens = set()
        for body in bodylist:
            tokens |= set("AGG:" + m.group(1) for m in AGGREGATE_RE.finditer(body))
            for match in MEMBER_WRITE_RE.finditer(body):
                tokens.add("MEM:" + (match.group(1) or match.group(2)))
            tokens |= set("FIELD:" + m.group(1) for m in FIELD_WRITE_RE.finditer(body))
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
    return reach


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


def resolve_reader(name, bodies, seen=None):
    """The members an accessor returns, through however many one-line getters it delegates
    to. An empty answer means the derivation could not follow it, which is reported as
    UNVERIFIED rather than treated as "moves nothing"."""
    seen = seen if seen is not None else set()
    if name in seen or name not in bodies:
        return set()
    seen.add(name)
    members = set()
    for body in bodies[name]:
        for match in RETURN_RE.finditer(body):
            expression = match.group(1)
            delegated = set()
            for call in MEMBER_CALL_RE.finditer(expression):
                delegated.add(call.group(1))
                members |= resolve_reader(call.group(2), bodies, seen)
            for member in MEMBER_RE.finditer(expression):
                if member.group(1) not in delegated:
                    members.add("MEM:" + member.group(1))
    return members


def shutter_readers():
    """{MGPipeDirty bit name: set of reader tokens} out of Tracker.h's Update()."""
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
        found |= set("FIELD:" + m.group(1) for m in FIELD_READ_RE.finditer(expression))
        for word in WORD_RE.findall(expression):
            if word in assignments and word not in ("now", "dirty"):
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
        # window would start at the top of the walk and inherit every other bit's readers.
        previous = 0
        for boundary in DIRTY_ANY_RE.finditer(body, 0, match.start()):
            previous = boundary.end()
        window = body[previous:match.start()]
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
    """{NEW_* bit name: (tokens whose write moves that bit's shutter, every reader resolved?)}"""
    out = {}
    for enumerator, tokens in readers.items():
        bit = aliases.get(enumerator)
        if bit is None:
            continue
        movers = set()
        resolved = True
        for token in tokens:
            if token.startswith("FIELD:"):
                movers.add(token)
                continue
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
    """{mutator: answer} from DirtySurface.def, or from `text` for the self-test. An answer
    keeps its "|"-joined spelling; answer_set() below is what compares them."""
    if text is None:
        with open(DEF_PATH, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    rows = {}
    duplicates = []
    for match in ROW_RE.finditer(mask_comments_and_strings(text)):
        mutator, answer = match.group(1), match.group(2)
        if mutator in rows:
            duplicates.append(mutator)
        rows[mutator] = answer
    return rows, duplicates


def answer_set(answer):
    return {part.strip() for part in answer.split("|") if part.strip()}


def object_class_problems(mapping, bits, movers, moved):
    """The under-firing check for every answer the RenderState derivation cannot reach.

    Returns (problems, verified, unverified) - `unverified` names the rows the derivation
    had to decline, with the reason, so --check reports its own coverage instead of letting
    a row it never looked at read as checked."""
    problems = []
    verified = 0
    unverified = []
    render_bits = {RENDER_STATE_BIT, PIPELINE_STATE_BIT}
    for mutator in sorted(mapping):
        claimed = (answer_set(mapping[mutator]) & bits) - render_bits
        if not claimed:
            continue
        if mutator not in moved:
            unverified.append("%s (no body found under MG_State/GLState or MG_Impl/Pipe to "
                              "derive from)" % mutator)
            continue
        for bit in sorted(claimed):
            shutter, resolved = movers.get(bit, (set(), False))
            if not resolved:
                unverified.append("%s <- %s (Tracker.h's shutter for that bit reads something "
                                  "this script cannot resolve to a member)" % (mutator, bit))
                continue
            if moved[mutator] & shutter:
                verified += 1
                continue
            problems.append(
                "UNDER-FIRING answer %s for %s - it writes nothing %s's shutter reads "
                "(shutter: %s), so a mutation through it publishes nothing"
                % (bit, mutator, bit, ", ".join(sorted(t.split(":", 1)[1] for t in shutter))))
    return problems, verified, unverified


def check_mapping(mapping, duplicates, scanned, bits, publishers=None, movers=None, moved=None):
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
        if len(answers) > 1 and answers & set(NON_BIT_ANSWERS):
            problems.append("BAD answer %s for %s - a non-bit answer stands alone"
                            % (mapping[mutator], mutator))

    if movers is not None and moved is not None:
        object_problems, _, _ = object_class_problems(mapping, bits, movers, moved)
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

SELF_TEST_STALE = None  # built from the real def at run time


def scan_all():
    """(findings-per-file, {mutator: call count}) over the whole scan root."""
    sources = []
    for root, _, files in os.walk(SCAN_ROOT):
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


def self_test(scanned, bits, publishers, movers, moved):
    """Canned negative controls. Each MUST trip; trips == 0 is an error, which is the shape
    check_include_closure.py and gen_pipe.py --self-test already use."""
    trips = 0
    failures = []

    # 1. a mutator withheld from the def.
    mapping, duplicates = load_mapping(SELF_TEST_WITHHELD)
    problems = check_mapping(mapping, duplicates, scanned, bits)
    if any(p.startswith("UNMAPPED") for p in problems):
        trips += 1
    else:
        failures.append("negative control 1 (a withheld mutator) did NOT trip")

    # 2. a row naming a mutator the scan does not find.
    real, real_duplicates = load_mapping()
    with_ghost = dict(real)
    with_ghost["SetSomethingThatDoesNotExist"] = "kImmediate"
    problems = check_mapping(with_ghost, real_duplicates, scanned, bits)
    if any(p.startswith("STALE") for p in problems):
        trips += 1
    else:
        failures.append("negative control 2 (a stale row) did NOT trip")

    # 3. a row whose answer is neither a dirty bit nor one of the documented non-bit answers.
    with_typo = dict(real)
    with_typo["RecordError"] = "NEW_TYPO_THAT_IS_NOT_A_BIT"
    problems = check_mapping(with_typo, real_duplicates, scanned, bits)
    if any(p.startswith("BAD answer") for p in problems):
        trips += 1
    else:
        failures.append("negative control 3 (a bad answer) did NOT trip")

    # 4. THE CONTROL FOR THE TRUTH HALF, and it is the shape of the defect that was actually
    #    in this file: a row claiming a publisher that fires on only some paths through the
    #    setter. SetCapability's ClipDistance0..7 arms move m_version alone, so
    #    NEW_PIPELINE_STATE here must read as under-firing rather than as a valid answer.
    with_under_firing = dict(real)
    with_under_firing["SetCapability"] = PIPELINE_STATE_BIT
    problems = check_mapping(with_under_firing, real_duplicates, scanned, bits, publishers)
    if any(p.startswith("UNDER-FIRING") for p in problems):
        trips += 1
    else:
        failures.append("negative control 4 (an under-firing render-state answer) did NOT trip")

    # 5. the other direction: a row that drops a publisher which DOES always fire. Silent
    #    today, load-bearing the moment P3a builds a shutter from the file.
    with_missing = dict(real)
    with_missing["SetBlendEquation"] = RENDER_STATE_BIT
    problems = check_mapping(with_missing, real_duplicates, scanned, bits, publishers)
    if any(p.startswith("MISSING publisher") for p in problems):
        trips += 1
    else:
        failures.append("negative control 5 (a dropped render-state publisher) did NOT trip")

    # 6. THE CONTROL FOR THE OBJECT-CLASS HALF, and it is again the shape of a defect that
    #    was actually in this file: X(SetNamedTransformFeedbackBinding, NEW_SO_TARGETS)
    #    named a shutter (the buffer-content aggregate mixed with the transform-feedback
    #    generation) that GLContext::SetNamedTransformFeedbackBinding moves on no path - it
    #    binds a BufferState binding point or writes a saved-bindings entry, and neither is
    #    a buffer CONTENT write or a BeginTransformFeedback.
    with_dead_shutter = dict(real)
    with_dead_shutter["SetNamedTransformFeedbackBinding"] = "NEW_SO_TARGETS"
    problems = check_mapping(with_dead_shutter, real_duplicates, scanned, bits, publishers,
                             movers, moved)
    if any("UNDER-FIRING answer NEW_SO_TARGETS" in p for p in problems):
        trips += 1
    else:
        failures.append("negative control 6 (an object-class answer whose shutter the mutator "
                        "never moves) did NOT trip")

    # 7. the same check pointed at a value-class bit, so one passing control cannot stand in
    #    for the whole family: a vertex-attribute default does not move the pixel-store bytes.
    with_wrong_bit = dict(real)
    with_wrong_bit["SetCurrentVertexAttributeInt"] = "NEW_PIXEL_PACK"
    problems = check_mapping(with_wrong_bit, real_duplicates, scanned, bits, publishers,
                             movers, moved)
    if any("UNDER-FIRING answer NEW_PIXEL_PACK" in p for p in problems):
        trips += 1
    else:
        failures.append("negative control 7 (a value-class answer whose shutter the mutator "
                        "never moves) did NOT trip")

    for failure in failures:
        print("dirty-surface self-test: %s" % failure)
    if trips == 0:
        print("dirty-surface self-test: NOTHING tripped - the gate cannot fail, which is worse "
              "than a red gate")
        return 1
    if failures:
        return 1
    print("dirty-surface self-test: %d negative controls, all tripped" % trips)
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

    if not os.path.isdir(SCAN_ROOT):
        sys.exit("missing %s" % SCAN_ROOT)
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
    bodies = state_bodies()
    reach = written_tokens(bodies)
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

    if args.self_test:
        return self_test(distinct_all, bits, publishers, movers, moved)

    mapping, duplicates = load_mapping()

    if args.check:
        problems = check_mapping(mapping, duplicates, distinct_all, bits, publishers, movers,
                                 moved)
        for problem in problems:
            print("dirty-surface: %s" % problem)
        if problems:
            print("dirty-surface: %d problem(s); the mapping must cover every mutator the scan "
                  "finds, in both directions, and every render-state answer must be the one "
                  "RenderState.cpp actually publishes" % len(problems))
            return 1
        derived = sum(1 for m in mapping if m in publishers)
        _, verified, unverified = object_class_problems(mapping, bits, movers, moved)
        prose = sorted(m for m in mapping if not (answer_set(mapping[m]) & bits))
        print("dirty-surface: %d mutators, all mapped, no stale rows; %d render-state answers "
              "derived from RenderState.cpp and matching" % (len(mapping), derived))
        # What the gate did NOT check is part of its output, or "all mapped" reads as "all
        # verified" - which it is not, and was not for two rows through a whole review.
        print("dirty-surface: %d other bit answers derived from their shutter in Tracker.h "
              "(under-firing only); %d declined; %d rows carry a prose answer (%s) that no "
              "derivation checks"
              % (verified, len(unverified), len(prose),
                 ", ".join(sorted(set(a for m in prose for a in answer_set(mapping[m]))))))
        for row in unverified:
            print("dirty-surface:   not derived: %s" % row)
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

    print("\ndirty-surface: %d files scanned under MG_Impl/GLImpl" % len(sources))
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
