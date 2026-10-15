# MobileGL - MobileGL/MG_Benchmark/Transpile/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.24)

# Deliberately NOT a google-benchmark target: the interesting quantity is a per-stage
# breakdown of one program build, which needs its own clock around sub-steps that share
# set-up, and a plain main() keeps the output a table this can be read straight out of.
add_executable(
        TranspileProfile
        TranspileProfile.cpp
)

target_include_directories(TranspileProfile PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        TranspileProfile PRIVATE
        ${LINK_LIBRARIES}
)
