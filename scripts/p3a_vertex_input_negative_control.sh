#!/usr/bin/env bash
# G7's negative control for P3a: drop one field from the vertex-input wire conversion on purpose
# and prove the emission-consistency suite says so, naming the field.
#
# WHAT G6 CLAIMS. For every VAO configuration the client's emitted MGPVertexElements blob +
# MGPVertexBuffers set + MGPIndexBuffer reproduce EXACTLY the values
# BackendVertexArrayObject::SyncToBackend reads from the frontend today, field by field, for all 32
# attribute slots. MG_Test/Pipe/VertexInputEmitTest.cpp (suite `VertexInputEmit`) is what walks it.
#
# WHY A CONTROL IS NEEDED AT ALL. That suite is green on a correct conversion, and it would be just
# as green on a conversion it had stopped looking at: a walk that drove no configurations, a
# comparison that stopped reading the blob, an assertion someone loosened. Green says nothing about
# whether the suite can still fail. This script makes it fail, for the one reason it exists to
# catch, and reports a NON-ZERO ctest THAT NAMES THE DROPPED FIELD as the pass.
#
# THE BREAK. `IsBgra` stops being copied into MGPVertexAttribWire. It is deliberately a break the
# COMPILER CANNOT SEE: the struct still has the member, MGPVertexAttribWire is still 24 bytes, its
# four static_asserts in MGPipeValueTypes.h's trip-wire block still hold, the field list in
# PipeFields.def still names it and the generated comparator still compares it. What breaks is the
# VALUE - the wire record now says "not BGRA" for a GL_BGRA attribute, which is the shape that made
# a swizzled colour array read back with its channels in driver order. It is also the field with
# the least other coverage: `Size` keeps 4 for GL_BGRA (D-G2), so a dropped IsBgra does not even
# change the attribute's size.
#
# The patch is applied by REGEX rather than by an exact line, because MG_Impl/Pipe/VertexInputEmit.h
# is package B's file and its spelling is B's to choose: any `<something>.IsBgra = <expr>;` (or the
# designated-initializer `.IsBgra = <expr>,`) has its right-hand side replaced by 0. If the header
# does not assign the field at all - because it has not landed yet, or because the conversion is
# spelled some other way - that is exit 2, "could not run", never a pass.
#
# WHY IT IS NOT A CI LANE. It rebuilds the library twice. It is run by hand, and by the integrator
# at the P3a five-part gate (D.3, beside P2's g7_negative_control.sh).
#
# Usage:
#   scripts/p3a_vertex_input_negative_control.sh <build-dir>
#
#   <build-dir>   a configured build directory carrying the push-only unit suites
#                 (VertexInputEmit's emission cases are compiled only under MOBILEGL_PIPE_PUSH)
#
# RESTORE IS NOT ENOUGH; THE REBUILD IS PART OF THE CONTRACT. Once the header has been patched,
# EVERY way out of this script goes through repair(): restore the header, rebuild the library from
# it, and re-run the suite to prove the tree really went back. `cp` alone leaves <build-dir> holding
# a libMobileGL.so in which IsBgra is hard-zeroed, `ctest` does not rebuild, and nothing in an exit
# status tells a caller to. The path that needs this most is the one that reads "NEGATIVE CONTROL
# DID NOT TRIP": an engineer reacts to it by opening the suite and re-running ctest against this
# very build directory, and every reading they take there would come from a deliberately corrupted
# library. Leaving a build directory holding the broken header is worse than any exit status, so
# repair() runs from the EXIT trap as well - a mid-way failure (the patched header would not
# compile, an interrupt) repairs too - and a repair that itself fails downgrades the verdict to 2.
#
# Nothing is written into the repository. Logs and the header backup live in
# <build-dir>/p3a-g7-logs/, which is inside the build tree and therefore neither committed nor
# picked up by `git status`; the paths are printed with every verdict.
#
# Exit codes: 0 the control tripped AND named IsBgra;
#             1 the control did not answer: either the suite stayed green with the field dropped,
#               or it went red without ever naming IsBgra, so the red cannot be attributed to the
#               dropped field. Both are findings about the TEST, not errors in this script - and
#               both leave the tree restored AND rebuilt AND re-run;
#             2 the script could not run the control at all (bad arguments; the header or the
#               field is absent on this tree; no matching test; a build that was already broken;
#               the patched header did not compile; a failed restore or a failed rebuild after
#               one).
set -u -o pipefail

BUILD_DIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    -*) echo "unknown arg: $1" >&2; exit 2 ;;
    *) BUILD_DIR=$1; shift ;;
  esac
done
[ -n "$BUILD_DIR" ] || { echo "usage: $0 <build-dir>" >&2; exit 2; }

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 2
cd "$REPO_ROOT" || exit 2
[ -f "$BUILD_DIR/CMakeCache.txt" ] || { echo "$BUILD_DIR is not a configured build directory" >&2; exit 2; }

HEADER=MobileGL/MG_Impl/Pipe/VertexInputEmit.h
FIELD=IsBgra
TEST_NAME='VertexInputEmit\.'
# Inside the build tree, never in the repository: a run must not leave untracked files behind, and
# .gitignore carries no rule for a p3a-*.log at the root. Falls back to a temp directory only if
# the build directory cannot be written, which would be a strange build directory.
LOG_DIR=$(cd "$BUILD_DIR" && pwd)/p3a-g7-logs
mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR=$(mktemp -d) || exit 2
BACKUP=$LOG_DIR/VertexInputEmit.h.orig

say() { echo "[p3a-g7] $*" >&2; }

restore_header() {
  # From the byte-for-byte copy taken before the patch, never from git: someone running this on a
  # dirty tree must get their own tree back, not HEAD.
  if [ -f "$BACKUP" ]; then cp -f "$BACKUP" "$HEADER"; fi
}

# Restore the header AND rebuild AND re-run the suite. Idempotent, and a no-op until the patch has
# actually been applied. Called explicitly before the verdict is decided, and from the EXIT trap
# for every path that does not reach it.
PATCH_APPLIED=0
REPAIR_DONE=0
REPAIR_RC=0
repair() {
  [ "$PATCH_APPLIED" = 1 ] || return 0
  if [ "$REPAIR_DONE" = 1 ]; then return "$REPAIR_RC"; fi
  REPAIR_DONE=1
  restore_header
  say "restored $HEADER; rebuilding $BUILD_DIR from it"
  if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-restored.log" 2>&1; then
    say "the tree did NOT rebuild after the restore - see $LOG_DIR/build-restored.log"
    say "THE BUILD DIRECTORY IS NOT TRUSTWORTHY: repair it before reading anything out of it."
    REPAIR_RC=2
    return 2
  fi
  if ! ctest --test-dir "$BUILD_DIR" -R "$TEST_NAME" --no-tests=error \
       > "$LOG_DIR/ctest-restored.log" 2>&1; then
    say "the tree did NOT go back to green after the restore - see $LOG_DIR/ctest-restored.log"
    REPAIR_RC=2
    return 2
  fi
  say "the tree is restored, rebuilt and green again"
  return 0
}

# --- 0. the control has to have something to break ------------------------------------------
# On the P3a contract tree this is where the script stops: package B owns VertexInputEmit.h and it
# does not exist yet. Saying so is the honest report - a control that "passed" because there was
# nothing to break would be the worst outcome available here.
if [ ! -f "$HEADER" ]; then
  say "$HEADER does not exist on this tree."
  say "It is P3a package B's file (BRIEF-P3A.md C.5): the client-side vertex-input emitter, which"
  say "is where the MGPVertexAttribWire conversion lives. Until it lands there is no field copy to"
  say "drop, so this control cannot run and MUST NOT report a pass. Re-run on a tree that carries"
  say "package B."
  exit 2
fi
if ! grep -qE "\.${FIELD}[[:space:]]*=" "$HEADER"; then
  say "$HEADER exists but assigns no .${FIELD}."
  say "Either the wire conversion moved out of this header, or it does not copy ${FIELD} at all -"
  say "and the second one would mean G6 is already broken in the way this control is supposed to"
  say "create. Neither is something this script may report as a pass; look at the header."
  exit 2
fi

# --- 1. the suite has to exist, and be green, BEFORE the break -------------------------------
# A missing test is NOT a pass: without this the script would patch, watch ctest match nothing,
# read that as "the test failed" and report the control as tripped.
matched=$(ctest --test-dir "$BUILD_DIR" -N -R "$TEST_NAME" 2>/dev/null | grep -cE '^ *Test *#[0-9]+:')
if [ "${matched:-0}" -eq 0 ]; then
  say "no test matches $TEST_NAME in $BUILD_DIR."
  say "The suite is MG_Test/Pipe/VertexInputEmitTest.cpp (suite name VertexInputEmit, registered by"
  say "the P3a contract commit). Until it carries the emission cases this control has nothing to"
  say "trip and cannot report a pass."
  exit 2
fi
say "$matched matching test(s) before the patch"

say "building $BUILD_DIR as it is"
if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-before.log" 2>&1; then
  say "the build is already broken before any patch - see $LOG_DIR/build-before.log"
  tail -20 "$LOG_DIR/build-before.log" >&2
  exit 2
fi
if ! ctest --test-dir "$BUILD_DIR" -R "$TEST_NAME" --no-tests=error --output-on-failure \
     > "$LOG_DIR/ctest-before.log" 2>&1; then
  say "$TEST_NAME is already red before the patch - fix that first, the control proves nothing here"
  tail -30 "$LOG_DIR/ctest-before.log" >&2
  exit 2
fi
say "$TEST_NAME is green before the patch"

# --- 2. stop copying IsBgra ------------------------------------------------------------------
cp -f "$HEADER" "$BACKUP" || exit 2
trap 'repair' EXIT
# Armed BEFORE the patcher runs, not after: a python that died half-way through the write must
# still be repaired. The cost of arming it early is one unnecessary rebuild in the case where the
# patcher matched nothing and the file is byte-identical (cp refreshes its mtime).
PATCH_APPLIED=1
say "dropping the ${FIELD} copy from $HEADER"
python3 - "$HEADER" "$FIELD" <<'PY' || exit 2
import re
import sys

path, field = sys.argv[1], sys.argv[2]
text = open(path, encoding='utf-8').read()
# `<lhs>.IsBgra = <expr>;` and the designated-initializer `.IsBgra = <expr>,`. The right-hand side
# is replaced rather than the line deleted, so the record still HAS the field and the break stays
# one the compiler cannot see.
pattern = re.compile(r'(\.' + re.escape(field) + r'\s*=\s*)([^;,\n]+)([;,])')
patched, count = pattern.subn(r'\g<1>0 /* G7 NEGATIVE CONTROL: was \g<2> */\g<3>', text)
if count == 0:
    sys.stderr.write('[p3a-g7] no assignment to .%s to patch - the header changed shape since this '
                     'control was written; update the control, do not delete it.\n' % field)
    sys.exit(1)
open(path, 'w', encoding='utf-8', newline='\n').write(patched)
print('[p3a-g7] neutralised %d assignment(s) to .%s' % (count, field))
PY

# --- 3. it must still COMPILE ----------------------------------------------------------------
# A build break here would prove the static_asserts work, not that the suite still checks.
say "rebuilding with the dropped field"
if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-after.log" 2>&1; then
  say "the patched header did not compile, so the control cannot tell 'the test failed' from"
  say "'nothing was built'. The break is supposed to be invisible to the compiler - if the field is"
  say "read somewhere that needs its value, say so in the control rather than working around it."
  grep -m10 -E 'error:' "$LOG_DIR/build-after.log" >&2
  # The EXIT trap repairs the tree on the way out; this path is a 'could not run', not a verdict.
  exit 2
fi
say "the patched header still compiles, so the record still has the field and still asserts its size"

# --- 4. the suite must now be RED, and name the field ----------------------------------------
# The verdict is only RECORDED here. Nothing is reported and nothing exits until step 5 has put the
# tree back, because all three outcomes below leave the same corrupted build directory behind.
say "running $TEST_NAME against the dropped field"
if ctest --test-dir "$BUILD_DIR" -R "$TEST_NAME" --no-tests=error --output-on-failure \
     > "$LOG_DIR/ctest-after.log" 2>&1; then
  VERDICT=did-not-trip
elif grep -q "$FIELD" "$LOG_DIR/ctest-after.log"; then
  # A red is not yet a pass: a suite that had started failing for an unrelated reason satisfies the
  # first half of the claim and none of the second.
  VERDICT=tripped
else
  VERDICT=wrong-reason
fi

# --- 5. put it back, and prove it went back --------------------------------------------------
# SHARED BY ALL THREE OUTCOMES, and that is the whole point of doing it before the verdict is
# printed. `restore` on its own only refreshes the header's mtime; without the rebuild the caller
# is handed a libMobileGL.so built from the hard-zeroed field, and the "did not trip" reader is
# precisely the caller who goes on to run more ctest against it.
repair
repair_rc=$?
trap - EXIT
if [ "$repair_rc" -ne 0 ]; then
  say "the control's own verdict was '$VERDICT', but the repair failed, so that verdict is not"
  say "what this run reports: a build directory that could not be put back is 'could not run'."
  exit 2
fi

case "$VERDICT" in
  did-not-trip)
    say "NEGATIVE CONTROL DID NOT TRIP: $TEST_NAME was still green with ${FIELD} no longer copied"
    say "into MGPVertexAttribWire. A GL_BGRA attribute travelled as a non-BGRA one and the emission"
    say "comparison did not notice, so G6 is not checking what it claims to check."
    say "The failing run's output is kept at $LOG_DIR/ctest-after.log"
    exit 1
    ;;
  wrong-reason)
    say "INCONCLUSIVE: $TEST_NAME went red with ${FIELD} dropped but never named it, so the red"
    say "cannot be attributed to the dropped field. The suite failed for some other reason."
    grep -m20 -E 'Failure|error|Expected|Actual' "$LOG_DIR/ctest-after.log" >&2
    say "The failing run's output is kept at $LOG_DIR/ctest-after.log"
    exit 1
    ;;
  tripped)
    say "negative control tripped, naming $FIELD, and the tree is green again"
    exit 0
    ;;
esac
say "unreachable verdict '$VERDICT'"
exit 2
