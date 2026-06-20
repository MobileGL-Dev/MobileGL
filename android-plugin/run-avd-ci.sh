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

print_diagnostics() {
  emulator_log="$1"
  adb devices -l >&2 || true
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

  system_image="system-images;android-${api_level};${target};${arch}"
  sdkmanager "platform-tools" "emulator" "platforms;android-${api_level}" "${system_image}"
  echo no | avdmanager create avd --force --name "${avd_name}" --package "${system_image}" --device "${profile}"
}

start_avd() {
  avd_name=""
  gpu=""
  emulator_log=""
  pid_file=""
  boot_timeout=""

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --avd-name) avd_name="$(next_arg "$@")"; shift 2 ;;
      --gpu) gpu="$(next_arg "$@")"; shift 2 ;;
      --emulator-log) emulator_log="$(next_arg "$@")"; shift 2 ;;
      --pid-file) pid_file="$(next_arg "$@")"; shift 2 ;;
      --boot-timeout) boot_timeout="$(next_arg "$@")"; shift 2 ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown start argument: $1" ;;
    esac
  done

  require_value "${avd_name}" "--avd-name"
  require_value "${gpu}" "--gpu"
  require_value "${emulator_log}" "--emulator-log"
  require_value "${pid_file}" "--pid-file"
  require_value "${boot_timeout}" "--boot-timeout"

  mkdir -p "$(dirname "${emulator_log}")" "$(dirname "${pid_file}")"
  emulator -avd "${avd_name}" \
    -no-window \
    -gpu "${gpu}" \
    -no-snapshot \
    -noaudio \
    -no-boot-anim \
    -camera-back none \
    -camera-front none \
    > "${emulator_log}" 2>&1 &
  emulator_pid="$!"
  echo "${emulator_pid}" > "${pid_file}"

  if ! timeout "${boot_timeout}" adb wait-for-device; then
    print_diagnostics "${emulator_log}"
    die "emulator did not connect to adb within ${boot_timeout}s"
  fi

  booted=""
  for _ in $(seq 1 "${boot_timeout}"); do
    booted="$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)"
    if [ "${booted}" = "1" ]; then
      break
    fi
    sleep 1
  done

  if [ "${booted}" != "1" ]; then
    print_diagnostics "${emulator_log}"
    die "emulator did not boot within ${boot_timeout}s"
  fi

  adb shell settings put global window_animation_scale 0
  adb shell settings put global transition_animation_scale 0
  adb shell settings put global animator_duration_scale 0
  adb shell input keyevent 82 || true
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

  adb emu kill >/dev/null 2>&1 || true
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
  stop) stop_avd "$@" ;;
  -h|--help) usage; exit 0 ;;
  *) die "unknown command: ${command}" ;;
esac
