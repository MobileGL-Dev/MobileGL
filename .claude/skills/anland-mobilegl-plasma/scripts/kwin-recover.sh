#!/usr/bin/env bash
# kwin-recover.sh [rounds] - does the desktop keep working when KWin's OWN MobileGL session is lost?
# Starts animated clients (glmark2-es2-wayland: Wayland, zero-copy shared images; glxgears: X11 through
# Xwayland's GLX; with CHROME=1 also Chrome on an animated WebGL page), then per round loses the server
# session of KWin with the pid-targeted debug knob (debug.mobilegl.inject_device_lost_pid, KWin's pid as
# it sees itself in the container), waits, and checks.  A round PASSES when:
#   - the server latched KWin's session (the server log file: "injected:pid=<kwin>"; logcat is too small);
#   - KWin, plasmashell and every test client are the SAME processes (nothing restarted);
#   - KWin restarted compositing on a fresh session (its journal: "compositing restarted on a fresh
#     MobileGL session"; its client log: one more "RECOVERED");
#   - the screen still animates (shot.sh hashes differ) and the clients keep presenting;
#   - no native crash (logcat -b crash).
# Between rounds the knob is reset to 0 while the clients run, which re-arms it for the same pid.
# Stops early below MIN_BATTERY percent (the phone drains faster under load than USB charges it).
#
# Deploy first (same MobileGL commit on both sides; see SKILL.md):
#   bash scripts/build-android.sh && bash scripts/cyc.sh pre      # server -> APK, restarts the session
#   bash scripts/xbuild.sh && bash scripts/xdeploy.sh             # client -> container
#   ANLAND_WORKTREE=<anland worktree> bash scripts/kwin-stage.sh build   # kwin.patch + anland backend
#   (wait for "== ALL DONE ==" in /root/kwin-build.log), then: adb shell 'su -c "sh /data/local/tmp/anl/run-plasma.sh"'
# Env: ROUNDS (default 2), SETTLE (s after the loss, default 10), REARM (s with the knob at 0 between
#      rounds, default 4), MIN_BATTERY (default 40), CHROME (1: add Chrome), CHANNEL (ct.sh channel,
#      default krec), TAG (screenshot/log tag, default krec).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
ROUNDS=${1:-${ROUNDS:-2}}
SETTLE=${SETTLE:-10}
REARM=${REARM:-4}
MIN_BATTERY=${MIN_BATTERY:-40}
CH=${CHANNEL:-krec}
TAG=${TAG:-krec}
COMPOSITOR_LOG=/tmp/mobilegl-compositor.client.log
SERVER_LOG=/data/data/$MGL_PKG/files/mobilegl-server.server.log

ct() { bash "$here/ct.sh" "$CH"; }

battery() { adb_ shell dumpsys battery | tr -d '\r' | awk '/ level:/ {print $2}'; }
stop_clients() {
    ct <<'IN' || true
pkill -x glmark2-es2-way 2>/dev/null; pkill -x glmark2-es2-wayland 2>/dev/null; pkill -x glxgears 2>/dev/null
true
IN
}
cleanup() {
    adb_ shell "su -c 'setprop debug.mobilegl.inject_device_lost_pid 0'" || true
    stop_clients
}
check_battery() {
    local level; level=$(battery)
    echo "battery: $level%"
    if [ -n "$level" ] && [ "$level" -lt "$MIN_BATTERY" ]; then
        echo "STOP: battery below $MIN_BATTERY%"; exit 3
    fi
}
trap cleanup EXIT

check_battery
adb_ shell "su -c 'sh $DEV_TOOLS/status.sh'" || true
adb_ shell "su -c 'setprop debug.mobilegl.inject_device_lost_pid 0'"

# The clients, each with its own log (mgrun's MGLOG), in the background for the whole run.
ct <<'IN'
rm -f /tmp/mgl-krec-gm.client.log /tmp/mgl-krec-gx.client.log /tmp/krec-gm.out /tmp/krec-gx.out
MGLOG=/tmp/mgl-krec-gm.log nohup timeout 900 mgrun offscreen glmark2-es2-wayland -s 960x540 --run-forever \
  > /tmp/krec-gm.out 2>&1 < /dev/null &
MGLOG=/tmp/mgl-krec-gx.log nohup timeout 900 mgrun offscreen glxgears -info > /tmp/krec-gx.out 2>&1 < /dev/null &
sleep 8
echo "glmark2: $(pgrep -x glmark2-es2-way || pgrep -x glmark2-es2-wayland)  glxgears: $(pgrep -x glxgears)"
grep -aE "presented through|could not be presented" /tmp/mgl-krec-gm.client.log | head -2
grep -aE "presented through|readback" /tmp/mgl-krec-gx.client.log | head -2
IN
if [ "${CHROME:-0}" = 1 ]; then
    ct <<'IN'
cat > /tmp/mgl-krec-spin.html <<'HTML'
<!doctype html><html><body style="margin:0;background:#000">
<canvas id=c width=640 height=360 style="width:100vw;height:100vh"></canvas>
<script>
const gl = document.getElementById('c').getContext('webgl'); let t = 0;
function frame() { t++; if (gl && !gl.isContextLost()) { gl.clearColor((t % 60) / 60, 0.2, 0.6, 1); gl.clear(gl.COLOR_BUFFER_BIT); }
  requestAnimationFrame(frame); }
frame();
</script></body></html>
HTML
chmod 644 /tmp/mgl-krec-spin.html
IN
    WAIT=${WAIT:-20} CHANNEL=$CH bash "$here/chrome-launch.sh" "$TAG" --new-window "file:///tmp/mgl-krec-spin.html"
fi

# KWin, plasmashell and the clients, as pids inside the container; how often KWin recovered so far.
snapshot() {
    ct <<IN | tr -d '\r' | tail -1
k=\$(pgrep -x kwin_wayland | head -1); p=\$(pgrep -x plasmashell | head -1)
g=\$(pgrep -x glmark2-es2-way || pgrep -x glmark2-es2-wayland); x=\$(pgrep -x glxgears | head -1)
c=\$(pgrep -x chrome | head -1)
r=\$(grep -ac "RECOVERED" $COMPOSITOR_LOG 2>/dev/null || true)
f=\$(grep -ac "FPS" /tmp/krec-gm.out 2>/dev/null || true)
echo "\${k:-none} \${p:-none} \${g:-none} \${x:-none} \${c:-none} \${r:-0} \${f:-0}"
IN
}

pass=0
for round in $(seq 1 "$ROUNDS"); do
    echo "=== round $round/$ROUNDS"
    check_battery
    read -r kwin plasma gm gx chrome recovered fps < <(snapshot)
    echo "kwin=$kwin plasmashell=$plasma glmark2=$gm glxgears=$gx chrome=$chrome recoveries=$recovered fps-lines=$fps"
    if [ "$kwin" = none ]; then echo "FAIL: KWin is not running"; break; fi
    bash "$here/shot.sh" "$TAG-r$round-pre" 2 1
    srvlines=$(adb_ shell "su -c 'wc -l < $SERVER_LOG'" | tr -dc '0-9')
    adb_ logcat -b crash -c || true
    adb_ shell "su -c 'setprop debug.mobilegl.inject_device_lost_pid $kwin'"
    sleep "$SETTLE"
    adb_ shell "su -c 'setprop debug.mobilegl.inject_device_lost_pid 0'"
    bash "$here/shot.sh" "$TAG-r$round-post" 3 1 | tee "$ANL_DIR/$TAG-r$round-post.md5"
    read -r kwin2 plasma2 gm2 gx2 chrome2 recovered2 fps2 < <(snapshot)
    echo "after: kwin=$kwin2 plasmashell=$plasma2 glmark2=$gm2 glxgears=$gx2 chrome=$chrome2 recoveries=$recovered2 fps-lines=$fps2"
    adb_ shell "su -c 'tail -n +$((srvlines + 1)) $SERVER_LOG'" > "$ANL_DIR/$TAG-r$round-server.log"
    latched=$(grep -c "injected:pid=$kwin" "$ANL_DIR/$TAG-r$round-server.log" || true)
    grep -aE "inject_device_lost_pid|SessionLatch|session #[0-9]+ (started|pid)|owner=server|lease" \
        "$ANL_DIR/$TAG-r$round-server.log" | cut -c1-220 | head -14 || true
    ct <<IN || true
echo "--- KWin's MobileGL client log:"; grep -aE "DEVICE LOST|RECOVERED|GL_UNKNOWN_CONTEXT_RESET|re-created on the fresh|Refuse" $COMPOSITOR_LOG | tail -6
echo "--- KWin journal:"; journalctl --no-pager --since -40s 2>/dev/null | grep -aE "kwin_wayland" | grep -aiE "reset|restarted|MobileGL swap|context|fatal|abort" | tail -10
echo "--- clients:"; grep -aE "DEVICE LOST|lost|Fatal" /tmp/mgl-krec-gm.client.log /tmp/mgl-krec-gx.client.log | tail -4
IN
    crashes=$(adb_ logcat -b crash -d | grep -c "Fatal signal\|Abort message" || true)
    hashes=$(awk '{print $1}' "$ANL_DIR/$TAG-r$round-post.md5" | sort -u | wc -l)
    verdict=PASS
    [ "$latched" -ge 1 ] || { echo "  - the server never latched KWin's session (pid $kwin)"; verdict=FAIL; }
    [ "$kwin2" = "$kwin" ] || { echo "  - KWin is a different process ($kwin -> $kwin2): it restarted"; verdict=FAIL; }
    [ "$plasma2" = "$plasma" ] || { echo "  - plasmashell restarted ($plasma -> $plasma2)"; verdict=FAIL; }
    [ "$gm2" = "$gm" ] && [ "$gx2" = "$gx" ] || { echo "  - a test client restarted or died"; verdict=FAIL; }
    [ "${CHROME:-0}" != 1 ] || [ "$chrome2" = "$chrome" ] || { echo "  - Chrome restarted"; verdict=FAIL; }
    [ "$recovered2" -gt "$recovered" ] || { echo "  - KWin's client log shows no new RECOVERED session"; verdict=FAIL; }
    [ "$fps2" -gt "$fps" ] || { echo "  - glmark2 stopped reporting frames"; verdict=FAIL; }
    [ "$hashes" -ge 2 ] || { echo "  - the screen does not change after the loss (frozen)"; verdict=FAIL; }
    [ "$crashes" -eq 0 ] || { echo "  - native crash(es):"; adb_ logcat -b crash -d | tail -8; verdict=FAIL; }
    echo "round $round: $verdict"
    [ "$verdict" = PASS ] && pass=$((pass + 1))
    [ "$verdict" = PASS ] || break
    # Re-arm the knob for the same pid: a session must observe 0 before the next value counts.
    sleep "$REARM"
done
echo "=== $pass/$ROUNDS rounds passed; screenshots in $ANL_DIR/$TAG-r*-*.png"
[ "$pass" = "$ROUNDS" ]
