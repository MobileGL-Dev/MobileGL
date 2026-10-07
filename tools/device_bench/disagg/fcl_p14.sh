#!/usr/bin/env bash
# FCL Minecraft in-world fps for P14: swaps FCL's built-in libMobileGL.so (rooted device), picks
# the backend through FCL's own renderer id and the transport through debug.mobilegl.transport,
# launches FCL (it auto-enters the last world), and reads MobileGL's opt-in fps log
# (debug.mobilegl.fps_log=1, one line about every 2 s) over a steady window.
#
#   SER=HA27Q3LQ LIBS="dev=<path> feat=<path>" CONFIGS="dev:DirectGLES feat:DirectGLES:inproc ..." \
#   REPS=2 WARMUP=45 WINDOW=30 bash fcl_p14.sh run|restore
#
# The original library is saved once to /data/local/tmp/p14-fcl-orig.so and put back by
# `restore` (md5-checked), along with config.json and the two properties. Clocks are the
# caller's (pin_clocks.sh). FCL can die by itself at start-up (DEBTS.md, SIGRTMIN+2): a run with
# no fps lines is retried once.
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SER="${SER:-HA27Q3LQ}"
FCL="${FCL:-com.tungsten.fcl.mgdebug.debug}"
REPS="${REPS:-2}" WARMUP="${WARMUP:-45}" WINDOW="${WINDOW:-30}"
OUT="${PERF_WORK:-$REPO_ROOT/.trace-work/perf-p14}/${TAG:-fcl}"
ESPRYT_ID=5e273ee2-baca-4c81-8e48-b63feefb9ba8
MAGMA_ID=2be0dc10-1eef-4ce2-b512-b266dd33fd9e
mkdir -p "$OUT"
su_sh() { adb -s "$SER" shell "su -c '$1'"; }
libdir() { su_sh "dirname /data/app/*/$FCL-*/lib/arm64/libMobileGL.so" | tr -d '\r' | head -1; }
CFG=/data/data/$FCL/files/config.json

save_original() {
  local dir; dir=$(libdir)
  # The renderer switch, as a device-side script: sed's own quotes cannot survive two shells.
  printf '%s\n' 'sed -i "s/\"renderer\": \"[^\"]*\"/\"renderer\": \"$1\"/" "$2"' >"$OUT/p14-fcl-set.sh"
  adb -s "$SER" push "$(cygpath -w "$OUT/p14-fcl-set.sh")" /data/local/tmp/p14-fcl-set.sh >/dev/null
  su_sh "test -f /data/local/tmp/p14-fcl-orig.so || cp $dir/libMobileGL.so /data/local/tmp/p14-fcl-orig.so; test -f /data/local/tmp/p14-fcl-config.json || cp $CFG /data/local/tmp/p14-fcl-config.json"
}

install_lib() { # $1 host path
  local dir; dir=$(libdir)
  adb -s "$SER" push "$(cygpath -w "$1")" /data/local/tmp/p14-fcl-lib.so >/dev/null
  # cat into the existing file keeps its owner, mode and SELinux label.
  su_sh "cat /data/local/tmp/p14-fcl-lib.so > $dir/libMobileGL.so; rm -f /data/local/tmp/p14-fcl-lib.so"
}

run_one() { # name backend transport rep
  local name="$1" backend="$2" transport="$3" rep="$4" dir="$OUT/$1-$2-$3-r$4"
  mkdir -p "$dir"
  local id=$ESPRYT_ID; [ "$backend" = DirectVulkan ] && id=$MAGMA_ID
  su_sh "am force-stop $FCL; sh /data/local/tmp/p14-fcl-set.sh $id $CFG; setprop debug.mobilegl.fps_log 1; setprop debug.mobilegl.transport $transport"
  adb -s "$SER" logcat -c
  local t0; t0=$(adb -s "$SER" shell dumpsys battery | tr -d '\r' | sed -n 's/.*temperature: \([0-9]*\).*/\1/p')
  su_sh "input keyevent KEYCODE_WAKEUP; monkey -p $FCL -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
  sleep "$WARMUP"
  local pid; pid=$(adb -s "$SER" shell pidof $FCL | tr -d '\r' | awk '{print $NF}')
  adb -s "$SER" shell "su -c 'for t in /proc/$pid/task/*; do echo \$(cat \$t/comm) \$(cut -d\" \" -f1 \$t/schedstat); done'" >"$dir/cpu0.txt" 2>/dev/null
  sleep "$WINDOW"
  adb -s "$SER" shell "su -c 'for t in /proc/$pid/task/*; do echo \$(cat \$t/comm) \$(cut -d\" \" -f1 \$t/schedstat); done'" >"$dir/cpu1.txt" 2>/dev/null
  adb -s "$SER" logcat -d >"$dir/logcat.txt"
  su_sh "am force-stop $FCL"
  local t1; t1=$(adb -s "$SER" shell dumpsys battery | tr -d '\r' | sed -n 's/.*temperature: \([0-9]*\).*/\1/p')
  local lines; lines=$(grep -a "MobileGL fps:" "$dir/logcat.txt" | wc -l)
  echo "name=$name backend=$backend transport=$transport rep=$rep pid=$pid temp0=$t0 temp1=$t1 fpslines=$lines" >"$dir/meta.txt"
  grep -aE "Active backend type|debug.mobilegl.transport|MOBILEGL_TRANSPORT=inproc|run-ahead ARMED" "$dir/logcat.txt" | head -5 >"$dir/markers.txt"
  echo "$(cat "$dir/meta.txt") :: $(grep -a 'MobileGL fps:' "$dir/logcat.txt" | tail -3 | sed 's/.*MobileGL fps: //' | tr '\n' '|')"
  [ "$lines" -gt 0 ]
}

case "${1:-run}" in
  run)
    save_original
    for ((r = 1; r <= REPS; r++)); do
      for cfg in $CONFIGS; do
        IFS=: read -r name backend transport <<<"$cfg"
        transport="${transport:-monolith}"
        lib=$(echo " $LIBS " | sed -n "s/.* $name=\([^ ]*\) .*/\1/p")
        install_lib "$lib"
        run_one "$name" "$backend" "$transport" "$r" || { sleep 10; run_one "$name" "$backend" "$transport" "$r" || true; }
        sleep "${COOL:-20}"
      done
    done ;;
  restore)
    dir=$(libdir)
    su_sh "am force-stop $FCL; cat /data/local/tmp/p14-fcl-orig.so > $dir/libMobileGL.so && md5sum $dir/libMobileGL.so; cat /data/local/tmp/p14-fcl-config.json > $CFG; resetprop --delete debug.mobilegl.fps_log; resetprop --delete debug.mobilegl.transport" ;;
esac
