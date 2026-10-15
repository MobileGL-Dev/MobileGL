# MobileGL - MobileGL/MG_Test/VertexArray/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.14)

add_executable(
        VertexArrayTest
        VertexArrayTest.cpp
)

target_include_directories(VertexArrayTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        VertexArrayTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        VertexAttribBindingStateTest
        VertexAttribBindingStateTest.cpp
)

target_include_directories(VertexAttribBindingStateTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        VertexAttribBindingStateTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(VertexArrayTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(VertexAttribBindingStateTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
