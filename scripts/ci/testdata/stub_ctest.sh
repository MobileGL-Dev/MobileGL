#!/bin/bash
# MobileGL - scripts/ci/testdata/stub_ctest.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

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
#   missing-fatal      private file exists but lacks Fatal (stdout status still fails)
#   stdout-fatal       Fatal exists only on stdout, never in the private file
#   stale-fatal        Fatal exists before reset, never from this control run
#   e3-unrelated       E1 has its private Fatal; E3(a) fails for an unrelated reason
#   skipped-selection  knob kills pre-flight; private Fatal exists, entry skips, ctest exits 0
#   notrun-selection   selected entry was not run, despite a private Fatal
#   missing-selection  selected entry is absent from the result XML
#   partial-fatal      two entries fail, but only the first has its expected private Fatal
#   wrong-fatal        entry fails with a different Fatal in its private file
#   e3-no-private      E1 has its private Fatal; E3(a) has its PIXEL assertion but the library
#                      never says the push was disabled (the half ID-65 added)
#   green              baseline green; the control's own run PASSES (the knob is not load-bearing)
#   red-baseline       the baseline itself has a failed entry
#   all-skipped        the baseline is entirely skipped (lost implementation, a hard failure)
set -u

mode="${STUB_MODE:?STUB_MODE must be set}"

listing=1
junit=""
prev=""
for a in "$@"; do
  [ "$a" = "--show-only=json-v1" ] && json_requested=1
  [ "$a" = "-N" ] && listing_requested=1
  if [ "$prev" = "--output-junit" ]; then junit="$a"; fi
  if [ "$prev" = "-R" ]; then selector="$a"; fi
  prev="$a"
done
listing_requested="${listing_requested:-0}"

# E1's CPU-only boundary probes use their own testcase results. Keep observations
# inside each testcase so stdout-only and stale evidence remain rejected.
if [[ "${selector:-}" == *RemoteWaitBoundaryControl* ]]; then
  python3 - "${mode}" "${junit}" "${selector}" <<'PY'
import os, re, sys
import xml.etree.ElementTree as ET
mode, path, selector = sys.argv[1:]
barrier = int(os.environ['MOBILEGL_IPC_VERB_BARRIER'])
negative = barrier == 0
prefix = 'RemoteWaitBoundaryControl.'
specs = [
 ('AppliedClassWaitsWithRunAheadCap', 'ApplierReset', 1, True,
  'E1 wait boundary: ApplierReset returned before apply'),
 ('MissingServerCapKeepsClearLockstep', 'Clear', 0, True,
  'E1 wait boundary: Clear without server cap returned before apply'),
 ('ServerCapAllowsClearToRunAhead', 'Clear', 1, False, ''),
]
root = ET.Element('testsuite')
failures = 0
for index, (short, op, cap, waits, diagnostic) in enumerate(specs):
    name = prefix + short
    if not re.search(selector, name):
        continue
    if mode == 'e1-empty' or (negative and mode == 'missing-selection' and index == 1):
        continue
    case = ET.SubElement(root, 'testcase', name=name, status='run')
    waited = int(waits and barrier == 1)
    observation = (f'E1 observation: op={op} cap={cap} barrier={barrier} parked={waited} '
                   f'applied_before_return={waited} emitted=1 peer_seen=1')
    failed = negative and mode != 'green'
    if mode == 'e1-red-baseline' and 'baseline' in path:
        failed = True
    if mode == 'e1-restore-red' and 'restored' in path:
        failed = True
    if mode == 'e1-waitall' and not waits:
        failed = True
        observation = observation.replace('parked=0', 'parked=1').replace('applied_before_return=0', 'applied_before_return=1')
    if negative and mode == 'skipped-selection':
        ET.SubElement(case, 'skipped')
        failed = False
    if negative and mode == 'notrun-selection':
        case.set('status', 'notrun')
        failed = False
    if failed:
        ET.SubElement(case, 'failure', message='control red')
        failures += 1
    text = observation + '\n' + (diagnostic if negative else '')
    if negative:
        if mode == 'unrelated' or mode == 'wrong-fatal':
            text = observation + '\nUNRELATED_CONTROL_FAILURE'
        elif mode == 'missing-fatal' or (mode == 'partial-fatal' and index == 1):
            text = diagnostic
        elif mode == 'stdout-fatal':
            print(text)  # Must not qualify as this testcase's own evidence.
            text = 'UNRELATED_CONTROL_FAILURE'
        elif mode == 'stale-fatal':
            text = text.replace('barrier=0', 'barrier=1')
        elif mode == 'e1-no-peer':
            text = text.replace('peer_seen=1', 'peer_seen=0')
        elif mode == 'e1-no-emit':
            text = text.replace('emitted=1', 'emitted=0')
    ET.SubElement(case, 'system-out').text = text
ET.ElementTree(root).write(path, encoding='utf-8', xml_declaration=True)
print(f'E1 stub: {len(root)} selected, {failures} failed')
sys.exit(8 if failures else 0)
PY
  exit $?
fi

emit_listing() {
  echo "Test project /stub"
  echo "  Test #1: DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels"
  if [ "${mode}" = partial-fatal ]; then
    echo "  Test #2: DirectGLES.Split.TriangleScenario.SecondEntry"
    echo "Total Tests: 2"
  else
    echo "Total Tests: 1"
  fi
}

write_junit() {
  if [ "${MOBILEGL_IPC_VERB_BARRIER:-1}" = 0 ] || [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" = 0 ]; then
    entry=DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels
    [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" != 0 ] || entry=DirectGLES.Split.PersistentCoherentMapScenario.TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw
    body="<testcase name=\"${entry}\" status=\"fail\"><failure message=\"control red\"/></testcase>"
    if [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" = 0 ]; then
      case "${mode}" in
        evidence|e3-no-private)
          body="<testcase name=\"${entry}\" status=\"fail\"><failure/><system-out>the SECOND write through the same mapping, announced by nothing</system-out></testcase>" ;;
      esac
    fi
    if [ "${mode}" = partial-fatal ] && [ "${MOBILEGL_IPC_VERB_BARRIER:-1}" = 0 ]; then
      body="${body}<testcase name=\"DirectGLES.Split.TriangleScenario.SecondEntry\" status=\"fail\"><failure/></testcase>"
    fi
    case "${mode}" in
      e3-skipped-selection)
        if [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" = 0 ]; then
          body="<testcase name=\"${entry}\" status=\"notrun\"><skipped/></testcase>"
        fi ;;
      skipped-selection) body="<testcase name=\"${entry}\" status=\"notrun\"><skipped/></testcase>" ;;
      notrun-selection) body="<testcase name=\"${entry}\" status=\"notrun\"/>" ;;
      missing-selection) body='' ;;
      green) body="<testcase name=\"${entry}\" status=\"run\"/>" ;;
    esac
    printf '%s\n' "<testsuite>${body}</testsuite>" > "$1"
    return
  fi
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

# Model the library file sink separately from ctest stdout (ID-53).
#
# P6: the real sink writes one file per ROLE, `<base>.client.log` / `<base>.server.log`, and the
# validator derives those from the ENV base. So the manifest below keeps the base names (what the
# lane sets) while the on-disk writes here land in the CLIENT file - which is the role that raises
# these markers in a real run.
log="${CONTROL_TMPDIR}/entry.client.log"
if [ "${json_requested:-0}" = 1 ]; then
  python3 -c 'import json, os; p=os.environ["CONTROL_TMPDIR"]; entries=[("ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels", "entry.log"), ("PersistentCoherentMapScenario.TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw", "pmap.log")]; entries += [("TriangleScenario.SecondEntry", "second.log")] if os.environ["STUB_MODE"] == "partial-fatal" else []; print(json.dumps({"tests": [{"name": "DirectGLES.Split."+n, "properties": [{"name": "LABELS", "value": ["integration-split"]}, {"name": "ENVIRONMENT", "value": ["MOBILEGL_LOG_FILE_PATH="+p+"/"+f]}]} for n,f in entries]}))'
  if [ "${mode}" = stale-fatal ]; then
    echo 'Fatal{BarrierViolation, "DrawVbo"}' > "${log}"
  fi
  exit 0
fi

if [ "${listing_requested}" = "1" ]; then
  emit_listing
  exit 0
fi

if [ -n "${junit}" ]; then
  write_junit "${junit}"
  if [ "${MOBILEGL_IPC_VERB_BARRIER:-1}" != 0 ] && [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" != 0 ]; then
  case "${mode}" in
    red-baseline) echo "1/1 Test #1: ... ***Failed"; exit 8 ;;
    *)            echo "100% tests passed, 0 tests failed out of 1"; exit 0 ;;
  esac
  fi
fi

# The control's own run.
if [ "${MOBILEGL_IPC_VERB_BARRIER:-1}" = 0 ]; then
  case "${mode}" in
    evidence|e3-unrelated|e3-no-private|skipped-selection|e3-skipped-selection|notrun-selection|missing-selection|partial-fatal) echo 'Fatal{BarrierViolation, "DrawVbo"}' > "${log}" ;;
    wrong-fatal) echo 'Fatal{ReplyMissing, "DrawVbo"}' > "${log}" ;;
    missing-fatal) echo "library setup only; no fatal" > "${log}" ;;
    stdout-fatal) echo 'Fatal{BarrierViolation, "DrawVbo"}' ;;
  esac
fi
# E3(a)'s library line, in the PERSISTENT-MAP entry's own private file - a different file from
# E1's, exactly as the manifest above declares. `e3-no-private` is the mode that leaves it out:
# the pixel assertion arrives, the library says nothing, and the control must refuse the red.
if [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" = 0 ] && [ "${mode}" = evidence ]; then
  echo 'MGPipe: persistent-map push disabled - MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0 is exit gate E3(a)'"'"'s NEGATIVE CONTROL' \
    > "${CONTROL_TMPDIR}/pmap.client.log"
fi
case "${mode}" in
  e3-skipped-selection)
    if [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" = 0 ]; then
      echo 'selected E3 entry ... ***Skipped'
      exit 0
    fi
    exit 8
    ;;
  skipped-selection|notrun-selection|missing-selection)
    echo '1/1 Test #1: selected entry ... ***Skipped'
    echo '100% tests passed, 0 tests failed out of 1'
    exit 0
    ;;
  e3-no-private)
    # E1's half passes (its private Fatal is written above); E3(a)'s pixel assertion arrives on
    # stdout and its private line does not, so the control must stop at the second half.
    echo "1/1 Test #1: DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels ...***Failed"
    if [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" = 0 ]; then
      echo "../MobileGL/MG_IntegrationTest/Scenarios/PersistentCoherentMapScenario.cpp:414: Failure"
      echo "the SECOND write through the same mapping, announced by nothing: this is exit gate E3(b)"
    fi
    exit 8
    ;;
  unrelated|missing-fatal|stdout-fatal|stale-fatal|e3-unrelated|partial-fatal|wrong-fatal)
    echo "1/1 Test #1: DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels ...***Failed"
    echo "UNRELATED_CONTROL_FAILURE: the harness aborted in setup before the knob was read"
    if [ "${MOBILEGL_IPC_PERSISTENT_BLOCK_KB:-64}" = 0 ]; then
      case "${mode}" in
        missing-fatal|stdout-fatal|stale-fatal)
          echo "the SECOND write through the same mapping, announced by nothing" ;;
      esac
    fi
    exit 8
    ;;
  evidence)
    # E1 is file-only above; E3(a) emits its scenario assertion to ctest.
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
  *)
    echo "stub_ctest: unknown STUB_MODE '${mode}'" >&2
    exit 127
    ;;
esac
