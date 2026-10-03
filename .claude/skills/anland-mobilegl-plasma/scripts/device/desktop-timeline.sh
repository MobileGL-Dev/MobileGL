# desktop-timeline.sh [timeout-s] - (Android root shell) open the anland app the way its launcher
# icon does (am start, no extras) and print when each piece of the MobileGL desktop comes up,
# in seconds after the open: display daemon socket, the app's MobileGL server listening, the
# container, kwin_wayland, plasmashell, and the splash (ksplashqml) gone = desktop on screen.
# Pieces that already run show 0.0. Read-only apart from the am start.
# Run: su -c "sh /data/local/tmp/anl/desktop-timeline.sh 180"      Env: PKG, CONTAINER, SOCK, DS.
T=${1:-180}
P=${PKG:-com.anland.consumer.mobilegl}
C=${CONTAINER:-arch-kde-mgl}
SOCK=${SOCK:-/data/local/tmp/anland-mobilegl/display.sock}
D=${DS:-/data/local/Droidspaces/bin/droidspaces}
now() { awk '{print $1}' /proc/uptime; }
since() { awk -v a="$1" -v b="$(now)" 'BEGIN{printf "%.1f", b - a}'; }
listening() { grep -q '00010000 0001 01 [0-9]* @anland-mobilegl$' /proc/net/unix; }
t0=$(now)
am start -n $P/com.anland.consumer.MainActivity > /dev/null
for k in daemon server container kwin shell splash_seen shown; do eval "t_$k="; done
while :; do
    [ -z "$t_daemon" ] && [ -S "$SOCK" ] && t_daemon=$(since $t0) && echo "daemon socket     +${t_daemon}s"
    [ -z "$t_server" ] && listening && t_server=$(since $t0) && echo "server listening  +${t_server}s"
    [ -z "$t_container" ] && [ -n "$($D --name=$C pid 2>/dev/null | tr -dc 0-9)" ] && t_container=$(since $t0) && echo "container up      +${t_container}s"
    [ -z "$t_kwin" ] && pidof kwin_wayland > /dev/null && t_kwin=$(since $t0) && echo "kwin_wayland      +${t_kwin}s"
    [ -z "$t_shell" ] && pidof plasmashell > /dev/null && t_shell=$(since $t0) && echo "plasmashell       +${t_shell}s"
    [ -z "$t_splash_seen" ] && pidof ksplashqml > /dev/null && t_splash_seen=1
    if [ -n "$t_shell" ] && [ -z "$t_shown" ] && ! pidof ksplashqml > /dev/null; then
        t_shown=$(since $t0); echo "splash gone       +${t_shown}s (desktop on screen)"; break
    fi
    [ "$(since $t0 | cut -d. -f1)" -ge "$T" ] && { echo "TIMEOUT after ${T}s"; break; }
    sleep 0.3
done
