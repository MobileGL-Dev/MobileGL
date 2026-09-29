cmake_minimum_required(VERSION 3.14)

add_executable(
        SwapchainPresentModeTest
        SwapchainPresentModeTest.cpp
)

target_include_directories(SwapchainPresentModeTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
)

target_link_libraries(
        SwapchainPresentModeTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(SwapchainPresentModeTest PRIVATE /Zc:preprocessor)
endif()

include(GoogleTest)
gtest_discover_tests(SwapchainPresentModeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
