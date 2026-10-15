# MobileGL - tools/device_bench/anland_glmark2/stfix.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# stfix.sh - (phone, root) if the stock desktop shows black, restart the stock stack; prints what it did
D=/data/local/tmp/anl/pb; DS=/data/local/Droidspaces/bin/droidspaces; C=/data/local/Droidspaces/Containers/arch-kde/container.config
screencap -p $D/pre.png; s=$(stat -c %s $D/pre.png)
if [ "$s" -lt 200000 ]; then
  echo "stfix: black screen ($s bytes) - restarting the stock stack"
  am force-stop com.anland.consumer; $DS -C $C stop >/dev/null 2>&1; sleep 2; $DS -C $C start >/dev/null 2>&1; sleep 3
  monkey -p com.anland.consumer -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1; sleep 25
  cp $D/../pbrun.sh /mnt/Droidspaces/arch-kde/root/pbrun.sh
  screencap -p $D/pre.png; echo "stfix: now $(stat -c %s $D/pre.png) bytes"
else echo "stfix: screen ok ($s bytes)"; fi
