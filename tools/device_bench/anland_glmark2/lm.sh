# lm.sh <tag> - per-frame link-metric averages of the last run's client log
L=/mnt/Droidspaces/arch-kde-mgl/root/pb/pb-$1.client.log
grep -a "kind=frame-op" $L | tail -1 | cut -c40-200
grep -a "P65LinkMetrics kind=frame " $L | tail -300 | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]} w+=v["wall_ns"]; c+=v["client_thread_cpu_ns"]; t+=v["transport_wait_ns"]; r+=v["rtt_mean_us"]; n++} END{printf "wall %.2fms cpu %.2fms twait %.2fms rtt %.0fus\n", w/n/1e6, c/n/1e6, t/n/1e6, r/n}'
