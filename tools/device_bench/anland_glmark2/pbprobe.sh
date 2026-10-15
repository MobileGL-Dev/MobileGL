# MobileGL - tools/device_bench/anland_glmark2/pbprobe.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

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
