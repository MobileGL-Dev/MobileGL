# bringup.sh [DirectGLES|DirectVulkan] - (Android root shell) bring the stack up after a phone reboot:
# stay-awake, container, backend property, experiment display daemon, anland app, then the Plasma
# session (run-plasma.sh, ~90 s; skip with NO_PLASMA=1).  The backend defaults to the container's
# /etc/mobilegl/backend (persists across reboots; the property does not).
# Assembled from the documented manual steps; not exercised as one unit yet.  Re-running restarts the
# daemon, the app and the session.  Run: su -c "sh /data/local/tmp/anl/bringup.sh"
# Env: PKG, CONTAINER, DS, SOCK, TOOLS (dir holding run-plasma.sh / restart-daemon.sh; default: this dir).
D=${DS:-/data/local/Droidspaces/bin/droidspaces}
P=${PKG:-com.anland.consumer.mobilegl}
C=${CONTAINER:-arch-kde-mgl}
SOCK=${SOCK:-/data/local/tmp/anland-mobilegl/display.sock}
T=${TOOLS:-$(dirname $0)}
svc power stayon true
if $D show 2>/dev/null | grep -q " $C "; then
    echo "container $C already running"
else
    $D -C /data/local/Droidspaces/Containers/$C/container.config start
    sleep 5
fi
B=${1:-$(cat /mnt/Droidspaces/$C/etc/mobilegl/backend 2>/dev/null)}
B=${B:-DirectGLES}
setprop debug.mobilegl.backend $B
echo "$B" > /mnt/Droidspaces/$C/etc/mobilegl/backend
echo "backend: $(getprop debug.mobilegl.backend)"
sh $T/restart-daemon.sh
am start -n $P/com.anland.consumer.MainActivity --es socket_path $SOCK >/dev/null
sleep 3
if pidof $P:mobilegl >/dev/null; then echo "server process $P:mobilegl up"; else echo "WARNING: $P:mobilegl not running"; fi
[ "${NO_PLASMA:-0}" = 1 ] || sh $T/run-plasma.sh
