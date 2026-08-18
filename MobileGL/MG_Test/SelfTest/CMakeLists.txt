# MobileGL - MobileGL/MG_Test/SelfTest/CMakeLists.txt

add_executable(
        DriverPostProgram203WitnessTest
        DriverPostProgram203WitnessTest.cpp
)

target_include_directories(DriverPostProgram203WitnessTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(DriverPostProgram203WitnessTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(DriverPostProgram203WitnessTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
