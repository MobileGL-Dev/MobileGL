cmake_minimum_required(VERSION 3.14)

add_executable(
        TextureTest
        TextureTest.cpp
)

target_include_directories(TextureTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        TextureTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(TextureTest DISCOVERY_TIMEOUT 30)

add_executable(
        VkClearManagerTest
        VkClearManagerTest.cpp
)

target_include_directories(VkClearManagerTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        VkClearManagerTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

gtest_discover_tests(VkClearManagerTest DISCOVERY_TIMEOUT 30)
