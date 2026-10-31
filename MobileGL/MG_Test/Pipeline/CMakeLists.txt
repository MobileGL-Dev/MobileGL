# MobileGL - MobileGL/MG_Test/Pipeline/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.14)

add_executable(
        PipelineQuirkTest
        PipelineQuirkTest.cpp
        PassthroughTessControlTest.cpp
        ViewportIndexReflectionTest.cpp
        DriverModuleScreenTest.cpp
        NarrowIntegerVertexFormatsTest.cpp
)

target_include_directories(PipelineQuirkTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        PipelineQuirkTest PRIVATE
        MobileGLTestMain
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(PipelineQuirkTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(PipelineQuirkTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
