#!/usr/bin/env bash
# devstate.sh <outdir>: the one-line device state every reported number carries (P15 measurement rules).
# Needs <outdir>/freq.txt + pin.txt (freq_sampler / pin_clocks show). Writes <outdir>/profile.txt,
# <outdir>/valid.txt and <outdir>/state.txt, and prints the state line:
#   STATE profile=<name>(cpu/little/gpu MHz) <VALID|INVALID ... freqcheck summary> daemons=<..> swap=<..> anland=<on|off>
set -u
export MSYS_NO_PATHCONV=1
out="$1"; kind="${2:-fcl}"; SER="${SER:-HA27Q3LQ}"; HERE="$(cd "$(dirname "$0")" && pwd)"
FCL=com.tungsten.fcl.mgdebug.debug
su_sh() { adb -s "$SER" shell "su -c '$1'"; }
su_sh "cat /data/local/tmp/p15-bench-profile 2>/dev/null || echo profile=unpinned" | tr -d '\r' > "$out/profile.txt"
python "$(cygpath -w "$HERE/freqcheck.py" 2>/dev/null || echo "$HERE/freqcheck.py")" "$(cygpath -w "$out/freq.txt")" "$(cygpath -w "$out/pin.txt")" > "$out/valid.txt" 2>&1
daemons=$(su_sh "test -f /data/local/tmp/p15-bench-session && echo stopped\(\$(grep -c \"^svc \" /data/local/tmp/p15-bench-session)\) || echo running" | tr -d '\r')
ver=$(adb -s "$SER" exec-out "su -c 'cat /data/data/$FCL/files/config.json'" 2>/dev/null | grep -o '"selectedMinecraftVersion": *"[^"][^"]*"' | head -1 | sed 's/.*: *"//; s/"$//')
swap=$(su_sh "grep -h enableVsync /sdcard/FCL/.minecraft/versions/$ver/options.txt 2>/dev/null" | tr -d '\r' | sed 's/enableVsync://')
anland=$(adb -s "$SER" shell pidof com.anland.consumer.mobilegl | tr -dc '0-9' | head -c 8)
p=$(cat "$out/profile.txt")
pn=$(echo "$p" | sed -n 's/.*profile=\([^ ]*\).*/\1/p'); pc=$(echo "$p" | sed -n 's/.*cpu=\([0-9]*\).*/\1/p'); pl=$(echo "$p" | sed -n 's/.*little=\([0-9]*\).*/\1/p'); pg=$(echo "$p" | sed -n 's/.*gpu=\([0-9]*\).*/\1/p')
[ "$kind" = trace ] && { ver=-; swap=pbuffer; }
line="STATE profile=${pn:-unpinned}(${pc:+$((pc/1000))/$((pl/1000))/$((pg/1000000))MHz}) $(head -1 "$out/valid.txt") daemons=$daemons mc=$ver swap=${swap:-?} anland=$([ -n "$anland" ] && echo on || echo off) $(cat "$out/env.txt" 2>/dev/null)"
echo "$line" > "$out/state.txt"; echo "$line"
