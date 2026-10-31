# MobileGL - tools/device_bench/anland_glmark2/pbset.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# pbset.sh <tag> [extra env] - (phone) the affected-scene set, once, es2 window + texture fullscreen
t=$1; shift; E="$*"
cd /data/local/tmp/anl
sh ab.sh arch-kde-mgl $t-w $E glmark2-es2-wayland -b texture:duration=5 -b build:use-vbo=false:duration=5 -b build:use-vbo=true:duration=5 -b desktop:blur-radius=5:effect=blur:passes=1:separable=true:windows=4:duration=5 -b desktop:effect=shadow:windows=4:duration=5 -b ideas:speed=duration:duration=5 -b terrain:duration=5 -b refract:duration=5 -b jellyfish:duration=5 -b shadow:duration=5
sh ab.sh arch-kde-mgl $t-f $E glmark2-es2-wayland --fullscreen -b texture:duration=5 -b terrain:duration=5
