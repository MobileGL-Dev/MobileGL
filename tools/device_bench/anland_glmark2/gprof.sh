# gprof.sh <ct> <tag> <rec_s> <cmd...> - offcpu+callgraph record of the busiest server apply thread + client
ct=$1; tag=$2; rec=$3; shift 3; D=/data/local/tmp/anl/prof; CR=/mnt/Droidspaces/$ct/root
echo "$*" > $CR/pb/$tag.cmd
/data/local/Droidspaces/bin/droidspaces --name=$ct run bash /root/pbrun.sh mgl $tag > $D/$tag.run 2>&1 &
sleep 9
s=$(pidof com.anland.consumer.mobilegl:mobilegl); c=$(pidof glmark2-es2-wayland glmark2-wayland | awk '{print $1}')
t=$(top -H -b -n1 -p $s -o TID,CMD,%CPU | grep mgl-srv-apply | sort -k3 -rn | head -1 | awk '{print $1}')
echo "server_tid=$t client=$c" > $D/$tag.pids
simpleperf record --trace-offcpu -e cpu-clock -f 2000 -g -t $t,$c --duration $rec -o $D/$tag.perf.data > $D/$tag.perf.log 2>&1
simpleperf report-sample -i $D/$tag.perf.data --show-callchain > $D/$tag.samples 2>/dev/null
chmod 644 $D/$tag.*
wait
