#!/usr/bin/env bash
# 40-bsl-stats.sh <stamp> [--arms inproc,spawn] [--interval 0.25]
#
# The M2 device reading the W6 window did not take (W6-verify/README.md: "峰值与 wbuf[] 仍待开
# stats 的专门一遍采集"): minecraft-1.21.4-fabric-iris-bsl-esc-menu-854, DirectVulkan x pbuffer,
# one replay per arm with MOBILEGL_PIPE_STATS=1 (PERIOD=1), while an on-device root sampler
# records every 0.25 s, for the replay's process(es):
#   /proc/<pid>/maps line count  (the vm.max_map_count question CONTRACT-P7 12 left open)
#   VmRSS / VmHWM
# inproc samples the app process (both roles live there); spawn samples the app (client) and the
# libMobileGLServer.so child it launched. The TCP supervisor's own libMobileGLServer.so (argv has
# tcp://...) is excluded by name. The wbuf[] gauges are read from the archived role logs.
# Output: <out>/bsl/<arm>/{samples.txt,peaks.txt,wbuf.txt} + the archived repeat.
set -uo pipefail
. "$(dirname "$0")/lib.sh"

[ $# -ge 1 ] || die "usage: $0 <stamp> [--arms inproc,spawn] [--interval S]"
STAMP=$1; shift
ARMS="inproc spawn"; INTERVAL=0.25
while [ $# -gt 0 ]; do
    case "$1" in
        --arms) ARMS=${2//,/ }; shift 2;;
        --interval) INTERVAL=$2; shift 2;;
        *) die "unknown option $1";;
    esac
done
w2_load_build "$STAMP"
OUT=$(w2_out "$STAMP")/bsl
mkdir -p "$OUT"
w2_require_device
w2_lock
[ "$(Ash "su -c 'id -u'")" = 0 ] || die "no root on $W2_SERIAL: /proc/<pid>/maps of an app process needs it"
export MOBILEGL_TRACE_PACKAGE=$W2_PKG MOBILEGL_TRACE_SKIP_INSTALL=1 MOBILEGL_TRACE_APK=$W2_APK
unset MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER
RUNNER="$W2_TOOLS/tools/trace_replay/run_android_retrace_local.py"
DEV_SAMPLER=/data/local/tmp/p7w7-sampler.sh

cat > "$OUT/sampler.sh" <<'SH'
#!/system/bin/sh
# p7w7 window-2 sampler: <out> <package> <stopfile> <interval>
out=$1; pkg=$2; stop=$3; interval=$4
: > "$out"
while [ ! -f "$stop" ]; do
  ts=$(date +%s.%N)
  for p in $(pidof "$pkg") $(pgrep -f libMobileGLServer); do
    [ -r /proc/$p/maps ] || continue
    args=$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)
    case "$args" in *tcp://*|*p7w7-sampler*) continue;; esac
    n=$(wc -l < /proc/$p/maps)
    st=$(grep -E '^(VmRSS|VmHWM):' /proc/$p/status | tr -s ' \t' ' ' | tr '\n' ' ')
    echo "$ts pid=$p comm=$(cat /proc/$p/comm) maps=$n $st"
  done >> "$out"
  sleep "$interval"
done
SH
A push "$OUT/sampler.sh" "$DEV_SAMPLER" >/dev/null || die "push sampler failed"
Ash "chmod 755 $DEV_SAMPLER"

t_all=$(date +%s)
for arm in $ARMS; do
    [ -f "$OUT/$arm/.done" ] && { log "bsl $arm already done"; continue; }
    rm -rf "$OUT/$arm"; mkdir -p "$OUT/$arm"
    dev_out=/data/local/tmp/p7w7-samples-$arm.txt; dev_stop=/data/local/tmp/p7w7-sampler.stop
    Ash "su -c 'rm -f $dev_stop $dev_out'"
    # The previous replay's app process outlives its Activity; stop it BEFORE the sampler starts,
    # or its 800 MB and its maps count land in this arm's peaks (seen in the dry run: a 2 s pid
    # that was gate 3's last case, not this replay).
    Ash "am force-stop $W2_PKG"
    sleep 1
    w2_wake
    # The sampler runs as root on the device, in the background of one adb shell.
    adb -s "$W2_SERIAL" shell "su -c 'sh $DEV_SAMPLER $dev_out $W2_PKG $dev_stop $INTERVAL'" > "$OUT/$arm/sampler-adb.log" 2>&1 &
    sampler=$!
    sleep 1
    t0=$(date +%s)
    python3 "$RUNNER" --case "$W2_BSL" --backend DirectVulkan --use-pbuffer --transport "$arm" --repeat 1 \
        --archive-dir "$OUT/$arm/archive" --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=1 \
        > "$OUT/$arm/replay.log" 2>&1
    rc=$?
    t1=$(date +%s)
    sleep 2
    Ash "su -c 'touch $dev_stop'"
    for _ in $(seq 1 20); do kill -0 $sampler 2>/dev/null || break; sleep 1; done
    kill $sampler 2>/dev/null || true
    A pull "$dev_out" "$OUT/$arm/samples.txt" >/dev/null 2>&1 || log "  WARN: no samples pulled for $arm"
    Ash "su -c 'rm -f $dev_stop $dev_out'"
    # peaks per (pid, comm)
    python3 - "$OUT/$arm/samples.txt" > "$OUT/$arm/peaks.txt" <<'PY'
import re, sys
peaks = {}
try:
    lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
except OSError:
    lines = []
for line in lines:
    m = re.match(r"(\S+) pid=(\d+) comm=(\S+) maps=(\d+)(.*)", line)
    if not m:
        continue
    key = (m.group(2), m.group(3))
    rss = re.search(r"VmRSS: (\d+) kB", m.group(5)); hwm = re.search(r"VmHWM: (\d+) kB", m.group(5))
    p = peaks.setdefault(key, {"samples": 0, "maps": 0, "rss": 0, "hwm": 0, "first": m.group(1), "last": m.group(1)})
    p["samples"] += 1; p["last"] = m.group(1)
    p["maps"] = max(p["maps"], int(m.group(4)))
    if rss: p["rss"] = max(p["rss"], int(rss.group(1)))
    if hwm: p["hwm"] = max(p["hwm"], int(hwm.group(1)))
print("%-8s %-18s %-6s %7s %10s %12s %12s %8s" % ("pid", "comm", "role", "samples", "maps_peak", "VmRSS_peak_kB", "VmHWM_kB", "span_s"))
for (pid, comm), p in sorted(peaks.items(), key=lambda kv: -kv[1]["maps"]):
    # comm is 15 chars: the app reads "ugin.p7w1.trace", the spawned server "libMobileGLServ".
    role = "server" if comm.startswith("libMobileGL") else "app"
    print("%-8s %-18s %-6s %7d %10d %12d %12d %8.1f" % (pid, comm, role, p["samples"], p["maps"], p["rss"], p["hwm"],
                                                        float(p["last"]) - float(p["first"])))
if not peaks:
    print("NO SAMPLES")
PY
    grep -rh 'wbuf\[' "$OUT/$arm/archive" 2>/dev/null | sed 's/^.*MGPipe stats: //' > "$OUT/$arm/wbuf.txt" || true
    grep -rhoE 'wbuf\[[^]]*\]' "$OUT/$arm/archive" 2>/dev/null | sort | uniq -c > "$OUT/$arm/wbuf-gauges.txt" || true
    printf 'rc=%s seconds=%s finished=%s\n' "$rc" "$((t1 - t0))" "$(date -Is)" > "$OUT/$arm/.done"
    log "bsl $arm rc=$rc $((t1 - t0))s"
    sed 's/^/    /' "$OUT/$arm/peaks.txt"
    sed 's/^/    /' "$OUT/$arm/wbuf-gauges.txt"
    w2_timing "$(w2_out "$STAMP")" "bsl-$arm" "$t0" "$t1"
done
Ash "rm -f $DEV_SAMPLER"
w2_timing "$(w2_out "$STAMP")" bsl "$t_all" "$(date +%s)"
log "=== BSL STATS DONE $STAMP ==="
