# run-plasma.sh - (Android root shell) DEVELOPER restart of the whole MobileGL desktop, e.g. after a
# server, client or KWin rebuild: ends the Plasma session (and the user's logind session, which takes
# a while to wind down), then restarts the anland app; opening it brings up the server, starts
# desktop-session.service again and the session waits for the server. Normal use needs none of this:
# opening the app is enough. ~60 s. Prints the unit status, plasma user units and recent coredumps.
# Run: su -c "sh /data/local/tmp/anl/run-plasma.sh"
# Env: PKG, CONTAINER, DESKTOP_USER (swung0x48), DS (droidspaces binary).
D=${DS:-/data/local/Droidspaces/bin/droidspaces}
P=${PKG:-com.anland.consumer.mobilegl}
C=${CONTAINER:-arch-kde-mgl}
U=${DESKTOP_USER:-swung0x48}
$D --name=$C run bash -lc "
systemctl stop desktop-session.service 2>/dev/null
loginctl terminate-user $U 2>/dev/null; sleep 30
systemctl reset-failed desktop-session.service
rm -f /tmp/mobilegl-compositor.log
date +%s > /tmp/plasma-start
"
am force-stop $P
am start -n $P/com.anland.consumer.MainActivity > /dev/null
sleep 30
$D --name=$C run bash -lc "
systemctl --no-pager status desktop-session.service | head -8
UID_=\$(id -u $U)
sudo -u $U XDG_RUNTIME_DIR=/run/user/\$UID_ systemctl --user --no-pager list-units --all 'plasma*' 2>&1 | head -30
coredumpctl --no-pager list --since '-2min' 2>&1 | tail -5
"
