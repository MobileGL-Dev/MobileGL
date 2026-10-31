# MobileGL - tools/anland-mobilegl-plasma/scripts/device/swap.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# swap.sh - (Android root shell) replace the server libMobileGL.so inside the INSTALLED anland APK with
# $SHARE/libMobileGL.new.so, keeping owner, mode and SELinux context; force-stops the app.  The first
# run backs the APK's original lib up to $SHARE/libMobileGL.so.orig.  No re-signing/reinstall (that
# would change the app UID and lose its root grant).  Run: su -c "sh /data/local/tmp/anl/swap.sh"
# Env: PKG (com.anland.consumer.mobilegl), SHARE (/data/local/tmp/anland-mobilegl).
set -e
P=${PKG:-com.anland.consumer.mobilegl}
SHARE=${SHARE:-/data/local/tmp/anland-mobilegl}
D=$(dirname $(pm path $P | sed 's/package://'))/lib/arm64
ls -laZ $D
[ -f $SHARE/libMobileGL.so.orig ] || cp $D/libMobileGL.so $SHARE/libMobileGL.so.orig
am force-stop $P
CTX=$(ls -Z $D/libMobileGL.so | awk '{print $1}')
OWN=$(stat -c %u:%g $D/libMobileGL.so)
cp $SHARE/libMobileGL.new.so $D/libMobileGL.so
chown $OWN $D/libMobileGL.so; chmod 755 $D/libMobileGL.so; chcon $CTX $D/libMobileGL.so
ls -laZ $D/libMobileGL.so
