#!/bin/bash
# MobileGL - tools/spikes/extmem_probe/build_android.sh
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# Build the spike-B external-memory probe for arm64 Android.
#
#   ANDROID_NDK=/path/to/ndk ./build_android.sh [build-dir]
#
# Produces <build-dir>/extmem_probe, a PIE arm64-v8a executable that depends
# only on the platform (libvulkan / libEGL / libGLESv3 / libandroid / liblog).
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${1:-$HERE/build-android}"
NDK="${ANDROID_NDK:-$HOME/android-sdk/ndk/27.3.13750724}"

if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
  echo "NDK not found at $NDK (set ANDROID_NDK)" >&2
  exit 1
fi

cmake -S "$HERE" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-30 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD" -j "$(nproc)"
echo "built: $BUILD/extmem_probe"
