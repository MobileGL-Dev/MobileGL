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
# Clock profiles (P15 measurement rules). Compare numbers only within one profile.
#   sustained (default): CPU 2035.2 MHz big/prime, 1574.4 little; GPU 680 MHz (soaked)
#   gpumax: as sustained but GPU 903 MHz (shows whether a scene is GPU-bound; not sustainable)
#   gpulow: as sustained but GPU 422 MHz (amplifies GPU cost)
#   cpulow: CPU 1286.4 big/prime, 1017.6 little; GPU 680 MHz (amplifies CPU differences)
#   cpuhunt: CPU <=1.5 GHz big/prime, <=1.25 GHz little; GPU 903 MHz - cleanly CPU-bound (CPU-overhead
#            hunting: vanilla/rd12 gap, driver-call redundancy, S1-S3). Short cool-started runs.
#   gpuhunt: CPU <=2.4 GHz big/prime, <=1.8 GHz little; GPU 578 MHz - cleanly GPU-bound (GPU-side
#            hunting: the Espryt BSL gap, pass structure, load/store).
# PIN_* variables still override a profile's values.
PROFILE="${PROFILE:-sustained}"
case "$PROFILE" in
  sustained) p_cpu=2035200 p_little=1574400 p_gpu=680000000 ;;
  gpumax) p_cpu=2035200 p_little=1574400 p_gpu=903000000 ;;
  gpulow) p_cpu=2035200 p_little=1574400 p_gpu=422000000 ;;
  cpulow) p_cpu=1286400 p_little=1017600 p_gpu=680000000 ;;
  cpuhunt) p_cpu=1500000 p_little=1250000 p_gpu=903000000 ;;
  gpuhunt) p_cpu=2400000 p_little=1800000 p_gpu=578000000 ;;
  *) echo "pin_clocks.sh: unknown PROFILE $PROFILE" >&2; exit 2 ;;
esac
PIN_CPU_KHZ="${PIN_CPU_KHZ:-$p_cpu}"
PIN_LITTLE_KHZ="${PIN_LITTLE_KHZ:-$p_little}"
PIN_GPU_HZ="${PIN_GPU_HZ:-$p_gpu}"
PROFILE_FILE=/data/local/tmp/p15-bench-profile
KGSL=/sys/class/kgsl/kgsl-3d0/devfreq
SAVED=/data/local/tmp/p14-clocks.saved

su_sh() { adb -s "$SER" shell "su -c '$1'"; }

case "${1:-show}" in
  pin)
    su_sh "test -f $SAVED || { for p in /sys/devices/system/cpu/cpufreq/policy*; do echo \$p \$(cat \$p/scaling_min_freq) \$(cat \$p/scaling_max_freq); done; echo $KGSL \$(cat $KGSL/min_freq) \$(cat $KGSL/max_freq); } > $SAVED"
    su_sh "for p in /sys/devices/system/cpu/cpufreq/policy*; do want=$PIN_CPU_KHZ; [ \$p = /sys/devices/system/cpu/cpufreq/policy0 ] && want=$PIN_LITTLE_KHZ; f=0; for a in \$(cat \$p/scaling_available_frequencies); do [ \$a -le \$want ] && f=\$a; done; echo \$f > \$p/scaling_max_freq; echo \$f > \$p/scaling_min_freq; echo \$f > \$p/scaling_max_freq; done; echo $PIN_GPU_HZ > $KGSL/max_freq; echo $PIN_GPU_HZ > $KGSL/min_freq; echo $PIN_GPU_HZ > $KGSL/max_freq"
    su_sh "echo profile=$PROFILE cpu=$PIN_CPU_KHZ little=$PIN_LITTLE_KHZ gpu=$PIN_GPU_HZ > $PROFILE_FILE"
    "$0" show ;;
  restore)
    su_sh "rm -f $PROFILE_FILE"
    su_sh "test -f $SAVED && while read p mn mx; do if [ \$p = $KGSL ]; then echo \$mx > \$p/max_freq; echo \$mn > \$p/min_freq; else echo \$mx > \$p/scaling_max_freq; echo \$mn > \$p/scaling_min_freq; fi; done < $SAVED; rm -f $SAVED"
    "$0" show ;;
  show)
    su_sh "for p in /sys/devices/system/cpu/cpufreq/policy*; do echo \$p cur=\$(cat \$p/scaling_cur_freq) min=\$(cat \$p/scaling_min_freq) max=\$(cat \$p/scaling_max_freq) avail=\$(cat \$p/scaling_available_frequencies); done; echo gpu cur=\$(cat $KGSL/cur_freq) min=\$(cat $KGSL/min_freq) max=\$(cat $KGSL/max_freq) avail=\$(cat /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies); dumpsys battery | grep temperature" ;;
esac
