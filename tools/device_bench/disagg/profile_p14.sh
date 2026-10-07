#!/usr/bin/env bash
# One simpleperf profile of a matrix_p14 arm, recorded inside the steady window.
#   ARM=monolith BACKEND=DirectVulkan WL=rd12 DELAY=30 DUR=6 TAG=prof bash profile_p14.sh
# DELAY is the seconds from launch to the start of recording (past the load frame), DUR the
# recording length. Records every thread of the replay process (and of the server process for
# spawn arms) with frame-pointer call graphs, then reports on the host against the unstripped
# libraries in SYMDIR. Clocks are expected to be pinned by the caller (pin_clocks.sh).
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SER="${SER:-HA27Q3LQ}"
ARM="${ARM:-monolith}" BACKEND="${BACKEND:-DirectVulkan}" WL="${WL:-rd12}"
DELAY="${DELAY:-30}" DUR="${DUR:-6}" TAG="${TAG:-prof}"
PKG_FEAT="${PKG_FEAT:-top.mobilegl.plugin.p14.trace}" PKG_DEV="${PKG_DEV:-top.mobilegl.plugin.p14dev.trace}"
SYMDIR="${SYMDIR:?unstripped library directory}"
NDK="${NDK:-/c/Users/yello/AppData/Local/Android/Sdk/ndk/27.3.13750724}"
SP_HOST="$NDK/simpleperf/bin/windows/x86_64/simpleperf.exe"
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
adb -s "$SER" shell "am start -a top.mobilegl.plugin.TRACE_REPLAY -n $pkg/top.mobilegl.plugin.trace.TraceReplayActivity --es trace_path $app/input-$WL/trace.trace --es golden_path $app/input-$WL/golden.png --es output_dir $app/output --es backend $BACKEND $args --ez use_pbuffer true --ez benchmark true --es benchmark_result_path $app/output/benchmark.json --es mobilegl_env '$env'" >/dev/null
sleep "$DELAY"
pids=$(adb -s "$SER" shell "su -c 'echo \$(pidof $pkg) \$(pgrep -f $pkg.*libMobileGLServer.so)'" | tr -d '\r' | tr -s ' ' ',' | sed 's/^,//; s/,$//')
echo "pids=$pids" | tee "$OUT/pids.txt"
adb -s "$SER" shell "su -c 'simpleperf record -e cpu-clock -p $pids --call-graph fp -f 4000 --duration $DUR -o /data/local/tmp/p14perf.data'" >"$OUT/record.log" 2>&1
adb -s "$SER" exec-out "run-as $pkg cat $app/output/benchmark.json" >"$OUT/benchmark.json" 2>/dev/null
until adb -s "$SER" exec-out "run-as $pkg cat $app/output/benchmark.json" 2>/dev/null | grep -q '"fps"'; do sleep 2; done
adb -s "$SER" exec-out "run-as $pkg cat $app/output/benchmark.json" >"$OUT/benchmark.json" 2>/dev/null
adb -s "$SER" shell "am force-stop $pkg" >/dev/null 2>&1
adb -s "$SER" shell "su -c 'chmod 644 /data/local/tmp/p14perf.data'"
adb -s "$SER" pull /data/local/tmp/p14perf.data "$(cygpath -w "$OUT/perf.data")" >/dev/null
W_OUT=$(cygpath -w "$OUT"); W_SYM=$(cygpath -w "$SYMDIR")
# binary_cache/ mirrors the device paths: our unstripped libraries by build id, the rest pulled.
(cd "$OUT" && ANDROID_SERIAL="$SER" python "$(cygpath -w "$NDK/simpleperf/binary_cache_builder.py")" -i perf.data -lib "$W_SYM" >binary_cache.log 2>&1)
"$SP_HOST" report -i "$W_OUT\perf.data" --symfs "$W_OUT\binary_cache" --sort comm,tid --percent-limit 1 >"$OUT/threads.txt" 2>/dev/null
"$SP_HOST" report -i "$W_OUT\perf.data" --symfs "$W_OUT\binary_cache" --sort comm,symbol --percent-limit 0.3 >"$OUT/self.txt" 2>/dev/null
"$SP_HOST" report -i "$W_OUT\perf.data" --symfs "$W_OUT\binary_cache" --children --sort comm,symbol --percent-limit 1.5 >"$OUT/children.txt" 2>/dev/null
echo "wrote $OUT"; head -30 "$OUT/threads.txt"
