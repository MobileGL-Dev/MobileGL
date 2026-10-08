#!/system/bin/sh
# freq_sampler.sh <outfile> [period_s=2] - DEVICE-SIDE (pushed to /data/local/tmp, run as root in the
# background for the length of one measurement, killed by the caller). One line per sample:
#   t=<uptime> c<policy>=<cur>/<max> ... gpu=<devfreq cur> gpuclk=<kgsl gpuclk> tpl=<thermal_pwrlevel>
#   thr=<throttling> busy=<gpu_busy_percentage> cpuT=<hottest cpu zone> gpuT=<hottest gpuss zone> skinT=<skin>
# Cheap sysfs reads only, so the run it watches is not perturbed. freqcheck.py turns the file into
# min/max/peak and a VALID/INVALID verdict (P15 measurement-validity rule).
o=$1; p=${2:-2}; : > "$o"
K=/sys/class/kgsl/kgsl-3d0
zones=""
for z in /sys/class/thermal/thermal_zone*; do
  case $(cat $z/type) in cpu-*|cpuss-*|gpuss-*|skin-msm-therm) zones="$zones $z";; esac
done
while :; do
  l="t=$(cut -d' ' -f1 /proc/uptime)"
  for c in /sys/devices/system/cpu/cpufreq/policy*; do
    l="$l c${c##*policy}=$(cat $c/scaling_cur_freq)/$(cat $c/scaling_max_freq)"
  done
  m=0; g=0; s=0
  for z in $zones; do
    v=$(cat $z/temp 2>/dev/null); [ -z "$v" ] && continue
    case $(cat $z/type) in cpu-*|cpuss-*) [ $v -gt $m ] && m=$v;; gpuss-*) [ $v -gt $g ] && g=$v;; skin-msm-therm) s=$v;; esac
  done
  echo "$l gpu=$(cat $K/devfreq/cur_freq) gpuclk=$(cat $K/gpuclk) tpl=$(cat $K/thermal_pwrlevel) thr=$(cat $K/throttling) busy=$(cat $K/gpu_busy_percentage | tr -d ' %') cpuT=$m gpuT=$g skinT=$s" >> "$o"
  sleep $p
done
