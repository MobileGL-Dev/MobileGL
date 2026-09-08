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
#      TWO MECHANISMS, NOT ONE, on a tree that carries the framebuffer emitter (review F-m8): the
#      regex also matches the field's copy inside the ContentHash staging helper, so the run drops
#      Layered from the emitted record AND from the framebuffer content hash. The suite still goes
#      red naming the field, which is what the control asks; the sentence above is narrowed here
#      rather than in the code because excluding the hash copy would mean hard-coding another
#      package's helper name into a regex that deliberately does not know one.
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
# THE CONSTANT IS `{}` AND NOT `0`, and the one character is the difference between a control that
# can answer and one that cannot (review F-M1). SamplerParameters::borderColorForm is a SCOPED enum
# (MG_Pipe/MGPipeValueTypes.h, `enum class BorderColorForm : Uint8`), and `x = 0` on one is
# `cannot convert 'int' to 'BorderColorForm' in assignment` - the patched header does not compile,
# the script takes its "the patched header did not compile" path, and the control reports
# could-not-run FOREVER, on every tree, with a summary line saying that exit 2 is the expected
# answer here. `x = {}` is valid for the scoped enum AND for MGPSurface::Layered (a Uint8), so ONE
# replacement covers both controls and neither of them needs to know its field's type.
#
# A CONTROL'S FIELD MUST MATTER IN AT LEAST ONE CASE. `{}` is the type's zero, so a suite whose
# every case happens to expect the zero value of the field would stay green with the copy dropped -
# which this script would then report, correctly, as "the negative control did not trip". Both
# suites' headers say they must fail BY FIELD NAME, and both drive a non-default value; that is the
# owning package's contract, and this script is what checks it rather than assuming it.
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
#                 cases are compiled only under MOBILEGL_PIPE_PUSH). It is CHECKED, not assumed:
#                 in a pull build every emission case is a visible skip, ctest is green before and
#                 after the patch, and the run would record "the negative control did not trip" -
#                 a finding about the suite that is really a finding about the build directory
#                 (review F-m10). MOBILEGL_PIPE_PUSH is read out of the directory's CMakeCache.txt.
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
# verdict to 2. That downgrade is delivered from THREE places, and REPAIR_RC is what makes the
# last two of them possible (review F-v2-m4, which found it set and never read): run_control's
# own `if ! repair` turns a failed repair into 'could-not-run' for that control; the check after
# the control loop catches a repair that failed on any earlier path and forces the run's exit
# code to 2; and the EXIT trap reads it too, so a repair that fails while the script is on its
# way out through an early `exit` cannot leave a corrupted build directory behind a 0 or a 1. The state repair() reads lives in THIS shell and not in a command substitution,
# which is what makes the trap version of it a real repair rather than a no-op, and every child's
# exit status is additionally checked for "killed by a signal" so that a Ctrl-C stops the run
# instead of carrying on into the next control and signing off with a verdict about a control
# nobody ran. On the exits that happen BEFORE any patch (bad arguments, a missing header, a
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
#             2 a control could not be run at all (bad arguments; a build directory that is not a
#               push one; a header or a field absent on this tree; no matching test; a build that
#               was already broken; a patched header that did not compile; a failed restore or a
#               failed rebuild after one; the run was INTERRUPTED). Exit 2 wins over exit 1:
#               "could not run" is never reported as "did not answer".
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

# F-m10: a PULL build directory would take both controls through "did-not-trip", which reads as a
# finding about the suites and is a finding about the argument. Every emission case is
# `#if MOBILEGL_PIPE_PUSH` and a visible skip otherwise, so ctest is green before the patch and
# green after it, and the script would report a defect in somebody else's test suite.
#
# BOTH CACHE ENTRIES ARE READ, because MOBILEGL_PIPE_VERIFY=ON forces MOBILEGL_PIPE_PUSH on for
# the configure WITHOUT writing it back to the cache (CMakeLists.txt: the `set(... ON)` shadows the
# cached OFF). A verify build directory therefore compiles the emission cases while its cache still
# says MOBILEGL_PIPE_PUSH:BOOL=OFF, and refusing it would be exactly the wrong answer.
mgl_cache_is_on() {
  local line
  line=$(grep -m1 "^$1:" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null) || return 1
  case "${line#*=}" in
    ON|On|on|1|TRUE|True|true|YES|Yes|yes|Y|y) return 0 ;;
    *) return 1 ;;
  esac
}
if ! mgl_cache_is_on MOBILEGL_PIPE_PUSH && ! mgl_cache_is_on MOBILEGL_PIPE_VERIFY; then
  echo "$BUILD_DIR does not carry the push-only emission cases:" >&2
  grep -E '^MOBILEGL_PIPE_(PUSH|VERIFY):' "$BUILD_DIR/CMakeCache.txt" >&2 || \
    echo "  (its CMakeCache.txt names neither MOBILEGL_PIPE_PUSH nor MOBILEGL_PIPE_VERIFY)" >&2
  echo "Every case in MG_Test/Pipe/{Framebuffer,Sampler}EmitTest.cpp is #if MOBILEGL_PIPE_PUSH and" >&2
  echo "a visible skip otherwise, so here ctest is green with the field dropped as well as without" >&2
  echo "it and this script would report 'the negative control did not trip' about a suite that" >&2
  echo "never ran - a finding about the build directory dressed up as a finding about the test." >&2
  echo "Point it at the push (or verify) build directory." >&2
  exit 2
fi

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
# Sticky across every repair this run performs: 0 while every repair put the tree back, 2 once
# any of them did not. Read in three places (see the header): run_control's `if ! repair`, the
# check after the control loop, and the EXIT trap. It is sticky rather than per-call because a
# build directory that was once left un-restored stays untrustworthy even if a later repair of a
# different header succeeds.
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
  if ! cmake --build "$BUILD_DIR" -j "$(nproc)" </dev/null > "$LOG_DIR/build-restored.log" 2>&1; then
    say "the tree did NOT rebuild after the restore - see $LOG_DIR/build-restored.log"
    say "THE BUILD DIRECTORY IS NOT TRUSTWORTHY: repair it before reading anything out of it."
    REPAIR_RC=2
    return 2
  fi
  if ! ctest --test-dir "$BUILD_DIR" -R "$test" --no-tests=error </dev/null \
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
#
# AND THE TRAPS ARE NOT THE ONLY DETECTION, because bash's SIGINT semantics do not guarantee that
# they run. A Ctrl-C in a terminal goes to the whole PROCESS GROUP, so the `cmake` this script is
# waiting on dies first and returns 130 - and bash, having a handler installed, may go on to the
# next command rather than to the trap. Measured exactly that way: the tree WAS repaired (through
# the explicit failure path below, which is why the state has to live in this shell), but the run
# then carried on into the second control and signed off with the summary it prints on an
# untouched contract tree. Repaired and wrong is still wrong. So every child's status is checked
# for "killed by a signal" as well, INTERRUPTED latches either way, and the run stops and says so.
INTERRUPTED=""
note_interrupt() { [ -n "$INTERRUPTED" ] || INTERRUPTED=$1; }
# 128 + signal number is how a shell reports a child that died on a signal; nothing this script
# runs exits above 128 for any other reason (ctest uses 8 for failing tests).
child_was_signalled() { [ "${1:-0}" -ge 128 ]; }
trap 'note_interrupt INT; repair; trap - INT; kill -INT $$' INT
trap 'note_interrupt TERM; repair; trap - TERM; kill -TERM $$' TERM
# The EXIT trap covers the paths that leave through an early `exit` with a header still patched
# (a patch that would not compile, a build that failed). On those the status is already chosen,
# so the trap has to OVERRIDE it when the repair did not put the tree back - which is what makes
# REPAIR_RC readable at all from here. Calling `exit` inside an EXIT trap replaces the status and
# does not re-enter the trap.
on_exit() {
  repair || true
  if [ "$REPAIR_RC" -ne 0 ]; then
    say "a repair did NOT put the tree back; exit 2 regardless of what this run was going to say."
    say "THE BUILD DIRECTORY IS NOT TRUSTWORTHY: repair it before reading anything out of it."
    exit 2
  fi
}
trap 'on_exit' EXIT

# --- one control -----------------------------------------------------------------------------
# Sets the GLOBAL CONTROL_VERDICT to "tripped" / "did-not-trip" / "wrong-reason" /
# "could-not-run"; everything it has to say goes to stderr. The tree is repaired before it
# returns, whatever the answer.
#
# A GLOBAL AND NOT AN ECHO, AND IT IS CALLED PLAINLY AND NOT IN `$(...)` (review F-M2 and F-M3,
# two separate defects that the same change closes). The earlier form was
# `verdict=$(run_control ...)`, and a command substitution is a SUBSHELL:
#
#   * F-M2 - anything the function or anything it called wrote to stdout became part of
#     `$verdict`. The patcher's own success line did, so on a tree where a control really tripped
#     the verdict was a TWO-LINE string, the `case` fell through to `*)`, and the run scored a
#     control that had answered perfectly as "a control did not answer". Exit 0 was unreachable.
#     The patcher now writes to stderr as well (belt and braces), but the verdict no longer
#     travels through a stream that anything else can write to, which is the actual fix.
#   * F-M3 - PATCHED_HEADER was assigned INSIDE that subshell. Bash resets a script's traps in a
#     command substitution, so the subshell had no traps to fire, and its assignments never
#     reached the parent, so the parent's EXIT/INT/TERM traps ran with PATCHED_HEADER empty and
#     repair() returned 0 immediately. A Ctrl-C during a rebuild left the patched header in the
#     tree - the exact thing the header's own paragraph promises cannot happen, and the p3a-g7 m3
#     lesson it cites. Called plainly, the state is the parent's and the traps repair.
CONTROL_VERDICT=""
run_control() {
  local header=$1 field=$2 test=$3 owner=$4
  local tag matched
  tag=$(basename "$header" .h)-$field
  CONTROL_VERDICT=could-not-run

  # 0. the control has to have something to break.
  if [ ! -f "$header" ]; then
    say "$header does not exist on this tree."
    say "It is P4a package $owner's file (BRIEF-P4A.md C.5/C.7): the client-side emitter that"
    say "carries the ${field} copy. Until it lands there is no field copy to drop, so this control"
    say "cannot run and MUST NOT report a pass. Re-run on a tree that carries that package."
    CONTROL_VERDICT=could-not-run
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
    CONTROL_VERDICT=could-not-run
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
    CONTROL_VERDICT=could-not-run
    return
  fi
  say "[$tag] $matched matching test(s) before the patch"

  say "[$tag] building $BUILD_DIR as it is"
  if ! cmake --build "$BUILD_DIR" -j "$(nproc)" </dev/null > "$LOG_DIR/build-before-$tag.log" 2>&1; then
    say "[$tag] the build is already broken before any patch - see $LOG_DIR/build-before-$tag.log"
    tail -20 "$LOG_DIR/build-before-$tag.log" >&2
    CONTROL_VERDICT=could-not-run
    return
  fi
  if ! ctest --test-dir "$BUILD_DIR" -R "$test" --no-tests=error --output-on-failure </dev/null \
       > "$LOG_DIR/ctest-before-$tag.log" 2>&1; then
    say "[$tag] $test is already red before the patch - fix that first, the control proves nothing"
    tail -30 "$LOG_DIR/ctest-before-$tag.log" >&2
    CONTROL_VERDICT=could-not-run
    return
  fi
  say "[$tag] $test is green before the patch"

  # 2. stop copying the field.
  PATCHED_BACKUP=$LOG_DIR/$(basename "$header").orig
  PATCHED_TEST=$test
  cp -f "$header" "$PATCHED_BACKUP" || { CONTROL_VERDICT=could-not-run; return; }
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
# `{}` and not `0`: borderColorForm is a SCOPED enum (enum class BorderColorForm : Uint8) and
# `= 0` does not convert, so the patched header would never compile and this control could
# never answer (review F-M1). `= {}` is valid for the scoped enum and for MGPSurface::Layered's
# Uint8 alike, in an assignment and in a designated initializer, so one string covers both.
patched, count = pattern.subn(r'\g<1>{} /* G7 NEGATIVE CONTROL: was \g<2> */\g<3>', text)
if count == 0:
    sys.stderr.write('[p4a-g7] no assignment to .%s to patch - the header changed shape since this '
                     'control was written; update the control, do not delete it.\n' % field)
    sys.exit(1)
open(path, 'w', encoding='utf-8', newline='\n').write(patched)
# STDERR, like every other diagnostic here: this used to be the one line in the file that
# went to stdout, and stdout was the channel the caller captured the verdict through.
sys.stderr.write('[p4a-g7] neutralised %d assignment(s) to .%s\n' % (count, field))
PY
  then
    say "[$tag] the patcher did not apply; the tree is repaired on the way out"
    repair || true
    CONTROL_VERDICT=could-not-run
    return
  fi

  # 3. it must still COMPILE. A build break here would prove the static_asserts work, not that the
  #    suite still checks.
  say "[$tag] rebuilding with the dropped field"
  cmake --build "$BUILD_DIR" -j "$(nproc)" </dev/null > "$LOG_DIR/build-after-$tag.log" 2>&1
  buildRc=$?
  if child_was_signalled "$buildRc" || [ -n "$INTERRUPTED" ]; then
    note_interrupt "exit $buildRc"
    say "[$tag] INTERRUPTED during the rebuild ($INTERRUPTED). Repairing the tree and stopping:"
    say "a run that carried on here would report a verdict about a control that never ran."
    repair || true
    CONTROL_VERDICT=could-not-run
    return
  fi
  if [ "$buildRc" -ne 0 ]; then
    say "[$tag] the patched header did not compile, so the control cannot tell 'the test failed'"
    say "from 'nothing was built'. The break is supposed to be invisible to the compiler - if the"
    say "field is read somewhere that needs its value, say so in the control rather than working"
    say "around it."
    grep -m10 -E 'error:' "$LOG_DIR/build-after-$tag.log" >&2
    repair || true
    CONTROL_VERDICT=could-not-run
    return
  fi
  say "[$tag] the patched header still compiles, so the record still has the field and its size"

  # 4. the suite must now be RED, and NAME the field. The verdict is only RECORDED here; nothing is
  #    reported and nothing returns until step 5 has put the tree back, because all three outcomes
  #    leave the same corrupted build directory behind.
  say "[$tag] running $test against the dropped field"
  ctest --test-dir "$BUILD_DIR" -R "$test" --no-tests=error --output-on-failure </dev/null \
       > "$LOG_DIR/ctest-after-$tag.log" 2>&1
  ctestRc=$?
  if child_was_signalled "$ctestRc" || [ -n "$INTERRUPTED" ]; then
    note_interrupt "exit $ctestRc"
    say "[$tag] INTERRUPTED while running $test ($INTERRUPTED). A suite that was killed is not a"
    say "suite that went red: repairing the tree and stopping rather than scoring the control."
    repair || true
    CONTROL_VERDICT=could-not-run
    return
  fi
  if [ "$ctestRc" -eq 0 ]; then
    CONTROL_VERDICT=did-not-trip
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
    CONTROL_VERDICT=tripped
  else
    CONTROL_VERDICT=wrong-reason
  fi

  # 5. put it back, and prove it went back. SHARED BY ALL THREE OUTCOMES, and that is the whole
  #    point of doing it before the verdict is reported.
  if ! repair; then
    say "[$tag] the control's own verdict was '$CONTROL_VERDICT', but the repair failed, so that"
    say "verdict is not what this run reports: a build directory that could not be put back is"
    say "'could not run'."
    CONTROL_VERDICT=could-not-run
    return
  fi

  case "$CONTROL_VERDICT" in
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
}

# --- both controls, then one verdict ----------------------------------------------------------
# Both are ALWAYS run, even when the first could not run: "FramebufferEmit has no emitter yet and
# SamplerEmit does" is a different tree from "neither does", and an engineer reading this output
# needs to know which. The exit code is the worst of the two, with 2 (could not run) outranking
# 1 (did not answer).
WORST=0
SUMMARY=""
# The rows are collected FIRST and the loop over them carries no redirection, so that
# run_control - which is called plainly now, in this shell, holding this shell's repair state -
# cannot have the remaining rows eaten out from under it by a child that reads stdin. (cmake and
# ctest are also given /dev/null explicitly below; this is the belt to that pair of braces.)
CONTROL_ROWS=()
while IFS= read -r mglRow; do
  [ -n "$mglRow" ] && CONTROL_ROWS+=("$mglRow")
done <<EOF
$CONTROLS
EOF

for mglRow in "${CONTROL_ROWS[@]}"; do
  IFS='@' read -r header field test owner <<< "$mglRow"
  [ -n "$header" ] || continue
  run_control "$header" "$field" "$test" "$owner"
  verdict=$CONTROL_VERDICT
  SUMMARY="$SUMMARY
  $field ($header): $verdict"
  case "$verdict" in
    tripped) ;;
    could-not-run) WORST=2 ;;
    *) [ "$WORST" -eq 2 ] || WORST=1 ;;
  esac
  if [ -n "$INTERRUPTED" ]; then
    SUMMARY="$SUMMARY
  (INTERRUPTED - the remaining control(s) did not run)"
    WORST=2
    break
  fi
done

# Every repair this run performed, read once. run_control already turns a failed repair of the
# control it is running into 'could-not-run', so this only ever fires for a repair that failed
# somewhere run_control does not report from - but the whole point of a sticky flag is that the
# verdict may not be better than the state of the build directory it was measured in.
if [ "$REPAIR_RC" -ne 0 ]; then
  SUMMARY="$SUMMARY
  (a repair did NOT put the tree back - the build directory is not trustworthy)"
  WORST=2
fi

trap - EXIT
say "---- G7 (P4a descriptor emission) ----$SUMMARY"
case "$WORST" in
  0) say "both controls tripped and named their field; exit 0" ;;
  1) say "a control did not answer; exit 1" ;;
  2) if [ -n "$INTERRUPTED" ]; then
       say "the run was INTERRUPTED ($INTERRUPTED); exit 2. The tree was repaired before this line -"
       say "\`git status\` is the check - and NOTHING here is a verdict about the controls."
     else
       say "a control could not be run; exit 2 (this is the expected answer on the P4a contract tree,"
       say "where both emit headers are the contract's stubs and copy nothing)"
     fi ;;
esac
# A run this shell interrupted still exits 2 rather than by signal, because the INT trap's
# re-raise only happens when bash actually reaches the trap. The summary above says which it was.
exit "$WORST"
