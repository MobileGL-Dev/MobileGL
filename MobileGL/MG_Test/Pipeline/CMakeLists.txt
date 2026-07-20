cmake_minimum_required(VERSION 3.14)

add_executable(
        PipelineQuirkTest
        PipelineQuirkTest.cpp
)

target_include_directories(PipelineQuirkTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        PipelineQuirkTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(PipelineQuirkTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(PipelineQuirkTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
