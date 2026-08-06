#!/bin/bash
# Run the headless EGL DriverBench on one renderer:
#   ./run_driver_bench.sh native            [bench args...]
#   ./run_driver_bench.sh espryt <libMobileGL.so> [bench args...]
#   ./run_driver_bench.sh magma  <libMobileGL.so> [bench args...]
# The bench dlopens exactly one EGL provider (DRIVERBENCH_EGL_LIB): the system
# libEGL.so.1 for native, or the given libMobileGL.so for a MobileGL backend -
# no LD_LIBRARY_PATH shadowing, so MobileGL's own loader still finds the real
# driver underneath.
#
# Pin the vendor libraries explicitly. A bare libEGL.so.1 on a glvnd system
# picks whatever vendor eglGetDisplay(EGL_DEFAULT_DISPLAY) resolves first,
# which is Mesa/llvmpipe here - a software rasteriser silently replacing the
# GPU under a benchmark. Override MGL_EGL_VENDOR / MGL_VK_ICD to test another
# driver.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=${DRIVERBENCH_BIN:-$HERE/DriverBench}
EGL_VENDOR=${MGL_EGL_VENDOR:-/usr/share/glvnd/egl_vendor.d/10_nvidia.json}
VK_ICD=${MGL_VK_ICD:-/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json}
MODE=$1; shift

export __EGL_VENDOR_LIBRARY_FILENAMES=$EGL_VENDOR
export EGL_PLATFORM=${EGL_PLATFORM:-x11}

case "$MODE" in
  native)
    export DRIVERBENCH_EGL_LIB=${DRIVERBENCH_EGL_LIB:-libEGL.so.1}
    ;;
  espryt)
    export DRIVERBENCH_EGL_LIB=$(readlink -f "$1"); shift
    export MOBILEGL_BACKEND_TYPE=DirectGLES
    ;;
  magma)
    export DRIVERBENCH_EGL_LIB=$(readlink -f "$1"); shift
    export MOBILEGL_BACKEND_TYPE=DirectVulkan
    export VK_ICD_FILENAMES=$VK_ICD
    ;;
  *) echo "unknown mode: $MODE (native|espryt|magma)"; exit 1 ;;
esac
exec "$BENCH" "$@"
