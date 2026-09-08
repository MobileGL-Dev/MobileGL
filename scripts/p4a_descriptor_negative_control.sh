#!/usr/bin/env bash
# G7's negative controls for P4a: drop one field from each of two descriptor conversions on purpose
# and prove the emission-consistency suites say so, NAMING the field.
#
# WHAT G6 CLAIMS. For every framebuffer configuration, texture object, sampler object and program
# the client's emitted MGPFramebufferState / MGPResourceDesc / MGPTextureParams / MGPSamplerDesc /
# MGPSamplerView / MGPProgramDesc reproduce EXACTLY the values Espryt's SyncToBackend family reads
# from the frontend today, field by field. MG_Test/Pipe/{FramebufferEmit,TextureEmit,SamplerEmit,
# ImageEmit,ProgramEmit}Test.cpp are what walk it.
#
# WHY A CONTROL IS NEEDED AT ALL. Those suites are green on a correct conversion, and they would be
# just as green on a conversion they had stopped looking at: a walk that drove no configurations, a
# comparison that stopped reading the record, an assertion someone loosened. Green says nothing
# about whether a suite can still fail. This script makes each of them fail, for the one reason it
# exists to catch, and reports a NON-ZERO ctest THAT NAMES THE DROPPED FIELD as the pass.
#
# THE TWO BREAKS, one per new conversion family, each a break the COMPILER CANNOT SEE - the struct
# still has the member, the record is still its pinned size, the PipeFields.def row still names it
# and the generated comparator still compares it. What breaks is the VALUE:
#
#   1. MGPSurface::Layered stops being copied in MG_Impl/Pipe/FramebufferEmit.h. A layered
#      attachment travels as a non-layered one, which is the shape that turns a whole-array render
#      target into slice 0 - and SupportsLayeredImageBinding's rule (DirectGLES.cpp:1992-2013,
#      D-O) forces `layer` to 0 for a non-layerable target, so the wrong answer is silently
#      plausible rather than an error. It is also the field with the least other coverage: the
#      surface's Res, Level and Layer are all still right, so nothing about the ATTACHMENT changes
#      except the one bit that says how much of the texture it is.
#   2. SamplerParameters::borderColorForm stops being copied in MG_Impl/Pipe/SamplerEmit.h. All
#      four border-colour VALUES still cross; what is lost is which of the three forms
#      (float / int / uint) they are to be read as, and D-F4 is explicit that the form crosses and
#      that all four values are compared. The failure it models is IntegerBorderColorScenario's:
#      an integer border colour read as floats is not an error anywhere, it is just the wrong
#      colour at the clamped edge of every sampled texture.
#
# Each patch is applied by REGEX rather than by an exact line, because both headers belong to OTHER
# PACKAGES (C.7: FramebufferEmit.h is package B's, SamplerEmit.h is package C's) and their spelling
# is theirs to choose: any `<something>.<field> = <expr>;` - or the designated-initializer
# `.<field> = <expr>,` - has its right-hand side replaced by a constant. If a header does not
# assign its field at all, because the emitter has not landed yet or because the conversion is
# spelled some other way, that is exit 2, "could not run", never a pass.
#
# ON THE P4a CONTRACT TREE THIS SCRIPT EXITS 2 AND SAYS SO. Both headers EXIST there - the contract
# commit creates all five emit headers with STUB emitters that return 0 payload bytes (contract-v1
# D1) - but neither assigns anything, so there is no field copy to drop. That is the honest report:
# a control that "passed" because there was nothing to break would be the worst outcome available.
#
# WHY IT IS NOT A CI LANE. It rebuilds the library up to four times. It is run by hand, and by the
# integrator at the P4a five-part gate (D.3 part 3, beside P2's g7_negative_control.sh and P3a's
# p3a_vertex_input_negative_control.sh, both of which keep running unchanged).
#
# Usage:
#   scripts/p4a_descriptor_negative_control.sh <build-dir>
#
#   <build-dir>   a configured build directory carrying the push-only unit suites (the emission
#                 cases are compiled only under MOBILEGL_PIPE_PUSH)
#
# RESTORE IS NOT ENOUGH; THE REBUILD IS PART OF THE CONTRACT. Once a header has been patched, EVERY
# way out of this script goes through repair(): restore the header, rebuild the library from it, and
# re-run the suite to prove the tree really went back. `cp` alone leaves <build-dir> holding a
# libMobileGL.so in which the field is hard-wired, `ctest` does not rebuild, and nothing in an exit
# status tells a caller to. The path that needs this most is the one that reads "NEGATIVE CONTROL
# DID NOT TRIP": an engineer reacts to it by opening the suite and re-running ctest against this
# very build directory, and every reading they take there would come from a deliberately corrupted
# library. So repair() runs from the EXIT trap as well - a mid-way failure (a patched header that
# would not compile, an interrupt) repairs too - and a repair that itself fails downgrades the
# verdict to 2. On the exits that happen BEFORE any patch (bad arguments, a missing header, a
# missing suite) the tree was never touched and the build directory still holds what the caller
# built: there is nothing to restore and nothing to rebuild, and the script says which of the two
# situations it is leaving behind.
#
# Nothing is written into the repository. Logs and the header backups live in
# <build-dir>/p4a-g7-logs/, which is inside the build tree and therefore neither committed nor
# picked up by `git status`; the paths are printed with every verdict.
#
# Exit codes: 0 BOTH controls tripped AND named their field;
#             1 a control did not answer: a suite stayed green with its field dropped, or it went
#               red without ever naming the field, so the red cannot be attributed to the drop.
#               Both are findings about the TEST, not errors in this script - and both leave the
#               tree restored AND rebuilt AND re-run;
#             2 a control could not be run at all (bad arguments; a header or a field absent on
#               this tree; no matching test; a build that was already broken; a patched header that
#               did not compile; a failed restore or a failed rebuild after one). Exit 2 wins over
#               exit 1: "could not run" is never reported as "did not answer".
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

# One row per control: <header>@<field>@<ctest regex>@<owning package>. The ctest regex is the
# SHORTEST string that selects only that suite, the way the CI filters are written.
CONTROLS="\
MobileGL/MG_Impl/Pipe/FramebufferEmit.h@Layered@FramebufferEmit\.@B (clientfb)
MobileGL/MG_Impl/Pipe/SamplerEmit.h@borderColorForm@SamplerEmit\.@C (clientsp)"

# Inside the build tree, never in the repository: a run must not leave untracked files behind, and
# .gitignore carries no rule for a p4a-*.log at the root. Falls back to a temp directory only if
# the build directory cannot be written, which would be a strange build directory.
LOG_DIR=$(cd "$BUILD_DIR" && pwd)/p4a-g7-logs
mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR=$(mktemp -d) || exit 2

say() { echo "[p4a-g7] $*" >&2; }

# --- the repair, shared by every exit path ---------------------------------------------------
# PATCHED_HEADER is the one header currently patched, empty when the tree is pristine. Only one
# control is ever in flight at a time, deliberately: two simultaneous drops would make a red
# unattributable to either.
PATCHED_HEADER=""
PATCHED_BACKUP=""
PATCHED_TEST=""
REPAIR_RC=0
repair() {
  [ -n "$PATCHED_HEADER" ] || return 0
  local header=$PATCHED_HEADER backup=$PATCHED_BACKUP test=$PATCHED_TEST
  # Cleared FIRST, so a repair that is re-entered (the explicit call, then the EXIT trap) does the
  # work once and reports the same answer twice.
  PATCHED_HEADER=""
  # From the byte-for-byte copy taken before the patch, never from git: someone running this on a
  # dirty tree must get their own tree back, not HEAD.
  if [ -f "$backup" ]; then cp -f "$backup" "$header" || { REPAIR_RC=2; return 2; }; fi
  say "restored $header; rebuilding $BUILD_DIR from it"
  if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-restored.log" 2>&1; then
    say "the tree did NOT rebuild after the restore - see $LOG_DIR/build-restored.log"
    say "THE BUILD DIRECTORY IS NOT TRUSTWORTHY: repair it before reading anything out of it."
    REPAIR_RC=2
    return 2
  fi
  if ! ctest --test-dir "$BUILD_DIR" -R "$test" --no-tests=error \
       > "$LOG_DIR/ctest-restored.log" 2>&1; then
    say "the tree did NOT go back to green after the restore - see $LOG_DIR/ctest-restored.log"
    REPAIR_RC=2
    return 2
  fi
  say "the tree is restored, rebuilt and green again"
  return 0
}

# INT and TERM as well as EXIT (p3a-g7 m3). A Ctrl-C during a rebuild would otherwise leave the
# patched header in the tree - bash runs no EXIT trap for an uncaught SIGINT - and the next thing
# that reader does is build, from a hard-wired field, with nothing saying so. The two extra traps
# repair and then re-raise with the default disposition, so the exit status still reports the
# signal. Armed for the whole run: before the first patch repair() is a no-op.
trap 'repair' EXIT
trap 'repair; trap - INT; kill -INT $$' INT
trap 'repair; trap - TERM; kill -TERM $$' TERM

# --- one control -----------------------------------------------------------------------------
# Echoes "tripped" / "did-not-trip" / "wrong-reason" / "could-not-run" on stdout; everything else
# goes to stderr. The tree is repaired before it returns, whatever the answer.
run_control() {
  local header=$1 field=$2 test=$3 owner=$4
  local tag matched
  tag=$(basename "$header" .h)-$field

  # 0. the control has to have something to break.
  if [ ! -f "$header" ]; then
    say "$header does not exist on this tree."
    say "It is P4a package $owner's file (BRIEF-P4A.md C.5/C.7): the client-side emitter that"
    say "carries the ${field} copy. Until it lands there is no field copy to drop, so this control"
    say "cannot run and MUST NOT report a pass. Re-run on a tree that carries that package."
    echo could-not-run
    return
  fi
  if ! grep -qE "\.${field}[[:space:]]*=" "$header"; then
    say "$header exists but assigns no .${field}."
    say "On the P4a CONTRACT tree that is the expected answer: the contract commit creates all five"
    say "emit headers with STUB emitters (contract-v1 D1) that return 0 payload bytes and copy"
    say "nothing, and package $owner fills the body in. Later in the phase it means something else"
    say "and worse - either the conversion moved out of this header, or it does not copy ${field}"
    say "at all, and the second one would mean G6 is already broken in exactly the way this control"
    say "is supposed to create. Neither is something this script may report as a pass; look at the"
    say "header."
    echo could-not-run
    return
  fi

  # 1. the suite has to exist, and be green, BEFORE the break. A missing test is NOT a pass:
  #    without this the script would patch, watch ctest match nothing, read that as "the test
  #    failed" and report the control as tripped.
  matched=$(ctest --test-dir "$BUILD_DIR" -N -R "$test" 2>/dev/null | grep -cE '^ *Test *#[0-9]+:')
  if [ "${matched:-0}" -eq 0 ]; then
    say "no test matches $test in $BUILD_DIR."
    say "The suite is registered by the P4a contract commit; until it carries the emission cases"
    say "this control has nothing to trip and cannot report a pass."
    echo could-not-run
    return
  fi
  say "[$tag] $matched matching test(s) before the patch"

  say "[$tag] building $BUILD_DIR as it is"
  if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-before-$tag.log" 2>&1; then
    say "[$tag] the build is already broken before any patch - see $LOG_DIR/build-before-$tag.log"
    tail -20 "$LOG_DIR/build-before-$tag.log" >&2
    echo could-not-run
    return
  fi
  if ! ctest --test-dir "$BUILD_DIR" -R "$test" --no-tests=error --output-on-failure \
       > "$LOG_DIR/ctest-before-$tag.log" 2>&1; then
    say "[$tag] $test is already red before the patch - fix that first, the control proves nothing"
    tail -30 "$LOG_DIR/ctest-before-$tag.log" >&2
    echo could-not-run
    return
  fi
  say "[$tag] $test is green before the patch"

  # 2. stop copying the field.
  PATCHED_BACKUP=$LOG_DIR/$(basename "$header").orig
  PATCHED_TEST=$test
  cp -f "$header" "$PATCHED_BACKUP" || { echo could-not-run; return; }
  # Set BEFORE the patcher runs, not after: a python that died half-way through the write must
  # still be repaired. The cost is one unnecessary rebuild in the case where the patcher matched
  # nothing and the file is byte-identical (cp refreshes its mtime).
  PATCHED_HEADER=$header
  say "[$tag] dropping the ${field} copy from $header"
  if ! python3 - "$header" "$field" <<'PY'
import re
import sys

path, field = sys.argv[1], sys.argv[2]
text = open(path, encoding='utf-8').read()
# `<lhs>.Layered = <expr>;` and the designated-initializer `.Layered = <expr>,`. The right-hand
# side is REPLACED rather than the line deleted, so the record still HAS the field and the break
# stays one the compiler cannot see.
pattern = re.compile(r'(\.' + re.escape(field) + r'\s*=\s*)([^;,\n]+)([;,])')
patched, count = pattern.subn(r'\g<1>0 /* G7 NEGATIVE CONTROL: was \g<2> */\g<3>', text)
if count == 0:
    sys.stderr.write('[p4a-g7] no assignment to .%s to patch - the header changed shape since this '
                     'control was written; update the control, do not delete it.\n' % field)
    sys.exit(1)
open(path, 'w', encoding='utf-8', newline='\n').write(patched)
print('[p4a-g7] neutralised %d assignment(s) to .%s' % (count, field))
PY
  then
    say "[$tag] the patcher did not apply; the tree is repaired on the way out"
    repair || true
    echo could-not-run
    return
  fi

  # 3. it must still COMPILE. A build break here would prove the static_asserts work, not that the
  #    suite still checks.
  say "[$tag] rebuilding with the dropped field"
  if ! cmake --build "$BUILD_DIR" -j "$(nproc)" > "$LOG_DIR/build-after-$tag.log" 2>&1; then
    say "[$tag] the patched header did not compile, so the control cannot tell 'the test failed'"
    say "from 'nothing was built'. The break is supposed to be invisible to the compiler - if the"
    say "field is read somewhere that needs its value, say so in the control rather than working"
    say "around it."
    grep -m10 -E 'error:' "$LOG_DIR/build-after-$tag.log" >&2
    repair || true
    echo could-not-run
    return
  fi
  say "[$tag] the patched header still compiles, so the record still has the field and its size"

  # 4. the suite must now be RED, and NAME the field. The verdict is only RECORDED here; nothing is
  #    reported and nothing returns until step 5 has put the tree back, because all three outcomes
  #    leave the same corrupted build directory behind.
  local verdict
  say "[$tag] running $test against the dropped field"
  if ctest --test-dir "$BUILD_DIR" -R "$test" --no-tests=error --output-on-failure \
       > "$LOG_DIR/ctest-after-$tag.log" 2>&1; then
    verdict=did-not-trip
  elif awk '/: Failure$/ || /: error:/ { block = 1 } block { print } /^[[:space:]]*$/ { block = 0 }' \
         "$LOG_DIR/ctest-after-$tag.log" | grep -q "$field"; then
    # A red is not yet a pass: a suite that had started failing for an unrelated reason satisfies
    # the first half of the claim and none of the second. Matched against the FAILING ASSERTIONS'
    # OWN BLOCKS rather than the whole ctest log (p3a-g7 m3): gtest prints a failure as
    # `<file>:<line>: Failure` followed by the compared expressions and their values, terminated by
    # a blank line, and the field name appears among those expressions - so the block, not the
    # line, is the right unit and the whole file is the wrong one. A case NAMED after the field, a
    # skip reason quoting it or a compiler note echoed into the log would all have made "tripped"
    # mean "the string exists somewhere in the output".
    verdict=tripped
  else
    verdict=wrong-reason
  fi

  # 5. put it back, and prove it went back. SHARED BY ALL THREE OUTCOMES, and that is the whole
  #    point of doing it before the verdict is reported.
  if ! repair; then
    say "[$tag] the control's own verdict was '$verdict', but the repair failed, so that verdict is"
    say "not what this run reports: a build directory that could not be put back is 'could not run'."
    echo could-not-run
    return
  fi

  case "$verdict" in
    did-not-trip)
      say "[$tag] NEGATIVE CONTROL DID NOT TRIP: $test was still green with ${field} no longer"
      say "copied into the record. The emission comparison did not notice a field it claims to"
      say "compare, so G6 is not checking what it claims to check."
      say "The run's output is kept at $LOG_DIR/ctest-after-$tag.log"
      ;;
    wrong-reason)
      say "[$tag] INCONCLUSIVE: $test went red with ${field} dropped but never named it, so the red"
      say "cannot be attributed to the dropped field. The suite failed for some other reason."
      grep -m20 -E 'Failure|error|Expected|Actual' "$LOG_DIR/ctest-after-$tag.log" >&2
      say "The run's output is kept at $LOG_DIR/ctest-after-$tag.log"
      ;;
    tripped)
      say "[$tag] negative control tripped, naming $field, and the tree is green again"
      ;;
  esac
  echo "$verdict"
}

# --- both controls, then one verdict ----------------------------------------------------------
# Both are ALWAYS run, even when the first could not run: "FramebufferEmit has no emitter yet and
# SamplerEmit does" is a different tree from "neither does", and an engineer reading this output
# needs to know which. The exit code is the worst of the two, with 2 (could not run) outranking
# 1 (did not answer).
WORST=0
SUMMARY=""
while IFS='@' read -r header field test owner; do
  [ -n "$header" ] || continue
  verdict=$(run_control "$header" "$field" "$test" "$owner")
  SUMMARY="$SUMMARY
  $field ($header): $verdict"
  case "$verdict" in
    tripped) ;;
    could-not-run) WORST=2 ;;
    *) [ "$WORST" -eq 2 ] || WORST=1 ;;
  esac
done <<EOF
$CONTROLS
EOF

trap - EXIT
say "---- G7 (P4a descriptor emission) ----$SUMMARY"
case "$WORST" in
  0) say "both controls tripped and named their field; exit 0" ;;
  1) say "a control did not answer; exit 1" ;;
  2) say "a control could not be run; exit 2 (this is the expected answer on the P4a contract tree,"
     say "where both emit headers are the contract's stubs and copy nothing)" ;;
esac
exit "$WORST"
