#!/bin/bash
# Repro hang, then map child fds -> socket inodes and dump per-socket rx queues + supervisor fds.
set -u
BASE=~/wifi-perf
export MOBILEGL_TRANSPORT=spawn
export MOBILEGL_IPC_CONTROL=tcp://192.168.21.181:40613
export MOBILEGL_IPC_DATA=stream
export MOBILEGL_IPC_TOKEN=wifibench-token-2026-09
export MOBILEGL_IPC_RUN_AHEAD=1
export MOBILEGL_IPC_COLD_START_MS=90000
export MOBILEGL_PIPE_STATS=1
cd ~/MobileGL
rm -rf "$BASE/out/fdprobe"; mkdir -p "$BASE/out/fdprobe"
./build-split/tools/trace_replay/mobilegl_trace_replay \
  --trace "$BASE/traces/openra.trace" --backend DirectGLES \
  --mobilegl-library ./build-split/libMobileGL.so --pbuffer-surface \
  --benchmark --benchmark-result="$BASE/out/fdprobe/benchmark.json" \
  --output "$BASE/out/fdprobe" > "$BASE/out/fdprobe/mobilegl.log" 2>&1 &
CPID=$!
echo "client pid=$CPID"
for i in $(seq 1 60); do
  grep -q "MobileGL initialized" "$BASE/out/fdprobe/mobilegl.client.log" 2>/dev/null && break
  sleep 1
done
sleep 8
echo "=== hang confirmed; dumping ==="
echo "=== CLIENT ss ==="
ss -tn dst 192.168.21.181:40613
echo "=== CLIENT bytes ==="
ss -ti dst 192.168.21.181:40613 | grep -E 'bytes_sent|ESTAB'
sleep 1
kill $CPID 2>/dev/null
echo DONE_LOCAL
