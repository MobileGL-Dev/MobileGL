#!/usr/bin/env bash
# In-game FPS benchmark for MobileGL on a real device, driven through FCL.
#
# Prerequisites (one-time, manual):
#   - FCL fordebug flavor installed (com.tungsten.fcl.mgdebug.debug); its splash
#     screen auto-launches the selected profile/version into the prepared world.
#   - FCL in-game menu "show FPS" toggle enabled (menu_setting.json showFps=true):
#     the FPS overlay thread logs one "FCLFPS: <n>" logcat line per second.
#   - The benchmark world saved with a deterministic state (mob spawning /
#     daylight cycle / weather gamerules off) and the desired options.txt
#     (renderDistance, vsync off, maxFps high).
#   - Rooted device (frequency pinning + GPU utilization sampling).
#
# Usage:
#   bench.sh --device devices/odinlite.env --backend magma [--samples 30]
#            [--warmup 180] [--label mylabel] [--no-pin]
#   backend: magma | espryt | mobileglues (reference)
#
# Output: one JSON line on stdout (also appended to results/results.jsonl) with
# mean/median/min/max FPS, GPU busy%, temperatures, and pin-integrity flags.
# Screenshots (pre/post measurement) land in results/<timestamp>-<label>/.

set -u -o pipefail
cd "$(dirname "$0")"
# Git Bash: stop MSYS from rewriting /sys/... arguments into C:/Program Files/...
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

PKG=com.tungsten.fcl.mgdebug.debug
ACTIVITY=$PKG/com.tungsten.fcl.activity.SplashActivity
RENDERER_ESPRYT=5e273ee2-baca-4c81-8e48-b63feefb9ba8
RENDERER_MAGMA=2be0dc10-1eef-4ce2-b512-b266dd33fd9e
RENDERER_MOBILEGLUES=com.fcl.plugin.mobileglues

DEVICE_ENV=""
BACKEND=""
SAMPLES=30
WARMUP=180
LABEL=""
DO_PIN=1
WORLD_LOAD_TIMEOUT=420

while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE_ENV=$2; shift 2 ;;
    --backend) BACKEND=$2; shift 2 ;;
    --samples) SAMPLES=$2; shift 2 ;;
    --warmup) WARMUP=$2; shift 2 ;;
    --label) LABEL=$2; shift 2 ;;
    --no-pin) DO_PIN=0; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

[ -n "$DEVICE_ENV" ] && [ -n "$BACKEND" ] || { echo "need --device and --backend" >&2; exit 2; }
# shellcheck disable=SC1090
. "$DEVICE_ENV"

case "$BACKEND" in
  espryt) RENDERER=$RENDERER_ESPRYT ;;
  magma) RENDERER=$RENDERER_MAGMA ;;
  mobileglues) RENDERER=$RENDERER_MOBILEGLUES ;;
  *) echo "unknown backend: $BACKEND" >&2; exit 2 ;;
esac

ADB="adb -s $DEVICE_SERIAL"
STAMP=$(date +%Y%m%d-%H%M%S)
RUNLABEL="${STAMP}-${BACKEND}${LABEL:+-$LABEL}"
OUTDIR="results/$RUNLABEL"
mkdir -p "$OUTDIR"

log() { echo "[bench] $*" >&2; }
# Quote the whole su invocation for the DEVICE shell, or redirects run unprivileged.
sushell() { $ADB shell "su -c '$*'"; }

read_temp() {
  $ADB shell "for tz in /sys/class/thermal/thermal_zone*; do
    if [ \"\$(cat \$tz/type)\" = \"$THERMAL_ZONE_TYPE\" ]; then cat \$tz/temp; break; fi; done" | tr -d '\r'
}

# MTK: plain cpufreq sysfs writes are reverted by the vendor boost/PowerHAL within
# seconds — pin through ppm hard_userlimit instead (cluster indices: 0=little, 1=big).
pin_freqs() {
  log "pinning CPU big=$CPU_BIG_FREQ little=$CPU_LITTLE_FREQ gpu=${GPU_PIN_KHZ}kHz (ppm)"
  sushell "echo 1 $CPU_BIG_FREQ > /proc/ppm/policy/hard_userlimit_max_cpu_freq;
           echo 1 $CPU_BIG_FREQ > /proc/ppm/policy/hard_userlimit_min_cpu_freq;
           echo 0 $CPU_LITTLE_FREQ > /proc/ppm/policy/hard_userlimit_max_cpu_freq;
           echo 0 $CPU_LITTLE_FREQ > /proc/ppm/policy/hard_userlimit_min_cpu_freq" >/dev/null
  sushell "echo $GPU_PIN_KHZ > /proc/gpufreq/gpufreq_opp_freq" >/dev/null
}

unpin_freqs() {
  log "unpinning frequencies (restore DVFS)"
  sushell "echo 1 -1 > /proc/ppm/policy/hard_userlimit_max_cpu_freq;
           echo 1 -1 > /proc/ppm/policy/hard_userlimit_min_cpu_freq;
           echo 0 -1 > /proc/ppm/policy/hard_userlimit_max_cpu_freq;
           echo 0 -1 > /proc/ppm/policy/hard_userlimit_min_cpu_freq" >/dev/null
  sushell "echo 0 > /proc/gpufreq/gpufreq_opp_freq" >/dev/null
}

cleanup() {
  $ADB shell am force-stop $PKG >/dev/null 2>&1
  [ "$DO_PIN" = 1 ] && unpin_freqs
  $ADB shell svc power stayon false >/dev/null 2>&1
}
trap cleanup EXIT

# --- 1. Wake, unlock, keep screen on, fan to sport ---------------------------
$ADB shell input keyevent KEYCODE_WAKEUP >/dev/null
$ADB shell input keyevent 82 >/dev/null
$ADB shell svc power stayon true >/dev/null
$ADB shell settings put global fan_mode 3 2>/dev/null
wakefulness=$($ADB shell dumpsys power | grep -o 'mWakefulness=[A-Za-z]*' | head -1)
log "screen: $wakefulness"

# --- 2. Thermal gate ----------------------------------------------------------
log "thermal gate: waiting for $THERMAL_ZONE_TYPE <= $THERMAL_START_MAX_MC"
for i in $(seq 1 60); do
  T=$(read_temp)
  [ "$T" -le "$THERMAL_START_MAX_MC" ] && break
  log "  temp=$T, cooling... ($i)"
  sleep 10
done
TEMP_START=$(read_temp)
log "start temp: $TEMP_START"

# --- 3. Select renderer (device-side sed, proven under run-as) ---------------
for known in $RENDERER_ESPRYT $RENDERER_MAGMA $RENDERER_MOBILEGLUES; do
  [ "$known" = "$RENDERER" ] && continue
  $ADB shell run-as $PKG sed -i "s/$known/$RENDERER/g" files/config.json
done
log "renderer now: $($ADB shell run-as $PKG grep renderer files/config.json | tr -d '\r' | tr -s ' ' | sort -u | tr '\n' ' ')"

# --- 4. Pin frequencies -------------------------------------------------------
[ "$DO_PIN" = 1 ] && pin_freqs

# --- 5. Launch, wait for world (retry: the JVM occasionally dies with
# "exited due to signal 34" right after JLI_Launch on this device) -------------
MCLOG="/storage/emulated/0/FCL/.minecraft/versions/*/logs/latest.log"
WORLD_UP=0
for attempt in 1 2 3; do
  $ADB shell am force-stop $PKG
  sleep 5
  $ADB shell "rm -f $MCLOG /sdcard/MG/latest.log" 2>/dev/null
  $ADB logcat -c 2>/dev/null
  log "launching $ACTIVITY (attempt $attempt)"
  $ADB shell am start -n $ACTIVITY >/dev/null
  for i in $(seq 1 $((WORLD_LOAD_TIMEOUT / 5))); do
    sleep 5
    if $ADB shell "grep -l -e 'logged in with entity id' -e 'Preparing spawn area: 100' $MCLOG" >/dev/null 2>&1; then
      WORLD_UP=1; break
    fi
    if ! $ADB shell pidof $PKG >/dev/null; then
      sleep 3
      if ! $ADB shell pidof $PKG >/dev/null; then
        log "attempt $attempt: game process died; retrying"
        break
      fi
    fi
  done
  [ "$WORLD_UP" = 1 ] && break
done
if [ "$WORLD_UP" != 1 ]; then
  log "world did not load within ${WORLD_LOAD_TIMEOUT}s"
  $ADB exec-out screencap -p > "$OUTDIR/failed-load.png" 2>/dev/null
  echo "{\"label\":\"$RUNLABEL\",\"error\":\"world-load-timeout\"}"
  exit 1
fi
log "world is up; warmup ${WARMUP}s"
sleep "$WARMUP"

# --- 6. Measure ---------------------------------------------------------------
$ADB exec-out screencap -p > "$OUTDIR/pre.png" 2>/dev/null
TEMP_MID=$(read_temp)
GPU_BUSY_SAMPLES=""
FPS_FILE="$OUTDIR/fps.txt"
: > "$FPS_FILE"
log "sampling $SAMPLES FPS values (1/s) + GPU busy"
$ADB logcat -v raw -s FCLFPS:I > "$OUTDIR/fclfps.log" &
LOGCAT_PID=$!
for i in $(seq 1 "$SAMPLES"); do
  sleep 1
  B=$(sushell "cat $GPU_UTIL_NODE" | tr -d '\r' | awk '{print $1}')
  GPU_BUSY_SAMPLES="$GPU_BUSY_SAMPLES $B"
done
kill $LOGCAT_PID 2>/dev/null
wait $LOGCAT_PID 2>/dev/null
grep -E '^[0-9]+$' "$OUTDIR/fclfps.log" | tail -n "$SAMPLES" > "$FPS_FILE"
$ADB exec-out screencap -p > "$OUTDIR/post.png" 2>/dev/null
TEMP_END=$(read_temp)

# Pin integrity: sample live freqs right at the end of the window (game still hot).
BIG_CUR=$($ADB shell cat /sys/devices/system/cpu/cpufreq/$CPU_BIG_POLICY/scaling_cur_freq | tr -d '\r')
LITTLE_CUR=$($ADB shell cat /sys/devices/system/cpu/cpufreq/$CPU_LITTLE_POLICY/scaling_cur_freq | tr -d '\r')
GPU_CUR=$(sushell "cat $GPU_CURFREQ_NODE" | tr -d '\r' | awk '{print $NF}')

# --- 7. Stats -----------------------------------------------------------------
STATS=$(sort -n "$FPS_FILE" | awk '
  { v[NR]=$1; s+=$1 }
  END {
    if (NR==0) { print "0 0 0 0 0 0"; exit }
    mean=s/NR; med=v[int((NR+1)/2)];
    for(i=1;i<=NR;i++) ss+=(v[i]-mean)^2;
    sd=(NR>1)?sqrt(ss/(NR-1)):0;
    printf "%d %.1f %d %d %d %.1f", NR, mean, med, v[1], v[NR], sd
  }')
set -- $STATS
N=$1 MEAN=$2 MED=$3 MIN=$4 MAX=$5 SD=$6
GPU_BUSY_MEAN=$(echo "$GPU_BUSY_SAMPLES" | tr ' ' '\n' | grep -E '^[0-9]+$' | awk '{s+=$1;n++} END{if(n) printf "%.0f", s/n; else print 0}')

RESULT=$(printf '{"label":"%s","backend":"%s","samples":%s,"fps_mean":%s,"fps_median":%s,"fps_min":%s,"fps_max":%s,"fps_sd":%s,"gpu_busy_mean":%s,"temp_start_mc":%s,"temp_mid_mc":%s,"temp_end_mc":%s,"big_cur":%s,"little_cur":%s,"gpu_cur_khz":%s,"pinned":%s,"warmup_s":%s}' \
  "$RUNLABEL" "$BACKEND" "$N" "$MEAN" "$MED" "$MIN" "$MAX" "$SD" "$GPU_BUSY_MEAN" \
  "$TEMP_START" "$TEMP_MID" "$TEMP_END" "$BIG_CUR" "$LITTLE_CUR" "${GPU_CUR:-0}" "$DO_PIN" "$WARMUP")
mkdir -p results
echo "$RESULT" >> results/results.jsonl
echo "$RESULT"
