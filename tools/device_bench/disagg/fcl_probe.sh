#!/usr/bin/env bash
# fcl_probe.sh <name> <hostlib> <backend> <transport> <outdir> [simpleperf=1]
# One FCL launch: wait for steady in-world frames (fps log), then
#  A) 4 s ftrace (sched + kgsl cmdbatch/waittimestamp + atrace gfx for the app), schedstat and gpu_clock_stats bracketing it
#  B) 6 s simpleperf --trace-offcpu on the game pid (optional)
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
su_sh "sh /data/local/tmp/p14-fcl-set.sh $id $CFG; setprop debug.mobilegl.fps_log 1; setprop debug.mobilegl.transport $transport"
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
pid=$(adb -s $SER shell pidof $FCL | tr -d '\r' | awk '{print $NF}')
echo "pid=$pid" >> "$out/wait.txt"
snap() { adb -s $SER shell "su -c 'cat /proc/uptime; cat /sys/class/kgsl/kgsl-3d0/gpu_clock_stats; for t in /proc/$pid/task/*; do echo T \${t##*/} \$(cut -d\" \" -f1-3 \$t/schedstat) \$(cat \$t/comm); done'" > "$1"; }
# P15 validity rule: frequency/thermal sampler for the whole measurement window (devstate.sh judges it)
BENCH="$(cd "$(dirname "$0")" && pwd)"
SER=$SER bash $BENCH/pin_clocks.sh show > "$out/pin.txt" 2>&1
adb -s $SER push "$(cygpath -w $BENCH/freq_sampler.sh)" /data/local/tmp/p15-freq_sampler.sh >/dev/null
su_sh "chmod 755 /data/local/tmp/p15-freq_sampler.sh; rm -f /data/local/tmp/p15freq.txt; (nohup sh /data/local/tmp/p15-freq_sampler.sh /data/local/tmp/p15freq.txt 2 >/dev/null 2>&1 &)"
# A: ftrace
EV=/sys/kernel/tracing/events
su_sh "atrace --async_start -c -b 65536 -a $FCL gfx view sched freq >/dev/null 2>&1; for e in adreno_cmdbatch_submitted adreno_cmdbatch_retired kgsl_waittimestamp_entry kgsl_waittimestamp_exit adreno_syncobj_submitted adreno_syncobj_retired adreno_drawctxt_wait_start adreno_drawctxt_wait_done kgsl_timeline_wait; do echo 1 > $EV/kgsl/\$e/enable; done; echo 1 > $EV/sched/sched_waking/enable"
snap "$out/snapA0.txt"
sleep 4
snap "$out/snapA1.txt"
su_sh "atrace --async_stop -o /data/local/tmp/p15.atrace >/dev/null 2>&1; for e in adreno_cmdbatch_submitted adreno_cmdbatch_retired kgsl_waittimestamp_entry kgsl_waittimestamp_exit adreno_syncobj_submitted adreno_syncobj_retired adreno_drawctxt_wait_start adreno_drawctxt_wait_done kgsl_timeline_wait; do echo 0 > $EV/kgsl/\$e/enable; done; chmod 644 /data/local/tmp/p15.atrace"
adb -s $SER pull /data/local/tmp/p15.atrace "$(cygpath -w "$out/trace.txt")" >/dev/null
# B: simpleperf
if [ "$sp" = 1 ]; then
  snap "$out/snapB0.txt"
  su_sh "rm -f /data/local/tmp/p15perf.data; simpleperf record -p $pid -e cpu-clock --trace-offcpu --call-graph fp -f 4000 --duration 6 -o /data/local/tmp/p15perf.data" > "$out/record.log" 2>&1
  snap "$out/snapB1.txt"
  su_sh "chmod 644 /data/local/tmp/p15perf.data"
  adb -s $SER pull /data/local/tmp/p15perf.data "$(cygpath -w "$out/perf.data")" >/dev/null
fi
su_sh "pkill -f p15-freq_sampler.sh; chmod 644 /data/local/tmp/p15freq.txt"
adb -s $SER pull /data/local/tmp/p15freq.txt "$(cygpath -w "$out/freq.txt")" >/dev/null
SER=$SER bash $BENCH/devstate.sh "$out"
adb -s $SER logcat -d > "$out/logcat.txt"
su_sh "am force-stop $FCL"
grep -a "MobileGL fps:" "$out/logcat.txt" | tail -8 | sed 's/.*MobileGL fps: //' > "$out/fps.txt"
echo "$name $backend $transport :: $(tail -4 "$out/fps.txt" | tr '\n' '|')"
tail -1 "$out/record.log" 2>/dev/null
