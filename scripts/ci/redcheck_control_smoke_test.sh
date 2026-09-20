#!/bin/bash
# R-16's "I made it red once, by doing X" for scripts/ci/control_smoke_test.sh, mechanised so the
# claim can be re-checked rather than believed.
#
# X = revert the message check in each control, i.e. put the controls back in the state ID-46
#     finding 8 found them in: a non-zero ctest exit is accepted whatever the failure was.
#
# The smoke test must then FAIL on the cases that exist for this defect -
# "unrelated failure with a non-empty selection" and "red without the transport-resolution
# message", plus the missing private-file Fatal - and not merely somewhere. A smoke test that goes
# red for any other reason when the evidence check is removed would not be pinning the evidence check.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)" || exit 1
HELPER="${HERE}/../../MobileGL/MG_IntegrationTest/Harness/split_log_paths.py"
WAIT_HELPER="${HERE}/wait_boundary_negative_control.py"
trap 'cp "${WORK}/split.orig" "${HERE}/split_negative_controls.sh"; cp "${WORK}/retrace.orig" "${HERE}/retrace_pull_library_control.sh"; cp "${WORK}/dropdraw.orig" "${HERE}/retrace_drop_draw_control.sh"; cp "${WORK}/helper.orig" "${HELPER}"; cp "${WORK}/wait.orig" "${WAIT_HELPER}"; rm -rf "${WORK}"' EXIT

cp "${HERE}/split_negative_controls.sh" "${WORK}/split.orig"
cp "${HERE}/retrace_pull_library_control.sh" "${WORK}/retrace.orig"
cp "${HERE}/retrace_drop_draw_control.sh" "${WORK}/dropdraw.orig"
cp "${HELPER}" "${WORK}/helper.orig"
cp "${WAIT_HELPER}" "${WORK}/wait.orig"

echo "=== baseline: the smoke test must be GREEN before anything is perturbed"
if ! bash "${HERE}/control_smoke_test.sh" > "${WORK}/before.log" 2>&1; then
  echo "the smoke test is ALREADY RED; the red-check below would prove nothing"
  cat "${WORK}/before.log"
  exit 1
fi
tail -1 "${WORK}/before.log"

echo
echo "=== perturbation: remove the evidence check from both controls"
python3 - "${HERE}/split_negative_controls.sh" "${HERE}/retrace_pull_library_control.sh" \
        "${HERE}/retrace_drop_draw_control.sh" "${WAIT_HELPER}" <<'PY' || exit 1
import sys
split, retrace, dropdraw, wait = sys.argv[1:]
# (file, [(needle, the prefix the line must start with)]). The draw-drop control has THREE
# evidence checks rather than one, because "the picture went red" has three different ways of
# being somebody else's red: the SSIM never fell, the library never said the knob armed, and the
# knob armed and dropped nothing. All three are reverted together, and the smoke cases that pin
# each of them must go red below.
#
# THE PREFIXES ARE A TUPLE, AND THAT IS A FIX, not a generalisation for its own sake. The first
# version matched only lines starting with `if ! `, and the split control's evidence check has
# been `elif ! tr -s ... | grep -qE "${evidence}"` since it was written - so this red-check found
# ZERO checks in it, the SystemExit below was not checked by the caller, and the run went on to
# smoke-test the UNPERTURBED controls and report "RED-CHECK FAILED: the controls accept an
# unrelated failure again and the smoke test still passed". It failed safe rather than green, so
# nothing was ever silently proved - but the one control it was most about was never perturbed.
# Reproduced on p5/joint@e61d0012 before this line changed.
#
# AND THE SPLIT CONTROL HAS THREE EVIDENCE CHECKS, NOT ONE. E1 checks each boundary
# probe's own observation and assertion; E3(a) checks its assertion and private log.
# All three are reverted: perturbing only one leaves the
# other two catching the smoke test's "unrelated failure" case, and the case never flips - which
# is what this script measured the first time the perturbation actually applied.
rules = [
    (split, [('LINE', '"${log_helper}" assertion', ('if ! ', 'elif ! ')),
             ('SUBST', '"${private_evidence}" "${name}" || exit 1',
                       '"${private_evidence}" "${name}" || true')]),
    (wait, [('SUBST', 'if not own_negative(text, observation, diagnostic):',
                       'if False:  # E1 evidence check removed by red-check')]),
    (retrace, [('LINE', 'grep -qF "${EVIDENCE}"', ('if ! ',))]),
    (dropdraw, [('LINE', 'awk -v a="${ssim}"', ('if ! ',)),
                ('LINE', '[ -z "${armed_line}" ]', ('if ',)),
                ('LINE', '[ "${dropped}" -lt 1 ]', ('if ',))]),
]
for path, checks in rules:
    text = open(path).read()
    for rule in checks:
        if rule[0] == 'SUBST':
            _, old, new = rule
            if text.count(old) != 1:
                raise SystemExit(f"expected exactly one {old!r} in {path}, found {text.count(old)}")
            text = text.replace(old, new)
            continue
        _, needle, prefixes = rule
        out, hit = [], 0
        for line in text.splitlines(keepends=True):
            if needle in line and line.lstrip().startswith(prefixes):
                indent = line[:len(line) - len(line.lstrip())]
                # KEEP THE KEYWORD. Turning an `elif` into an `if` splits the chain in two and
                # leaves the second half's `fi` dangling - a syntax error, which the smoke test
                # then reports as rc=2 on every case and which looks nothing like "the control
                # accepted an unrelated failure".
                keyword = 'elif' if line.lstrip().startswith('elif') else 'if'
                out.append(f"{indent}{keyword} false; then\n")
                hit += 1
            else:
                out.append(line)
        if hit != 1:
            raise SystemExit(f"expected exactly one {needle!r} check in {path}, found {hit}")
        text = ''.join(out)
    open(path, 'w').write(text)
print("all seven evidence checks reverted to 'any non-zero ctest exit is accepted'")
PY
# AND THE PERTURBATION'S OWN FAILURE IS FATAL. Without this, a SystemExit above left the controls
# UNTOUCHED and the run continued to smoke-test them, printing "RED-CHECK FAILED: the controls
# accept an unrelated failure again and the smoke test still passed" - a true statement about a
# perturbation that never happened, and the wrong diagnosis to hand whoever reads it.
if [ $? -ne 0 ]; then
  echo "RED-CHECK ABORTED: the perturbation did not apply, so nothing below would be measuring it."
  exit 1
fi

echo
echo "=== the smoke test on the reverted controls (it MUST be red on the pinned cases)"
bash "${HERE}/control_smoke_test.sh" > "${WORK}/after.log" 2>&1
rc=$?
cat "${WORK}/after.log"

if [ "${rc}" -eq 0 ]; then
  echo
  echo "RED-CHECK FAILED: the controls accept an unrelated failure again and the smoke test still passed."
  exit 1
fi

missed=0
grep -q "NOT OK   unrelated failure with a non-empty selection" "${WORK}/after.log" || missed=1
grep -q "NOT OK   red without the transport-resolution message" "${WORK}/after.log" || missed=1
grep -qF "NOT OK missing-fatal: control must report FAILED for its own reason" "${WORK}/after.log" || missed=1
for case in \
  "red but the ssim is above the threshold" \
  "no 'E2 control armed' line in the library log" \
  "the knob armed but dropped zero records"; do
  grep -q "NOT OK   ${case}" "${WORK}/after.log" || { echo "still green: ${case}"; missed=1; }
done
if [ "${missed}" -ne 0 ]; then
  echo
  echo "RED-CHECK FAILED: the smoke test did not red every named split, retrace, draw-drop and private-log evidence case."
  exit 1
fi

echo
cp "${WORK}/split.orig" "${HERE}/split_negative_controls.sh"
cp "${WORK}/retrace.orig" "${HERE}/retrace_pull_library_control.sh"
cp "${WORK}/dropdraw.orig" "${HERE}/retrace_drop_draw_control.sh"
cp "${WORK}/wait.orig" "${WAIT_HELPER}"
echo "=== ID-62 perturbation: remove only the skip check"
python3 - "${HELPER}" "${WAIT_HELPER}" <<'PY' || exit 1
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
needle = '        if skipped:\n'
if text.count(needle) != 1:
    raise SystemExit('expected exactly one skip check')
path.write_text(text.replace(needle, '        if False:  # skip check removed by red-check\n'))
path = Path(sys.argv[2])
text = path.read_text()
needle = '        if case.find("skipped") is not None or case.get("status") in {"notrun", "disabled"}:\n'
if text.count(needle) != 1:
    raise SystemExit('expected exactly one E1 skip check')
path.write_text(text.replace(needle, '        if False:  # E1 skip check removed by red-check\n'))
PY
bash "${HERE}/control_smoke_test.sh" > "${WORK}/skips.log" 2>&1
rc=$?
cat "${WORK}/skips.log"
if [ "${rc}" -eq 0 ] || ! grep -qFx 'NOT OK skipped-selection: control must report FAILED for its own reason' "${WORK}/skips.log"; then
  echo 'RED-CHECK FAILED: removing the skip check must red the skipped-selection message assertion'
  exit 1
fi
echo 'ID-62 red-once: removing only the skip check made skipped-selection red (smoke rc=1)'
echo "P5_T1_CONTROL_SMOKE_REDCHECK_OK - evidence and skipped-selection checks each made every named smoke case red"
