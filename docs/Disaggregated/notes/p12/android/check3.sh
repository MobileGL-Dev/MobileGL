#!/usr/bin/env bash
# Device check 3: two sequential on-screen sessions in ONE display-Activity process (the second
# works), and a second concurrent authenticated client is refused Busy. Runs on the Activity check 1
# left serving (it re-checks the pid).
#   STAMP=p12w1-<sha8> [LONG_CASE=...] bash check3.sh
. "$(dirname "$0")/dev.sh"
OUT=$DEV/3-sequential-and-busy
LONG_CASE=${LONG_CASE:-minecraft-1.21.4-startup}
rm -rf "$OUT"; mkdir -p "$OUT"
{
PREV=$(cat "$DEV/1-onscreen-espryt/mglwin.pid" 2>/dev/null)
WIN=$(pid_of "$PKG:mglwin")
log ":mglwin pid now=$WIN (check 1's session ran in pid=$PREV)"
[ -n "$WIN" ] || { log "no display Activity process - run check1.sh first"; exit 1; }
wake
T0=$(devtime); echo "$T0" > "$OUT/devtime0"

log "=== session 2 in the same process: OpenRA on-screen"
replay_with_shots "$OUT/second" OpenRA DirectGLES window MOBILEGL_IPC_SURFACE=server
log "second session rc=$?"
result_summary "$OUT/second"
shot_report "$OUT/second" | tail -2
log "Welcome server pids seen by the two clients (check 1, this one):"
grep -ho "control=tcp data=stream server=[^ ]* pid=[0-9]*" "$DEV/1-onscreen-espryt/replay/output/mobilegl.client.log" "$OUT/second/output/mobilegl.client.log" | sort | uniq -c

log "=== Busy: a long on-screen session ($LONG_CASE), then a second authenticated client"
T1=$(devtime)
mkdir -p "$OUT/long"; : > "$OUT/long/shots.tsv"
replay "$OUT/long" "$LONG_CASE" DirectGLES window MOBILEGL_IPC_SURFACE=server &
LONG=$!
shots "$OUT/long" $LONG &
SHOOTER=$!
if wait_logcat "$T1" 'owner=server' 60; then log "long session is on screen"; else log "long session never reached surface=window"; fi
sleep 2
kill -0 $LONG 2>/dev/null && log "long session still live - starting the second client" || log "WARNING: the long session already ended"
replay "$OUT/concurrent" OpenRA DirectGLES window MOBILEGL_IPC_SURFACE=server
log "concurrent client rc=$?"
grep -hiE "busy|refuse" "$OUT/concurrent/replay.log" "$OUT/concurrent/output/"mobilegl*.log 2>/dev/null | cut -c1-260 | sort | uniq -c | head
wait $LONG; log "long session rc=$?"
wait $SHOOTER 2>/dev/null
result_summary "$OUT/long"
shot_report "$OUT/long" | tail -3
log ":mglwin pid after: $(pid_of "$PKG:mglwin")"
logcat_since "$T0" > "$OUT/logcat.txt"
mglines "$OUT/logcat.txt" | grep -E "MobileGLDisplay|in-process session|Busy|owner=server|surface=window|listening" | head -60
log "done 3"
} 2>&1 | tee "$OUT/check.log"
