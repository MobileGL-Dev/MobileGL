#!/usr/bin/env bash
# chrome-launch.sh <tag> [extra chrome flags...] - (re)start Chrome inside the running Plasma session
# through mobilegl-startup.sh's `chrome` mode (ANGLE GLES over MobileGL EGL, in-process GPU), client
# log /tmp/mgl-chrome-<tag>.client.log, then print its "presented through" line and save a screenshot
# to $ANL_DIR/ch-<tag>.png.  Kills the running chrome first (pkill -x, never -f).
# Env: WAIT (s before checking, default 20), CHANNEL (ct.sh channel, default chrome).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
TAG=${1:?usage: chrome-launch.sh <tag> [chrome flags...]}; shift
bash "$here/ct.sh" "${CHANNEL:-chrome}" <<IN
pkill -x chrome; sleep 2
U=\$(id -u $MGL_DESKTOP_USER)
sudo -u $MGL_DESKTOP_USER env XDG_RUNTIME_DIR=/run/user/\$U WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/\$U/bus __EGL_VENDOR_LIBRARY_FILENAMES=/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=shm MOBILEGL_ENDPOINT=unix:@anland-mobilegl MOBILEGL_BACKEND_TYPE=\$(cat /etc/mobilegl/backend) MOBILEGL_LOG_FILE_PATH=/tmp/mgl-chrome-$TAG.log bash -c 'nohup /opt/mobilegl/bin/mobilegl-startup.sh chrome --restore-last-session $* > /tmp/chrome-$TAG-out.log 2>&1 &'
sleep ${WAIT:-20}
grep -ah 'presented through' /tmp/mgl-chrome-$TAG.client.log | head -3
IN
adb_ shell "su -c 'screencap -p $DEV_SHARE/ch-$TAG.png'"
adb_ pull "$DEV_SHARE/ch-$TAG.png" "$ANL_DIR/ch-$TAG.png" >/dev/null && echo "screenshot: $ANL_DIR/ch-$TAG.png"
