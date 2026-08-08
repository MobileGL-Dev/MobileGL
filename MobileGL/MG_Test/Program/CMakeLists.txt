cmake_minimum_required(VERSION 3.24)

add_executable(
        ProgramUtilTest
        ProgramUtilTest.cpp

        ${MGL_ROOT}/MobileGL/MG_Util/Converters/GLToStr/GLEnumConverter.cpp
        ${MGL_ROOT}/MobileGL/MG_Util/Converters/GLToGlslang/ProgramEnumConverter.cpp
        ${MGL_ROOT}/MobileGL/MG_Util/ShaderTranspiler/ShaderCompiler.cpp
        ${MGL_ROOT}/MobileGL/MG_Util/ShaderTranspiler/SpvcSession.cpp
        ${MGL_ROOT}/MobileGL/MG_Util/ShaderTranspiler/glslang/UniformTraverser.cpp
        ${MGL_ROOT}/MobileGL/MG_State/GLState/ProgramState/ShaderPreprocessCache.cpp
)

target_include_directories(ProgramUtilTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ProgramUtilTest
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        ProgramTest
        ProgramTest.cpp
)

add_executable(
        AsyncCompileTest
        AsyncCompileTest.cpp
)

target_include_directories(AsyncCompileTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        AsyncCompileTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        AsyncLinkTest
        AsyncLinkTest.cpp
)

target_include_directories(AsyncLinkTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        AsyncLinkTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

# Its own binary on purpose: this one calls MobileGL::Destroy(), and ShaderCompilePool's
# stop is a one-way latch for the whole process - every case declared after it in the same
# binary would silently run its compiles and links inline.
add_executable(
        AsyncTeardownTest
        AsyncTeardownTest.cpp
)

target_include_directories(AsyncTeardownTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        AsyncTeardownTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

target_include_directories(ProgramTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ProgramTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(ProgramUtilTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(ProgramTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
# Heavier than the rest of the unit suite by design: several cases deliberately saturate the
# compile pool so there is something in flight to race against.
gtest_discover_tests(AsyncCompileTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
gtest_discover_tests(AsyncLinkTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
gtest_discover_tests(AsyncTeardownTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
