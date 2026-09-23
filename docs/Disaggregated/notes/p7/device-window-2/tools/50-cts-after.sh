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
# record stream" and a "Config: IPC" line with strict=1 role-split-state=1 run-ahead=1.
# RESUMABLE per block (<out>/cts/runs/<block>/.done). --limit N runs the first N cases of each
# block (the dry run). --restore-base-lib re-pushes the $BASE library afterwards (dry run only).
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
    touch "$OUT/preflight.done"
fi

# ---- blocks -------------------------------------------------------------------------------------
for b in $BLOCKS; do
    run="$OUT/runs/$b"
    [ -f "$run/.done" ] && { log "block $b already done"; continue; }
    rm -rf "$run"; mkdir -p "$run"
    caselist="$W2_TOOLS/tools/cts/caselists/p7-$b-gl46.txt"
    [ -f "$caselist" ] || die "no caselist $caselist"
    if [ "$LIMIT" -gt 0 ]; then
        grep -v '^#' "$caselist" | sed '/^$/d' | head -n "$LIMIT" > "$OUT/caselists/p7-$b-gl46.first$LIMIT.txt"
        caselist="$OUT/caselists/p7-$b-gl46.first$LIMIT.txt"
    fi
    echo "$caselist" > "$run/caselist.path"
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
    pin=$(w2_pin_check "$W2_TOOLS" "$OUT/pin-after-$b.txt")
    for f in unrun hung crashed; do [ -s "$run/$f.txt" ] && log "  [$b] $f.txt: $(wc -l < "$run/$f.txt") lines"; done
    grep -E 'Pass|Fail|NotSupported|Crash|rate' "$OUT/report-$b.txt" | head -8 | sed "s/^/  [$b] /"
    printf 'rc=%s seconds=%s pin_after=%s finished=%s\n' "$rc" "$((t1 - t0))" "$pin" "$(date -Is)" > "$run/.done"
    w2_timing "$(w2_out "$STAMP")" "cts-$b" "$t0" "$t1" "limit=$LIMIT"
    log "block $b rc=$rc $((t1 - t0))s"
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
    BASE_LIB="$W2_CTS_BUNDLE/libMobileGL.so"
    BASE_SHA=$(awk '/^libMobileGL.so  *sha256:/ {print $3}' "$W2_CTS_BUNDLE/IDENTITY.txt")
    A push "$BASE_LIB" "$W2_CTS_DEV/libMobileGL.so" >/dev/null 2>&1
    GOT=$(Ash "sha256sum $W2_CTS_DEV/libMobileGL.so" | cut -d' ' -f1)
    [ "$GOT" = "$BASE_SHA" ] && log "restored the \$BASE library ($BASE_SHA)" || log "WARN: restore mismatch $GOT != $BASE_SHA"
fi
w2_timing "$(w2_out "$STAMP")" cts "$t_all" "$(date +%s)" "blocks=[$BLOCKS] limit=$LIMIT"
log "=== CTS AFTER DONE $STAMP ==="
