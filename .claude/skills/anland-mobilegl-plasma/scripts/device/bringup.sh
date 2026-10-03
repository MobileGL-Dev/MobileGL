# bringup.sh - (Android root shell) bring the MobileGL desktop up after a phone reboot. There is
# nothing left to do by hand: opening the anland app (its launcher icon, or this am start) starts
# the display daemon, the MobileGL server, the container and its Plasma session. Prints the timeline.
# Run: su -c "sh /data/local/tmp/anl/bringup.sh"      Env: as desktop-timeline.sh.
input keyevent KEYCODE_WAKEUP
wm dismiss-keyguard
sh "$(dirname "$0")/desktop-timeline.sh" 300
