#!/usr/bin/env bash
# MobileGL - tools/anland-mobilegl-plasma/scripts/shot.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

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
