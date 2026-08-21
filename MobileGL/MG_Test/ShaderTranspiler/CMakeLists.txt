cmake_minimum_required(VERSION 3.14)

add_executable(
        SpirvPassTest
        SpirvPassTest.cpp
        DeriveNumSubgroupsTest.cpp
        FixIterationRPBarrierTest.cpp
        FixIterationRPSubgroupScratchTest.cpp
        EmulateSubgroupsTest.cpp
        DemoteFloat64Test.cpp
        FlattenXfbInterfaceBlocksTest.cpp
        UniquifyIoBlockNamesTest.cpp
        LowerViewportIndexTest.cpp
        ClampMultisampleFetchTest.cpp
        LegalizeResourceArrayIndexTest.cpp
        FlattenAtomicCounterBlockTest.cpp
        WidenImageFormatsTest.cpp
        GlslangCaptureTest.cpp
)

target_include_directories(SpirvPassTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        SpirvPassTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(SpirvPassTest PRIVATE /Zc:preprocessor)
endif()

# Its own binary: several cases flip MG_Config::Features.ShaderTranslationCache and
# MG_Config::Features.AsyncShaderCompile and read the PROCESS-GLOBAL L1 counters straight
# afterwards, so another suite's links running in the same process would show up as
# hits/misses this one did not cause.
add_executable(
        TranslationCacheTest
        TranslationCacheTest.cpp
)

target_include_directories(TranslationCacheTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        TranslationCacheTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(TranslationCacheTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(SpirvPassTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
# Heavier than a pure-pass suite: the concurrency cases run 8 threads over thousands of
# rounds and the end-to-end cases link real programs through the compile pool.
gtest_discover_tests(TranslationCacheTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
