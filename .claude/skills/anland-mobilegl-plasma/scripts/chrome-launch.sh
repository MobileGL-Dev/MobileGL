#!/usr/bin/env bash
# chrome-launch.sh <tag> [extra chrome flags...] - (re)start Chrome inside the running Plasma session
# through mobilegl-startup.sh's `chrome` mode (ANGLE GLES over MobileGL EGL), client log
# /tmp/mgl-chrome-<tag>.client.log, then print its "presented through" line and save a screenshot
# to $ANL_DIR/ch-<tag>.png.  Kills the running chrome first (pkill -x, never -f).  MobileGL is the
# container's system-wide vendor (10_mobilegl.json + /etc/mobilegl/client.conf), so only the log is set here.
# Env: WAIT (s before checking, default 20), CHANNEL (ct.sh channel, default chrome),
#      CHROME_GPU (process, the default: the GPU in its own process, which presents
#      through GBM dma-bufs and is restarted by Chrome after a device loss; in-process opts out -
#      needs the anland startup script that knows MOBILEGL_CHROME_GPU; in process mode the GPU process and the browser
#      write the one client log, the GPU process last); CHROME_GPU_SANDBOX=on keeps its GPU sandbox.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
TAG=${1:?usage: chrome-launch.sh <tag> [chrome flags...]}; shift
bash "$here/ct.sh" "${CHANNEL:-chrome}" <<IN
pkill -x chrome; sleep 2
U=\$(id -u $MGL_DESKTOP_USER)
rm -f /tmp/mgl-chrome-$TAG.client.log /tmp/chrome-$TAG-out.log
sudo -u $MGL_DESKTOP_USER env XDG_RUNTIME_DIR=/run/user/\$U WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/\$U/bus MOBILEGL_LOG_FILE_PATH=/tmp/mgl-chrome-$TAG.log MOBILEGL_CHROME_GPU=${CHROME_GPU:-process} MOBILEGL_CHROME_GPU_SANDBOX=${CHROME_GPU_SANDBOX:-off} bash -c 'nohup /opt/mobilegl/bin/mobilegl-startup.sh chrome --restore-last-session $* > /tmp/chrome-$TAG-out.log 2>&1 &'
sleep ${WAIT:-20}
grep -ah 'presented through' /tmp/mgl-chrome-$TAG.client.log | head -3
IN
adb_ shell "su -c 'screencap -p $DEV_SHARE/ch-$TAG.png'"
adb_ pull "$DEV_SHARE/ch-$TAG.png" "$ANL_DIR/ch-$TAG.png" >/dev/null && echo "screenshot: $ANL_DIR/ch-$TAG.png"
