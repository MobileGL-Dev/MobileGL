#!/usr/bin/env bash
# xsysroot/refresh-sysroot.sh - (re)export the cross sysroot from the container into WSL
# ~/sysroots/arch-kde-mgl and install the toolchain file as ~/sysroots/aarch64-arch.cmake.
# Read-only on the container side (it only tars headers/libs into /root and moves the tarball out).
# Run once on a new machine, and again after the container upgrades glibc/gcc/vulkan/libdrm/gbm;
# then `bash xbuild.sh --reconfigure`.  Env: CHANNEL (default xr), WSL_DISTRO.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../env.sh"
cp -f "$here/aarch64-arch.cmake" "$ANL_DIR/aarch64-arch.cmake"
bash "$here/../ct.sh" "${CHANNEL:-xr}" <<'IN'
set -e; cd /
L=$(ls -d usr/lib/*crt*.o usr/lib/lib{c,m,mvec,pthread,dl,rt,util,resolv,anl,stdc++,stdc++fs,stdc++exp,supc++,gcc_s,atomic,vulkan,drm,drm_*,gbm,z}.* usr/lib/lib{c,m}_nonshared* usr/lib/ld-linux* usr/lib/pkgconfig usr/share/pkgconfig usr/share/cmake/VulkanHeaders usr/lib/cmake/Vulkan* 2>/dev/null)
tar czf /root/xsysroot.tar.gz --exclude='usr/lib/gcc/*/*/cc1' --exclude='usr/lib/gcc/*/*/cc1plus' --exclude='usr/lib/gcc/*/*/lto1' --exclude='usr/lib/gcc/*/*/lto-wrapper' --exclude='usr/lib/gcc/*/*/collect2' --exclude='usr/lib/gcc/*/*/plugin' --exclude='usr/lib/gcc/*/*/install-tools' --exclude='usr/lib/gcc/*/*/liblto_plugin.so' lib usr/include usr/lib/gcc $L
echo SYSROOT_TAR_DONE
IN
adb_ shell "su -c 'mv $CT_ROOT/root/xsysroot.tar.gz $DEV_TOOLS/xsysroot.tar.gz && chmod 644 $DEV_TOOLS/xsysroot.tar.gz'"
adb_ pull "$DEV_TOOLS/xsysroot.tar.gz" "$ANL_DIR/xsysroot.tar.gz"
adb_ shell "su -c 'rm -f $DEV_TOOLS/xsysroot.tar.gz'"
wsl -d "${WSL_DISTRO:-archlinux}" -- env A="$(win2wsl "$ANL_DIR")" bash -s <<'IN' 2>&1 | tr -d '\0' | grep -av '^wsl:'
set -e; S=$HOME/sysroots/arch-kde-mgl; rm -rf "$S.new"; mkdir -p "$S.new"
tar xzf "$A/xsysroot.tar.gz" -C "$S.new"
rm -rf "$S"; mv "$S.new" "$S"; cp -f "$A/aarch64-arch.cmake" "$HOME/sysroots/aarch64-arch.cmake"
du -sh "$S"; ls "$S/usr/lib/gcc/aarch64-unknown-linux-gnu/"
IN
rm -f "$ANL_DIR/xsysroot.tar.gz"
