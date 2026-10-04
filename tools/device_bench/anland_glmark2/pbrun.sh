#!/bin/bash
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
