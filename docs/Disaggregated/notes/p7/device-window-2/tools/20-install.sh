#!/usr/bin/env bash
# 20-install.sh <stamp> [--verify-only]
#
# Installs <out>/apk/trace-<stamp>.apk over the side-by-side package (adb install -r keeps the
# package's data and never uninstalls anything), waits for dex2oat, and PROVES the identity: the
# on-device base.apk sha256 must equal the signed file's. That on-device hash - not run.json, not
# the newest Gradle output (W6-verify README's wrong-SHA note) - is what names the binary behind
# every picture of the window. --verify-only skips the install (the dry run proves p7w6 this way).
# The TCP supervisor dies with the package on install; 90-restore.sh restarts it if it was up.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

[ $# -ge 1 ] || die "usage: $0 <stamp> [--verify-only]"
STAMP=$1; VERIFY_ONLY=0; [ "${2:-}" = --verify-only ] && VERIFY_ONLY=1
w2_load_build "$STAMP"
OUT=$(w2_out "$STAMP")/session
mkdir -p "$OUT"
w2_require_device
w2_lock
t0=$(date +%s)
WANT=$(sha256sum "$W2_APK" | cut -d' ' -f1)
echo "$WANT  $W2_APK" > "$OUT/apk.sha256"
if [ $VERIFY_ONLY -eq 0 ]; then
    Ash "pm list packages | grep -i mobilegl" > "$OUT/packages-before.txt" || true
    log "installing $W2_APK ($WANT)"
    A install -r "$W2_APK" 2>&1 | tr -d '\r' | tee "$OUT/install.txt"
    grep -q Success "$OUT/install.txt" || die "install failed"
fi
Ash "dumpsys package $W2_PKG" | grep -E 'versionName|versionCode|lastUpdateTime|codePath|primaryCpuAbi' > "$OUT/apk-install.txt"
cat "$OUT/apk-install.txt"
BASE=$(Ash "pm path $W2_PKG" | sed -n 's/^package://p' | grep 'base.apk' | head -1)
GOT=$(Ash "sha256sum $BASE" | cut -d' ' -f1)
echo "device base.apk: $BASE $GOT" | tee "$OUT/apk-on-device.sha256"
[ "$GOT" = "$WANT" ] || die "installed APK ($GOT) is not $W2_APK ($WANT)"
if [ -n "${BUILD_SHA:-}" ]; then
    grep -q "versionName=.*${BUILD_SHA:0:7}" "$OUT/apk-install.txt" || die "versionName does not carry ${BUILD_SHA:0:7}"
fi
# An am_kill "due to installPackageLI" inside an arm measures the installer (runbook 1 section 3).
Ash 'n=0; while pgrep dex2oat >/dev/null && [ $n -lt 150 ]; do sleep 2; n=$((n+1)); done; echo "dex2oat idle after $((n*2))s"' | tee "$OUT/dex2oat.txt"
w2_timing "$(w2_out "$STAMP")" install "$t0" "$(date +%s)" "verify_only=$VERIFY_ONLY"
log "=== INSTALL OK $STAMP (on-device sha256 $GOT) ==="
