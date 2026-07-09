cmake_minimum_required(VERSION 3.14)

add_executable(
        BackendLoaderTest
        BackendLoaderTest.cpp
)

target_include_directories(BackendLoaderTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        BackendLoaderTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(BackendLoaderTest DISCOVERY_TIMEOUT 30)
