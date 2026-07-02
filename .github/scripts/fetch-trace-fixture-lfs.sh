#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <trace-case> [fixture-dir]" >&2
  exit 2
fi

case_name="$1"
fixture_dir="${2:-tools/trace_replay/fixtures}"
python_bin="${PYTHON:-python3}"

if ! command -v "${python_bin}" >/dev/null 2>&1 && command -v python >/dev/null 2>&1; then
  python_bin=python
fi

fixture_list="$("${python_bin}" tools/trace_replay/trace_cases.py \
  --format fixture-files \
  --case "${case_name}" \
  --fixture-root "${fixture_dir}")"
mapfile -t files <<< "${fixture_list}"

include="$(IFS=,; echo "${files[*]}")"
if [ "${case_name}" = "OpenRA" ]; then
  echo "Fixture files for ${case_name} are stored in Git: ${include}"
  for file in "${files[@]}"; do
    test -s "${file}"
    if head -n 1 "${file}" | grep -q "version https://git-lfs.github.com/spec/v1"; then
      echo "fixture should not be stored as an LFS pointer: ${file}" >&2
      exit 1
    fi
  done
  exit 0
fi

echo "Fetching LFS fixture files for ${case_name}: ${include}"

git lfs install --local
git lfs pull --include="${include}" --exclude=""

for file in "${files[@]}"; do
  test -s "${file}"
  if head -n 1 "${file}" | grep -q "version https://git-lfs.github.com/spec/v1"; then
    echo "failed to hydrate LFS fixture: ${file}" >&2
    exit 1
  fi
done
