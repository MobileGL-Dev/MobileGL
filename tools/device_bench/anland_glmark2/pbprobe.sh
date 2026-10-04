# pbprobe.sh <tag> - every 15 s while the run lasts: two screenshots 1 s apart; logs size and whether they differ
D=/data/local/tmp/anl/pb; tag=$1; n=0
sleep 10
while [ ! -f $D/$tag.ended ]; do
  screencap -p $D/pr-a.png; sleep 1; screencap -p $D/pr-b.png
  a=$(md5sum $D/pr-a.png | cut -c1-8); b=$(md5sum $D/pr-b.png | cut -c1-8); s=$(stat -c %s $D/pr-a.png)
  st=ok; [ "$a" = "$b" ] && st=STATIC; [ $s -lt 200000 ] && st=BLACK
  echo "probe t=$n size=$s $st" >> $D/$tag.meta
  n=$((n+15)); sleep 13
done
