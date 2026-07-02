#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <trace-case> [fixture-dir]" >&2
  exit 2
fi

case_name="$1"
fixture_dir="${2:-tools/trace_replay/fixtures}"
python_bin="${PYTHON:-python3}"
mirror_base="${MOBILEGL_TRACE_FIXTURE_MIRROR_BASE:-https://repo.miawa.cn/mgl/tools/trace_replay/fixtures}"

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

fetch_from_mirror() {
  mkdir -p "${fixture_dir}"
  for file in "${files[@]}"; do
    local name
    local url
    name="$(basename "${file}")"
    url="${mirror_base%/}/${name}"
    echo "Fetching trace fixture from mirror: ${url}"
    if ! curl -L --fail --retry 3 --retry-delay 2 -o "${file}.tmp" "${url}"; then
      rm -f "${file}.tmp"
      return 1
    fi
    mv "${file}.tmp" "${file}"
  done
}

if fetch_from_mirror; then
  echo "Fetched trace fixture files for ${case_name} from mirror: ${include}"
else
  echo "Mirror fetch failed for ${case_name}; falling back to Git LFS: ${include}"
  git lfs install --local
  git lfs pull --include="${include}" --exclude=""
fi

for file in "${files[@]}"; do
  test -s "${file}"
  if head -n 1 "${file}" | grep -q "version https://git-lfs.github.com/spec/v1"; then
    echo "failed to hydrate LFS fixture: ${file}" >&2
    exit 1
  fi
done
