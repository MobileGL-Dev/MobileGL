# pbprof.sh <container> <tag> <warm_s> <rec_s> <cmd...>  (phone, root)
# Runs <cmd> in the container (mgl stack via pbrun), then after warm_s records: per-thread CPU (top -H),
# simpleperf on the server (:mobilegl) and on the client, GPU busy samples. Results: /data/local/tmp/anl/prof/<tag>.*
ct=$1; tag=$2; warm=$3; rec=$4; shift 4
D=/data/local/tmp/anl/prof; mkdir -p $D; DS=/data/local/Droidspaces/bin/droidspaces; CR=/mnt/Droidspaces/$ct/root
st=mgl; [ $ct = arch-kde ] && st=stock
mkdir -p $CR/pb; echo "$*" > $CR/pb/$tag.cmd
$DS --name=$ct run bash /root/pbrun.sh $st $tag > $D/$tag.run 2>&1 &
sleep $warm
cli=$(pidof glmark2-es2-wayland glmark2-wayland | awk '{print $1}')
srv=$(pidof com.anland.consumer.mobilegl:mobilegl)
kw=$(pidof kwin_wayland | awk '{print $1}')
echo "client=$cli server=$srv kwin=$kw" > $D/$tag.pids
sh /data/local/tmp/anl/pbsamp.sh $D/$tag.samp & sp=$!
P=$cli; [ -n "$srv" ] && P=$P,$srv; [ -n "$kw" ] && P=$P,$kw
top -H -b -d $rec -n 2 -p $(echo $P | tr , ' ' | sed 's/ /,/g') -m 40 -o TID,PID,CMD,%CPU > $D/$tag.top 2>&1 &
simpleperf record -e cpu-clock -f 4000 -p $P --duration $rec -o $D/$tag.perf.data > $D/$tag.perf.log 2>&1
kill $sp
sh /data/local/tmp/anl/pbshown.sh >> $D/$tag.pids
wait
