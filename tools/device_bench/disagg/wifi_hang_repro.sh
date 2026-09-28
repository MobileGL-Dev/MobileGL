#!/bin/bash
# Start one openra client and leave it hanging on MakeCurrent for stack capture.
set -u
BASE=~/wifi-perf
export MOBILEGL_TRANSPORT=spawn
export MOBILEGL_IPC_CONTROL=tcp://192.168.21.181:40613
export MOBILEGL_IPC_DATA=stream
export MOBILEGL_IPC_TOKEN=wifibench-token-2026-09
export MOBILEGL_IPC_RUN_AHEAD=1
export MOBILEGL_IPC_COLD_START_MS=180000
export MOBILEGL_PIPE_STATS=1
cd ~/MobileGL
rm -rf "$BASE/out/hang"; mkdir -p "$BASE/out/hang"
./build-split/tools/trace_replay/mobilegl_trace_replay \
  --trace "$BASE/traces/openra.trace" --backend DirectGLES \
  --mobilegl-library ./build-split/libMobileGL.so --pbuffer-surface \
  --benchmark --benchmark-result="$BASE/out/hang/benchmark.json" \
  --output "$BASE/out/hang" > "$BASE/out/hang/mobilegl.log" 2>&1 &
echo "client pid=$!"
sleep 40
echo "SLEEP_DONE - client still running: $(jobs -p)"
