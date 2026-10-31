# MobileGL - MobileGL/MG_Benchmark/Buffer/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.24)

add_executable(
        BufferBench
        BufferBench.cpp
)

target_include_directories(BufferBench PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        BufferBench PRIVATE
        benchmark::benchmark
        ${LINK_LIBRARIES}
)

add_test(NAME BufferBench COMMAND BufferBench --benchmark_counters_tabular=true)
set_tests_properties(BufferBench PROPERTIES LABELS benchmark)