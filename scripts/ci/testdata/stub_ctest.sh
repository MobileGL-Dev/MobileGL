#!/bin/bash
# A stubbed `ctest` for scripts/ci/control_smoke_test.sh.
#
# Descended from the verification agent's stub (wave1-codex-verify.md 8, ~/w7/p5-verify-f8-stub/ctest),
# which is what CONFIRMED that the negative controls accepted an unrelated failure. Every mode below
# is deliberately the BEST case for the control under test: the selection is never empty except in
# the mode that exists to test the empty-selection guard, and the baseline is green except in the
# mode that exists to test the red-baseline guard. If a control passes here it is because the
# control's logic is wrong, not because the stub starved it.
#
# STUB_MODE:
#   unrelated          baseline green; the control's own run fails with UNRELATED_CONTROL_FAILURE
#   evidence           baseline green; the control's own run fails with the scenarios' own wording
#   green              baseline green; the control's own run PASSES (the knob is not load-bearing)
#   red-baseline       the baseline itself has a failed entry
#   all-skipped        the baseline is entirely skipped (the disarmed lane, a legitimate exit 0)
#   retrace-noselect   `ctest -N` matches nothing; the run exits 8 the way --no-tests=error does
#   retrace-unrelated  one match; the run fails without naming the transport
#   retrace-evidence   one match; the run fails with run_trace_case.cmake's own sentence
#   retrace-green      one match; the run PASSES
set -u

mode="${STUB_MODE:?STUB_MODE must be set}"

listing=1
junit=""
prev=""
for a in "$@"; do
  [ "$a" = "-N" ] && listing_requested=1
  if [ "$prev" = "--output-junit" ]; then junit="$a"; fi
  prev="$a"
done
listing_requested="${listing_requested:-0}"

emit_listing() {
  echo "Test project /stub"
  if [ "${mode}" = "retrace-noselect" ]; then
    echo "Total Tests: 0"
    return
  fi
  echo "  Test #1: DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels"
  echo "Total Tests: 1"
}

write_junit() {
  case "${mode}" in
    red-baseline)
      body='<testcase name="DirectGLES.Split.TriangleScenario.AVboBackedTriangleReachesReadPixels" status="failed"><failure message="already red"/></testcase>'
      ;;
    all-skipped)
      body='<testcase name="DirectGLES.Split.TriangleScenario.AVboBackedTriangleReachesReadPixels" status="notrun"><skipped/></testcase>'
      ;;
    *)
      body='<testcase name="DirectGLES.Split.TriangleScenario.AVboBackedTriangleReachesReadPixels" status="run" time="0.3"/>'
      ;;
  esac
  printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' "<testsuite name=\"stub\">" "  ${body}" '</testsuite>' > "$1"
}

if [ "${listing_requested}" = "1" ]; then
  emit_listing
  exit 0
fi

if [ -n "${junit}" ]; then
  write_junit "${junit}"
  case "${mode}" in
    red-baseline) echo "1/1 Test #1: ... ***Failed"; exit 8 ;;
    *)            echo "100% tests passed, 0 tests failed out of 1"; exit 0 ;;
  esac
fi

# The control's own run.
case "${mode}" in
  unrelated)
    echo "1/1 Test #1: DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels ...***Failed"
    echo "UNRELATED_CONTROL_FAILURE: the harness aborted in setup before the knob was read"
    exit 8
    ;;
  evidence)
    # Both controls' required wording, so one stub serves E1 and E3(a). Copied from the real
    # diagnostics: ~/w7/p5-v1-joint-isplit-barrier0.log for the first, and
    # PersistentCoherentMapScenario.cpp:414-417 for the second.
    echo "1/1 Test #1: DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels ...***Failed"
    echo "../MobileGL/MG_IntegrationTest/Scenarios/ClearThenReadPixelsScenario.cpp:290: Failure"
    echo "Expected: (bottom.r) > (200), actual: '\\0' vs 200"
    echo "the SECOND write through the same mapping, announced by nothing: this is exit gate E3(b)"
    exit 8
    ;;
  green)
    echo "100% tests passed, 0 tests failed out of 1"
    exit 0
    ;;
  retrace-noselect)
    echo "No tests were found!!!"
    exit 8
    ;;
  retrace-unrelated)
    echo "1/1 Test #1: MobileGLTraceReplay.OpenRA.DirectGLES ...***Failed"
    echo "CMake Error: the fixture could not be unpacked"
    exit 8
    ;;
  retrace-evidence)
    echo "1/1 Test #1: MobileGLTraceReplay.OpenRA.DirectGLES ...***Failed"
    echo "CMake Error at run_trace_case.cmake:279 (message):"
    echo "  MOBILEGL_TRANSPORT=inproc is set for OpenRA DirectGLES and the library never"
    echo "  reported resolving it: mobilegl.log carries no"
    echo '  "MOBILEGL_TRANSPORT=inproc - the MGPipe record stream".'
    exit 8
    ;;
  retrace-green)
    echo "100% tests passed, 0 tests failed out of 1"
    exit 0
    ;;
  *)
    echo "stub_ctest: unknown STUB_MODE '${mode}'" >&2
    exit 127
    ;;
esac
