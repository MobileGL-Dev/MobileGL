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
        PersistentBufferOrderingProbeTest.cpp
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

# MOBILEGL_BAKED_INTERNAL_SHADERS (CONTRACT-P7 §5.1 (D)): recompiles every baked SPIR-V header
# in the tree from its committed GLSL with the IN-TREE glslang and byte-compares. It is also the
# BAKER - scripts/bake_internal_shaders.py runs this binary rather than a glslangValidator of its
# own, so the gate and the generator cannot be two compilers with two answers.
#
# glslang-default-resource-limits is the standalone limits table glslangValidator itself parses
# with; MobileGL_s links glslang but builds its own limits from the live GL context, which a test
# with no context cannot do. MOBILEGL_BAKED_SHADER_ROOT is the SOURCE tree: the .vert/.frag/.comp
# inputs are not installed next to the binary and the gate fails rather than skips without them.
add_executable(
        BakedInternalShadersTest
        BakedInternalShadersTest.cpp
)

target_include_directories(BakedInternalShadersTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_compile_definitions(BakedInternalShadersTest PRIVATE
        MOBILEGL_BAKED_SHADER_ROOT="${MGL_ROOT}"
)

target_link_libraries(BakedInternalShadersTest PRIVATE
        GTest::gtest_main
        glslang
        glslang-default-resource-limits
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(DriverPostIterationRPWitnessTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(BakedInternalShadersTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(DriverBugProbesTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(PrimitivesGeneratedNoXfbProbeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
