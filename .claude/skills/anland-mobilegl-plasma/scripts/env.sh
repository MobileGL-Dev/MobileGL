# env.sh - shared parameters for the host-side (Git Bash) scripts of the anland-mobilegl-plasma skill.
# Sourced, never run. Every value can be overridden from the environment before calling a script.
#
#   MGL_WORKTREE    MobileGL worktree (Windows form C:/...). Default: the worktree this skill lives in.
#   ANL_DIR         Local scratch dir: channel files, screenshots, xdist/. Default: %LOCALAPPDATA%/Temp/anl
#   ANDROID_NDK     NDK used for the Android server lib. Default: <SDK>/ndk/27.2.12479018
#   MGL_PKG         Anland APK that embeds the server. Default: com.anland.consumer.mobilegl
#   MGL_CONTAINER   Droidspaces container. Default: arch-kde-mgl
#   MGL_DESKTOP_USER  Desktop user inside the container. Default: swung0x48
#   DS_BIN          droidspaces binary on the phone. Default: /data/local/Droidspaces/bin/droidspaces
#   DEV_TOOLS       Phone dir for device/*.sh and channel files. Default: /data/local/tmp/anl
#   DEV_SHARE       Phone dir bind-mounted into the container as /run/anland-mobilegl (screenshots,
#                   server lib staging, display daemon socket). Default: /data/local/tmp/anland-mobilegl
#   ANDROID_SERIAL  Honoured by adb itself; leave unset when only one device is attached
#                   (USB serial or a wireless "ip:port").

_skill_scripts=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ -z "${MGL_WORKTREE:-}" ]; then
    MGL_WORKTREE=$(cd "$_skill_scripts/../../../.." && pwd -W 2>/dev/null || pwd)
fi
: "${ANL_DIR:=$(cygpath -m "${LOCALAPPDATA:-C:/Users/$USERNAME/AppData/Local}" 2>/dev/null || echo /tmp)/Temp/anl}"
: "${ANDROID_NDK:=$(cygpath -m "${LOCALAPPDATA:-C:/Users/$USERNAME/AppData/Local}" 2>/dev/null)/Android/Sdk/ndk/27.2.12479018}"
: "${MGL_PKG:=com.anland.consumer.mobilegl}"
: "${MGL_CONTAINER:=arch-kde-mgl}"
: "${MGL_DESKTOP_USER:=swung0x48}"
: "${DS_BIN:=/data/local/Droidspaces/bin/droidspaces}"
: "${DEV_TOOLS:=/data/local/tmp/anl}"
: "${DEV_SHARE:=/data/local/tmp/anland-mobilegl}"
# The container rootfs as Android root sees it (the container's /tmp is NOT visible here).
CT_ROOT=/mnt/Droidspaces/$MGL_CONTAINER
export MGL_WORKTREE ANL_DIR ANDROID_NDK MGL_PKG MGL_CONTAINER MGL_DESKTOP_USER DS_BIN DEV_TOOLS DEV_SHARE CT_ROOT

# Git Bash would rewrite /data/... arguments into C:/Program Files/Git/data/...; keep them literal.
# Consequence: pass LOCAL paths to Windows tools (adb, cmake, git) in C:/ form, never /c/.
export MSYS_NO_PATHCONV=1
adb_() { MSYS2_ARG_CONV_EXCL='*' adb "$@"; }

# C:/x/y -> /mnt/c/x/y (for WSL).
win2wsl() { local p=$1 d; d=$(printf '%s' "${p:0:1}" | tr 'A-Z' 'a-z'); printf '/mnt/%s%s' "$d" "${p:2}"; }

mkdir -p "$ANL_DIR"
