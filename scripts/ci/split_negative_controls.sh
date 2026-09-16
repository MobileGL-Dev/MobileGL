#!/bin/bash
# EXIT GATE E1's NEGATIVE CONTROL and EXIT GATE E3(a)'s.
#
# This file is the body of .github/workflows/test.yml's "Negative controls - the verb barrier and
# the persistent-map push must be load-bearing" step. It lives in the repository rather than inline
# in the workflow for one reason: a workflow `run:` block cannot be executed anywhere except on a
# runner, so the logic below was unreviewable and untestable until it ran in CI - and when the
# wave-1 cross-family review claimed it was broken, confirming the claim needed a hand-made copy of
# these lines with their inputs stubbed (wave1-codex-verify.md 8). A copy is not the thing. The
# smoke test at scripts/ci/control_smoke_test.sh now runs THIS file, so the lines CI executes and
# the lines the smoke test proves are the same lines.
#
# WHAT THE REVIEW FOUND (ID-46 finding 8, CONFIRMED by execution; ID-48 assigns it here).
# The previous version accepted ANY non-zero ctest exit as "the knob is load-bearing". A timeout, a
# setup abort, an unrelated assertion, a harness that died before it read the knob at all - every
# one of them printed "turned N selected entries red, as it must" and the step went green. The
# verifier demonstrated it: a stubbed ctest with a NON-EMPTY selection that failed with
# `UNRELATED_CONTROL_FAILURE` produced both controls' success messages and HARNESS_EXIT=0.
#
# So each control now has to say WHY the red is its own:
#
#   1. THE BASELINE MUST BE GREEN. The arming run below used to end in `|| true` and count every
#      case that was not <skipped/> as "ran" - so a case that RAN AND FAILED armed the controls,
#      and a control that turns an already-red entry red proves nothing at all. It now counts
#      PASSED cases, and a baseline with any failure in it is a hard error rather than an arming
#      signal.
#   2. THE RED MUST CARRY THE SELECTED CASE'S OWN FAILURE TEXT. Each control names a regex of the
#      diagnostics its scenarios emit when that knob is off, and the red is refused if the output
#      carries none of them.
#
# WHY THE EVIDENCE IS THE SCENARIO'S ASSERTION TEXT AND NOT THE KNOB'S OWN LOG LINE.
# ConfigLoader logs a named line for both knobs (ConfigLoader.cpp:385-393, "is the R-1 NEGATIVE
# CONTROL", "is the E3(a) NEGATIVE CONTROL"), and it is tempting to grep for that. It is not
# evidence: it is written at config load, by every process in the run, whatever happens next. A
# setup abort would carry it too. It proves the knob was READ, never that the knob caused the red.
# Only the failing case's own diagnostic does that. (It is also unreachable from here: the three
# DirectGLES.Split. lanes set no MOBILEGL_LOG_FILE_PATH, and the library's console sink is compiled
# out of this configuration, so no MGLOG_ output of any level reaches ctest's transcript. Measured:
# ~/w7/p5-v1-joint-isplit-barrier0.log, 18 aborted entries, zero occurrences of the string "Fatal".)
#
# Usage:  split_negative_controls.sh
#   CTEST          ctest binary                       (default: ctest)
#   CONTROL_TMPDIR scratch dir for the junit + output  (default: ${RUNNER_TEMP:-/tmp})
set -u

CTEST="${CTEST:-ctest}"
CONTROL_TMPDIR="${CONTROL_TMPDIR:-${RUNNER_TEMP:-/tmp}}"
mkdir -p "${CONTROL_TMPDIR}"

junit="${CONTROL_TMPDIR}/isplit.xml"

# ---- the baseline ---------------------------------------------------------------------------
#
# THE ARMED STATE IS DERIVED FROM BEHAVIOUR, not from a marker string in the generated ctest files.
# The first version read MGITEST_REMOTE_CLIENT_PRESENT out of *_tests.cmake, which was a
# restatement of the CMake source probe review finding M-1 falsified; the arming condition is a
# runtime fact inside each test process (MG_Config::Transport, ClientSession::Active(),
# ImplementedVerbCount(), read by Harness/SplitRuntimePeek), so the only honest way to ask it from a
# shell is to look at what the entries DID.
"${CTEST}" -L integration-split -j 4 --no-tests=error --output-junit "${junit}"
baseline_rc=$?

if [ ! -f "${junit}" ]; then
  echo "::error::the baseline run wrote no ${junit} (ctest exit ${baseline_rc}), so nothing below can tell an armed lane from a broken one"
  exit 1
fi

tally=$(python3 "$(dirname "$0")/junit_tally.py" "${junit}")
if [ -z "${tally}" ]; then
  echo "::error::could not tally ${junit}; a run whose result cannot be read is not an arming signal"
  exit 1
fi
baseline_passed=$(echo "${tally}" | cut -d' ' -f1)
baseline_failed=$(echo "${tally}" | cut -d' ' -f2)
baseline_skipped=$(echo "${tally}" | cut -d' ' -f3)

echo "split entries - passed: ${baseline_passed}, failed: ${baseline_failed}, skipped: ${baseline_skipped} (ctest exit ${baseline_rc})"

# A RED BASELINE DISARMS THE CONTROLS RATHER THAN ARMING THEM (review finding 8, second half).
# `|| true` plus a "not skipped" counter used to treat a case that ran and FAILED as evidence the
# lane was live. Turning an already-red entry red is not a measurement.
if [ "${baseline_failed}" -gt 0 ]; then
  echo "::error::${baseline_failed} DirectGLES.Split. entries are ALREADY RED with both knobs at their defaults, so neither negative control below can attribute its red to the knob it turns. Fix the lane first; a control measured against a red baseline is not a control. (This used to be swallowed by an unconditional '|| true' and counted as 'the lane is armed'.)"
  exit 1
fi

if [ "${baseline_passed}" -lt 1 ]; then
  echo "::warning::every DirectGLES.Split. entry SKIPPED, so neither negative control can fire. The arming condition is a runtime fact - MG_Config::Transport, ClientSession::Active() and ImplementedVerbCount(), read by Harness/SplitRuntimePeek - and it becomes true on the commit that lands the last of c1/s1/v1. This step becomes a gate then, with no edit; it is not a green that asserted anything today."
  exit 0
fi

# ---- the controls ---------------------------------------------------------------------------
#
# run_control <name> <filter> <evidence regex> <VAR=VALUE>...
run_control() {
  name="$1"; filter="$2"; evidence="$3"; shift 3

  matched=$("${CTEST}" -N -L integration-split -R "${filter}" | grep -cE '^ *Test *#[0-9]+:')
  if [ "${matched}" -lt 1 ]; then
    echo "::error::${name} selected ${matched} tests; its filter no longer matches anything"
    exit 1
  fi

  out="${CONTROL_TMPDIR}/control-output.txt"
  env "$@" "${CTEST}" --output-on-failure -L integration-split -R "${filter}" --no-tests=error > "${out}" 2>&1
  control_rc=$?
  cat "${out}"

  if [ "${control_rc}" -eq 0 ]; then
    echo "::error::${name} left ${matched} split entries GREEN, so the knob it turns is not load-bearing and the gate it controls proves nothing."
    exit 1
  fi

  # THE HALF THAT WAS MISSING. A non-zero exit is necessary and nowhere near sufficient.
  # Whitespace is normalised across the whole file first, for the reason given in
  # retrace_pull_library_control.sh: a diagnostic that arrives wrapped is still the diagnostic.
  if ! tr -s '[:space:]' ' ' < "${out}" | grep -qE "${evidence}"; then
    echo "::error::${name} turned ${matched} selected entries red, but the red carries NONE of the diagnostics those scenarios emit when this knob is off, so it is not this control's red. Required one of: ${evidence}. A timeout, a setup abort, a harness that died before it read the knob, or any unrelated assertion lands here - and every one of them used to print the success message below and leave this step green (ID-46 finding 8). If the entries aborted with no output at all, that is the barrier path having no named diagnostic of its own: see t1-v2.md, it is a debt on the server package, not a reason to accept the red."
    exit 1
  fi

  echo "${name} turned ${matched} selected entries red, and the red carries the scenario's own diagnostic, as it must"
}

# E1: R-1's lockstep verb barrier. Without it the client keeps pulling fields from a live GLContext
# while the server runs ahead, so the server reads future values.
#
# THE SELECTION INCLUDES THE SmallRing LANE, and that is not cosmetic. Measured on v1's joint tree
# (~/w7/p5-v1-joint-isplit-barrier0.log): with the barrier off, every entry the OLD filter selected
# aborted with no output whatsoever, and the one entry in the whole run that failed with a readable
# assertion - ClearThenReadPixelsScenario.cpp:290/:295, reading back 0 where >200 was cleared - was
# a DirectGLES.Split.SmallRing. entry, which the old filter excluded. A control whose selection
# contains no case able to say why it failed cannot assert its own failure reason. The SmallRing
# entries are the same two scenarios under the same transport with SEG_CMD/SEG_STAGE at their floor,
# so including them widens E1's selection strictly within E1's charter.
run_control "negative control E1 (MOBILEGL_IPC_VERB_BARRIER=0)" \
  'DirectGLES\.Split\.(SmallRing\.)?(Triangle|ClearThenReadPixels)' \
  'ClearThenReadPixelsScenario\.cpp:(290|295)|the bottom band should be red after the resolve|the top band should be blue after the resolve|TriangleScenario\.cpp:[0-9]+: Failure' \
  MOBILEGL_IPC_VERB_BARRIER=0

# E3(a): the persistent-map push. 0 is admitted by ConfigLoader on purpose and is documented there
# as this control. The evidence is the scenario's own wording for "the second write never arrived":
# with the push off, the write that no GL call announces cannot reach its draw, which is exactly
# what TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw and
# AWriteAfterAFrameBoundaryReachesTheNextFramesDraw read back
# (PersistentCoherentMapScenario.cpp:414-417, :442-443). The counting case's pmap= assertion
# (:531-541) is listed too, for the lane where it is the one that runs.
run_control "negative control E3(a) (MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0)" \
  'DirectGLES\.Split\.(SmallRing\.)?PersistentCoherentMapScenario' \
  "the SECOND write through the same mapping, announced by nothing|frame 1's write through the SAME mapping|cannot have pushed|PersistentCoherentMapScenario\.cpp:[0-9]+: Failure" \
  MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0
