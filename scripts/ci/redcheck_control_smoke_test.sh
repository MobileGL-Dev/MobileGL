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
trap 'cp "${WORK}/split.orig" "${HERE}/split_negative_controls.sh"; cp "${WORK}/retrace.orig" "${HERE}/retrace_pull_library_control.sh"; rm -rf "${WORK}"' EXIT

cp "${HERE}/split_negative_controls.sh" "${WORK}/split.orig"
cp "${HERE}/retrace_pull_library_control.sh" "${WORK}/retrace.orig"

echo "=== baseline: the smoke test must be GREEN before anything is perturbed"
if ! bash "${HERE}/control_smoke_test.sh" > "${WORK}/before.log" 2>&1; then
  echo "the smoke test is ALREADY RED; the red-check below would prove nothing"
  cat "${WORK}/before.log"
  exit 1
fi
tail -1 "${WORK}/before.log"

echo
echo "=== perturbation: remove the evidence check from both controls"
python3 - "${HERE}/split_negative_controls.sh" "${HERE}/retrace_pull_library_control.sh" <<'PY'
import sys
split, retrace = sys.argv[1], sys.argv[2]
for path, needle in ((split, 'grep -qE "${evidence}"'), (retrace, 'grep -qF "${EVIDENCE}"')):
    text = open(path).read()
    out, hit = [], 0
    for line in text.splitlines(keepends=True):
        if needle in line and line.lstrip().startswith('if ! '):
            indent = line[:len(line) - len(line.lstrip())]
            out.append(f"{indent}if false; then\n")
            hit += 1
        else:
            out.append(line)
    if hit != 1:
        raise SystemExit(f"expected exactly one evidence check in {path}, found {hit}")
    open(path, 'w').write(''.join(out))
print("both evidence checks reverted to 'any non-zero ctest exit is accepted'")
PY

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
grep -q "NOT OK   unrelated failure with a non-empty selection" "${WORK}/after.log" || missed=1
grep -q "NOT OK   red without the transport-resolution message" "${WORK}/after.log" || missed=1
if [ "${missed}" -ne 0 ]; then
  echo
  echo "RED-CHECK FAILED: the smoke test went red, but not on the two cases the evidence check exists for."
  exit 1
fi

echo
echo "P5_T1_CONTROL_SMOKE_REDCHECK_OK - removing the evidence check reds exactly the two cases that pin it"
