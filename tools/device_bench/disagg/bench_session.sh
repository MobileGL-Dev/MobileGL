#!/usr/bin/env bash
# bench_session.sh start|stop|status - P15 measurement-validity rule, device side (Lenovo Y700, SM8650).
#
# start:
#   1. records each frequency-moving service's prior state and the msm_performance requests
#      (/data/local/tmp/p15-bench-session, also echoed to the caller's log);
#   2. stops those services: thermal-engine, the QTI perf HAL, the vendor perf service, Lenovo's
#      performance and hyperschedule services. Their requests OUTLIVE them: the perf stack's
#      game-mode levels land in /sys/kernel/msm_performance/parameters/cpu_{max,min}_freq, a
#      frequency QoS that clamps scaling_max_freq below any pin (measured 2026-10-08: 787 / 1286 /
#      1075 / 1248 MHz on policies 0/2/5/7 while pin_clocks.sh believed it had pinned 2035 MHz);
#   3. releases those requests and re-pins (pin_clocks.sh).
# stop: starts every service that was running before start, and verifies each is running again.
#   ALWAYS run it - callers trap it on EXIT/INT/TERM, so an interrupted session restores too.
# Kernel-level protection (LMh, BCL, kernel thermal trip points) is never touched; the run's sampler
# (freq_sampler.sh -> freqcheck.py) shows any frequency collapse, and a collapse ends benchmarking.
set -u
export MSYS_NO_PATHCONV=1
SER="${SER:-HA27Q3LQ}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SERVICES="${BENCH_STOP_SERVICES:-thermal-engine perf2-hal-1-0 vendor.perfservice performance hyperschedule_hal_service}"
MARK=/data/local/tmp/p15-bench-session
MP=/sys/kernel/msm_performance/parameters
su_sh() { adb -s "$SER" shell "su -c '$1'"; }
case "${1:-status}" in
  start)
    su_sh "if [ ! -f $MARK ]; then for s in $SERVICES; do echo svc \$s \$(getprop init.svc.\$s); done > $MARK; echo maxreq \$(cat $MP/cpu_max_freq) >> $MARK; echo minreq \$(cat $MP/cpu_min_freq) >> $MARK; fi; cat $MARK"
    su_sh "for s in $SERVICES; do setprop ctl.stop \$s; done"
    sleep 2
    su_sh "echo \"0:4294967295 1:4294967295 2:4294967295 3:4294967295 4:4294967295 5:4294967295 6:4294967295 7:4294967295\" > $MP/cpu_max_freq; echo \"0:0 1:0 2:0 3:0 4:0 5:0 6:0 7:0\" > $MP/cpu_min_freq"
    SER=$SER bash "$HERE/pin_clocks.sh" pin
    "$0" status ;;
  stop)
    su_sh "if [ -f $MARK ]; then grep \"^svc \" $MARK | while read k s st; do [ \"\$st\" = running ] && setprop ctl.start \$s; done; else for s in $SERVICES; do setprop ctl.start \$s; done; fi"
    sleep 3
    bad=$(su_sh "if [ -f $MARK ]; then grep \"^svc \" $MARK | while read k s st; do [ \"\$st\" = running ] && [ \"\$(getprop init.svc.\$s)\" != running ] && echo \$s; done; fi" | tr -d '\r')
    if [ -n "$bad" ]; then echo "bench_session: NOT RESTORED: $bad" >&2; exit 1; fi
    su_sh "rm -f $MARK"
    "$0" status ;;
  status)
    su_sh "for s in $SERVICES; do echo \$s=\$(getprop init.svc.\$s); done | tr '\n' ' '; echo; echo maxreq \$(cat $MP/cpu_max_freq)" ;;
esac
