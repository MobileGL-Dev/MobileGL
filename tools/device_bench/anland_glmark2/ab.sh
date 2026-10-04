# ab.sh <ct> <tag> <cmd...> - one quick run (phone, root), prints FPS lines
ct=$1; tag=$2; shift 2; CR=/mnt/Droidspaces/$ct/root; st=mgl; [ $ct = arch-kde ] && st=stock
sh /data/local/tmp/anl/pbcool.sh 46000 20 300 >/dev/null
mkdir -p $CR/pb; echo "$*" > $CR/pb/$tag.cmd
/data/local/Droidspaces/bin/droidspaces --name=$ct run bash /root/pbrun.sh $st $tag >/dev/null 2>&1
grep -h FPS $CR/pb/$tag.out | sed -e 's/FrameTime.*//' -e "s/^/$tag /"
