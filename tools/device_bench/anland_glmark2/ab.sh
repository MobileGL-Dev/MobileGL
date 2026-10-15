# MobileGL - tools/device_bench/anland_glmark2/ab.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# ab.sh <ct> <tag> <cmd...> - one quick run (phone, root), prints FPS lines
ct=$1; tag=$2; shift 2; CR=/mnt/Droidspaces/$ct/root; st=mgl; [ $ct = arch-kde ] && st=stock
sh /data/local/tmp/anl/pbcool.sh 46000 20 300 >/dev/null
mkdir -p $CR/pb; echo "$*" > $CR/pb/$tag.cmd
/data/local/Droidspaces/bin/droidspaces --name=$ct run bash /root/pbrun.sh $st $tag >/dev/null 2>&1
grep -h FPS $CR/pb/$tag.out | sed -e 's/FrameTime.*//' -e "s/^/$tag /"
