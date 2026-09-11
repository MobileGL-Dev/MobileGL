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

# P5 b1: the split buffer side - the client GPU-write set, the persistent-map block push and
# tier 1 of the flush ladder. Built in EVERY configuration on purpose; the cases that need
# MOBILEGL_BUILD_DISAGGREGATED skip elsewhere rather than disappearing, so the ctest name set
# is the same in the pull and the push lanes (G2) and build-split removes none of them (G14).
add_executable(
        SplitBufferTest
        SplitBufferTest.cpp
)

target_include_directories(SplitBufferTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        SplitBufferTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(SplitBufferTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(BufferTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(SplitBufferTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
