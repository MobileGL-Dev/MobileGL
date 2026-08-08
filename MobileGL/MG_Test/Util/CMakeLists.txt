cmake_minimum_required(VERSION 3.14)

add_executable(
        JobNodeTest
        JobNodeTest.cpp
)

target_include_directories(JobNodeTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        JobNodeTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(JobNodeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
