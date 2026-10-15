# MobileGL - MobileGL/MG_Test/Swapchain/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.14)

add_executable(
        SwapchainPresentModeTest
        SwapchainPresentModeTest.cpp
)

target_include_directories(SwapchainPresentModeTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
)

target_link_libraries(
        SwapchainPresentModeTest PRIVATE
        MobileGLTestMain
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(SwapchainPresentModeTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(SwapchainPresentModeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
