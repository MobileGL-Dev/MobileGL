#!/bin/bash
# Run the headless MobileGL integration scenarios on one backend:
#   ./run_integration_test.sh espryt [gtest args...]     -> DirectGLES
#   ./run_integration_test.sh magma  [gtest args...]     -> DirectVulkan
#
# The backend is latched at initialization from MOBILEGL_BACKEND_TYPE, so one
# process is one backend; this script is the dev-box equivalent of the two ctest
# registrations in CMakeLists.txt.
#
# Pin the vendor libraries explicitly, for the same reason
# MG_Benchmark/Driver/run_driver_bench.sh does: a bare libEGL on a glvnd system
# resolves to whatever vendor comes first, which is usually Mesa/llvmpipe - a
# software rasteriser silently replacing the GPU under a GPU test. Override
# MGL_EGL_VENDOR / MGL_VK_ICD to test another driver.
#
# Set MOBILEGL_ITEST_REQUIRE_GPU=1 to turn "the harness is unusable" from a clean
# skip into a failure. Do that anywhere the machine is supposed to have a GPU: a
# run that skipped everything and a run that passed everything are otherwise the
# same green, so without it a broken driver pinning is invisible.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${MOBILEGL_ITEST_BIN:-$HERE/MobileGLIntegrationTest}
EGL_VENDOR=${MGL_EGL_VENDOR:-/usr/share/glvnd/egl_vendor.d/10_nvidia.json}
VK_ICD=${MGL_VK_ICD:-/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json}
MODE=$1; shift

if [ ! -x "$BIN" ]; then
  echo "MobileGLIntegrationTest not found at $BIN"
  echo "configure with -DMOBILEGL_BUILD_INTEGRATION_TEST=ON and set MOBILEGL_ITEST_BIN"
  exit 1
fi

[ -r "$EGL_VENDOR" ] && export __EGL_VENDOR_LIBRARY_FILENAMES=$EGL_VENDOR
export EGL_PLATFORM=${EGL_PLATFORM:-x11}

case "$MODE" in
  espryt|DirectGLES)
    export MOBILEGL_BACKEND_TYPE=DirectGLES
    ;;
  magma|DirectVulkan)
    export MOBILEGL_BACKEND_TYPE=DirectVulkan
    [ -r "$VK_ICD" ] && export VK_ICD_FILENAMES=$VK_ICD
    ;;
  *) echo "unknown mode: $MODE (espryt|magma)"; exit 1 ;;
esac
export MOBILEGL_ITEST_REQUIRE_GPU=${MOBILEGL_ITEST_REQUIRE_GPU:-}
exec "$BIN" "$@"
