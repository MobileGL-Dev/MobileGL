#!/usr/bin/env bash
# Shared helpers for trace-fixture handling: reading the in-tree Git LFS pointer
# metadata and verifying a fixture file against it. Sourced by
# fetch-trace-fixture-lfs.sh (verify after download) and by
# trace-fixture-cache.sh (cache key derivation and verify after cache restore),
# so both paths agree on what a valid fixture is.

# Reads the Git LFS pointer tracked at HEAD for a fixture path and prints
# "<oid> <size>". Fails if the tracked blob is not a well-formed LFS pointer.
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

# Checks an on-disk fixture against the size and SHA-256 from its LFS pointer.
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

# Prints the fixture file paths of a trace case, one per line. Strips CR so the
# result is usable when python emits CRLF (Git Bash on Windows).
trace_fixture_files() {
  local case_name="$1"
  local fixture_dir="$2"
  local python_bin="${3:-python3}"

  "${python_bin}" tools/trace_replay/trace_cases.py \
    --format fixture-files \
    --case "${case_name}" \
    --fixture-root "${fixture_dir}" | tr -d '\r'
}
