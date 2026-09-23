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
# RESUMABLE: one runner invocation per (arm, case); a finished pair writes
# <out>/gate3/state/<arm>/<case>.done and is skipped on the next start. An interrupted pair is
# re-run from scratch (its partial archive is removed first). Run it detached:
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
export MOBILEGL_TRACE_PACKAGE=$W2_PKG MOBILEGL_TRACE_SKIP_INSTALL=1 MOBILEGL_TRACE_APK=$W2_APK
RUNNER="$W2_TOOLS/tools/trace_replay/run_android_retrace_local.py"

# ---- the case list is frozen on first start so a resume cannot change the denominator ----------
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

t_all=$(date +%s)
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
        [ -f "$OUT/archive/$arm/run.json" ] && mv "$OUT/archive/$arm/run.json" "$OUT/archive/$arm/$c-DirectVulkan/run.json" 2>/dev/null
        passes=$(grep -c '"passed": true' "$OUT/logs/$arm/$c.log" || true)
        printf 'rc=%s seconds=%s attempts=%s passed_lines=%s finished=%s\n' "$rc" "$((t1 - t0))" "$attempt" "$passes" "$(date -Is)" \
            > "$OUT/state/$arm/$c.done"
        printf '%s\t%s\t%s\t%s\t%s\n' "$arm" "$c" "$rc" "$((t1 - t0))" "$(date -Is)" >> "$OUT/progress.tsv"
        log "  $arm $c rc=$rc $((t1 - t0))s"
    done
    pin=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-after-$arm.txt"); log "pin after $arm: $pin"
    w2_timing "$(w2_out "$STAMP")" "gate3-$arm" "$t_arm" "$(date +%s)" "${#CASES[@]} cases"
    log "=== arm $arm done in $(( $(date +%s) - t_arm ))s ==="
done
w2_timing "$(w2_out "$STAMP")" gate3 "$t_all" "$(date +%s)"
log "=== GATE3 DONE $STAMP ==="
