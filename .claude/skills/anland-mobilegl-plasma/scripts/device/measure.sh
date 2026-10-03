# measure.sh <label> [seconds] - (Android root shell) CPU and GPU load of the MobileGL desktop over a
# window: % of ONE core (so 800 = all 8 cores) for the whole phone, KWin (+Xwayland), plasmashell,
# Chrome (all its processes), the app's :mobilegl server and its UI process, plus the mean of the
# kgsl gpu_busy_percentage samples (1/s). The container's processes are visible from Android's
# /proc (Droidspaces only gives them their own pid namespace).
# Run: su -c "sh /data/local/tmp/anl/measure.sh visible 20"      Env: PKG.
L=${1:-sample}
N=${2:-20}
P=${PKG:-com.anland.consumer.mobilegl}
GPU=/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage

pids_named() {   # exact process names (comm or argv0 basename)
    for n in "$@"; do pidof "$n"; done
}
pids_matching() {   # argv contains the word
    for d in /proc/[0-9]*; do
        { tr '\0' ' ' < $d/cmdline; } 2>/dev/null | grep -q "$1" && echo ${d#/proc/}
    done
}
ticks() {   # utime+stime of the pids, in clock ticks
    t=0
    for p in "$@"; do
        [ -r /proc/$p/stat ] || continue
        v=$(sed 's/^.*) //' /proc/$p/stat | awk '{print $12 + $13}')
        t=$((t + v))
    done
    echo $t
}
total() { awk '/^cpu /{print $2+$3+$4+$6+$7+$8}' /proc/stat; }

KW="$(pids_named kwin_wayland Xwayland)"
SH="$(pids_named plasmashell)"
CH="$(pids_matching /opt/google/chrome/)"
SV="$(pidof $P:mobilegl)"
UI="$(pidof $P)"

a0=$(total); k0=$(ticks $KW); s0=$(ticks $SH); c0=$(ticks $CH); v0=$(ticks $SV); u0=$(ticks $UI)
g=0; i=0
while [ $i -lt $N ]; do
    sleep 1
    b=$(cat $GPU 2>/dev/null | tr -dc '0-9'); g=$((g + ${b:-0})); i=$((i + 1))
done
a1=$(total); k1=$(ticks $KW); s1=$(ticks $SH); c1=$(ticks $CH); v1=$(ticks $SV); u1=$(ticks $UI)
# CLK_TCK is 100: ticks per second == percent of one core.
pct() { echo $(( ($2 - $1) / N )); }
printf '%-10s %3ss  phone %4s%%  kwin %3s%%  plasmashell %3s%%  chrome %3s%%  server %3s%%  app-ui %3s%%  gpu %3s%%\n' \
    "$L" "$N" "$(pct $a0 $a1)" "$(pct $k0 $k1)" "$(pct $s0 $s1)" "$(pct $c0 $c1)" "$(pct $v0 $v1)" "$(pct $u0 $u1)" "$((g / N))"
