#!/usr/bin/env bash
# Cache-side helper for trace fixtures.
#
#   key <case> [fixture-dir]     derive the actions/cache key and path list
#   verify <case> [fixture-dir]  check restored fixtures against their pointers
#   reset <case> [fixture-dir]   drop restored fixtures, leaving the pointers
#
# The cache key is content-addressed on the Git LFS pointer oids tracked at
# HEAD, which are readable from a plain checkout without smudging. Fixture
# content therefore maps 1:1 onto a key: unchanged content hits, changed
# content is a new key and thus a miss, and the download path handles it. The
# key deliberately carries no restore-keys prefix in the workflow - a fixture
# that does not match the pointer exactly must never be restored.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=trace-fixture-lib.sh
. "${script_dir}/trace-fixture-lib.sh"

# Bump when the key derivation changes in a way that must invalidate old
# entries; the content digest alone would not notice a format change.
key_schema="v1"

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: $0 <key|verify|reset> <trace-case> [fixture-dir]" >&2
  exit 2
fi

command_name="$1"
case_name="$2"
fixture_dir="${3:-tools/trace_replay/fixtures}"
python_bin="${PYTHON:-python3}"

if ! command -v "${python_bin}" >/dev/null 2>&1 && command -v python >/dev/null 2>&1; then
  python_bin=python
fi

mapfile -t files < <(trace_fixture_files "${case_name}" "${fixture_dir}" "${python_bin}")
if [ "${#files[@]}" -eq 0 ]; then
  echo "no fixture files declared for trace case: ${case_name}" >&2
  exit 1
fi

# Writes "name=value" to $GITHUB_OUTPUT when running under Actions, and to
# stdout otherwise so the script stays runnable (and testable) off-CI.
emit_output() {
  local name="$1"
  local value="$2"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    if [[ "${value}" == *$'\n'* ]]; then
      local delimiter="ghadelim_$(date +%s%N)_$$"
      {
        printf '%s<<%s\n' "${name}" "${delimiter}"
        printf '%s\n' "${value}"
        printf '%s\n' "${delimiter}"
      } >> "${GITHUB_OUTPUT}"
    else
      printf '%s=%s\n' "${name}" "${value}" >> "${GITHUB_OUTPUT}"
    fi
  fi
  printf '%s=%s\n' "${name}" "${value}"
}

sanitize_case() {
  printf '%s' "$1" | sed 's/[^A-Za-z0-9._-]/_/g'
}

case "${command_name}" in
  key)
    manifest=""
    for file in "${files[@]}"; do
      # A case whose fixtures are committed directly rather than through Git LFS
      # (OpenRA) has no pointer oid to key on, and nothing to download either.
      # Report it as uncacheable so the workflow skips the cache entirely.
      if ! metadata="$(get_lfs_metadata "${file}" 2>/dev/null)"; then
        echo "trace case ${case_name} is not stored in Git LFS; skipping fixture cache" >&2
        emit_output "cacheable" "false"
        emit_output "key" ""
        exit 0
      fi
      read -r expected_oid expected_size <<< "${metadata}"
      manifest+="$(basename "${file}") ${expected_oid} ${expected_size}"$'\n'
    done

    digest="$(printf '%s' "${manifest}" | sha256sum | awk '{ print substr($1, 1, 16) }')"
    safe_case="$(sanitize_case "${case_name}")"

    emit_output "cacheable" "true"
    emit_output "key" "trace-fixture-${key_schema}-${safe_case}-${digest}"
    emit_output "paths" "$(printf '%s\n' "${files[@]}")"
    ;;

  verify)
    for file in "${files[@]}"; do
      metadata="$(get_lfs_metadata "${file}")"
      read -r expected_oid expected_size <<< "${metadata}"
      verify_fixture_file "${file}" "${file}" "${expected_oid}" "${expected_size}"
    done
    echo "Verified ${#files[@]} fixture file(s) for ${case_name} against the tracked Git LFS pointers."
    ;;

  reset)
    # Put the working tree back to the pointer files a fresh checkout would
    # have, so that a rejected cache entry falls through to exactly the same
    # download path a cache miss takes.
    for file in "${files[@]}"; do
      rm -f "${file}" "${file}.tmp"
    done
    git checkout -- "${files[@]}"
    echo "Reset ${#files[@]} fixture file(s) for ${case_name} to their tracked Git LFS pointers."
    ;;

  *)
    echo "unknown command: ${command_name}" >&2
    exit 2
    ;;
esac
