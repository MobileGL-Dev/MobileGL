#!/bin/sh
set -eu

usage() {
  cat <<'EOF'
Usage:
  sh android-plugin/trace-replay-ci.sh \
    --apk-dir DIR \
    --result-root DIR \
    --fixture-root DIR \
    --case NAME \
    --trace-archive FILE \
    --trace-file FILE_IN_ARCHIVE \
    --golden FILE \
    --target-call N \
    --width N \
    --height N \
    --tolerance N \
    --crop-x N \
    --crop-y N \
    --crop-width N \
    --crop-height N \
    --fuzz-percent N \
    --timeout-seconds N \
    --espryt-package PACKAGE \
    --espryt-apk-name APK_NAME \
    --espryt-backend BACKEND \
    --magma-package PACKAGE \
    --magma-apk-name APK_NAME \
    --magma-backend BACKEND
EOF
}

die() {
  echo "trace-replay-ci.sh: $*" >&2
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

apk_dir=""
result_root=""
fixture_root=""
case_name=""
trace_archive=""
trace_file=""
golden_path=""
target_call=""
width=""
height=""
tolerance=""
crop_x=""
crop_y=""
crop_width=""
crop_height=""
fuzz_percent=""
timeout_seconds=""
espryt_package=""
espryt_apk_name=""
espryt_backend=""
magma_package=""
magma_apk_name=""
magma_backend=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --apk-dir) apk_dir="$(next_arg "$@")"; shift 2 ;;
    --result-root) result_root="$(next_arg "$@")"; shift 2 ;;
    --fixture-root) fixture_root="$(next_arg "$@")"; shift 2 ;;
    --case) case_name="$(next_arg "$@")"; shift 2 ;;
    --trace-archive) trace_archive="$(next_arg "$@")"; shift 2 ;;
    --trace-file) trace_file="$(next_arg "$@")"; shift 2 ;;
    --golden) golden_path="$(next_arg "$@")"; shift 2 ;;
    --target-call) target_call="$(next_arg "$@")"; shift 2 ;;
    --width) width="$(next_arg "$@")"; shift 2 ;;
    --height) height="$(next_arg "$@")"; shift 2 ;;
    --tolerance) tolerance="$(next_arg "$@")"; shift 2 ;;
    --crop-x) crop_x="$(next_arg "$@")"; shift 2 ;;
    --crop-y) crop_y="$(next_arg "$@")"; shift 2 ;;
    --crop-width) crop_width="$(next_arg "$@")"; shift 2 ;;
    --crop-height) crop_height="$(next_arg "$@")"; shift 2 ;;
    --fuzz-percent) fuzz_percent="$(next_arg "$@")"; shift 2 ;;
    --timeout-seconds) timeout_seconds="$(next_arg "$@")"; shift 2 ;;
    --espryt-package) espryt_package="$(next_arg "$@")"; shift 2 ;;
    --espryt-apk-name) espryt_apk_name="$(next_arg "$@")"; shift 2 ;;
    --espryt-backend) espryt_backend="$(next_arg "$@")"; shift 2 ;;
    --magma-package) magma_package="$(next_arg "$@")"; shift 2 ;;
    --magma-apk-name) magma_apk_name="$(next_arg "$@")"; shift 2 ;;
    --magma-backend) magma_backend="$(next_arg "$@")"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

require_value "${apk_dir}" "--apk-dir"
require_value "${result_root}" "--result-root"
require_value "${fixture_root}" "--fixture-root"
require_value "${case_name}" "--case"
require_value "${trace_archive}" "--trace-archive"
require_value "${trace_file}" "--trace-file"
require_value "${golden_path}" "--golden"
require_value "${target_call}" "--target-call"
require_value "${width}" "--width"
require_value "${height}" "--height"
require_value "${tolerance}" "--tolerance"
require_value "${crop_x}" "--crop-x"
require_value "${crop_y}" "--crop-y"
require_value "${crop_width}" "--crop-width"
require_value "${crop_height}" "--crop-height"
require_value "${fuzz_percent}" "--fuzz-percent"
require_value "${timeout_seconds}" "--timeout-seconds"
require_value "${espryt_package}" "--espryt-package"
require_value "${espryt_apk_name}" "--espryt-apk-name"
require_value "${espryt_backend}" "--espryt-backend"
require_value "${magma_package}" "--magma-package"
require_value "${magma_apk_name}" "--magma-apk-name"
require_value "${magma_backend}" "--magma-backend"

test -f "${trace_archive}" || die "trace archive does not exist: ${trace_archive}"
test -f "${golden_path}" || die "golden image does not exist: ${golden_path}"

safe_case="$(printf '%s' "${case_name}" | sed 's/[^A-Za-z0-9._-]/_/g')"

quote_android_path() {
  printf '%s' "$1" | sed "s/'/'\\\\''/g"
}

find_apk() {
  apk_name="$1"
  apk_path="$(find "${apk_dir}" -name "${apk_name}" -print -quit)"
  test -n "${apk_path}" || die "APK not found under ${apk_dir}: ${apk_name}"
  printf '%s' "${apk_path}"
}

prepare_fixture() {
  fixture_dir="${fixture_root}/${safe_case}"
  rm -rf "${fixture_dir}"
  mkdir -p "${fixture_dir}"
  tar -xzf "${trace_archive}" -C "${fixture_dir}"

  extracted_trace="${fixture_dir}/${trace_file}"
  test -f "${extracted_trace}" || die "trace file not found in archive: ${trace_file}"

  adb push "${extracted_trace}" "/data/local/tmp/mobilegl-${safe_case}.trace"
  adb push "${golden_path}" "/data/local/tmp/mobilegl-${safe_case}.golden.png"
  adb shell chmod 0644 "/data/local/tmp/mobilegl-${safe_case}.trace" "/data/local/tmp/mobilegl-${safe_case}.golden.png"
}

copy_fixture_to_app() {
  package_name="$1"
  trace_tmp="/data/local/tmp/mobilegl-${safe_case}.trace"
  golden_tmp="/data/local/tmp/mobilegl-${safe_case}.golden.png"
  escaped_trace_tmp="$(quote_android_path "${trace_tmp}")"
  escaped_golden_tmp="$(quote_android_path "${golden_tmp}")"

  adb shell run-as "${package_name}" rm -rf files/trace-replay
  adb shell run-as "${package_name}" mkdir -p files/trace-replay/input files/trace-replay/output
  adb shell run-as "${package_name}" sh -c "cat '${escaped_trace_tmp}' > files/trace-replay/input/trace.trace"
  adb shell run-as "${package_name}" sh -c "cat '${escaped_golden_tmp}' > files/trace-replay/input/golden.png"
}

run_retrace() {
  package_name="$1"
  apk_path="$2"
  backend="$3"
  app_dir="/data/user/0/${package_name}/files/trace-replay"
  result_dir="${result_root}/${safe_case}-${backend}"

  mkdir -p "${result_dir}"
  adb install -r "${apk_path}"
  copy_fixture_to_app "${package_name}"
  adb shell am force-stop "${package_name}"
  adb logcat -c
  adb shell am start -W -a top.mobilegl.plugin.TRACE_REPLAY \
    -n "${package_name}/top.mobilegl.plugin.trace.TraceReplayActivity" \
    --es trace_path "${app_dir}/input/trace.trace" \
    --es golden_path "${app_dir}/input/golden.png" \
    --es output_dir "${app_dir}/output" \
    --es diff_path "${app_dir}/output/${safe_case}-diff.png" \
    --es backend "${backend}" \
    --el target_call "${target_call}" \
    --ei width "${width}" \
    --ei height "${height}" \
    --ei tolerance "${tolerance}" \
    --ei crop_x "${crop_x}" \
    --ei crop_y "${crop_y}" \
    --ei crop_width "${crop_width}" \
    --ei crop_height "${crop_height}" \
    --ei fuzz_percent "${fuzz_percent}"

  for _ in $(seq 1 "${timeout_seconds}"); do
    if adb shell run-as "${package_name}" ls files/trace-replay/output/result.json >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done

  adb logcat -d -t 1000 > "${result_dir}/logcat.txt" || true
  adb shell run-as "${package_name}" ls files/trace-replay/output/result.json
  adb exec-out run-as "${package_name}" cat files/trace-replay/output/result.json > "${result_dir}/result.json"
  cat "${result_dir}/result.json"
  adb exec-out run-as "${package_name}" cat files/trace-replay/output/actual.png > "${result_dir}/${safe_case}-${backend}-actual.png"
  adb exec-out run-as "${package_name}" cat "files/trace-replay/output/${safe_case}-diff.png" > "${result_dir}/${safe_case}-${backend}-diff.png"
  adb exec-out run-as "${package_name}" cat files/trace-replay/output/retrace.log > "${result_dir}/retrace.log" || true

  python3 -c 'import json, sys; result = json.load(open(sys.argv[1], encoding="utf-8")); sys.exit(0 if result.get("passed") else f"trace replay failed: {result}")' "${result_dir}/result.json"
}

mkdir -p "${fixture_root}" "${result_root}"
prepare_fixture

run_retrace "${espryt_package}" "$(find_apk "${espryt_apk_name}")" "${espryt_backend}"
run_retrace "${magma_package}" "$(find_apk "${magma_apk_name}")" "${magma_backend}"
