#!/usr/bin/env bash
# P14 perf matrix: dev (pull monolith, baseline APK) vs feat (monolith / inproc / spawn+shm /
# spawn+tcp-localhost) x {Espryt, Magma} x {openra, rd12}, on a rooted device with pinned clocks.
#
#   SER=HA27Q3LQ PKG_FEAT=top.mobilegl.plugin.p14.trace PKG_DEV=top.mobilegl.plugin.p14dev.trace \
#   ARMS="dev monolith inproc shm tcp" BACKENDS="DirectGLES DirectVulkan" WLS="openra rd12" REPS=3 \
#   TAG=base bash matrix_p14.sh prep|run|ssim
#
# Per run: force-stop, clear outputs, start cpusampler (per-thread schedstat + kgsl gpubusy, 10 ms)
# watching benchmark.json, launch the replay, wait, pull result/benchmark/logcat/sampler CSV, and
# verify the arm and backend markers. Rounds interleave arms (and backends) so drift spreads evenly.
# Results: $PERF_WORK/<TAG>/<wl>/<backend>/<arm>-r<n>/ ; reduce with p14_report.py.
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SER="${SER:-HA27Q3LQ}"
PKG_FEAT="${PKG_FEAT:-top.mobilegl.plugin.p14.trace}"
PKG_DEV="${PKG_DEV:-top.mobilegl.plugin.p14dev.trace}"
ARMS=(${ARMS:-dev monolith inproc shm tcp})
BACKENDS=(${BACKENDS:-DirectGLES DirectVulkan})
WLS=(${WLS:-openra rd12})
REPS="${REPS:-3}"
TAG="${TAG:-base}"
FIXTURES="${FIXTURES:-$REPO_ROOT/tools/trace_replay/fixtures}"
ROOT="${PERF_WORK:-$REPO_ROOT/.trace-work/perf-p14}/$TAG"
TIMEOUT="${TIMEOUT:-900}"
EXTRA_ENV="${EXTRA_ENV:-}"
COOL_C="${COOL_C:-38}"          # wait until battery temperature is below this before a run
LOG="$ROOT/matrix.log"
SAMPLER=/data/local/tmp/cpusampler

shq() { adb -s "$SER" shell "$1"; }
log() { local line; line="[$(date '+%F %T')] $*"; mkdir -p "$ROOT"; echo "$line" >>"$LOG"; echo "$line"; }
# An arm is <kind>[@<tag>]: the tag picks another installed build of the trace APK
# (top.mobilegl.plugin.<tag>.trace), so two builds can be interleaved in one matrix.
arm_kind() { echo "${1%@*}"; }
pkg_of() {
  case "$1" in
    *@*) echo "top.mobilegl.plugin.${1#*@}.trace" ;;
    dev) echo "$PKG_DEV" ;;
    *) echo "$PKG_FEAT" ;;
  esac
}
app_of() { echo "/data/user/0/$1/files/trace-replay"; }

wl_args() { # trace golden w h target crop
  case "$1" in
    openra) echo "openra openra.0000031249.png 640 480 31249 1 1 638 478" ;;
    rd12) echo "rd12 minecraft-1.21.4-rd12-odinlite-in-world.0004660351.png 854 480 4660351 0 0 0 0" ;;
  esac
}

arm_env() {
  case "$(arm_kind "$1")" in
    dev|monolith) echo "" ;;
    inproc) echo "MOBILEGL_TRANSPORT=inproc;MOBILEGL_IPC_RUN_AHEAD=1" ;;
    shm) echo "MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_CONTROL=fork;MOBILEGL_IPC_DATA=auto;MOBILEGL_IPC_RUN_AHEAD=1" ;;
    tcp) echo "MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_CONTROL=tcp://127.0.0.1:40613;MOBILEGL_IPC_DATA=stream;MOBILEGL_IPC_RUN_AHEAD=1" ;;
  esac
}

temp_dc() { adb -s "$SER" shell dumpsys battery 2>/dev/null | tr -d '\r' | sed -n 's/.*temperature: \([0-9]*\).*/\1/p' | head -1; }

cool_down() {
  local t; t=$(temp_dc); local waited=0
  while [ -n "$t" ] && [ "$t" -ge $((COOL_C * 10)) ] && [ "$waited" -lt 900 ]; do
    sleep 15; waited=$((waited + 15)); t=$(temp_dc)
  done
  [ "$waited" -gt 0 ] && log "cooled ${waited}s to ${t}"
}

prep_pkg() {
  local pkg="$1" app; app=$(app_of "$pkg")
  local tmp="$ROOT/fixtures"; mkdir -p "$tmp"
  for wl in openra rd12; do
    local tgz="$FIXTURES/$( [ $wl = openra ] && echo openra.tgz || echo minecraft-1.21.4-rd12-odinlite-in-world.tgz)"
    set -- $(wl_args $wl)
    local golden="$FIXTURES/$2"
    if [ ! -f "$tmp/$wl.trace" ]; then
      rm -rf "$tmp/x"; mkdir -p "$tmp/x"; tar xzf "$tgz" -C "$tmp/x" || exit 1
      mv "$(find "$tmp/x" -name '*.trace' | head -1)" "$tmp/$wl.trace"; rm -rf "$tmp/x"
    fi
    adb -s "$SER" push "$(cygpath -w "$tmp/$wl.trace")" /data/local/tmp/p14-$wl.trace >/dev/null || exit 1
    adb -s "$SER" push "$(cygpath -w "$golden")" /data/local/tmp/p14-$wl.png >/dev/null || exit 1
    shq "am force-stop $pkg; run-as $pkg sh -c 'mkdir -p $app/input-$wl $app/output'"
    shq "cat /data/local/tmp/p14-$wl.trace | run-as $pkg sh -c 'cat > $app/input-$wl/trace.trace'"
    shq "cat /data/local/tmp/p14-$wl.png | run-as $pkg sh -c 'cat > $app/input-$wl/golden.png'"
    local size host; size=$(shq "run-as $pkg sh -c 'wc -c < $app/input-$wl/trace.trace'" | tr -d '\r ')
    host=$(wc -c <"$tmp/$wl.trace")
    [ "$size" = "$host" ] || { log "FATAL: $pkg $wl size $size != $host"; exit 1; }
    log "prep $pkg $wl ok ($size bytes)"
  done
  shq "rm -f /data/local/tmp/p14-*.trace /data/local/tmp/p14-*.png; chmod 755 $SAMPLER"
}

verify_run() {
  local dir="$1" arm="$2" backend="$3" all="$1/markers.txt" v="$1/verify.txt"
  grep -aE "Config: MOBILEGL_TRANSPORT|spawn ARMED|control=tcp data=stream|run-ahead ARMED|running lockstep|DISARMED|Active backend type|debug.mobilegl.backend|Fatal\{" \
    "$dir/logcat.txt" "$dir"/mgl.*.log 2>/dev/null >"$all"
  {
    echo "arm=$arm backend=$backend"
    grep -aq "Active backend type set to $backend" "$all" && echo "BACKEND=OK" || echo "BACKEND=MISSING"
    case "$(arm_kind "$arm")" in
      dev|monolith) grep -aqE "Config: MOBILEGL_TRANSPORT=(inproc|spawn)" "$all" && echo "ARM=UNEXPECTED_SPLIT" || echo "ARM=OK(monolith)" ;;
      inproc) grep -aq "Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream" "$all" && echo "ARM=OK(inproc)" || echo "ARM=MISSING" ;;
      shm) grep -aq "spawn ARMED - the server role runs in pid" "$all" && echo "ARM=OK(spawn)" || echo "ARM=MISSING" ;;
      tcp) grep -aq "control=tcp data=stream server=" "$all" && echo "ARM=OK(tcp)" || echo "ARM=MISSING" ;;
    esac
    case "$(arm_kind "$arm")" in inproc|shm|tcp) grep -aq "run-ahead ARMED" "$all" && echo "RUN_AHEAD=OK" || echo "RUN_AHEAD=MISSING" ;; esac
    grep -aq "running lockstep" "$all" && echo "LOCKSTEP=PRESENT"
    echo "FATAL_LINES=$(grep -ac 'Fatal{' "$all")"
  } >"$v"
}

# run_one <wl> <backend> <arm> <rep> <mode:benchmark|ssim>
run_one() {
  local wl="$1" backend="$2" arm="$3" rep="$4" mode="$5"
  local pkg app dir; pkg=$(pkg_of "$arm"); app=$(app_of "$pkg")
  dir="$ROOT/$wl/$backend/$arm-r$rep"; rm -rf "$dir"; mkdir -p "$dir"
  set -- $(wl_args "$wl"); local w="$3" h="$4" target="$5" cx="$6" cy="$7" cw="$8" ch="$9"
  cool_down
  local t0 epoch0 status=""; t0=$(temp_dc); epoch0=$(date +%s)
  log "BEGIN $TAG $wl/$backend/$arm r$rep $mode temp=$t0"
  shq "am force-stop $PKG_FEAT; am force-stop $PKG_DEV; am force-stop $pkg" >/dev/null 2>&1
  shq "run-as $pkg sh -c 'rm -f $app/output/*.json $app/output/*.log $app/output/*.png /data/user/0/$pkg/files/mgl.*.log'" >/dev/null 2>&1
  shq "su -c 'pkill -f cpusampler; rm -f /data/local/tmp/p14cpu.csv'" >/dev/null 2>&1
  adb -s "$SER" logcat -c >/dev/null 2>&1
  if [ "$(arm_kind "$arm")" = tcp ]; then
    python "$(cygpath -w "$REPO_ROOT/tools/trace_replay/tcp_device_server.py")" start --serial "$SER" --package "$pkg" \
      --backend "$backend" --listen tcp://127.0.0.1:40613 >"$dir/server-start.log" 2>&1 || { echo server_start_failed >"$dir/STATUS"; log "FAIL tcp server"; return 1; }
    local waited=0
    until adb -s "$SER" logcat -d 2>/dev/null | grep -q "listening on tcp://127.0.0.1:40613"; do
      sleep 1; waited=$((waited + 1)); [ $waited -ge 30 ] && { echo server_not_listening >"$dir/STATUS"; log "FAIL tcp listen"; return 1; }
    done
  fi
  local passflag=""
  [ "$mode" = benchmark ] && passflag="--ez benchmark true --es benchmark_result_path $app/output/benchmark.json"
  local env; env="$(arm_env "$arm")"; [ -n "$EXTRA_ENV" ] && env="${env:+$env;}$EXTRA_ENV"
  if [ "$mode" = benchmark ]; then
    shq "su -c 'nohup taskset 01 $SAMPLER $pkg 10 $app/output/benchmark.json /data/local/tmp/p14cpu.csv $TIMEOUT >/dev/null 2>&1 &'" >/dev/null 2>&1
  fi
  local am_out
  am_out=$(shq "am start -a top.mobilegl.plugin.TRACE_REPLAY -n $pkg/top.mobilegl.plugin.trace.TraceReplayActivity --es trace_path $app/input-$wl/trace.trace --es golden_path $app/input-$wl/golden.png --es output_dir $app/output --es backend $backend --el target_call $target --ei width $w --ei height $h --es ssim_threshold 0.99 --ez use_pbuffer true $passflag --ei crop_x $cx --ei crop_y $cy --ei crop_width $cw --ei crop_height $ch --es mobilegl_env '$env'" 2>&1)
  printf '%s\n' "$am_out" >"$dir/am-start.txt"
  local deadline=$((epoch0 + TIMEOUT)) got=0
  while [ "$(date +%s)" -lt "$deadline" ]; do
    if adb -s "$SER" exec-out "run-as $pkg cat $app/output/result.json" 2>/dev/null | head -c 400 | grep -q '"passed"'; then got=1; break; fi
    if [ $(( $(date +%s) - epoch0 )) -gt 25 ] && [ -z "$(shq "pidof $pkg" 2>/dev/null | tr -d '\r ')" ]; then status=process_died; break; fi
    sleep 2
  done
  [ $got -eq 1 ] && { status=finished; sleep 2; } || [ -n "$status" ] || status=timeout
  local t1; t1=$(temp_dc)
  adb -s "$SER" logcat -d >"$dir/logcat.txt" 2>/dev/null
  for f in result.json benchmark.json; do
    adb -s "$SER" exec-out "run-as $pkg cat $app/output/$f" >"$dir/$f" 2>/dev/null
    { [ -s "$dir/$f" ] && ! head -c 200 "$dir/$f" | grep -q "No such file"; } || rm -f "$dir/$f"
  done
  if [ "$(arm_kind "$arm")" = tcp ]; then
    for f in mgl.client.log mgl.server.log; do adb -s "$SER" exec-out "run-as $pkg cat /data/user/0/$pkg/files/$f" >"$dir/$f" 2>/dev/null; done
    python "$(cygpath -w "$REPO_ROOT/tools/trace_replay/tcp_device_server.py")" stop --serial "$SER" --package "$pkg" >/dev/null 2>&1
  fi
  if [ "$mode" = benchmark ]; then
    sleep 1
    adb -s "$SER" exec-out "su -c 'cat /data/local/tmp/p14cpu.csv'" >"$dir/cpu.csv" 2>/dev/null
    shq "su -c 'pkill -f cpusampler'" >/dev/null 2>&1
  fi
  shq "am force-stop $pkg" >/dev/null 2>&1
  local passed="none"; [ -f "$dir/result.json" ] && passed=$(sed -n 's/.*"passed": *\([a-z]*\).*/\1/p' "$dir/result.json" | head -1)
  echo "$status" >"$dir/STATUS"
  echo "tag=$TAG wl=$wl backend=$backend arm=$arm rep=$rep mode=$mode status=$status passed=$passed dur=$(( $(date +%s) - epoch0 )) temp0=$t0 temp1=$t1" >"$dir/meta.txt"
  verify_run "$dir" "$arm" "$backend"
  local fps="-"; [ -f "$dir/benchmark.json" ] && fps=$(sed -n 's/.*"fps": *\([0-9.]*\).*/\1/p' "$dir/benchmark.json" | head -1)
  log "END   $TAG $wl/$backend/$arm r$rep $mode status=$status passed=$passed fps=$fps temp=$t1 :: $(tr '\n' ' ' <"$dir/verify.txt")"
}

main() {
  log "=== matrix_p14 $1 tag=$TAG arms=${ARMS[*]} backends=${BACKENDS[*]} wls=${WLS[*]} reps=$REPS ==="
  shq "input keyevent KEYCODE_WAKEUP; svc power stayon usb" >/dev/null 2>&1
  case "$1" in
    prep) for p in ${PREP_PKGS:-$PKG_FEAT $PKG_DEV}; do prep_pkg "$p"; done ;;
    run)
      for wl in "${WLS[@]}"; do
        for ((r = 1; r <= REPS; r++)); do
          for backend in "${BACKENDS[@]}"; do
            for arm in "${ARMS[@]}"; do run_one "$wl" "$backend" "$arm" "$r" benchmark || true; done
          done
        done
      done ;;
    ssim)
      for wl in "${WLS[@]}"; do for backend in "${BACKENDS[@]}"; do for arm in "${ARMS[@]}"; do
        run_one "$wl" "$backend" "$arm" ssim ssim || true
      done; done; done ;;
  esac
  log "=== done ==="
}
main "${1:-run}"
