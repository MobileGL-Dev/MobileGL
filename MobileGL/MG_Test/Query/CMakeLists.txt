cmake_minimum_required(VERSION 3.14)

add_executable(
        QueryTest
        QueryTest.cpp
)

target_include_directories(QueryTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        QueryTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(QueryTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
