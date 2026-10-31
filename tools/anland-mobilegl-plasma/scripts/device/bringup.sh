# MobileGL - tools/anland-mobilegl-plasma/scripts/device/bringup.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# bringup.sh - (Android root shell) bring the MobileGL desktop up after a phone reboot. There is
# nothing left to do by hand: opening the anland app (its launcher icon, or this am start) starts
# the display daemon, the MobileGL server, the container and its Plasma session. Prints the timeline.
# Run: su -c "sh /data/local/tmp/anl/bringup.sh"      Env: as desktop-timeline.sh.
input keyevent KEYCODE_WAKEUP
wm dismiss-keyguard
sh "$(dirname "$0")/desktop-timeline.sh" 300
