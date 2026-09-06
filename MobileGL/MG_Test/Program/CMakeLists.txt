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

if (MSVC)
    # This test compiles library sources of its own; pulling the whole static
    # library in as well (the directory-wide MG_Test link option) would define
    # them twice, so that option is dropped for this one target.
    get_target_property(_program_util_link_options ProgramUtilTest LINK_OPTIONS)
    if (_program_util_link_options)
        list(REMOVE_ITEM _program_util_link_options /WHOLEARCHIVE:MobileGL_s)
        set_target_properties(ProgramUtilTest PROPERTIES LINK_OPTIONS "${_program_util_link_options}")
    endif()
endif()

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

add_executable(
        OptimisticStatusTest
        OptimisticStatusTest.cpp
)

target_include_directories(OptimisticStatusTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        OptimisticStatusTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
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

# Its own binary, like the other async suites: its cases pin the compile pool down to one
# worker so a phase-B job really is still queued while the GL query surface is being read.
add_executable(
        AsyncSpirvPhaseTest
        AsyncSpirvPhaseTest.cpp
)

target_include_directories(AsyncSpirvPhaseTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        AsyncSpirvPhaseTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        ShaderCompileAdoptionTest
        ShaderCompileAdoptionTest.cpp
)

target_include_directories(ShaderCompileAdoptionTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ShaderCompileAdoptionTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        ParallelShaderCompileTest
        ParallelShaderCompileTest.cpp
)

target_include_directories(ParallelShaderCompileTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ParallelShaderCompileTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

# Its own binary so the "fresh process" isolation level in it is really available
# through --gtest_filter, and so its 60 A->B link pairs cannot perturb another
# suite's per-context caches.
add_executable(
        XfbFrontendOrderInvarianceTest
        XfbFrontendOrderInvarianceTest.cpp
)

target_include_directories(XfbFrontendOrderInvarianceTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        XfbFrontendOrderInvarianceTest PRIVATE
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

add_executable(
        XfbBlockVaryingTest
        XfbBlockVaryingTest.cpp
)

target_include_directories(XfbBlockVaryingTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        XfbBlockVaryingTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        TessellationLinkTest
        TessellationLinkTest.cpp
)

target_include_directories(TessellationLinkTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        TessellationLinkTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        ProgramPipelineCompositeTest
        ProgramPipelineCompositeTest.cpp
)

target_include_directories(ProgramPipelineCompositeTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ProgramPipelineCompositeTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

add_executable(
        ProgramInterfaceTest
        ProgramInterfaceTest.cpp
)

target_include_directories(ProgramInterfaceTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ProgramInterfaceTest PRIVATE
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
gtest_discover_tests(ProgramInterfaceTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(ProgramPipelineCompositeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(XfbBlockVaryingTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(TessellationLinkTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
# Heavier than the rest of the unit suite by design: several cases deliberately saturate the
# compile pool so there is something in flight to race against.
gtest_discover_tests(AsyncCompileTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
gtest_discover_tests(AsyncLinkTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
# Same reason: every case here links a batch against a one-worker pool so that phase B is
# genuinely outstanding while phase A is being interrogated.
gtest_discover_tests(AsyncSpirvPhaseTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
# Same reason: the stage-6 cases keep a backlog in flight so a release really can race a
# worker, and the 48-object stress links every one of them.
gtest_discover_tests(ShaderCompileAdoptionTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
# Same reason: the GL_COMPLETION_STATUS_KHR cases saturate a one-worker pool on purpose.
gtest_discover_tests(ParallelShaderCompileTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
# Same reason: the optimistic-window cases need a saturated one-worker pool to observe an
# in-flight compile, and the two-phase replay links 48 programs across both flag states.
gtest_discover_tests(OptimisticStatusTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
gtest_discover_tests(AsyncTeardownTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)
# Same reason again: several cases leave A links outstanding while B compiles and links.
gtest_discover_tests(XfbFrontendOrderInvarianceTest DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit TIMEOUT 300)

# P0.5: the ProgramArtifacts.h header on its own - the alias identity proofs, the POD trip
# wire and the archive field tables. Its first include is the artifacts header, so this
# binary is also the compile-time proof that the header is self-contained.
add_executable(
        ProgramArtifactsTest
        ProgramArtifactsTest.cpp
)

target_include_directories(ProgramArtifactsTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ProgramArtifactsTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

gtest_discover_tests(ProgramArtifactsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
