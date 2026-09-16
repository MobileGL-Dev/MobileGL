#!/bin/bash
# R-16 FOR THE CI NEGATIVE CONTROLS THEMSELVES: a control-run smoke test.
#
# BRIEF-P5 13 (R-16) says a negative control must assert its own failure reason and that every gate
# carries a line saying "I made it red once, by doing X". The two controls this file exercises ARE
# gates, and until ID-46 finding 8 nobody could make either of them red, because a workflow `run:`
# block only executes on a runner. The wave-1 verification agent had to hand-copy the blocks into
# throwaway harnesses to show they were broken (wave1-codex-verify.md 8). This file is that
# experiment, kept: it runs the REAL control scripts - the same files .github/workflows/test.yml
# invokes, not copies of them - against a stubbed ctest, and checks that each one passes exactly
# when it should.
#
# The case that matters is the first one. A stubbed ctest reports a NON-EMPTY selection and then
# fails with UNRELATED_CONTROL_FAILURE: a reason that has nothing to do with the knob the control
# turns. Before ID-48's fix both controls printed their success message and the step exited 0. They
# must now report FAILED.
#
# usage: control_smoke_test.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

STUB_DIR="${WORK}/stub"
mkdir -p "${STUB_DIR}"
cp "${HERE}/testdata/stub_ctest.sh" "${STUB_DIR}/ctest"
chmod +x "${STUB_DIR}/ctest"

passes=0
failures=0

# expect <expected: PASSED|FAILED> <label> -- <command...>
expect() {
  want="$1"; label="$2"; shift 3   # shift past the literal "--"
  outfile="${WORK}/run.out"
  "$@" > "${outfile}" 2>&1
  rc=$?
  if [ "${rc}" -eq 0 ]; then got="PASSED"; else got="FAILED"; fi
  if [ "${got}" = "${want}" ]; then
    passes=$((passes + 1))
    printf 'ok       %-58s %s (rc=%d)\n' "${label}" "${got}" "${rc}"
  else
    failures=$((failures + 1))
    printf 'NOT OK   %-58s expected %s, got %s (rc=%d)\n' "${label}" "${want}" "${got}" "${rc}"
    sed 's/^/           | /' "${outfile}"
  fi
}

run_split() { # $1 = STUB_MODE
  env -i PATH="${STUB_DIR}:/usr/bin:/bin" STUB_MODE="$1" \
      CTEST=ctest CONTROL_TMPDIR="${WORK}/tmp-$1" \
      bash "${HERE}/split_negative_controls.sh"
}

run_retrace() { # $1 = STUB_MODE
  cd "${WORK}" || return 127
  mkdir -p "${WORK}/OpenRA"
  env -i PATH="${STUB_DIR}:/usr/bin:/bin" STUB_MODE="$1" \
      CTEST=ctest CONTROL_TMPDIR="${WORK}/tmp-$1" \
      PULL_LIBRARY="${WORK}/pull.so" FROZEN_LIBRARY="${WORK}/frozen.so" \
      bash "${HERE}/retrace_pull_library_control.sh" OpenRA DirectGLES
}

echo "=== the split lane's E1 / E3(a) controls (scripts/ci/split_negative_controls.sh)"
# THE FINDING, REPRODUCED. Non-empty selection, green baseline, and a red that is not the knob's.
expect FAILED "unrelated failure with a non-empty selection" -- run_split unrelated
# ... and the same control on the same stub, failing for its own reason: it must PASS.
expect PASSED "the scenarios' own diagnostic"               -- run_split evidence
# The pre-existing half of the control, which was never broken: a knob that reds nothing.
expect FAILED "the knob leaves the selection green"          -- run_split green
# The arming counter's half of the finding: a baseline that is already red cannot arm anything.
expect FAILED "the baseline is already red"                  -- run_split red-baseline
# The disarmed lane, which is a legitimate exit 0 while c1/s1/v1 are landing.
expect PASSED "every split entry skipped (lane not armed)"   -- run_split all-skipped

echo
echo "=== the retrace lane's pull-library control (scripts/ci/retrace_pull_library_control.sh)"
# A pull-shaped library the nm identity check accepts: a real ELF .so defining no MG_Remote symbol.
if command -v cc > /dev/null 2>&1; then
  printf '%s\n' 'int mobilegl_pull_only(void) { return 1; }' > "${WORK}/pull.c"
  cc -shared -fPIC -o "${WORK}/pull.so" "${WORK}/pull.c" || { echo "cannot build the stand-in library"; exit 1; }
else
  echo "no cc available; the retrace half of this smoke test needs one" >&2
  exit 1
fi
: > "${WORK}/frozen.so"

# THE FINDING, part (b): a regex matching no tests. --no-tests=error exits non-zero and the old
# control read that as "the pull library turned it red".
expect FAILED "empty selection (--no-tests=error exit)"      -- run_retrace retrace-noselect
# A red that never names the transport: a fixture failure, a loader failure, a timeout.
expect FAILED "red without the transport-resolution message" -- run_retrace retrace-unrelated
# The real thing - and note the stub emits it CMake-wrapped across two lines, which a line-oriented
# grep for the literal sentence would miss.
expect PASSED "run_trace_case.cmake's own sentence, wrapped" -- run_retrace retrace-evidence
# The pull library replaying green is the failure this control exists to catch.
expect FAILED "a pull library passed the split retrace"      -- run_retrace retrace-green

echo
echo "smoke test: ${passes} passed, ${failures} failed"
if [ "${failures}" -gt 0 ]; then
  echo "CONTROL_SMOKE_TEST_FAILED"
  exit 1
fi
echo "CONTROL_SMOKE_TEST_OK"
