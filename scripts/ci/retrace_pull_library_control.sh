#!/bin/bash
# THE RETRACE-SPLIT LANE'S NEGATIVE CONTROL: a PULL library must red this split retrace.
#
# This file is the body of .github/workflows/test.yml's "Negative control - the PULL library must
# red this split retrace" step, extracted for the reason given at the top of
# scripts/ci/split_negative_controls.sh: a `run:` block is unreviewable and untestable off a
# runner, and scripts/ci/control_smoke_test.sh now runs THIS file rather than a hand-made copy.
#
# WHAT THE CONTROL IS FOR. The retrace-split job replays a trace against the SPLIT runtime under
# MOBILEGL_TRANSPORT=inproc. OpenRA scores ssim 1.000000 against a MONOLITH library too - measured -
# so the picture is not and cannot be this lane's gate. What stands between the job and a green that
# ran monolith end to end is run_trace_case.cmake's transport-resolution assertion
# (run_trace_case.cmake:265-289): the library must have logged
# "MOBILEGL_TRANSPORT=inproc - the MGPipe record stream", which exists only in
# ConfigLoader::InitTransport's InProcess arm, which exists only under MOBILEGL_BUILD_DISAGGREGATED.
# This control swaps the pull library over the frozen path and requires the same replay to fail FOR
# THAT REASON.
#
# WHAT THE REVIEW FOUND (ID-46 finding 8 part (b), CONFIRMED by execution against the REAL ctest in
# a REAL build tree; ID-48 assigns it here). Two holes, both of which let the control pass while
# asserting nothing:
#
#   1. NO SELECTION GUARD AT ALL - unlike the split lane's run_control, which has had one since
#      review finding M-4. With a case/backend regex matching no tests, `--no-tests=error` exits 8,
#      and the old `if [ "${control_rc}" -eq 0 ]` accepted 8 as "the pull library turned it red".
#      The verifier measured exactly that: "real ctest exit for a regex matching NO tests: 8",
#      HARNESS_EXIT=0. An empty selection was the one thing this control could not tell apart from
#      a working transport-identity assertion.
#   2. ONLY "non-zero ctest" WAS CHECKED after the nm identity check. The nm check establishes that
#      the library IS a pull build; it says nothing about why the replay failed. A loader failure, a
#      missing fixture or a timeout all passed the control.
#
# Both are closed below: the selection is counted before the run, and the red must carry
# run_trace_case.cmake's own words.
#
# Usage:  retrace_pull_library_control.sh <case> <backend>
#   CTEST           ctest binary                      (default: ctest)
#   CONTROL_TMPDIR  scratch dir                       (default: ${RUNNER_TEMP:-/tmp})
#   PULL_LIBRARY    the pull libMobileGL.so to swap in
#   FROZEN_LIBRARY  the path every case has baked in, which PULL_LIBRARY is copied over
set -u

CASE="${1:?usage: retrace_pull_library_control.sh <case> <backend>}"
BACKEND="${2:?usage: retrace_pull_library_control.sh <case> <backend>}"

CTEST="${CTEST:-ctest}"
CONTROL_TMPDIR="${CONTROL_TMPDIR:-${RUNNER_TEMP:-/tmp}}"
PULL_LIBRARY="${PULL_LIBRARY:?PULL_LIBRARY must name the pull build libMobileGL.so}"
FROZEN_LIBRARY="${FROZEN_LIBRARY:?FROZEN_LIBRARY must name the path the cases have baked in}"
mkdir -p "${CONTROL_TMPDIR}"

# run_trace_case.cmake's own sentences for "this library never resolved the transport", one per
# line (grep -F takes its patterns newline-separated). Anchored on the distinctive clause rather
# than on the whole paragraph, which carries substituted paths.
#
# TWO SENTENCES SINCE P6's LOG RENAME, and the second is the one a pull library actually gets.
# A pull build has one log role and writes MOBILEGL_LOG_FILE_PATH unchanged - output/mobilegl.log
# (MG_Util/Debug/Log.h, the pull arm of RoleLogPath) - while the disaggregated build's readers,
# run_trace_case.cmake among them, read output/mobilegl.client.log. So the pull library never
# reaches the marker search that says "never reported resolving it": the runner stops one check
# earlier, at "the run wrote no <client log>, so there is no evidence the transport ever
# resolved". Both are the transport-identity assertion refusing the same library for the same
# reason (a pull build has no split log for the marker to be in); neither is a loader failure, a
# missing fixture, a timeout or an SSIM drop, which are the reds hole 2 below exists to refuse.
# MEASURED on the B3 package tree with the real ctest and the real pull library: with only the
# first sentence here, the control exited 1 through hole 2 on every pull-library run - the step
# had been red for no product reason since the rename. scripts/ci/testdata/stub_ctest.sh's
# retrace-evidence-nolog mode reproduces the shape.
EVIDENCE='never reported resolving it
no evidence the transport ever resolved'
# EVERY LINE OF EVIDENCE IS A PATTERN, SO AN EMPTY LINE IS A PATTERN THAT MATCHES EVERYTHING. A
# trailing newline, or a blank line between the two sentences, would turn hole 2's check below
# into "any red will do". The smoke test's "red without the transport-resolution message" case
# would catch that (22/23), but only once somebody ran it; this refuses it here, before the
# selection is counted or the library is swapped.
case "${EVIDENCE}" in
  *$'\n\n'*|*$'\n'|$'\n'*)
    echo "::error::EVIDENCE carries an empty grep -F pattern (a blank line or a leading/trailing newline), which would match every failure"
    exit 1;;
esac

selector="^MobileGLTraceReplay\.${CASE}\.${BACKEND}$"

# The rerun replays into the same case directory, so the good run's images are put aside and
# restored whichever way the control goes; "Upload actual image" runs `if: always()` and would
# otherwise ship the deliberately-wrong run's output under the good run's name.
GOOD_OUTPUT="${CONTROL_TMPDIR}/split-verified-output"
rm -rf "${GOOD_OUTPUT}"
if [ -d "${CASE}" ]; then cp -a "${CASE}" "${GOOD_OUTPUT}"; fi

restore_good_output() {
  if [ -d "${GOOD_OUTPUT}" ]; then
    rm -rf "${CASE}"; mv "${GOOD_OUTPUT}" "${CASE}"
    echo "restored the verified run's output over the control's"
  fi
}

# Restore the exact split library on success, failure, and interruption. The following
# draw-drop control uses this same frozen path.
saved_library=$(mktemp "${CONTROL_TMPDIR}/split-library.XXXXXX") || exit 1
cp -p "${FROZEN_LIBRARY}" "${saved_library}" || exit 1
restore_control() {
  cp -p "${saved_library}" "${FROZEN_LIBRARY}"
  rm -f "${saved_library}"
  restore_good_output
}
trap restore_control EXIT
trap 'exit 130' INT TERM

# HOLE 1: COUNT THE SELECTION FIRST. `--no-tests=error` turns an empty selection into a non-zero
# exit, which is indistinguishable from a working control unless the selection is counted.
matched=$("${CTEST}" -N -R "${selector}" | grep -cE '^ *Test *#[0-9]+:')
if [ "${matched}" -lt 1 ]; then
  restore_good_output
  echo "::error::the control selected ${matched} tests with -R '${selector}', so there is nothing for the pull library to red. --no-tests=error would have exited non-zero on the empty selection and this control used to read that as success (ID-46 finding 8b, measured: ctest exit 8, step green)."
  exit 1
fi

# The pull library, unpacked from build-linux's artifact, over the frozen path every case has baked
# in. It defines no MG_Remote symbol, so ConfigLoader has no transport parser and
# MOBILEGL_TRANSPORT=inproc is accepted and ignored - the exact shape of "the split lane ran
# monolith".
cp "${PULL_LIBRARY}" "${FROZEN_LIBRARY}" || exit 1
symbols=$(nm --defined-only "${FROZEN_LIBRARY}") || exit 1
remote_count=$(printf '%s\n' "${symbols}" | grep -ic MG_Remote || true)
echo "pull control library: ${FROZEN_LIBRARY}: MG_Remote=${remote_count}"
if [ "${remote_count}" -ne 0 ]; then
  restore_good_output
  echo "::error::the control's own library defines MG_Remote symbols, so it is not a pull build and this control would prove nothing"
  exit 1
fi

out="${CONTROL_TMPDIR}/retrace-control-output.txt"
# THE TRANSPORT COMES FROM THE JOB, not from this file. retrace-split is a matrix over
# {backend, case, transport} as of P6, and a control that hard-coded `inproc` would have gone on
# proving something about the OTHER arm while the spawn arm ran unguarded. Default inproc so a
# caller that sets nothing behaves exactly as before.
transport="${SPLIT_TRANSPORT:-inproc}"
export MOBILEGL_TRANSPORT="${transport}"
"${CTEST}" -V --no-tests=error --timeout 10800 -R "${selector}" > "${out}" 2>&1
control_rc=$?
cat "${out}"

restore_good_output

if [ "${control_rc}" -eq 0 ]; then
  echo "::error::a PULL library passed the split retrace. OpenRA scores ssim 1.000000 under a monolith library too (measured), so the picture is not and cannot be this lane's gate - run_trace_case.cmake's transport-resolution assertion is, and it has stopped working. Every green in this job is then a monolith run under a name that says split."
  exit 1
fi

# HOLE 2: THE RED MUST BE THIS CONTROL'S RED.
#
# Whitespace is normalised across the WHOLE file before the match, newlines included, because the
# sentence is emitted by CMake's message(FATAL_ERROR ...) and CMake re-wraps that text to its own
# width: "never reported resolving it" arrives split over two lines with a two-space continuation
# indent, and a line-oriented grep for the literal finds nothing. That is not hypothetical - it is
# the shape the stub reproduces in scripts/ci/testdata/stub_ctest.sh.
if ! tr -s '[:space:]' ' ' < "${out}" | grep -qF "${EVIDENCE}"; then
  echo "::error::the split retrace went red (ctest exit ${control_rc}) with the pull library in place, but the failure never says the library did not resolve the transport - neither of run_trace_case.cmake's sentences (\"${EVIDENCE//$'\n'/\" / \"}\") is in the output. A loader failure, a missing fixture, a timeout or an SSIM drop all land here, and none of them establishes that the transport-identity assertion is what caught the pull library. Only 'non-zero ctest' used to be checked (ID-46 finding 8b)."
  exit 1
fi

echo "the pull library turned the split retrace red for its own reason (ctest exit ${control_rc}): ${matched} selected case(s) named the transport, not the picture"
