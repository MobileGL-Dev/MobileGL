# MobileGL - tools/device_bench/anland_glmark2/lm.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# lm.sh <tag> - per-frame link-metric averages of the last run's client log
L=/mnt/Droidspaces/arch-kde-mgl/root/pb/pb-$1.client.log
grep -a "kind=frame-op" $L | tail -1 | cut -c40-200
grep -a "P65LinkMetrics kind=frame " $L | tail -300 | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]} w+=v["wall_ns"]; c+=v["client_thread_cpu_ns"]; t+=v["transport_wait_ns"]; r+=v["rtt_mean_us"]; n++} END{printf "wall %.2fms cpu %.2fms twait %.2fms rtt %.0fus\n", w/n/1e6, c/n/1e6, t/n/1e6, r/n}'
