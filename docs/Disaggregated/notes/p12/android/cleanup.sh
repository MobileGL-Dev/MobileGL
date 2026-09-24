#!/usr/bin/env bash
# Leaves the phone clean: both servers stopped (am force-stop), the new APK kept, the adb forward
# this stage added removed, the Doze whitelist and stay-on setting left as found (never touched).
#   STAMP=p12w1-<sha8> bash cleanup.sh
. "$(dirname "$0")/dev.sh"
OUT=$DEV/99-cleanup
mkdir -p "$OUT"
{
force_stop
A forward --remove tcp:40613 2>/dev/null || true
Ash input keyevent KEYCODE_HOME
sleep 1
echo "processes : $(ps_mobilegl | tr '\n' ';')"
echo "forwards  : $(A forward --list | tr '\n' ';')"
echo "doze wl   : $(Ash 'dumpsys deviceidle whitelist' | grep -i mobilegl | tr '\n' ' ')"
echo "stay_on   : $(Ash 'settings get global stay_on_while_plugged_in')"
echo "focus     : $(Ash 'dumpsys window' | grep -m1 mCurrentFocus | tr -s ' ')"
echo "installed : $(Ash "dumpsys package $PKG" | grep -E 'versionName|lastUpdateTime' | tr -s ' ' | tr '\n' ' ')"
} 2>&1 | tee "$OUT/final-state.txt"
