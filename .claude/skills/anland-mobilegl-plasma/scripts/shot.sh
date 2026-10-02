#!/usr/bin/env bash
# shot.sh <name> [count] [interval-s] - take <count> (default 2) phone screenshots <interval> s apart
# (default 1), pull them to $ANL_DIR/<name>-<i>.png and print their md5.  Identical hashes while an
# animated client (glmark2, a video) runs mean the frames are NOT reaching the screen.
set -euo pipefail
. "$(dirname "$0")/env.sh"
name=${1:?usage: shot.sh <name> [count] [interval]}; n=${2:-2}; dt=${3:-1}
cmd=""; i=1
while [ "$i" -le "$n" ]; do cmd="$cmd screencap -p $DEV_SHARE/$name-$i.png; sleep $dt;"; i=$((i+1)); done
adb_ shell "su -c '$cmd'"
i=1
while [ "$i" -le "$n" ]; do
    adb_ pull "$DEV_SHARE/$name-$i.png" "$ANL_DIR/$name-$i.png" >/dev/null
    md5sum "$ANL_DIR/$name-$i.png"; i=$((i+1))
done
