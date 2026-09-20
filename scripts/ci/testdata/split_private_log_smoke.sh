#!/bin/bash
# Exercise E1's per-test boundary evidence and E3(a)'s private-file read (R-16).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/split-private-log.XXXXXX")
trap 'rm -rf "${WORK}"' EXIT
cp "${HERE}/testdata/stub_ctest.sh" "${WORK}/ctest"
chmod +x "${WORK}/ctest"
passes=0
for mode in missing-fatal stdout-fatal stale-fatal evidence e3-unrelated e3-no-private skipped-selection e3-skipped-selection notrun-selection missing-selection partial-fatal wrong-fatal; do
  mkdir -p "${WORK}/${mode}"
  rc=0
  STUB_MODE="${mode}" CTEST="${WORK}/ctest" CONTROL_TMPDIR="${WORK}/${mode}" \
    bash "${HERE}/split_negative_controls.sh" > "${WORK}/${mode}.out" 2>&1 || rc=$?
  if [ "${mode}" = evidence ]; then
    [ "${rc}" = 0 ] && grep -q "negative control E3(a).*scenario's own diagnostic" "${WORK}/${mode}.out" || {
      cat "${WORK}/${mode}.out"; echo "NOT OK own boundary/private-file evidence must PASS"; exit 1;
    }
    grep -qFx "private-log evidence: DirectGLES.Split.PersistentCoherentMapScenario.TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw: ${WORK}/${mode}/pmap.log" "${WORK}/${mode}.out" || {
      cat "${WORK}/${mode}.out"; echo "NOT OK private-file evidence line must name the selected entry and path"; exit 1;
    }
  else
    message='red lacks its own observed wait-boundary failure'
    [ "${mode}" != e3-unrelated ] || message='FAILED: red lacks its persistent-map push diagnostic'
    case "${mode}" in
      e3-no-private) message='no selected private log carries /MGPipe: persistent-map push disabled' ;;
      skipped-selection|notrun-selection) message='selected testcase skipped or did not run' ;;
      e3-skipped-selection) message='SplitLogPaths FAILED: E3(a) control: the knob killed the pre-flight, not the entry - 1 selected entries skipped' ;;
      missing-selection) message='selected testcase names are missing, duplicated or unexpected' ;;
    esac
    match=(-qF)
    [ "${rc}" != 0 ] && grep "${match[@]}" "${message}" "${WORK}/${mode}.out" || {
      cat "${WORK}/${mode}.out"; echo "NOT OK ${mode}: control must report FAILED for its own reason"; exit 1;
    }
  fi
  echo "ok private-log ${mode} (control rc=${rc})"
  passes=$((passes + 1))
done
echo "private-log smoke test: ${passes} passed, 0 failed"
