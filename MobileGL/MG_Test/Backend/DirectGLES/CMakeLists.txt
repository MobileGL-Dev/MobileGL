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
add_executable(ContextEpochTest ContextEpochTest.cpp)
target_include_directories(ContextEpochTest PRIVATE ${MGL_ROOT}/include ${MGL_ROOT}/MobileGL)
target_link_libraries(ContextEpochTest PRIVATE GTest::gtest_main ${LINK_LIBRARIES})
gtest_discover_tests(ContextEpochTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

# P3b/P4b R-5: D-K2's dependency rule had six statements and nothing compared them. This binary
# is where the comparison can live, because it is the one that links BOTH the client check
# (MG_Impl/Pipe, through MG_Pipe) and the server gate (MG_Backend/DirectGLES's resolvers).
add_executable(SubsystemDepsTest SubsystemDepsTest.cpp)
target_include_directories(SubsystemDepsTest PRIVATE ${MGL_ROOT}/include ${MGL_ROOT}/MobileGL)
target_link_libraries(SubsystemDepsTest PRIVATE GTest::gtest_main ${LINK_LIBRARIES})
gtest_discover_tests(SubsystemDepsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(EsslShaderPassTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(BaseInstanceInjectionTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(ViewportIndexRoutingTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
