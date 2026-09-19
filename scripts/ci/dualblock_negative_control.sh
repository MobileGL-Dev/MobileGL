#!/bin/bash
# P5f (f1)'s NEGATIVE CONTROL (P5F-WIRE-COMPLETENESS.md §6): the dual-block rehearsal must be
# the thing the dual-block lane measures.
#
# The contract is split_negative_controls.sh's, shrunk to one control because there is one
# knob:
#
#   1. THE BASELINE MUST BE GREEN. DirectGLES.Split.DualBlock.* runs with
#      MOBILEGL_IPC_ROLE_SPLIT_STATE=1 exported (the entry deliberately does NOT pin the knob in
#      its ctest ENVIRONMENT - a property would override the process environment and make the
#      red form below unreachable, see its registration block). A skipped baseline is not a
#      green one (ID-62), so skips are a hard error here, not a pass.
#   2. THE RED MUST CARRY THE SELECTED CASE'S OWN FAILURE TEXT. With the knob off the two roles
#      share one block again and DualBlockScenario.TheTwoRolesHaveDistinctBlocks must fail on
#      its own "dual-block control:" assertion - a timeout, a setup abort or an unrelated
#      failure proves nothing about the knob (ID-46 finding 8).
#
# Usage:  dualblock_negative_control.sh
#   CTEST          ctest binary                       (default: ctest)
#   CONTROL_TMPDIR scratch dir for the junit + output  (default: ${RUNNER_TEMP:-/tmp})
set -u

CTEST="${CTEST:-ctest}"
CONTROL_TMPDIR="${CONTROL_TMPDIR:-${RUNNER_TEMP:-/tmp}}"
mkdir -p "${CONTROL_TMPDIR}"

label="integration-dualblock-split"
tally_tool="$(dirname "$0")/junit_tally.py"

matched=$("${CTEST}" -N -L "${label}" | grep -cE '^ *Test *#[0-9]+:')
if [ "${matched}" -lt 1 ]; then
  echo "::error::the dual-block lane selected ${matched} tests; its label no longer matches anything"
  exit 1
fi

# ---- the baseline: knob ON, and it must be GREEN ------------------------------------------
baseline_xml="${CONTROL_TMPDIR}/dualblock-baseline.xml"
rm -f "${baseline_xml}"
MOBILEGL_IPC_ROLE_SPLIT_STATE=1 "${CTEST}" -L "${label}" --no-tests=error \
  --output-on-failure --output-junit "${baseline_xml}"
baseline_rc=$?
tally=$(python3 "${tally_tool}" "${baseline_xml}")
baseline_passed=$(echo "${tally}" | cut -d' ' -f1)
baseline_failed=$(echo "${tally}" | cut -d' ' -f2)
baseline_skipped=$(echo "${tally}" | cut -d' ' -f3)
echo "dual-block baseline (knob on) - passed: ${baseline_passed}, failed: ${baseline_failed}, skipped: ${baseline_skipped}"
if [ "${baseline_rc}" -ne 0 ] || [ "${baseline_failed}" -gt 0 ]; then
  echo "::error::the dual-block lane is ALREADY RED with the knob armed, so turning it off below proves nothing. Fix the lane first."
  exit 1
fi
if [ "${baseline_passed}" -lt 1 ] || [ "${baseline_skipped}" -gt 0 ]; then
  echo "::error::the dual-block baseline skipped or ran nothing - a control whose armed arm never executed is not a control (ID-62)"
  exit 1
fi

# ---- the control: knob OFF, and the red must be the case's own assertion -------------------
control_xml="${CONTROL_TMPDIR}/dualblock-control.xml"
out="${CONTROL_TMPDIR}/dualblock-control-output.txt"
rm -f "${control_xml}"
control_rc=0
MOBILEGL_IPC_ROLE_SPLIT_STATE=0 "${CTEST}" -L "${label}" --no-tests=error \
  --output-on-failure --output-junit "${control_xml}" > "${out}" 2>&1 || control_rc=$?
cat "${out}"
if [ "${control_rc}" -eq 0 ]; then
  echo "::error::MOBILEGL_IPC_ROLE_SPLIT_STATE=0 left the dual-block lane GREEN: the two roles shared one block and nothing noticed - the mechanism is not load-bearing (P5F §6)"
  exit 1
fi
if ! grep -q 'dual-block control:' "${out}"; then
  echo "::error::the knob-off run is red but carries none of the case's own 'dual-block control:' assertions - the failure is something else wearing the control's name"
  exit 1
fi
echo "negative control P5f-f1 (MOBILEGL_IPC_ROLE_SPLIT_STATE=0) turned ${matched} dual-block entries red, and the red is the case's own assertion, as it must"
