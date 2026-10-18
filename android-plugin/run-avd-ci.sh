#!/bin/sh
set -eu

usage() {
  cat <<'EOF'
Usage:
  sh android-plugin/run-avd-ci.sh create \
    --api-level N \
    --target TARGET \
    --arch ARCH \
    --profile PROFILE \
    --avd-name NAME

  sh android-plugin/run-avd-ci.sh start \
    --avd-name NAME \
    --gpu GPU \
    --emulator-log FILE \
    --pid-file FILE \
    --boot-timeout SECONDS \
    [--snapshot NAME]

  sh android-plugin/run-avd-ci.sh snapshot \
    --avd-name NAME \
    --gpu GPU \
    --name SNAPSHOT \
    --emulator-log FILE \
    --pid-file FILE \
    --boot-timeout SECONDS

  sh android-plugin/run-avd-ci.sh stop \
    --avd-name NAME \
    --emulator-log FILE \
    --pid-file FILE
EOF
}

die() {
  echo "run-avd-ci.sh: $*" >&2
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

find_android_tool() {
  tool="$1"
  fallback="$2"

  if command -v "${tool}" >/dev/null 2>&1; then
    command -v "${tool}"
    return 0
  fi

  sdk_root="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
  if [ -n "${sdk_root}" ] && [ -x "${sdk_root}/${fallback}" ]; then
    printf '%s\n' "${sdk_root}/${fallback}"
    return 0
  fi

  die "unable to find Android SDK tool: ${tool}"
}

ensure_android_avd_home() {
  if [ -z "${ANDROID_AVD_HOME:-}" ]; then
    export ANDROID_AVD_HOME="${RUNNER_TEMP:-${PWD}}/mobilegl-avd"
  fi

  mkdir -p "${ANDROID_AVD_HOME}"
}

print_diagnostics() {
  emulator_log="$1"
  adb_bin="$(find_android_tool adb platform-tools/adb)"
  "${adb_bin}" devices -l >&2 || true
  if [ -f "${emulator_log}" ]; then
    echo "----- emulator log -----" >&2
    cat "${emulator_log}" >&2 || true
    echo "----- end emulator log -----" >&2
  fi
}

create_avd() {
  api_level=""
  target=""
  arch=""
  profile=""
  avd_name=""

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --api-level) api_level="$(next_arg "$@")"; shift 2 ;;
      --target) target="$(next_arg "$@")"; shift 2 ;;
      --arch) arch="$(next_arg "$@")"; shift 2 ;;
      --profile) profile="$(next_arg "$@")"; shift 2 ;;
      --avd-name) avd_name="$(next_arg "$@")"; shift 2 ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown create argument: $1" ;;
    esac
  done

  require_value "${api_level}" "--api-level"
  require_value "${target}" "--target"
  require_value "${arch}" "--arch"
  require_value "${profile}" "--profile"
  require_value "${avd_name}" "--avd-name"

  sdkmanager_bin="$(find_android_tool sdkmanager cmdline-tools/latest/bin/sdkmanager)"
  avdmanager_bin="$(find_android_tool avdmanager cmdline-tools/latest/bin/avdmanager)"
  ensure_android_avd_home

  system_image="system-images;android-${api_level};${target};${arch}"
  yes | "${sdkmanager_bin}" --licenses >/dev/null
  yes | "${sdkmanager_bin}" "platform-tools" "emulator" "platforms;android-${api_level}" "${system_image}"
  echo no | "${avdmanager_bin}" create avd --force --name "${avd_name}" --package "${system_image}" --device "${profile}"
}

# The guest property a snapshot carries: `snapshot` sets it just before saving, and it lives only
# in guest RAM, so after `start` it reads the snapshot's name exactly when that snapshot loaded.
snapshot_marker_prop="debug.ci.boot_snapshot"

# launch_emulator AVD GPU LOG PID_FILE SNAPSHOT_FLAG...: the flags a snapshot is saved and loaded
# with must match, so both commands launch through here.
launch_emulator() {
  launch_avd="$1"
  launch_gpu="$2"
  launch_log="$3"
  launch_pid_file="$4"
  shift 4
  mkdir -p "$(dirname "${launch_log}")" "$(dirname "${launch_pid_file}")"
  "${emulator_bin}" -avd "${launch_avd}" \
    -no-window \
    -gpu "${launch_gpu}" \
    "$@" \
    -noaudio \
    -no-boot-anim \
    -camera-back none \
    -camera-front none \
    > "${launch_log}" 2>&1 &
  echo "$!" > "${launch_pid_file}"
}

wait_for_boot() {
  emulator_log="$1"
  boot_timeout="$2"
  if ! timeout "${boot_timeout}" "${adb_bin}" wait-for-device; then
    print_diagnostics "${emulator_log}"
    die "emulator did not connect to adb within ${boot_timeout}s"
  fi

  booted=""
  for _ in $(seq 1 "${boot_timeout}"); do
    booted="$("${adb_bin}" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)"
    if [ "${booted}" = "1" ]; then
      break
    fi
    sleep 1
  done

  if [ "${booted}" != "1" ]; then
    print_diagnostics "${emulator_log}"
    die "emulator did not boot within ${boot_timeout}s"
  fi

  "${adb_bin}" shell settings put global window_animation_scale 0
  "${adb_bin}" shell settings put global transition_animation_scale 0
  "${adb_bin}" shell settings put global animator_duration_scale 0
  "${adb_bin}" shell input keyevent 82 || true
}

start_avd() {
  avd_name=""
  gpu=""
  emulator_log=""
  pid_file=""
  boot_timeout=""
  snapshot=""

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --avd-name) avd_name="$(next_arg "$@")"; shift 2 ;;
      --gpu) gpu="$(next_arg "$@")"; shift 2 ;;
      --emulator-log) emulator_log="$(next_arg "$@")"; shift 2 ;;
      --pid-file) pid_file="$(next_arg "$@")"; shift 2 ;;
      --boot-timeout) boot_timeout="$(next_arg "$@")"; shift 2 ;;
      --snapshot) snapshot="$(next_arg "$@")"; shift 2 ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown start argument: $1" ;;
    esac
  done

  require_value "${avd_name}" "--avd-name"
  require_value "${gpu}" "--gpu"
  require_value "${emulator_log}" "--emulator-log"
  require_value "${pid_file}" "--pid-file"
  require_value "${boot_timeout}" "--boot-timeout"

  adb_bin="$(find_android_tool adb platform-tools/adb)"
  emulator_bin="$(find_android_tool emulator emulator/emulator)"
  ensure_android_avd_home

  # A snapshot only shortens the boot: the image, the AVD and the GPU mode are the same, and the
  # emulator cold-boots by itself when the snapshot does not load.
  started_at="$(date +%s)"
  if [ -n "${snapshot}" ] && [ -d "${ANDROID_AVD_HOME}/${avd_name}.avd/snapshots/${snapshot}" ]; then
    launch_emulator "${avd_name}" "${gpu}" "${emulator_log}" "${pid_file}" -snapshot "${snapshot}" -no-snapshot-save
  else
    launch_emulator "${avd_name}" "${gpu}" "${emulator_log}" "${pid_file}" -no-snapshot
  fi
  wait_for_boot "${emulator_log}" "${boot_timeout}"

  seconds="$(( $(date +%s) - started_at ))"
  marker="$("${adb_bin}" shell getprop "${snapshot_marker_prop}" 2>/dev/null | tr -d '\r' || true)"
  if [ -n "${snapshot}" ] && [ "${marker}" = "${snapshot}" ]; then
    echo "run-avd-ci.sh: booted in ${seconds}s from snapshot ${snapshot}"
  elif [ -n "${snapshot}" ]; then
    echo "run-avd-ci.sh: booted in ${seconds}s by cold boot; snapshot ${snapshot} did not load"
  else
    echo "run-avd-ci.sh: booted in ${seconds}s by cold boot"
  fi
}

# Boot the AVD cold once in GPU mode GPU and save snapshot NAME of the booted device, then shut it
# down cleanly so the AVD can be cached. The first call also does the AVD's first-boot setup,
# which a cold boot after a cache restore then no longer repeats.
snapshot_avd() {
  avd_name=""
  gpu=""
  snapshot_name=""
  emulator_log=""
  pid_file=""
  boot_timeout=""

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --avd-name) avd_name="$(next_arg "$@")"; shift 2 ;;
      --gpu) gpu="$(next_arg "$@")"; shift 2 ;;
      --name) snapshot_name="$(next_arg "$@")"; shift 2 ;;
      --emulator-log) emulator_log="$(next_arg "$@")"; shift 2 ;;
      --pid-file) pid_file="$(next_arg "$@")"; shift 2 ;;
      --boot-timeout) boot_timeout="$(next_arg "$@")"; shift 2 ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown snapshot argument: $1" ;;
    esac
  done

  require_value "${avd_name}" "--avd-name"
  require_value "${gpu}" "--gpu"
  require_value "${snapshot_name}" "--name"
  require_value "${emulator_log}" "--emulator-log"
  require_value "${pid_file}" "--pid-file"
  require_value "${boot_timeout}" "--boot-timeout"

  adb_bin="$(find_android_tool adb platform-tools/adb)"
  emulator_bin="$(find_android_tool emulator emulator/emulator)"
  ensure_android_avd_home

  started_at="$(date +%s)"
  launch_emulator "${avd_name}" "${gpu}" "${emulator_log}" "${pid_file}" -no-snapshot-load -no-snapshot-save
  wait_for_boot "${emulator_log}" "${boot_timeout}"
  echo "run-avd-ci.sh: ${gpu} cold boot took $(( $(date +%s) - started_at ))s"

  "${adb_bin}" shell setprop "${snapshot_marker_prop}" "${snapshot_name}"
  saved="$("${adb_bin}" emu avd snapshot save "${snapshot_name}" 2>&1 | tr -d '\r' || true)"
  echo "run-avd-ci.sh: snapshot save ${snapshot_name}: ${saved}"

  "${adb_bin}" emu kill >/dev/null 2>&1 || true
  emulator_pid="$(cat "${pid_file}")"
  for _ in $(seq 1 120); do
    kill -0 "${emulator_pid}" 2>/dev/null || break
    sleep 1
  done
  if kill -0 "${emulator_pid}" 2>/dev/null; then
    kill "${emulator_pid}" >/dev/null 2>&1 || true
    die "emulator did not exit after saving snapshot ${snapshot_name}"
  fi

  snapshot_dir="${ANDROID_AVD_HOME}/${avd_name}.avd/snapshots/${snapshot_name}"
  case "${saved}" in
    *OK*) ;;
    *) rm -rf "${snapshot_dir}"; die "snapshot ${snapshot_name} was not saved: ${saved}" ;;
  esac
  [ -d "${snapshot_dir}" ] || die "snapshot ${snapshot_name} reported OK but ${snapshot_dir} does not exist"
  du -sh "${snapshot_dir}"
}

stop_avd() {
  avd_name=""
  emulator_log=""
  pid_file=""

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --avd-name) avd_name="$(next_arg "$@")"; shift 2 ;;
      --emulator-log) emulator_log="$(next_arg "$@")"; shift 2 ;;
      --pid-file) pid_file="$(next_arg "$@")"; shift 2 ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown stop argument: $1" ;;
    esac
  done

  require_value "${avd_name}" "--avd-name"
  require_value "${emulator_log}" "--emulator-log"
  require_value "${pid_file}" "--pid-file"

  adb_bin="$(find_android_tool adb platform-tools/adb)"
  ensure_android_avd_home

  "${adb_bin}" emu kill >/dev/null 2>&1 || true
  if [ -f "${pid_file}" ]; then
    emulator_pid="$(cat "${pid_file}")"
    if [ -n "${emulator_pid}" ] && kill -0 "${emulator_pid}" 2>/dev/null; then
      kill "${emulator_pid}" >/dev/null 2>&1 || true
    fi
  fi
}

if [ "$#" -eq 0 ]; then
  die "missing command"
fi

command="$1"
shift

case "${command}" in
  create) create_avd "$@" ;;
  start) start_avd "$@" ;;
  snapshot) snapshot_avd "$@" ;;
  stop) stop_avd "$@" ;;
  -h|--help) usage; exit 0 ;;
  *) die "unknown command: ${command}" ;;
esac
