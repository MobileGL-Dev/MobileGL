#!/bin/sh
set -eu

usage() {
  cat <<'EOF'
Usage:
  sh android-plugin/run-trace-replay-cases-ci.sh \
    --apk-file FILE \
    --package PACKAGE \
    --backend BACKEND \
    --cases-file FILE \
    --result-root DIR \
    --fixture-root DIR
EOF
}

die() {
  echo "run-trace-replay-cases-ci.sh: $*" >&2
  usage >&2
  exit 2
}

next_arg() {
  if [ "$#" -lt 2 ]; then
    die "$1 requires a value"
  fi
  printf '%s' "$2"
}

require_value() {
  value="$1"
  name="$2"
  if [ -z "${value}" ]; then
    die "missing ${name}"
  fi
}

apk_file=""
package_name=""
backend=""
cases_file=""
result_root=""
fixture_root=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --apk-file) apk_file="$(next_arg "$@")"; shift 2 ;;
    --package) package_name="$(next_arg "$@")"; shift 2 ;;
    --backend) backend="$(next_arg "$@")"; shift 2 ;;
    --cases-file) cases_file="$(next_arg "$@")"; shift 2 ;;
    --result-root) result_root="$(next_arg "$@")"; shift 2 ;;
    --fixture-root) fixture_root="$(next_arg "$@")"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

require_value "${apk_file}" "--apk-file"
require_value "${package_name}" "--package"
require_value "${backend}" "--backend"
require_value "${cases_file}" "--cases-file"
require_value "${result_root}" "--result-root"
require_value "${fixture_root}" "--fixture-root"

status=0
mkdir -p "${result_root}/status"

if [ ! -f "${apk_file}" ]; then
  echo "APK does not exist: ${apk_file}" >&2
  echo 1 > "${result_root}/status/${backend}.status"
  exit 0
fi

if [ ! -f "${cases_file}" ]; then
  echo "cases file does not exist: ${cases_file}" >&2
  echo 1 > "${result_root}/status/${backend}.status"
  exit 0
fi

while IFS='|' read -r case_name trace_archive trace_file golden target_call width height tolerance crop_x crop_y crop_width crop_height fuzz_percent timeout_seconds; do
  [ -n "${case_name}" ] || continue
  sh android-plugin/trace-replay-ci.sh \
    --apk-file "${apk_file}" \
    --package "${package_name}" \
    --backend "${backend}" \
    --result-root "${result_root}" \
    --fixture-root "${fixture_root}" \
    --case "${case_name}" \
    --trace-archive "${trace_archive}" \
    --trace-file "${trace_file}" \
    --golden "${golden}" \
    --target-call "${target_call}" \
    --width "${width}" \
    --height "${height}" \
    --tolerance "${tolerance}" \
    --crop-x "${crop_x}" \
    --crop-y "${crop_y}" \
    --crop-width "${crop_width}" \
    --crop-height "${crop_height}" \
    --fuzz-percent "${fuzz_percent}" \
    --timeout-seconds "${timeout_seconds}" \
    || status=1
done < "${cases_file}"

echo "${status}" > "${result_root}/status/${backend}.status"
