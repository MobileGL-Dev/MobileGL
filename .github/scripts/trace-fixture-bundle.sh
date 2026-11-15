#!/usr/bin/env bash
# One cache entry that holds every Git LFS trace fixture of the CI cases.
#
#   key                derive the actions/cache key, restore prefix and path of the bundle
#   ensure             after a cache miss: complete the bundle directory, fetching only the
#                      cases whose files are missing from it or do not match their pointers
#   wait <key> [secs]  wait until the cache entry <key> is listed for this ref (needs GH_TOKEN)
#   install <case>...  move the fixtures of these cases from the restored bundle directory into
#                      the fixture directory, fetch the cases it does not hold, and check every
#                      file against its pointer
#
# The key is content-addressed on the pointer oids tracked at HEAD, which a plain checkout
# reads without smudging. The bundle directory is the same path for every revision, so
# `ensure` can start from an older bundle. Fixtures stored in Git itself rather than in Git
# LFS (OpenRA) are not in the bundle; `install` checks that they are real files.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=trace-fixture-lib.sh
. "${script_dir}/trace-fixture-lib.sh"

# Bump when the key derivation or the bundle layout changes in a way that must invalidate
# old entries; the content digest alone would not notice a format change.
key_schema="v1"
bundle_dir="trace-fixture-bundle"
fixture_dir="tools/trace_replay/fixtures"
python_bin="${PYTHON:-python3}"

if ! command -v "${python_bin}" >/dev/null 2>&1 && command -v python >/dev/null 2>&1; then
  python_bin=python
fi

usage() {
  echo "usage: $0 <key|ensure|wait <key> [seconds]|install <trace-case>...>" >&2
  exit 2
}

# Writes "name=value" to $GITHUB_OUTPUT when running under Actions, and to stdout as well so
# the script stays runnable off-CI.
emit_output() {
  local name="$1"
  local value="$2"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    printf '%s=%s\n' "${name}" "${value}" >> "${GITHUB_OUTPUT}"
  fi
  printf '%s=%s\n' "${name}" "${value}"
}

# "<case><TAB><file>" for every fixture file of every CI case.
ci_fixture_manifest() {
  "${python_bin}" tools/trace_replay/trace_cases.py --ci --format fixture-manifest \
    --fixture-root "${fixture_dir}" | tr -d '\r'
}

is_lfs_fixture() {
  get_lfs_metadata "$1" >/dev/null 2>&1
}

# A fixture that Git stores directly must be a real file, not a pointer left by a wrong
# attribute.
check_plain_fixture() {
  local file="$1"
  if [ ! -s "${file}" ]; then
    echo "fixture file is missing or empty: ${file}" >&2
    return 1
  fi
  if head -n 1 "${file}" | grep -q "version https://git-lfs.github.com/spec/v1"; then
    echo "fixture should not be stored as an LFS pointer: ${file}" >&2
    return 1
  fi
}

# Puts the tracked pointer files back. The smudge filter is skipped so that a clone with Git
# LFS set up does not download the objects here.
restore_pointers() {
  GIT_LFS_SKIP_SMUDGE=1 git checkout -- "$@"
}

verify_lfs_fixture() {
  local on_disk="$1"
  local tracked="$2"
  local metadata expected_oid expected_size
  metadata="$(get_lfs_metadata "${tracked}")" || return 1
  read -r expected_oid expected_size <<< "${metadata}"
  verify_fixture_file "${on_disk}" "${tracked}" "${expected_oid}" "${expected_size}"
}

# Moves the given Git LFS fixtures of one case from the bundle directory into the fixture
# directory. Succeeds when every one of them is then in place and matches its pointer.
take_from_bundle() {
  local file bundled
  local incomplete=0
  for file in "$@"; do
    bundled="${bundle_dir}/$(basename "${file}")"
    # Two cases may share a file; the second finds it already in place.
    if [ -f "${bundled}" ]; then
      mv -f "${bundled}" "${file}"
    fi
    if ! verify_lfs_fixture "${file}" "${file}" 2>/dev/null; then
      incomplete=1
    fi
  done
  return "${incomplete}"
}

# Downloads the given Git LFS fixtures of one case over their pointers: the mirrors first,
# Git LFS for what no mirror serves. The fetch script verifies what it downloaded.
fetch_case_fixtures() {
  local case_name="$1"
  shift
  local file
  for file in "$@"; do
    rm -f "${file}" "${file}.tmp"
  done
  restore_pointers "$@" || return 1
  bash "${script_dir}/fetch-trace-fixture-lfs.sh" "${case_name}" "${fixture_dir}"
}

[ "$#" -ge 1 ] || usage
command_name="$1"
shift

case "${command_name}" in
  key)
    [ "$#" -eq 0 ] || usage
    # One "<basename> <oid> <size>" line per bundled file, sorted, each file once.
    manifest=""
    count=0
    while IFS= read -r file; do
      [ -n "${file}" ] || continue
      if ! metadata="$(get_lfs_metadata "${file}" 2>/dev/null)"; then
        continue
      fi
      read -r expected_oid expected_size <<< "${metadata}"
      manifest+="$(basename "${file}") ${expected_oid} ${expected_size}"$'\n'
      count=$((count + 1))
    done < <(ci_fixture_manifest | cut -f2 | LC_ALL=C sort -u)
    if [ "${count}" -eq 0 ]; then
      echo "no Git LFS fixture found for the CI trace cases; the bundle would be empty" >&2
      exit 1
    fi
    duplicates="$(printf '%s' "${manifest}" | cut -d' ' -f1 | LC_ALL=C sort | uniq -d)"
    if [ -n "${duplicates}" ]; then
      echo "two fixture files share a name, so the bundle directory cannot hold both: ${duplicates}" >&2
      exit 1
    fi
    digest="$(printf '%s' "${manifest}" | sha256sum | awk '{ print substr($1, 1, 16) }')"
    emit_output "key" "trace-fixture-bundle-${key_schema}-${digest}"
    emit_output "restore-prefix" "trace-fixture-bundle-${key_schema}-"
    emit_output "path" "${bundle_dir}"
    emit_output "files" "${count}"
    ;;

  ensure)
    [ "$#" -eq 0 ] || usage
    mkdir -p "${bundle_dir}" "${fixture_dir}"
    mapfile -t manifest_lines < <(ci_fixture_manifest)
    declare -A case_seen=()
    case_order=()
    for line in "${manifest_lines[@]}"; do
      case_name="${line%%$'\t'*}"
      if [ -z "${case_seen[${case_name}]:-}" ]; then
        case_seen["${case_name}"]=1
        case_order+=("${case_name}")
      fi
    done

    fetched=0
    reused=0
    bundled_files=()
    for case_name in "${case_order[@]}"; do
      lfs_files=()
      for line in "${manifest_lines[@]}"; do
        [ "${line%%$'\t'*}" = "${case_name}" ] || continue
        file="${line#*$'\t'}"
        if is_lfs_fixture "${file}"; then
          lfs_files+=("${file}")
        else
          check_plain_fixture "${file}"
        fi
      done
      [ "${#lfs_files[@]}" -gt 0 ] || continue

      # Take what an older bundle already holds, and fetch the case if that is not all of it.
      if take_from_bundle "${lfs_files[@]}"; then
        reused=$((reused + 1))
      else
        echo "Fetching trace fixtures of ${case_name}: the bundle does not hold them"
        fetch_case_fixtures "${case_name}" "${lfs_files[@]}"
        fetched=$((fetched + 1))
      fi
      bundled_files+=("${lfs_files[@]}")
    done

    # The bundle is rebuilt from the verified files, so nothing an older bundle carried and
    # no case names any more is saved again.
    rm -rf "${bundle_dir}"
    mkdir -p "${bundle_dir}"
    declare -A file_seen=()
    saved=0
    for file in "${bundled_files[@]}"; do
      [ -z "${file_seen[${file}]:-}" ] || continue
      file_seen["${file}"]=1
      verify_lfs_fixture "${file}" "${file}"
      mv -f "${file}" "${bundle_dir}/$(basename "${file}")"
      saved=$((saved + 1))
    done
    if [ "${saved}" -eq 0 ]; then
      echo "the trace fixture bundle came out empty" >&2
      exit 1
    fi
    # Leave the working tree as a checkout has it: pointers in the fixture directory.
    restore_pointers "${!file_seen[@]}"
    echo "Trace fixture bundle: ${saved} file(s), ${reused} case(s) from the restored bundle, ${fetched} case(s) fetched."
    emit_output "fetched" "${fetched}"
    ;;

  wait)
    [ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
    key="$1"
    limit="${2:-300}"
    # Another workflow of the same push may be the one saving this key. The replay jobs
    # restore the entry, so it has to be listed before this job ends. The caller checks the
    # entry itself afterwards; this only waits, and never fails.
    #
    # The listing is narrowed to the ref of this run. The key is content-addressed, so another
    # branch may already hold an entry under it, and that entry cannot be restored from here:
    # counting it would end the wait while the entry of this ref is still being uploaded.
    query=(-f "key=${key}" -f per_page=100)
    if [ -n "${GITHUB_REF:-}" ]; then
      query+=(-f "ref=${GITHUB_REF}")
    fi
    waited=0
    refused=0
    while :; do
      if listed="$(gh api -X GET "repos/${GITHUB_REPOSITORY}/actions/caches" "${query[@]}" \
            --jq '[.actions_caches[] | select(.key == "'"${key}"'")] | length' 2>/dev/null)"; then
        if [ "${listed:-0}" -ge 1 ]; then
          echo "cache entry ${key} is listed after ${waited} s"
          break
        fi
      else
        refused=$((refused + 1))
        if [ "${refused}" -ge 3 ]; then
          echo "the cache listing cannot be read (no token, or no actions: read permission); not waiting"
          break
        fi
      fi
      if [ "${waited}" -ge "${limit}" ]; then
        echo "cache entry ${key} is not listed after ${waited} s"
        break
      fi
      sleep 10
      waited=$((waited + 10))
    done
    ;;

  install)
    [ "$#" -ge 1 ] || usage
    installed=0
    fetched=0
    for case_name in "$@"; do
      mapfile -t files < <(trace_fixture_files "${case_name}" "${fixture_dir}" "${python_bin}")
      if [ "${#files[@]}" -eq 0 ]; then
        echo "no fixture files declared for trace case: ${case_name}" >&2
        exit 1
      fi
      lfs_files=()
      for file in "${files[@]}"; do
        if is_lfs_fixture "${file}"; then
          lfs_files+=("${file}")
        else
          check_plain_fixture "${file}"
        fi
      done
      [ "${#lfs_files[@]}" -gt 0 ] || continue

      # A bundle that was not restored (the cache entry was evicted, or its download failed)
      # or that lacks a file costs this job a download of the case, not the replay.
      if ! take_from_bundle "${lfs_files[@]}"; then
        echo "::warning::the restored fixture bundle does not hold the trace fixtures of ${case_name}; fetching them"
        if ! fetch_case_fixtures "${case_name}" "${lfs_files[@]}"; then
          echo "::error::the trace fixtures of ${case_name} are not in the restored bundle and could not be fetched"
          exit 1
        fi
        fetched=$((fetched + 1))
        for file in "${lfs_files[@]}"; do
          if ! verify_lfs_fixture "${file}" "${file}"; then
            echo "::error::trace fixture ${file} of ${case_name} does not match its Git LFS pointer after the fetch"
            exit 1
          fi
        done
      fi
      installed=$((installed + ${#lfs_files[@]}))
    done
    # The rest of the bundle belongs to other shards.
    rm -rf "${bundle_dir}"
    echo "Installed and verified ${installed} Git LFS fixture file(s) for $# trace case(s), ${fetched} case(s) fetched."
    ;;

  *)
    usage
    ;;
esac
