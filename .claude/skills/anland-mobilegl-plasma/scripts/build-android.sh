#!/usr/bin/env bash
# build-android.sh - build the Android SERVER libMobileGL.so (NDK, arm64-v8a) that the anland APK embeds.
# Output: $MGL_WORKTREE/build-android/libMobileGL.so (RelWithDebInfo, unstripped, ~320 MB; keep it for
# llvm-addr2line, it is what cyc.sh pushes).  Stamp = worktree HEAD, so the container client
# (xbuild.sh, same stamp) and the server agree on the build ("compatible wire with different build"
# WARN otherwise).  Env: MGL_WORKTREE, ANDROID_NDK, BUILD_DIR (default build-android), JOBS (12).
# Log level is compiled to INFO.
set -euo pipefail
. "$(dirname "$0")/env.sh"
W=$MGL_WORKTREE
B=$W/${BUILD_DIR:-build-android}
STAMP=$(git -C "$W" rev-parse HEAD)
if [ ! -f "$B/build.ninja" ]; then
cmake -S "$W" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON \
  -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF \
  -DCMAKE_CXX_FLAGS=-DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO \
  -DMOBILEGL_BUILD_STAMP="$STAMP"
else
cmake "$B" -DMOBILEGL_BUILD_STAMP="$STAMP" >/dev/null
fi
cmake --build "$B" --target MobileGL -j"${JOBS:-12}"
ls -la "$B/libMobileGL.so"
