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
WORK="$(mktemp -d)" || exit 1
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
  local rc=0
  env -i PATH="${STUB_DIR}:/usr/bin:/bin" STUB_MODE="$1" \
      CTEST=ctest CONTROL_TMPDIR="${WORK}/tmp-$1" \
      PULL_LIBRARY="${WORK}/pull.so" FROZEN_LIBRARY="${WORK}/frozen.so" \
      bash "${HERE}/retrace_pull_library_control.sh" OpenRA DirectGLES || rc=$?
  cmp -s "${WORK}/frozen.so" "${WORK}/split.so" || {
    echo 'F6 FAILED: pull control did not restore the split library'; return 1;
  }
  return "${rc}"
}

run_drop_draw() { # $1 = STUB_MODE
  cd "${WORK}" || return 127
  mkdir -p "${WORK}/OpenRA"
  env -i PATH="${STUB_DIR}:/usr/bin:/bin" STUB_MODE="$1" \
      CTEST=ctest CONTROL_TMPDIR="${WORK}/tmp-$1" \
      FROZEN_LIBRARY="${WORK}/frozen.so" LIBRARY_LOG="${WORK}/tmp-$1/mobilegl.log" \
      bash "${HERE}/retrace_drop_draw_control.sh" OpenRA DirectGLES
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
# P5 is complete: losing the runtime implementation must no longer disarm the gate.
expect FAILED "every split entry skipped (implementation lost)" -- run_split all-skipped
expect FAILED "E1 missing boundary probes"                  -- run_split e1-empty
expect FAILED "E1 own baseline is already red"               -- run_split e1-red-baseline
expect FAILED "E1 restored boundary remains red"             -- run_split e1-restore-red
expect FAILED "E1 record never reaches the peer"             -- run_split e1-no-peer
expect FAILED "E1 observed no real emission"                 -- run_split e1-no-emit
expect FAILED "E1 incorrectly waits every record"            -- run_split e1-waitall

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
printf '%s\n' 'int MG_Remote_stub(void) { return 1; }' > "${WORK}/split.c"
cc -shared -fPIC -o "${WORK}/frozen.so" "${WORK}/split.c" || exit 1
cp "${WORK}/frozen.so" "${WORK}/split.so"

# THE FINDING, part (b): a regex matching no tests. --no-tests=error exits non-zero and the old
# control read that as "the pull library turned it red".
expect FAILED "empty selection (--no-tests=error exit)"      -- run_retrace retrace-noselect
# A red that never names the transport: a fixture failure, a loader failure, a timeout.
expect FAILED "red without the transport-resolution message" -- run_retrace retrace-unrelated
# The real thing - and note the stub emits it CMake-wrapped across two lines, which a line-oriented
# grep for the literal sentence would miss.
expect PASSED "run_trace_case.cmake's own sentence, wrapped" -- run_retrace retrace-evidence
# The sentence a pull library ACTUALLY gets since P6's log rename: it wrote the unsuffixed
# mobilegl.log, the runner reads mobilegl.client.log, and the no-log check fires before the marker
# search. The control accepted only the marker sentence until B3's fix round 2, and was red on
# every real pull-library run (measured on the B3 package tree).
expect PASSED "the no-client-log sentence a pull library gets"  -- run_retrace retrace-evidence-nolog
# Both sentences again as `ctest -V` really prints them, with "1: " on every line: folded
# naively that is "never 1: reported resolving it", and the control used to red on it.
expect PASSED "the marker sentence under ctest -V's N: prefix" -- run_retrace retrace-evidence-prefixed
expect PASSED "the no-log sentence under ctest -V's N: prefix" -- run_retrace retrace-evidence-nolog-prefixed
# The pull library replaying green is the failure this control exists to catch.
expect FAILED "a pull library passed the split retrace"      -- run_retrace retrace-green

echo
echo "=== the retrace lane's draw-drop control (scripts/ci/retrace_drop_draw_control.sh)"
# Exit gate E2's picture control. The joint gate's finding is the reason it exists at all: the
# CLEAR-drop knob was armed, was read, dropped all 29 of OpenRA's clears - and the retrace still
# scored ssim 1.000000, because OpenRA overdraws every pixel it clears. So this control's three
# guards are each a different way for "the picture went red" to be someone else's red.
expect FAILED "empty selection (--no-tests=error exit)"      -- run_drop_draw dropdraw-noselect
# The failure the gate exists to catch: the golden survives the loss of every draw.
expect FAILED "the retrace passed with every draw dropped"   -- run_drop_draw dropdraw-green
# A red with no comparator output at all - a loader failure, a missing fixture, a timeout.
expect FAILED "red with no ssim summary in the output"       -- run_drop_draw dropdraw-nossim
# A red whose SSIM is FINE: something else (a Fatal{, a transport assertion) reddened the case.
expect FAILED "red but the ssim is above the threshold"      -- run_drop_draw dropdraw-ssimhigh
# A red picture with no evidence the knob was ever read by the process that produced it.
expect FAILED "no 'E2 control armed' line in the library log" -- run_drop_draw dropdraw-nolog
# The R-16 case the dropped-record COUNT exists for: the knob armed and dropped nothing, so the
# wrong picture came from somewhere else.
expect FAILED "the knob armed but dropped zero records"      -- run_drop_draw dropdraw-zero
# The real thing: a red picture, a fallen SSIM, and N > 0 records the library says it dropped.
expect PASSED "ssim below threshold and N records dropped"   -- run_drop_draw dropdraw-evidence

echo
echo "smoke test: ${passes} passed, ${failures} failed"
# Keep the private-file cases on the entry point used by CI and the local gate.
if ! bash "${HERE}/testdata/split_private_log_smoke.sh"; then
  failures=$((failures + 1))
fi
if [ "${failures}" -gt 0 ]; then
  echo "CONTROL_SMOKE_TEST_FAILED"
  exit 1
fi
echo "CONTROL_SMOKE_TEST_OK"
