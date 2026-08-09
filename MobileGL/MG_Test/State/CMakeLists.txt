cmake_minimum_required(VERSION 3.14)

add_executable(
        ObjectLifetimeIdTest
        ObjectLifetimeIdTest.cpp
)

target_include_directories(ObjectLifetimeIdTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        ObjectLifetimeIdTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(ObjectLifetimeIdTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(ObjectLifetimeIdTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
