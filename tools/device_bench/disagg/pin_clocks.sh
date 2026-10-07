#!/usr/bin/env bash
# Pin / restore CPU and GPU clocks on a rooted bench device (P14).
#   SER=<serial> bash pin_clocks.sh pin|restore|show
# pin: every cpufreq policy gets scaling_min = scaling_max = its PIN_<policy> value (default:
# the highest available frequency <= PIN_CPU_KHZ, 2.0 GHz), the Adreno devfreq min = max =
# PIN_GPU_HZ. The pre-pin values are saved on the device (/data/local/tmp/p14-clocks.saved)
# and restore puts them back. Never pin a hot device: check the battery temperature first.
set -u
export MSYS_NO_PATHCONV=1
SER="${SER:-HA27Q3LQ}"
PIN_CPU_KHZ="${PIN_CPU_KHZ:-2035200}"
PIN_LITTLE_KHZ="${PIN_LITTLE_KHZ:-1574400}"
PIN_GPU_HZ="${PIN_GPU_HZ:-903000000}"
KGSL=/sys/class/kgsl/kgsl-3d0/devfreq
SAVED=/data/local/tmp/p14-clocks.saved

su_sh() { adb -s "$SER" shell "su -c '$1'"; }

case "${1:-show}" in
  pin)
    su_sh "test -f $SAVED || { for p in /sys/devices/system/cpu/cpufreq/policy*; do echo \$p \$(cat \$p/scaling_min_freq) \$(cat \$p/scaling_max_freq); done; echo $KGSL \$(cat $KGSL/min_freq) \$(cat $KGSL/max_freq); } > $SAVED"
    su_sh "for p in /sys/devices/system/cpu/cpufreq/policy*; do want=$PIN_CPU_KHZ; [ \$p = /sys/devices/system/cpu/cpufreq/policy0 ] && want=$PIN_LITTLE_KHZ; f=0; for a in \$(cat \$p/scaling_available_frequencies); do [ \$a -le \$want ] && f=\$a; done; echo \$f > \$p/scaling_max_freq; echo \$f > \$p/scaling_min_freq; echo \$f > \$p/scaling_max_freq; done; echo $PIN_GPU_HZ > $KGSL/max_freq; echo $PIN_GPU_HZ > $KGSL/min_freq; echo $PIN_GPU_HZ > $KGSL/max_freq"
    "$0" show ;;
  restore)
    su_sh "test -f $SAVED && while read p mn mx; do if [ \$p = $KGSL ]; then echo \$mx > \$p/max_freq; echo \$mn > \$p/min_freq; else echo \$mx > \$p/scaling_max_freq; echo \$mn > \$p/scaling_min_freq; fi; done < $SAVED; rm -f $SAVED"
    "$0" show ;;
  show)
    su_sh "for p in /sys/devices/system/cpu/cpufreq/policy*; do echo \$p cur=\$(cat \$p/scaling_cur_freq) min=\$(cat \$p/scaling_min_freq) max=\$(cat \$p/scaling_max_freq); done; echo gpu cur=\$(cat $KGSL/cur_freq) min=\$(cat $KGSL/min_freq) max=\$(cat $KGSL/max_freq); dumpsys battery | grep temperature" ;;
esac
