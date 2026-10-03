# x11-measure.sh <label> <app-process-name> [seconds] - (Android root shell) CPU of the X11 path while
# an X11 app runs: % of ONE core (800 = all 8 cores) for the whole phone, the app, Xwayland,
# kwin_wayland, the app's :mobilegl server; mean kgsl gpu_busy_percentage; and the number of dma-buf
# descriptors Xwayland and KWin hold (window pixmaps / client buffers as dma-bufs). Refuses to run
# below 40% battery: the phone drains faster under load than USB charges it.
# Run: su -c "sh /data/local/tmp/anl/x11-measure.sh glamor glxgears 10"      Env: PKG, MIN_BATTERY.
L=${1:-sample}
APP=${2:-glxgears}
N=${3:-10}
P=${PKG:-com.anland.consumer.mobilegl}
GPU=/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage
level=$(dumpsys battery | sed -n 's/^ *level: *//p')
if [ "${level:-0}" -lt "${MIN_BATTERY:-40}" ]; then
    echo "battery ${level}% < ${MIN_BATTERY:-40}%: not loading the phone; charge first" >&2
    exit 3
fi
ticks() {
    t=0
    for p in "$@"; do
        [ -r /proc/$p/stat ] || continue
        v=$(sed 's/^.*) //' /proc/$p/stat | awk '{print $12 + $13}')
        t=$((t + v))
    done
    echo $t
}
total() { awk '/^cpu /{print $2+$3+$4+$6+$7+$8}' /proc/stat; }
dmabufs() {   # dma-buf descriptors held by the pids (Android kernels link them to /dmabuf:...)
    n=0
    for p in "$@"; do n=$((n + $(ls -l /proc/$p/fd 2>/dev/null | grep -c dmabuf))); done
    echo $n
}
AP="$(pidof $APP)"
XW="$(pidof Xwayland)"
KW="$(pidof kwin_wayland)"
SV="$(pidof $P:mobilegl)"
[ -n "$AP" ] || { echo "no running process named $APP" >&2; exit 2; }
a0=$(total); p0=$(ticks $AP); x0=$(ticks $XW); k0=$(ticks $KW); v0=$(ticks $SV)
g=0; i=0
while [ $i -lt $N ]; do
    sleep 1
    b=$(cat $GPU 2>/dev/null | tr -dc '0-9'); g=$((g + ${b:-0})); i=$((i + 1))
done
a1=$(total); p1=$(ticks $AP); x1=$(ticks $XW); k1=$(ticks $KW); v1=$(ticks $SV)
pct() { echo $(( ($2 - $1) / N )); }
printf '%-12s %3ss  phone %4s%%  %s %3s%%  Xwayland %3s%%  kwin %3s%%  server %3s%%  gpu %3s%%  dmabufs Xwayland %s kwin %s  battery %s%%\n' \
    "$L" "$N" "$(pct $a0 $a1)" "$APP" "$(pct $p0 $p1)" "$(pct $x0 $x1)" "$(pct $k0 $k1)" "$(pct $v0 $v1)" "$((g / N))" \
    "$(dmabufs $XW)" "$(dmabufs $KW)" "$level"
