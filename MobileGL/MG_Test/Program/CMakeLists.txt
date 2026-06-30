cmake_minimum_required(VERSION 3.24)

add_executable(
        ProgramUtilTest
        ProgramUtilTest.cpp

        ${MGL_ROOT}/MobileGL/MG_Util/Converters/GLToStr/GLEnumConverter.cpp
        ${MGL_ROOT}/MobileGL/MG_Util/Converters/GLToGlslang/ProgramEnumConverter.cpp
        ${MGL_ROOT}/MobileGL/MG_Util/ShaderTranspiler/ShaderCompiler.cpp
        ${MGL_ROOT}/MobileGL/MG_Util/ShaderTranspiler/SpvcSession.cpp
        ${MGL_ROOT}/MobileGL/MG_Util/ShaderTranspiler/glslang/UniformTraverser.cpp
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
gtest_discover_tests(ProgramUtilTest DISCOVERY_TIMEOUT 30)
gtest_discover_tests(ProgramTest DISCOVERY_TIMEOUT 30)
