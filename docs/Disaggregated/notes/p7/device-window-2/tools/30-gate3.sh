#!/usr/bin/env bash
# 30-gate3.sh <stamp> [--cases a,b,...] [--repeat N] [--arms monolith,inproc,spawn,inproc-ra0]
#
# CONTRACT-P7 7.1/7.2 exit gate 3, one reboot-clean session, DirectVulkan x --use-pbuffer:
#   monolith   x1   the same-session control every split reading is scored against
#   inproc     xN   (N = --repeat, default 3) must be bit-identical across passes
#   spawn      xN   same, server role in a second process
#   inproc-ra0 x1   MOBILEGL_IPC_RUN_AHEAD=0 lockstep control, recorded only
# over the 36-case denominator = --matrix's DirectVulkan cases minus W2_EXCLUDED (the three that
# are red on monolith: create-indirect, 1.21.11 main menu, photon v1.3b). iterationrp carries the
# apk.yml:460-462 Magma knobs on every arm. Every repeat's result.json, actual PNG, both role logs,
# transport proof and logcat are archived under <out>/gate3/archive/<arm>/<case>-DirectVulkan/.
#
# ONE SESSION (CONTRACT-P7 7.2 "same reboot-clean session"): the phone's boot_id must equal
# <out>/session/boot-id.txt (21-preflight.sh) before and after every pair; the driver stops the
# moment it does not. Every pair's .done and every archived repeat (repeat-NN/boot_id.txt) records
# boot_id / boot_id_before, and 60-reduce.py calls the gate INVALID-SESSION unless all of them,
# over all arms, are that one boot_id.
#
# RESUMABLE: one runner invocation per (arm, case); a pair writes
# <out>/gate3/state/<arm>/<case>.done ONLY when every repeat it asked for left result.json AND an
# actual PNG in the archive - otherwise it is logged INCOMPLETE, left without a .done, and the
# next start re-runs it from scratch (its partial archive is removed first). Exit 0 = every pair
# done; 3 = the loop finished but some pairs are INCOMPLETE (window.sh carries on and leaves the
# step for the next run); anything else = stopped. Run it detached:
#   bash detach.sh ~/w7/logs/p7w7/<stamp>/gate3.log 30-gate3.sh <stamp>
set -uo pipefail
. "$(dirname "$0")/lib.sh"

[ $# -ge 1 ] || die "usage: $0 <stamp> [--cases a,b] [--repeat N] [--arms list]"
STAMP=$1; shift
CASES_ARG=""; REPEAT=3; ARMS="monolith inproc spawn inproc-ra0"
while [ $# -gt 0 ]; do
    case "$1" in
        --cases) CASES_ARG=$2; shift 2;;
        --repeat) REPEAT=$2; shift 2;;
        --arms) ARMS=${2//,/ }; shift 2;;
        *) die "unknown option $1";;
    esac
done
w2_load_build "$STAMP"
OUT=$(w2_out "$STAMP")/gate3
mkdir -p "$OUT"
w2_require_device
w2_lock
SESSION_BOOT=$(w2_session_boot_id "$STAMP")
[ -n "$SESSION_BOOT" ] || die "no $(w2_out "$STAMP")/session/boot-id.txt: run 21-preflight.sh first"
w2_same_session "$SESSION_BOOT" "gate 3 start"
export MOBILEGL_TRACE_PACKAGE=$W2_PKG MOBILEGL_TRACE_SKIP_INSTALL=1 MOBILEGL_TRACE_APK=$W2_APK
RUNNER="$W2_TOOLS/tools/trace_replay/run_android_retrace_local.py"

# ---- the case list (and below, the repeat count) is frozen on first start: a resume cannot change
# the denominator ------------------------------------------------------------------------------------
if [ ! -s "$OUT/cases.txt" ]; then
    if [ -n "$CASES_ARG" ]; then
        tr ',' '\n' <<<"$CASES_ARG" | sed '/^$/d' > "$OUT/cases.txt"
    else
        W2_EXCLUDED="$W2_EXCLUDED" python3 - "$W2_TOOLS/tools/trace_replay" > "$OUT/cases.txt" <<'PY'
import os, sys
sys.path.insert(0, sys.argv[1])
from trace_cases import ci_backends, ci_trace_cases, load_trace_cases, split_trace_cases
excluded = set(os.environ["W2_EXCLUDED"].split())
for case in split_trace_cases(ci_trace_cases(load_trace_cases())):
    if "DirectVulkan" in ci_backends(case) and case["name"] not in excluded:
        print(case["name"])
PY
    fi
    printf '%s\n' $W2_EXCLUDED > "$OUT/excluded.txt"
fi
mapfile -t CASES < "$OUT/cases.txt"
[ ${#CASES[@]} -gt 0 ] || die "empty case list"
[ -f "$OUT/arms.txt" ] || printf 'arms=%s\nrepeat=%s\napk=%s\ntools=%s\n' "$ARMS" "$REPEAT" "$W2_APK" "$W2_TOOLS" > "$OUT/arms.txt"
# The repeat count is frozen with the case list: a resume started without the first run's --repeat
# (RUNBOOK: "30-gate3.sh <stamp>") must not re-run a pair with another count than its siblings'.
FROZEN_REPEAT=$(sed -n 's/^repeat=//p' "$OUT/arms.txt")
if [ -n "$FROZEN_REPEAT" ] && [ "$FROZEN_REPEAT" != "$REPEAT" ]; then
    log "repeat frozen at $FROZEN_REPEAT by $OUT/arms.txt (asked: $REPEAT)"
    REPEAT=$FROZEN_REPEAT
fi
log "gate3 stamp=$STAMP cases=${#CASES[@]} arms=[$ARMS] repeat=$REPEAT apk=$W2_APK tools=$W2_TOOLS"

arm_args() {  # <arm> -> runner arguments
    case "$1" in
        monolith)   echo "--transport monolith --repeat 1";;
        inproc)     echo "--transport inproc --repeat $REPEAT";;
        spawn)      echo "--transport spawn --repeat $REPEAT";;
        inproc-ra0) echo "--transport inproc --repeat 1 --env MOBILEGL_IPC_RUN_AHEAD=0";;
        *) die "unknown arm $1";;
    esac
}
arm_repeats() { case "$1" in inproc|spawn) echo "$REPEAT";; *) echo 1;; esac; }

# <case-archive> <n> -> the repeats (of 1..n) that lack result.json or an actual PNG; empty = complete
missing_repeats() {
    local i d miss=()
    for i in $(seq 1 "$2"); do
        d=$(printf '%s/repeat-%02d' "$1" "$i")
        if [ ! -s "$d/result.json" ] || ! compgen -G "$d/*-actual.png" > /dev/null; then miss+=("repeat-$(printf %02d "$i")"); fi
    done
    echo "${miss[*]}"
}

t_all=$(date +%s)
INCOMPLETE=0
for arm in $ARMS; do
    mkdir -p "$OUT/state/$arm" "$OUT/logs/$arm" "$OUT/archive/$arm"
    pending=0
    for c in "${CASES[@]}"; do [ -f "$OUT/state/$arm/$c.done" ] || pending=$((pending + 1)); done
    if [ $pending -eq 0 ]; then log "arm $arm: all ${#CASES[@]} done, skipping"; continue; fi
    log "=== arm $arm start ($pending pending) ==="
    pin=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-before-$arm.txt"); log "pin before $arm: $pin"
    t_arm=$(date +%s)
    for c in "${CASES[@]}"; do
        [ -f "$OUT/state/$arm/$c.done" ] && continue
        rm -rf "$OUT/archive/$arm/$c-DirectVulkan"
        if [ "$c" = "$W2_ITERATIONRP" ]; then
            # apk.yml:460-462, verbatim; trace-replay-ci.sh reads them from the environment.
            export MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH=1 MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS=1 \
                   MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER=1
        else
            unset MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS \
                  MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER
        fi
        attempt=1
        while :; do
            w2_require_device
            boot0=$(w2_boot_id)
            w2_same_session "$SESSION_BOOT" "before $arm $c" "$boot0"
            w2_wake
            t0=$(date +%s)
            # shellcheck disable=SC2046
            python3 "$RUNNER" --case "$c" --backend DirectVulkan --use-pbuffer $(arm_args "$arm") \
                --archive-dir "$OUT/archive/$arm" > "$OUT/logs/$arm/$c.attempt$attempt.log" 2>&1
            rc=$?
            t1=$(date +%s)
            # trace-replay-ci.sh's "infrastructure" verdict (adb/system_server gone, not the case):
            # the CI lane retries once, so does this driver - and says so in the done marker.
            if [ $attempt -eq 1 ] && grep -q 'requesting one infrastructure retry' "$OUT/logs/$arm/$c.attempt1.log"; then
                log "  $arm $c: infrastructure failure, one retry"
                attempt=2; rm -rf "$OUT/archive/$arm/$c-DirectVulkan"; sleep 20; continue
            fi
            break
        done
        cp "$OUT/logs/$arm/$c.attempt$attempt.log" "$OUT/logs/$arm/$c.log"
        # The runner rewrites <archive>/run.json per invocation; keep each case's copy beside it.
        [ -f "$OUT/archive/$arm/run.json" ] && mkdir -p "$OUT/archive/$arm/$c-DirectVulkan" \
            && mv "$OUT/archive/$arm/run.json" "$OUT/archive/$arm/$c-DirectVulkan/run.json" 2>/dev/null
        # Session stamp on every repeat the runner archived (complete or not), read AFTER the run: a
        # reboot inside the invocation shows as boot_id != boot_id_before.
        boot1=$(w2_boot_id)
        for d in "$OUT/archive/$arm/$c-DirectVulkan"/repeat-*/; do
            [ -d "$d" ] && printf 'boot_id=%s boot_id_before=%s stamped=%s\n' "$boot1" "$boot0" "$(date -Is)" > "$d/boot_id.txt"
        done
        passes=$(grep -c '"passed": true' "$OUT/logs/$arm/$c.log" || true)
        miss=$(missing_repeats "$OUT/archive/$arm/$c-DirectVulkan" "$(arm_repeats "$arm")")
        if [ -n "$boot1" ] && [ "$boot1" != "$SESSION_BOOT" ]; then
            printf '%s\t%s\t%s\t%s\t%s\tSESSION-BROKEN\n' "$arm" "$c" "$rc" "$((t1 - t0))" "$(date -Is)" >> "$OUT/progress.tsv"
            w2_same_session "$SESSION_BOOT" "after $arm $c" "$boot1"
        fi
        if [ -n "$miss" ] || [ -z "$boot1" ]; then
            # No .done: the next start of this script re-runs the pair from scratch.
            printf '%s\t%s\t%s\t%s\t%s\tINCOMPLETE\n' "$arm" "$c" "$rc" "$((t1 - t0))" "$(date -Is)" >> "$OUT/progress.tsv"
            why=${miss:+$miss lack result.json / actual PNG}
            [ -z "$boot1" ] && why="${why:+$why; }boot_id unreadable after the run"
            log "  $arm $c rc=$rc $((t1 - t0))s INCOMPLETE ($why): no .done, the next run retries the pair"
            INCOMPLETE=$((INCOMPLETE + 1))
            continue
        fi
        printf 'rc=%s seconds=%s attempts=%s passed_lines=%s boot_id=%s boot_id_before=%s finished=%s\n' \
            "$rc" "$((t1 - t0))" "$attempt" "$passes" "$boot1" "$boot0" "$(date -Is)" > "$OUT/state/$arm/$c.done"
        printf '%s\t%s\t%s\t%s\t%s\tdone\n' "$arm" "$c" "$rc" "$((t1 - t0))" "$(date -Is)" >> "$OUT/progress.tsv"
        log "  $arm $c rc=$rc $((t1 - t0))s"
    done
    pin=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-after-$arm.txt"); log "pin after $arm: $pin"
    w2_timing "$(w2_out "$STAMP")" "gate3-$arm" "$t_arm" "$(date +%s)" "${#CASES[@]} cases"
    log "=== arm $arm done in $(( $(date +%s) - t_arm ))s ==="
done
w2_timing "$(w2_out "$STAMP")" gate3 "$t_all" "$(date +%s)" "incomplete=$INCOMPLETE"
if [ $INCOMPLETE -gt 0 ]; then
    log "=== GATE3 INCOMPLETE $STAMP: $INCOMPLETE pair(s) without .done (progress.tsv 'INCOMPLETE'); run the same command again to retry them ==="
    exit 3
fi
log "=== GATE3 DONE $STAMP ==="
