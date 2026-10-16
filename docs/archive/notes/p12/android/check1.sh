#!/usr/bin/env bash
# Device check 1 (and 2 with BACKEND=DirectVulkan): on-screen replay through the display Activity.
#   STAMP=p12w1-<sha8> [BACKEND=DirectGLES|DirectVulkan] [CASE=OpenRA] bash check1.sh
. "$(dirname "$0")/dev.sh"
BACKEND=${BACKEND:-DirectGLES}
CASE=${CASE:-OpenRA}
CHECK_NAME=${CHECK_NAME:-$([ "$BACKEND" = DirectVulkan ] && echo 2-onscreen-magma || echo 1-onscreen-espryt)}
OUT=$DEV/$CHECK_NAME
rm -rf "$OUT"; mkdir -p "$OUT"
{
log "check $CHECK_NAME: stamp $STAMP backend $BACKEND case $CASE"
log "found: $(ps_mobilegl | tr '\n' ';')"
force_stop; sleep 1
wake
T0=$(devtime); echo "$T0" > "$OUT/devtime0"
PIN=(); [ "$BACKEND" = DirectVulkan ] && PIN=(--backend DirectVulkan)
tds start --surface window --forward "${PIN[@]}" > "$OUT/tds-start.txt" 2>&1; cat "$OUT/tds-start.txt"
if wait_logcat "$T0" 'listening on tcp://127\.0\.0\.1:40613 \(in-process display server, display installed\)' 30; then
    log "READY"
else
    log "NOT READY within 30 s"
fi
WIN=$(pid_of "$PKG:mglwin"); log ":mglwin pid=$WIN"; echo "$WIN" > "$OUT/mglwin.pid"
Ash dumpsys window | grep -E "mCurrentFocus|mFocusedApp" | head -2
log "replay $CASE $BACKEND --window-surface MOBILEGL_IPC_SURFACE=server"
replay_with_shots "$OUT/replay" "$CASE" "$BACKEND" window MOBILEGL_IPC_SURFACE=server
log "replay rc=$?"
result_summary "$OUT/replay"
shot_report "$OUT/replay"
log ":mglwin pid after: $(pid_of "$PKG:mglwin")"
if [ -n "${SECOND_CASE:-}" ]; then
    log "a second $BACKEND session in the same process: $SECOND_CASE on-screen"
    replay_with_shots "$OUT/second" "$SECOND_CASE" "$BACKEND" window MOBILEGL_IPC_SURFACE=server
    log "second session rc=$?"
    result_summary "$OUT/second"
    shot_report "$OUT/second" | tail -2
    log ":mglwin pid after the second session: $(pid_of "$PKG:mglwin")"
fi
logcat_since "$T0" > "$OUT/logcat.txt"
mglines "$OUT/logcat.txt" | head -60
A shell run-as "$PKG" cat files/mglwin.server.log > "$OUT/mglwin.server.log" 2>&1
log "done $CHECK_NAME"
} 2>&1 | tee "$OUT/check.log"
