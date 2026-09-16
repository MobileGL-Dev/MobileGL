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
python3 - "${HERE}/split_negative_controls.sh" "${HERE}/retrace_pull_library_control.sh" <<'PY' || exit 1
import sys
import re
split, retrace = sys.argv[1], sys.argv[2]
for path, needle in ((split, 'grep -qE "${evidence}"'), (retrace, 'grep -qF "${EVIDENCE}"')):
    text = open(path).read()
    if path == split:
        text, count = re.subn(
            r'(?m)^    python3 "\$\{log_helper\}" evidence[^\n]*\n[^\n]*\|\| exit 1\n',
            '    : "private-file evidence check removed by red-check"\n', text)
        if count != 1:
            raise SystemExit(f"expected exactly one private-file evidence check in {path}, found {count}")
    out, hit = [], 0
    for line in text.splitlines(keepends=True):
        if needle in line and line.lstrip().startswith(('if ! ', 'elif ! ')):
            indent = line[:len(line) - len(line.lstrip())]
            keyword = 'elif' if line.lstrip().startswith('elif ') else 'if'
            out.append(f"{indent}{keyword} false; then\n")
            hit += 1
        else:
            out.append(line)
    if hit != 1:
        raise SystemExit(f"expected exactly one evidence check in {path}, found {hit}")
    open(path, 'w').write(''.join(out))
print("E1 private-file, E3(a) and retrace evidence checks reverted to 'any non-zero ctest exit is accepted'")
PY

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
if [ "${missed}" -ne 0 ]; then
  echo
  echo "RED-CHECK FAILED: the smoke test went red, but not on the unrelated split, unrelated retrace and missing private-Fatal cases."
  exit 1
fi

echo
echo "P5_T1_CONTROL_SMOKE_REDCHECK_OK - removing the evidence checks reds the unrelated split, unrelated retrace and missing private-Fatal cases"
