#!/usr/bin/env bash
# xdeploy.sh - install the WSL cross-built libMobileGL.so + mobilegl_gbm.so into the container, exactly
# like anland's anland-build-client.sh: copy to a NEW inode and rename into place (overwriting a mapped
# .so in place crashes every running client, KWin included), refresh the libEGL/libGL aliases, the
# glvnd GLX vendor link and the EGL vendor JSON.  Does not restart anything: already-running clients
# keep the old inode; restart the client (or the session) to pick up the new library.
# Env: MGL_XBUILD_DIR (WSL build dir), CHANNEL (ct.sh channel, default xd), WSL_DISTRO.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
mkdir -p "$ANL_DIR/xdist"
# 1. Copy the build outputs out of WSL.
wsl -d "${WSL_DISTRO:-archlinux}" -- env OUT="${MGL_XBUILD_DIR:-}" D="$(win2wsl "$ANL_DIR")/xdist" bash -s <<'IN' 2>&1 | tr -d '\0' | grep -av '^wsl:'
set -e; OUT=${OUT:-$HOME/mgl-xbuild-a64}
cp -f "$OUT/libMobileGL.so" "$D/libMobileGL.so"; cp -f "$OUT/MobileGL/MG_Gbm/mobilegl_gbm.so" "$D/mobilegl_gbm.so"
sha256sum "$D/libMobileGL.so" "$D/mobilegl_gbm.so"
IN
# 2. Push to the phone and stage them in the container's /root/xdeploy (the container's /tmp and the
#    phone's /data/local/tmp are different places; the rootfs is reachable at $CT_ROOT).
adb_ shell "mkdir -p $DEV_TOOLS/xdist"
adb_ push "$ANL_DIR/xdist/libMobileGL.so" "$DEV_TOOLS/xdist/libMobileGL.so"
adb_ push "$ANL_DIR/xdist/mobilegl_gbm.so" "$DEV_TOOLS/xdist/mobilegl_gbm.so"
adb_ shell "su -c 'mkdir -p $CT_ROOT/root/xdeploy && cp $DEV_TOOLS/xdist/libMobileGL.so $DEV_TOOLS/xdist/mobilegl_gbm.so $CT_ROOT/root/xdeploy/ && rm -f $DEV_TOOLS/xdist/*.so'"
# 3. Install inside the container.
bash "$here/ct.sh" "${CHANNEL:-xd}" <<'IN'
set -euo pipefail
in=/root/xdeploy
mkdir -p /opt/mobilegl/lib /opt/mobilegl/share/glvnd/egl_vendor.d
cp "$in/libMobileGL.so" /opt/mobilegl/lib/libMobileGL.so.new
mv -f /opt/mobilegl/lib/libMobileGL.so.new /opt/mobilegl/lib/libMobileGL.so
for lib in libEGL.so libEGL.so.1 libGL.so libGL.so.1; do ln -sf libMobileGL.so "/opt/mobilegl/lib/$lib"; done
ln -sf /opt/mobilegl/lib/libMobileGL.so /usr/lib/libGLX_mobilegl.so.0
printf '%s\n' '{"file_format_version":"1.0.0","ICD":{"library_path":"/opt/mobilegl/lib/libMobileGL.so"}}' >/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json
mkdir -p /usr/lib/gbm
cp "$in/mobilegl_gbm.so" /usr/lib/gbm/mobilegl_gbm.so.new
mv -f /usr/lib/gbm/mobilegl_gbm.so.new /usr/lib/gbm/mobilegl_gbm.so
rm -f "$in/libMobileGL.so" "$in/mobilegl_gbm.so"
sha256sum /opt/mobilegl/lib/libMobileGL.so /usr/lib/gbm/mobilegl_gbm.so
echo DEPLOYED
IN
