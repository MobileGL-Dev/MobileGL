# MobileGL - MobileGL/MG_Test/Backend/DirectGLES/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.14)

add_executable(
        EsslShaderPassTest
        EsslShaderPassTest.cpp
)

target_include_directories(EsslShaderPassTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        EsslShaderPassTest PRIVATE
        MobileGLTestMain
        ${LINK_LIBRARIES}
)

add_executable(
        BaseInstanceInjectionTest
        BaseInstanceInjectionTest.cpp
)

target_include_directories(BaseInstanceInjectionTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        BaseInstanceInjectionTest PRIVATE
        MobileGLTestMain
        ${LINK_LIBRARIES}
)

add_executable(
        ViewportIndexRoutingTest
        ViewportIndexRoutingTest.cpp
)

target_include_directories(ViewportIndexRoutingTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ViewportIndexRoutingTest PRIVATE
        MobileGLTestMain
        ${LINK_LIBRARIES}
)

include(GoogleTest)
add_executable(CapabilityLimitsTest CapabilityLimitsTest.cpp)
target_include_directories(CapabilityLimitsTest PRIVATE ${MGL_ROOT}/include ${MGL_ROOT}/MobileGL)
target_link_libraries(CapabilityLimitsTest PRIVATE MobileGLTestMain ${LINK_LIBRARIES})
gtest_discover_tests(CapabilityLimitsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

add_executable(ContextEpochTest ContextEpochTest.cpp)
target_include_directories(ContextEpochTest PRIVATE ${MGL_ROOT}/include ${MGL_ROOT}/MobileGL)
target_link_libraries(ContextEpochTest PRIVATE MobileGLTestMain ${LINK_LIBRARIES})
gtest_discover_tests(ContextEpochTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

# P3b/P4b R-5: D-K2's dependency rule had six statements and nothing compared them. This binary
# is where the comparison can live, because it is the one that links BOTH the client check
# (MG_Impl/Pipe, through MG_Pipe) and the server gate (MG_Backend/DirectGLES's resolvers).
add_executable(SubsystemDepsTest SubsystemDepsTest.cpp)
target_include_directories(SubsystemDepsTest PRIVATE ${MGL_ROOT}/include ${MGL_ROOT}/MobileGL)
target_link_libraries(SubsystemDepsTest PRIVATE MobileGLTestMain ${LINK_LIBRARIES})
gtest_discover_tests(SubsystemDepsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(EsslShaderPassTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(BaseInstanceInjectionTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(ViewportIndexRoutingTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
