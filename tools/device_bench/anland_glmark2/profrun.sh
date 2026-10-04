# profrun.sh <container> <prefix> - profile the scene list (phone, root)
ct=$1; pre=$2; E="MOBILEGL_PIPE_STATS=1"; [ $ct = arch-kde ] && E=""
run() { t=$1; shift; sh /data/local/tmp/anl/pbcool.sh 46000 30 300 > /data/local/tmp/anl/prof/$pre-$t.cool; sh /data/local/tmp/anl/pbprof.sh $ct $pre-$t 7 8 "$@"; }
mkdir -p /data/local/tmp/anl/prof
run tex   $E glmark2-es2-wayland -b texture:texture-filter=linear:duration=18
run texF  $E glmark2-es2-wayland --fullscreen -b texture:texture-filter=linear:duration=18
run blur  $E glmark2-es2-wayland -b desktop:blur-radius=5:effect=blur:passes=1:separable=true:windows=4:duration=18
run shad  $E glmark2-es2-wayland -b desktop:effect=shadow:windows=4:duration=18
run ideas $E glmark2-es2-wayland -b ideas:speed=duration:duration=18
run terr  $E glmark2-es2-wayland -b terrain:duration=18
run bvbo  $E glmark2-es2-wayland -b build:use-vbo=false:duration=18
echo DONE > /data/local/tmp/anl/prof/$pre.done
