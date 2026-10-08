#!/usr/bin/env bash
# fcl_count.sh <name> <hostlib> <backend> <transport> <outdir>
# One FCL launch with a P15Count build (p15count.patch): waits for steady in-world frames, runs
# 25 s with debug.mobilegl.p15count=1, and extracts the P15CNT lines to <outdir>/count.txt.
# Caller restores FCL (fcl_p14.sh restore).
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
name="$1" lib="$2" backend="$3" transport="$4" out="$5" sp="${6:-1}"
SER=HA27Q3LQ FCL=com.tungsten.fcl.mgdebug.debug
CFG=/data/data/$FCL/files/config.json
mkdir -p "$out"
su_sh() { adb -s $SER shell "su -c '$1'"; }
dir=$(su_sh "dirname /data/app/*/$FCL-*/lib/arm64/libMobileGL.so" | tr -d '\r' | head -1)
id=5e273ee2-baca-4c81-8e48-b63feefb9ba8; [ "$backend" = DirectVulkan ] && id=2be0dc10-1eef-4ce2-b512-b266dd33fd9e
adb -s $SER push "$(cygpath -w "$lib")" /data/local/tmp/p15-fcl-lib.so >/dev/null
su_sh "am force-stop $FCL; cat /data/local/tmp/p15-fcl-lib.so > $dir/libMobileGL.so; rm -f /data/local/tmp/p15-fcl-lib.so; md5sum $dir/libMobileGL.so" > "$out/libmd5.txt"
su_sh "sh /data/local/tmp/p14-fcl-set.sh $id $CFG; setprop debug.mobilegl.fps_log 1; setprop debug.mobilegl.transport $transport; setprop debug.mobilegl.p15count 1"
ok=0
for attempt in 1 2 3 4; do
  su_sh "am force-stop $FCL"; sleep 2
  adb -s $SER logcat -c
  su_sh "input keyevent KEYCODE_WAKEUP; monkey -p $FCL -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
  t=0
  while [ $t -lt 200 ]; do
    sleep 3; t=$((t+3))
    if [ $t -gt 12 ] && [ -z "$(adb -s $SER shell pidof $FCL | tr -dc '0-9')" ]; then echo "attempt $attempt: died at $t s" >> "$out/wait.txt"; break; fi
    adb -s $SER logcat -d > "$out/logcat_wait.txt" 2>/dev/null
    n=$(awk '/Time elapsed:/{f=1} f && /MobileGL fps:/{ if (match($0,/over [0-9]+ frames/)) { s=substr($0,RSTART+5,RLENGTH-12); if (s+0>150) c++ } } END{print c+0}' "$out/logcat_wait.txt")
    if [ "$n" -ge 4 ]; then ok=1; break; fi
  done
  [ $ok = 1 ] && break
done
echo "waited=$t ok=$ok attempt=$attempt" >> "$out/wait.txt"
[ $ok = 1 ] || { su_sh "am force-stop $FCL"; echo "NO STEADY FRAMES"; exit 3; }
sleep 25
adb -s $SER logcat -d > "$out/logcat.txt"
su_sh "am force-stop $FCL; resetprop --delete debug.mobilegl.p15count"
grep -a "P15CNT" "$out/logcat.txt" | sed 's/.*P15CNT/P15CNT/' > "$out/count.txt"
grep -a "MobileGL fps:" "$out/logcat.txt" | tail -3 | sed 's/.*MobileGL fps: //'
tail -12 "$out/count.txt"
