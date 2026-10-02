# unlock.sh - (Android root shell) ask logind to unlock the desktop user's session.  UNVERIFIED that this
# helps here: the handoff notes this logind session type does not support lock-session (a real lock
# screen then needs the password).  Prevention: [Daemon] Autolock=false in ~/.config/kscreenlockerrc.
# Env: DS, CONTAINER, DESKTOP_USER.
D=${DS:-/data/local/Droidspaces/bin/droidspaces}
C=${CONTAINER:-arch-kde-mgl}
U=${DESKTOP_USER:-swung0x48}
$D --name=$C run bash -lc "S=\$(loginctl list-sessions --no-legend | awk '\$3==\"$U\"{print \$1}' | head -1); loginctl unlock-session \$S; echo unlocked \$S"
