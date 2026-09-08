cmake_minimum_required(VERSION 3.14)

add_executable(
        PipeCatalogueTest
        PipeCatalogueTest.cpp
)

target_include_directories(PipeCatalogueTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/MobileGL/MG_Pipe
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        PipeCatalogueTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(PipeCatalogueTest PRIVATE /Zc:preprocessor)
endif()

# The P1 poison and verify shapes at the block level: a fake GLContext, the real filler and
# accessors out of the static library. Links gtest (not gtest_main): the suite needs its own
# main() to point MOBILEGL_LOG_FILE_PATH at a temp file before anything logs, because its
# abort cases read the Fatal line back out of that file. Every case is a visible SKIP in a
# pull build.
add_executable(
        PipeInputsTest
        PipeInputsTest.cpp
)

target_include_directories(PipeInputsTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/MobileGL/MG_Pipe
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        PipeInputsTest PRIVATE
        GTest::gtest
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(PipeInputsTest PRIVATE /Zc:preprocessor)
endif()


# The four P2 suites. Their targets and this registration are the CONTRACT commit's; their
# CONTENTS belong to the packages named in each file's header, so no package after A has to
# come back to this file.
#
# RenderStateSpansTest links gtest, not gtest_main, for PipeInputsTest's reason: its applier
# trip-wire cases read the wire's line back out of a log file, so the suite needs its own
# main() to point MOBILEGL_LOG_FILE_PATH at one before anything logs. The other three need no
# main() of their own.
foreach(pipeTest TrackerTest SlotAllocatorTest CsoCacheTest)
        add_executable(${pipeTest} ${pipeTest}.cpp)

        target_include_directories(${pipeTest} PRIVATE
                ${MGL_ROOT}/include
                ${MGL_ROOT}/MobileGL
                ${MGL_ROOT}/MobileGL/MG_Pipe
                ${MGL_ROOT}/3rdparty/xxHash
                ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
                ${MGL_ROOT}/3rdparty/SPIRV-Reflect
        )

        target_link_libraries(${pipeTest} PRIVATE
                GTest::gtest_main
                ${LINK_LIBRARIES}
        )

        if (MSVC)
                target_compile_options(${pipeTest} PRIVATE /Zc:preprocessor)
        endif()
endforeach()

add_executable(
        RenderStateSpansTest
        RenderStateSpansTest.cpp
)

target_include_directories(RenderStateSpansTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/MobileGL/MG_Pipe
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(RenderStateSpansTest PRIVATE
        GTest::gtest
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(RenderStateSpansTest PRIVATE /Zc:preprocessor)
endif()

# Magma's own {slot, gen} mint and the claim rule its per-slot memo tables use
# (MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h, which is header-only). It sits here rather
# than under Backend/DirectVulkan because it needs no device, no GLFW and no Vulkan loader - and
# because that directory is registered only when ENABLE_INTEGRATION_TESTS is on, while this
# suite is the only place in the tree where a deleted `++Gen` is caught. See the file header.
add_executable(
        MagmaPipeIdentityTest
        MagmaPipeIdentityTest.cpp
)

target_include_directories(MagmaPipeIdentityTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/MobileGL/MG_Pipe
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(MagmaPipeIdentityTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

if (MSVC)
        target_compile_options(MagmaPipeIdentityTest PRIVATE /Zc:preprocessor)
endif()

# P3a's two suites AND P4a's six, together because they take exactly the same shape. Their
# targets and this registration are the CONTRACT commit's, for the same reason the four P2
# suites' are: their CONTENTS belong to two later packages each, and neither of them should
# have to come back to this file to add a case.
#
# P4a's six are FramebufferEmitTest, TextureEmitTest, SamplerEmitTest, ImageEmitTest,
# ProgramEmitTest and CompositeResolverTest. Each lands from the contract commit with one case
# that pins the shape its later cases depend on - the emitter is one never-destroyed process
# singleton, or the composite band has exactly one door - so none of them is an empty file
# waiting for a package, and none of them can be registered wrongly without a red test.
#
# They link gtest rather than gtest_main and carry their own main(), like PipeInputsTest and
# RenderStateSpansTest: the applier's bounds and protocol trip wires report through a log line
# in a shipped push build and std::abort() in a poison or verify one, so a case that drives
# one reads the line back out of a file the process names before anything logs. Deciding that
# HERE is what keeps the later packages out of this file.
foreach(pipeTest ResourceEmitTest VertexInputEmitTest
                 FramebufferEmitTest TextureEmitTest SamplerEmitTest ImageEmitTest
                 ProgramEmitTest CompositeResolverTest)
        add_executable(${pipeTest} ${pipeTest}.cpp)

        target_include_directories(${pipeTest} PRIVATE
                ${MGL_ROOT}/include
                ${MGL_ROOT}/MobileGL
                ${MGL_ROOT}/MobileGL/MG_Pipe
                ${MGL_ROOT}/3rdparty/xxHash
                ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
                ${MGL_ROOT}/3rdparty/SPIRV-Reflect
        )

        target_link_libraries(${pipeTest} PRIVATE
                GTest::gtest
                ${LINK_LIBRARIES}
        )

        if (MSVC)
                target_compile_options(${pipeTest} PRIVATE /Zc:preprocessor)
        endif()
endforeach()

include(GoogleTest)
gtest_discover_tests(PipeCatalogueTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
foreach(pipeTest ResourceEmitTest VertexInputEmitTest
                 FramebufferEmitTest TextureEmitTest SamplerEmitTest ImageEmitTest
                 ProgramEmitTest CompositeResolverTest)
        gtest_discover_tests(${pipeTest} DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
endforeach()
gtest_discover_tests(MagmaPipeIdentityTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(PipeInputsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(RenderStateSpansTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
foreach(pipeTest TrackerTest SlotAllocatorTest CsoCacheTest)
        gtest_discover_tests(${pipeTest} DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
endforeach()
