# MobileGL - tools/anland-mobilegl-plasma/scripts/device/desktop-reset.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# desktop-reset.sh - (Android root shell) take the MobileGL desktop fully down, as after a phone
# reboot: the app force-stopped (its foreground service and server with it), the container
# stopped, the experiment display daemon killed. Leaves the original anland daemon alone.
# Run: su -c "sh /data/local/tmp/anl/desktop-reset.sh"      Env: PKG, CONTAINER, SOCK, DS.
P=${PKG:-com.anland.consumer.mobilegl}
C=${CONTAINER:-arch-kde-mgl}
SOCK=${SOCK:-/data/local/tmp/anland-mobilegl/display.sock}
D=${DS:-/data/local/Droidspaces/bin/droidspaces}
am force-stop $P
$D --name=$C stop > /dev/null 2>&1
for p in $(pidof display_daemon); do
    tr '\0' ' ' < /proc/$p/cmdline | grep -qF " $SOCK " && kill $p
done
rm -f $SOCK
sleep 2
echo "app: $(pidof $P) server: $(pidof $P:mobilegl) daemon: $(ls $SOCK 2>/dev/null)"
$D show
