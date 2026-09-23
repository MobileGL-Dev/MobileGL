#!/usr/bin/env bash
# window.sh <pipe-sha> [<stamp>] [--no-reboot] [--skip build,preflight,install,gate3,bsl,cts,reduce,restore]
#
# The whole device window 2 as one resumable sequence (run it detached: see RUNBOOK.md):
#   10-build.sh      APK + host artefacts, stamp <stamp> (default p7w7-<sha8>)        host only
#   21-preflight.sh  record found state, reboot-clean (unless --no-reboot), stay-on, pin
#   20-install.sh    adb install -r, on-device base.apk sha256 == built APK, dex2oat idle
#   30-gate3.sh      36 cases x {monolith x1, inproc x3, spawn x3, inproc RUN_AHEAD=0 x1}
#   40-bsl-stats.sh  bsl-esc-menu x {inproc, spawn} with MOBILEGL_PIPE_STATS + maps sampler
#   50-cts-after.sh  five KHR-GL46 blocks, inproc x DirectVulkan, AFTER lib deployed to mgcts
#   60-reduce.py     gate-3 verdict per case, bsl peaks, CTS delta table + new-crash list
#   90-restore.sh    Home, supervisor as found, unpin, stay-on as found
# Each finished step leaves <out>/steps/<step>.done; a re-run skips it (gate3 / cts also resume
# inside a step). A step that fails stops the sequence; fix the cause and run the same command.
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

step() {  # <name> <command...>
    local name=$1; shift
    if [ -f "$OUT/steps/$name.done" ]; then log "--- $name: done earlier, skipping"; return 0; fi
    case "$SKIP" in *",$name,"*) log "--- $name: skipped by --skip"; return 0;; esac
    log "--- $name: $*"
    local t0; t0=$(date +%s)
    "$@"
    date -Is > "$OUT/steps/$name.done"
    log "--- $name: OK in $(( $(date +%s) - t0 ))s"
}

step build     bash "$HERE/10-build.sh" "$SHA" "$STAMP"
step preflight bash "$HERE/21-preflight.sh" "$STAMP" $REBOOT
step install   bash "$HERE/20-install.sh" "$STAMP"
step gate3     bash "$HERE/30-gate3.sh" "$STAMP"
step bsl       bash "$HERE/40-bsl-stats.sh" "$STAMP"
step cts       bash "$HERE/50-cts-after.sh" "$STAMP"
# The verdict is data, not a stop condition: a red gate still has to restore the phone.
step reduce    bash -c "python3 '$HERE/60-reduce.py' '$STAMP' > '$OUT/verdict.txt' 2>&1; echo reduce-rc=\$? >> '$OUT/verdict.txt'"
step restore   bash "$HERE/90-restore.sh" "$STAMP"
log "=== WINDOW 2 DONE $STAMP: verdict in $OUT/verdict.txt ==="
cat "$OUT/verdict.txt"
