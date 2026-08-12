cmake_minimum_required(VERSION 3.24)

add_executable(
        UnorderedMapBench
        UnorderedMapBench.cpp
)

target_include_directories(UnorderedMapBench PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        UnorderedMapBench PRIVATE
        benchmark::benchmark
        ${LINK_LIBRARIES}
)

add_test(NAME UnorderedMapBench COMMAND UnorderedMapBench --benchmark_counters_tabular=true)
set_tests_properties(UnorderedMapBench PROPERTIES LABELS benchmark)
