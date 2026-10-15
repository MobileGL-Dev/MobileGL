# MobileGL - tools/device_bench/anland_glmark2/visset.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# visset.sh <tag> - screenshot several scenes mid-run (phone, root)
CR=/mnt/Droidspaces/arch-kde-mgl/root; D=/data/local/tmp/anland-mobilegl
for sc in ideas:duration=6 terrain:duration=6 shadow:duration=6 refract:duration=6 jellyfish:duration=6 desktop:effect=shadow:windows=4:duration=6 desktop:blur-radius=5:effect=blur:passes=1:separable=true:windows=4:duration=6 bump:bump-render=high-poly:duration=6 conditionals:duration=6 effect2d:duration=6; do
  n=$(echo $sc | cut -d: -f1)
  echo "glmark2-es2-wayland -b $sc" > $CR/pb/vis.cmd
  /data/local/Droidspaces/bin/droidspaces --name=arch-kde-mgl run bash /root/pbrun.sh mgl vis >/dev/null 2>&1 &
  sleep 4; screencap -p $D/$1-$n.png; wait
done
ls $D/$1-*.png | wc -l
