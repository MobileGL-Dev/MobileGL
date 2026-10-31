# MobileGL - MobileGL/MG_Test/EGLState/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.14)

add_executable(
        EGLStateTest
        EGLStateTest.cpp
        FrameThrottleTest.cpp
)

target_include_directories(EGLStateTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        EGLStateTest PRIVATE
        MobileGLTestMain
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(EGLStateTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
