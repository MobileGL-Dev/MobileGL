cmake_minimum_required(VERSION 3.14)

message(STATUS "Generating build files for MobileGL Diligent Backend Test...")

add_executable(
        DiligentVulkanSanityTest
        SanityTest.cpp
)

target_include_directories(DiligentVulkanSanityTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        DiligentVulkanSanityTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(DiligentVulkanSanityTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS integration)
