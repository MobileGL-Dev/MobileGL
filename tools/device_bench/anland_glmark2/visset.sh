# visset.sh <tag> - screenshot several scenes mid-run (phone, root)
CR=/mnt/Droidspaces/arch-kde-mgl/root; D=/data/local/tmp/anland-mobilegl
for sc in ideas:duration=6 terrain:duration=6 shadow:duration=6 refract:duration=6 jellyfish:duration=6 desktop:effect=shadow:windows=4:duration=6 desktop:blur-radius=5:effect=blur:passes=1:separable=true:windows=4:duration=6 bump:bump-render=high-poly:duration=6 conditionals:duration=6 effect2d:duration=6; do
  n=$(echo $sc | cut -d: -f1)
  echo "glmark2-es2-wayland -b $sc" > $CR/pb/vis.cmd
  /data/local/Droidspaces/bin/droidspaces --name=arch-kde-mgl run bash /root/pbrun.sh mgl vis >/dev/null 2>&1 &
  sleep 4; screencap -p $D/$1-$n.png; wait
done
ls $D/$1-*.png | wc -l
