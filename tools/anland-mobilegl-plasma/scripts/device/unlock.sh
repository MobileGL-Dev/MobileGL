# MobileGL - tools/anland-mobilegl-plasma/scripts/device/unlock.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# unlock.sh - (Android root shell) ask logind to unlock the desktop user's session.  UNVERIFIED that this
# helps here: the handoff notes this logind session type does not support lock-session (a real lock
# screen then needs the password).  Prevention: [Daemon] Autolock=false in ~/.config/kscreenlockerrc.
# Env: DS, CONTAINER, DESKTOP_USER.
D=${DS:-/data/local/Droidspaces/bin/droidspaces}
C=${CONTAINER:-arch-kde-mgl}
U=${DESKTOP_USER:-swung0x48}
$D --name=$C run bash -lc "S=\$(loginctl list-sessions --no-legend | awk '\$3==\"$U\"{print \$1}' | head -1); loginctl unlock-session \$S; echo unlocked \$S"
