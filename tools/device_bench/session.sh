#!/usr/bin/env bash
# Launch the game into the benchmark world and leave it running (for profiling
# or ad-hoc probing). Handles the flaky-launch failure mode seen on Odin Lite:
# the JVM occasionally dies with "exited due to signal 34" right after
# JLI_Launch (~40% of launches, cause unknown) — so launching retries up to
# --retries times before giving up.
#
# Usage: session.sh --device devices/odinlite.env [--backend magma|espryt|mobileglues]
#                   [--settle 150] [--retries 3] [--no-pin]
# Exits 0 with the game in-world (after settle seconds), 1 otherwise.

set -u -o pipefail
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

PKG=com.tungsten.fcl.mgdebug.debug
ACTIVITY=$PKG/com.tungsten.fcl.activity.SplashActivity
RENDERER_ESPRYT=5e273ee2-baca-4c81-8e48-b63feefb9ba8
RENDERER_MAGMA=2be0dc10-1eef-4ce2-b512-b266dd33fd9e
RENDERER_MOBILEGLUES=com.fcl.plugin.mobileglues

DEVICE_ENV="" BACKEND="" SETTLE=150 RETRIES=3 DO_PIN=1
while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE_ENV=$2; shift 2 ;;
    --backend) BACKEND=$2; shift 2 ;;
    --settle) SETTLE=$2; shift 2 ;;
    --retries) RETRIES=$2; shift 2 ;;
    --no-pin) DO_PIN=0; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[ -n "$DEVICE_ENV" ] || { echo "need --device" >&2; exit 2; }
# shellcheck disable=SC1090
. "$DEVICE_ENV"
ADB="adb -s $DEVICE_SERIAL"
log() { echo "[session] $*" >&2; }

if [ -n "$BACKEND" ]; then
  case "$BACKEND" in
    espryt) RENDERER=$RENDERER_ESPRYT ;;
    magma) RENDERER=$RENDERER_MAGMA ;;
    mobileglues) RENDERER=$RENDERER_MOBILEGLUES ;;
    *) echo "unknown backend: $BACKEND" >&2; exit 2 ;;
  esac
  for known in $RENDERER_ESPRYT $RENDERER_MAGMA $RENDERER_MOBILEGLUES; do
    [ "$known" = "$RENDERER" ] && continue
    $ADB shell run-as $PKG sed -i "s/$known/$RENDERER/g" files/config.json
  done
fi

$ADB shell "input keyevent KEYCODE_WAKEUP; sleep 1; input keyevent 82; svc power stayon true; settings put global fan_mode 3"
if [ "$DO_PIN" = 1 ]; then
  $ADB shell "su -c 'echo 1 $CPU_BIG_FREQ > /proc/ppm/policy/hard_userlimit_max_cpu_freq;
                     echo 1 $CPU_BIG_FREQ > /proc/ppm/policy/hard_userlimit_min_cpu_freq;
                     echo 0 $CPU_LITTLE_FREQ > /proc/ppm/policy/hard_userlimit_max_cpu_freq;
                     echo 0 $CPU_LITTLE_FREQ > /proc/ppm/policy/hard_userlimit_min_cpu_freq;
                     echo $GPU_PIN_KHZ > /proc/gpufreq/gpufreq_opp_freq'"
fi

MCLOG=/storage/emulated/0/FCL/.minecraft/versions/1.21.4/logs/latest.log
for attempt in $(seq 1 "$RETRIES"); do
  $ADB shell am force-stop $PKG
  sleep 5
  $ADB shell "rm -f $MCLOG"
  $ADB shell am start -n $ACTIVITY >/dev/null
  log "attempt $attempt: launched"
  DEADLINE=$((SECONDS + 300))
  while [ $SECONDS -lt $DEADLINE ]; do
    sleep 5
    if $ADB shell "grep -q 'joined the game' $MCLOG" 2>/dev/null; then
      log "world up (attempt $attempt); settling ${SETTLE}s"
      sleep "$SETTLE"
      exit 0
    fi
    if ! $ADB shell pidof $PKG >/dev/null; then
      # process may still be between splash and JVM start briefly; confirm twice
      sleep 3
      if ! $ADB shell pidof $PKG >/dev/null; then
        log "attempt $attempt: process died (signal-34 flake?); retrying"
        break
      fi
    fi
  done
done
log "no world after $RETRIES attempts"
exit 1
