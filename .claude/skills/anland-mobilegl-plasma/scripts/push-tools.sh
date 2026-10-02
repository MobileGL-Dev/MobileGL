#!/usr/bin/env bash
# push-tools.sh - install this skill's device- and container-side helpers on the phone.
#   device/*.sh      -> $DEV_TOOLS            (run as root: adb shell 'su -c "sh $DEV_TOOLS/<x>.sh"')
#   container/mgrun  -> <container>/usr/local/bin/mgrun
#   container/kwprep.sh -> <container>/root/kwprep.sh
# Needs the container running (its rootfs is mounted at $CT_ROOT only then).
# Overwrites same-named files other sessions may be using in $DEV_TOOLS; check first on a shared phone.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
adb_ shell "mkdir -p $DEV_TOOLS"
for f in "$here"/device/*.sh; do
    tr -d '\r' < "$f" > "$ANL_DIR/push.tmp"
    adb_ push "$ANL_DIR/push.tmp" "$DEV_TOOLS/$(basename "$f")" >/dev/null
    echo "pushed $DEV_TOOLS/$(basename "$f")"
done
for f in mgrun kwprep.sh; do
    tr -d '\r' < "$here/container/$f" > "$ANL_DIR/push.tmp"
    adb_ push "$ANL_DIR/push.tmp" "$DEV_TOOLS/$f" >/dev/null
done
rm -f "$ANL_DIR/push.tmp"
adb_ shell "su -c 'cp $DEV_TOOLS/mgrun $CT_ROOT/usr/local/bin/mgrun && chmod 755 $CT_ROOT/usr/local/bin/mgrun && cp $DEV_TOOLS/kwprep.sh $CT_ROOT/root/kwprep.sh && echo installed mgrun kwprep.sh'"
