cmake_minimum_required(VERSION 3.14)

message(STATUS "Generating build files for MobileGL DirectVulkan Integration Test...")

if (APPLE)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
endif()

add_executable(
        DirectVulkanSanityTest
        SanityTest.cpp
)

target_include_directories(DirectVulkanSanityTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

if (WIN32)
    # I know this is horrible, but glfw only provides binary but not CMake config for Windows, so we have to manually link against the library and include directories
    #set(GLFW_ROOT path/to/glfw-3.4.bin.WIN64)
    target_include_directories(DirectVulkanSanityTest PRIVATE ${GLFW_ROOT}/include)
    target_link_directories(DirectVulkanSanityTest PRIVATE ${GLFW_ROOT}/lib-vc2022)
    target_link_libraries(DirectVulkanSanityTest PRIVATE glfw3_mt.lib) 
else()
    find_package(glfw3 REQUIRED)
    target_link_libraries(DirectVulkanSanityTest PRIVATE glfw)
endif()

find_package(Vulkan REQUIRED)
target_include_directories(DirectVulkanSanityTest PUBLIC ${Vulkan_INCLUDE_DIRS})
target_link_libraries(DirectVulkanSanityTest PRIVATE Vulkan::Vulkan)

target_link_libraries(
        DirectVulkanSanityTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)
if (APPLE)
    target_link_libraries(DirectVulkanSanityTest PRIVATE objc)
    target_link_libraries(DirectVulkanSanityTest PRIVATE ${QUARTZCORE_FRAMEWORK})
endif()
target_compile_definitions(DirectVulkanSanityTest PRIVATE -DNOMINMAX)

include(GoogleTest)
gtest_discover_tests(DirectVulkanSanityTest DISCOVERY_TIMEOUT 30)

add_executable(
        DirectVulkanTestExec
        TestExec.cpp
)

target_include_directories(DirectVulkanTestExec PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

if (WIN32)
    # I know this is horrible, but glfw only provides binary but not CMake config for Windows, so we have to manually link against the library and include directories
    #set(GLFW_ROOT path/to/glfw-3.4.bin.WIN64)
    target_include_directories(DirectVulkanTestExec PRIVATE ${GLFW_ROOT}/include)
    target_link_directories(DirectVulkanTestExec PRIVATE ${GLFW_ROOT}/lib-vc2022)
    target_link_libraries(DirectVulkanTestExec PRIVATE glfw3_mt.lib)
else()
    find_package(glfw3 REQUIRED)
    target_link_libraries(DirectVulkanTestExec PRIVATE glfw)
endif()

find_package(Vulkan REQUIRED)
target_include_directories(DirectVulkanTestExec PUBLIC ${Vulkan_INCLUDE_DIRS})
target_link_libraries(DirectVulkanTestExec PRIVATE Vulkan::Vulkan)

target_link_libraries(
        DirectVulkanTestExec PRIVATE
        ${LINK_LIBRARIES}
)
if (APPLE)
    target_link_libraries(DirectVulkanTestExec PRIVATE objc)
    target_link_libraries(DirectVulkanTestExec PRIVATE ${QUARTZCORE_FRAMEWORK})
endif()
target_compile_definitions(DirectVulkanTestExec PRIVATE -DNOMINMAX)
