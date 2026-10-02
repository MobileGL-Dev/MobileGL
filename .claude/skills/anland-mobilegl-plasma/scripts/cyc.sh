#!/usr/bin/env bash
# cyc.sh <shot-name> - one server iteration: clear Plasma's sticky software-renderer key, push the
# NDK-built server lib, swap it into the installed APK (device/swap.sh), restart app + Plasma session
# (device/run-plasma.sh, ~90 s), screenshot to $ANL_DIR/<shot-name>.png, print fatal logcat lines.
# Env: SERVER_LIB (default $MGL_WORKTREE/build-android/libMobileGL.so), WAIT (s after the session
# restart, default 15), N (logcat lines, default 20), CHANNEL (ct.sh channel, default cyc).
# Needs device/swap.sh + device/run-plasma.sh in $DEV_TOOLS (push-tools.sh).  RESTARTS THE SESSION.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
shot=${1:?usage: cyc.sh <shot-name>}
LIB=${SERVER_LIB:-$MGL_WORKTREE/build-android/libMobileGL.so}
printf '%s\n' "sudo -u $MGL_DESKTOP_USER kwriteconfig6 --file kdeglobals --group QtQuickRendererSettings --key SceneGraphBackend --delete; rm -f /tmp/mgl-plasmashell.client.log" \
    | bash "$here/ct.sh" "${CHANNEL:-cyc}"
adb_ push "$LIB" "$DEV_SHARE/libMobileGL.new.so" >/dev/null
adb_ logcat -c
adb_ shell "su -c 'sh $DEV_TOOLS/swap.sh >/dev/null && sh $DEV_TOOLS/run-plasma.sh >/dev/null 2>&1; sleep ${WAIT:-15}; screencap -p $DEV_SHARE/$shot.png'"
adb_ pull "$DEV_SHARE/$shot.png" "$ANL_DIR/$shot.png" >/dev/null && echo "screenshot: $ANL_DIR/$shot.png"
adb_ logcat -d | grep -E "Fatal signal|FATAL|Abort message|Magma wire buffer|E MobileGL" | cut -c1-260 | head -"${N:-20}" || true
