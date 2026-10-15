#!/bin/bash
# MobileGL - tools/device_bench/anland_glmark2/pbrun.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# pbrun.sh <stack: stock|mgl> <tag>  (container, root) - runs the command in /root/pb/<tag>.cmd
st=$1; tag=$2
mkdir -p /root/pb; out=/root/pb/$tag.out; cmd=$(cat /root/pb/$tag.cmd)
if [ "$st" = stock ]; then
  sudo -u swung0x48 bash -c 'set -a; . /etc/environment; set +a; export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus; cd /tmp; kscreen-doctor --dpms on >/dev/null 2>&1; eval "exec kde-inhibit --power --screenSaver $1"' x "$cmd" > $out 2>&1
else
  rm -f /tmp/pb-$tag.client.log
  MGLOG=/tmp/pb-$tag.log mgrun offscreen bash -c "$cmd" > $out 2>&1
  grep -a -m1 "presented through" /tmp/pb-$tag.client.log >> $out
  cp /tmp/pb-$tag.client.log /root/pb/ 2>/dev/null
fi
grep -a "Score" $out
