cmake_minimum_required(VERSION 3.14)

add_executable(
        BufferTest
        BufferTest.cpp
)

target_include_directories(BufferTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        BufferTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(BufferTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(BufferTest DISCOVERY_TIMEOUT 30)
