# MobileGL - tools/device_bench/anland_glmark2/pbcool.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# pbcool.sh [limit_mC=40000] [min_s=20] [max_s=600] - wait until CPU/GPU zones are below the limit
lim=${1:-40000}; mn=${2:-20}; mx=${3:-600}; t=0
hot() { m=0; for z in /sys/class/thermal/thermal_zone*; do case $(cat $z/type) in cpu-*|cpuss-*|gpuss-*) v=$(cat $z/temp); [ $v -gt $m ] && m=$v;; esac; done; echo $m; }
while :; do h=$(hot); if [ $t -ge $mn ] && [ $h -lt $lim ]; then break; fi; [ $t -ge $mx ] && break; sleep 5; t=$((t+5)); done
echo "cool: waited ${t}s maxtemp=$h batt=$(dumpsys battery | grep level | tr -d ' ')"
