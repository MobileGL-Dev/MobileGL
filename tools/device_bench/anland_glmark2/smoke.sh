#!/usr/bin/env bash
# smoke.sh <tag> - (host) Chrome scrolling + lock screen greeter + Plasma screenshot on the running MobileGL desktop
S=${SKILL_SCRIPTS:-$(cd "$(dirname "$0")/../../../.claude/skills/anland-mobilegl-plasma/scripts" && pwd)}
O=${OUT_DIR:-${TEMP:-/tmp}}
export MSYS_NO_PATHCONV=1
T=${1:-smk}
printf '%s\n' 'python3 -c "print(\"<html><body style=font-size:40px>\" + \"\".join(\"<p>line %d lorem ipsum dolor sit amet</p>\" % i for i in range(400)) + \"</body></html>\")" > /tmp/long.html; chmod 644 /tmp/long.html' | bash $S/ct.sh pb >/dev/null 2>&1
WAIT=15 bash $S/chrome-launch.sh $T file:///tmp/long.html 2>&1 | grep -E "presented|screenshot"
adb shell 'su -c "screencap -p /data/local/tmp/anl/sa.png; input mouse scroll 800 1200 --axis VSCROLL,-15; sleep 1; input mouse scroll 800 1200 --axis VSCROLL,-15; sleep 2; screencap -p /data/local/tmp/anl/sb.png; md5sum /data/local/tmp/anl/sa.png /data/local/tmp/anl/sb.png | cut -c1-12"'
printf '%s\n' 'U=$(id -u swung0x48); sudo -u swung0x48 env XDG_RUNTIME_DIR=/run/user/$U WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$U/bus nohup /usr/lib/kscreenlocker_greet --testing > /tmp/greet.log 2>&1 & sleep 6; true' | bash $S/ct.sh pb >/dev/null 2>&1
adb shell 'su -c "screencap -p /data/local/tmp/anland-mobilegl/lock-'$T'.png"'; adb pull /data/local/tmp/anland-mobilegl/lock-$T.png $O/lock-$T.png >/dev/null
printf '%s\n' 'pkill -x kscreenlocker_greet; pkill -x chrome; true' | bash $S/ct.sh pb >/dev/null 2>&1
adb shell 'su -c "logcat -b crash -d -T 1 2>/dev/null | grep -c Abort; pidof com.anland.consumer.mobilegl:mobilegl"'
echo "lock screenshot: $O/lock-$T.png ; chrome: see the chrome-launch.sh line above"
