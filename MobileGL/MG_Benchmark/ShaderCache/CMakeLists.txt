# MobileGL - MobileGL/MG_Benchmark/ShaderCache/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.24)

add_executable(
        TranslationCacheBench
        TranslationCacheBench.cpp
)

target_include_directories(TranslationCacheBench PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        TranslationCacheBench PRIVATE
        benchmark::benchmark
        ${LINK_LIBRARIES}
)

add_test(NAME TranslationCacheBench COMMAND TranslationCacheBench --benchmark_counters_tabular=true)
set_tests_properties(TranslationCacheBench PROPERTIES LABELS benchmark)
