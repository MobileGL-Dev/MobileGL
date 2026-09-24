#!/usr/bin/env bash
# Device check 7: only one server active.
#   7a  the offscreen service runs with a LIVE session child; starting the display Activity stops
#       the service - the supervisor goes, and its session child with it (PR_SET_PDEATHSIG) - and the
#       Activity's server takes the port (listen retry).
#   7b  starting the service kills :mglwin (also recorded by check 5).
#   STAMP=p12w1-<sha8> [LONG_CASE=...] bash check7.sh
. "$(dirname "$0")/dev.sh"
OUT=$DEV/7-one-server
LONG_CASE=${LONG_CASE:-minecraft-1.21.4-startup}
rm -rf "$OUT"; mkdir -p "$OUT"
{
wake
T0=$(devtime); echo "$T0" > "$OUT/devtime0"
if [ -z "$(Ash "ps -A -o ARGS" | grep "libMobileGLServer.so $LISTEN --serve")" ]; then
    log "no supervisor running - starting the service"
    tds start --forward > "$OUT/tds-service.txt" 2>&1
    wait_logcat "$T0" 'listening on tcp://127\.0\.0\.1:40613' 30 || log "supervisor NOT READY"
fi
log "=== 7a: a live offscreen session, then the display Activity"
T1=$(devtime)
replay "$OUT/victim" "$LONG_CASE" DirectGLES pbuffer &
VICTIM=$!
if wait_logcat "$T1" 'surface=pbuffer' 60; then log "offscreen session live"; else log "offscreen session never reached surface=pbuffer"; fi
sleep 1
ps_mobilegl | tee "$OUT/ps-before.txt"
SUP=$(Ash "ps -A -o PID,ARGS" | grep "libMobileGLServer.so $LISTEN --serve" | grep -v grep | awk '{print $1}' | head -1)
CHILD=$(Ash "ps -A -o PID,PPID,NAME" | awk -v p="$SUP" '$2==p && $3 ~ /libMobileGLServer/ {print $1}' | head -1)
log "supervisor pid=$SUP session child pid=${CHILD:-none}"
T2=$(devtime)
tds start --surface window > "$OUT/tds-activity.txt" 2>&1; cat "$OUT/tds-activity.txt"
if wait_logcat "$T2" 'listening on tcp://127\.0\.0\.1:40613 \(in-process display server, display installed\)' 30; then
    log "display server READY"
else
    log "display server NOT READY"
fi
sleep 1
log "supervisor $SUP alive? $(Ash "[ -d /proc/$SUP ] && echo yes || echo no")  child ${CHILD:-none} alive? $([ -n "$CHILD" ] && Ash "[ -d /proc/$CHILD ] && echo yes || echo no")"
ps_mobilegl | tee "$OUT/ps-after-activity.txt"
wait $VICTIM; log "the offscreen session's client rc=$? (its server went away)"
grep -hiE "device.lost|ServerCrashed|hung up|PeerHungUp|EOF|lost" "$OUT/victim/replay.log" "$OUT/victim/output/"mobilegl.client.log 2>/dev/null | cut -c1-240 | sort | uniq -c | head -8
logcat_since "$T2" > "$OUT/logcat-7a.txt"; mglines "$OUT/logcat-7a.txt" | grep -vE "pre-auth deadline" | head -24

log "=== 7b: the service again - :mglwin must go"
WIN=$(pid_of "$PKG:mglwin"); log ":mglwin pid=$WIN"
T3=$(devtime)
tds start > "$OUT/tds-service2.txt" 2>&1
wait_logcat "$T3" 'listening on tcp://127\.0\.0\.1:40613' 30 && log "supervisor READY" || log "supervisor NOT READY"
sleep 1
log ":mglwin pid now=$(pid_of "$PKG:mglwin") (was $WIN); /proc/$WIN exists? $(Ash "[ -d /proc/$WIN ] && echo yes || echo no")"
ps_mobilegl | tee "$OUT/ps-after-service.txt"
logcat_since "$T3" > "$OUT/logcat-7b.txt"; mglines "$OUT/logcat-7b.txt" | head -16
logcat_since "$T0" > "$OUT/logcat.txt"
log "done 7"
} 2>&1 | tee "$OUT/check.log"
