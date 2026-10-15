# MobileGL - tools/device_bench/anland_glmark2/pbsamp.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# pbsamp.sh <outfile> - sample GPU freq/busy, temps, cpu7 freq every 2 s until killed
o=$1; : > $o
while :; do
  g=$(cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq); b=$(cat /sys/class/kgsl/kgsl-3d0/gpubusy)
  m=0; gt=0; for z in /sys/class/thermal/thermal_zone*; do case $(cat $z/type) in cpu-*|cpuss-*) v=$(cat $z/temp); [ $v -gt $m ] && m=$v;; gpuss-*) v=$(cat $z/temp); [ $v -gt $gt ] && gt=$v;; esac; done
  echo "$(date +%s) gpuf=$g busy=$b cpuT=$m gpuT=$gt c7=$(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq) c5=$(cat /sys/devices/system/cpu/cpu5/cpufreq/scaling_cur_freq)" >> $o
  sleep 2
done
