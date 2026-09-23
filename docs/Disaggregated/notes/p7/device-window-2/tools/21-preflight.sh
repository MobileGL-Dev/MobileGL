#!/usr/bin/env bash
# 21-preflight.sh <stamp> [--reboot] [--no-pin]
#
# Session start (runbook 1 section 2, CONTRACT-P7 7.2 "same reboot-clean session"):
#   --reboot   records boot_id, reboots, waits for boot_completed, records boot_id again; the two
#              must differ or the window is NOT reboot-clean. Gate 3 requires it; the dry run
#              does not reboot. Refused while gate3/state holds finished pairs (they belong to
#              the old session). session/boot-id.txt is THE session every gate-3 record must match;
#   without --reboot on a stamp that already has session/boot-id.txt: a resume - the phone must
#              still be in that boot session (else stop), and boot-id.txt / reboot-clean.txt are
#              kept, not rewritten (session/resumed.txt logs the re-entry);
#   records the state 90-restore.sh puts back: TCP supervisor up or not, stay-on setting, pin
#   state, Doze whitelist line, focused activity;
#   wakes the phone and sets `svc power stayon true` (息屏 cut child TCP in window 1b, ID-P7-48);
#   pins (pin_device.sh pin) unless --no-pin, then classifies `check` (PINNED-1100 = the board's
#   documented 1100 MHz pwrlevel-0 reading, accepted);
#   checks the CTS bundle is on the device and there is room to work.
set -uo pipefail
. "$(dirname "$0")/lib.sh"

[ $# -ge 1 ] || die "usage: $0 <stamp> [--reboot] [--no-pin]"
STAMP=$1; shift
REBOOT=0; PIN=1
for a in "$@"; do case "$a" in --reboot) REBOOT=1;; --no-pin) PIN=0;; *) die "unknown option $a";; esac; done
w2_load_build "$STAMP"
OUT=$(w2_out "$STAMP")/session
mkdir -p "$OUT"
w2_require_device
w2_lock
t0=$(date +%s)

# ---- what we found (only the FIRST preflight of a stamp writes these: restore reads them) ---
if [ ! -f "$OUT/found.env" ]; then
    sup=0; w2_supervisor_running && sup=1
    stayon=$(Ash "settings get global stay_on_while_plugged_in")
    pinstate=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-found.txt")
    cat > "$OUT/found.env" <<EOF
FOUND_SUPERVISOR=$sup
FOUND_STAYON=$stayon
FOUND_PIN=$pinstate
FOUND_AT=$(date -Is)
EOF
    Ash "dumpsys deviceidle whitelist" | grep -i mobilegl > "$OUT/deviceidle-found.txt" || true
    Ash "dumpsys window" | grep -E 'mCurrentFocus' > "$OUT/focus-found.txt" || true
    Ash "ps -A -o PID,PPID,USER,NAME,ARGS" | grep -iE 'mobilegl|mglsrv' > "$OUT/processes-found.txt" || true
    log "found: supervisor=$sup stayon=$stayon pin=$pinstate"
fi

SESSION_BOOT=$(w2_session_boot_id "$STAMP")
if [ $REBOOT -eq 1 ] && compgen -G "$(w2_out "$STAMP")/gate3/state/*/*.done" > /dev/null; then
    die "gate 3 has finished pairs from boot session ${SESSION_BOOT:-?}; --reboot starts a NEW session and would void them (60-reduce.py: INVALID-SESSION). Delete them first (RUNBOOK section 1 'phone rebooted') or use a new stamp"
fi
if [ $REBOOT -eq 1 ]; then
    Ash "cat /proc/sys/kernel/random/boot_id" > "$OUT/boot-id-before.txt"
    log "rebooting for a reboot-clean session"
    A reboot
    sleep 20
    A wait-for-device
    for _ in $(seq 1 120); do [ "$(Ash 'getprop sys.boot_completed')" = 1 ] && break; sleep 2; done
    sleep 20
    # File-based encryption: the app's data dir (where trace-replay-ci.sh copies each fixture with
    # run-as) is readable only after the first unlock. A phone with a lock-screen credential needs
    # one manual unlock here; wait up to 10 minutes for it rather than failing every case.
    w2_wake
    for i in $(seq 1 120); do
        [ "$(Ash 'getprop sys.user.0.ce_available')" = true ] && break
        [ $i = 1 ] && log "waiting for credential-encrypted storage: UNLOCK THE PHONE BY HAND if it shows a lock screen"
        sleep 5
    done
    [ "$(Ash 'getprop sys.user.0.ce_available')" = true ] || die "user 0 storage still locked after 10 min"
fi
if [ $REBOOT -eq 0 ] && [ -n "$SESSION_BOOT" ]; then
    # Re-entering this stamp's session (window.sh after 90-restore.sh ran with steps still open, or
    # by hand): the phone must still be in it, and boot-id.txt / reboot-clean.txt stay as the
    # session's first preflight wrote them. Stay-on and the pin are set again below.
    w2_same_session "$SESSION_BOOT" "preflight (resume of $STAMP)"
    echo "resumed $(date -Is): boot_id $SESSION_BOOT unchanged ($(cat "$OUT/reboot-clean.txt" 2>/dev/null))" | tee -a "$OUT/resumed.txt"
else
    Ash "cat /proc/sys/kernel/random/boot_id" > "$OUT/boot-id.txt"
    if [ $REBOOT -eq 1 ]; then
        if cmp -s "$OUT/boot-id-before.txt" "$OUT/boot-id.txt"; then die "boot_id unchanged: the reboot did not happen"; fi
        echo "reboot-clean: $(cat "$OUT/boot-id-before.txt") -> $(cat "$OUT/boot-id.txt")" | tee "$OUT/reboot-clean.txt"
    else
        echo "NOT reboot-clean (no --reboot): boot_id $(cat "$OUT/boot-id.txt"), up $(Ash 'cat /proc/uptime' | cut -d' ' -f1)s" | tee "$OUT/reboot-clean.txt"
    fi
fi

w2_wake
Ash "svc power stayon true"
Ash "dumpsys power" | grep -E 'mWakefulness=|mStayOn=' > "$OUT/power.txt"
Ash "dumpsys deviceidle get deep" > "$OUT/deviceidle-deep.txt" 2>&1 || true
{
    echo "model    : $(Ash 'getprop ro.product.model') / $(Ash 'getprop ro.board.platform')"
    echo "build    : $(Ash 'getprop ro.build.fingerprint')"
    echo "battery  : $(Ash 'dumpsys battery' | grep -E 'level|temperature|AC powered|USB powered' | tr -s ' ' | tr '\n' ' ')"
    echo "memory   : $(Ash 'grep -E "MemTotal|MemAvailable" /proc/meminfo' | tr -s ' ' | tr '\n' ' ')"
    echo "tmp free : $(Ash 'df -h /data/local/tmp' | tail -1)"
    echo "mgcts    : $(Ash "ls $W2_CTS_DEV 2>&1" | tr '\n' ' ')"
    echo "apk      : $(grep versionName "$(w2_out "$STAMP")/session/apk-install.txt" 2>/dev/null | tr -d ' ')"
} | tee "$OUT/device.txt"

if [ $PIN -eq 1 ]; then
    bash "$W2_TOOLS/tools/device_bench/pin_device.sh" "$W2_SERIAL" pin > "$OUT/pin-session-start.txt" 2>&1
    echo 1 > "$OUT/pinned-by-window"
fi
pin=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-check-00.txt")
log "pin: $pin"
if [ $PIN -eq 1 ] && [ "$pin" != PINNED ] && [ "$pin" != PINNED-1100 ]; then
    die "pin did not take ($pin, see $OUT/pin-check-00.txt)"
fi
w2_timing "$(w2_out "$STAMP")" preflight "$t0" "$(date +%s)" "reboot=$REBOOT pin=$PIN"
log "=== PREFLIGHT OK $STAMP ==="
