#!/usr/bin/env bash
# dry-run-p7w6.sh - the plumbing proof recorded in RUNBOOK.md section 5, against the p7w6 APK that
# is ALREADY installed (nothing is installed or uninstalled): 2 gate-3 cases x all four arms x1,
# bsl stats on inproc once, CTS AFTER on the first 20 dsa cases (then the $BASE lib goes back),
# reduce, restore. Not a verdict run.
set -uo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export W2_LOGROOT=${W2_LOGROOT:-$HOME/w7/logs/devprep/w2}
export W2_APK_OVERRIDE=${W2_APK_OVERRIDE:-$HOME/w7/logs/p7w6/trace-p7w6.apk}
export W2_TOOLS_OVERRIDE=${W2_TOOLS_OVERRIDE:-$HOME/w7/p7-devprep}
STAMP=${1:-dry-p7w6}
CASES=${DRY_CASES:-OpenRA,minecraft-1.21.4-fabric-iris-bsl-in-world}
echo "=== DRY RUN START $(date -Is) stamp=$STAMP apk=$W2_APK_OVERRIDE tools=$W2_TOOLS_OVERRIDE ==="
bash "$HERE/21-preflight.sh" "$STAMP" || exit 1
bash "$HERE/20-install.sh" "$STAMP" --verify-only || exit 1
bash "$HERE/30-gate3.sh" "$STAMP" --cases "$CASES" --repeat 1
bash "$HERE/40-bsl-stats.sh" "$STAMP" --arms inproc
bash "$HERE/50-cts-after.sh" "$STAMP" --blocks dsa --limit 20 --restore-base-lib
python3 "$HERE/60-reduce.py" "$STAMP" > "$W2_LOGROOT/$STAMP/verdict.txt" 2>&1; echo "reduce rc=$?" >> "$W2_LOGROOT/$STAMP/verdict.txt"
bash "$HERE/90-restore.sh" "$STAMP"
cat "$W2_LOGROOT/$STAMP/verdict.txt"
echo "=== DRY RUN DONE $(date -Is) ==="
