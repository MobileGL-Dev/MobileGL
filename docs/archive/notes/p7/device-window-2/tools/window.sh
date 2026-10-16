#!/usr/bin/env bash
# window.sh <pipe-sha> [<stamp>] [--no-reboot] [--skip build,preflight,install,gate3,bsl,cts,reduce,restore]
#
# The whole device window 2 as one resumable sequence (run it detached: see RUNBOOK.md):
#   10-build.sh      APK + host artefacts, stamp <stamp> (default p7w7-<sha8>)        host only
#   21-preflight.sh  record found state, reboot-clean (unless --no-reboot), stay-on, pin
#   20-install.sh    adb install -r, on-device base.apk sha256 == built APK, dex2oat idle
#   30-gate3.sh      36 cases x {monolith x5, inproc x3, spawn x3, inproc RUN_AHEAD=0 x1}
#   40-bsl-stats.sh  bsl-esc-menu x {inproc, spawn} with MOBILEGL_PIPE_STATS + maps sampler
#   50-cts-after.sh  five KHR-GL46 blocks, inproc x DirectVulkan, AFTER lib deployed to mgcts
#   60-reduce.py     gate-3 verdict per case, bsl peaks, CTS delta table + new-crash list
#   90-restore.sh    Home, supervisor as found, unpin, stay-on as found, $BASE lib back in mgcts
# From preflight to restore the window holds the wave's shared device lock ($W2_SHARED_LOCK, the
# one other agents take with 'flock -w 3600 ... <cmd>'; it waits up to W2_SHARED_LOCK_WAIT s for it).
# Each finished step leaves <out>/steps/<step>.done; a re-run skips it (gate3 / cts also resume
# inside a step). A step that exits 3 (gate3 / cts: loop finished, some pairs / blocks INCOMPLETE)
# is NOT marked done and the sequence carries on; a re-run of the same command retries only what
# is missing (after re-entering the same boot session with 21-preflight.sh, no reboot, if the
# phone was already restored). reduce and restore run on every pass. Any other failure stops the
# sequence; fix the cause and run the same command (RUNBOOK section 1).
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/lib.sh"

[ $# -ge 1 ] || die "usage: $0 <pipe-sha> [<stamp>] [--no-reboot] [--skip a,b]"
PIPE_SHA=$1; shift
STAMP=""; REBOOT=--reboot; SKIP=""
while [ $# -gt 0 ]; do
    case "$1" in
        --no-reboot) REBOOT=""; shift;;
        --skip) SKIP=",$2,"; shift 2;;
        --*) die "unknown option $1";;
        *) STAMP=$1; shift;;
    esac
done
SHA=$(git -C "$W2_PIPE" rev-parse --verify "$PIPE_SHA^{commit}")
STAMP=${STAMP:-p7w7-${SHA:0:8}}
OUT=$(w2_out "$STAMP")
mkdir -p "$OUT/steps"
log "=== WINDOW 2 START sha=$SHA stamp=$STAMP out=$OUT ==="

PARTIAL=""
skipped() { case "$SKIP" in *",$1,"*) return 0;; esac; return 1; }
step() {  # <name> <command...>
    local name=$1; shift
    if [ -f "$OUT/steps/$name.done" ]; then log "--- $name: done earlier, skipping"; return 0; fi
    if skipped "$name"; then log "--- $name: skipped by --skip"; return 0; fi
    log "--- $name: $*"
    local t0 rc=0; t0=$(date +%s)
    "$@" || rc=$?
    if [ $rc -eq 3 ]; then
        PARTIAL="$PARTIAL $name"
        log "--- $name: INCOMPLETE after $(( $(date +%s) - t0 ))s - not marked done; the next run of this command retries what is missing"
        return 0
    fi
    [ $rc -eq "$W2_SHARED_LOCK_RC" ] && die "--- $name: the shared device lock $W2_SHARED_LOCK was not free within ${W2_SHARED_LOCK_WAIT}s (another agent holds $W2_SERIAL); run the same command later"
    [ $rc -eq 0 ] || die "--- $name failed (rc=$rc); fix the cause and run the same command"
    date -Is > "$OUT/steps/$name.done"
    log "--- $name: OK in $(( $(date +%s) - t0 ))s"
}

step build     bash "$HERE/10-build.sh" "$SHA" "$STAMP"
# From here to the end the window holds the wave's shared device lock (lib.sh w2_shared_lock: this
# script is re-run under `flock -o`; the build above is done by then and skipped by the re-run), so no
# other agent gets the phone between two steps; the steps see W2_SHARED_LOCK_HELD and do not re-take it.
w2_shared_lock
step preflight bash "$HERE/21-preflight.sh" "$STAMP" $REBOOT
step install   bash "$HERE/20-install.sh" "$STAMP"
# A previous pass restored the phone (unpinned, stay-on as found) with device steps still open:
# re-enter the SAME boot session first (no reboot; 21-preflight.sh stops if the phone rebooted).
if [ -f "$OUT/steps/restore.done" ]; then
    for s in gate3 bsl cts; do
        if [ ! -f "$OUT/steps/$s.done" ] && ! skipped "$s"; then
            log "--- resume: $s still open after a restore; re-entering the session"
            bash "$HERE/21-preflight.sh" "$STAMP"
            rm -f "$OUT/steps/restore.done"
            break
        fi
    done
fi
step gate3     bash "$HERE/30-gate3.sh" "$STAMP"
step bsl       bash "$HERE/40-bsl-stats.sh" "$STAMP"
step cts       bash "$HERE/50-cts-after.sh" "$STAMP"
# The verdict is data, not a stop condition: a red gate still has to restore the phone. Both run on
# every pass (cheap, idempotent), so a resumed window re-reduces and re-restores.
rm -f "$OUT/steps/reduce.done" "$OUT/steps/restore.done"
step reduce    bash -c "python3 '$HERE/60-reduce.py' '$STAMP' > '$OUT/verdict.txt' 2>&1; echo reduce-rc=\$? >> '$OUT/verdict.txt'"
step restore   bash "$HERE/90-restore.sh" "$STAMP"
if [ -n "$PARTIAL" ]; then
    log "=== WINDOW 2 INCOMPLETE $STAMP: step(s)$PARTIAL left open; run the same command again (RUNBOOK section 1). verdict so far in $OUT/verdict.txt ==="
else
    log "=== WINDOW 2 DONE $STAMP: verdict in $OUT/verdict.txt ==="
fi
cat "$OUT/verdict.txt"
