#!/usr/bin/env bash
# Device check 4: HOME during an on-screen replay. The session must end with the named
# ServerWindowLost latch, :mglwin must survive, the client must read a clean device loss; bringing
# the Activity back must let the next session render.
#   STAMP=p12w1-<sha8> [LONG_CASE=...] bash check4.sh
. "$(dirname "$0")/dev.sh"
OUT=$DEV/4-window-lost
LONG_CASE=${LONG_CASE:-minecraft-1.21.4-startup}
rm -rf "$OUT"; mkdir -p "$OUT"
{
WIN=$(pid_of "$PKG:mglwin")
log ":mglwin pid before=$WIN"
[ -n "$WIN" ] || { log "no display Activity process - run check1.sh first"; exit 1; }
wake
T0=$(devtime); echo "$T0" > "$OUT/devtime0"
mkdir -p "$OUT/lost"; : > "$OUT/lost/shots.tsv"
replay "$OUT/lost" "$LONG_CASE" DirectGLES window MOBILEGL_IPC_SURFACE=server &
LONG=$!
shots "$OUT/lost" $LONG &
SHOOTER=$!
if wait_logcat "$T0" 'owner=server' 60; then log "session is on screen"; else log "session never reached surface=window"; fi
sleep 3
kill -0 $LONG 2>/dev/null && log "session live - pressing HOME" || log "WARNING: the session already ended before HOME"
date +%s.%N > "$OUT/lost/t-home"
Ash input keyevent KEYCODE_HOME
wait $LONG; RC=$?
wait $SHOOTER 2>/dev/null
log "client rc=$RC after HOME ($(python3 -c "print(round($(cat "$OUT/lost/t1")-$(cat "$OUT/lost/t-home"),2))") s after the key)"
result_summary "$OUT/lost"
grep -hiE "ServerWindowLost|device.lost|ServerCrashed|SessionFault|latched" "$OUT/lost/replay.log" "$OUT/lost/output/"mobilegl*.log 2>/dev/null | cut -c1-260 | sort | uniq -c | head -12
sleep 2
WIN2=$(pid_of "$PKG:mglwin")
log ":mglwin pid after HOME=$WIN2 (before=$WIN): $([ "$WIN" = "$WIN2" ] && echo SURVIVED || echo CHANGED)"
logcat_since "$T0" > "$OUT/logcat-lost.txt"
mglines "$OUT/logcat-lost.txt" | grep -E "surfaceDestroyed|ServerWindowLost|in-process session|detached|reaped|latch" | head -20

log "=== bring the Activity back, then a new on-screen session"
T1=$(devtime)
wake
tds start --surface window > "$OUT/tds-restart.txt" 2>&1
if wait_logcat "$T1" 'surfaceCreated: server window' 20; then log "window re-attached"; else log "no surfaceCreated within 20 s"; fi
Ash dumpsys window | grep -E "mCurrentFocus|mFocusedApp" | head -2
replay_with_shots "$OUT/after" OpenRA DirectGLES window MOBILEGL_IPC_SURFACE=server
log "after-return session rc=$?"
result_summary "$OUT/after"
shot_report "$OUT/after" | tail -2
log ":mglwin pid at the end=$(pid_of "$PKG:mglwin")"
logcat_since "$T0" > "$OUT/logcat.txt"
mglines "$OUT/logcat.txt" | grep -E "MobileGLDisplay|in-process session|ServerWindowLost|owner=server|window .* (attached|detached)" | head -40
A shell run-as "$PKG" cat files/mglwin.server.log > "$OUT/mglwin.server.log" 2>&1
log "done 4"
} 2>&1 | tee "$OUT/check.log"
