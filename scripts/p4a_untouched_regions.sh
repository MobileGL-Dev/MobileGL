#!/usr/bin/env bash
# G5's gate for P4a: the Espryt do-not-touch list is LITERAL - SEVENTEEN regions across THREE
# files stay byte-identical after P4a.
#
# WHAT G5 CLAIMS, and why a diff of the files cannot say it. ARCHITECTURE.md:318 is the Espryt
# do-not-touch list and :321 names the ONE item moved out of it (the sub-rect upload decision and
# the stride computation, which is P4a's D-D3/D-D6 subject). P3a made the buffer half of that list
# literal with a sha gate over eleven functions in Managers.cpp (scripts/p3a_untouched_regions.sh);
# P4a rewrites the rest of Managers.cpp, DirectGLES.cpp and Utils.cpp by design - the twins become
# handle-shaped, the descriptors replace the frontend reads - so the files' diffs are large and say
# nothing about whether the ring, the permutation, the depth/stencil sampling core or the format
# caveat moved. This gate extracts the seventeen BODIES and compares them on their own.
#
# THE SEVENTEEN. P3a's eleven are carried forward unchanged (BRIEF-P4A.md D-N: "P4a must not touch
# any of the eleven"), so this script is a superset of its parent and the two run side by side -
# the parent keeps answering P3a's question against P3a's base ref, this one answers P4a's.
#
#   -- P3a's eleven, in Managers.cpp, in the parent's fixed order --
#   IsPoolable                     takes the server-side resource, never the frontend object
#   EnrollIntoPool                 the retireSerial = CurrentFrameSerial() + 1 stamp is load-bearing
#   AcquireFromPool                hands back only entries whose GPU work is complete
#   TrimBufferPool                 called once per frame from Present
#   ClearBufferPool                context loss
#   ProcessDeferredBufferReleases  drained per draw, fast-outs on an atomic flag
#   CreateRingStorage              glBufferStorageEXT + persistent|coherent, retires at serial + 1
#   RingAvailable                  self-heals a stale context generation
#   RingAllocate                   the fast path on the hot upload route
#   FlushPendingRangesNow          the three-tier drain, pull arm
#   FlushPendingRangesFrom         the SAME three-tier drain, on the arm that ships (ID-15). Born
#                                  in P3a, so its baseline is PINNED, not read from <ref-a>
#
#   -- P4a's six (BRIEF-P4A.md D-N), all of which predate the phase --
#   StageBlocksIntoUnpackRing      Managers.cpp: the unpack-PBO staging repack. It is what makes the
#                                  ring path issue NO glPixelStorei, and the +6 ms/frame Mali cliff
#                                  lives on the other side of it
#   UnpackRingAvailable            Managers.cpp: honours Features.EsprytDisableUnpackRing and
#                                  self-heals a stale context generation
#   UnpackRingAllocate             Managers.cpp: on the hot upload route
#   RecomputeBackendColorSlots     Managers.cpp: the attachment permutation - three passes plus the
#                                  forced ~0 version memo on every moved attachment; removing the
#                                  empty-point detach breaks the invariant (Managers.cpp:7651-7658)
#   DepthStencilSamplingReadImpl   DirectGLES.cpp: the D24S8 sampling-emulation core (memory
#                                  better-clouds-fullmode), entered from :8617 and :8721
#   ShouldUseCaveatTextureFormat   Utils.cpp: the format handler P4a's InternalFormat descriptors
#                                  feed; a change here silently changes what every texture is
#                                  allocated as
#
# TWO THINGS THIS SCRIPT ADDS TO ITS PARENT, AND NOTHING ELSE (BRIEF-P4A.md D-N: "keeps every other
# property of its parent verbatim"):
#
#   1. A PER-REGION SOURCE PATH. The parent's SOURCE_PATH is a single file (:85); the seventeen live
#      in three. Each row below carries its own path and the extractor reads each file once.
#   2. A REGION KIND. [declared deviation, see DEVIATIONS below] Sixteen of the seventeen are
#      FUNCTIONS. `DepthStencilSamplingReadImpl` is a NAMESPACE (DirectGLES.cpp:8117-8578) - the
#      brief's D-N table names it as though it were a function, and the parent's extractor, which
#      finds a definition as the one `<name> (` whose closing paren is followed by `{`, finds ZERO
#      definitions of it and exits 2 forever. Hashing the whole namespace block is also the
#      stronger reading of "the D24S8 sampling-emulation CORE": the core is the ~460-line block of
#      staging, conversion and readback, not any one function inside it.
#
# Everything else is the parent verbatim, and deliberately so:
#
# HOW A BODY IS EXTRACTED. The file is masked first - comments, string, char and raw-string
# literals are replaced by spaces of the same length, so a brace or a parenthesis inside one can
# never be counted - and the DEFINITION is then found as the one occurrence of `<name> (` whose
# closing parenthesis is followed (past qualifiers like const/noexcept) by `{`. That is what tells
# a definition from a forward declaration and from a call site: a call's `)` is followed by `)`,
# `;` or `,`, never by `{`. A NAMESPACE row is found as the one occurrence of `namespace <name>`
# followed by `{`. The body is then brace matched in the masked text and hashed from the ORIGINAL
# text, so a comment change inside one of these is a difference too - which is deliberate: the
# claim is "byte-identical", and a comment that stopped describing what the code does is exactly
# the kind of drift a "verbatim move" is supposed to be checked for.
#
# THE HASH'S EXACT EXTENT, so that the claim is not read wider than it is (review F-m3, inherited
# from the parent verbatim): it starts at the beginning of the LINE THAT CARRIES THE NAME and ends
# at the region's closing brace. A return type, an attribute or a template header sitting on an
# EARLIER line of a multi-line signature is therefore OUTSIDE the hash, and changing one of those
# alone does not move this gate. The end IS covered - the self-test's tail controls below prove
# the extent reaches the closing brace - and G1's symbol report is what catches a signature that
# changed shape.
#
# Exactly one definition must be found per name. Zero or two is exit 2 (could not run), never a
# silent pass: a rename this gate could not follow must not read as "nothing moved". Two is the
# expected shape of a #if/#else pair that re-spells one of these bodies beside an untouched copy -
# a finding, not a limitation, because two ladders drift (ID-11, ID-15).
#
# A SHARED TEMPLATE OVER AN ACCESSOR INTERFACE IS REJECTED (ID-13, MEASUREMENTS.md:521): it resizes
# pull symbols and breaks G1. If a P4a arm needs a variant of one of the seventeen it gets its own
# `#if MOBILEGL_PIPE_PUSH` function with its own name and its own pinned sha - the
# FlushPendingRangesFrom shape.
#
# Usage:
#   scripts/p4a_untouched_regions.sh <ref-a> <ref-b>   compare the seventeen at two git refs
#   scripts/p4a_untouched_regions.sh <ref>             print the seventeen shas at one ref (a
#                                                      baseline capture: ... > p4a-before.sha)
#   scripts/p4a_untouched_regions.sh --self-test       prove the comparison can go red
#
# stdout is always the sha list - `<sha256>  <region>`, one per line, in the fixed order above - so
# a baseline capture is a plain redirect. Everything else goes to stderr.
#
# Both arguments are GIT REFS: the gate is about what landed, so an uncommitted edit is invisible
# by design. Use HEAD after committing, which is what D.1 and the CI row do.
#
# Exit codes: 0 the seventeen are identical at both refs (or a single ref was listed);
#             1 at least one moved - the first one in the fixed order is named on stderr;
#             2 the gate could not run: a bad ref, a missing file, a name that is not defined
#               exactly once, or a self-test whose control failed to trip.
#
# DEVIATIONS from BRIEF-P4A.md D-N, both declared here rather than in a commit message so that the
# next reader of this file meets them:
#   D-N/1  the namespace region kind, above.
#   D-N/2  the PINNED baseline is CONSULTED UNCONDITIONALLY for FlushPendingRangesFrom, where the
#          parent consulted it only when <ref-a> did not define the function. D-N says that row is
#          compared "against its pinned sha", and at P4a's base ref the function DOES exist - so
#          the parent's fallback would silently never fire and the pin would stop being the
#          baseline the brief names. INTEGRATOR DECISION ID-41 has since made this the parent's
#          reading too, so D-N/2 is no longer a deviation between the two scripts; it is kept here
#          as the record of why this one got there first.
#
#          The two answers now DISAGREE on every ref CI passes, and that is the expected state
#          rather than a finding: P5 (b1) re-parameterised the shipping ladder
#          (hostBaseFrom/hostBaseTo, a MOBILEGL_PIPE_VERIFY-only StageSnapshotTooNarrow log, the
#          tier-1 access computation moved into InvalidateFlushAccessFor), the PULL arm is
#          byte-identical across that change and G1 reports .text +0, and ID-41 ruled that the row
#          be RE-PINNED on the reviewed P5 body rather than reverted or quietly left comparing
#          against <ref-a>. This script says so on stderr, in both directions, and keeps the PIN -
#          because the pin is the reviewed text.
set -u -o pipefail

# One row per region: <name>@<kind>@<path>. The ORDER is the fixed order the sha list is printed
# in and the order `compare_lists` names the first mover from; P3a's eleven keep their parent's
# positions so a reader can diff the two scripts' outputs.
REGIONS="\
IsPoolable@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
EnrollIntoPool@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
AcquireFromPool@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
TrimBufferPool@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
ClearBufferPool@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
ProcessDeferredBufferReleases@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
CreateRingStorage@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
RingAvailable@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
RingAllocate@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
FlushPendingRangesNow@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
FlushPendingRangesFrom@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
StageBlocksIntoUnpackRing@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
UnpackRingAvailable@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
UnpackRingAllocate@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
RecomputeBackendColorSlots@function@MobileGL/MG_Backend/DirectGLES/Managers.cpp
DepthStencilSamplingReadImpl@namespace@MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
ShouldUseCaveatTextureFormat@function@MobileGL/MG_Backend/DirectGLES/Utils.cpp"

EXPECTED_FUNCTION_COUNT=17

# The one region born in P3a, so there is no body at P4a's base ref that this phase reviewed: its
# baseline is PINNED and consulted UNCONDITIONALLY (DEVIATIONS D-N/2, and ID-41 for the parent).
#
# THE PIN, AND WHAT RE-PINS IT. Whoever moves this body deliberately replaces BOTH lines and
# writes the decision beside them; a pin with no commit and no decision next to it is a number
# nobody can audit. The parent script carries the identical table and the two must not drift.
#   3e298c9a  37fc94ff...  ID-15, P3a: the two-arm shape, reviewed and accepted
#   3dadd4c1  172b0222...  ID-41, P5 (b1): [Fix] (DirectGLES): make the extent hostBase is good
#                          for a parameter of the flush ladder, so tier 1's widening refusal is
#                          live code the moment a SEG_STAGE snapshot is narrower than the queued
#                          range.  <- CURRENT
PINNED_FUNCTIONS="FlushPendingRangesFrom"
PINNED_BASELINE_REF=3dadd4c1
PINNED_BASELINE_DECISION=ID-41
PINNED_SHA_FlushPendingRangesFrom=172b022273db01b16e772d15b269ffcd797fe38c767f7354d83ce113a66040d0

# The regions the self-test perturbs, one negative control each. FOUR, exactly as D-N requires, and
# each is a different shape so that a control which only ever perturbed the easy one cannot leave
# the others unproven:
#   ClearBufferPool             P3a's easy control - small, no forward declaration, no overload, so
#                               a failure there is about the COMPARISON rather than the extraction
#   FlushPendingRangesNow       P3a's hard control - the longest body in the set, three nested
#                               tiers, its own early returns, and one of a PAIR of identically
#                               shaped bodies in two preprocessor arms
#   RecomputeBackendColorSlots  P4a's method-shaped control: a member function spelled
#                               `BackendFramebufferObject::RecomputeBackendColorSlots(` with a
#                               multi-line signature and a call site of its own, so a naive
#                               extraction picks the wrong occurrence
#   StageBlocksIntoUnpackRing   P4a's static-in-an-anonymous-namespace control, on the hot upload
#                               route, and the one whose file position sits between two other
#                               protected bodies
SELF_TEST_FUNCTIONS="ClearBufferPool FlushPendingRangesNow RecomputeBackendColorSlots StageBlocksIntoUnpackRing"

say() { echo "[p4a-untouched] $*" >&2; }

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 2
cd "$REPO_ROOT" || exit 2

WORK_DIR=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK_DIR"' EXIT

# Every distinct source path the rows name, in first-appearance order.
region_paths() {
  printf '%s\n' "$REGIONS" | awk -F@ '!seen[$3]++ { print $3 }'
}

# The extractor. Three modes, all over a SPEC FILE of `<name>\t<kind>\t<file>` rows so that the
# self-test can drive it without inventing a commit: `extract` prints one `<sha>  <name>` line per
# row in spec order, `perturb` writes a copy of one file with one statement inserted at the top of
# one region's body, `count` is unused by the shell and kept out.
PY=$WORK_DIR/extract.py
cat > "$PY" <<'PYTHON'
import hashlib
import re
import sys


def mask(text):
    """Comments and literals replaced by spaces of the same length, offsets preserved."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                out[i] = ' '
                i += 1
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            out[i] = out[i + 1] = ' '
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i + 1 < n:
                out[i] = out[i + 1] = ' '
                i += 2
        elif c == 'R' and i + 1 < n and text[i + 1] == '"':
            # R"delim( ... )delim" - a shader source is one of these, and it is full of braces.
            close = text.find('(', i + 2)
            if close < 0:
                i += 1
                continue
            delim = text[i + 2:close]
            end = text.find(')' + delim + '"', close)
            end = n if end < 0 else end + len(delim) + 2
            for j in range(i, end):
                if text[j] != '\n':
                    out[j] = ' '
            i = end
        elif c == "'" and i > 0 and (text[i - 1].isdigit() or
                                     (text[i - 1] in 'abcdefABCDEF' and i > 1 and
                                      text[i - 2] in "0123456789abcdefABCDEFxX")):
            # A C++14 DIGIT SEPARATOR (16'777'216, 0xff'ff), not a char literal. Treating it as one
            # would blank forward to the next apostrophe - which can be a whole function away, in a
            # comment - and silently swallow a brace, shifting a body's extent with no diagnostic.
            i += 1
        elif c in '"\'':
            quote = c
            out[i] = ' '
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\' and i + 1 < n:
                    out[i] = ' '
                    i += 1
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i < n:
                out[i] = ' '
                i += 1
        else:
            i += 1
    return ''.join(out)


def match_forward(masked, start, opener, closer):
    depth = 0
    for i in range(start, len(masked)):
        if masked[i] == opener:
            depth += 1
        elif masked[i] == closer:
            depth -= 1
            if depth == 0:
                return i
    return -1


def find_function(text, masked, name):
    """Every (begin, end) at which `name` is DEFINED as a function."""
    hits = []
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', masked):
        open_paren = m.end() - 1
        close_paren = match_forward(masked, open_paren, '(', ')')
        if close_paren < 0:
            continue
        tail = masked[close_paren + 1:close_paren + 96]
        # Past whatever qualifiers a definition may carry; anything else means this was a call
        # or a declaration.
        stripped = re.sub(r'^(\s|const\b|noexcept\b|override\b|final\b)*', '', tail)
        if not stripped.startswith('{'):
            continue
        brace = masked.index('{', close_paren)
        end = match_forward(masked, brace, '{', '}')
        if end < 0:
            continue
        begin = text.rfind('\n', 0, m.start()) + 1
        hits.append((begin, end + 1))
    return hits


def find_namespace(text, masked, name):
    """Every (begin, end) at which `name` is DEFINED as a namespace.

    P4a's DepthStencilSamplingReadImpl is a namespace, not a function (DirectGLES.cpp:8117-8578):
    the D24S8 sampling-emulation CORE is the whole block, and a function-shaped search finds no
    definition of it at all. The closing `} // namespace <name>` comment is masked away by the
    time this runs, so the extent is decided by brace matching exactly as a function's is - and a
    USE of the namespace (`DepthStencilSamplingReadImpl::Read(...)`) is not matched, because the
    `namespace` keyword is not in front of it.
    """
    hits = []
    for m in re.finditer(r'\bnamespace\s+' + re.escape(name) + r'\b', masked):
        rest = masked[m.end():m.end() + 96]
        if not rest.lstrip().startswith('{'):
            continue
        brace = masked.index('{', m.end())
        end = match_forward(masked, brace, '{', '}')
        if end < 0:
            continue
        begin = text.rfind('\n', 0, m.start()) + 1
        hits.append((begin, end + 1))
    return hits


def find_definition(text, masked, name, kind):
    if kind == 'namespace':
        return find_namespace(text, masked, name)
    return find_function(text, masked, name)


def read_spec(path):
    rows = []
    with open(path, encoding='utf-8') as spec:
        for line in spec:
            line = line.rstrip('\n')
            if not line:
                continue
            name, kind, source = line.split('\t')
            rows.append((name, kind, source))
    return rows


class Sources(object):
    """Each file read and masked once, however many regions name it."""

    def __init__(self):
        self.cache = {}

    def get(self, path):
        if path not in self.cache:
            text = open(path, encoding='utf-8', newline='').read()
            self.cache[path] = (text, mask(text))
        return self.cache[path]


def extract(rows):
    sources = Sources()
    out, problems = [], []
    for name, kind, source in rows:
        try:
            text, masked = sources.get(source)
        except OSError as err:
            problems.append('%s: cannot read %s (%s)' % (name, source, err))
            continue
        hits = find_definition(text, masked, name, kind)
        if len(hits) != 1:
            problems.append('%s: expected exactly one %s definition in %s, found %d'
                            % (name, kind, source, len(hits)))
            if len(hits) > 1:
                # The expected shape of this failure, and it is a FINDING rather than a limitation:
                # a `#if MOBILEGL_PIPE_PUSH` arm that re-spells one of these bodies beside an
                # untouched `#else` copy satisfies G1 (the pull build's text did not move) and
                # defeats G5 (the push build compiles a second copy that can drift). The gate
                # cannot say which of the two is "the" body, and must not pick one.
                for begin, end in hits:
                    problems.append('  ...definition at line %d, %d lines'
                                    % (text[:begin].count('\n') + 1,
                                       text[:end].count('\n') - text[:begin].count('\n') + 1))
                problems.append('  the P4a arm must CALL the untouched %s, not carry a copy of it: '
                                'two ladders drift (BRIEF-P4A.md D-N, ID-11, ID-15)' % name)
            continue
        begin, end = hits[0]
        body = text[begin:end]
        out.append((hashlib.sha256(body.encode('utf-8')).hexdigest(), name))
    return out, problems


def perturb(rows, target, src, dst, where='head'):
    """Insert one line into a region's body at its HEAD or its TAIL, or one TOKEN at its head.

    TWO POSITIONS, AND THE SECOND ONE IS REVIEW FINDING F-m2. Every control used to insert at the
    very first byte after the opening brace, so all four of them would still have tripped if
    find_definition() returned an extent that stopped short of the closing brace - nothing in the
    self-test proved that a region reaches its own end, and an extractor that hashed all but the
    last statement of every body would have passed the whole self-test while being blind to a
    change in that statement. The `tail` position inserts immediately BEFORE the closing brace,
    which is the byte such an extractor would have dropped.
    """
    for name, kind, source in rows:
        if name != target:
            continue
        text = open(src, encoding='utf-8', newline='').read()
        masked = mask(text)
        hits = find_definition(text, masked, name, kind)
        if len(hits) != 1:
            sys.stderr.write('[p4a-untouched] cannot perturb %s: %d definitions\n'
                             % (target, len(hits)))
            return 2
        begin, end = hits[0]
        if where == 'token':
            # ONE TOKEN - a single empty statement at the head of the body - and nothing else.
            # ID-41(d) asks the pinned row's control to perturb the LADDER rather than a comment
            # beside it, and this is the smallest edit that is unambiguously code: a reader cannot
            # answer "the gate only notices comments". The perturbed copy is never compiled, only
            # hashed, so an empty statement is legal here in a way it would not be in the tree.
            brace = masked.index('{', begin)
            patched = text[:brace + 1] + ';' + text[brace + 1:]
        elif where == 'tail':
            # end is one PAST the closing brace (find_function / find_namespace both return
            # `match_forward(...) + 1`), so end - 1 is the brace itself and this lands inside the
            # body, one character before it ends.
            note = '\n            // p4a_untouched_regions.sh --self-test: a body whose TAIL moved.\n'
            patched = text[:end - 1] + note + text[end - 1:]
        else:
            # The opening brace is located in the MASKED text and then used as an offset into the
            # original: a brace inside a comment or a string on the signature line would otherwise
            # send the perturbation somewhere that is not the body, and the control would be
            # proving the wrong thing. Offsets are identical between the two by construction
            # (mask() preserves length).
            brace = masked.index('{', begin)
            patched = (text[:brace + 1] +
                       '\n            // p4a_untouched_regions.sh --self-test: a body that MOVED.\n' +
                       text[brace + 1:])
        open(dst, 'w', encoding='utf-8', newline='').write(patched)
        return 0
    sys.stderr.write('[p4a-untouched] %s is not one of the regions\n' % target)
    return 2


def main(argv):
    # The sha list is parsed by awk, and on Windows (Git Bash, MSYS python) text-mode stdout
    # translates '\n' into CRLF - after which `$2 == n` never matches and the gate exits 1 on an
    # untouched tree. Linux CI never saw it; a developer running the gate locally always did.
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(newline='\n')
    mode = argv[1]
    rows = read_spec(argv[2])
    if mode == 'extract':
        found, problems = extract(rows)
        for problem in problems:
            sys.stderr.write('[p4a-untouched] %s\n' % problem)
        for sha, name in found:
            sys.stdout.write('%s  %s\n' % (sha, name))
        return 2 if problems else 0
    if mode == 'perturb':
        return perturb(rows, argv[3], argv[4], argv[5], argv[6] if len(argv) > 6 else 'head')
    sys.stderr.write('[p4a-untouched] unknown mode %r\n' % mode)
    return 2


sys.exit(main(sys.argv))
PYTHON

# Write a spec file whose rows point at the blobs in "$2" (a directory holding one file per
# region path, named by a sanitised path so two files of the same basename could never collide).
blob_name() { printf '%s' "$1" | tr '/' '_'; }

write_spec() {
  local dir=$1 spec=$2 row name kind source
  : > "$spec"
  printf '%s\n' "$REGIONS" | while IFS='@' read -r name kind source; do
    [ -n "$name" ] || continue
    printf '%s\t%s\t%s/%s\n' "$name" "$kind" "$dir" "$(blob_name "$source")" >> "$spec"
  done
}

# Materialise every region source at a git ref into "$2".
checkout_ref() {
  local ref=$1 dir=$2 source
  mkdir -p "$dir" || return 2
  for source in $(region_paths); do
    if ! git show "$ref:$source" > "$dir/$(blob_name "$source")" 2>"$WORK_DIR/show.err"; then
      say "cannot read $source at '$ref':"
      sed 's/^/[p4a-untouched]   /' "$WORK_DIR/show.err" >&2
      return 2
    fi
  done
  return 0
}

# Extract at a git ref into "$WORK_DIR/$2.sha". Every region must be defined exactly once there;
# this is the side the gate is ABOUT (<ref-b>, and the single-ref listing's ref).
extract_ref() {
  local ref=$1 out=$2
  checkout_ref "$ref" "$WORK_DIR/$out" || return 2
  write_spec "$WORK_DIR/$out" "$WORK_DIR/$out.spec"
  python3 "$PY" extract "$WORK_DIR/$out.spec" > "$WORK_DIR/$out.sha"
  return $?
}

# The BASELINE side (<ref-a>). The sixteen ordinary regions are extracted strictly, so a rename of
# one of THOSE is exit 2 rather than a silently short list. FlushPendingRangesFrom then takes the
# PINNED sha (DEVIATIONS D-N/2) whether or not <ref-a> defines it, and a <ref-a> that defines it
# DIFFERENTLY is reported - loudly - because the two answers disagreeing is itself a finding.
extract_baseline() {
  local ref=$1 out=$2 name pinned atRef
  checkout_ref "$ref" "$WORK_DIR/$out" || return 2
  write_spec "$WORK_DIR/$out" "$WORK_DIR/$out.spec.all"
  # The strict pass, minus the pinned rows.
  cp -f "$WORK_DIR/$out.spec.all" "$WORK_DIR/$out.spec" || return 2
  for name in $PINNED_FUNCTIONS; do
    grep -v "^$name$(printf '\t')" "$WORK_DIR/$out.spec" > "$WORK_DIR/$out.spec.tmp" || true
    mv -f "$WORK_DIR/$out.spec.tmp" "$WORK_DIR/$out.spec" || return 2
  done
  if ! python3 "$PY" extract "$WORK_DIR/$out.spec" > "$WORK_DIR/$out.sha" 2>"$WORK_DIR/$out.err"; then
    say "the baseline ref '$ref' does not define the sixteen unpinned regions exactly once each:"
    sed 's/^/[p4a-untouched]   /' "$WORK_DIR/$out.err" >&2
    return 2
  fi
  for name in $PINNED_FUNCTIONS; do
    eval "pinned=\$PINNED_SHA_$name"
    grep "^$name$(printf '\t')" "$WORK_DIR/$out.spec.all" > "$WORK_DIR/$out.spec.pinned" || true
    atRef=$(python3 "$PY" extract "$WORK_DIR/$out.spec.pinned" 2>/dev/null | awk '{ print $1 }')
    if [ -n "$atRef" ] && [ "$atRef" != "$pinned" ]; then
      say "NOTE: $name IS defined at '$ref' and hashes"
      say "  $atRef, which is not the pin"
      say "  ($pinned,"
      say "  captured at $PINNED_BASELINE_REF, $PINNED_BASELINE_DECISION). THE PIN IS WHAT IS"
      say "  COMPARED - it is the reviewed body - and this note is not a verdict in either"
      say "  direction. If '$ref' PREDATES $PINNED_BASELINE_REF the two SHOULD disagree: the pinned"
      say "  body is the change $PINNED_BASELINE_DECISION admitted, which is why it was re-pinned"
      say "  rather than reverted. If it does not predate it, the ladder that ships has moved away"
      say "  from the reviewed text without this gate being re-pinned - re-pin deliberately or"
      say "  revert, but do not leave them disagreeing."
    fi
  done
  # The pinned rows are appended and the list is put back into the FIXED ORDER, both inside
  # apply_pinned_shas - ONE spelling of the substitution, which --self-test's pin controls drive
  # as well, so a control cannot prove only that a copy of the logic agrees with itself.
  apply_pinned_shas "$WORK_DIR/$out.sha" || return 2
  return 0
}

# True when $1 is one of the rows whose baseline is the PIN rather than <ref-a>.
is_pinned_row() {
  local name candidate
  for candidate in $PINNED_FUNCTIONS; do
    [ "$candidate" = "$1" ] && return 0
  done
  return 1
}

# Overwrite the pinned rows of a `<sha>  <region>` list with the shas PINNED at the top of this
# script, then restore the FIXED ORDER (review F-m1: the pinned rows would otherwise be emitted
# LAST while extract_ref emits everything in REGIONS order. The gate itself never noticed -
# compare_lists looks rows up by name - but the documented capture workflow did: the header
# promises "stdout is always the sha list ... in the fixed order above - so a baseline capture is a
# plain redirect", and a baseline captured that way then diffed against a two-ref stdout showed
# seven spurious differences purely from row order).
apply_pinned_shas() {
  local file=$1 name pinned
  for name in $PINNED_FUNCTIONS; do
    eval "pinned=\$PINNED_SHA_$name"
    if [ -z "$pinned" ] || [ "$pinned" = "PLACEHOLDER_SHA" ]; then
      say "$name has no pinned baseline sha; that row cannot be compared"
      return 2
    fi
    grep -v "  $name\$" "$file" > "$file.unpinned" || true
    mv -f "$file.unpinned" "$file" || return 2
    printf '%s  %s\n' "$pinned" "$name" >> "$file"
  done
  reorder_sha_list "$file" || return 2
  return 0
}

# Rewrite a `<sha>  <region>` list in REGIONS order, in place. Rows whose name is not in REGIONS
# would be a bug in the caller rather than a difference, so they are kept at the end where they are
# visible instead of being dropped.
reorder_sha_list() {
  local file=$1 name
  : > "$file.ordered" || return 2
  printf '%s\n' "$REGIONS" | awk -F@ '{ print $1 }' > "$WORK_DIR/reorder.names" || return 2
  while read -r name; do
    [ -n "$name" ] || continue
    awk -v n="$name" '$2 == n { print }' "$file" >> "$file.ordered" || return 2
  done < "$WORK_DIR/reorder.names"
  awk 'NR == FNR { known[$0] = 1; next } !($2 in known) { print }' \
      "$WORK_DIR/reorder.names" "$file" >> "$file.ordered" || return 2
  mv -f "$file.ordered" "$file" || return 2
  return 0
}

# Compare two sha lists. Prints the first region that moved, in REGIONS order.
compare_lists() {
  local a=$1 b=$2 labelA=$3 labelB=$4 moved=0 name shaA shaB
  printf '%s\n' "$REGIONS" | awk -F@ '{ print $1 }' > "$WORK_DIR/order"
  while read -r name; do
    [ -n "$name" ] || continue
    shaA=$(awk -v n="$name" '$2 == n { print $1 }' "$a")
    shaB=$(awk -v n="$name" '$2 == n { print $1 }' "$b")
    if [ "$shaA" != "$shaB" ]; then
      if [ "$moved" -eq 0 ]; then
        say "FIRST REGION THAT MOVED: $name"
        say "  $labelA ${shaA:-<not found>}"
        say "  $labelB ${shaB:-<not found>}"
        if is_pinned_row "$name"; then
          # ITS OWN MESSAGE, and that is R-16 rather than decoration: the pinned row and the
          # sixteen ref-a rows fail differently and are fixed differently, so a reader who sees
          # only the generic paragraph below goes looking for a diff against <ref-a> that does not
          # exist.
          say "  $name IS A PINNED ROW ($PINNED_BASELINE_DECISION): its baseline is ALWAYS the sha"
          say "  PINNED in this script - the body reviewed at $PINNED_BASELINE_REF - and never the"
          say "  body at '$labelA'. So this is not a diff against the base ref: the ladder that"
          say "  SHIPS has moved away from the text that was reviewed. Either re-pin deliberately,"
          say "  replacing the sha AND the commit AND the decision beside it in BOTH this script"
          say "  and its parent scripts/p3a_untouched_regions.sh, or revert the body."
        fi
        say "  G5 (ARCHITECTURE.md:318, :321, :515) says the Espryt do-not-touch list is literal:"
        say "  P3a's buffer pool, deferred-release drain, three rings and BOTH arms of the three-tier"
        say "  flush drain, plus P4a's unpack-PBO staging repack and its two ring helpers, the"
        say "  attachment permutation, the D24S8 sampling-emulation core and the format-caveat"
        say "  handler, all move VERBATIM. If this change is intended it is not a P4a change and it"
        say "  needs its own commit and its own reason; if it is not, revert the body. A P4a arm that"
        say "  carries its own COPY of one of these beside an untouched one is the same finding: the"
        say "  new arm must CALL the untouched region, not re-spell it."
      else
        say "also moved: $name"
      fi
      moved=$((moved + 1))
    fi
  done < "$WORK_DIR/order"
  return $((moved > 0 ? 1 : 0))
}

# --- self-test ------------------------------------------------------------------------------
# A gate that always says "identical" and a gate that is working produce the same green, so the
# comparison has to be shown failing. Both controls run: the POSITIVE ones (an untouched copy
# compares equal; an edit OUTSIDE the regions is invisible; a baseline that names another sha for
# the PINNED row is overridden by the pin) rule out a comparison that reports every region as
# moved, and the NINE NEGATIVE ones - D-N's four regions, each perturbed at the HEAD of its body
# and again at its TAIL, plus a ONE-TOKEN edit to the pinned ladder compared AGAINST THE PIN
# (ID-41) - rule out the comparison that never reports any, the extraction whose extent stops
# before the closing brace (F-m2), and a pin that nothing consults.
if [ "${1:-}" = "--self-test" ]; then
  [ $# -eq 1 ] || { say "--self-test takes no other arguments"; exit 2; }
  mkdir -p "$WORK_DIR/pristine" || exit 2
  for source in $(region_paths); do
    [ -f "$source" ] || { say "$source is not in this tree"; exit 2; }
    cp -f "$source" "$WORK_DIR/pristine/$(blob_name "$source")" || exit 2
  done
  write_spec "$WORK_DIR/pristine" "$WORK_DIR/pristine.spec"
  if ! python3 "$PY" extract "$WORK_DIR/pristine.spec" > "$WORK_DIR/pristine.sha"; then
    say "the extractor could not read the $EXPECTED_FUNCTION_COUNT regions out of the working tree"
    exit 2
  fi
  found=$(wc -l < "$WORK_DIR/pristine.sha")
  if [ "$found" -ne "$EXPECTED_FUNCTION_COUNT" ]; then
    say "extracted $found regions, expected $EXPECTED_FUNCTION_COUNT"
    exit 2
  fi
  say "positive control: $EXPECTED_FUNCTION_COUNT regions extracted from the working tree"

  cp -r "$WORK_DIR/pristine" "$WORK_DIR/copy" || exit 2
  write_spec "$WORK_DIR/copy" "$WORK_DIR/copy.spec"
  python3 "$PY" extract "$WORK_DIR/copy.spec" > "$WORK_DIR/copy.sha" || exit 2
  if ! compare_lists "$WORK_DIR/pristine.sha" "$WORK_DIR/copy.sha" "pristine" "copy" 2>/dev/null; then
    say "POSITIVE CONTROL FAILED: an untouched copy compared as MOVED. The comparison is reporting"
    say "differences that are not there, so its verdict means nothing in either direction."
    exit 2
  fi
  say "positive control: an untouched copy compares equal"

  # The second positive control, and it is the one that matters for P4a: the rest of these three
  # files IS going to be rewritten (the twins become handle-shaped, the descriptors replace the
  # frontend reads), so a gate that fired on any edit to them would have to be switched off in the
  # same week it landed. An edit outside the seventeen must be invisible here - in EVERY file, so
  # that a per-file extraction bug cannot hide behind the one file that was probed.
  cp -r "$WORK_DIR/pristine" "$WORK_DIR/outside" || exit 2
  for source in $(region_paths); do
    { echo "// p4a_untouched_regions.sh --self-test: an edit OUTSIDE the seventeen regions."; \
      cat "$WORK_DIR/pristine/$(blob_name "$source")"; } > "$WORK_DIR/outside/$(blob_name "$source")"
  done
  write_spec "$WORK_DIR/outside" "$WORK_DIR/outside.spec"
  python3 "$PY" extract "$WORK_DIR/outside.spec" > "$WORK_DIR/outside.sha" || exit 2
  if ! compare_lists "$WORK_DIR/pristine.sha" "$WORK_DIR/outside.sha" "pristine" "outside" \
       2>/dev/null; then
    say "POSITIVE CONTROL FAILED: an edit OUTSIDE the seventeen regions was reported as one of them"
    say "moving. This gate would fire on every P4a commit to these three files and would have to be"
    say "silenced, which is the same as not having it."
    exit 2
  fi
  say "positive control: an edit outside the seventeen regions is invisible, in all three files"

  # TWO negative controls per SELF_TEST_FUNCTIONS entry - one at the HEAD of the body and one at
  # its TAIL. Each is run on its own, from the pristine copy, so the message it produces has to
  # NAME that region: a control that only proved "some region moved" would not distinguish "this
  # row is compared" from "this row is extracted as an empty range and every comparison of it is
  # vacuous".
  #
  # THE TAIL HALF IS REVIEW FINDING F-m2. With head-only controls, an extraction that returned an
  # extent stopping short of the closing brace would still have tripped all four - the inserted
  # line is at the very first byte of the body - so nothing here proved that a region reaches its
  # own end, and a change to the LAST statement of a protected body would have been invisible to a
  # gate whose self-test was fully green. The tail control inserts immediately before the closing
  # brace, which is exactly the byte such an extractor would have dropped.
  controls=0
  for target in $SELF_TEST_FUNCTIONS; do
    targetSource=$(printf '%s\n' "$REGIONS" | awk -F@ -v n="$target" '$1 == n { print $3 }')
    [ -n "$targetSource" ] || { say "$target is not one of the regions"; exit 2; }
    for position in head tail; do
      rm -rf "$WORK_DIR/perturbed"
      cp -r "$WORK_DIR/pristine" "$WORK_DIR/perturbed" || exit 2
      write_spec "$WORK_DIR/perturbed" "$WORK_DIR/perturbed.spec"
      python3 "$PY" perturb "$WORK_DIR/perturbed.spec" "$target" \
          "$WORK_DIR/pristine/$(blob_name "$targetSource")" \
          "$WORK_DIR/perturbed/$(blob_name "$targetSource")" "$position" || exit 2
      python3 "$PY" extract "$WORK_DIR/perturbed.spec" > "$WORK_DIR/perturbed.sha" || exit 2
      if compare_lists "$WORK_DIR/pristine.sha" "$WORK_DIR/perturbed.sha" "pristine" "perturbed" \
           2> "$WORK_DIR/perturbed.err"; then
        say "NEGATIVE CONTROL DID NOT TRIP: $target's body was changed at its $position and the"
        say "comparison still reported every region as identical. This gate cannot go red for the"
        say "reason it exists, so every green it has ever printed means nothing."
        if [ "$position" = tail ]; then
          say "  A TAIL control that does not trip while the head one does means the extracted"
          say "  extent stops before the closing brace: the last statement of every protected body"
          say "  is outside the hash and can be rewritten silently."
        fi
        exit 2
      fi
      if ! grep -q "FIRST REGION THAT MOVED: $target" "$WORK_DIR/perturbed.err"; then
        say "NEGATIVE CONTROL TRIPPED FOR THE WRONG REASON: the comparison went red but did not name"
        say "$target as the first region that moved ($position control). It said:"
        sed 's/^/[p4a-untouched]   /' "$WORK_DIR/perturbed.err" >&2
        exit 2
      fi
      controls=$((controls + 1))
      say "negative control $controls: a perturbed $target body ($position) is reported, and named"
    done
  done
  if [ "$controls" -ne 8 ]; then
    say "expected EIGHT negative controls (BRIEF-P4A.md D-N's four regions, each at its head and at"
    say "its tail), ran $controls"
    exit 2
  fi

  # --- THE PINNED ROW (ID-41) -----------------------------------------------------------------
  # TWO more controls, and they exist because none of the ten above can see the pin at all: every
  # one of them compares one extraction of the working tree against another, so they would all be
  # green on a build of this script in which PINNED_SHA_* was never read by anything. The pinned
  # row's whole claim is "the baseline is the PIN, not <ref-a>" (D-N/2, ID-41), and that claim
  # needs its own two.
  for target in $PINNED_FUNCTIONS; do
    eval "pinned=\$PINNED_SHA_$target"
    targetSource=$(printf '%s\n' "$REGIONS" | awk -F@ -v n="$target" '$1 == n { print $3 }')
    [ -n "$targetSource" ] || { say "$target is not one of the regions"; exit 2; }

    # (1) PIN PRECEDENCE, positive. A baseline that carries some OTHER sha for the pinned row -
    # which is the shape of every <ref-a> CI passes, since 37da3c3a DOES define
    # FlushPendingRangesFrom and no longer hashes the pin - must come out of apply_pinned_shas
    # carrying the PIN.
    grep -v "  $target\$" "$WORK_DIR/pristine.sha" > "$WORK_DIR/pinprec.sha" || true
    printf '%s  %s\n' \
        "0000000000000000000000000000000000000000000000000000000000000000" "$target" \
        >> "$WORK_DIR/pinprec.sha"
    apply_pinned_shas "$WORK_DIR/pinprec.sha" || exit 2
    got=$(awk -v n="$target" '$2 == n { print $1 }' "$WORK_DIR/pinprec.sha")
    if [ "$got" != "$pinned" ]; then
      say "PIN CONTROL FAILED: a baseline that carried a DIFFERENT sha for $target came out as"
      say "  '${got:-<absent>}' and not as the pin ($pinned). That row would be compared against"
      say "  <ref-a> again, which is exactly what D-N/2 and $PINNED_BASELINE_DECISION forbid."
      exit 2
    fi
    say "pin control: a baseline that defines $target differently is overridden by the PIN"

    # (2) A ONE-TOKEN EDIT TO THE PINNED LADDER, negative, AGAINST THE PIN. The comparison must go
    # red, must name the region, and must say the region is PINNED - R-16's "a control asserts its
    # OWN failure string": the pinned row and the sixteen ref-a rows are fixed differently, and a
    # reader who gets only the generic paragraph goes looking for a diff against <ref-a> that does
    # not exist.
    rm -rf "$WORK_DIR/pinperturbed"
    cp -r "$WORK_DIR/pristine" "$WORK_DIR/pinperturbed" || exit 2
    write_spec "$WORK_DIR/pinperturbed" "$WORK_DIR/pinperturbed.spec"
    python3 "$PY" perturb "$WORK_DIR/pinperturbed.spec" "$target" \
        "$WORK_DIR/pristine/$(blob_name "$targetSource")" \
        "$WORK_DIR/pinperturbed/$(blob_name "$targetSource")" token || exit 2
    python3 "$PY" extract "$WORK_DIR/pinperturbed.spec" > "$WORK_DIR/pinperturbed.sha" || exit 2
    cp -f "$WORK_DIR/pristine.sha" "$WORK_DIR/pinbase.sha" || exit 2
    apply_pinned_shas "$WORK_DIR/pinbase.sha" || exit 2
    if compare_lists "$WORK_DIR/pinbase.sha" "$WORK_DIR/pinperturbed.sha" \
         "PIN($PINNED_BASELINE_REF)" "one-token-perturbed" 2> "$WORK_DIR/pinperturbed.err"; then
      say "NEGATIVE CONTROL DID NOT TRIP: one token was inserted into $target's body and the"
      say "comparison AGAINST THE PIN still reported every region as identical. The pinned row is"
      say "not being compared at all, so every green this gate has printed for it means nothing."
      exit 2
    fi
    if ! grep -q "FIRST REGION THAT MOVED: $target" "$WORK_DIR/pinperturbed.err"; then
      say "NEGATIVE CONTROL TRIPPED FOR THE WRONG REASON: the comparison against the pin went red"
      say "but did not name $target as the first region that moved. It said:"
      sed 's/^/[p4a-untouched]   /' "$WORK_DIR/pinperturbed.err" >&2
      exit 2
    fi
    if ! grep -q "$target IS A PINNED ROW" "$WORK_DIR/pinperturbed.err"; then
      say "NEGATIVE CONTROL TRIPPED FOR THE WRONG REASON: it went red and named $target, but did"
      say "not say that this row's baseline is the PIN. That is the half a reader acts on, and a"
      say "control that does not assert its own message is not a control (R-16). It said:"
      sed 's/^/[p4a-untouched]   /' "$WORK_DIR/pinperturbed.err" >&2
      exit 2
    fi
    controls=$((controls + 1))
    say "negative control $controls: a ONE-TOKEN edit to the pinned $target body goes red AGAINST"
    say "  THE PIN, is named, and says the row is pinned"

    # NOT a failure, deliberately: --self-test is about whether the comparison works, and it runs
    # on the WORKING tree, which may legitimately carry an uncommitted edit. The two-ref gate is
    # what fails when the committed region has left the pin.
    treeSha=$(awk -v n="$target" '$2 == n { print $1 }' "$WORK_DIR/pristine.sha")
    if [ "$treeSha" != "$pinned" ]; then
      say "NOTE: this working tree's $target hashes $treeSha, not the pin ($pinned). The"
      say "  self-test's verdict is unaffected; the two-ref gate will be RED until you re-pin or"
      say "  revert."
    fi
  done

  say "self-test passed: $controls negative controls, all tripped and all named"
  exit 0
fi

# --- the gate -------------------------------------------------------------------------------
case $# in
  1)
    extract_baseline "$1" one || exit 2
    cat "$WORK_DIR/one.sha"
    say "listed the $EXPECTED_FUNCTION_COUNT regions at $1"
    exit 0
    ;;
  2) ;;
  *)
    say "usage: $0 <ref-a> <ref-b> | $0 <ref> | $0 --self-test"
    exit 2
    ;;
esac

extract_baseline "$1" a || exit 2
extract_ref "$2" b || exit 2
cat "$WORK_DIR/b.sha"

if compare_lists "$WORK_DIR/a.sha" "$WORK_DIR/b.sha" "$1" "$2"; then
  say "the $EXPECTED_FUNCTION_COUNT pool / ring / unpack-staging / attachment-permutation /"
  say "depth-stencil-sampling / format-caveat regions are byte-identical between $1 and $2"
  exit 0
fi
exit 1
