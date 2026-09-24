#!/usr/bin/env bash
# final-run.sh <stamp>: install and run every device check in order on one build.
set -uo pipefail
STAMP=$1
D=/home/swung/w7/logs/p12/device
H=/home/swung/w7/notes/p12/android
prev=$(ls -d "$D"/[1-7]-* 2>/dev/null | head -1)
if [ -n "$prev" ]; then
    arch=$D/trial-$(cat "$D/.stamp" 2>/dev/null || echo unknown)
    mkdir -p "$arch"; mv "$D"/[1-7]-* "$arch"/
fi
echo "${STAMP#p12w1-}" > "$D/.stamp"
bash /home/swung/w7/notes/p7/window2/20-install.sh "$STAMP" 2>&1 | tail -3
export STAMP
bash "$H/check1.sh" > /dev/null 2>&1                                          # 1 Espryt on-screen
bash "$H/check3.sh" > /dev/null 2>&1                                          # 3 sequential + Busy
LONG_CASE=minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 bash "$H/check4.sh" > /dev/null 2>&1   # 4 window lost
BACKEND=DirectVulkan SECOND_CASE=minecraft-1.21.4-startup bash "$H/check1.sh" > /dev/null 2>&1 # 2 Magma
bash "$H/check5.sh" > /dev/null 2>&1                                          # 5 + 6 offscreen service
LONG_CASE=minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 bash "$H/check7.sh" > /dev/null 2>&1   # 7 one server
echo "all checks ran for $STAMP"
ls "$D"
