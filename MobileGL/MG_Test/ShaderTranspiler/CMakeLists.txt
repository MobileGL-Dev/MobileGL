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
        LowerViewportIndexTest.cpp
        ClampMultisampleFetchTest.cpp
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

include(GoogleTest)
gtest_discover_tests(SpirvPassTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
