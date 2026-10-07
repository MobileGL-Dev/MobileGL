#!/usr/bin/env bash
# One simpleperf profile of a matrix_p14 arm.
#   ARM=monolith BACKEND=DirectVulkan WL=rd12 TAG=prof SYMDIR=<unstripped libs> bash profile_p14.sh
# Records the whole run (every thread of the app's processes, the spawn/tcp render server
# included, via --app) with frame-pointer call graphs, stops when benchmark.json appears, and
# builds a binary cache against the unstripped libraries in SYMDIR. The steady window is cut out
# afterwards: components.py / callee_split.py take --tail-ms (the steady frames' wall span from
# benchmark.json), so no launch-to-steady delay has to be guessed. Clocks are expected to be
# pinned by the caller (pin_clocks.sh).
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SER="${SER:-HA27Q3LQ}"
ARM="${ARM:-monolith}" BACKEND="${BACKEND:-DirectVulkan}" WL="${WL:-rd12}" TAG="${TAG:-prof}"
FREQ="${FREQ:-2000}"
PKG_FEAT="${PKG_FEAT:-top.mobilegl.plugin.p14.trace}" PKG_DEV="${PKG_DEV:-top.mobilegl.plugin.p14dev.trace}"
SYMDIR="${SYMDIR:?unstripped library directory}"
NDK="${NDK:-/c/Users/yello/AppData/Local/Android/Sdk/ndk/27.3.13750724}"
OUT="${PERF_WORK:-$REPO_ROOT/.trace-work/perf-p14}/$TAG/$WL-$BACKEND-$ARM"
mkdir -p "$OUT"
pkg=$([ "$ARM" = dev ] && echo "$PKG_DEV" || echo "$PKG_FEAT")
app=/data/user/0/$pkg/files/trace-replay
case "$ARM" in
  dev|monolith) env="" ;;
  inproc) env="MOBILEGL_TRANSPORT=inproc;MOBILEGL_IPC_RUN_AHEAD=1" ;;
  shm) env="MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_CONTROL=fork;MOBILEGL_IPC_DATA=auto;MOBILEGL_IPC_RUN_AHEAD=1" ;;
esac
[ -n "${EXTRA_ENV:-}" ] && env="${env:+$env;}$EXTRA_ENV"
case "$WL" in
  openra) args="--el target_call 31249 --ei width 640 --ei height 480" ;;
  rd12) args="--el target_call 4660351 --ei width 854 --ei height 480" ;;
esac
adb -s "$SER" shell "am force-stop $PKG_FEAT; am force-stop $PKG_DEV; run-as $pkg sh -c 'rm -f $app/output/*.json'" >/dev/null 2>&1
adb -s "$SER" shell "su -c 'pkill -INT simpleperf; rm -f /data/local/tmp/p14perf.data'" >/dev/null 2>&1
# --app follows the package's processes and their new threads from the moment they exist.
adb -s "$SER" shell "su -c 'simpleperf record --app $pkg -e cpu-clock --call-graph fp -f $FREQ -o /data/local/tmp/p14perf.data'" >"$OUT/record.log" 2>&1 &
sp_pid=$!
sleep 2
adb -s "$SER" shell "am start -a top.mobilegl.plugin.TRACE_REPLAY -n $pkg/top.mobilegl.plugin.trace.TraceReplayActivity --es trace_path $app/input-$WL/trace.trace --es golden_path $app/input-$WL/golden.png --es output_dir $app/output --es backend $BACKEND $args --ez use_pbuffer true --ez benchmark true --es benchmark_result_path $app/output/benchmark.json --es mobilegl_env '$env'" >/dev/null
waited=0
until adb -s "$SER" exec-out "run-as $pkg cat $app/output/benchmark.json" 2>/dev/null | grep -q '"fps"'; do
  sleep 1; waited=$((waited + 1)); [ "$waited" -ge 600 ] && break
done
adb -s "$SER" shell "su -c 'pkill -INT simpleperf'" >/dev/null 2>&1
wait "$sp_pid"
adb -s "$SER" exec-out "run-as $pkg cat $app/output/benchmark.json" >"$OUT/benchmark.json" 2>/dev/null
adb -s "$SER" shell "am force-stop $pkg" >/dev/null 2>&1
adb -s "$SER" shell "su -c 'chmod 644 /data/local/tmp/p14perf.data'"
adb -s "$SER" pull /data/local/tmp/p14perf.data "$(cygpath -w "$OUT/perf.data")" >/dev/null
W_SYM=$(cygpath -w "$SYMDIR")
# binary_cache/ mirrors the device paths: our unstripped libraries by build id, the rest pulled.
(cd "$OUT" && ANDROID_SERIAL="$SER" python "$(cygpath -w "$NDK/simpleperf/binary_cache_builder.py")" -i perf.data -lib "$W_SYM" >binary_cache.log 2>&1)
tail -1 "$OUT/record.log"
echo "wrote $OUT (steady tail: $(python -c "import json,sys; f=json.load(open(sys.argv[1]))['frameTimesMs']; print(round(sum(f[4:]),1))" "$(cygpath -w "$OUT/benchmark.json")") ms)"
