# run-plasma.sh - (Android root shell) restart the anland app and the container's Plasma session
# (desktop-session.service -> mobilegl-startup.sh plasma).  Takes ~90 s (two 40 s waits: logind must
# finish tearing the old user session down before the new one starts).  Prints the unit status, the
# plasma user units and recent coredumps.  Run: su -c "sh /data/local/tmp/anl/run-plasma.sh"
# Env: PKG, CONTAINER, DESKTOP_USER (swung0x48), SOCK, DS (droidspaces binary).
D=${DS:-/data/local/Droidspaces/bin/droidspaces}
P=${PKG:-com.anland.consumer.mobilegl}
C=${CONTAINER:-arch-kde-mgl}
U=${DESKTOP_USER:-swung0x48}
SOCK=${SOCK:-/data/local/tmp/anland-mobilegl/display.sock}
am force-stop $P
am start -n $P/com.anland.consumer.MainActivity --es socket_path $SOCK >/dev/null
sleep 3
$D --name=$C run bash -lc "
systemctl stop desktop-session.service 2>/dev/null
loginctl terminate-user $U 2>/dev/null; sleep 40
systemctl reset-failed desktop-session.service
rm -f /tmp/mobilegl-compositor.log
date +%s > /tmp/plasma-start
systemctl start desktop-session.service
sleep 40
systemctl --no-pager status desktop-session.service | head -8
UID_=\$(id -u $U)
sudo -u $U XDG_RUNTIME_DIR=/run/user/\$UID_ systemctl --user --no-pager list-units --all 'plasma*' 2>&1 | head -30
coredumpctl --no-pager list --since '-2min' 2>&1 | tail -5
"
