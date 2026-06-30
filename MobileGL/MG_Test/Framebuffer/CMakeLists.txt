cmake_minimum_required(VERSION 3.14)

add_executable(
        FramebufferTest
        FramebufferTest.cpp
)

target_include_directories(FramebufferTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        FramebufferTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(FramebufferTest DISCOVERY_TIMEOUT 30)
