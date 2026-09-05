# Standalone NDK command-line probe for MobileGL disaggregation spike B
# (plan-B §8.3 / §11 P0).  Deliberately NOT part of the MobileGL build graph:
# it links nothing from the project and is configured on its own, e.g.
#
#   cmake -S tools/spikes/extmem_probe -B /tmp/extmem-build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo
#
# See build_android.sh for the exact invocation used on the devices.

cmake_minimum_required(VERSION 3.16)
project(extmem_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(extmem_probe extmem_probe.cpp)

# PIE is the NDK default for executables; make it explicit so a stale toolchain
# cannot produce something Android's linker refuses to run.
target_compile_options(extmem_probe PRIVATE -fPIE -Wall -Wextra -Wno-unused-parameter)
target_link_options(extmem_probe PRIVATE -pie)

if(ANDROID)
  target_link_libraries(extmem_probe PRIVATE vulkan EGL GLESv3 android log)
else()
  # Host build: T0 (AHardwareBuffer) compiles out, T1/T3 stay. Its only purpose is
  # to validate this harness against a driver that is known to implement the
  # routes (lavapipe), so that a device-side FAIL is attributable to the device
  # driver and not to the probe. It is NOT a substitute for a device run.
  message(STATUS "extmem_probe: host build -- T1/T3 harness validation only, T0 disabled")
  target_link_libraries(extmem_probe PRIVATE vulkan EGL GLESv2)
endif()
