#!/usr/bin/env bash
# Windows Git Bash. Root coordinator runs this only AFTER its own clean reboot.
# Does not reboot, install an APK, alter clocks, clear logcat, or launch FCL.
# Usage: bash run_pixel_arm.sh <bundle-dir> <new-output-dir> DirectGLES|DirectVulkan inproc|monolith 0|1
set -euo pipefail
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
BUNDLE=${1:?bundle}; OUT=${2:?new output}; BACKEND=${3:?backend}; TRANSPORT=${4:?transport}; ROLE=${5:?role}
[[ $BACKEND == DirectGLES || $BACKEND == DirectVulkan ]] || exit 64
[[ $TRANSPORT == inproc || $TRANSPORT == monolith ]] || exit 64
[[ $ROLE == 0 || $ROLE == 1 ]] || exit 64
[[ ! -e $OUT ]] || { echo 'Output exists; do not overwrite an arm'; exit 65; }
HEAD_SHA=$(tr -d '\r\n' < "$BUNDLE/source-head.txt")
[[ $HEAD_SHA =~ ^[0-9a-f]{40}$ ]] || exit 66
(cd "$BUNDLE" && sha256sum -c sha256.txt)
ADB_BIN=${ADB_BIN:-/c/Users/geekerwan/AppData/Local/Android/Sdk/platform-tools/adb.exe}
A=("$ADB_BIN" -s 2f7cbe2e)
[[ $("${A[@]}" get-state | tr -d '\r') == device ]] || exit 67
mkdir -p "$OUT"
cp "$BUNDLE/source-head.txt" "$BUNDLE/sha256.txt" "$OUT/"
"${A[@]}" shell cat /proc/sys/kernel/random/boot_id > "$OUT/boot-id.txt"
REMOTE=/data/local/tmp/p5f-${HEAD_SHA:0:12}
TAG=$BACKEND-$TRANSPORT-r$ROLE-$(date +%s)
"${A[@]}" shell "mkdir -p $REMOTE"
for f in libMobileGL.so libc++_shared.so P5fDevicePixels; do
    "${A[@]}" push "$BUNDLE/$f" "$REMOTE/$f" > "$OUT/push-$f.log" 2>&1
done
"${A[@]}" shell "chmod 755 $REMOTE/P5fDevicePixels; cd $REMOTE; sha256sum libMobileGL.so libc++_shared.so P5fDevicePixels" > "$OUT/device-sha256.txt"
FILTER='ClipDistanceScenario.AnEnabledClipDistanceRemovesTheNegativeHalf:ClipDistanceScenario.ADisabledClipDistanceRemovesNothing:ClipDistanceScenario.TheEnablesAreIndependentPerDistance:CopyImageLayeredScenario.*:P5fPublicMipmapScenario.*'
printf '%s\n' "backend=$BACKEND transport=$TRANSPORT role=$ROLE" > "$OUT/arm.txt"
rc=0
"${A[@]}" shell "cd $REMOTE; env LD_LIBRARY_PATH=$REMOTE MOBILEGL_BACKEND_TYPE=$BACKEND MOBILEGL_TRANSPORT=$TRANSPORT MOBILEGL_IPC_ROLE_SPLIT_STATE=$ROLE MOBILEGL_IPC_STRICT_ERRORS=1 MOBILEGL_IPC_RUN_AHEAD=1 MOBILEGL_IPC_STAGE_MB=256 MOBILEGL_ITEST_REQUIRE_GPU=1 MOBILEGL_ITEST_REQUIRE_HARDWARE_GPU=1 MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1 MOBILEGL_LOG_FILE_PATH=$REMOTE/$TAG.library.log ./P5fDevicePixels --gtest_filter='$FILTER' --gtest_output=xml:$REMOTE/$TAG.xml >$REMOTE/$TAG.stdout.txt 2>&1" || rc=$?
printf '%s\n' "$rc" > "$OUT/process-exit.txt"
for f in library.log xml stdout.txt; do
    "${A[@]}" pull "$REMOTE/$TAG.$f" "$OUT/$f" > "$OUT/pull-$f.log" 2>&1 || true
done
"${A[@]}" logcat -d -b crash -v time > "$OUT/crash-buffer.txt"
echo "Arm completed with process rc=$rc. Verify XML, library log, boot id, and hashes in $OUT."
exit "$rc"
