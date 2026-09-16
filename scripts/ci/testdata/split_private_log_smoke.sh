#!/bin/bash
# Exercise the production control, including the private-file read (ID-53 / R-16).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/split-private-log.XXXXXX")
trap 'rm -rf "${WORK}"' EXIT
cp "${HERE}/testdata/stub_ctest.sh" "${WORK}/ctest"
chmod +x "${WORK}/ctest"
passes=0
for mode in missing-fatal stdout-fatal stale-fatal evidence e3-unrelated; do
  mkdir -p "${WORK}/${mode}"
  rc=0
  STUB_MODE="${mode}" CTEST="${WORK}/ctest" CONTROL_TMPDIR="${WORK}/${mode}" \
    bash "${HERE}/split_negative_controls.sh" > "${WORK}/${mode}.out" 2>&1 || rc=$?
  if [ "${mode}" = evidence ]; then
    [ "${rc}" = 0 ] && grep -q "negative control E3(a).*scenario's own diagnostic" "${WORK}/${mode}.out" || {
      cat "${WORK}/${mode}.out"; echo "NOT OK private-file Fatal must PASS"; exit 1;
    }
    grep -qFx "private-log evidence: DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels: ${WORK}/${mode}/entry.log" "${WORK}/${mode}.out" || {
      cat "${WORK}/${mode}.out"; echo "NOT OK private-file evidence line must name the selected entry and path"; exit 1;
    }
  else
    message='E1 FAILED: selected private logs lack expected Fatal'
    [ "${mode}" != e3-unrelated ] || message='FAILED: red lacks its persistent-map push diagnostic'
    [ "${rc}" != 0 ] && grep -q "${message}" "${WORK}/${mode}.out" || {
      cat "${WORK}/${mode}.out"; echo "NOT OK ${mode}: control must report FAILED for its own reason"; exit 1;
    }
  fi
  echo "ok private-log ${mode} (control rc=${rc})"
  passes=$((passes + 1))
done
echo "private-log smoke test: ${passes} passed, 0 failed"
