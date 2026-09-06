#!/usr/bin/env bash
# G7's negative control: break the pipeline/dynamic split on purpose and prove the
# setter-consistency test says so.
#
# WHAT G7 CLAIMS. MG_Pipe/MGPipeRenderStateSpans.h partitions RenderStateParameters into a
# pipeline half and a dynamic half by one rule - a byte is pipeline if and only if some public
# RenderState setter that calls BumpVersions() writes it - and
# MG_Test/Pipe/RenderStateSpansTest.cpp walks EVERY setter asserting that the pipeline-subset
# hash moves exactly when GetPipelineStateVersion() moves.
#
# WHY A CONTROL IS NEEDED AT ALL. That test is green on a correct table, and it would also be
# green on a table it had stopped looking at: a walk that silently drove no setters, a hash that
# stopped depending on the chunks, an assertion someone loosened. Green tells you nothing about
# whether the test can still fail. This script makes it fail, for the one reason it exists to
# catch, and reports a NON-zero ctest THAT NAMES THE DEMOTED MEMBER'S SETTER as the pass. A red
# for any other reason is reported as inconclusive (rc 1), not as a pass: the script knows the
# difference, so its exit status has to carry it.
#
# THE BREAK. ColorMasks is moved out of pipeline chunk P1 into a dynamic chunk of its own, by
# inserting two boundaries - at ColorMasks and at FramebufferSrgbEnabled - into the boundary
# table. That is deliberately a break the compiler CANNOT catch on its own: the chunks still
# ascend, still do not overlap and still cover [0, sizeof(RenderStateParameters)) exactly, so
# every structural static_assert in the header still holds. What breaks is the meaning:
# glColorMask bumps m_pipelineStateVersion but no longer moves the pipeline-subset hash, and
# SetterConsistency has to name SetColorMask.
#
# The four measurement pins (7 / 8 chunks, 396 / 772 bytes) are relaxed by the same patch,
# because they pin the SHIPPED table rather than the invariant - leaving them would turn this
# into a build break, which proves the assertions compile rather than that the test still checks.
#
# WHY IT IS NOT A CI LANE. It rebuilds the library twice. It is run by hand, and by the
# integrator at the P2 five-part gate.
#
# Usage:
#   scripts/g7_negative_control.sh <build-dir> [--verify-patch-only]
#
#   <build-dir>            a configured build directory carrying the push-only unit tests
#                          (MGPipeRenderStateSpans.cpp is compiled only under MOBILEGL_PIPE_PUSH,
#                          so a pull build has neither the table nor the test)
#   --verify-patch-only    apply the patch, rebuild, report whether it still compiles, and
#                          revert - WITHOUT requiring the test to exist. This is the mechanism
#                          check, not the control; it never reports the control as passed.
#
# Exit codes: 0 the control tripped AND named SetColorMask (or, under --verify-patch-only, the
#               patch compiled);
#             1 the control did not answer: either the test stayed green on a demoted member, or
#               it went red without ever naming SetColorMask, so the red cannot be attributed to
#               the demotion. Both are findings about the test, not errors in this script - and
#               both leave the tree restored and rebuilt;
#             2 the script could not run the control at all (bad arguments, missing test,
#               a build that was already broken, a failed restore).
set -u -o pipefail

BUILD_DIR=""
PATCH_ONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --verify-patch-only) PATCH_ONLY=1; shift ;;
    -*) echo "unknown arg: $1" >&2; exit 2 ;;
    *) BUILD_DIR=$1; shift ;;
  esac
done
[ -n "$BUILD_DIR" ] || { echo "usage: $0 <build-dir> [--verify-patch-only]" >&2; exit 2; }

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT" || exit 2
[ -f "$BUILD_DIR/CMakeCache.txt" ] || { echo "$BUILD_DIR is not a configured build directory" >&2; exit 2; }

HEADER=MobileGL/MG_Pipe/MGPipeRenderStateSpans.h
SOURCE=MobileGL/MG_Pipe/MGPipeRenderStateSpans.cpp
TEST_NAME='RenderStateSpans\.SetterConsistency'
LOG_DIR=$(mktemp -d)
BACKUP_DIR="$LOG_DIR/orig"
mkdir -p "$BACKUP_DIR"

say() { echo "[g7] $*" >&2; }

restore() {
  # Restore from the byte-for-byte copies taken before the patch, never from git: a developer
  # running this on a dirty tree must get their tree back, not HEAD.
  if [ -f "$BACKUP_DIR/header" ]; then cp -f "$BACKUP_DIR/header" "$HEADER"; fi
  if [ -f "$BACKUP_DIR/source" ]; then cp -f "$BACKUP_DIR/source" "$SOURCE"; fi
}
trap 'restore' EXIT

cp -f "$HEADER" "$BACKUP_DIR/header" || exit 2
cp -f "$SOURCE" "$BACKUP_DIR/source" || exit 2

# --- 0. the control has to have something to control -----------------------------------------
# A missing test is NOT a pass. Without this the script would patch, watch ctest match no tests,
# read that as "the test failed" and report the control as tripped - a green that means the
# opposite of what it says.
if [ "$PATCH_ONLY" = 0 ]; then
  matched=$(ctest --test-dir "$BUILD_DIR" -N -R "$TEST_NAME" 2>/dev/null | grep -cE '^ *Test *#[0-9]+:')
  if [ "${matched:-0}" -eq 0 ]; then
    say "no test matches $TEST_NAME in $BUILD_DIR."
    say "That test is P2 package A's (MG_Test/Pipe/RenderStateSpansTest.cpp, commit c2 on p2/spans);"
    say "until it exists this control has nothing to trip and cannot report a pass. Re-run against a"
    say "tree that carries it, or use --verify-patch-only to exercise the patch mechanism alone."
    exit 2
  fi
  say "$matched matching test(s) before the patch"
fi

# --- 1. the tree must be green BEFORE the break ----------------------------------------------
# Otherwise a red after the patch says nothing: it could have been red already.
say "building $BUILD_DIR as it is"
if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-before.log" 2>&1; then
  say "the build is already broken before any patch - see $LOG_DIR/build-before.log"
  tail -20 "$LOG_DIR/build-before.log" >&2
  exit 2
fi
if [ "$PATCH_ONLY" = 0 ]; then
  if ! ctest --test-dir "$BUILD_DIR" -R "$TEST_NAME" --no-tests=error --output-on-failure \
       > "$LOG_DIR/ctest-before.log" 2>&1; then
    say "$TEST_NAME is already red before the patch - fix that first, the control proves nothing here"
    tail -30 "$LOG_DIR/ctest-before.log" >&2
    exit 2
  fi
  say "$TEST_NAME is green before the patch"
fi

# --- 2. demote ColorMasks --------------------------------------------------------------------
say "demoting ColorMasks out of the pipeline half"
python3 - "$HEADER" "$SOURCE" <<'PY' || exit 2
import sys

header_path, source_path = sys.argv[1], sys.argv[2]


def patch(path, pairs):
    text = open(path, encoding='utf-8').read()
    for old, new in pairs:
        if text.count(old) != 1:
            sys.stderr.write("[g7] cannot patch %s: %d matches for %r\n"
                             % (path, text.count(old), old[:70]))
            sys.stderr.write("[g7] the chunk table has been rewritten since this control was "
                             "written; update the control, do not delete it.\n")
            sys.exit(1)
        text = text.replace(old, new)
    open(path, 'w', encoding='utf-8', newline='\n').write(text)


# Two extra boundaries split pipeline chunk P1 into
#   [BlendStates, ColorMasks)          pipeline (odd index, unchanged parity)
#   [ColorMasks, FramebufferSrgb)      DYNAMIC  - the demotion
#   [FramebufferSrgb, ClearColor)      pipeline
# Adding exactly TWO boundaries keeps every later chunk's index parity, so the alternating
# pipeline/dynamic rule still assigns every other chunk the half it had.
patch(header_path, [
    ("inline constexpr SizeT kMGPipeRenderStateChunkCount = 15;",
     "inline constexpr SizeT kMGPipeRenderStateChunkCount = 17; // G7 NEGATIVE CONTROL"),
    ("""        offsetof(RenderStateParameters, BlendStates),""",
     """        offsetof(RenderStateParameters, BlendStates),
        // G7 NEGATIVE CONTROL: ColorMasks demoted to a dynamic chunk of its own.
        offsetof(RenderStateParameters, ColorMasks),
        offsetof(RenderStateParameters, FramebufferSrgbEnabled),"""),
    ("static_assert(kMGPipePipelineChunkCount == 7);",
     "static_assert(kMGPipePipelineChunkCount == 8); // G7 NEGATIVE CONTROL"),
    ("static_assert(kMGPipeDynamicChunkCount == 8);",
     "static_assert(kMGPipeDynamicChunkCount == 9); // G7 NEGATIVE CONTROL"),
    ('static_assert(kMGPipePipelineChunkBytes == 396, "the pipeline subset is 396 bytes");',
     'static_assert(kMGPipePipelineChunkBytes == 364, "G7 NEGATIVE CONTROL: 396 - 32 for ColorMasks");'),
    ('static_assert(kMGPipeDynamicChunkBytes == 772, "the dynamic subset is 772 bytes");',
     'static_assert(kMGPipeDynamicChunkBytes == 804, "G7 NEGATIVE CONTROL: 772 + 32 for ColorMasks");'),
])

patch(source_path, [
    ("""        MGPipeRenderStateChunkAt(GlobalPipelineChunk(6)),
    };""",
     """        MGPipeRenderStateChunkAt(GlobalPipelineChunk(6)),
        MGPipeRenderStateChunkAt(GlobalPipelineChunk(7)), // G7 NEGATIVE CONTROL
    };"""),
    ("""        MGPipeRenderStateChunkAt(GlobalDynamicChunk(6)), MGPipeRenderStateChunkAt(GlobalDynamicChunk(7)),
    };""",
     """        MGPipeRenderStateChunkAt(GlobalDynamicChunk(6)), MGPipeRenderStateChunkAt(GlobalDynamicChunk(7)),
        MGPipeRenderStateChunkAt(GlobalDynamicChunk(8)), // G7 NEGATIVE CONTROL
    };"""),
])
print("[g7] patched the chunk table")
PY

# --- 3. it must still COMPILE ----------------------------------------------------------------
# A build break here would mean the control proved the static_asserts work, not that the test
# still checks anything.
say "rebuilding with the demoted member"
if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-after.log" 2>&1; then
  say "the patched table did not compile - the control cannot distinguish 'the test failed' from"
  say "'nothing was built'. See $LOG_DIR/build-after.log"
  grep -m10 -E 'error:' "$LOG_DIR/build-after.log" >&2
  exit 2
fi
say "the patched table still compiles, so the partition is still complete"

if [ "$PATCH_ONLY" = 1 ]; then
  # Restore AND rebuild before returning. Leaving the build directory holding a library built
  # from the deliberately-broken table would be the nastiest thing this script could do: the
  # sources would look clean, and the next `ctest` in that directory would be measuring the
  # break.
  restore
  trap - EXIT
  if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-restored.log" 2>&1; then
    say "the tree did NOT rebuild after the restore - see $LOG_DIR/build-restored.log"
    exit 2
  fi
  say "--verify-patch-only: the patch applies, compiles and reverts, and $BUILD_DIR is rebuilt from"
  say "the restored sources. This is the MECHANISM check; it does NOT report the control as passed."
  exit 0
fi

# --- 4. the test must now be RED -------------------------------------------------------------
say "running $TEST_NAME against the broken table"
if ctest --test-dir "$BUILD_DIR" -R "$TEST_NAME" --no-tests=error --output-on-failure \
     > "$LOG_DIR/ctest-after.log" 2>&1; then
  say "NEGATIVE CONTROL DID NOT TRIP: $TEST_NAME is still green with ColorMasks demoted to the"
  say "dynamic half. glColorMask bumps m_pipelineStateVersion and no longer moves the pipeline"
  say "subset hash, so the G7 invariant is violated and the test did not notice. The test is not"
  say "checking what it claims to check."
  cp -f "$LOG_DIR/ctest-after.log" ./g7-negative-control-failure.log
  say "ctest output kept at ./g7-negative-control-failure.log"
  exit 1
fi

# A red is not yet a pass. The control's claim is "demoting ColorMasks makes the setter-consistency
# test fail AND the failure names glColorMask's setter"; a SetterConsistency that had started
# failing for an unrelated reason would satisfy the first half and none of the second, and the
# caller (the integrator's D.3 reads this script's rc) would record it as "the negative control
# passed". So the answer is remembered here and decided at the end - AFTER the restore, because
# leaving a build directory holding the broken table is worse than any exit status.
TRIPPED_FOR_THE_RIGHT_REASON=1
if grep -q 'SetColorMask' "$LOG_DIR/ctest-after.log"; then
  say "negative control tripped, naming SetColorMask"
else
  TRIPPED_FOR_THE_RIGHT_REASON=0
  say "negative control tripped, but its output does not name SetColorMask - the test failed for"
  say "some other reason, so this is NOT a pass. Restoring first, then reporting it."
  grep -m20 -E 'Failure|error|Expected|Actual' "$LOG_DIR/ctest-after.log" >&2
fi

# --- 5. put it back, and prove it went back --------------------------------------------------
restore
trap - EXIT
say "restored; rebuilding"
if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-restored.log" 2>&1; then
  say "the tree did NOT rebuild after the restore - see $LOG_DIR/build-restored.log"
  exit 2
fi
if ! ctest --test-dir "$BUILD_DIR" -R "$TEST_NAME" --no-tests=error \
     > "$LOG_DIR/ctest-restored.log" 2>&1; then
  say "the tree did NOT go back to green after the restore - see $LOG_DIR/ctest-restored.log"
  exit 2
fi

if [ "$TRIPPED_FOR_THE_RIGHT_REASON" = 0 ]; then
  cp -f "$LOG_DIR/ctest-after.log" ./g7-negative-control-wrong-reason.log
  say "INCONCLUSIVE: $TEST_NAME went red under the demotion but its output never names"
  say "SetColorMask, so the red cannot be attributed to the demoted member. The tree is restored"
  say "and green again; the failing output is kept at ./g7-negative-control-wrong-reason.log."
  exit 1
fi

say "negative control tripped and the tree is green again"
exit 0
