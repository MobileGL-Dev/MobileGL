# status.sh - (Android root shell) READ-ONLY health check of the whole stack.
# Run: su -c "sh /data/local/tmp/anl/status.sh"      Env: PKG, CONTAINER, DS, SOCK.
D=${DS:-/data/local/Droidspaces/bin/droidspaces}
P=${PKG:-com.anland.consumer.mobilegl}
C=${CONTAINER:-arch-kde-mgl}
SOCK=${SOCK:-/data/local/tmp/anland-mobilegl/display.sock}
echo "battery: $(dumpsys battery | grep ' level:' | tr -d ' ')  $(dumpsys power | grep -o 'mStayOn=[a-z]*')"
echo "backend: app setting=$(cat /data/data/$P/files/mobilegl-backend 2>/dev/null || echo 'DirectGLES (default)')  published=$(cat ${SOCK%/*}/backend 2>/dev/null)  container /etc/mobilegl/backend=$(cat /mnt/Droidspaces/$C/etc/mobilegl/backend 2>/dev/null)  prop override=$(getprop debug.mobilegl.backend)"
echo "desktop service log: $(tail -1 ${SOCK%/*}/desktop.log 2>/dev/null)"
echo "app pid: $(pidof $P)  server (:mobilegl) pid: $(pidof $P:mobilegl)"
ps -A -o PID,ARGS | grep "display_daemon $SOCK" | grep -v grep || echo "experiment display daemon NOT running"
$D show
$D --name=$C run bash -lc 'echo "desktop-session: $(systemctl is-active desktop-session.service)"; pgrep -a -x kwin_wayland; pgrep -a -x plasmashell; grep -A1 QtQuickRendererSettings /home/*/.config/kdeglobals 2>/dev/null || echo "kdeglobals: no sticky software renderer"'
