#!/bin/bash
# MobileGL - scripts/ci/control_smoke_test.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# A smoke test for the CI negative controls themselves.
#
# A negative control is a gate, and a workflow `run:` block only executes on a runner, so this
# file runs the real control script - the same file .github/workflows/test.yml invokes, not a copy
# of it - against a stubbed ctest, and checks that it passes exactly when it should.
#
# The case that matters is the first one. A stubbed ctest reports a non-empty selection and then
# fails with UNRELATED_CONTROL_FAILURE: a reason that has nothing to do with the knob the control
# turns. The control must report FAILED on it, and PASSED only on its scenarios' own diagnostic.
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
