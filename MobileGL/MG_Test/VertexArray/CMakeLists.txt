cmake_minimum_required(VERSION 3.14)

add_executable(
        VertexArrayTest
        VertexArrayTest.cpp
)

target_include_directories(VertexArrayTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        VertexArrayTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        VertexAttribBindingStateTest
        VertexAttribBindingStateTest.cpp
)

target_include_directories(VertexAttribBindingStateTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        VertexAttribBindingStateTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(VertexArrayTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(VertexAttribBindingStateTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
