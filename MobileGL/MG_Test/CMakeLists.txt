cmake_minimum_required(VERSION 3.24)
project(MobileGLTest)

message(STATUS "Generating build files for MobileGL Test...")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MGL_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# # Add the directory containing our custom Find modules
# list(APPEND CMAKE_MODULE_PATH "${MGL_ROOT}/buildsystem/cmake")

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# # Find the required libraries using our custom Find modules
# find_package(glslang REQUIRED)
# find_package(SPIRVCross REQUIRED)
# find_package(SPIRVTools)

add_executable(
  SanityTest
  SanityTest.cpp
)
target_link_libraries(
  SanityTest
  GTest::gtest_main
  MobileGL_s
)

target_include_directories(SanityTest PRIVATE
  ${MGL_ROOT}/include
  ${MGL_ROOT}/MobileGL
)

# message(STATUS "glslang_LIBRARIES: ${glslang_LIBRARIES}")
# message(STATUS "SPIRVTools_LIBRARIES: ${SPIRVTools_LIBRARIES}")
# message(STATUS "SPIRVCross_LIBRARIES: ${SPIRVCross_LIBRARIES}")

# # Set the libraries to link against
# set(LINK_LIBRARIES 
#     ${glslang_LIBRARIES}
#     ${SPIRVCross_LIBRARIES}
# )

# if (SPIRVTools_FOUND)
#   list(APPEND LINK_LIBRARIES ${SPIRVTools_LIBRARIES})
# endif()

set(LINK_LIBRARIES 
    MobileGL_s
)

include(GoogleTest)
gtest_discover_tests(SanityTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

add_subdirectory(BackendLoader)
add_subdirectory(Buffer)
# The heap-address-is-not-an-identity invariant the backends' per-object memos
# rest on. No GL context, no driver: it only needs the allocator.
add_subdirectory(State)
add_subdirectory(EGLState)
add_subdirectory(Framebuffer)
add_subdirectory(Texture)
add_subdirectory(VertexArray)
add_subdirectory(Program)
add_subdirectory(Query)
add_subdirectory(Pipeline)
add_subdirectory(ShaderTranspiler)
add_subdirectory(Util)
# The DirectGLES post-transpile ESSL passes are pure String -> String, so unlike the
# DirectVulkan suite below this one needs no device and always builds.
add_subdirectory(Backend/DirectGLES)
if (ENABLE_INTEGRATION_TESTS)
    add_subdirectory(Backend/DirectVulkan)
endif()
