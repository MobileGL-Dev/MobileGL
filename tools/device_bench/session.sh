#!/usr/bin/env bash
# Launch the game into the benchmark world and leave it running (for profiling
# or ad-hoc probing). Handles the flaky-launch failure mode seen on Odin Lite:
# the JVM occasionally dies with "exited due to signal 34" right after
# JLI_Launch (~40% of launches, cause unknown) — so launching retries up to
# --retries times before giving up.
#
# Usage: session.sh --device devices/odinlite.env [--backend magma|espryt|mobileglues]
#                   [--settle 150] [--retries 3] [--no-pin]
#                   [--allow-unverified-profile]
# Exits 0 with the game in-world (after settle seconds), 1 otherwise.
# NOTE: leaves the game running AND the frequency pins active (that is the
# point of a session). When done: am force-stop the game and unpin via
# `echo <cluster> -1 > /proc/ppm/policy/hard_userlimit_{min,max}_cpu_freq`
# and `echo 0 > /proc/gpufreq/gpufreq_opp_freq` (or run a bench.sh, whose
# cleanup unpins).

set -u -o pipefail
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

PKG=com.tungsten.fcl.mgdebug.debug
ACTIVITY=$PKG/com.tungsten.fcl.activity.SplashActivity
RENDERER_ESPRYT=5e273ee2-baca-4c81-8e48-b63feefb9ba8
RENDERER_MAGMA=2be0dc10-1eef-4ce2-b512-b266dd33fd9e
RENDERER_MOBILEGLUES=com.fcl.plugin.mobileglues

DEVICE_ENV="" BACKEND="" SETTLE=150 RETRIES=3 DO_PIN=1 ALLOW_UNVERIFIED_PROFILE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE_ENV=$2; shift 2 ;;
    --backend) BACKEND=$2; shift 2 ;;
    --settle) SETTLE=$2; shift 2 ;;
    --retries) RETRIES=$2; shift 2 ;;
    --no-pin) DO_PIN=0; shift ;;
    --allow-unverified-profile) ALLOW_UNVERIFIED_PROFILE=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[ -n "$DEVICE_ENV" ] || { echo "need --device" >&2; exit 2; }
# shellcheck disable=SC1090
. "$DEVICE_ENV"

# A device profile that has not been read off its device yet is refused here rather than acted
# on. The failure it prevents is silent and expensive: the pin path below is MediaTek-specific
# (/proc/ppm, /proc/gpufreq), `su -c 'echo ... > /proc/...'` fails without a non-zero exit, and a
# run against a profile whose nodes do not exist reports numbers it believes were taken under a
# frequency pin. The pin-integrity fields sampled at window end are the only clue, and they are
# read after the run rather than before it.
#
# PROFILE_VERIFIED=1 means: somebody read the cpufreq policies, the GPU OPP and the thermal zone
# TYPE off THIS device, ran one pinned window, and checked big_cur/little_cur/gpu_cur_khz in the
# result JSON against the pins. Nothing else earns it.
require_verified_profile() {
  # The default is UNVERIFIED. A profile that simply omits the key is a profile nobody has
  # confirmed against its device, and defaulting it to "verified" would hand exactly the
  # fail-open behaviour this guard exists to prevent to the most likely way a new profile is
  # written - by copying an existing one and editing the serial.
  if [ "${PROFILE_VERIFIED:-0}" = "1" ]; then return 0; fi
  if [ "$ALLOW_UNVERIFIED_PROFILE" = "1" ]; then
    echo "[warn] $DEVICE_ENV does not carry PROFILE_VERIFIED=1 and --allow-unverified-profile was passed:" >&2
    echo "[warn] the frequency pins and the thermal gate in it are UNCONFIRMED, so any number this" >&2
    echo "[warn] run produces is not comparable with a pinned one." >&2
    return 0
  fi
  echo "$DEVICE_ENV does not carry PROFILE_VERIFIED=1 (it says 0, or says nothing at all): its" >&2
  echo "sysfs nodes and OPPs have not been read off the device, so pinning would fail silently" >&2
  echo "and the run would look pinned but not be." >&2
  echo "Fill in the TODO_VERIFY_ON_DEVICE fields, confirm one pinned window, set PROFILE_VERIFIED=1 -" >&2
  echo "or pass --allow-unverified-profile to measure anyway and label the result unpinned." >&2
  exit 2
}
require_verified_profile
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
