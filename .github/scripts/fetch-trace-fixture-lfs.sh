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
download_attempts="${MOBILEGL_TRACE_FIXTURE_DOWNLOAD_ATTEMPTS:-5}"
retry_delay="${MOBILEGL_TRACE_FIXTURE_RETRY_DELAY:-2}"

if ! command -v "${python_bin}" >/dev/null 2>&1 && command -v python >/dev/null 2>&1; then
  python_bin=python
fi

if ! [[ "${download_attempts}" =~ ^[1-9][0-9]*$ ]]; then
  echo "MOBILEGL_TRACE_FIXTURE_DOWNLOAD_ATTEMPTS must be a positive integer: ${download_attempts}" >&2
  exit 2
fi
if ! [[ "${retry_delay}" =~ ^[0-9]+$ ]]; then
  echo "MOBILEGL_TRACE_FIXTURE_RETRY_DELAY must be a non-negative integer: ${retry_delay}" >&2
  exit 2
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

get_lfs_metadata() {
  local file="$1"
  local pointer
  local expected_oid
  local expected_size

  if ! pointer="$(git show "HEAD:${file}" 2>/dev/null)"; then
    echo "failed to read tracked fixture metadata: ${file}" >&2
    return 1
  fi
  if ! grep -q '^version https://git-lfs.github.com/spec/v1$' <<< "${pointer}"; then
    echo "tracked fixture is not a Git LFS pointer: ${file}" >&2
    return 1
  fi

  expected_oid="$(awk '$1 == "oid" && $2 ~ /^sha256:/ { sub(/^sha256:/, "", $2); print $2 }' <<< "${pointer}")"
  expected_size="$(awk '$1 == "size" { print $2 }' <<< "${pointer}")"
  if ! [[ "${expected_oid}" =~ ^[0-9a-f]{64}$ ]] || ! [[ "${expected_size}" =~ ^[0-9]+$ ]]; then
    echo "invalid Git LFS pointer metadata: ${file}" >&2
    return 1
  fi

  printf '%s %s\n' "${expected_oid}" "${expected_size}"
}

verify_fixture_file() {
  local downloaded_file="$1"
  local display_name="$2"
  local expected_oid="$3"
  local expected_size="$4"
  local actual_oid
  local actual_size

  if [ ! -f "${downloaded_file}" ]; then
    echo "fixture file is missing: ${display_name}" >&2
    return 1
  fi

  actual_size="$(wc -c < "${downloaded_file}" | tr -d '[:space:]')"
  if [ "${actual_size}" != "${expected_size}" ]; then
    echo "fixture size mismatch for ${display_name}: expected ${expected_size}, got ${actual_size}" >&2
    return 1
  fi

  actual_oid="$(sha256sum "${downloaded_file}" | awk '{ print $1 }')"
  if [ "${actual_oid}" != "${expected_oid}" ]; then
    echo "fixture SHA-256 mismatch for ${display_name}: expected ${expected_oid}, got ${actual_oid}" >&2
    return 1
  fi
}

fetch_file_from_mirror() {
  local file="$1"
  local url="$2"
  local metadata
  local expected_oid
  local expected_size
  local tmp_file="${file}.tmp"
  local attempt
  local partial_size
  local curl_status

  metadata="$(get_lfs_metadata "${file}")" || return 1
  read -r expected_oid expected_size <<< "${metadata}"

  if [ -f "${tmp_file}" ]; then
    partial_size="$(wc -c < "${tmp_file}" | tr -d '[:space:]')"
    if [ "${partial_size}" -gt "${expected_size}" ]; then
      echo "Discarding oversized partial fixture ${tmp_file}: ${partial_size} > ${expected_size}" >&2
      rm -f "${tmp_file}"
    elif [ "${partial_size}" = "${expected_size}" ]; then
      if verify_fixture_file "${tmp_file}" "${file}" "${expected_oid}" "${expected_size}"; then
        mv "${tmp_file}" "${file}"
        return 0
      fi
      rm -f "${tmp_file}"
    fi
  fi

  for ((attempt = 1; attempt <= download_attempts; attempt++)); do
    partial_size=0
    if [ -f "${tmp_file}" ]; then
      partial_size="$(wc -c < "${tmp_file}" | tr -d '[:space:]')"
    fi

    if [ "${partial_size}" -gt 0 ]; then
      echo "Resuming mirror download for ${file} at byte ${partial_size} (attempt ${attempt}/${download_attempts})"
    else
      echo "Starting mirror download for ${file} (attempt ${attempt}/${download_attempts})"
    fi

    if curl -L --fail --show-error --continue-at - --output "${tmp_file}" "${url}"; then
      if verify_fixture_file "${tmp_file}" "${file}" "${expected_oid}" "${expected_size}"; then
        mv "${tmp_file}" "${file}"
        return 0
      fi
      echo "Mirror download failed integrity verification; retrying from the beginning: ${file}" >&2
      rm -f "${tmp_file}"
    else
      curl_status=$?
      partial_size=0
      if [ -f "${tmp_file}" ]; then
        partial_size="$(wc -c < "${tmp_file}" | tr -d '[:space:]')"
      fi

      if [ "${partial_size}" = "${expected_size}" ]; then
        if verify_fixture_file "${tmp_file}" "${file}" "${expected_oid}" "${expected_size}"; then
          mv "${tmp_file}" "${file}"
          return 0
        fi
        rm -f "${tmp_file}"
        partial_size=0
      elif [ "${partial_size}" -gt "${expected_size}" ]; then
        echo "Discarding oversized partial fixture ${tmp_file}: ${partial_size} > ${expected_size}" >&2
        rm -f "${tmp_file}"
        partial_size=0
      elif [ "${curl_status}" -eq 33 ]; then
        echo "Mirror refused the resume request; retrying from the beginning: ${file}" >&2
        rm -f "${tmp_file}"
        partial_size=0
      fi

      echo "Mirror download attempt ${attempt}/${download_attempts} failed with curl exit ${curl_status}; retained ${partial_size} bytes for resume: ${file}" >&2
    fi

    if [ "${attempt}" -lt "${download_attempts}" ]; then
      sleep "${retry_delay}"
    fi
  done

  rm -f "${tmp_file}"
  return 1
}

fetch_from_mirror() {
  mkdir -p "${fixture_dir}"
  for file in "${files[@]}"; do
    local name
    local url
    name="$(basename "${file}")"
    url="${mirror_base%/}/${name}"
    echo "Fetching trace fixture from mirror: ${url}"
    if ! fetch_file_from_mirror "${file}" "${url}"; then
      return 1
    fi
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
  metadata="$(get_lfs_metadata "${file}")"
  read -r expected_oid expected_size <<< "${metadata}"
  verify_fixture_file "${file}" "${file}" "${expected_oid}" "${expected_size}"
done
