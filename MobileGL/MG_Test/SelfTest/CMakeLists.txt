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

include(GoogleTest)
gtest_discover_tests(DriverPostIterationRPWitnessTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
