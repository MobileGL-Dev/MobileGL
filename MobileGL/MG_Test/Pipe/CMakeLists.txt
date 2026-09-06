cmake_minimum_required(VERSION 3.14)

add_executable(
        PipeCatalogueTest
        PipeCatalogueTest.cpp
)

target_include_directories(PipeCatalogueTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/MobileGL/MG_Pipe
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        PipeCatalogueTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(PipeCatalogueTest PRIVATE /Zc:preprocessor)
endif()

# The P1 poison and verify shapes at the block level: a fake GLContext, the real filler and
# accessors out of the static library. Links gtest (not gtest_main): the suite needs its own
# main() to point MOBILEGL_LOG_FILE_PATH at a temp file before anything logs, because its
# abort cases read the Fatal line back out of that file. Every case is a visible SKIP in a
# pull build.
add_executable(
        PipeInputsTest
        PipeInputsTest.cpp
)

target_include_directories(PipeInputsTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/MobileGL/MG_Pipe
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        PipeInputsTest PRIVATE
        GTest::gtest
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(PipeInputsTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(PipeCatalogueTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(PipeInputsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
