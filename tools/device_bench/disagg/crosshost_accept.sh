#!/bin/bash
# P12 exit gate (b): a client on ANOTHER machine (WSL on the Windows host) replays the in-world
# Minecraft trace against the phone's render server over TCP + stream data plane, and the P6.5
# link numbers are recorded. Run from Git Bash. Results: $PERF_WORK/crosshost/<run>/.
#
#   bash crosshost_accept.sh                 # ssim gate for both traces, then credit 1/2/3 benchmarks
#   RUNS="ssim:openra bench:rd12:1" bash crosshost_accept.sh
#
# The WSL default route may be a TUN proxy (see tcp_path_check.sh). This script adds a temporary
# /32 route around it for the phone, and removes it on exit.
set -u
export MSYS_NO_PATHCONV=1
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"; REPO_WIN="$(cygpath -m "$REPO")"
SER="${SER:-2f7cbe2e}"
PKG="${PKG:-top.mobilegl.plugin.trace}"
PHONE="${PHONE:-192.168.21.181}"
GATEWAY="${GATEWAY:-192.168.31.1}"
TOKEN="${TOKEN:-crosshost-token-2026-09}"
WSL="${WSL_DISTRO:-Ubuntu}"
PERF_WORK="${PERF_WORK:-$REPO/.trace-work/perf-compare}"
RUNS="${RUNS:-ssim:openra ssim:rd12 bench:openra:1 bench:openra:2 bench:openra:3 bench:rd12:1 bench:rd12:2 bench:rd12:3}"
TRACES='$HOME/wifi-perf/traces'
WSL_OUT='$HOME/wifi-perf/out/crosshost'
LOCAL_OUT="$PERF_WORK/crosshost"
log() { echo "[$(date +%H:%M:%S)] $*"; }

cleanup() {
  wsl.exe -d "$WSL" -u root -- ip route del "$PHONE/32" via "$GATEWAY" dev eth0 2>/dev/null
  python "$REPO_WIN/tools/trace_replay/tcp_device_server.py" stop --serial "$SER" --package "$PKG" >/dev/null 2>&1
  log "cleanup: temporary route removed, server stopped"
}
trap cleanup EXIT

wsl.exe -d "$WSL" -u root -- ip route replace "$PHONE/32" via "$GATEWAY" dev eth0 || exit 1
bash "$HERE/tcp_path_check.sh" || { log "FATAL: path is not end to end"; exit 1; }

adb -s "$SER" logcat -c
python "$REPO_WIN/tools/trace_replay/tcp_device_server.py" start --serial "$SER" --package "$PKG" \
  --backend DirectGLES --listen tcp://0.0.0.0:40613 --token "$TOKEN" >/dev/null 2>&1 || { log "FATAL: server start"; exit 1; }
for i in $(seq 1 30); do
  adb -s "$SER" logcat -d -s MobileGL:* | tr -d '\r' | grep -q 'listening on tcp://0.0.0.0:40613' && break; sleep 1
done
adb -s "$SER" logcat -d -s MobileGL:* | tr -d '\r' | grep -q 'listening on tcp://0.0.0.0:40613' || { log "FATAL: server not listening"; exit 1; }
log "server listening on $PHONE:40613 (phone $(adb -s "$SER" shell dumpsys battery | tr -d '\r' | sed -n 's/.*temperature: \([0-9]*\).*/\1/p' | head -1) x0.1C)"

mkdir -p "$LOCAL_OUT"
for run in $RUNS; do
  IFS=: read -r mode wl credit <<<"$run"
  [ "$mode" = ssim ] && credit=1
  name="$mode-$wl${credit:+-c$credit}"; [ "$mode" = ssim ] && name="ssim-$wl"
  case "$wl" in
    openra) trace=openra; golden=openra.0000031249.png; target=31249; geom="--width 640 --height 480 --crop-x 1 --crop-y 1 --crop-width 638 --crop-height 478"; tmo=600 ;;
    rd12)   trace=minecraft-1.21.4-rd12-odinlite-in-world; golden=minecraft-1.21.4-rd12-odinlite-in-world.0004660351.png; target=4660351; geom="--width 854 --height 480"; tmo=1500 ;;
    *) log "unknown workload $wl"; continue ;;
  esac
  if [ "$mode" = ssim ]; then
    mode_args="--target-call $target --golden $TRACES/$golden --ssim-threshold 0.99 $geom"
  else
    mode_args="--benchmark --benchmark-result=$WSL_OUT/$name/benchmark.json"
  fi
  # One session at a time: no stray client of ours, and the previous session child reaped, or the
  # supervisor answers Refuse{Busy} and the run measures nothing.
  wsl.exe -d "$WSL" -- pkill -f mobilegl_trace_replay 2>/dev/null
  for i in $(seq 1 60); do
    n=$(adb -s "$SER" shell 'ps -A -o NAME | grep -c "^libMobileGLServer.so"' | tr -d '')
    [ "${n:-0}" -le 1 ] && break; sleep 1
  done
  [ "${n:-0}" -le 1 ] || { log "FATAL: a session child is still alive after 60 s"; exit 1; }
  log "RUN $name"
  adb -s "$SER" logcat -c
  wsl.exe -d "$WSL" -- bash -c "
    set -u; cd ~/MobileGL; rm -rf $WSL_OUT/$name; mkdir -p $WSL_OUT/$name
    export MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_CONTROL=tcp://$PHONE:40613 MOBILEGL_IPC_DATA=stream
    export MOBILEGL_IPC_TOKEN=$TOKEN MOBILEGL_IPC_RUN_AHEAD=1 MOBILEGL_IPC_COLD_START_MS=120000
    export MOBILEGL_IPC_PRESENT_CREDIT=$credit MOBILEGL_PIPE_STATS=1
    SECONDS=0
    timeout $tmo ./build-split/tools/trace_replay/mobilegl_trace_replay --trace $TRACES/$trace.trace \
      --backend DirectGLES --mobilegl-library ./build-split/libMobileGL.so --pbuffer-surface \
      --output $WSL_OUT/$name $mode_args > $WSL_OUT/$name/mobilegl.log 2>&1
    echo rc=\$? secs=\$SECONDS > $WSL_OUT/$name/rc.txt"
  mkdir -p "$LOCAL_OUT/$name"
  cp -r "//wsl.localhost/$WSL/home/swung/wifi-perf/out/crosshost/$name/." "$LOCAL_OUT/$name/" 2>/dev/null
  adb -s "$SER" logcat -d > "$LOCAL_OUT/$name/phone-logcat.txt" 2>/dev/null
  log "  $(cat "$LOCAL_OUT/$name/rc.txt" 2>/dev/null) $(grep -o '"ssim": [-0-9.]*' "$LOCAL_OUT/$name/result.json" 2>/dev/null | head -1) $(grep -o '"fps": [0-9.]*' "$LOCAL_OUT/$name/benchmark.json" 2>/dev/null)"
  sleep 3
done
log "ALL DONE"
