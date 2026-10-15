# MobileGL - tools/device_bench/p5f/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.24)
project(P5fDevicePixels LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
foreach(required MOBILEGL_SOURCE MOBILEGL_LIBRARY GTEST_SOURCE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Set -D${required}=...")
    endif()
endforeach()
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
add_subdirectory("${GTEST_SOURCE}" googletest EXCLUDE_FROM_ALL)
add_library(DeviceMobileGL SHARED IMPORTED)
set_target_properties(DeviceMobileGL PROPERTIES IMPORTED_LOCATION "${MOBILEGL_LIBRARY}")
set(itest "${MOBILEGL_SOURCE}/MobileGL/MG_IntegrationTest")
add_executable(P5fDevicePixels
    "${itest}/Main.cpp"
    "${itest}/Harness/HeadlessGL.cpp"
    "${itest}/Harness/SplitRuntimePeek.cpp"
    "${itest}/Scenarios/ClipDistanceScenario.cpp"
    "${itest}/Scenarios/CopyImageLayeredScenario.cpp"
    P5fPublicMipmapScenario.cpp)
target_include_directories(P5fDevicePixels PRIVATE
    "${MOBILEGL_SOURCE}/include" "${MOBILEGL_SOURCE}/MobileGL" "${itest}")
target_link_libraries(P5fDevicePixels PRIVATE DeviceMobileGL GTest::gtest)
if(ANDROID)
    target_link_libraries(P5fDevicePixels PRIVATE android log mediandk)
endif()
