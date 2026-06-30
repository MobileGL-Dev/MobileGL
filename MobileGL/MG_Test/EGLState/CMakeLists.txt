cmake_minimum_required(VERSION 3.14)

add_executable(
        EGLStateTest
        EGLStateTest.cpp
)

target_include_directories(EGLStateTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        EGLStateTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(EGLStateTest DISCOVERY_TIMEOUT 30)
