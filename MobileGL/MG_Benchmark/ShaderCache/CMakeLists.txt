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
