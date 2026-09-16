#!/bin/bash
# R-16's "I made it red once, by doing X" for scripts/ci/control_smoke_test.sh, mechanised so the
# claim can be re-checked rather than believed.
#
# X = revert the message check in each control, i.e. put the controls back in the state ID-46
#     finding 8 found them in: a non-zero ctest exit is accepted whatever the failure was.
#
# The smoke test must then FAIL, and it must fail on the two cases that exist for this defect -
# "unrelated failure with a non-empty selection" and "red without the transport-resolution
# message" - and not merely somewhere. A smoke test that goes red for any other reason when the
# evidence check is removed would not be pinning the evidence check.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'cp "${WORK}/split.orig" "${HERE}/split_negative_controls.sh"; cp "${WORK}/retrace.orig" "${HERE}/retrace_pull_library_control.sh"; cp "${WORK}/dropdraw.orig" "${HERE}/retrace_drop_draw_control.sh"; rm -rf "${WORK}"' EXIT

cp "${HERE}/split_negative_controls.sh" "${WORK}/split.orig"
cp "${HERE}/retrace_pull_library_control.sh" "${WORK}/retrace.orig"
cp "${HERE}/retrace_drop_draw_control.sh" "${WORK}/dropdraw.orig"

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
        "${HERE}/retrace_drop_draw_control.sh" <<'PY'
import sys
split, retrace, dropdraw = sys.argv[1], sys.argv[2], sys.argv[3]
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
# AND THE SPLIT CONTROL HAS THREE EVIDENCE CHECKS, NOT ONE, so all three are reverted. E1's is a
# read of the entry's own private library log, E3(a)'s ctest-output regex is the `elif` above,
# and E3(a)'s private-log half is ID-65's addition. Perturbing only one of the three leaves the
# other two catching the smoke test's "unrelated failure" case, and the case never flips - which
# is what this script measured the first time the perturbation actually applied.
rules = [
    (split, [('LINE', 'grep -qE "${evidence}"', ('if ! ', 'elif ! ')),
             ('SUBST', '"[A-Za-z_][A-Za-z_0-9]*"\\}\' || exit 1', '"[A-Za-z_][A-Za-z_0-9]*"\\}\' || true'),
             ('SUBST', '"${private_evidence}" "${name}" || exit 1',
                       '"${private_evidence}" "${name}" || true')]),
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
echo "=== the smoke test on the reverted controls (it MUST be red, on those two cases)"
bash "${HERE}/control_smoke_test.sh" > "${WORK}/after.log" 2>&1
rc=$?
cat "${WORK}/after.log"

if [ "${rc}" -eq 0 ]; then
  echo
  echo "RED-CHECK FAILED: the controls accept an unrelated failure again and the smoke test still passed."
  exit 1
fi

missed=0
for case in \
  "unrelated failure with a non-empty selection" \
  "red without the transport-resolution message" \
  "red but the ssim is above the threshold" \
  "no 'E2 control armed' line in the library log" \
  "the knob armed but dropped zero records"; do
  grep -q "NOT OK   ${case}" "${WORK}/after.log" || { echo "still green: ${case}"; missed=1; }
done
if [ "${missed}" -ne 0 ]; then
  echo
  echo "RED-CHECK FAILED: the smoke test went red, but not on the five cases the evidence checks exist for."
  exit 1
fi

echo
echo "P5_T1_CONTROL_SMOKE_REDCHECK_OK - removing the evidence checks reds exactly the five cases that pin them"
