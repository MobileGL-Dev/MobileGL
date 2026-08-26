#!/usr/bin/env bash
# CPU-profile the running game with simpleperf (DWARF call graphs) and produce
# a symbolized report on the host. Run while the game is in-world (e.g. during
# a bench.sh warmup, or standalone after launching the game manually).
#
# Usage:
#   profile.sh --device devices/odinlite.env [--duration 30] [--label hot1]
#              [--freq 800]
#
# Requires: debuggable app (fordebug flavor), host NDK simpleperf, and the
# unstripped libMobileGL.so from the same build as the installed APK
# (MobileGL/build/intermediates/merged_native_libs/fordebug/mergeFordebugNativeLibs/out/lib/arm64-v8a).
#
# Notes carried over from earlier campaigns:
#   - DWARF unwinding (-g): frame-pointer call graphs are broken on these builds.
#   - The MC render thread is a JVM thread with a generic name (Thread-NN, varies
#     per run) — find it in the report with --sort comm first.
#   - Use Git Bash, not PowerShell (binary pull corruption via > redirect).

set -u -o pipefail
cd "$(dirname "$0")"

PKG=com.tungsten.fcl.mgdebug.debug
DEVICE_ENV=""
DURATION=30
FREQ=800
LABEL=prof

while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE_ENV=$2; shift 2 ;;
    --duration) DURATION=$2; shift 2 ;;
    --freq) FREQ=$2; shift 2 ;;
    --label) LABEL=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[ -n "$DEVICE_ENV" ] || { echo "need --device" >&2; exit 2; }
# shellcheck disable=SC1090
. "$DEVICE_ENV"

ADB="adb -s $DEVICE_SERIAL"
STAMP=$(date +%Y%m%d-%H%M%S)
OUTDIR="results/${STAMP}-${LABEL}"
mkdir -p "$OUTDIR"

$ADB shell pidof $PKG >/dev/null || { echo "game not running" >&2; exit 1; }

echo "[profile] recording ${DURATION}s @ ${FREQ}Hz (DWARF)..." >&2
$ADB shell simpleperf record --app $PKG -e cpu-clock -f "$FREQ" -g \
  --duration "$DURATION" -o /data/local/tmp/mgprof.data || exit 1
(cd "$OUTDIR" && MSYS_NO_PATHCONV=1 adb -s "$DEVICE_SERIAL" pull /data/local/tmp/mgprof.data perf.data >/dev/null)
echo "[profile] pulled to $OUTDIR/perf.data" >&2

# Host-side symbolization if the NDK simpleperf scripts are available.
SIMPLEPERF_DIR="${SIMPLEPERF_DIR:-$LOCALAPPDATA/Android/Sdk/ndk/28.2.13676358/simpleperf}"
SYMDIR="../../build/intermediates/merged_native_libs/fordebug/mergeFordebugNativeLibs/out/lib/arm64-v8a"
if [ -d "$SIMPLEPERF_DIR" ] && [ -d "$SYMDIR" ]; then
  echo "[profile] building binary cache (symbolized)..." >&2
  (cd "$OUTDIR" && python "$SIMPLEPERF_DIR/binary_cache_builder.py" -i perf.data -lib "../../$SYMDIR" >/dev/null 2>&1)
  echo "[profile] per-thread summary:" >&2
  "$SIMPLEPERF_DIR/bin/windows/x86_64/simpleperf.exe" report -i "$OUTDIR/perf.data" \
    --symfs "$OUTDIR/binary_cache" --sort comm -n 2>/dev/null | head -25
  echo "[profile] done; drill down with:" >&2
  echo "  $SIMPLEPERF_DIR/bin/windows/x86_64/simpleperf.exe report -i $OUTDIR/perf.data --symfs $OUTDIR/binary_cache --comms <renderthread> --sort symbol -n | head -40" >&2
else
  echo "[profile] NDK simpleperf or symbol dir missing; raw perf.data kept at $OUTDIR" >&2
fi
