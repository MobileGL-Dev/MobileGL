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
#          parent consults it only when <ref-a> does not define the function. D-N says that row is
#          compared "against its pinned 3e298c9a sha", and at P4a's base ref the function DOES
#          exist - so the parent's fallback would silently never fire and the pin would stop being
#          the baseline the brief names. Both readings agree on this tree (measured: the body at
#          37da3c3a hashes to the pinned value); where they would ever disagree, this script says
#          so on stderr and keeps the PIN, because the pin is the reviewed text.
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
# baseline is the sha captured at 3e298c9a, the commit at which the two-arm shape was reviewed and
# accepted (ID-15). See DEVIATIONS D-N/2 for why it is consulted unconditionally.
PINNED_FUNCTIONS="FlushPendingRangesFrom"
PINNED_BASELINE_REF=3e298c9a
PINNED_SHA_FlushPendingRangesFrom=37fc94ffc5991923d222d585daa3af6511d2352d255623026ce35a3b6963c4a6

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


def perturb(rows, target, src, dst):
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
        # The opening brace is located in the MASKED text and then used as an offset into the
        # original: a brace inside a comment or a string on the signature line would otherwise send
        # the perturbation somewhere that is not the body, and the control would be proving the
        # wrong thing. Offsets are identical between the two by construction (mask() preserves
        # length).
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
        return perturb(rows, argv[3], argv[4], argv[5])
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
    if [ -z "$pinned" ] || [ "$pinned" = "PLACEHOLDER_SHA" ]; then
      say "$name has no pinned baseline sha; that row cannot be compared"
      return 2
    fi
    grep "^$name$(printf '\t')" "$WORK_DIR/$out.spec.all" > "$WORK_DIR/$out.spec.pinned" || true
    atRef=$(python3 "$PY" extract "$WORK_DIR/$out.spec.pinned" 2>/dev/null | awk '{ print $1 }')
    if [ -n "$atRef" ] && [ "$atRef" != "$pinned" ]; then
      say "NOTE: $name IS defined at '$ref' and hashes $atRef, which is NOT the sha pinned in this"
      say "  script ($pinned, captured at $PINNED_BASELINE_REF). The PIN is what is compared - it is"
      say "  the reviewed text (ID-15) - but the two disagreeing means the push ladder moved between"
      say "  $PINNED_BASELINE_REF and '$ref' without this gate being re-pinned. Re-pin deliberately or"
      say "  revert; do not leave them disagreeing."
    fi
    printf '%s  %s\n' "$pinned" "$name" >> "$WORK_DIR/$out.sha"
  done
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
# compares equal; an edit OUTSIDE the regions is invisible) rule out a comparison that reports
# every region as moved, and the four NEGATIVE ones rule out the comparison that never reports any.
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

  # One negative control per SELF_TEST_FUNCTIONS entry. Each is run on its own, from the pristine
  # copy, so the message it produces has to NAME that region - a control that only proved "some
  # region moved" would not distinguish "this row is compared" from "this row is extracted as an
  # empty range and every comparison of it is vacuous".
  controls=0
  for target in $SELF_TEST_FUNCTIONS; do
    targetSource=$(printf '%s\n' "$REGIONS" | awk -F@ -v n="$target" '$1 == n { print $3 }')
    [ -n "$targetSource" ] || { say "$target is not one of the regions"; exit 2; }
    rm -rf "$WORK_DIR/perturbed"
    cp -r "$WORK_DIR/pristine" "$WORK_DIR/perturbed" || exit 2
    write_spec "$WORK_DIR/perturbed" "$WORK_DIR/perturbed.spec"
    python3 "$PY" perturb "$WORK_DIR/perturbed.spec" "$target" \
        "$WORK_DIR/pristine/$(blob_name "$targetSource")" \
        "$WORK_DIR/perturbed/$(blob_name "$targetSource")" || exit 2
    python3 "$PY" extract "$WORK_DIR/perturbed.spec" > "$WORK_DIR/perturbed.sha" || exit 2
    if compare_lists "$WORK_DIR/pristine.sha" "$WORK_DIR/perturbed.sha" "pristine" "perturbed" \
         2> "$WORK_DIR/perturbed.err"; then
      say "NEGATIVE CONTROL DID NOT TRIP: $target's body was changed and the comparison still"
      say "reported every region as identical. This gate cannot go red for the reason it exists, so"
      say "every green it has ever printed means nothing."
      exit 2
    fi
    if ! grep -q "FIRST REGION THAT MOVED: $target" "$WORK_DIR/perturbed.err"; then
      say "NEGATIVE CONTROL TRIPPED FOR THE WRONG REASON: the comparison went red but did not name"
      say "$target as the first region that moved. It said:"
      sed 's/^/[p4a-untouched]   /' "$WORK_DIR/perturbed.err" >&2
      exit 2
    fi
    controls=$((controls + 1))
    say "negative control $controls: a perturbed $target body is reported, and named"
  done
  if [ "$controls" -ne 4 ]; then
    say "expected FOUR negative controls (BRIEF-P4A.md D-N), ran $controls"
    exit 2
  fi
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
