cmake_minimum_required(VERSION 3.24)

add_executable(
        ProgramBench
        ProgramBench.cpp
)

target_include_directories(ProgramBench PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ProgramBench PRIVATE
        benchmark::benchmark
        ${LINK_LIBRARIES}
)

add_test(NAME ProgramBench COMMAND ProgramBench --benchmark_counters_tabular=true)
set_tests_properties(ProgramBench PROPERTIES LABELS benchmark)