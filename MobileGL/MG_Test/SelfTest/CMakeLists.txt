# MobileGL - MobileGL/MG_Test/SelfTest/CMakeLists.txt

add_executable(
        DriverPostIterationRPWitnessTest
        DriverPostIterationRPWitnessTest.cpp
)

target_include_directories(DriverPostIterationRPWitnessTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(DriverPostIterationRPWitnessTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        DriverBugProbesTest
        DriverBugProbesTest.cpp
)

target_include_directories(DriverBugProbesTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(DriverBugProbesTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        PrimitivesGeneratedNoXfbProbeTest
        PrimitivesGeneratedNoXfbProbeTest.cpp
)

target_include_directories(PrimitivesGeneratedNoXfbProbeTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(PrimitivesGeneratedNoXfbProbeTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(DriverPostIterationRPWitnessTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(DriverBugProbesTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(PrimitivesGeneratedNoXfbProbeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
