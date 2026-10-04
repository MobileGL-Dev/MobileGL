# pbclk.sh save|pin|restore|show  (phone, root) - CPU/GPU clock pinning for the glmark2 baseline
O=/data/local/tmp/anl/pb-clocks.orig
C=/sys/devices/system/cpu/cpufreq; G=/sys/class/kgsl/kgsl-3d0
CPU_PIN="policy0:1804800 policy2:2246400 policy5:2246400 policy7:2246400"
GPU_PIN=680000000
case $1 in
save)
  [ -f $O ] && { echo "already saved: $O"; cat $O; exit 0; }
  for p in policy0 policy2 policy5 policy7; do echo "$p $(cat $C/$p/scaling_governor) $(cat $C/$p/scaling_min_freq) $(cat $C/$p/scaling_max_freq)"; done > $O
  echo "gpu $(cat $G/devfreq/governor) $(cat $G/devfreq/min_freq) $(cat $G/devfreq/max_freq) $(cat $G/max_pwrlevel) $(cat $G/min_pwrlevel)" >> $O
  cat $O;;
pin)
  for e in $CPU_PIN; do p=${e%%:*}; f=${e##*:}; echo $f > $C/$p/scaling_max_freq; echo $f > $C/$p/scaling_min_freq; echo $f > $C/$p/scaling_max_freq; done
  echo $GPU_PIN > $G/devfreq/max_freq; echo $GPU_PIN > $G/devfreq/min_freq; echo $GPU_PIN > $G/devfreq/max_freq
  sh $0 show;;
restore)
  [ -f $O ] || { echo "no saved state"; exit 1; }
  while read p gov mn mx a b; do
    if [ $p = gpu ]; then echo $mx > $G/devfreq/max_freq; echo $mn > $G/devfreq/min_freq; echo $mx > $G/devfreq/max_freq
    else echo $gov > $C/$p/scaling_governor; echo $mx > $C/$p/scaling_max_freq; echo $mn > $C/$p/scaling_min_freq; echo $mx > $C/$p/scaling_max_freq; fi
  done < $O
  sh $0 show;;
show)
  for p in policy0 policy2 policy5 policy7; do echo "$p gov=$(cat $C/$p/scaling_governor) min=$(cat $C/$p/scaling_min_freq) max=$(cat $C/$p/scaling_max_freq) cur=$(cat $C/$p/scaling_cur_freq)"; done
  echo "gpu gov=$(cat $G/devfreq/governor) min=$(cat $G/devfreq/min_freq) max=$(cat $G/devfreq/max_freq) cur=$(cat $G/devfreq/cur_freq)";;
esac
