#!/usr/bin/env bash
# Build only. No adb or device operation. Run after the final integrated SHA exists.
# Usage: bash build_android.sh /home/swung/w7/p5f-int <full-final-sha>
set -euo pipefail
TREE=$(realpath "${1:?source tree}")
HEAD_SHA=${2:?full final SHA}
[[ $HEAD_SHA =~ ^[0-9a-f]{40}$ ]] || exit 64
[[ $(git -C "$TREE" rev-parse HEAD) == "$HEAD_SHA" ]] || { echo 'source head mismatch'; exit 65; }
git -C "$TREE" diff --quiet --ignore-submodules=all HEAD -- MobileGL CMakeLists.txt
PREP=$(cd "$(dirname "$0")" && pwd)
OUT="$HOME/w7/p5f-android-${HEAD_SHA:0:12}"
NDK="$HOME/android-sdk/ndk/27.3.13750724"
GTEST="$TREE/build-split/_deps/googletest-src"
[[ -f "$GTEST/CMakeLists.txt" && -f "$NDK/build/cmake/android.toolchain.cmake" ]] || exit 66
mkdir -p "$OUT"
COMMON=(-G Ninja "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
cmake -S "$TREE" -B "$OUT/library" "${COMMON[@]}" \
    -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF \
    -DMOBILEGL_BUILD_INTEGRATION_TEST=OFF -DMOBILEGL_BUILD_DISAGGREGATED=ON \
    -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_PIPE_PUSH=ON \
    -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO > "$OUT/configure-library.log" 2>&1
grep -q -- '-DMOBILEGL_BUILD_DISAGGREGATED=1' "$OUT/library/compile_commands.json"
grep -q -- '-DMOBILEGL_PIPE_PUSH=1' "$OUT/library/compile_commands.json"
cmake --build "$OUT/library" --target MobileGL -j "${P5F_ANDROID_JOBS:-4}" > "$OUT/build-library.log" 2>&1
cmake -S "$PREP" -B "$OUT/pixels" "${COMMON[@]}" \
    "-DMOBILEGL_SOURCE=$TREE" "-DMOBILEGL_LIBRARY=$OUT/library/libMobileGL.so" \
    "-DGTEST_SOURCE=$GTEST" > "$OUT/configure-pixels.log" 2>&1
cmake --build "$OUT/pixels" --target P5fDevicePixels -j "${P5F_ANDROID_JOBS:-4}" > "$OUT/build-pixels.log" 2>&1
mkdir -p "$OUT/bundle"
cp "$OUT/library/libMobileGL.so" "$OUT/pixels/P5fDevicePixels" "$OUT/bundle/"
cp "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$OUT/bundle/"
# Keep unstripped originals in library/pixels; deploy only a debug-stripped copy.
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug \
    "$OUT/bundle/libMobileGL.so" "$OUT/bundle/P5fDevicePixels"
printf '%s\n' "$HEAD_SHA" > "$OUT/bundle/source-head.txt"
file "$OUT/bundle/P5fDevicePixels" "$OUT/bundle/libMobileGL.so" > "$OUT/bundle/elf-identity.txt"
(cd "$OUT/bundle" && sha256sum ./*.so ./P5fDevicePixels > sha256.txt)
echo "Prepared $OUT/bundle (not deployed)"
