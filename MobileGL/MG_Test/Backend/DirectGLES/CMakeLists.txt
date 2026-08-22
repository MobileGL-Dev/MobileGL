cmake_minimum_required(VERSION 3.14)

add_executable(
        EsslShaderPassTest
        EsslShaderPassTest.cpp
)

target_include_directories(EsslShaderPassTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        EsslShaderPassTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        BaseInstanceInjectionTest
        BaseInstanceInjectionTest.cpp
)

target_include_directories(BaseInstanceInjectionTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        BaseInstanceInjectionTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        ViewportIndexRoutingTest
        ViewportIndexRoutingTest.cpp
)

target_include_directories(ViewportIndexRoutingTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ViewportIndexRoutingTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(EsslShaderPassTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(BaseInstanceInjectionTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(ViewportIndexRoutingTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
