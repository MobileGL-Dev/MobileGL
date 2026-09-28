#!/usr/bin/env bash
# Four-arm MobileGL transport performance matrix on device 2f7cbe2e.
# Arms (interleaved A B C D rounds): monolith / inproc / spawn+shm / spawn+tcp-localhost.
#
# Usage: run_matrix.sh [smoke|prep|openra|rd12|all]
#   smoke  - one openra monolith run to validate the pipeline, then exit
#   prep   - extract + push the rd12 fixture only
#   openra - openra: 4 arms x 3 perf reps (interleaved) + 1 ssim rep/arm (interleaved)
#   rd12   - minecraft rd12: perf reps (auto-drops to 2 if round 1 > 300s/run) + 1 ssim rep/arm
#   all    - openra + rd12 + summary (default)
#
# benchmark=true runs record fps/mean/median/p95 but never snapshot, so result.json
# carries ssim=-1; a separate non-benchmark round per workload supplies the ssim gate.
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
if command -v cygpath >/dev/null 2>&1; then REPO_ROOT_WIN=$(cygpath -w "$REPO_ROOT"); else REPO_ROOT_WIN="$REPO_ROOT"; fi
ROOT="${PERF_WORK:-$REPO_ROOT/.trace-work/perf-compare}"
SER="${SER:-2f7cbe2e}"
PKG=top.mobilegl.plugin.trace
APP=/data/user/0/$PKG/files/trace-replay
FILES=/data/user/0/$PKG/files
LOG="$ROOT/run_matrix.log"
FIXTURES="$REPO_ROOT/tools/trace_replay/fixtures"
OPENRA_TIMEOUT=${OPENRA_TIMEOUT:-900}
RD12_TIMEOUT=${RD12_TIMEOUT:-900}
OPENRA_REPS=${OPENRA_REPS:-3}
RD12_REPS=${RD12_REPS:-3}
ARMS=(monolith inproc shm tcp)

shq()    { adb -s "$SER" shell "$1"; }
execout(){ adb -s "$SER" exec-out "$1"; }

log() { local line; line="[$(date '+%F %T')] $*"; echo "$line" >>"$LOG"; echo "$line"; }

arm_env() {
  local stats="MOBILEGL_PIPE_STATS=1;MOBILEGL_PIPE_STATS_FILE=$APP/output/pipestats.json"
  local extra="${EXTRA_ENV:-}"
  case "$1" in
    monolith) echo "$stats$extra" ;;
    inproc)   echo "MOBILEGL_TRANSPORT=inproc;MOBILEGL_IPC_RUN_AHEAD=1;$stats$extra" ;;
    shm)      echo "MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_CONTROL=fork;MOBILEGL_IPC_DATA=auto;MOBILEGL_IPC_RUN_AHEAD=1;$stats$extra" ;;
    tcp)      echo "MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_CONTROL=tcp://127.0.0.1:40613;MOBILEGL_IPC_DATA=stream;MOBILEGL_IPC_RUN_AHEAD=1;$stats$extra" ;;
  esac
}

# Pull one run-as file to host; drops it when absent or when exec-out folded the
# device-side "No such file" error into the stdout stream (same rule as
# trace-replay-ci.sh's collect_role_logs).
pull() {
  local src="$1" dst="$2"
  rm -f "$dst"
  execout "run-as $PKG cat $src" >"$dst" 2>/dev/null || true
  if [ ! -s "$dst" ] || head -c 4096 "$dst" | grep -q "No such file"; then rm -f "$dst"; fi
}

# ---- PROFILING HOOKS (profile harness only; the source tree is untouched) ----
PROF_PIDS="$ROOT/.prof.pids"

# prof_start <dir>: launched right after am start (background). Attaches simpleperf
# (cpu-clock + callgraph) to the client pid and the spawn server pid, starts the
# /proc-syscall sampler on both, and arms ftrace sched_* for wakeup-latency capture.
prof_start() {
  local pdir="$1" waited=0 app="" server="" arm="${2:-}"
  mkdir -p "$pdir"
  : >"$PROF_PIDS"
  while [ "$waited" -lt 30 ]; do
    app=$(shq "pidof $PKG" 2>/dev/null | tr -d '\r' | awk '{print $1}')
    [ -n "${app:-}" ] && [ "$app" -gt 0 ] 2>/dev/null && break
    sleep 1; waited=$((waited + 1))
  done
  if [ -z "${app:-}" ]; then echo "no app pid" >"$pdir/prof-err.txt"; return 1; fi
  echo "app=$app" >"$pdir/prof-pids.txt"
  # ftrace sched capture (shell can write tracing_on; ring keeps the MOST RECENT events)
  shq "echo 2048 > /sys/kernel/tracing/buffer_size_kb;
       echo 1 > /sys/kernel/tracing/events/sched/sched_waking/enable;
       echo 1 > /sys/kernel/tracing/events/sched/sched_wakeup/enable;
       echo 1 > /sys/kernel/tracing/events/sched/sched_switch/enable;
       echo > /sys/kernel/tracing/trace;
       echo 1 > /sys/kernel/tracing/tracing_on" >/dev/null 2>&1
  # sampler binary into the app's files dir
  shq "run-as $PKG sh -c 'cp /data/local/tmp/sampler files/sampler 2>/dev/null; chmod 755 files/sampler'" >/dev/null 2>&1
  # spawn arms only: the server lives in a child process (pid is printed by the client log).
  if [ "$arm" != "monolith" ] && [ "$arm" != "inproc" ] && [ -n "$arm" ]; then
    waited=0
    while [ "$waited" -lt 25 ] && [ -z "$server" ]; do
      local line s
      line=$(execout "run-as $PKG cat $APP/output/mobilegl.client.log" 2>/dev/null | tr -d '\r')
      s=$(printf '%s' "$line" | grep -ao "runs in pid [0-9]*" | tail -1)
      [ -n "$s" ] && server="${s##* }"
      if [ -z "$server" ]; then
        s=$(printf '%s' "$line" | grep -ao "pid=[0-9]* dial" | tail -1)
        [ -n "$s" ] && server="${s#pid=}" && server="${server%% *}"
      fi
      [ -n "$server" ] && break
      sleep 1; waited=$((waited + 1))
    done
  fi
  echo "app=$app server=${server:-none} arm=$arm" >>"$pdir/prof-pids.txt"
  # simpleperf on the client (runs until prof_stop sends SIGINT; 600s safety cap)
  adb -s "$SER" shell "run-as $PKG simpleperf record -e cpu-clock -f 4000 -g --duration 600 -p $app -o $FILES/perf-client.data" \
    >"$pdir/perf-client.log" 2>&1 &
  echo "perf-client $!" >>"$PROF_PIDS"
  if [ -n "$server" ]; then
    adb -s "$SER" shell "run-as $PKG simpleperf record -e cpu-clock -f 4000 -g --duration 600 -p $server -o $FILES/perf-server.data" \
      >"$pdir/perf-server.log" 2>&1 &
    echo "perf-server $!" >>"$PROF_PIDS"
  fi
  # /proc syscall samplers: 2ms samples of every thread's blocked syscall
  adb -s "$SER" shell "run-as $PKG sh -c 'files/sampler $app 590 2 files/sampler-client.csv'" \
    >"$pdir/sampler-client.log" 2>&1 &
  echo "sampler-client $!" >>"$PROF_PIDS"
  if [ -n "$server" ]; then
    adb -s "$SER" shell "run-as $PKG sh -c 'files/sampler $server 590 2 files/sampler-server.csv'" \
      >"$pdir/sampler-server.log" 2>&1 &
    echo "sampler-server $!" >>"$PROF_PIDS"
  fi
  echo "started app=$app server=${server:-none}"
}

# prof_stop <dir>: terminate profilers, wait for simpleperf to finalize, pull artifacts.
prof_stop() {
  local pdir="$1"
  [ -d "$pdir" ] || return 0
  shq "run-as $PKG pkill -TERM sampler" >/dev/null 2>&1 || true
  sleep 1
  shq "run-as $PKG pkill -INT simpleperf" >/dev/null 2>&1 || true
  # simpleperf needs time to write the PERFILE2 trailer; poll the magic instead of a fixed sleep.
  local w=0 magic=""
  while [ "$w" -lt 60 ]; do
    magic=$(execout "run-as $PKG sh -c 'head -c 8 $FILES/perf-client.data 2>/dev/null'" 2>/dev/null | tr -d '\0\r\n')
    [ "$magic" = "PERFILE2" ] && break
    shq "run-as $PKG pkill -INT simpleperf" >/dev/null 2>&1 || true
    sleep 2; w=$((w + 2))
  done
  sleep 2
  shq "echo 0 > /sys/kernel/tracing/tracing_on;
       echo 0 > /sys/kernel/tracing/events/sched/sched_waking/enable;
       echo 0 > /sys/kernel/tracing/events/sched/sched_wakeup/enable;
       echo 0 > /sys/kernel/tracing/events/sched/sched_switch/enable" >/dev/null 2>&1 || true
  pull "$FILES/perf-client.data"  "$pdir/perf-client.data"
  pull "$FILES/perf-server.data"  "$pdir/perf-server.data"
  pull "$FILES/sampler-client.csv" "$pdir/sampler-client.csv"
  pull "$FILES/sampler-server.csv" "$pdir/sampler-server.csv"
  execout "cat /sys/kernel/tracing/trace" >"$pdir/ftrace-sched.txt" 2>/dev/null || true
  shq "run-as $PKG pkill -TERM sampler; run-as $PKG pkill -TERM simpleperf" >/dev/null 2>&1 || true
  if [ -f "$PROF_PIDS" ]; then
    while read -r _ job; do [ -n "${job:-}" ] && kill "$job" 2>/dev/null || true; done <"$PROF_PIDS"
    rm -f "$PROF_PIDS"
  fi
  ls -la "$pdir" >"$pdir/manifest.txt" 2>&1
}

extract_lines() {
  # $1 dir; pulls the two counter-line families out of every pulled log on the host.
  local dir="$1" f
  : >"$dir/pipestats-lines.txt"
  : >"$dir/linkmetrics-lines.txt"
  for f in "$dir"/logcat.txt "$dir"/mobilegl.client.log "$dir"/mobilegl.server.log \
           "$dir"/mgl.client.log "$dir"/mgl.server.log; do
    [ -f "$f" ] || continue
    grep -a "MGPipe stats:" "$f" 2>/dev/null | sed "s|^|[$(basename "$f")] |" >>"$dir/pipestats-lines.txt"
    grep -a "P65LinkMetrics" "$f" 2>/dev/null | sed "s|^|[$(basename "$f")] |" >>"$dir/linkmetrics-lines.txt"
  done
}

verify_run() {
  local dir="$1" arm="$2"
  local all="$dir/all-marker-evidence.txt"
  : >"$all"
  local f
  for f in "$dir"/logcat.txt "$dir"/mobilegl.client.log "$dir"/mobilegl.server.log \
           "$dir"/mgl.client.log "$dir"/mgl.server.log; do
    [ -f "$f" ] && grep -aE "Config: MOBILEGL_TRANSPORT|spawn ARMED|control=tcp data=stream|run-ahead ARMED|running lockstep|DISARMED|Config: IPC|MGPipe stats:" "$f" 2>/dev/null | sed "s|^|[$(basename "$f")] |" >>"$all"
  done
  local v="$dir/verify.txt" nstats nfatal
  nstats=$(grep -ac "MGPipe stats:" "$all" 2>/dev/null || true); [ -n "$nstats" ] || nstats=0
  nfatal=$(grep -ac "Fatal{" "$all" 2>/dev/null || true); [ -n "$nfatal" ] || nfatal=0
  {
    echo "arm=$arm"
    case "$arm" in
      inproc)
        grep -aq "Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream" "$all" \
          && echo "TRANSPORT_MARKER=inproc:OK" || echo "TRANSPORT_MARKER=inproc:MISSING" ;;
      shm)
        grep -aq "Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream" "$all" \
          && echo "TRANSPORT_MARKER=spawn:OK" || echo "TRANSPORT_MARKER=spawn:MISSING"
        grep -aq "spawn ARMED - the server role runs in pid" "$all" \
          && echo "SECOND_PROCESS_SPAWN_ARMED=OK: $(grep -a "spawn ARMED" "$all" | head -1)" \
          || echo "SECOND_PROCESS_SPAWN_ARMED=MISSING" ;;
      tcp)
        grep -aq "Config: MOBILEGL_TRANSPORT=spawn - the MGPipe record stream" "$all" \
          && echo "TRANSPORT_MARKER=spawn:OK" || echo "TRANSPORT_MARKER=spawn:MISSING"
        grep -aq "control=tcp data=stream server=" "$all" \
          && echo "TCP_SESSION=OK: $(grep -a "control=tcp data=stream" "$all" | head -1)" \
          || echo "TCP_SESSION=MISSING" ;;
      monolith)
        if grep -aqE "Config: MOBILEGL_TRANSPORT=(inproc|spawn)" "$all"; then
          echo "SPLIT_MARKER=UNEXPECTED_PRESENT"
        else
          echo "SPLIT_MARKER=absent (monolith:OK)"
        fi ;;
    esac
    if grep -aq "run-ahead ARMED" "$all"; then
      echo "RUN_AHEAD_ARMED=OK: $(grep -a "run-ahead ARMED" "$all" | head -1)"
    else
      echo "RUN_AHEAD_ARMED=MISSING"
    fi
    if grep -aq "running lockstep" "$all"; then
      echo "LOCKSTEP_WARNING=PRESENT: $(grep -a "running lockstep" "$all" | head -1)"
    else
      echo "LOCKSTEP_WARNING=absent"
    fi
    if grep -aqi "DISARMED" "$all"; then
      echo "DISARMED=PRESENT: $(grep -ai "DISARMED" "$all" | head -1)"
    else
      echo "DISARMED=absent"
    fi
    echo "PIPESTATS_LINES=$nstats"
    echo "FATAL_LINES=$nfatal"
  } >"$v"
}

# run_one <wl> <arm> <rep> <trace> <golden> <w> <h> <target> <timeout> <mode:benchmark|ssim> [crop_x crop_y crop_w crop_h]
run_one() {
  local wl="$1" arm="$2" rep="$3" trace="$4" golden="$5" w="$6" h="$7" target="$8" timeout_s="$9" mode="${10}"
  local cx="${11:-0}" cy="${12:-0}" cw="${13:-0}" ch="${14:-0}"
  local dir="$ROOT/$wl/$arm-r$rep"
  rm -rf "$dir"; mkdir -p "$dir"
  local epoch0 dur temp0 am_out status="" detail="" passflag=""
  if [ "$mode" = "benchmark" ]; then passflag="--ez benchmark true --es benchmark_result_path $APP/output/benchmark.json"; fi
  epoch0=$(date +%s)
  log "BEGIN $wl/$arm rep$rep mode=$mode (timeout ${timeout_s}s)"
  shq "am force-stop $PKG" >/dev/null 2>&1 || true
  shq "run-as $PKG sh -c 'rm -f $APP/output/result.json $APP/output/benchmark.json $APP/output/mobilegl.client.log $APP/output/mobilegl.server.log $APP/output/pipestats.client.json $APP/output/pipestats.server.json $APP/output/retrace.log $FILES/mgl.client.log $FILES/mgl.server.log'" >/dev/null 2>&1 || true
  adb -s "$SER" logcat -c >/dev/null 2>&1 || true
  temp0=$(adb -s "$SER" shell dumpsys battery 2>/dev/null | tr -d '\r' | sed -n 's/.*temperature: \([0-9]*\).*/\1/p' | head -1)

  if [ "$arm" = "tcp" ]; then
    if ! python "$REPO_ROOT_WIN/tools/trace_replay/tcp_device_server.py" start \
         --serial "$SER" --package "$PKG" --backend DirectGLES --listen tcp://127.0.0.1:40613 \
         --env "MOBILEGL_PIPE_STATS=1" --env "MOBILEGL_PIPE_STATS_FILE=$FILES/pipestats.json" \
         ${EXTRA_SERVER_ENV:+--env "$EXTRA_SERVER_ENV"} \
         >"$dir/server-start.log" 2>&1; then
      dur=$(( $(date +%s) - epoch0 )); status="server_start_failed"
      { echo "arm=$arm rep=$rep workload=$wl duration_s=$dur"; cat "$dir/server-start.log"; } >"$dir/meta.txt"
      echo "$status" >"$dir/STATUS"; log "FAIL $wl/$arm rep$rep: tcp server start"; return 1
    fi
    local waited=0 listening=0
    while [ "$waited" -lt 30 ]; do
      if adb -s "$SER" logcat -d -s MobileGL:* MobileGLServer:* 2>/dev/null | tr -d '\r' | grep -q "listening on tcp://127.0.0.1:40613"; then
        listening=1; break
      fi
      sleep 1; waited=$((waited + 1))
    done
    if [ "$listening" -ne 1 ]; then
      dur=$(( $(date +%s) - epoch0 )); status="server_not_listening"
      adb -s "$SER" logcat -d -s MobileGL:* MobileGLServer:* >"$dir/server-logcat.txt" 2>/dev/null || true
      { echo "arm=$arm rep=$rep workload=$wl duration_s=$dur listening_wait_s=$waited"; } >"$dir/meta.txt"
      echo "$status" >"$dir/STATUS"; log "FAIL $wl/$arm rep$rep: tcp server not listening after ${waited}s"; return 1
    fi
    log "tcp supervisor listening after ${waited}s"
  fi

  local envarg
  envarg=$(arm_env "$arm")
  am_out=$(shq "am start -a top.mobilegl.plugin.TRACE_REPLAY -n $PKG/top.mobilegl.plugin.trace.TraceReplayActivity --es trace_path $trace --es golden_path $golden --es output_dir $APP/output --es backend DirectGLES --el target_call $target --ei width $w --ei height $h --es ssim_threshold 0.99 --ez use_pbuffer true $passflag --ei crop_x $cx --ei crop_y $cy --ei crop_width $cw --ei crop_height $ch --es mobilegl_env '${envarg}'" 2>&1)
  printf '%s\n' "$am_out" >"$dir/am-start.txt"
  if printf '%s' "$am_out" | grep -qE "Error type|Exception|does not exist|Unknown"; then
    dur=$(( $(date +%s) - epoch0 )); status="am_start_failed"
    { echo "arm=$arm rep=$rep workload=$wl duration_s=$dur"; echo "$am_out"; } >"$dir/meta.txt"
    echo "$status" >"$dir/STATUS"; log "FAIL $wl/$arm rep$rep: am start error: $am_out"; return 1
  fi

  local deadline=$(( epoch0 + timeout_s )) got=0
  # ---- PROF hooks: attach simpleperf (client + server) and the /proc sampler ----
  if [ "${PROF:-0}" = "1" ]; then
    local pdir="$ROOT/prof-$arm-r$rep"
    mkdir -p "$pdir"
    echo "$pdir" >"$ROOT/PROF_LAST_DIR"
    ( prof_start "$pdir" "$arm" ) >"$pdir/prof-start.log" 2>&1 &
  fi
  while [ "$(date +%s)" -lt "$deadline" ]; do
    if execout "run-as $PKG cat $APP/output/result.json" 2>/dev/null | head -c 400 | grep -q '"passed"'; then
      got=1; break
    fi
    if [ $(( $(date +%s) - epoch0 )) -gt 25 ] && [ -z "$(adb -s "$SER" shell pidof $PKG 2>/dev/null | tr -d '\r' | tr -d ' ')" ]; then
      status="process_died"; break
    fi
    sleep 4
  done
  if [ "$got" -eq 1 ]; then status="finished"; sleep 3; elif [ -z "$status" ]; then status="timeout"; fi
  if [ "${PROF:-0}" = "1" ] && [ -f "$ROOT/PROF_LAST_DIR" ]; then
    prof_stop "$(cat "$ROOT/PROF_LAST_DIR")" >>"$(cat "$ROOT/PROF_LAST_DIR")/prof-start.log" 2>&1 || true
  fi

  dur=$(( $(date +%s) - epoch0 ))
  adb -s "$SER" logcat -d >"$dir/logcat.txt" 2>/dev/null || true
  pull "$APP/output/result.json"          "$dir/result.json"
  pull "$APP/output/benchmark.json"       "$dir/benchmark.json"
  pull "$APP/output/mobilegl.client.log"  "$dir/mobilegl.client.log"
  pull "$APP/output/mobilegl.server.log"  "$dir/mobilegl.server.log"
  pull "$APP/output/pipestats.client.json" "$dir/pipestats.client.json"
  pull "$APP/output/pipestats.server.json" "$dir/pipestats.server.json"
  pull "$APP/output/retrace.log"          "$dir/retrace.log"
  if [ "$arm" = "tcp" ]; then
    pull "$FILES/mgl.client.log" "$dir/mgl.client.log"
    pull "$FILES/mgl.server.log" "$dir/mgl.server.log"
    pull "$FILES/pipestats.server.json" "$dir/tcp-server-pipestats.json"
  fi

  local passed="none"
  if [ -f "$dir/result.json" ]; then
    passed=$(sed -n 's/.*"passed": *\([a-z]*\).*/\1/p' "$dir/result.json" | head -1)
    [ -n "$passed" ] || passed="unparsed"
  fi
  if [ "$passed" != "true" ] && [ "$status" = "finished" ]; then status="result_not_passed"; fi
  { echo "arm=$arm rep=$rep workload=$wl duration_s=$dur battery_temp_dc=$temp0 status=$status passed=$passed mode=$mode"
    echo "--- am start ---"; printf '%s\n' "$am_out"; } >"$dir/meta.txt"
  echo "$status" >"$dir/STATUS"
  extract_lines "$dir"
  verify_run "$dir" "$arm"
  local vlines
  vlines=$(tr '\n' '; ' <"$dir/verify.txt")
  log "END   $wl/$arm rep$rep mode=$mode status=$status passed=$passed dur=${dur}s temp=$temp0 :: $vlines"
  [ "$status" = "finished" ] || return 1
  return 0
}

check_openra() {
  if ! shq "run-as $PKG sh -c 'test -f $APP/input/trace.trace && test -f $APP/input/golden.png'" >/dev/null 2>&1; then
    log "FATAL: openra fixture missing on device ($APP/input)"; exit 1
  fi
}

prep_rd12() {
  local host_trace="$ROOT/fixtures/trace.trace"
  local host_golden="$FIXTURES/minecraft-1.21.4-rd12-odinlite-in-world.0004660351.png"
  [ -f "$host_trace" ] || { tar xzf "$FIXTURES/minecraft-1.21.4-rd12-odinlite-in-world.tgz" -C "$ROOT/fixtures" || exit 1; }
  [ -f "$host_golden" ] || { log "FATAL: rd12 golden missing: $host_golden"; exit 1; }
  shq "run-as $PKG sh -c 'mkdir -p $APP/input-rd12'" || true
  log "pushing rd12 fixture ($(wc -c <"$host_trace") bytes)"
  adb -s "$SER" push "$(cygpath -w "$host_trace")" /data/local/tmp/mgl-rd12.trace >/dev/null || exit 1
  adb -s "$SER" push "$(cygpath -w "$host_golden")" /data/local/tmp/mgl-rd12-golden.png >/dev/null || exit 1
  if ! shq "run-as $PKG sh -c 'cp /data/local/tmp/mgl-rd12.trace $APP/input-rd12/trace.trace'"; then
    log "run-as cp failed; streaming via cat pipe"
    shq "cat /data/local/tmp/mgl-rd12.trace | run-as $PKG sh -c 'cat > $APP/input-rd12/trace.trace'" || exit 1
  fi
  shq "run-as $PKG sh -c 'cp /data/local/tmp/mgl-rd12-golden.png $APP/input-rd12/golden.png'" \
    || shq "cat /data/local/tmp/mgl-rd12-golden.png | run-as $PKG sh -c 'cat > $APP/input-rd12/golden.png'" || exit 1
  shq "rm -f /data/local/tmp/mgl-rd12.trace /data/local/tmp/mgl-rd12-golden.png" >/dev/null 2>&1 || true
  local size hostsize
  size=$(shq "run-as $PKG sh -c 'wc -c < $APP/input-rd12/trace.trace'" | tr -d '\r ')
  hostsize=$(wc -c <"$host_trace")
  if [ "$size" != "$hostsize" ]; then log "FATAL: rd12 trace size mismatch device=$size host=$hostsize"; exit 1; fi
  log "rd12 fixture on device: $size bytes"
}

round_max_duration() {
  local wl="$1" round="$2" max=0 a d
  for a in "${ARMS[@]}"; do
    d=$(sed -n 's/.*duration_s=\([0-9]*\).*/\1/p' "$ROOT/$wl/$a-r$round/meta.txt" 2>/dev/null | head -1)
    [ -n "${d:-}" ] && [ "$d" -gt "$max" ] && max=$d
  done
  echo "$max"
}

run_perf() {
  local wl="$1" trace="$2" golden="$3" w="$4" h="$5" target="$6" reps="$7" timeout_s="$8" crop="$9"
  local round=1
  while [ "$round" -le "$reps" ]; do
    local arm
    for arm in "${ARMS[@]}"; do
      run_one "$wl" "$arm" "$round" "$trace" "$golden" "$w" "$h" "$target" "$timeout_s" benchmark $crop || true
    done
    if [ "$wl" = "rd12" ] && [ "$round" -eq 1 ] && [ "$reps" -gt 2 ]; then
      local maxd; maxd=$(round_max_duration "$wl" 1)
      if [ "$maxd" -gt 300 ]; then
        reps=2
        log "rd12 round1 max run ${maxd}s > 300s -> reduced to 2 reps/arm (will be noted in report)"
      fi
    fi
    round=$((round + 1))
  done
}

run_ssim() {
  # One non-benchmark run per arm, interleaved: replays to target_call, snapshots and
  # compares against the golden. Supplies the ssim column the benchmark lane cannot.
  local wl="$1" trace="$2" golden="$3" w="$4" h="$5" target="$6" timeout_s="$7" crop="$8"
  local arm
  for arm in "${ARMS[@]}"; do
    run_one "$wl" "$arm" "ssim" "$trace" "$golden" "$w" "$h" "$target" "$timeout_s" ssim $crop || true
  done
}

summarize() {
  local winroot="$ROOT"
  command -v cygpath >/dev/null 2>&1 && winroot=$(cygpath -w "$ROOT")
  python - "$winroot" <<'PYEOF'
import json, re, sys, statistics
from pathlib import Path
root = Path(sys.argv[1])
rows = []
for wl_dir in sorted(p for p in root.iterdir() if p.is_dir() and p.name in ("openra", "rd12")):
    for run_dir in sorted(wl_dir.glob("*-r*")):
        m = re.fullmatch(r"(.+)-r(\d+|ssim)", run_dir.name)
        if not m:
            continue
        arm, rep = m.group(1), m.group(2)
        meta = (run_dir / "meta.txt").read_text(encoding="utf-8", errors="replace") if (run_dir / "meta.txt").exists() else ""
        dur = re.search(r"duration_s=(\d+)", meta)
        temp = re.search(r"battery_temp_dc=(\d+)", meta)
        mode = re.search(r"mode=(\w+)", meta)
        status = (run_dir / "STATUS").read_text().strip() if (run_dir / "STATUS").exists() else "missing"
        bench = res = None
        if (run_dir / "benchmark.json").exists():
            try:
                bench = json.loads((run_dir / "benchmark.json").read_text(encoding="utf-8", errors="replace"))
            except Exception:
                bench = None
        if (run_dir / "result.json").exists():
            try:
                res = json.loads((run_dir / "result.json").read_text(encoding="utf-8", errors="replace"))
            except Exception:
                res = None
        verify = {}
        if (run_dir / "verify.txt").exists():
            for line in (run_dir / "verify.txt").read_text(encoding="utf-8", errors="replace").splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    verify[k] = v
        rows.append({
            "workload": wl_dir.name, "arm": arm, "rep": rep,
            "mode": mode.group(1) if mode else "?",
            "status": status,
            "duration_s": int(dur.group(1)) if dur else None,
            "battery_temp_dc": int(temp.group(1)) if temp else None,
            "fps": bench.get("fps") if bench else None,
            "mean_ms": bench.get("meanFrameMs") if bench else None,
            "median_ms": bench.get("medianFrameMs") if bench else None,
            "p95_ms": bench.get("p95FrameMs") if bench else None,
            "total_frames": bench.get("totalFrames") if bench else None,
            "total_seconds": bench.get("totalSeconds") if bench else None,
            "passed": res.get("passed") if res else None,
            "ssim": res.get("ssim") if res else None,
            "message": (res.get("message") if res else None),
            "verify": verify,
        })

agg = []
for wl in sorted({r["workload"] for r in rows}):
    arms = {}
    for r in rows:
        if (r["workload"] == wl and r["mode"] == "benchmark" and r["status"] == "finished"
                and r["mean_ms"] and r["mean_ms"] > 0):
            arms.setdefault(r["arm"], []).append(r)
    for arm, runs in arms.items():
        means = sorted(r["mean_ms"] for r in runs)
        best = min(runs, key=lambda r: r["mean_ms"])
        agg.append({
            "workload": wl, "arm": arm, "n_ok": len(runs),
            "best_mean_ms": best["mean_ms"], "best_median_ms": best["median_ms"],
            "best_p95_ms": best["p95_ms"], "best_fps": best["fps"],
            "median_of_mean_ms": statistics.median(means),
            "spread_mean_ms": [means[0], means[-1]],
            "spread_pct": round(100.0 * (means[-1] - means[0]) / statistics.median(means), 2) if len(means) > 1 else 0.0,
        })
out = {"runs": rows, "aggregates": agg}
(root / "summary.json").write_text(json.dumps(out, indent=2), encoding="utf-8")
print(f"wrote {root / 'summary.json'} ({len(rows)} runs, {len(agg)} aggregates)")
PYEOF
}

main() {
  local mode="${1:-all}"
  mkdir -p "$ROOT"
  log "=== run_matrix mode=$mode ==="
  adb -s "$SER" get-state >/dev/null 2>&1 || { log "FATAL: device $SER not connected"; exit 1; }
  shq "input keyevent KEYCODE_WAKEUP" >/dev/null 2>&1 || true
  shq "wm dismiss-keyguard" >/dev/null 2>&1 || true
  shq "svc stayon usb" >/dev/null 2>&1 || true
  check_openra
  case "$mode" in
    smoke)
      run_one openra monolith 0 "$APP/input/trace.trace" "$APP/input/golden.png" 640 480 31249 "$OPENRA_TIMEOUT" benchmark 1 1 638 478 || true
      summarize ;;
    prep) prep_rd12 ;;
    one)
      # one single run: ONE=<arm> [WL=openra|rd12] [REP=n]
      local wl="${WL:-openra}" arm="${ONE:-tcp}" rep="${REP:-99}"
      if [ "$wl" = "openra" ]; then
        run_one openra "$arm" "$rep" "$APP/input/trace.trace" "$APP/input/golden.png" 640 480 31249 "$OPENRA_TIMEOUT" benchmark 1 1 638 478 || true
      else
        prep_rd12
        run_one rd12 "$arm" "$rep" "$APP/input-rd12/trace.trace" "$APP/input-rd12/golden.png" 854 480 4660351 "$RD12_TIMEOUT" benchmark 0 0 0 0 || true
      fi
      summarize ;;
    ssimonly)
      run_ssim openra "$APP/input/trace.trace" "$APP/input/golden.png" 640 480 31249 "$OPENRA_TIMEOUT" "1 1 638 478"
      summarize ;;
    openra)
      run_perf openra "$APP/input/trace.trace" "$APP/input/golden.png" 640 480 31249 "$OPENRA_REPS" "$OPENRA_TIMEOUT" "1 1 638 478"
      run_ssim openra "$APP/input/trace.trace" "$APP/input/golden.png" 640 480 31249 "$OPENRA_TIMEOUT" "1 1 638 478"
      summarize ;;
    rd12)
      prep_rd12
      run_perf rd12 "$APP/input-rd12/trace.trace" "$APP/input-rd12/golden.png" 854 480 4660351 "$RD12_REPS" "$RD12_TIMEOUT" "0 0 0 0"
      run_ssim rd12 "$APP/input-rd12/trace.trace" "$APP/input-rd12/golden.png" 854 480 4660351 "$RD12_TIMEOUT" "0 0 0 0"
      shq "am force-stop $PKG" >/dev/null 2>&1 || true
      summarize ;;
    all)
      run_perf openra "$APP/input/trace.trace" "$APP/input/golden.png" 640 480 31249 "$OPENRA_REPS" "$OPENRA_TIMEOUT" "1 1 638 478"
      run_ssim openra "$APP/input/trace.trace" "$APP/input/golden.png" 640 480 31249 "$OPENRA_TIMEOUT" "1 1 638 478"
      prep_rd12
      run_perf rd12 "$APP/input-rd12/trace.trace" "$APP/input-rd12/golden.png" 854 480 4660351 "$RD12_REPS" "$RD12_TIMEOUT" "0 0 0 0"
      run_ssim rd12 "$APP/input-rd12/trace.trace" "$APP/input-rd12/golden.png" 854 480 4660351 "$RD12_TIMEOUT" "0 0 0 0"
      shq "am force-stop $PKG" >/dev/null 2>&1 || true
      summarize ;;
    *) echo "unknown mode: $mode" >&2; exit 2 ;;
  esac
  log "=== run_matrix mode=$mode DONE ==="
}

main "${1:-all}"
