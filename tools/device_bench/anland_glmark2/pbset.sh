# pbset.sh <tag> [extra env] - (phone) the affected-scene set, once, es2 window + texture fullscreen
t=$1; shift; E="$*"
cd /data/local/tmp/anl
sh ab.sh arch-kde-mgl $t-w $E glmark2-es2-wayland -b texture:duration=5 -b build:use-vbo=false:duration=5 -b build:use-vbo=true:duration=5 -b desktop:blur-radius=5:effect=blur:passes=1:separable=true:windows=4:duration=5 -b desktop:effect=shadow:windows=4:duration=5 -b ideas:speed=duration:duration=5 -b terrain:duration=5 -b refract:duration=5 -b jellyfish:duration=5 -b shadow:duration=5
sh ab.sh arch-kde-mgl $t-f $E glmark2-es2-wayland --fullscreen -b texture:duration=5 -b terrain:duration=5
