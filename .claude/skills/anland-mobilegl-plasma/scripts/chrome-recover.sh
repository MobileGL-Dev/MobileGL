#!/usr/bin/env bash
# chrome-recover.sh <in-process|process> [rounds] - does Chrome repaint after a GPU device loss,
# without a restart?  Starts Chrome (chrome-launch.sh, CHROME_GPU=<mode>) on an animated WebGL page,
# then per round: screenshots (the page must animate), loses the session of Chrome's GPU client with
# the server's pid-targeted debug knob (debug.mobilegl.inject_device_lost_pid; the pid as Chrome sees
# itself in the container), waits, and screenshots again.  A round PASSES when the server latched
# that pid's session, the browser process is the same one (no restart), a GPU client is drawing on a
# NEW server session, and the screen still animates.  Stops early below MIN_BATTERY percent.
#
#   in-process  the GPU in the browser process (the startup script's default today): the target is
#               the browser itself; recovery needs ANGLE to make a new native context.
#   process     the GPU in its own process (MOBILEGL_CHROME_GPU=process): the target is the GPU
#               process; Chrome restarts it after the loss, so the next round's target is a new pid.
#
# Deploy first (same MobileGL commit on both sides; see SKILL.md):
#   bash scripts/build-android.sh && bash scripts/cyc.sh pre      # server -> APK, restarts the session
#   bash scripts/xbuild.sh && bash scripts/xdeploy.sh             # client -> container
#   STARTUP=<anland worktree>/producers/kde/Arch_v5/mobilegl-startup.sh bash scripts/chrome-recover.sh deploy-startup
# Env: ROUNDS (default 2), SETTLE (s after the loss, default 8), MIN_BATTERY (default 40),
#      CHANNEL (ct.sh channel, default crec), TAG (log tag, default crec-<mode>).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
MODE=${1:?usage: chrome-recover.sh <in-process|process|deploy-startup> [rounds]}
ROUNDS=${2:-${ROUNDS:-2}}
SETTLE=${SETTLE:-8}
MIN_BATTERY=${MIN_BATTERY:-40}
CH=${CHANNEL:-crec}
TAG=${TAG:-crec-$MODE}
PAGE=/tmp/mgl-spin.html

ct() { bash "$here/ct.sh" "$CH"; }

battery() { adb_ shell dumpsys battery | tr -d '\r' | awk '/ level:/ {print $2}'; }
check_battery() {
    local level; level=$(battery)
    echo "battery: $level%"
    if [ -n "$level" ] && [ "$level" -lt "$MIN_BATTERY" ]; then
        echo "STOP: battery below $MIN_BATTERY%"; cleanup; exit 3
    fi
}
cleanup() { adb_ shell "su -c 'setprop debug.mobilegl.inject_device_lost_pid 0'" || true; }

if [ "$MODE" = deploy-startup ]; then
    # The anland startup script that knows MOBILEGL_CHROME_GPU, installed by rename (a running
    # session keeps its open copy).
    src=${STARTUP:?STARTUP=<path to the anland mobilegl-startup.sh>}
    tr -d '\r' < "$src" > "$ANL_DIR/mobilegl-startup.sh"
    adb_ push "$ANL_DIR/mobilegl-startup.sh" "$DEV_SHARE/mobilegl-startup.sh.new" >/dev/null
    ct <<'IN'
install -m 755 /run/anland-mobilegl/mobilegl-startup.sh.new /opt/mobilegl/bin/mobilegl-startup.sh.new
mv -f /opt/mobilegl/bin/mobilegl-startup.sh.new /opt/mobilegl/bin/mobilegl-startup.sh
grep -c MOBILEGL_CHROME_GPU /opt/mobilegl/bin/mobilegl-startup.sh
IN
    exit 0
fi
case "$MODE" in in-process|process) ;; *) echo "mode must be in-process or process" >&2; exit 2;; esac
trap cleanup EXIT

check_battery
adb_ shell "su -c 'sh $DEV_TOOLS/status.sh'" || true
cleanup

# The page: WebGL clears that change colour every frame plus a moving block, so a frame that
# reaches the screen changes the screenshot.
ct <<IN
cat > $PAGE <<'HTML'
<!doctype html><html><body style="margin:0;background:#000">
<canvas id=c width=640 height=360 style="width:100vw;height:100vh"></canvas>
<script>
const gl = document.getElementById('c').getContext('webgl');
let t = 0, lost = 0;
document.getElementById('c').addEventListener('webglcontextlost', e => { e.preventDefault(); lost++; });
function frame() {
  t++;
  if (gl && !gl.isContextLost()) {
    gl.clearColor((t % 60) / 60, ((t + 20) % 60) / 60, ((t + 40) % 60) / 60, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.SCISSOR_TEST); gl.scissor((t * 7) % 600, 100, 40, 160);
    gl.clearColor(1, 1, 1, 1); gl.clear(gl.COLOR_BUFFER_BIT); gl.disable(gl.SCISSOR_TEST);
  }
  document.title = 'spin ' + t + ' lost ' + lost;
  requestAnimationFrame(frame);
}
frame();
</script></body></html>
HTML
chmod 644 $PAGE
IN

WAIT=${WAIT:-20} CHROME_GPU=$MODE CHANNEL=$CH bash "$here/chrome-launch.sh" "$TAG" --new-window "file://$PAGE"

# The target pid inside the container: the browser (in-process) or the GPU process.
target_pid() {
    ct <<'IN' | tr -d '\r' | tail -1
for p in $(pgrep -x chrome); do
  args=$(tr '\0' ' ' < /proc/$p/cmdline)
  case "$args" in
    *--type=gpu-process*) echo "gpu $p" ;;
    *--type=*) ;;
    *) echo "browser $p" ;;
  esac
done | sort > /tmp/mgl-crec-pids
cat /tmp/mgl-crec-pids >&2
echo "$(awk '/^browser/ {print $2; exit}' /tmp/mgl-crec-pids) $(awk '/^gpu/ {print $2; exit}' /tmp/mgl-crec-pids)"
IN
}

pass=0
for round in $(seq 1 "$ROUNDS"); do
    echo "=== round $round/$ROUNDS ($MODE)"
    check_battery
    read -r browser gpu < <(target_pid)
    if [ "$MODE" = process ]; then target=$gpu; else target=$browser; fi
    echo "browser=$browser gpu=${gpu:-none} target=$target"
    if [ -z "$target" ]; then echo "FAIL: no target process"; break; fi
    bash "$here/shot.sh" "$TAG-r$round-pre" 2 1
    adb_ logcat -c
    adb_ shell "su -c 'setprop debug.mobilegl.inject_device_lost_pid $target'"
    sleep "$SETTLE"
    adb_ shell "su -c 'setprop debug.mobilegl.inject_device_lost_pid 0'"
    bash "$here/shot.sh" "$TAG-r$round-post" 3 1 | tee "$ANL_DIR/$TAG-r$round-post.md5"
    latched=$(adb_ logcat -d | grep -c "injected:pid=$target" || true)
    started=$(adb_ logcat -d | grep -c "session #[0-9]* started" || true)
    adb_ logcat -d | grep -E "inject_device_lost_pid|SessionLatch|session #[0-9]+ (started|pid)" | cut -c1-220 | head -12 || true
    read -r browser2 gpu2 < <(target_pid)
    echo "after: browser=$browser2 gpu=${gpu2:-none}"
    ct <<IN || true
echo "--- client log ($TAG):"; grep -aE "DEVICE LOST|RECOVERED|GL_UNKNOWN_CONTEXT_RESET|failed to make|session #" /tmp/mgl-chrome-$TAG.client.log | tail -8
echo "--- chrome stderr:"; grep -aiE "context lost|GPU process|make.*current|lost" /tmp/chrome-$TAG-out.log | tail -8
IN
    adb_ logcat -b crash -d | tail -5
    hashes=$(awk '{print $1}' "$ANL_DIR/$TAG-r$round-post.md5" | sort -u | wc -l)
    verdict=PASS
    [ "$latched" -ge 1 ] || { echo "  - the server never latched pid $target"; verdict=FAIL; }
    [ "$browser2" = "$browser" ] || { echo "  - the browser process changed ($browser -> $browser2): Chrome restarted"; verdict=FAIL; }
    [ "$started" -ge 1 ] || { echo "  - no new server session started after the loss"; verdict=FAIL; }
    if [ "$MODE" = process ]; then
        [ -n "$gpu2" ] && [ "$gpu2" != "$gpu" ] || { echo "  - no new GPU process"; verdict=FAIL; }
    fi
    [ "$hashes" -ge 2 ] || { echo "  - the screen does not change after the loss (frozen)"; verdict=FAIL; }
    echo "round $round: $verdict"
    [ "$verdict" = PASS ] && pass=$((pass + 1))
    [ "$verdict" = PASS ] || break
done
echo "=== $pass/$ROUNDS rounds passed ($MODE); screenshots in $ANL_DIR/$TAG-r*-*.png"
