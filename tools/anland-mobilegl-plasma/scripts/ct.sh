#!/usr/bin/env bash
# MobileGL - tools/anland-mobilegl-plasma/scripts/ct.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# ct.sh <channel> [script-file] - run a bash script as ROOT inside the container (login shell),
# reading the script from the file or from stdin.  Generic form of the old c.sh/c2.sh/c3.sh.
#
#   printf '%s\n' 'ls -la /tmp/*.log' | bash ct.sh mychan
#   bash ct.sh mychan ./something.cmd
#
# How: the script is written to $ANL_DIR/<channel>.cmd, pushed to $DEV_TOOLS/<channel>.cmd together with
# a one-line <channel>.sh that does `droidspaces --name=<container> run bash -lc "$(cat <channel>.cmd)"`,
# and that runs under `su`.  Quoting through adb + su + droidspaces is thereby avoided entirely.
#
# Pick a channel name of your own: other sessions on the same phone use c, c2, c3, xd, xr, gbm...;
# writing to their channel files races with them.  CR characters are stripped (Windows checkouts).
# Inside, run user-side commands with `sudo -u <desktop user> env XDG_RUNTIME_DIR=/run/user/<uid> ...`.
# Never `pkill -f <word>` in a channel script: the `bash -lc "<script>"` process carries the whole
# script on its command line and kills itself.  Use `pkill -x <name>`.
set -euo pipefail
. "$(dirname "$0")/env.sh"
ch=${1:?usage: ct.sh <channel> [script-file]}
src=${2:--}
if [ "$src" = - ]; then tr -d '\r' > "$ANL_DIR/$ch.cmd"; else tr -d '\r' < "$src" > "$ANL_DIR/$ch.cmd"; fi
printf '%s\n' "$DS_BIN --name=$MGL_CONTAINER run bash -lc \"\$(cat $DEV_TOOLS/$ch.cmd)\"" > "$ANL_DIR/$ch.sh"
adb_ shell "mkdir -p $DEV_TOOLS"
adb_ push "$ANL_DIR/$ch.cmd" "$DEV_TOOLS/$ch.cmd" >/dev/null
adb_ push "$ANL_DIR/$ch.sh" "$DEV_TOOLS/$ch.sh" >/dev/null
adb_ shell "su -c 'sh $DEV_TOOLS/$ch.sh'"
