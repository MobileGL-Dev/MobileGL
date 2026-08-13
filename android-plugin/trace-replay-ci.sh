#!/bin/sh
set -eu

ADB="${ADB:-adb}"
PYTHON="${PYTHON:-python3}"
INFRASTRUCTURE_FAILURE_EXIT_CODE=75

usage() {
  cat <<'EOF'
Usage:
  sh android-plugin/trace-replay-ci.sh \
    --apk-file FILE \
    --package PACKAGE \
    --backend BACKEND \
    --result-root DIR \
    --fixture-root DIR \
    --case NAME \
    --trace-archive FILE \
    --trace-file FILE_IN_ARCHIVE \
    --golden FILE \
    [--alternate-golden FILE] \
    --target-call N \
    --width N \
    --height N \
    --ssim-threshold N \
    --crop-x N \
    --crop-y N \
    --crop-width N \
    --crop-height N \
    [--use-pbuffer] \
    [--avoid-angle-llvmpipe-sampler-mipmap-min-filter] \
    [--avoid-angle-llvmpipe-explicit-lod-bias] \
    [--coherent-as-flush] \
    --timeout-seconds N

Set MOBILEGL_USE_ANGLE=1 to run DirectGLES replay with packaged ANGLE
instead of the device system GLES driver.
Set MOBILEGL_TRACE_ANGLE_VARIANT to the packaged ANGLE short hash used by
DirectGLES replay.
Set MOBILEGL_RETRACE_USE_PBUFFER=1 or pass --use-pbuffer to run DirectGLES
against an offscreen EGL pbuffer instead of the Activity surface.
Pass --avoid-angle-llvmpipe-sampler-mipmap-min-filter for DirectGLES traces that
need ANGLE llvmpipe sampler mipmap filters downgraded to avoid driver stalls.
Pass --avoid-angle-llvmpipe-explicit-lod-bias for DirectGLES traces whose shaders
sample with an explicit LOD that ANGLE llvmpipe cannot take a LOD bias on
(MOBILEGL_AVOID_EXPLICIT_LOD_BIAS=1).
Pass --coherent-as-flush for traces whose engine writes persistent
GL_MAP_FLUSH_EXPLICIT_BIT maps it never flushes (MOBILEGL_COHERENT_AS_FLUSH=1).
EOF
}

die() {
  echo "trace-replay-ci.sh: $*" >&2
  usage >&2
  exit 2
}

host_path_for_adb() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$1"
  else
    printf '%s' "$1"
  fi
}

adb_device_path() {
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "${ADB}" "$@"
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
result_root=""
fixture_root=""
case_name=""
trace_archive=""
trace_file=""
golden_path=""
alternate_golden_path=""
target_call=""
width=""
height=""
ssim_threshold=""
crop_x=""
crop_y=""
crop_width=""
crop_height=""
use_pbuffer=0
avoid_angle_llvmpipe_sampler_mipmap_min_filter=0
avoid_angle_llvmpipe_explicit_lod_bias=0
coherent_as_flush=0
timeout_seconds=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --apk-file) apk_file="$(next_arg "$@")"; shift 2 ;;
    --package) package_name="$(next_arg "$@")"; shift 2 ;;
    --backend) backend="$(next_arg "$@")"; shift 2 ;;
    --result-root) result_root="$(next_arg "$@")"; shift 2 ;;
    --fixture-root) fixture_root="$(next_arg "$@")"; shift 2 ;;
    --case) case_name="$(next_arg "$@")"; shift 2 ;;
    --trace-archive) trace_archive="$(next_arg "$@")"; shift 2 ;;
    --trace-file) trace_file="$(next_arg "$@")"; shift 2 ;;
    --golden) golden_path="$(next_arg "$@")"; shift 2 ;;
    --alternate-golden)
      if [ "$#" -lt 2 ] || [ "${2#--}" != "$2" ]; then
        alternate_golden_path=""
        shift 1
      else
        alternate_golden_path="$2"
        shift 2
      fi
      ;;
    --target-call) target_call="$(next_arg "$@")"; shift 2 ;;
    --width) width="$(next_arg "$@")"; shift 2 ;;
    --height) height="$(next_arg "$@")"; shift 2 ;;
    --ssim-threshold) ssim_threshold="$(next_arg "$@")"; shift 2 ;;
    --crop-x) crop_x="$(next_arg "$@")"; shift 2 ;;
    --crop-y) crop_y="$(next_arg "$@")"; shift 2 ;;
    --crop-width) crop_width="$(next_arg "$@")"; shift 2 ;;
    --crop-height) crop_height="$(next_arg "$@")"; shift 2 ;;
    --use-pbuffer) use_pbuffer=1; shift 1 ;;
    --avoid-angle-llvmpipe-sampler-mipmap-min-filter)
      avoid_angle_llvmpipe_sampler_mipmap_min_filter=1
      shift 1
      ;;
    --avoid-angle-llvmpipe-explicit-lod-bias)
      avoid_angle_llvmpipe_explicit_lod_bias=1
      shift 1
      ;;
    --coherent-as-flush) coherent_as_flush=1; shift 1 ;;
    --timeout-seconds) timeout_seconds="$(next_arg "$@")"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

require_value "${apk_file}" "--apk-file"
require_value "${package_name}" "--package"
require_value "${backend}" "--backend"
require_value "${result_root}" "--result-root"
require_value "${fixture_root}" "--fixture-root"
require_value "${case_name}" "--case"
require_value "${trace_archive}" "--trace-archive"
require_value "${trace_file}" "--trace-file"
require_value "${golden_path}" "--golden"
require_value "${target_call}" "--target-call"
require_value "${width}" "--width"
require_value "${height}" "--height"
require_value "${ssim_threshold}" "--ssim-threshold"
require_value "${crop_x}" "--crop-x"
require_value "${crop_y}" "--crop-y"
require_value "${crop_width}" "--crop-width"
require_value "${crop_height}" "--crop-height"
require_value "${timeout_seconds}" "--timeout-seconds"

test -f "${apk_file}" || die "APK does not exist: ${apk_file}"
test -f "${trace_archive}" || die "trace archive does not exist: ${trace_archive}"
test -f "${golden_path}" || die "golden image does not exist: ${golden_path}"
if [ -n "${alternate_golden_path}" ]; then
  test -f "${alternate_golden_path}" || die "alternate golden image does not exist: ${alternate_golden_path}"
fi
safe_case="$(printf '%s' "${case_name}" | sed 's/[^A-Za-z0-9._-]/_/g')"
app_dir="/data/user/0/${package_name}/files/trace-replay"

collect_run_diagnostics() {
  diagnostics_dir="$1"
  mkdir -p "${diagnostics_dir}"
  adb_state="$(adb_device_path get-state 2>/dev/null | tr -d '\r' || true)"
  printf '%s\n' "${adb_state}" > "${diagnostics_dir}/adb-state.txt"
  : > "${diagnostics_dir}/logcat.txt"
  if [ "${adb_state}" != "device" ]; then
    return
  fi
  # Depth matters, not just content: is_infrastructure_failure below decides "the emulator
  # broke, retry" by finding a system_server crash in this window, and MobileGL logs into the
  # same buffer under its own tag. A tail that is too short lets routine MobileGL output evict
  # the crash line and charges an infrastructure fault to the trace under test.
  "${ADB}" logcat -d -t 20000 > "${diagnostics_dir}/logcat.txt" || true
  adb_device_path shell pidof "${package_name}" > "${diagnostics_dir}/pidof.txt" 2>&1 || true
  adb_device_path shell dumpsys activity activities > "${diagnostics_dir}/activity.txt" 2>&1 || true
  adb_device_path shell run-as "${package_name}" ls -laR "${app_dir}" > "${diagnostics_dir}/app-files.txt" 2>&1 || true
  adb_device_path exec-out run-as "${package_name}" cat "${app_dir}/output/retrace.log" > "${diagnostics_dir}/retrace.log" || true
  adb_device_path exec-out run-as "${package_name}" cat "${app_dir}/output/mobilegl.log" > "${diagnostics_dir}/mobilegl.log" || true
}

# Records why a retrace was charged to the infrastructure rather than the code
# under test, so the workflow can count the classes it retried.
record_infrastructure_reason() {
  printf '%s\n' "$1" >> "${result_root}/infrastructure-failure-reason.txt"
}

# True when the replay never got a usable window surface out of ANGLE. EGL
# 0x300b is EGL_BAD_NATIVE_WINDOW and -1000000001 is VK_ERROR_SURFACE_LOST_KHR,
# which ANGLE reports out of vkCreateAndroidSurfaceKHR when the Activity's
# native window is not usable. Observed intermittently on cases that pass in
# every other run, so it is an environment fault, not a property of a trace.
is_angle_surface_lost() {
  diagnostics_dir="$1"
  retrace_log="${diagnostics_dir}/retrace.log"
  mobilegl_log="${diagnostics_dir}/mobilegl.log"

  if [ ! -s "${retrace_log}" ]; then
    return 1
  fi
  if ! grep -Eq 'EGL surface creation failed: 0x300b|Vulkan error -1000000001|VK_ERROR_SURFACE_LOST_KHR' \
       "${retrace_log}"; then
    return 1
  fi
  # Co-signature, and the reason this cannot swallow a real regression: a lost
  # surface at startup stops MobileGL at init, before it ever runs the capability
  # probe. If the probe ran, the replay had a working context and lost it later -
  # that is a genuine defect and must stay a failure.
  if [ -s "${mobilegl_log}" ] && grep -q 'OpenGL ES capabilities:' "${mobilegl_log}"; then
    return 1
  fi
  return 0
}

is_infrastructure_failure() {
  diagnostics_dir="$1"
  adb_state="$(cat "${diagnostics_dir}/adb-state.txt" 2>/dev/null || true)"
  if [ "${adb_state}" != "device" ]; then
    echo "trace-replay-ci.sh: Android device is unavailable (state: ${adb_state:-unknown})" >&2
    record_infrastructure_reason "device-unavailable"
    return 0
  fi
  if grep -Eq 'Fatal signal [0-9]+.*[(]system_server[)]|F system_server[ :]' "${diagnostics_dir}/logcat.txt"; then
    echo "trace-replay-ci.sh: Android system_server crashed during retrace" >&2
    record_infrastructure_reason "system-server-crash"
    return 0
  fi
  if is_angle_surface_lost "${diagnostics_dir}"; then
    echo "trace-replay-ci.sh: ANGLE could not create its window surface (EGL_BAD_NATIVE_WINDOW / VK_ERROR_SURFACE_LOST_KHR) before MobileGL finished init" >&2
    record_infrastructure_reason "angle-surface-lost"
    return 0
  fi
  return 1
}

copy_app_artifact() {
  source_path="$1"
  destination_path="$2"
  if ! adb_device_path exec-out run-as "${package_name}" cat "${source_path}" > "${destination_path}"; then
    echo "trace-replay-ci.sh: warning: failed to copy ${source_path}" >&2
    rm -f "${destination_path}"
  fi
}

prepare_fixture() {
  fixture_dir="${fixture_root}/${safe_case}"
  rm -rf "${fixture_dir}"
  mkdir -p "${fixture_dir}"
  tar -xzf "${trace_archive}" -C "${fixture_dir}"

  extracted_trace="${fixture_dir}/${trace_file}"
  test -f "${extracted_trace}" || die "trace file not found in archive: ${trace_file}"

  adb_device_path push "$(host_path_for_adb "${extracted_trace}")" "/data/local/tmp/mobilegl-${safe_case}.trace"
  adb_device_path push "$(host_path_for_adb "${golden_path}")" "/data/local/tmp/mobilegl-${safe_case}.golden.png"
  if [ -n "${alternate_golden_path}" ]; then
    adb_device_path push "$(host_path_for_adb "${alternate_golden_path}")" "/data/local/tmp/mobilegl-${safe_case}.alternate-golden.png"
  fi
  adb_device_path shell chmod 0644 "/data/local/tmp/mobilegl-${safe_case}.trace" "/data/local/tmp/mobilegl-${safe_case}.golden.png"
  if [ -n "${alternate_golden_path}" ]; then
    adb_device_path shell chmod 0644 "/data/local/tmp/mobilegl-${safe_case}.alternate-golden.png"
  fi
}

copy_fixture_to_app() {
  trace_tmp="/data/local/tmp/mobilegl-${safe_case}.trace"
  golden_tmp="/data/local/tmp/mobilegl-${safe_case}.golden.png"
  alternate_golden_tmp="/data/local/tmp/mobilegl-${safe_case}.alternate-golden.png"

  adb_device_path shell run-as "${package_name}" rm -rf "${app_dir}"
  adb_device_path shell run-as "${package_name}" mkdir -p "${app_dir}/input" "${app_dir}/output"
  adb_device_path shell run-as "${package_name}" cp "${trace_tmp}" "${app_dir}/input/trace.trace"
  adb_device_path shell run-as "${package_name}" cp "${golden_tmp}" "${app_dir}/input/golden.png"
  if [ -n "${alternate_golden_path}" ]; then
    adb_device_path shell run-as "${package_name}" cp "${alternate_golden_tmp}" "${app_dir}/input/alternate-golden.png"
  fi
}

run_retrace() {
  result_dir="${result_root}/${safe_case}-${backend}"
  alternate_golden_app_path=""
  use_angle=0
  if [ -n "${alternate_golden_path}" ]; then
    alternate_golden_app_path="${app_dir}/input/alternate-golden.png"
  fi
  if [ "${MOBILEGL_USE_ANGLE:-}" = "1" ] && [ "${backend}" = "DirectGLES" ]; then
    use_angle=1
    test -n "${MOBILEGL_TRACE_ANGLE_VARIANT:-}" || die "MOBILEGL_TRACE_ANGLE_VARIANT is required for DirectGLES ANGLE replay"
  fi
  if [ "${MOBILEGL_RETRACE_USE_PBUFFER:-}" = "1" ] && [ "${backend}" = "DirectGLES" ]; then
    use_pbuffer=1
  fi

  mkdir -p "${result_dir}"
  "${ADB}" install -r "$(host_path_for_adb "${apk_file}")"
  copy_fixture_to_app
  adb_device_path shell am force-stop "${package_name}"
  "${ADB}" logcat -c
  set -- am start -a top.mobilegl.plugin.TRACE_REPLAY \
    -n "${package_name}/top.mobilegl.plugin.trace.TraceReplayActivity" \
    --es trace_path "${app_dir}/input/trace.trace" \
    --es golden_path "${app_dir}/input/golden.png"
  if [ -n "${alternate_golden_app_path}" ]; then
    set -- "$@" --es alternate_golden_path "${alternate_golden_app_path}"
  fi
  if [ "${use_angle}" -eq 1 ]; then
    set -- "$@" --ez use_angle true
    set -- "$@" --es angle_variant "${MOBILEGL_TRACE_ANGLE_VARIANT}"
  fi
  if [ "${use_pbuffer}" -eq 1 ] && [ "${backend}" = "DirectGLES" ]; then
    set -- "$@" --ez use_pbuffer true
  fi
  if [ "${avoid_angle_llvmpipe_sampler_mipmap_min_filter}" -eq 1 ] && [ "${backend}" = "DirectGLES" ]; then
    set -- "$@" --ez avoid_angle_llvmpipe_sampler_mipmap_min_filter true
  fi
  if [ "${avoid_angle_llvmpipe_explicit_lod_bias}" -eq 1 ] && [ "${backend}" = "DirectGLES" ]; then
    set -- "$@" --ez avoid_angle_llvmpipe_explicit_lod_bias true
  fi
  if [ "${coherent_as_flush}" -eq 1 ]; then
    set -- "$@" --ez coherent_as_flush true
  fi
  set -- "$@" \
    --es output_dir "${app_dir}/output" \
    --es diff_path "${app_dir}/output/${safe_case}-diff.png" \
    --es backend "${backend}" \
    --el target_call "${target_call}" \
    --ei width "${width}" \
    --ei height "${height}" \
    --es ssim_threshold "${ssim_threshold}" \
    --ei crop_x "${crop_x}" \
    --ei crop_y "${crop_y}" \
    --ei crop_width "${crop_width}" \
    --ei crop_height "${crop_height}"
  adb_device_path shell "$@"

  app_exited=0
  saw_app_process=0
  for _ in $(seq 1 "${timeout_seconds}"); do
    if adb_device_path shell run-as "${package_name}" ls "${app_dir}/output/result.json" >/dev/null 2>&1; then
      break
    fi
    if adb_device_path shell pidof "${package_name}" >/dev/null 2>&1; then
      saw_app_process=1
    elif [ "${saw_app_process}" -eq 1 ]; then
      app_exited=1
      break
    fi
    sleep 1
  done

  collect_run_diagnostics "${result_dir}"
  if ! adb_device_path shell run-as "${package_name}" ls "${app_dir}/output/result.json" >/dev/null 2>&1; then
    if [ "${app_exited}" -eq 1 ]; then
      echo "trace-replay-ci.sh: app process exited before result.json was created" >&2
    else
      echo "trace-replay-ci.sh: result.json was not created after ${timeout_seconds}s" >&2
    fi
    if [ -s "${result_dir}/retrace.log" ]; then
      echo "trace-replay-ci.sh: retrace.log:" >&2
      cat "${result_dir}/retrace.log" >&2
    fi
    if [ -s "${result_dir}/mobilegl.log" ]; then
      echo "trace-replay-ci.sh: tail of mobilegl.log:" >&2
      tail -200 "${result_dir}/mobilegl.log" >&2
    fi
    echo "trace-replay-ci.sh: recent relevant logcat lines:" >&2
    grep -E 'MobileGLTraceRunner|AndroidRuntime|FATAL EXCEPTION|trace_replay|MobileGL|libc' "${result_dir}/logcat.txt" | tail -200 >&2 || true
    echo "trace-replay-ci.sh: app trace-replay files:" >&2
    cat "${result_dir}/app-files.txt" >&2 || true
    if is_infrastructure_failure "${result_dir}"; then
      echo "trace-replay-ci.sh: requesting one infrastructure retry" >&2
      exit "${INFRASTRUCTURE_FAILURE_EXIT_CODE}"
    fi
    exit 1
  fi
  adb_device_path exec-out run-as "${package_name}" cat "${app_dir}/output/result.json" > "${result_dir}/result.json"
  cat "${result_dir}/result.json"
  copy_app_artifact "${app_dir}/output/actual.png" "${result_dir}/${safe_case}-${backend}-actual.png"
  copy_app_artifact "${app_dir}/output/${safe_case}-diff.png" "${result_dir}/${safe_case}-${backend}-diff.png"
  copy_app_artifact "${app_dir}/output/retrace.log" "${result_dir}/retrace.log"
  copy_app_artifact "${app_dir}/output/mobilegl.log" "${result_dir}/mobilegl.log"

  # A replay that wrote result.json but did not pass used to print nothing but
  # the JSON, which for a non-zero statusCode says only "retrace failed with
  # status N". The logs that say why are already on disk here, so echo their
  # tails the same way the missing-result.json path does; the job log is the one
  # place a failure stays readable after the result artifact expires.
  if ! grep -q '"passed"[[:space:]]*:[[:space:]]*true' "${result_dir}/result.json"; then
    if [ -s "${result_dir}/retrace.log" ]; then
      echo "trace-replay-ci.sh: tail of retrace.log:" >&2
      tail -200 "${result_dir}/retrace.log" >&2
    fi
    if [ -s "${result_dir}/mobilegl.log" ]; then
      echo "trace-replay-ci.sh: tail of mobilegl.log:" >&2
      tail -200 "${result_dir}/mobilegl.log" >&2
    fi
    # A replay that writes result.json still reaches here on an environment
    # fault: ANGLE failing to make a window surface reports statusCode 5 rather
    # than dying, so it never hit the missing-result.json branch above and used
    # to be charged to the trace.
    if is_infrastructure_failure "${result_dir}"; then
      echo "trace-replay-ci.sh: requesting one infrastructure retry" >&2
      exit "${INFRASTRUCTURE_FAILURE_EXIT_CODE}"
    fi
  fi

  "${PYTHON}" -c 'import json, sys; result = json.load(open(sys.argv[1], encoding="utf-8")); sys.exit(0 if result.get("passed") else f"trace replay failed: {result}")' "${result_dir}/result.json"
}

mkdir -p "${fixture_root}" "${result_root}"
prepare_fixture

run_retrace
