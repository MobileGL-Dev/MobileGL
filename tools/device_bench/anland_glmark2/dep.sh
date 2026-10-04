#!/usr/bin/env bash
# dep.sh <DirectGLES|DirectVulkan> - (host) deploy a server (SERVER_LIB) + client (MGL_XBUILD_DIR) build and restart the desktop on that backend
S=${SKILL_SCRIPTS:-$(cd "$(dirname "$0")/../../../.claude/skills/anland-mobilegl-plasma/scripts" && pwd)}
# SERVER_LIB: the NDK-built server lib (Windows form path); MGL_XBUILD_DIR: the WSL client build dir.
export MSYS_NO_PATHCONV=1
adb push "${SERVER_LIB:?set SERVER_LIB}" /data/local/tmp/anland-mobilegl/libMobileGL.new.so >/dev/null
CHANNEL=pbxd bash $S/xdeploy.sh 2>&1 | grep -E "DEPLOYED|rror"
adb shell "su -c 'sh /data/local/tmp/anl/swap.sh >/dev/null; am startservice -n com.anland.consumer.mobilegl/com.anland.consumer.MobileGLWorker -a com.anland.consumer.mobilegl.STOP >/dev/null; sleep 6; am force-stop com.anland.consumer.mobilegl; sleep 1; am start -n com.anland.consumer.mobilegl/com.anland.consumer.MainActivity --es mobilegl_backend $1 >/dev/null'"
sleep 40
printf '%s\n' 'sleep 2; pkill -x systemsettings; true' | bash $S/ct.sh pb >/dev/null 2>&1
adb shell 'su -c "sh /data/local/tmp/anl/status.sh 2>&1 | grep -E \"backend:|server|desktop-session\""'
