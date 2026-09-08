#!/usr/bin/env bash
# G5's gate: "pool 与延迟释放原样搬" is LITERAL - TEN functions in
# MobileGL/MG_Backend/DirectGLES/Managers.cpp are byte-identical after P3a.
#
# WHAT G5 CLAIMS, and why a diff of the file cannot say it. ARCHITECTURE.md:316 puts the three
# persistently mapped rings and the buffer pool in the do-not-touch list, and :515 says what
# protects them: `present` and eglSwapBuffers are strictly 1:1, so the rings' and the pool's retire
# only ever happens inside Present, and a batching change would starve them. P3a rewrites the file
# those ten functions live in - Ops_* becomes handle-shaped, the twin's gate is re-keyed - so the
# file's diff is large by design and says nothing about whether the pool moved. This gate extracts
# the ten BODIES and compares them on their own.
#
# The ten, and what each one is (BRIEF-P3A.md D-F for the first nine, D-C and E's risk row for the
# tenth):
#
#   IsPoolable                     takes the server-side resource, never the frontend object
#   EnrollIntoPool                 the retireSerial = CurrentFrameSerial() + 1 stamp is load-bearing
#   AcquireFromPool                hands back only entries whose GPU work is complete
#   TrimBufferPool                 called once per frame from Present
#   ClearBufferPool                context loss
#   ProcessDeferredBufferReleases  drained per draw, fast-outs on an atomic flag
#   CreateRingStorage              glBufferStorageEXT + persistent|coherent, retires at serial + 1
#   RingAvailable                  self-heals a stale context generation
#   RingAllocate                   the fast path on the hot upload route
#   FlushPendingRangesNow          the three-tier drain: tier 1 is an INVALIDATE_BUFFER map + memcpy
#                                  for a whole-buffer flush or a range >= 128 KiB, tier 2 the upload
#                                  ring + glCopyBufferSubData, tier 3 UploadRangeNow
#
# THE TENTH IS HERE BY INTEGRATOR DECISION ID-11, resolving a contradiction inside the brief.
# D-F's "Decision: nine functions" table omits FlushPendingRangesNow, but BRIEF-P3A.md:420 calls it
# "a G5-protected function" and E's risk row at :1708 makes this gate the WHOLE mitigation for
# "the three-tier drain silently changes tier because its caller now supplies bytes differently":
# dropping the hot path from tier 1 into tier 3 costs the Mali WAR stall back, which is
# MEASUREMENTS.md:87's p99 163 -> 21 ms and ROADMAP.md:19's headline number, and under ID-6
# performance is RECORDED rather than gated, so no other gate stops it. A risk row whose named
# mitigation does not exist is worse than an unmitigated one. It is appended LAST rather than
# inserted, so the first nine lines of the D.0 baseline capture keep their positions.
#
# What "byte-identical" means for it, and it is the point of the row: the P3a handle arm must CALL
# this function, not carry a copy of the ladder. A push arm that re-spells the three tiers beside
# an untouched pull arm satisfies G1 and defeats this gate's reason to exist - two ladders drift.
#
# HOW A BODY IS EXTRACTED. The file is masked first - comments, string, char and raw-string
# literals are replaced by spaces of the same length, so a brace or a parenthesis inside one can
# never be counted - and the DEFINITION is then found as the one occurrence of `<name> (` whose
# closing parenthesis is followed (past qualifiers like const/noexcept) by `{`. That is what tells
# a definition from the forward declarations at the top of the anonymous namespace and from the
# call sites: a call's `)` is followed by `)`, `;` or `,`, never by `{`. The body is then brace
# matched in the masked text and hashed from the ORIGINAL text, so a comment change inside one of
# these functions is a difference too - which is deliberate: the claim is "byte-identical", and a
# comment that stopped describing what the code does is exactly the kind of drift a "verbatim
# move" is supposed to be checked for.
#
# Exactly one definition must be found per name. Zero or two is exit 2 (could not run), never a
# silent pass: a rename that this gate could not follow must not read as "nothing moved".
#
# Usage:
#   scripts/p3a_untouched_regions.sh <ref-a> <ref-b>   compare the ten bodies at two git refs
#   scripts/p3a_untouched_regions.sh <ref>             print the ten shas at one ref (the D.0
#                                                      baseline capture: ... > p3a-before-untouched.sha)
#   scripts/p3a_untouched_regions.sh --self-test       prove the comparison can go red
#
# stdout is always the sha list - `<sha256>  <function>`, one per line, in the fixed order above -
# so the baseline capture is a plain redirect. Everything else goes to stderr.
#
# Both arguments are GIT REFS: the gate is about what landed, so an uncommitted edit is invisible
# by design. Use HEAD after committing, which is what D.1 and the CI row do.
#
# Exit codes: 0 the ten bodies are identical at both refs (or a single ref was listed);
#             1 at least one moved - the first one in the fixed order is named on stderr;
#             2 the gate could not run: a bad ref, a missing file, a name that is not defined
#               exactly once, or a self-test whose control failed to trip.
set -u -o pipefail

SOURCE_PATH=MobileGL/MG_Backend/DirectGLES/Managers.cpp
FUNCTIONS="IsPoolable EnrollIntoPool AcquireFromPool TrimBufferPool ClearBufferPool ProcessDeferredBufferReleases CreateRingStorage RingAvailable RingAllocate FlushPendingRangesNow"
EXPECTED_FUNCTION_COUNT=10
# The functions the self-test perturbs, one control each. ClearBufferPool is small, has no forward
# declaration and no overload, so a failure to trip there is about the COMPARISON rather than about
# the extraction. FlushPendingRangesNow is the opposite shape on purpose - the longest body in the
# set, three nested tiers, its own early returns - and it is the ID-11 addition, so a control that
# only ever perturbed the easy one would leave the tenth entry unproven: an entry that silently
# extracted the wrong extent would compare equal forever.
SELF_TEST_FUNCTIONS="ClearBufferPool FlushPendingRangesNow"

say() { echo "[p3a-untouched] $*" >&2; }

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 2
cd "$REPO_ROOT" || exit 2

WORK_DIR=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK_DIR"' EXIT

# The extractor. Two modes, both over a FILE so the self-test can drive it without inventing a
# commit: `extract` prints one `<sha>  <name>` line per function, `perturb` writes a copy of the
# file with one statement inserted at the top of one function's body.
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
            # There is no digit separator in Managers.cpp today; this is here so that adding one
            # cannot quietly turn this gate into a vacuous green.
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


def find_definition(text, masked, name):
    """(begin, end) of the ONE definition of `name`, or a reason it could not be found."""
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


def extract(path, names):
    text = open(path, encoding='utf-8', newline='').read()
    masked = mask(text)
    rows, problems = [], []
    for name in names:
        hits = find_definition(text, masked, name)
        if len(hits) != 1:
            problems.append('%s: expected exactly one definition, found %d' % (name, len(hits)))
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
                problems.append('  the handle arm must CALL the untouched %s, not carry a copy of '
                                'it: two ladders drift (BRIEF-P3A.md:1708, ID-11)' % name)
            continue
        begin, end = hits[0]
        body = text[begin:end]
        rows.append((hashlib.sha256(body.encode('utf-8')).hexdigest(), name))
    return rows, problems


def perturb(src, dst, names, target):
    text = open(src, encoding='utf-8', newline='').read()
    masked = mask(text)
    hits = find_definition(text, masked, target)
    if len(hits) != 1:
        sys.stderr.write('[p3a-untouched] cannot perturb %s: %d definitions\n' % (target, len(hits)))
        return 2
    begin, end = hits[0]
    # The opening brace is located in the MASKED text and then used as an offset into the original:
    # a brace inside a comment or a string on the signature line would otherwise send the
    # perturbation somewhere that is not the body, and the control would be proving the wrong
    # thing. Offsets are identical between the two by construction (mask() preserves length).
    brace = masked.index('{', begin)
    patched = (text[:brace + 1] +
               '\n            // p3a_untouched_regions.sh --self-test: a body that MOVED.\n' +
               text[brace + 1:])
    open(dst, 'w', encoding='utf-8', newline='').write(patched)
    return 0


def main(argv):
    mode = argv[1]
    names = argv[-1].split()
    if mode == 'extract':
        rows, problems = extract(argv[2], names)
        for problem in problems:
            sys.stderr.write('[p3a-untouched] %s\n' % problem)
        for sha, name in rows:
            sys.stdout.write('%s  %s\n' % (sha, name))
        return 2 if problems else 0
    if mode == 'perturb':
        return perturb(argv[2], argv[3], names, argv[4])
    sys.stderr.write('[p3a-untouched] unknown mode %r\n' % mode)
    return 2


sys.exit(main(sys.argv))
PYTHON

# Extract the nine bodies at a git ref into "$2".
extract_ref() {
  local ref=$1 out=$2 blob="$WORK_DIR/$2.cpp"
  if ! git show "$ref:$SOURCE_PATH" > "$blob" 2>"$WORK_DIR/show.err"; then
    say "cannot read $SOURCE_PATH at '$ref':"
    sed 's/^/[p3a-untouched]   /' "$WORK_DIR/show.err" >&2
    return 2
  fi
  python3 "$PY" extract "$blob" "$FUNCTIONS" > "$WORK_DIR/$out.sha"
  return $?
}

# Compare two sha lists. Prints the first function that moved.
compare_lists() {
  local a=$1 b=$2 labelA=$3 labelB=$4 moved=0
  while read -r shaA name; do
    local shaB
    shaB=$(awk -v n="$name" '$2 == n { print $1 }' "$b")
    if [ "$shaA" != "$shaB" ]; then
      if [ "$moved" -eq 0 ]; then
        say "FIRST FUNCTION THAT MOVED: $name"
        say "  $labelA $shaA"
        say "  $labelB ${shaB:-<not found>}"
        say "  G5 (ARCHITECTURE.md:316, :515) says the buffer pool, the deferred-release drain, the"
        say "  three rings and FlushPendingRangesNow's three-tier drain (BRIEF-P3A.md:420, :1708)"
        say "  move VERBATIM. If this change is intended, it is not a P3a change and it needs its"
        say "  own commit and its own reason; if it is not, revert the body. A push arm that carries"
        say "  its own COPY of one of these bodies beside an untouched pull arm is the same finding:"
        say "  the handle arm must CALL the untouched function, not re-spell it."
      else
        say "also moved: $name"
      fi
      moved=$((moved + 1))
    fi
  done < "$a"
  return $((moved > 0 ? 1 : 0))
}

# --- self-test ------------------------------------------------------------------------------
# A gate that always says "identical" and a gate that is working produce the same green, so the
# comparison has to be shown failing. Both controls run: the POSITIVE one (an untouched copy
# compares equal) rules out a comparison that reports every function as moved, and the NEGATIVE
# one (one body perturbed) rules out the comparison that never reports any.
if [ "${1:-}" = "--self-test" ]; then
  [ $# -eq 1 ] || { say "--self-test takes no other arguments"; exit 2; }
  [ -f "$SOURCE_PATH" ] || { say "$SOURCE_PATH is not in this tree"; exit 2; }

  cp -f "$SOURCE_PATH" "$WORK_DIR/pristine.cpp" || exit 2
  if ! python3 "$PY" extract "$WORK_DIR/pristine.cpp" "$FUNCTIONS" > "$WORK_DIR/pristine.sha"; then
    say "the extractor could not read the ten bodies out of the working tree's $SOURCE_PATH"
    exit 2
  fi
  found=$(wc -l < "$WORK_DIR/pristine.sha")
  if [ "$found" -ne "$EXPECTED_FUNCTION_COUNT" ]; then
    say "extracted $found bodies, expected $EXPECTED_FUNCTION_COUNT"
    exit 2
  fi
  say "positive control: $EXPECTED_FUNCTION_COUNT bodies extracted from the working tree"

  cp -f "$WORK_DIR/pristine.cpp" "$WORK_DIR/copy.cpp"
  python3 "$PY" extract "$WORK_DIR/copy.cpp" "$FUNCTIONS" > "$WORK_DIR/copy.sha" || exit 2
  if ! compare_lists "$WORK_DIR/pristine.sha" "$WORK_DIR/copy.sha" "pristine" "copy" 2>/dev/null; then
    say "POSITIVE CONTROL FAILED: an untouched copy compared as MOVED. The comparison is reporting"
    say "differences that are not there, so its verdict means nothing in either direction."
    exit 2
  fi
  say "positive control: an untouched copy compares equal"

  # The second positive control, and it is the one that matters for P3a: the rest of this file
  # IS going to be rewritten (the Ops_* become handle-shaped, the twin's gate is re-keyed), so a
  # gate that fired on any edit to Managers.cpp would have to be switched off in the same week it
  # landed. An edit outside the ten bodies must be invisible here.
  { echo "// p3a_untouched_regions.sh --self-test: an edit OUTSIDE the ten bodies."; \
    cat "$WORK_DIR/pristine.cpp"; } > "$WORK_DIR/outside.cpp"
  python3 "$PY" extract "$WORK_DIR/outside.cpp" "$FUNCTIONS" > "$WORK_DIR/outside.sha" || exit 2
  if ! compare_lists "$WORK_DIR/pristine.sha" "$WORK_DIR/outside.sha" "pristine" "outside" \
       2>/dev/null; then
    say "POSITIVE CONTROL FAILED: an edit OUTSIDE the ten bodies was reported as one of them"
    say "moving. This gate would fire on every P3a commit to Managers.cpp and would have to be"
    say "silenced, which is the same as not having it."
    exit 2
  fi
  say "positive control: an edit outside the ten bodies is invisible"

  # One negative control per SELF_TEST_FUNCTIONS entry. Each is run on its own, from the pristine
  # copy, so the message it produces has to NAME that function - a control that only proved "some
  # body moved" would not distinguish "the tenth entry is compared" from "the tenth entry is
  # extracted as an empty range and every comparison of it is vacuous".
  for target in $SELF_TEST_FUNCTIONS; do
    python3 "$PY" perturb "$WORK_DIR/pristine.cpp" "$WORK_DIR/perturbed.cpp" \
        "$target" "$FUNCTIONS" || exit 2
    python3 "$PY" extract "$WORK_DIR/perturbed.cpp" "$FUNCTIONS" > "$WORK_DIR/perturbed.sha" || exit 2
    if compare_lists "$WORK_DIR/pristine.sha" "$WORK_DIR/perturbed.sha" "pristine" "perturbed" \
         2> "$WORK_DIR/perturbed.err"; then
      say "NEGATIVE CONTROL DID NOT TRIP: $target's body was changed and the comparison"
      say "still reported every function as identical. This gate cannot go red for the reason it"
      say "exists, so every green it has ever printed means nothing."
      exit 2
    fi
    if ! grep -q "FIRST FUNCTION THAT MOVED: $target" "$WORK_DIR/perturbed.err"; then
      say "NEGATIVE CONTROL TRIPPED FOR THE WRONG REASON: the comparison went red but did not name"
      say "$target as the first function that moved. It said:"
      sed 's/^/[p3a-untouched]   /' "$WORK_DIR/perturbed.err" >&2
      exit 2
    fi
    say "negative control: a perturbed $target body is reported, and named"
  done
  say "self-test passed"
  exit 0
fi

# --- the gate -------------------------------------------------------------------------------
case $# in
  1)
    extract_ref "$1" one || exit 2
    cat "$WORK_DIR/one.sha"
    say "listed the $EXPECTED_FUNCTION_COUNT bodies at $1"
    exit 0
    ;;
  2) ;;
  *)
    say "usage: $0 <ref-a> <ref-b> | $0 <ref> | $0 --self-test"
    exit 2
    ;;
esac

extract_ref "$1" a || exit 2
extract_ref "$2" b || exit 2
cat "$WORK_DIR/b.sha"

if compare_lists "$WORK_DIR/a.sha" "$WORK_DIR/b.sha" "$1" "$2"; then
  say "the $EXPECTED_FUNCTION_COUNT pool / deferred-release / ring / flush-drain functions are"
  say "byte-identical between $1 and $2"
  exit 0
fi
exit 1
