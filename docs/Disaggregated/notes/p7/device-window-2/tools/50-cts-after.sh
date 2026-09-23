#!/usr/bin/env bash
# 50-cts-after.sh <stamp> [--blocks shader-image,ssbo,dsa,texture,packed-pixels] [--limit N]
#                         [--restore-base-lib]
#
# CONTRACT-P7 7.3 exit gate 5, AFTER reading: the five KHR-GL46 blocks, inproc x DirectVulkan,
# against device-window-1/CTS-base ($BASE = monolith x DirectVulkan, p7w1). Same glcts bundle,
# same caselists, same skip file and the same run_cts.py flags as $BASE (CTS-base/README.md,
# ~/w7/logs/cts-a64/base/run-base-inner.sh); the ONLY differences are the library and the arm:
#   MOBILEGL_TRANSPORT=inproc + the split acceptance knobs the retrace split lane uses
#   (MOBILEGL_IPC_ROLE_SPLIT_STATE=1 MOBILEGL_IPC_STRICT_ERRORS=1 MOBILEGL_IPC_RUN_AHEAD=1).
# The library is this build's APK arm64 libMobileGL.so, pushed to /data/local/tmp/mgcts exactly as
# ~/w7/logs/cts-a64/deploy/deploy.sh pushed the $BASE one, and verified by sha256 on the device.
#
# Arm proof before any block: mgprobe (the SKILL step-5 preflight) and one glcts case, each under
# the AFTER environment, must log ConfigLoader's "Config: MOBILEGL_TRANSPORT=inproc - the MGPipe
# record stream" and a "Config: IPC" line with strict=1 role-split-state=1 run-ahead=1; the proof
# case's StatusCode is recorded in arm-proof.txt (a WARN when it is neither Pass nor Fail).
# RESUMABLE per block (<out>/cts/runs/<block>/.done), and a block's .done is written ONLY when the
# block is a complete reading: run_cts.py exited 0 and 60-reduce.py --check-block (the very test the
# verdict applies) finds report-<block>.json with a result for every non-skipped case of the
# caselist and unrun.txt empty. Otherwise the block is logged INCOMPLETE (runs/<block>/complete.txt
# says why) and redone from scratch by the next run (exit 3 = the loop finished with such blocks;
# window.sh carries on and leaves the step for the next run). Each block keeps a copy of the
# caselist it ran (runs/<block>/caselist.txt) beside its path, so the window's output reduces on its
# own after the build tree is gone. 60-reduce.py requires all five blocks. The UBO block
# (GTF-GL46 uniform_buffer_object) is not run: this glcts has no GTF module (ID-P7-16).
# --limit N runs the first N cases of each block (the dry run). --restore-base-lib re-pushes the
# $BASE library at once; 90-restore.sh does it at the end of every window anyway.
set -uo pipefail
. "$(dirname "$0")/lib.sh"

[ $# -ge 1 ] || die "usage: $0 <stamp> [--blocks list] [--limit N] [--restore-base-lib]"
STAMP=$1; shift
BLOCKS="shader-image ssbo dsa texture packed-pixels"; LIMIT=0; RESTORE_BASE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --blocks) BLOCKS=${2//,/ }; shift 2;;
        --limit) LIMIT=$2; shift 2;;
        --restore-base-lib) RESTORE_BASE=1; shift;;
        *) die "unknown option $1";;
    esac
done
w2_load_build "$STAMP"
OUT=$(w2_out "$STAMP")/cts
mkdir -p "$OUT/deploy" "$OUT/runs" "$OUT/caselists"
w2_require_device
w2_lock
CTS_ENV=(MOBILEGL_CTS_FBO_COLOR_TEXTURE=1 MOBILEGL_TRANSPORT=inproc $W2_SPLIT_KNOBS)
ENV_ARGS=(); for kv in "${CTS_ENV[@]}"; do ENV_ARGS+=(--env "$kv"); done
DEV_ENV="MOBILEGL_BACKEND_TYPE=DirectVulkan ${CTS_ENV[*]}"
t_all=$(date +%s)

# ---- deploy ---------------------------------------------------------------------------------
for f in glcts mgprobe; do
    [ "$(Ash "test -x $W2_CTS_DEV/$f && echo ok")" = ok ] || die "$W2_CTS_DEV/$f missing: deploy the bundle first ($W2_CTS_BUNDLE/deploy.sh $W2_SERIAL)"
done
[ "$(Ash "test -d $W2_CTS_DEV/gl_cts && echo ok")" = ok ] || die "$W2_CTS_DEV/gl_cts missing"
unzip -o -q -j "$W2_APK" lib/arm64-v8a/libMobileGL.so -d "$OUT/deploy" || die "no arm64 libMobileGL.so in $W2_APK"
WANT=$(sha256sum "$OUT/deploy/libMobileGL.so" | cut -d' ' -f1)
BEFORE=$(Ash "sha256sum $W2_CTS_DEV/libMobileGL.so" | cut -d' ' -f1)
{
    echo "apk            : $W2_APK"
    echo "apk sha256     : $(sha256sum "$W2_APK" | cut -d' ' -f1)"
    echo "lib sha256     : $WANT"
    echo "lib stamp hits : $(strings "$OUT/deploy/libMobileGL.so" | grep -c -F "${BUILD_STAMP:-$STAMP}" || true) (${BUILD_STAMP:-$STAMP})"
    echo "device before  : $BEFORE"
    echo "glcts sha256   : $(Ash "sha256sum $W2_CTS_DEV/glcts" | cut -d' ' -f1)"
    echo "env            : $DEV_ENV"
} > "$OUT/deploy/IDENTITY.txt"
if [ "$BEFORE" != "$WANT" ]; then
    A push "$OUT/deploy/libMobileGL.so" "$W2_CTS_DEV/libMobileGL.so" > "$OUT/deploy/push.log" 2>&1 || die "push failed"
fi
GOT=$(Ash "sha256sum $W2_CTS_DEV/libMobileGL.so" | cut -d' ' -f1)
echo "device after   : $GOT" >> "$OUT/deploy/IDENTITY.txt"
[ "$GOT" = "$WANT" ] || die "the device does not carry the AFTER library ($GOT != $WANT)"
log "deployed AFTER lib $WANT to $W2_CTS_DEV"

# ---- preflight + arm proof ---------------------------------------------------------------------
arm_proof() {  # <label> <logcat file> -> 0 when inproc really resolved with the gate's knobs
    local f=$2 ok=1 marker ipc good fatal
    marker=$(grep -c 'Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream' "$f")
    ipc=$(grep -oE 'Config: IPC.*' "$f")
    good=$(printf '%s\n' "$ipc" | grep -E '\bstrict=1\b' | grep -E '\brole-split-state=1\b' | grep -cE '\brun-ahead=1\b')
    fatal=$(grep -c 'Fatal{' "$f")
    [ "$marker" -ge 1 ] && [ "$good" -ge 1 ] && [ "$fatal" -eq 0 ] || ok=0
    echo "$1: transport-marker=$marker ipc-lines=[$(printf '%s\n' "$ipc" | head -1)] fatal=$fatal -> $([ $ok = 1 ] && echo PROVEN || echo NOT-PROVEN)"
    [ $ok = 1 ]
}
if [ ! -f "$OUT/preflight.done" ]; then
    w2_wake
    A logcat -c
    Ash "cd $W2_CTS_DEV && LD_LIBRARY_PATH=. $DEV_ENV ./mgprobe --backend DirectVulkan --surface imagereader --lib ./libMobileGL.so" > "$OUT/preflight.txt" 2>&1
    A logcat -d > "$OUT/preflight-logcat.txt" 2>&1
    grep -q '^PASS' "$OUT/preflight.txt" || { cat "$OUT/preflight.txt"; die "mgprobe preflight did not PASS under the AFTER environment"; }
    arm_proof mgprobe "$OUT/preflight-logcat.txt" | tee "$OUT/arm-proof.txt" || die "mgprobe did not prove the inproc arm (see $OUT/preflight-logcat.txt)"
    first=$(grep -v '^#' "$W2_TOOLS/tools/cts/caselists/p7-dsa-gl46.txt" | sed '/^$/d' | head -1)
    A logcat -c
    Ash "cd $W2_CTS_DEV && echo $first > p7w7-proof.txt && $DEV_ENV LD_LIBRARY_PATH=. ./glcts --deqp-caselist-file=p7w7-proof.txt --deqp-surface-type=fbo --deqp-surface-width=256 --deqp-surface-height=256 --deqp-gl-config-name=rgba8888d24s8 --deqp-terminate-on-device-lost=disable --deqp-log-images=disable --deqp-log-shader-sources=disable --deqp-log-filename=p7w7-proof.qpa > /dev/null 2>&1; echo RC=\$?; grep -o 'StatusCode=\"[A-Za-z]*\"' p7w7-proof.qpa; rm -f p7w7-proof.txt p7w7-proof.qpa" > "$OUT/proof-glcts.txt" 2>&1
    A logcat -d > "$OUT/proof-glcts-logcat.txt" 2>&1
    arm_proof "glcts $first" "$OUT/proof-glcts-logcat.txt" | tee -a "$OUT/arm-proof.txt" || die "glcts did not prove the inproc arm (see $OUT/proof-glcts-logcat.txt)"
    # Information: a proof case that is NotSupported (or crashed) says the AFTER library may not
    # reach a GL 4.6 core context at all; 60-reduce.py FAILs a block without a single Pass/Fail.
    proof_status=$(sed -n 's/.*StatusCode="\([A-Za-z]*\)".*/\1/p' "$OUT/proof-glcts.txt" | sed -n 1p)
    echo "glcts $first: StatusCode=${proof_status:-none}" | tee -a "$OUT/arm-proof.txt"
    case "$proof_status" in
        Pass|Fail) ;;
        *) log "WARN: the proof case $first gave StatusCode=${proof_status:-none} under the AFTER environment (see $OUT/proof-glcts.txt)";;
    esac
    touch "$OUT/preflight.done"
fi

# ---- blocks -------------------------------------------------------------------------------------
INCOMPLETE=0
for b in $BLOCKS; do
    run="$OUT/runs/$b"
    [ -f "$run/.done" ] && { log "block $b already done"; continue; }
    # A report left by an interrupted attempt must not stand in for this one.
    rm -rf "$run"; rm -f "$OUT/report-$b.json" "$OUT/report-$b.txt"; mkdir -p "$run"
    caselist="$W2_TOOLS/tools/cts/caselists/p7-$b-gl46.txt"
    [ -f "$caselist" ] || die "no caselist $caselist"
    if [ "$LIMIT" -gt 0 ]; then
        grep -v '^#' "$caselist" | sed '/^$/d' | head -n "$LIMIT" > "$OUT/caselists/p7-$b-gl46.first$LIMIT.txt"
        caselist="$OUT/caselists/p7-$b-gl46.first$LIMIT.txt"
    fi
    echo "$caselist" > "$run/caselist.path"
    cp "$caselist" "$run/caselist.txt"
    pin=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-before-$b.txt")
    w2_wake
    log "block $b start ($(grep -vc '^#' "$caselist") cases, pin $pin)"
    t0=$(date +%s)
    python3 "$W2_TOOLS/tools/cts/scripts/run_cts.py" --serial "$W2_SERIAL" --backend DirectVulkan \
        --caselist "$caselist" --outdir "$run" "${ENV_ARGS[@]}" \
        --surface fbo --gl-config-name rgba8888d24s8 --cpu-mask fast \
        --chunk-timeout 900 --skip-file "$W2_TOOLS/tools/cts/caselists/p7-skip.txt" > "$OUT/run-$b.log" 2>&1
    rc=$?
    t1=$(date +%s)
    python3 "$W2_TOOLS/tools/cts/scripts/qpa_report.py" "$run" --label "DirectVulkan-inproc-$b" \
        --json "$OUT/report-$b.json" > "$OUT/report-$b.txt" 2>&1
    qrc=$?
    pin=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-after-$b.txt")
    for f in unrun hung crashed; do [ -s "$run/$f.txt" ] && log "  [$b] $f.txt: $(wc -l < "$run/$f.txt") lines"; done
    grep -E 'Pass|Fail|NotSupported|Crash|rate' "$OUT/report-$b.txt" | head -8 | sed "s/^/  [$b] /"
    w2_timing "$(w2_out "$STAMP")" "cts-$b" "$t0" "$t1" "limit=$LIMIT"
    # The verdict's own completeness test (60-reduce.py block_scope), so a block the reducer would
    # call INCOMPLETE is never frozen by a .done that a resume then skips.
    complete=$(python3 "$W2_HERE/60-reduce.py" --check-block "$run" "$OUT/report-$b.json" 2>&1); crc=$?
    [ $rc -eq 0 ] || { complete="run_cts.py rc=$rc; $complete"; crc=3; }
    echo "$complete" > "$run/complete.txt"
    if [ $crc -eq 0 ]; then
        printf 'rc=%s seconds=%s pin_after=%s finished=%s\n' "$rc" "$((t1 - t0))" "$pin" "$(date -Is)" > "$run/.done"
        log "block $b rc=$rc $((t1 - t0))s: $complete"
    else
        INCOMPLETE=$((INCOMPLETE + 1))
        log "block $b rc=$rc $((t1 - t0))s INCOMPLETE: $complete (qpa_report rc=$qrc, see $OUT/report-$b.txt); no .done, the next run redoes the block"
    fi
done

# The same machine-readable shape as $BASE (tools/cts/baselines/base-DirectVulkan-monolith-p7w1-
# 9be62cbc.json, cts_multi_report.py over the $BASE runs): one suite per finished block.
suites=()
for b in $BLOCKS; do
    [ -f "$OUT/runs/$b/.done" ] && suites+=(--suite DirectVulkan "$b" "$(cat "$OUT/runs/$b/caselist.path")" "$OUT/runs/$b")
done
if [ ${#suites[@]} -gt 0 ]; then
    python3 "$W2_TOOLS/tools/cts/scripts/cts_multi_report.py" "${suites[@]}" --adopt-legacy --allow-incomplete \
        --markdown "$OUT/after-DirectVulkan-inproc-$STAMP.md" --json "$OUT/after-DirectVulkan-inproc-$STAMP.json" \
        > "$OUT/multi-report.log" 2>&1 || log "WARN: cts_multi_report failed (see $OUT/multi-report.log)"
fi

if [ $RESTORE_BASE -eq 1 ]; then
    w2_restore_base_lib | tee "$OUT/deploy/restored-base.txt" || log "WARN: the \$BASE library is NOT back in $W2_CTS_DEV"
fi
w2_timing "$(w2_out "$STAMP")" cts "$t_all" "$(date +%s)" "blocks=[$BLOCKS] limit=$LIMIT incomplete=$INCOMPLETE"
if [ $INCOMPLETE -gt 0 ]; then
    log "=== CTS AFTER INCOMPLETE $STAMP: $INCOMPLETE block(s) not a complete reading (runs/<block>/complete.txt); run the same command again to redo them ==="
    exit 3
fi
log "=== CTS AFTER DONE $STAMP ==="
