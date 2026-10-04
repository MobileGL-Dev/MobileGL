# pbbatch.sh <container> <stack stock|mgl> <label> <cfg...>  (phone, root; run detached)
# cfg: es2w es2f glw glf ; results -> /data/local/tmp/anl/pb/<label>-<cfg>.*
ct=$1; st=$2; lab=$3; shift 3
D=/data/local/tmp/anl/pb; mkdir -p $D; DS=/data/local/Droidspaces/bin/droidspaces
CR=/mnt/Droidspaces/$ct/root
for cfg in "$@"; do
  case $cfg in
    es2w) cmd="glmark2-es2-wayland -b :duration=5";;
    es2f) cmd="glmark2-es2-wayland --fullscreen -b :duration=5";;
    glw)  cmd="glmark2-wayland -b :duration=5";;
    glf)  cmd="glmark2-wayland --fullscreen -b :duration=5";;
    es2o) cmd="glmark2-es2-wayland --off-screen -b :duration=5";;
    glo)  cmd="glmark2-wayland --off-screen -b :duration=5";;
    *) cmd="$cfg";;
  esac
  tag=$lab-$cfg
  [ $ct = arch-kde ] && sh $D/../stfix.sh > $D/$tag.fix 2>&1
  sh $D/../pbcool.sh 46000 45 300 > $D/$tag.meta
  echo "cmd: $cmd" >> $D/$tag.meta; echo "start $(date +%s)" >> $D/$tag.meta
  sh $D/../pbsamp.sh $D/$tag.samp & sp=$!
  rm -f $D/$tag.ended; sh $D/../pbprobe.sh $tag &
  ( sleep 25; screencap -p /data/local/tmp/anl/pb/$tag-s1.png; sleep 1; screencap -p /data/local/tmp/anl/pb/$tag-s2.png; md5sum $D/$tag-s1.png $D/$tag-s2.png >> $D/$tag.meta; stat -c "s1_bytes=%s" $D/$tag-s1.png >> $D/$tag.meta; sh $D/../pbshown.sh >> $D/$tag.meta ) &
  mkdir -p $CR/pb; echo "$cmd" > $CR/pb/$tag.cmd
  $DS --name=$ct run bash /root/pbrun.sh $st $tag >> $D/$tag.meta 2>&1
  kill $sp; touch $D/$tag.ended; echo "end $(date +%s)" >> $D/$tag.meta; sleep 2
  cp $CR/pb/$tag.out $D/ 2>/dev/null; cp $CR/pb/pb-$tag.client.log $D/ 2>/dev/null
done
echo DONE > $D/$lab.done
