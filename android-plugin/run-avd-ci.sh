#!/bin/sh
set -eu

usage() {
  cat <<'EOF'
Usage:
  sh android-plugin/run-avd-ci.sh \
    --api-level N \
    --target TARGET \
    --arch ARCH \
    --profile PROFILE \
    --gpu GPU \
    --avd-name NAME \
    -- COMMAND [ARGS ...]
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

api_level=""
target=""
arch=""
profile=""
gpu=""
avd_name=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --api-level) api_level="$(next_arg "$@")"; shift 2 ;;
    --target) target="$(next_arg "$@")"; shift 2 ;;
    --arch) arch="$(next_arg "$@")"; shift 2 ;;
    --profile) profile="$(next_arg "$@")"; shift 2 ;;
    --gpu) gpu="$(next_arg "$@")"; shift 2 ;;
    --avd-name) avd_name="$(next_arg "$@")"; shift 2 ;;
    --) shift; break ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

require_value "${api_level}" "--api-level"
require_value "${target}" "--target"
require_value "${arch}" "--arch"
require_value "${profile}" "--profile"
require_value "${gpu}" "--gpu"
require_value "${avd_name}" "--avd-name"

if [ "$#" -eq 0 ]; then
  die "missing command"
fi

system_image="system-images;android-${api_level};${target};${arch}"
sdkmanager "platform-tools" "emulator" "platforms;android-${api_level}" "${system_image}"
echo no | avdmanager create avd --force --name "${avd_name}" --package "${system_image}" --device "${profile}"

emulator_log="${RUNNER_TEMP:-.}/${avd_name}-emulator.log"
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

cleanup() {
  if kill -0 "${emulator_pid}" 2>/dev/null; then
    adb -s emulator-5554 emu kill >/dev/null 2>&1 || kill "${emulator_pid}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

adb wait-for-device

booted=""
for _ in $(seq 1 300); do
  booted="$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)"
  if [ "${booted}" = "1" ]; then
    break
  fi
  sleep 1
done

if [ "${booted}" != "1" ]; then
  cat "${emulator_log}" >&2 || true
  die "emulator did not boot"
fi

adb shell settings put global window_animation_scale 0
adb shell settings put global transition_animation_scale 0
adb shell settings put global animator_duration_scale 0
adb shell input keyevent 82 || true

"$@"
