#!/usr/bin/env bash
# desktop-verify.sh <phase>... - check that the MobileGL desktop starts, idles and stops by itself
# (anland app: foreground service + mobilegl-desktop.sh; container: desktop-session.service).
# Phases run in the order given; screenshots go to $ANL_DIR/dv-<phase>.png.
#
#   push     install device/*.sh into $DEV_TOOLS (push-tools.sh's device half)
#   reset    everything down: app force-stopped (its service with it), container stopped,
#            experiment display daemon killed - the state after a phone reboot
#   cold     reset, then open the app like its icon does and time each piece (desktop-timeline.sh)
#   open     open the app without resetting (after a reboot: the reboot was the reset)
#   chrome   (re)start Chrome in the session on an animated WebGL page (kills a running Chrome)
#   idle     CPU/GPU for visible, hidden (HOME), locked (screen off), visible again (measure.sh)
#   reopen   swipe the app away (remove its task), check KWin survives, open it again, time it
#   stop     Stop desktop from the notification (the service's STOP action) and check it all ends
#   rtest    lock/unlock + window resize regression ($ANL_DIR/rtest.sh)
# Env: SECS (idle window, default 20), URL (chrome page).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
SECS=${SECS:-20}
URL=${URL:-https://webglsamples.org/aquarium/aquarium.html}
SOCK=$DEV_SHARE/display.sock
su_() { adb_ shell "su -c '$*'"; }
shot() {
    su_ "screencap -p $DEV_SHARE/dv-$1.png"
    adb_ pull "$DEV_SHARE/dv-$1.png" "$ANL_DIR/dv-$1.png" > /dev/null && echo "screenshot $ANL_DIR/dv-$1.png"
}
kwin_log() {   # the anland backend's lines since <time>
    printf '%s\n' "journalctl --no-pager --since '$1' 2>/dev/null | grep -a -E 'viewer (gone|back)|consumer (reconnected|disconnected)|failed to connect' | tail -${2:-8}" |
        bash "$here/ct.sh" dv 2>/dev/null | grep -v pushed || true
}
task_id() {
    su_ "am stack list" | tr -d '\r' | grep -m1 "$MGL_PKG/" | sed -n 's/.*taskId=\([0-9]*\).*/\1/p'
}
wake() {
    su_ "input keyevent KEYCODE_WAKEUP"; sleep 1; su_ "wm dismiss-keyguard"; sleep 2
}

for phase in "$@"; do
    echo "=== $phase"
    case $phase in
    push)
        for f in "$here"/device/*.sh; do
            tr -d '\r' < "$f" > "$ANL_DIR/push.tmp"
            adb_ push "$ANL_DIR/push.tmp" "$DEV_TOOLS/$(basename "$f")" > /dev/null
        done
        rm -f "$ANL_DIR/push.tmp"; echo "device scripts in $DEV_TOOLS" ;;
    reset)
        su_ "sh $DEV_TOOLS/desktop-reset.sh" ;;
    cold)
        bash "$0" reset
        wake
        su_ "sh $DEV_TOOLS/desktop-timeline.sh 240"
        sleep 3; shot cold ;;
    open)
        wake
        su_ "sh $DEV_TOOLS/desktop-timeline.sh 300"
        sleep 3; shot open ;;
    chrome)
        WAIT=15 bash "$here/chrome-launch.sh" dv "$URL" ;;
    idle)
        su_ "sh $DEV_TOOLS/measure.sh visible $SECS"
        t=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
        su_ "input keyevent KEYCODE_HOME"; sleep 4
        su_ "sh $DEV_TOOLS/measure.sh hidden $SECS"
        su_ "am start -n $MGL_PKG/com.anland.consumer.MainActivity >/dev/null"; sleep 5
        su_ "input keyevent KEYCODE_SLEEP"; sleep 4
        su_ "sh $DEV_TOOLS/measure.sh locked $SECS"
        wake
        su_ "am start -n $MGL_PKG/com.anland.consumer.MainActivity >/dev/null"; sleep 5
        su_ "sh $DEV_TOOLS/measure.sh visible2 $SECS"
        shot idle
        kwin_log "$t" 12 ;;
    reopen)
        t=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
        id=$(task_id); echo "removing task $id"
        [ -n "$id" ] && su_ "am stack remove $id"
        sleep 8
        su_ "echo server: \$(pidof $MGL_PKG:mobilegl) ui: \$(pidof $MGL_PKG) kwin: \$(pidof kwin_wayland) plasmashell: \$(pidof plasmashell)"
        su_ "sh $DEV_TOOLS/measure.sh closed 10"
        su_ "sh $DEV_TOOLS/desktop-timeline.sh 120"
        sleep 3; shot reopen
        kwin_log "$t" ;;
    stop)
        su_ "am startservice -n $MGL_PKG/com.anland.consumer.MobileGLWorker -a com.anland.consumer.mobilegl.STOP" || true
        sleep 25
        su_ "echo server: \$(pidof $MGL_PKG:mobilegl) ui: \$(pidof $MGL_PKG); $DS_BIN show"
        su_ "tail -3 $DEV_SHARE/desktop.log" ;;
    rtest)
        bash "$ANL_DIR/rtest.sh" dv ;;
    *)
        echo "unknown phase $phase" >&2; exit 2 ;;
    esac
done
