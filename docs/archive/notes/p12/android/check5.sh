#!/usr/bin/env bash
# Device checks 5 and 6 (and 7b): the offscreen service.
#   5  start MobileGLServerService while the display Activity runs -> :mglwin is gone; the same
#      fixture WITHOUT the knob replays as before (ssim), server log surface=pbuffer.
#   6  MOBILEGL_IPC_SURFACE=server against it -> refused by name on both sides; the service still
#      serves the next (offscreen) session.
#   STAMP=p12w1-<sha8> bash check5.sh
. "$(dirname "$0")/dev.sh"
OUT=$DEV/5-offscreen-service
OUT6=$DEV/6-negative-serverowned-vs-offscreen
rm -rf "$OUT" "$OUT6"; mkdir -p "$OUT" "$OUT6"
{
WIN=$(pid_of "$PKG:mglwin")
log "before: :mglwin pid=${WIN:-none}"
ps_mobilegl
wake
T0=$(devtime); echo "$T0" > "$OUT/devtime0"
tds start --forward > "$OUT/tds-start.txt" 2>&1; cat "$OUT/tds-start.txt"
if wait_logcat "$T0" 'listening on tcp://127\.0\.0\.1:40613' 30; then log "supervisor READY"; else log "supervisor NOT READY"; fi
sleep 1
log "after: :mglwin pid=$(pid_of "$PKG:mglwin" || true) (was ${WIN:-none})"
ps_mobilegl | tee "$OUT/ps-after-start.txt"
logcat_since "$T0" > "$OUT/logcat-start.txt"; mglines "$OUT/logcat-start.txt" | head -20

log "=== 5: offscreen replay, no knob"
replay "$OUT/offscreen" OpenRA DirectGLES pbuffer
log "offscreen rc=$?"
result_summary "$OUT/offscreen"
logcat_since "$T0" > "$OUT/logcat.txt"
log "done 5"
} 2>&1 | tee "$OUT/check.log"
{
T0=$(devtime); echo "$T0" > "$OUT6/devtime0"
log "=== 6: MOBILEGL_IPC_SURFACE=server --window-surface against the offscreen service"
replay "$OUT6/refused" OpenRA DirectGLES window MOBILEGL_IPC_SURFACE=server
log "refused client rc=$?"
result_summary "$OUT6/refused"
grep -hE "NoServerDisplay|owns no display|EGL window surface creation failed|SurfaceRefusal|Refuse ServerOwned" \
    "$OUT6/refused/replay.log" "$OUT6/refused/output/"mobilegl*.log 2>/dev/null | cut -c1-300 | sort | uniq -c | head
log "=== 6: the next offscreen session on the same service"
replay "$OUT6/next" OpenRA DirectGLES pbuffer
log "next offscreen rc=$?"
result_summary "$OUT6/next"
ps_mobilegl | tee "$OUT6/ps-end.txt"
logcat_since "$T0" > "$OUT6/logcat.txt"
mglines "$OUT6/logcat.txt" | grep -E "listening|reaped|Refuse|no display|surface=" | head -20
log "done 6"
} 2>&1 | tee "$OUT6/check.log"
