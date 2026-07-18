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