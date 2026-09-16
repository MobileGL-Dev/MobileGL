#!/bin/bash
# EXIT GATE E2's PICTURE CONTROL: dropping the draws on the wire must redden this retrace, and
# it must be THIS control's red.
#
# WHY THERE IS A SECOND CONTROL ON THIS LANE, and why it is the draws and not the clears.
#
# The retrace-split job already carries scripts/ci/retrace_pull_library_control.sh, which swaps a
# PULL library in and requires run_trace_case.cmake's transport-resolution assertion to catch it.
# That control proves the lane is running a SPLIT library. It does not prove that the PICTURE came
# through the wire, and it cannot: OpenRA scores ssim 1.000000 against a monolith library too, so
# the transport assertion is what reds it and the comparator is never consulted. BRIEF-P5 7 E2
# names a second, sharper statement - "patch an emitter to drop a record and the SSIM must fall
# below the threshold" - and until now nothing executed it.
#
# c1 shipped MOBILEGL_IPC_E2_DROP_CLEAR for exactly that, and the joint gate ran it and it DID NOT
# WORK: with the knob armed and its WARN in the library's own log, the OpenRA retrace under inproc
# still scored ssim=1.000000 with mismatchPixels=0 (joint-v1.md 3, "E2 clear-drop control - NOT
# VERIFIED"). That is not a broken knob. `apitrace dump` over the 31249 replayed calls counts 30
# glClear, 30 glXSwapBuffers and 788 glDrawArrays, and the final frame issues its clear at call
# 30197 and then covers the surface four times over with a terrain layer before the snapshot at
# 31249. A frame that overdraws every pixel it clears has a picture that does not depend on the
# clear. Measured here with the dropped-record count published: 29 Clear records dropped, ssim
# still 1.000000. The knob worked; the observable was invisible.
#
# So this control drops every DrawVbo record instead (MOBILEGL_IPC_E2_DROP_DRAW=1): the surface
# can then only carry the clear colour, and the golden is made of the geometry. Measured on the
# same head: 758 DrawVbo records dropped, ssim=0.000036, mismatchPixels=295296.
#
# WHAT THE CONTROL ASSERTS, and none of the three is the process exit code alone (R-16):
#   1. a NON-EMPTY selection, counted before the run - `--no-tests=error` turns an empty selection
#      into a non-zero exit, which is how ID-46 finding 8(b) got a control to congratulate itself;
#   2. the SSIM ACTUALLY FELL: the numbers are parsed out of the retrace's own summary and
#      compared, rather than "ctest was non-zero". A loader failure, a missing fixture, a timeout
#      and a Fatal{ all exit non-zero and none of them is this control's red;
#   3. the LIBRARY SAID IT DROPPED SOMETHING: its own "E2 control armed ... N records dropped on
#      the wire" line, with N > 0, from the log file the replay wrote. Without this, a knob that
#      was never read - a stale library, a variable that did not reach the process, an emit table
#      that fell through to the driver - would redden the picture for some other reason and pass.
#
# Usage:  retrace_drop_draw_control.sh <case> <backend>
#   CTEST           ctest binary                      (default: ctest)
#   CONTROL_TMPDIR  scratch dir                       (default: ${RUNNER_TEMP:-/tmp})
#   LIBRARY_LOG     the replay's library log          (default: <case>/<backend>/output/mobilegl.log)
set -u

CASE="${1:?usage: retrace_drop_draw_control.sh <case> <backend>}"
BACKEND="${2:?usage: retrace_drop_draw_control.sh <case> <backend>}"

CTEST="${CTEST:-ctest}"
CONTROL_TMPDIR="${CONTROL_TMPDIR:-${RUNNER_TEMP:-/tmp}}"
LIBRARY_LOG="${LIBRARY_LOG:-${CASE}/${BACKEND}/output/mobilegl.log}"
mkdir -p "${CONTROL_TMPDIR}"
FROZEN_LIBRARY="${FROZEN_LIBRARY:?FROZEN_LIBRARY must name the split library the replay loads}"
symbols=$(nm --defined-only "${FROZEN_LIBRARY}") || exit 1
remote_count=$(printf '%s\n' "${symbols}" | grep -ic MG_Remote || true)
echo "draw-drop control library: ${FROZEN_LIBRARY}: MG_Remote=${remote_count}"
if [ "${remote_count}" -lt 1 ]; then
  echo '::error::draw-drop control requires a split library: MG_Remote=0'
  exit 1
fi

selector="^MobileGLTraceReplay\.${CASE}\.${BACKEND}$"

# The rerun replays into the same case directory, so the good run's images are put aside and
# restored whichever way the control goes; "Upload actual image" runs `if: always()` and would
# otherwise ship the deliberately-wrong run's output under the good run's name.
GOOD_OUTPUT="${CONTROL_TMPDIR}/drop-draw-verified-output"
rm -rf "${GOOD_OUTPUT}"
if [ -d "${CASE}" ]; then cp -a "${CASE}" "${GOOD_OUTPUT}"; fi

restore_good_output() {
  if [ -d "${GOOD_OUTPUT}" ]; then
    rm -rf "${CASE}"; mv "${GOOD_OUTPUT}" "${CASE}"
    echo "restored the verified run's output over the control's"
  fi
}
trap restore_good_output EXIT
trap 'exit 130' INT TERM

matched=$("${CTEST}" -N -R "${selector}" | grep -cE '^ *Test *#[0-9]+:')
if [ "${matched}" -lt 1 ]; then
  restore_good_output
  echo "::error::the control selected ${matched} tests with -R '${selector}', so there is nothing for the dropped draws to redden. --no-tests=error would have exited non-zero on the empty selection and a control without this guard reads that as success (ID-46 finding 8b)."
  exit 1
fi

# A PREVIOUS RUN'S LINE MUST NEVER ARM THIS ONE. The library opens its log fopen(path, "w"), so
# the replay truncates it - but only if the replay gets that far, and a run that died in the
# loader would leave the baseline's log in place with a perfectly good "control armed" line in
# it. Removing it first is the same rule split_negative_controls.sh's `reset` step follows.
rm -f "${LIBRARY_LOG}"

out="${CONTROL_TMPDIR}/retrace-drop-draw-output.txt"
export MOBILEGL_TRANSPORT=inproc
export MOBILEGL_IPC_E2_DROP_DRAW=1
"${CTEST}" -V --no-tests=error --timeout 10800 -R "${selector}" > "${out}" 2>&1
control_rc=$?
unset MOBILEGL_IPC_E2_DROP_DRAW
cat "${out}"

# Read the library's evidence BEFORE the good output is restored over it.
armed_line=""
dropped=0
if [ -f "${LIBRARY_LOG}" ]; then
  armed_line=$(grep 'MGPipe: E2 control armed' "${LIBRARY_LOG}" | tail -1)
  dropped=$(printf '%s' "${armed_line}" | sed -n 's/.*armed[^,]*, \([0-9][0-9]*\) records dropped.*/\1/p')
  dropped="${dropped:-0}"
fi
cp -f "${LIBRARY_LOG}" "${CONTROL_TMPDIR}/drop-draw-library.log" 2>/dev/null

restore_good_output

if [ "${control_rc}" -eq 0 ]; then
  echo "::error::the split retrace PASSED with every DrawVbo record dropped on the wire. The golden is made of that geometry, so a green here means the picture did not come from the wire: the emit table fell through to the driver, the library under test is not the one the lane thinks it is, or MOBILEGL_IPC_E2_DROP_DRAW never reached the process. Exit gate E2 is exactly this statement and nothing weaker - MOBILEGL_IPC_E2_DROP_CLEAR is NOT a substitute (measured: 29 clears dropped, ssim still 1.000000, because OpenRA overdraws every pixel it clears)."
  exit 1
fi

# 2. THE SSIM ACTUALLY FELL. Parsed, not inferred from the exit code.
ssim_line=$(grep -o 'ssim=[0-9.]*, ssimThreshold=[0-9.]*' "${out}" | tail -1)
if [ -z "${ssim_line}" ]; then
  echo "::error::the split retrace went red (ctest exit ${control_rc}) with the draws dropped, but its output carries no 'ssim=..., ssimThreshold=...' summary at all, so the comparator never ran. A loader failure, a missing fixture, a timeout or a Fatal{ all land here and none of them is this control's red."
  exit 1
fi
ssim=${ssim_line#ssim=}; ssim=${ssim%%,*}
threshold=${ssim_line##*ssimThreshold=}
if ! awk -v a="${ssim}" -v b="${threshold}" 'BEGIN { exit !(a + 0 < b + 0) }'; then
  echo "::error::the split retrace went red (ctest exit ${control_rc}) but its ${ssim_line} is NOT below the threshold, so the picture is not what reddened it. This control's whole claim is that the golden is made of the dropped geometry."
  exit 1
fi

# 3. THE LIBRARY SAID IT DROPPED SOMETHING.
if [ -z "${armed_line}" ]; then
  echo "::error::the split retrace went red with ssim ${ssim} < ${threshold}, but ${LIBRARY_LOG} carries no 'MGPipe: E2 control armed' line, so there is no evidence the knob was ever read by the process that produced the picture. A library that is not the one under test, an emit table that fell through to the driver, or an environment that did not reach the replay all produce a wrong picture for a reason that has nothing to do with this control."
  exit 1
fi
if [ "${dropped}" -lt 1 ]; then
  echo "::error::the split retrace went red with ssim ${ssim} < ${threshold} and the knob announced itself - '${armed_line}' - but it reports ZERO records dropped. The emitter was never reached, so whatever changed the picture was not this control. This is the R-16 case the dropped-record COUNT exists for: the arming message alone proves only that the knob was read."
  exit 1
fi

echo "the dropped draws turned the split retrace red for their own reason (ctest exit ${control_rc}): ${matched} selected case(s), ssim ${ssim} < ${threshold}, and the library dropped ${dropped} record(s) on the wire"
