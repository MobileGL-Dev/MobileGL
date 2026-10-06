#!/bin/bash
# soak2.sh <runs-magma> <runs-espryt> <secs-in-world>: FCL launch soak (ONE instance at a time - it takes a
# lock) with diagnostics, plus a device-side sampler of the FCL process's caught-signal mask (SigCgt), so a
# death by signal 34 can be tied to whether bionic/libcore's SIGRTMIN+2 handler is still installed.
export MSYS_NO_PATHCONV=1
NM=${1:-30}; NE=${2:-10}; SECS=${3:-150}
BASE=${SOAK_DIR:?set SOAK_DIR}
LOCK=/c/Users/yello/AppData/Local/Temp/claude/C--Users-yello-AndroidStudioProjects-FoldCraftLauncher-MobileGL/0e0e570f-5721-41d8-9d0f-80fcde638a84/scratchpad/soak.lock
if ! mkdir "$LOCK" 2>/dev/null; then echo "another soak holds $LOCK"; exit 1; fi
trap 'rmdir "$LOCK"' EXIT
mkdir -p "$BASE"
PKG=com.tungsten.fcl.mgdebug.debug
CFG=/data/data/$PKG/files/config.json
SUM=$BASE/summary.txt
echo "# run backend result mgl arm inworld deathTime exit sig34CaughtAtLaunch sig34CaughtInGame firstUncaught firstMGL ftraceFatal34 tomb+ kgsl" >> "$SUM"
adb logcat -G 16M >/dev/null 2>&1
# The sampler: every 0.5 s, "<time> <pid> <SigCgt>" for the package's main process.
adb shell "su -c 'cat > /data/local/tmp/p13/sigsample.sh'" <<'EOS'
end=$(( $(date +%s) + $1 ))
while [ $(date +%s) -lt $end ]; do
  p=$(pidof com.tungsten.fcl.mgdebug.debug | cut -d" " -f1)
  if [ -n "$p" ]; then echo "$(date +%H:%M:%S.%N | cut -c1-12) $p $(grep SigCgt /proc/$p/status | cut -f2)"; fi
  sleep 0.5
done
EOS
plan=""
for i in $(seq 1 $NM); do plan="$plan magma"; done
for i in $(seq 1 $NE); do plan="$plan espryt"; done
order=$(echo $plan | tr ' ' '\n' | awk 'BEGIN{srand(29)} {print rand()"\t"$0}' | sort | cut -f2)
idx=0
for be in $order; do
  idx=$((idx+1))
  lvl=$(adb shell dumpsys battery | grep level | tr -dc 0-9)
  if [ "${lvl:-100}" -lt 40 ]; then echo "battery $lvl - stopping" >> "$SUM"; break; fi
  OUT=$BASE/run$(printf %03d $idx)-$be; mkdir -p "$OUT"
  case $be in espryt) C=cfg-espryt.json;; *) C=cfg-magma.json;; esac
  adb shell "su -c 'am force-stop $PKG; cat /data/local/tmp/p13/$C > $CFG'"
  adb shell "su -c 'ls /data/tombstones | wc -l'" > "$OUT/tomb-before.txt"
  adb shell "su -c 'dmesg -c >/dev/null 2>&1; echo > /sys/kernel/tracing/trace'"
  adb logcat -b all -c
  adb shell "svc power stayon true; wm dismiss-keyguard" >/dev/null 2>&1
  adb shell "su -c 'sh /data/local/tmp/p13/sigsample.sh $((SECS + 40))'" > "$OUT/sigcgt.txt" 2>&1 &
  SAMP=$!
  sleep 1
  adb shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
  sleep $((SECS + 45))
  wait $SAMP 2>/dev/null
  adb exec-out screencap -p > "$OUT/screen.png"
  adb logcat -b all -d > "$OUT/logcat.txt"
  adb shell "su -c 'dmesg'" > "$OUT/dmesg.txt" 2>/dev/null
  adb shell "su -c 'cat /sys/kernel/tracing/trace'" > "$OUT/ftrace.txt" 2>/dev/null
  adb shell "dumpsys activity exit-info $PKG" > "$OUT/exitinfo.txt" 2>/dev/null
  adb shell "su -c 'ls /data/tombstones | wc -l'" > "$OUT/tomb-after.txt"
  adb shell "su -c 'am force-stop $PKG'"
  died=$(grep -ac "Process $PKG (pid [0-9]*) has died" "$OUT/logcat.txt")
  mgl=$(grep -ac " MobileGL" "$OUT/logcat.txt")
  arm=$(grep -ao "monolith data arm = [a-z]*" "$OUT/logcat.txt" | head -1 | awk '{print $NF}')
  inworld=$(grep -ac "Server thread\|Preparing spawn area\|Loaded [0-9]* advancements\|joined the game" "$OUT/logcat.txt")
  firstmgl=$(grep -a " MobileGL" "$OUT/logcat.txt" | head -1 | cut -c7-18)
  deatht=$(grep -a "Process $PKG (pid [0-9]*) has died" "$OUT/logcat.txt" | head -1 | cut -c7-18)
  ex=$(grep -aE "reason=" "$OUT/exitinfo.txt" | head -1 | grep -oE "reason=[0-9]+ \([A-Z_ ]+\)|status=[0-9]+" | tr '\n' ' ')
  # SigCgt bit 33 = signal 34 (SIGRTMIN+2, libcore's async-close wakeup).
  bit() { [ -z "$1" ] && { echo "?"; return; }; [ $(( (0x$1 >> 33) & 1 )) -eq 1 ] && echo yes || echo no; }
  launchMask=$(grep -aE "^[0-9:.]+ [0-9]+ [0-9a-f]{16}" "$OUT/sigcgt.txt" | head -1 | awk '{print $3}')
  gameMask=$(grep -aE "^[0-9:.]+ [0-9]+ [0-9a-f]{16}" "$OUT/sigcgt.txt" | sed -n 60p | awk '{print $3}')
  firstUn=$(grep -aE "^[0-9:.]+ [0-9]+ [0-9a-f]{16}" "$OUT/sigcgt.txt" | while read t p m; do [ "$(bit $m)" = no ] && { echo $t; break; }; done)
  # A sig 34 generated toward the package with no matching delivery = the kernel's fatal fast path.
  gen=$(grep -ac "signal_generate: sig=34" "$OUT/ftrace.txt"); del=$(grep -ac "signal_deliver: sig=34" "$OUT/ftrace.txt")
  tb=$(( $(tr -dc 0-9 < "$OUT/tomb-after.txt") - $(tr -dc 0-9 < "$OUT/tomb-before.txt") ))
  kg=$(grep -aciE "kgsl.*(fault|hang|recover|snapshot)|adreno.*fault" "$OUT/dmesg.txt")
  res=ok; [ "$died" -gt 0 ] && res=DIED
  echo "$idx $be $res mgl=$mgl arm=${arm:-?} inworld=$inworld death=${deatht:--} exit=[$ex] launch34=$(bit "$launchMask") game34=$(bit "$gameMask") firstUncaught=${firstUn:--} firstMGL=${firstmgl:--} sig34gen/deliv=$gen/$del tomb+=$tb kgsl=$kg" >> "$SUM"
  if [ "$res" = ok ] && [ $((idx % 5)) -ne 0 ]; then rm -f "$OUT/logcat.txt" "$OUT/dmesg.txt" "$OUT/ftrace.txt"; fi
done
adb shell "svc power stayon false"
echo "soak done" >> "$SUM"
