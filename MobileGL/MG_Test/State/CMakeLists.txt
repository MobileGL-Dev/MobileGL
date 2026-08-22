cmake_minimum_required(VERSION 3.14)

add_executable(
        DebugTest
        DebugTest.cpp
)

target_include_directories(DebugTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        DebugTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(DebugTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

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

add_executable(
        RenderStateTest
        RenderStateTest.cpp
)

target_include_directories(RenderStateTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        RenderStateTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(RenderStateTest PRIVATE /Zc:preprocessor)
endif()

gtest_discover_tests(RenderStateTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

add_executable(
        NegativeApiErrorsTest
        NegativeApiErrorsTest.cpp
)

target_include_directories(NegativeApiErrorsTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        NegativeApiErrorsTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(NegativeApiErrorsTest PRIVATE /Zc:preprocessor)
endif()

gtest_discover_tests(NegativeApiErrorsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
