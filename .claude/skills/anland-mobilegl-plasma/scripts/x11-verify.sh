#!/usr/bin/env bash
# x11-verify.sh <step> [...] - device verification of X11 on the GPU (docs/Disaggregated/notes/anland/
# plan-x11-gpu.md): Xwayland drawing with glamor on MobileGL, and GLX windows presented through
# DRI3 + Present from shared images. Steps, in the order a full run takes them:
#
#   preflight             read-only: battery, stack status, Xwayland package, render node, xcb libs,
#                         test apps, Xwayland's extensions and its GLX vendor
#   measure <label> [s]   glxgears (vblank_mode=0) for s seconds (10): its fps, then CPU/GPU/dma-buf
#                         counts (device/x11-measure.sh); also with MOBILEGL_GLX_PRESENT=readback
#   deploy-client         xbuild.sh + xdeploy.sh of this worktree's client (MGL_XBUILD_DIR)
#   deploy-kwin           kwin-stage.sh build from ANLAND_WORKTREE; waits for "== ALL DONE =="
#   deploy-server         build-android.sh + cyc.sh x11-server (swaps the APK lib, RESTARTS the session)
#   check                 the evidence: Xwayland took glamor (journal), its MobileGL log, GLX's path
#                         line, dma-buf counts in Xwayland/KWin, the server's import lines
#   apps                  xterm + xeyes + glxgears on screen, two screenshots (hashes must differ)
#   stop                  kill the test apps
#
# Typical run: preflight; measure before; deploy-client; deploy-kwin; deploy-server; check; measure
# after; apps; stop. Battery: every load step refuses below MIN_BATTERY (40) %.
# Env: CHANNEL (ct.sh channel, default x11), ANLAND_WORKTREE (anland x11-gpu worktree, Windows
# form), MGL_XBUILD_DIR (default /home/swung/mgl-xbuild-x11), MIN_BATTERY.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
CH=${CHANNEL:-x11}
: "${MGL_XBUILD_DIR:=/home/swung/mgl-xbuild-x11}"
: "${ANLAND_WORKTREE:=$(cygpath -m "$USERPROFILE" 2>/dev/null)/AndroidStudioProjects/anland/.claude/worktrees/x11-gpu}"
export MGL_XBUILD_DIR ANLAND_WORKTREE

battery_ok() {
    local level
    level=$(adb_ shell "dumpsys battery" | sed -n 's/^ *level: *//p' | tr -d '\r')
    echo "battery: ${level}%"
    if [ "${level:-0}" -lt "${MIN_BATTERY:-40}" ]; then
        echo "below ${MIN_BATTERY:-40}%: stopping GPU-heavy work; charge the phone first" >&2
        return 1
    fi
}

push_measure() {
    tr -d '\r' < "$here/device/x11-measure.sh" > "$ANL_DIR/x11-measure.sh"
    adb_ push "$ANL_DIR/x11-measure.sh" "$DEV_TOOLS/x11-measure.sh" >/dev/null
}

step=${1:?usage: x11-verify.sh <preflight|measure|deploy-client|deploy-kwin|deploy-server|check|apps|stop>}
shift
case "$step" in
preflight)
    battery_ok || true
    adb_ shell "su -c 'sh $DEV_TOOLS/status.sh'" || true
    bash "$here/ct.sh" "$CH" <<'IN'
echo "== packages"; pacman -Q xorg-xwayland libxcb mesa-utils xorg-xeyes xterm xorg-xdpyinfo 2>&1 | sed 's/^/  /'
echo "== render nodes"; ls -l /dev/dri/ 2>&1 | sed 's/^/  /'
U=swung0x48; for n in /dev/dri/renderD*; do sudo -u $U test -r "$n" -a -w "$n" && echo "  $U can open $n"; done
echo "== KWin's thin node"; tr '\0' '\n' < /proc/$(pgrep -x kwin_wayland)/environ 2>/dev/null | grep -E "MOBILEGL_GBM_NODE|ANLAND_XWAYLAND_GLAMOR|GBM_BACKEND" | sed 's/^/  /'
echo "== xcb libraries"; for l in libxcb-dri3.so.0 libxcb-present.so.0 libxcb-xfixes.so.0 libxcb-shm.so.0; do ls /usr/lib/$l >/dev/null 2>&1 && echo "  $l ok" || echo "  $l MISSING"; done
echo "== Xwayland"; pgrep -a -x Xwayland | sed 's/^/  /'
mgrun offscreen xdpyinfo 2>/dev/null | grep -E "^ +(DRI3|Present|MIT-SHM|XFIXES|GLX)$" | sed 's/^/  ext/'
mgrun offscreen glxinfo -B 2>&1 | grep -E "vendor|renderer|direct rendering" | sed 's/^/  /'
IN
    ;;
measure)
    label=${1:?usage: x11-verify.sh measure <label> [seconds]}
    secs=${2:-10}
    battery_ok
    push_measure
    for mode in auto readback; do
        printf '%s\n' \
            "pkill -x glxgears; rm -f /tmp/mgl-x11-gears.client.log /tmp/x11-gears.out" \
            "MGLOG=/tmp/mgl-x11-gears timeout $((secs + 8)) mgrun offscreen env vblank_mode=0 MOBILEGL_GLX_PRESENT=$mode glxgears > /tmp/x11-gears.out 2>&1 &" \
            "sleep 3" | bash "$here/ct.sh" "$CH" >/dev/null
        adb_ shell "su -c 'sh $DEV_TOOLS/x11-measure.sh $label-$mode glxgears $secs'" || true
        sleep 6
        printf '%s\n' "grep -a 'frames in' /tmp/x11-gears.out | tail -3; grep -a 'presented through' /tmp/mgl-x11-gears.client.log | head -1; pkill -x glxgears; true" \
            | bash "$here/ct.sh" "$CH"
    done
    ;;
deploy-client)
    bash "$here/xbuild.sh"
    CHANNEL=$CH bash "$here/xdeploy.sh"
    ;;
deploy-kwin)
    CHANNEL=$CH bash "$here/kwin-stage.sh" build
    for _ in $(seq 1 120); do
        out=$(printf 'tail -3 /root/kwin-build.log\n' | bash "$here/ct.sh" "$CH" 2>/dev/null || true)
        echo "$out" | tail -1
        echo "$out" | grep -q "== ALL DONE ==" && exit 0
        echo "$out" | grep -qiE "error|failed" && { echo "KWin build failed" >&2; exit 1; }
        sleep 30
    done
    echo "KWin build did not finish in an hour" >&2; exit 1
    ;;
deploy-server)
    battery_ok
    bash "$here/build-android.sh"
    CHANNEL=$CH bash "$here/cyc.sh" x11-server
    ;;
check)
    bash "$here/ct.sh" "$CH" <<'IN'
echo "== Xwayland's command line (no -shm with glamor)"; pgrep -a -x Xwayland | sed 's/^/  /'
echo "== Xwayland's glamor (journal)"
journalctl --no-pager --since -30min 2>/dev/null | grep -aiE "glamor|Xwayland.*(EGL|gbm|dri3|falling back)" | tail -10 | sed 's/^/  /'
echo "== Xwayland's MobileGL client log"; grep -aE "presented|shared image|declin|Fatal|ERROR" /tmp/mgl-xwayland.client.log 2>/dev/null | tail -8 | sed 's/^/  /' || echo "  (none)"
X=$(pgrep -x Xwayland); K=$(pgrep -x kwin_wayland)
echo "== dma-bufs held: Xwayland $(ls -l /proc/$X/fd 2>/dev/null | grep -c dmabuf), kwin $(ls -l /proc/$K/fd 2>/dev/null | grep -c dmabuf)"
echo "== Xwayland's GLX vendor and extensions"
mgrun offscreen xdpyinfo 2>/dev/null | grep -E "^ +(DRI3|Present|MIT-SHM)$" | sed 's/^/  ext/'
echo "== a GLX window's path"
rm -f /tmp/mgl-x11-check.client.log
MGLOG=/tmp/mgl-x11-check timeout 6 mgrun offscreen glxgears >/dev/null 2>&1
grep -a "presented through\|leaves DRI3" /tmp/mgl-x11-check.client.log | sed 's/^/  /'
IN
    adb_ logcat -d | grep -aE "MobileGL.*(shared image|dma-buf import|refused)" | tail -8 || true
    adb_ logcat -b crash -d | tail -5 || true
    ;;
apps)
    battery_ok
    printf '%s\n' \
        "pkill -x xterm; pkill -x xeyes; pkill -x glxgears" \
        "MGLOG=/tmp/mgl-x11-xterm mgrun offscreen xterm -geometry 80x24+40+40 >/dev/null 2>&1 &" \
        "MGLOG=/tmp/mgl-x11-xeyes mgrun offscreen xeyes -geometry 200x150+700+40 >/dev/null 2>&1 &" \
        "MGLOG=/tmp/mgl-x11-gears mgrun offscreen glxgears >/dev/null 2>&1 &" \
        "sleep 4; pgrep -a -x xterm; pgrep -a -x xeyes; pgrep -a -x glxgears; true" | bash "$here/ct.sh" "$CH"
    bash "$here/shot.sh" x11-apps 2 1
    ;;
stop)
    printf '%s\n' "pkill -x xterm; pkill -x xeyes; pkill -x glxgears; true" | bash "$here/ct.sh" "$CH"
    ;;
*)
    echo "unknown step $step" >&2; exit 2
    ;;
esac
