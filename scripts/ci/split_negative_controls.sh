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
# Only the failing case's own diagnostic does that. E1 now observes deterministic transport
# waits in dedicated CPU tests; E3(a) retains its scenario assertion and private library log.
# P5e ID-122: ordinary run-ahead draws need not fail with VERB_BARRIER=0, and the old
# overlap Fatal cannot fire with the default BATCH_WAITS=1. Pixel failures are not E1 evidence.
#
# Usage:  split_negative_controls.sh [--self-test]
#   CTEST          ctest binary                       (default: ctest)
#   CONTROL_TMPDIR scratch dir for the junit + output  (default: ${RUNNER_TEMP:-/tmp})
set -u

if [ "${1:-}" = "--self-test" ]; then
  bash "$(dirname "$0")/control_smoke_test.sh"
  exit $?
fi

CTEST="${CTEST:-ctest}"
CONTROL_TMPDIR="${CONTROL_TMPDIR:-${RUNNER_TEMP:-/tmp}}"
mkdir -p "${CONTROL_TMPDIR}"

junit="${CONTROL_TMPDIR}/isplit.xml"
rm -f "${junit}"
log_helper="$(dirname "$0")/../../MobileGL/MG_IntegrationTest/Harness/split_log_paths.py"

# Check ownership even while the runtime lane is disarmed and will skip.
python3 "${log_helper}" check "${CTEST}" "$PWD" || exit 1

# ---- the baseline ---------------------------------------------------------------------------
#
# THE ARMED STATE IS DERIVED FROM BEHAVIOUR, not from a marker string in the generated ctest files.
# The first version read MGITEST_REMOTE_CLIENT_PRESENT out of *_tests.cmake, which was a
# restatement of the CMake source probe review finding M-1 falsified; the arming condition is a
# runtime fact inside each test process (MG_Config::Transport, ClientSession::Active(),
# ImplementedVerbCount(), read by Harness/SplitRuntimePeek), so the only honest way to ask it from a
# shell is to look at what the entries DID.
# The metadata ownership test shares the label but cannot arm runtime controls.
"${CTEST}" -L integration-split -R '^DirectGLES\.Split\.' -j 4 --no-tests=error --output-junit "${junit}"
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
if [ "${baseline_failed}" -gt 0 ] || [ "${baseline_rc}" -ne 0 ]; then
  echo "::error::${baseline_failed} DirectGLES.Split. entries are ALREADY RED with both knobs at their defaults, so neither negative control below can attribute its red to the knob it turns. Fix the lane first; a control measured against a red baseline is not a control. (This used to be swallowed by an unconditional '|| true' and counted as 'the lane is armed'.)"
  exit 1
fi

if [ "${baseline_passed}" -lt 1 ]; then
  echo "::error::split baseline FAILED: every DirectGLES.Split. entry SKIPPED; the split implementation did not execute"
  exit 1
fi

# ---- the controls ---------------------------------------------------------------------------
#
# run_control <name> <filter> <ctest evidence regex> <private-log evidence regex|""> <VAR=VALUE>...
#
# THE FOURTH ARGUMENT IS NEW AND E3(a) IS WHY. The header above argues that the knob's own
# ConfigLoader line is not evidence, and that is still true: it is written at config load, by
# every process in the run, whatever happens next. What IS evidence is a line the knob's
# BEHAVIOUR emits at the site that changed - and until now E3(a) had none, because
# PersistentMapTracker::PushBlocksFor simply `return`ed at blockBytes == 0. The joint gate
# recorded exactly that ("There is no Fatal for block size zero ... No nonexistent private-file
# Fatal is quoted", joint-v1.md 3) and ID-65 assigned the missing line here. With it, E3(a) no
# longer rests on a pixel assertion alone: the red must carry the scenario's own diagnostic AND
# the library's own statement that the push was disabled, from the entry's private file.
# The optional 5th positional is a ctest -E EXCLUSION, applied to every ctest invocation this
# function makes. It exists because ctest -R is POSIX ERE: there is no negative lookahead, so a
# selection that must say "these, except that one" cannot say it in -R alone. E1 below is the
# case that forced it (ID-122).
run_control() {
  name="$1"; filter="$2"; evidence="$3"; private_evidence="$4"; exclude="${5:-}"; shift 5
  if [ -n "${exclude}" ]; then EX=(-E "${exclude}"); else EX=(); fi
  export SPLIT_LOG_EXCLUDE="${exclude}"

  matched=$("${CTEST}" -N -L integration-split -R "${filter}" "${EX[@]}" | grep -cE '^ *Test *#[0-9]+:')
  if [ "${matched}" -lt 1 ]; then
    echo "::error::${name} selected ${matched} tests; its filter no longer matches anything"
    exit 1
  fi

  manifest="${CONTROL_TMPDIR}/split-tests.json"
  "${CTEST}" --show-only=json-v1 > "${manifest}" || exit 1
  # Remove selected files first: a previous Fatal must never arm a new red.
  python3 "${log_helper}" reset "${manifest}" "${filter}" || exit 1

  out="${CONTROL_TMPDIR}/control-output.txt"
  result="${CONTROL_TMPDIR}/control.xml"
  rm -f "${result}"
  env "$@" "${CTEST}" --output-on-failure -L integration-split -R "${filter}" "${EX[@]}" --no-tests=error --output-junit "${result}" > "${out}" 2>&1
  control_rc=$?
  cat "${out}"

  # Inspect JUnit before exit status or private Fatal: a skipped pre-flight can carry both.
  python3 "${log_helper}" results "${manifest}" "${filter}" "${result}" 'E3(a)' || exit 1

  if [ "${control_rc}" -eq 0 ]; then
    echo "::error::${name} left ${matched} split entries GREEN, so the knob it turns is not load-bearing and the gate it controls proves nothing."
    exit 1
  fi

  if ! python3 "${log_helper}" assertion "${manifest}" "${filter}" "${result}" "${evidence}"; then
    echo "::error::${name} FAILED: red lacks its persistent-map push diagnostic. Required: ${evidence}"
    exit 1
  fi

  # ... and, where the knob has one, the LIBRARY's own line as well, out of the entry's private
  # file. Both halves are required: the scenario assertion says the pixels were wrong, and this
  # says the code path the knob turns off is the one that stopped running. A red that has only
  # the first half is consistent with any other defect in the same scenario.
  if [ -n "${private_evidence}" ]; then
    python3 "${log_helper}" evidence "${manifest}" "${filter}" "${private_evidence}" "${name}" || exit 1
  fi

  echo "${name} turned ${matched} selected entries red, and the red carries the scenario's own diagnostic, as it must"
}

# E1: the default arm must actually park for a kWaitApplied record and for a
# kWaitNone record whose server has no run-ahead cap. Removing the verb barrier
# must fail those exact return-before-apply assertions. A third positive case
# proves that a cap-authorized kWaitNone record does not wait. The peer releases
# on an observed Park or on the client's return, not on a sleep or pixel race.
# The helper demands green baseline, two named reds, and green restoration;
# skipped/missing cases, timeouts and unrelated failures never count as evidence.
python3 "$(dirname "$0")/wait_boundary_negative_control.py" \
  --ctest "${CTEST}" --out "${CONTROL_TMPDIR}/e1" || exit 1

# E3(a): PersistentMapTracker::PushBlocksFor stops at blockBytes == 0 - deliberately, because 0
# is the negative control and not "unlimited". Two independent halves are now required:
#   * the SCENARIO's own assertion in ctest's output. PersistentCoherentMapScenario.cpp:414-417 /
#     442-443 name the missing second write; a generic source-line Failure is not accepted,
#     because an unrelated assertion in the same case is not this red;
#   * the LIBRARY's own line in the entry's private file, saying the push was disabled by this
#     knob. It did not exist until ID-65 assigned it (joint-v1.md 3), which is why this control
#     used to rest on the pixels alone.
# TheMapLandsInTheArmItsLaneDeclares skips by design outside PersistentMapArm.
# Select only the pixel cases; a pre-flight skip in either remains a hard failure.
run_control "negative control E3(a) (MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0)" \
  'DirectGLES\.Split\.(SmallRing\.)?PersistentCoherentMapScenario\.(TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw|AWriteAfterAFrameBoundaryReachesTheNextFramesDraw)$' \
  "the SECOND write through the same mapping, announced by nothing|frame 1's write through the SAME mapping, after a Present" \
  'MGPipe: persistent-map push disabled - MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0' \
  '' \
  MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0
