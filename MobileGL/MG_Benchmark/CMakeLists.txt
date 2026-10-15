# MobileGL - MobileGL/MG_Benchmark/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.14)
project(MobileGLBenchmark)

message(STATUS "Generating build files for MobileGL Benchmark...")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable downloading dependencies
set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "Allow downloading benchmark dependencies")
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "Disable GTest tests")
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable testing")

set(LINK_LIBRARIES 
    MobileGL_s
)

include(FetchContent)
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(benchmark)

enable_testing()

set(MGL_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_executable(SanityBench
  SanityBench.cpp
)

target_link_libraries(
  SanityBench PRIVATE
  benchmark::benchmark
)

add_test(NAME SanityBench COMMAND SanityBench --benchmark_counters_tabular=true)
set_tests_properties(SanityBench PROPERTIES LABELS benchmark)

add_subdirectory(Program)
add_subdirectory(Buffer)
add_subdirectory(Driver)
add_subdirectory(Container)
add_subdirectory(ShaderCache)
add_subdirectory(Transpile)
