cmake_minimum_required(VERSION 3.24)
project(P5fDevicePixels LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
foreach(required MOBILEGL_SOURCE MOBILEGL_LIBRARY GTEST_SOURCE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Set -D${required}=...")
    endif()
endforeach()
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
add_subdirectory("${GTEST_SOURCE}" googletest EXCLUDE_FROM_ALL)
add_library(DeviceMobileGL SHARED IMPORTED)
set_target_properties(DeviceMobileGL PROPERTIES IMPORTED_LOCATION "${MOBILEGL_LIBRARY}")
set(itest "${MOBILEGL_SOURCE}/MobileGL/MG_IntegrationTest")
add_executable(P5fDevicePixels
    "${itest}/Main.cpp"
    "${itest}/Harness/HeadlessGL.cpp"
    "${itest}/Harness/SplitRuntimePeek.cpp"
    "${itest}/Scenarios/ClipDistanceScenario.cpp"
    "${itest}/Scenarios/CopyImageLayeredScenario.cpp"
    P5fPublicMipmapScenario.cpp)
target_include_directories(P5fDevicePixels PRIVATE
    "${MOBILEGL_SOURCE}/include" "${MOBILEGL_SOURCE}/MobileGL" "${itest}")
target_link_libraries(P5fDevicePixels PRIVATE DeviceMobileGL GTest::gtest)
if(ANDROID)
    target_link_libraries(P5fDevicePixels PRIVATE android log mediandk)
endif()
