#!/usr/bin/env bash
# 90-restore.sh <stamp>
#
# Puts the phone back the way 21-preflight.sh found it (<out>/session/found.env): Home screen,
# the TCP supervisor restarted on the INSTALLED package if it was running (every replay's
# `am force-stop` kills it; tcp_device_server.py start with the same listen/token and the saved
# Doze state file, which it never overwrites), the pin released if this window set it, and the
# stay-on setting as found. Prints the final state; never uninstalls anything.
set -uo pipefail
. "$(dirname "$0")/lib.sh"

[ $# -ge 1 ] || die "usage: $0 <stamp>"
STAMP=$1
w2_load_build "$STAMP"
OUT=$(w2_out "$STAMP")/session
[ -f "$OUT/found.env" ] || die "no $OUT/found.env: nothing recorded to restore to"
. "$OUT/found.env"
w2_require_device
w2_lock
t0=$(date +%s)

Ash "input keyevent KEYCODE_HOME"
if [ "${FOUND_SUPERVISOR:-0}" = 1 ]; then
    if w2_supervisor_running; then
        log "supervisor already listening on $W2_LISTEN"
    else
        python3 "$W2_TOOLS/tools/trace_replay/tcp_device_server.py" start --serial "$W2_SERIAL" \
            --package "$W2_PKG" --listen "$W2_LISTEN" --token "$W2_TOKEN" --allow-idle \
            --state-file "$W2_IDLE_STATE" > "$OUT/supervisor-restart.txt" 2>&1
        for _ in $(seq 1 20); do w2_supervisor_running && break; sleep 1; done
        w2_supervisor_running && log "supervisor restarted on $W2_LISTEN" || log "WARN: supervisor NOT listening (see $OUT/supervisor-restart.txt)"
    fi
    Ash "input keyevent KEYCODE_HOME"
fi
if [ -f "$OUT/pinned-by-window" ]; then
    bash "$W2_TOOLS/tools/device_bench/pin_device.sh" "$W2_SERIAL" unpin > "$OUT/pin-session-end.txt" 2>&1
    rm -f "$OUT/pinned-by-window"
fi
pin=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-check-final.txt")
if [ -n "${FOUND_STAYON:-}" ] && [ "$(Ash 'settings get global stay_on_while_plugged_in')" != "$FOUND_STAYON" ]; then
    Ash "settings put global stay_on_while_plugged_in $FOUND_STAYON"
fi
sleep 2
{
    echo "focus      : $(Ash 'dumpsys window' | grep -m1 mCurrentFocus | tr -s ' ')"
    echo "supervisor : $(w2_supervisor_running && echo listening || echo not-running) (found: ${FOUND_SUPERVISOR:-?})"
    echo "pin        : $pin (found: ${FOUND_PIN:-?})"
    echo "stayon     : $(Ash 'settings get global stay_on_while_plugged_in') (found: ${FOUND_STAYON:-?})"
    echo "doze wl    : $(Ash 'dumpsys deviceidle whitelist' | grep -i mobilegl | tr '\n' ' ')"
} | tee "$OUT/restored.txt"
w2_timing "$(w2_out "$STAMP")" restore "$t0" "$(date +%s)"
log "=== RESTORE DONE $STAMP ==="
