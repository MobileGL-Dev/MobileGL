# MobileGL - MobileGL/MG_IntegrationTest/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.24)

# MobileGL headless GPU integration tests. Each scenario brings up a real EGL pbuffer context,
# renders through a real backend and asserts on glReadPixels output. The module is OFF by default
# (MOBILEGL_BUILD_INTEGRATION_TEST). Without a GPU every scenario skips: the harness runs bring-up
# in a forked child first, because MobileGL aborts on an unusable platform (HeadlessGL.cpp).
# A skip looks like a pass, so set MOBILEGL_ITEST_REQUIRE_GPU where a GPU is expected.
# MOBILEGL_BACKEND_TYPE is latched at initialization, so one process is one backend and each
# scenario is registered once per backend under the `integration-gpu` label.

message(STATUS "Generating build files for MobileGL Integration Test...")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MGL_ITEST_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# Desktop links the static implementation directly. Android runs the same
# executable from adb shell and links the shipping shared library instead.
if (ANDROID)
    set(MGL_ITEST_MOBILEGL_TARGET MobileGL)
elseif (TARGET MobileGL_s)
    set(MGL_ITEST_MOBILEGL_TARGET MobileGL_s)
else()
    message(STATUS "No MobileGL library target is available; skipping the integration test module")
    return()
endif()

# MG_Test already pulls googletest in when MOBILEGL_BUILD_TEST is ON. Stand on
# our own feet when it is not, so this module can be built by itself.
if (NOT TARGET GTest::gtest)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(MobileGLIntegrationTest
    Main.cpp
    Harness/HeadlessGL.cpp
    Harness/BackendCapsPeek.cpp
    Harness/PipeSlotPeek.cpp
    Harness/PipeApplyPeek.cpp
    Harness/P4aSeamPeek.cpp
    Harness/P4aFinalFixPeek.cpp
    Harness/PersistentMapPeek.cpp
    Harness/PoisonOmissionArm.cpp
    Harness/SplitRuntimePeek.cpp
    # ---- P3b/P4b wave 2-D package D2 (own block; D1 edits this file elsewhere) --------------
    # The client texture emitter's own counters, for TextureViewAliasScenario's half of the
    # owner-keyed emission cursor gate: a drain that ran and emitted nothing is invisible in
    # pixels and is exactly the two-cursor regression's signature.
    Harness/TextureEmitPeek.cpp
    Scenarios/OrientationScenario.cpp
    Scenarios/CrossFrameBufferScenario.cpp
    Scenarios/ResidentIndexScenario.cpp
    Scenarios/MultiDrawScenario.cpp
    Scenarios/IndexedDrawFamilyScenario.cpp
    Scenarios/SyncWireScenario.cpp
    Scenarios/DrawParametersScenario.cpp
    Scenarios/AsyncCompileScenario.cpp
    Scenarios/ArbShaderObjectsScenario.cpp
    Scenarios/CubeFaceReadbackScenario.cpp
    Scenarios/TextureUnitSamplingScenario.cpp
    Scenarios/XfbAfterClipDistanceScenario.cpp
    Scenarios/UnwrittenPositionOutputScenario.cpp
    Scenarios/SampleMaskScopeScenario.cpp
    Scenarios/SampledSetStalenessScenario.cpp
    Scenarios/ThreeChannelAttachmentScenario.cpp
    Scenarios/SnormAttachmentScenario.cpp
    Scenarios/MonolithAttachmentClearScenario.cpp
    Scenarios/PipelineFailureScenario.cpp
    Scenarios/AdvertisedLimitsScenario.cpp
    Scenarios/PixelStoreSweepScenario.cpp
    # P7 gate 5 (g5-readback): readbacks larger than one reply slot. Ordinary GL cases, so the
    # monolith arms run them too - that is the parity half of the split registrations below.
    Scenarios/BandedReadbackScenario.cpp
    # P9 (W1): reads into a pixel pack buffer answered by nothing. Ordinary GL cases too, plus
    # one wire case that skips where there is no wire.
    Scenarios/PackBufferReadbackScenario.cpp
    # P10 (A): fence polls answered from the server's reports.
    Scenarios/FencePollScenario.cpp
    Scenarios/PrimitiveRestartScenario.cpp
    Scenarios/FragCoordOriginScenario.cpp
    Scenarios/ClipControlScenario.cpp
    Scenarios/ClientVertexArrayScenario.cpp
    Scenarios/ClearThenReadPixelsScenario.cpp
    # P15 S0: a clear between draws into the same framebuffer (DirectVulkan records it in the pass).
    Scenarios/InPassClearScenario.cpp
    # P15 S0: a CPU-converted vertex stream reused and re-written within a frame (DirectVulkan caches it).
    Scenarios/ConvertedVertexStreamScenario.cpp
    Scenarios/WriteRenameScenario.cpp
    Scenarios/VaoShadowScenario.cpp
    Scenarios/SampleVariablesScenario.cpp
    Scenarios/DepthStencilReadbackScenario.cpp
    Scenarios/DepthStencilReadbackMatrixScenario.cpp
    Scenarios/DepthStencilReadbackAttachmentShapeScenario.cpp
    Scenarios/ClipDistanceScenario.cpp
    Scenarios/ViewportArrayScenario.cpp
    Scenarios/SsboArrayLengthScenario.cpp
    Scenarios/DoublePrecisionScenario.cpp
    Scenarios/UniformInitializerScenario.cpp
    Scenarios/SwizzleAccessRoutineScenario.cpp
    Scenarios/IterationRPFirstReductionScenario.cpp
    Scenarios/IterationRPProgram203Scenario.cpp
    Scenarios/IterationRPScratchFixScenario.cpp
    Scenarios/ProgramPipelineScenario.cpp
    Scenarios/ImageLoadStoreSsoScenario.cpp
    Scenarios/ImageTargetKindScenario.cpp
    Scenarios/ImageFormatQualifierScenario.cpp
    Scenarios/TextureRemintPullScenario.cpp
    # P8-B: glGenerateMipmap's window and RGB16F / RGB32F on Espryt's server. Ordinary GL, so the
    # monolith arms of both backends run it too.
    Scenarios/GenerateMipmapServerScenario.cpp
    Scenarios/NonCoreImageFormatScenario.cpp
    Scenarios/ImageSizeAfterRespecScenario.cpp
    Scenarios/EmptyTextureLevelScenario.cpp
    Scenarios/ImageBindableRemintScenario.cpp
    Scenarios/PartialUploadOverGpuContentScenario.cpp
    Scenarios/SsboDeclarationFormScenario.cpp
    Scenarios/Glsl420DeclarationScenario.cpp
    Scenarios/IoBlockNameCollisionScenario.cpp
    Scenarios/UnlocatedIoBlockScenario.cpp
    Scenarios/TessellationDrawModeScenario.cpp
    Scenarios/GeometryDrawModeScenario.cpp
    Scenarios/PostLinkAttachScenario.cpp
    Scenarios/FormatlessImageBakeScenario.cpp
    Scenarios/FragmentOutputArrayIndexScenario.cpp
    Scenarios/BufferTextureScenario.cpp
    Scenarios/VertexAttribBindingScenario.cpp
    Scenarios/XfbCaptureBufferReuseScenario.cpp
    Scenarios/XfbPrimitiveQueryScenario.cpp
    Scenarios/PrimitivesGeneratedNoXfbScenario.cpp
    Scenarios/XfbRepeatedCaptureScenario.cpp
    Scenarios/TessellationXfbCaptureScenario.cpp
    Scenarios/PointSizeDemotionScenario.cpp
    Scenarios/VertexArrayEnableDisableScenario.cpp
    Scenarios/CopyImageLevelRangeScenario.cpp
    Scenarios/CopyImageLayeredScenario.cpp
    Scenarios/CopyImagePacked16Scenario.cpp
    Scenarios/CopyImageStoreReadbackScenario.cpp
    Scenarios/TextureViewScenario.cpp
    # P3b/P4b wave 2-D package D2: the verification half of the owner-keyed emission cursor.
    Scenarios/TextureViewAliasScenario.cpp
    Scenarios/PackedWordReadbackScenario.cpp
    Scenarios/LayeredAttachmentBarrierScenario.cpp
    Scenarios/LayeredAttachmentShapeScenario.cpp
    Scenarios/LayeredTextureReadbackScenario.cpp
    Scenarios/AtomicCounterScenario.cpp
    Scenarios/LargeArenaAdoptionScenario.cpp
    Scenarios/DeletedBoundBufferScenario.cpp
    Scenarios/SsboArrayDynamicIndexScenario.cpp
    Scenarios/StorageBufferRegrowScenario.cpp
    Scenarios/SpirvShaderBinaryScenario.cpp
    Scenarios/RelinkStageSetScenario.cpp
    Scenarios/GuiBatchScenario.cpp
    Scenarios/UnboundImageDescriptorScenario.cpp
    # P7 gate 5: an image unit naming a (level, layer) its texture lacks (GL 4.6 core 8.26).
    Scenarios/InvalidImageUnitScenario.cpp
    Scenarios/UnboundUniformBlockScenario.cpp
    Scenarios/IntegerBorderColorScenario.cpp
    Scenarios/ClearTexImageUndefinedLevelZeroScenario.cpp
    Scenarios/RenderbufferBlendFormatScenario.cpp
    Scenarios/DualSourceBlendScenario.cpp
    Scenarios/OversizedRecordingSplitScenario.cpp
    Scenarios/DescriptorPoolGrowthScenario.cpp
    Scenarios/PipeVerifyArmingScenario.cpp
    Scenarios/PoisonOmissionScenario.cpp
    Scenarios/HandleRecycleScenario.cpp
    Scenarios/CsoContentAddressingScenario.cpp
    Scenarios/ResourceSubsystemControlScenario.cpp
    Scenarios/TextureParamsWithoutASamplerViewScenario.cpp
    # P13 W3b: the GL-level successor of the borrowed-slot unit-memo probe.
    Scenarios/DsaUpdateKeepsTheBoundUnitScenario.cpp
    # C8: a shared object changed through a sibling context reaches the other's next draw.
    Scenarios/SharedContextChangeScenario.cpp
    Scenarios/TextureUploadShapeScenario.cpp
    Scenarios/ObjectSubsystemControlScenario.cpp
    Scenarios/P4aSeamAuditScenario.cpp
    Scenarios/P4aFinalFixScenario.cpp
    # P5's two new scenarios, targets B and C of the reduced path (BRIEF-P5 4). Both are
    # ORDINARY GL scenarios that run in every lane; the DirectGLES.Split. entries further down
    # run the same cases with MOBILEGL_TRANSPORT=inproc.
    Scenarios/TriangleScenario.cpp
    Scenarios/PersistentCoherentMapScenario.cpp
    Scenarios/DefaultFramebufferAcrossSwapScenario.cpp
    # P15 present wait: a frame's off-screen work is submitted ahead of its window pass.
    Scenarios/WindowPassGateScenario.cpp
    Scenarios/ScenarioIsolationScenario.cpp
    Scenarios/UploadOrderScenario.cpp
    Scenarios/PassOrderScenario.cpp
    Scenarios/DefaultScissorFollowsSurfaceScenario.cpp
    Scenarios/ClearOrderScenario.cpp
)

target_include_directories(MobileGLIntegrationTest PRIVATE
    ${MGL_ITEST_ROOT}/include
    ${MGL_ITEST_ROOT}/MobileGL
)

# gtest, not gtest_main: Main.cpp installs the harness banner itself.
target_link_libraries(MobileGLIntegrationTest PRIVATE
    GTest::gtest
    ${MGL_ITEST_MOBILEGL_TARGET}
)

if (ANDROID)
    find_library(MGL_ITEST_ANDROID_LIBRARY android REQUIRED)
    find_library(MGL_ITEST_LOG_LIBRARY log REQUIRED)
    find_library(MGL_ITEST_MEDIANDK_LIBRARY mediandk REQUIRED)
    target_link_libraries(MobileGLIntegrationTest PRIVATE
        ${MGL_ITEST_ANDROID_LIBRARY}
        ${MGL_ITEST_LOG_LIBRARY}
        ${MGL_ITEST_MEDIANDK_LIBRARY}
    )
endif()

if (MSVC)
    # Same reason as MG_Test/Backend/DirectVulkan: the GLES headers declare gl*
    # as dllimport on Windows, so the in-library GL entry-point definitions only
    # resolve if the whole static library is part of the link.
    target_link_options(MobileGLIntegrationTest PRIVATE /WHOLEARCHIVE:MobileGL_s)
endif()
target_compile_definitions(MobileGLIntegrationTest PRIVATE -DNOMINMAX)

if (ANDROID)
    return()
endif()

# --- ctest wiring --------------------------------------------------------
# A bare libEGL on a glvnd box resolves to whatever vendor comes first, which is
# usually llvmpipe - a software rasteriser silently replacing the GPU under
# a GPU test. Pin the vendor/ICD json the same way MG_Benchmark's
# run_driver_bench.sh does.
#
# Leaving these empty is not a neutral default, it is the failure mode: an
# unpinned libEGL lands on llvmpipe and the suite goes green having tested a
# software rasteriser. So they are DETECTED here rather than defaulted to empty,
# and an empty result is a loud warning.
#
# mgl_itest_find_driver_json(<outVar> <description> <glob> [<glob>...])
#   Picks the first json a real hardware vendor owns, in preference order, and
#   never picks a software rasteriser (llvmpipe / lavapipe / swrast) - landing on
#   one of those silently is the exact accident this pinning exists to prevent.
function(mgl_itest_find_driver_json outVar)
    set(candidates "")
    foreach(pattern IN LISTS ARGN)
        file(GLOB matches "${pattern}")
        list(APPEND candidates ${matches})
    endforeach()
    list(SORT candidates)
    # Vendors ship an i686 json beside the x86_64 one and it sorts first. Pinning
    # the wrong word size is worse than not pinning at all - the loader finds no
    # driver and the whole suite skips - so drop the mismatched ones outright.
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(FILTER candidates EXCLUDE REGEX "i686|i386")
    else()
        list(FILTER candidates EXCLUDE REGEX "x86_64|aarch64")
    endif()
    set(software "")
    foreach(vendor IN ITEMS nvidia amdgpu amd radeon intel_hasvk intel broadcom freedreno panfrost)
        foreach(candidate IN LISTS candidates)
            get_filename_component(leaf "${candidate}" NAME)
            string(TOLOWER "${leaf}" leaf)
            if (leaf MATCHES "${vendor}")
                set(${outVar} "${candidate}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
    # Nothing recognised as hardware. Report the first non-software entry if there
    # is one; otherwise report nothing, so the warning below fires.
    foreach(candidate IN LISTS candidates)
        get_filename_component(leaf "${candidate}" NAME)
        string(TOLOWER "${leaf}" leaf)
        if (NOT leaf MATCHES "lvp|llvmpipe|lavapipe|swrast|softpipe")
            set(${outVar} "${candidate}" PARENT_SCOPE)
            return()
        endif()
        set(software "${candidate}")
    endforeach()
    set(${outVar} "" PARENT_SCOPE)
endfunction()

set(MGL_ITEST_DETECTED_EGL_VENDOR "")
set(MGL_ITEST_DETECTED_VK_ICD "")
if (UNIX AND NOT APPLE AND NOT ANDROID)
    mgl_itest_find_driver_json(MGL_ITEST_DETECTED_EGL_VENDOR
        "/usr/share/glvnd/egl_vendor.d/*.json"
        "/etc/glvnd/egl_vendor.d/*.json")
    mgl_itest_find_driver_json(MGL_ITEST_DETECTED_VK_ICD
        "/usr/share/vulkan/icd.d/*.json"
        "/etc/vulkan/icd.d/*.json")
endif()

set(MOBILEGL_ITEST_EGL_VENDOR "${MGL_ITEST_DETECTED_EGL_VENDOR}" CACHE FILEPATH
    "glvnd EGL vendor json to pin for the integration tests (empty: leave the loader alone)")
set(MOBILEGL_ITEST_VK_ICD "${MGL_ITEST_DETECTED_VK_ICD}" CACHE FILEPATH
    "Vulkan ICD json to pin for the DirectVulkan integration tests (empty: leave the loader alone)")

if (MOBILEGL_ITEST_EGL_VENDOR)
    message(STATUS "Integration tests: pinning EGL vendor ${MOBILEGL_ITEST_EGL_VENDOR}")
else()
    message(WARNING
        "Integration tests: no EGL vendor json found or configured (MOBILEGL_ITEST_EGL_VENDOR is empty). "
        "An unpinned libEGL on a glvnd system resolves to whichever vendor comes first, which is usually "
        "llvmpipe - the scenarios would then go green against a software rasteriser instead of the GPU. "
        "Set -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/<vendor>.json.")
endif()
if (MOBILEGL_ITEST_VK_ICD)
    message(STATUS "Integration tests: pinning Vulkan ICD ${MOBILEGL_ITEST_VK_ICD}")
else()
    message(WARNING
        "Integration tests: no Vulkan ICD json found or configured (MOBILEGL_ITEST_VK_ICD is empty). "
        "DirectVulkan would then load whichever ICD the loader enumerates first, quite possibly lavapipe. "
        "Set -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/<vendor>.json.")
endif()

# Turns "no usable GPU" from a clean skip into a failure - see ScenarioFixture.h.
# Without it the integration-gpu label is unfalsifiable: a run that skipped every
# scenario and a run that passed every scenario are the same green in ctest.
option(MOBILEGL_ITEST_REQUIRE_GPU
    "Fail (rather than skip) the integration scenarios when the headless harness is unusable" OFF)

# No EGL_PLATFORM knob here on purpose. The harness pins EGL_PLATFORM=surfaceless
# itself before its first EGL call (HeadlessGL.cpp, EnsureHeadlessPlatform) so a
# developer's machine and a CI runner take the SAME path whether or not a window
# system happens to be running. This used to inject "x11", which is how the lane
# came up green on a workstation with WSLg and died on a runner with no X server.
#
# A build-system knob would not just be redundant, it would be a trap: `set(...
# CACHE ...)` does not rewrite an existing cache, so every build directory
# configured before this change would keep injecting EGL_PLATFORM=x11 and go on
# binding to a window system - silently, and only on the machines that have one.
# Someone reproducing a platform-specific bug sets EGL_PLATFORM in their own
# environment, which the harness still honours.

set(MGL_ITEST_COMMON_ENV "")
if (MOBILEGL_ITEST_EGL_VENDOR)
    list(APPEND MGL_ITEST_COMMON_ENV "__EGL_VENDOR_LIBRARY_FILENAMES=${MOBILEGL_ITEST_EGL_VENDOR}")
endif()
unset(MOBILEGL_ITEST_EGL_PLATFORM CACHE) # see above: an old cache must not resurrect x11
if (MOBILEGL_ITEST_REQUIRE_GPU)
    list(APPEND MGL_ITEST_COMMON_ENV "MOBILEGL_ITEST_REQUIRE_GPU=1")
endif()

set(MGL_ITEST_VULKAN_ENV ${MGL_ITEST_COMMON_ENV})
if (MOBILEGL_ITEST_VK_ICD)
    list(APPEND MGL_ITEST_VULKAN_ENV "VK_ICD_FILENAMES=${MOBILEGL_ITEST_VK_ICD}")
    # The three iterationRP repairs are tri-state quirks that default to device
    # auto-detection, and lavapipe is not on any auto list - so on lavapipe the
    # iterationRP scenarios run unrepaired and Program 203 misses its golden
    # output. CI's integration-gpu job exports these three by hand; pinning them
    # to the ICD instead means a local `ctest -L integration-gpu` measures the
    # same thing the gate does, with no environment to remember.
    if (MOBILEGL_ITEST_VK_ICD MATCHES "lvp_icd|lavapipe")
        message(STATUS "Integration tests: lavapipe ICD - forcing the iterationRP repairs on")
        list(APPEND MGL_ITEST_VULKAN_ENV
            "MOBILEGL_ITRP_FIX=ForceOn")
    endif()
endif()

# The ENVIRONMENT test property is itself a `;`-list, and gtest_discover_tests
# forwards PROPERTIES as a flat list - so a plain `;`-joined value arrives as
# four separate arguments and everything after the first is silently read as
# another property name. Escaping the separators keeps the whole thing one list
# element until set_tests_properties expands it back. Without this only
# MOBILEGL_BACKEND_TYPE reaches the test and the vendor/ICD pinning is lost.
function(mgl_itest_join_environment outVar)
    set(joined "")
    foreach(entry IN LISTS ARGN)
        if (joined)
            string(APPEND joined "\\;${entry}")
        else()
            set(joined "${entry}")
        endif()
    endforeach()
    set(${outVar} "${joined}" PARENT_SCOPE)
endfunction()

# One lane of the integration binary: the cases FILTER selects, registered as PREFIX<suite.case>
# with the environment ENV (a list joined by mgl_itest_join_environment). LABELS defaults to
# integration-gpu and TIMEOUT to the scenario timeout. LOCK serialises lanes that share a resource
# such as a log file; TEST_LIST names the ctest-time variable listing the lane's tests; FAIL_REGEX
# and FIXTURES set those properties; PROPERTIES passes further property pairs through.
function(mgl_itest_lane prefix filter)
    cmake_parse_arguments(PARSE_ARGV 2 lane "" "ENV;LABELS;TIMEOUT;LOCK;TEST_LIST;FAIL_REGEX;FIXTURES"
        "PROPERTIES")
    set(labels integration-gpu)
    if (DEFINED lane_LABELS)
        set(labels "${lane_LABELS}")
    endif()
    set(timeout ${MGL_ITEST_TIMEOUT})
    if (DEFINED lane_TIMEOUT)
        set(timeout ${lane_TIMEOUT})
    endif()
    # A value may itself be a list (two resource locks); escaped, it stays one argument when the
    # optional pair is expanded into the call below.
    set(listArgs "")
    if (lane_TEST_LIST)
        set(listArgs TEST_LIST "${lane_TEST_LIST}")
    endif()
    set(lockArgs "")
    if (lane_LOCK)
        string(REPLACE ";" "\;" lock "${lane_LOCK}")
        set(lockArgs RESOURCE_LOCK "${lock}")
    endif()
    set(fixtureArgs "")
    if (lane_FIXTURES)
        string(REPLACE ";" "\;" fixtures "${lane_FIXTURES}")
        set(fixtureArgs FIXTURES_REQUIRED "${fixtures}")
    endif()
    if (DEFINED lane_FAIL_REGEX)
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "${prefix}" ${listArgs}
            TEST_FILTER "${filter}"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${labels}" TIMEOUT ${timeout} ${lockArgs} ${fixtureArgs}
                FAIL_REGULAR_EXPRESSION "${lane_FAIL_REGEX}" ${lane_PROPERTIES} ENVIRONMENT "${lane_ENV}")
    else()
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "${prefix}" ${listArgs}
            TEST_FILTER "${filter}"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${labels}" TIMEOUT ${timeout} ${lockArgs} ${fixtureArgs}
                ${lane_PROPERTIES} ENVIRONMENT "${lane_ENV}")
    endif()
endfunction()

# --- capability markers: what this tree implements, answered at configure time ----------
#
# Some scenarios assert on a mechanism a backend may not implement (a {slot, gen} key, a consumer
# of MOBILEGL_PIPE_HANDLE_ABA_CONTROL, a counter's emitter). The probes below add an MGITEST_*
# marker to MGL_ITEST_CAPABILITY_ENV for each one found; a scenario without its marker skips and
# names what is missing, instead of being unregistered or passing vacuously. Every verdict is
# printed at configure time, so a probe that wrongly answers "no" is visible.
set(MGL_ITEST_CAPABILITY_ENV "")

# Whether the library under test compiled the push arm. Passed in rather than inferred, because
# the two CSO counters and the cso[] bracket of the stats line are #if MOBILEGL_PIPE_PUSH: in a
# pull build there is no CSO to mint and no channel to read, so the control has nothing to say -
# and "nothing to say" must be a SKIP that names the reason, not an assertion failure about a
# missing bracket.
#
# The lanes themselves are registered in BOTH builds even so. `ctest -L integration-gpu` has to
# be name-for-name identical between the pull build and the push build (P2 gate G2), and a lane
# that exists in only one of them breaks that comparison for every future package - a much worse
# outcome than four entries that skip. P13 W3b: every build pushes, so the marker is unconditional.
list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_PIPE_PUSH_BUILD=1")

# mgl_itest_probe_for_symbol: outVar is the first source under `directory` with a line matching
# symbolRegex, or "" when none has one. Markers probe content, never a file name: the owning code
# picks its own file layout, and a file probe would answer "no" forever after a move, leaving its
# arm skipping for a reason that is no longer true. The glob is CONFIGURE_DEPENDS (adding or
# removing a file reconfigures) and every file found joins CMAKE_CONFIGURE_DEPENDS (editing one
# reconfigures).
function(mgl_itest_probe_for_symbol outVar directory symbolRegex)
    file(GLOB_RECURSE mglItestProbeSources CONFIGURE_DEPENDS
        "${directory}/*.h" "${directory}/*.hpp" "${directory}/*.cpp" "${directory}/*.c")
    set(mglItestProbeHit "")
    foreach(mglItestProbeSource IN LISTS mglItestProbeSources)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${mglItestProbeSource}")
        file(STRINGS "${mglItestProbeSource}" mglItestProbeLines REGEX "${symbolRegex}")
        if (mglItestProbeLines AND NOT mglItestProbeHit)
            set(mglItestProbeHit "${mglItestProbeSource}")
        endif()
    endforeach()
    set(${outVar} "${mglItestProbeHit}" PARENT_SCOPE)
endfunction()

# mgl_itest_probe_for_two_symbols: the same probe over a conjunction. outVar is
# "<file matching A> + <file matching B>" when both regexes match somewhere under the directory,
# not necessarily in one file, and "" otherwise. It decides whether the ABA knob steers a
# backend's object keys: reading Features.PipeHandleAbaControl alone is not enough (a backend may
# consume it for other keys only), and requiring one file to hold both would tie the answer to
# file layout and leave the controls unarmed without notice; a false positive shows up as a red
# lane. Staleness is handled as in mgl_itest_probe_for_symbol.
function(mgl_itest_probe_for_two_symbols outVar directory symbolRegexA symbolRegexB)
    file(GLOB_RECURSE mglItestProbeSources CONFIGURE_DEPENDS
        "${directory}/*.h" "${directory}/*.hpp" "${directory}/*.cpp" "${directory}/*.c")
    set(mglItestProbeHitA "")
    set(mglItestProbeHitB "")
    foreach(mglItestProbeSource IN LISTS mglItestProbeSources)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${mglItestProbeSource}")
        if (NOT mglItestProbeHitA)
            file(STRINGS "${mglItestProbeSource}" mglItestProbeLinesA REGEX "${symbolRegexA}")
            if (mglItestProbeLinesA)
                set(mglItestProbeHitA "${mglItestProbeSource}")
            endif()
        endif()
        if (NOT mglItestProbeHitB)
            file(STRINGS "${mglItestProbeSource}" mglItestProbeLinesB REGEX "${symbolRegexB}")
            if (mglItestProbeLinesB)
                set(mglItestProbeHitB "${mglItestProbeSource}")
            endif()
        endif()
    endforeach()
    if (mglItestProbeHitA AND mglItestProbeHitB)
        set(${outVar} "${mglItestProbeHitA} + ${mglItestProbeHitB}" PARENT_SCOPE)
    else()
        set(${outVar} "" PARENT_SCOPE)
    endif()
endfunction()

# DirectGLES' Track H arm, probed by the subsystem bit it is gated on rather than by
# SlotTables.h existing: the bit is declared in the contract (kMGPipeSubsystemEsprytSlots, MG_Pipe/MGPipe.h) and the
# backend has to name it to honour MOBILEGL_PIPE_PUSH's default mask, whatever files package C
# spreads the slot tables across.
mgl_itest_probe_for_symbol(MGL_ITEST_ESPRYT_SLOTS
    "${MGL_ITEST_ROOT}/MobileGL/MG_Backend/DirectGLES" "kMGPipeSubsystemEsprytSlots")
if (MGL_ITEST_ESPRYT_SLOTS)
    message(STATUS "Integration tests: DirectGLES is keyed on {slot, gen} (${MGL_ITEST_ESPRYT_SLOTS})")
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_HANDLE_REKEY_DirectGLES=1")
else()
    message(STATUS "Integration tests: no DirectGLES source names kMGPipeSubsystemEsprytSlots - "
                   "HandleRecycle.Handles will SKIP on it")
endif()

# The CSO counters' EMITTER. The tracker package may implement the tracker and the cache
# header-only - today it does (MG_Impl/Pipe/{Tracker,CsoCache}.h, no Tracker.cpp) - so what is
# looked for is what the control actually reads: a source emitting the two counters.
mgl_itest_probe_for_symbol(MGL_ITEST_CSO_EMITTER
    "${MGL_ITEST_ROOT}/MobileGL/MG_Impl/Pipe" "RenderStateCso(Mints|Binds)")
if (MGL_ITEST_CSO_EMITTER)
    message(STATUS "Integration tests: the CSO counters have an emitter (${MGL_ITEST_CSO_EMITTER})")
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_PIPE_TRACKER_PRESENT=1")
else()
    message(STATUS "Integration tests: no MG_Impl/Pipe source emits RenderStateCsoMints/Binds - "
                   "CsoContentAddressing will SKIP")
endif()

# DirectVulkan's Track H arm, and the ABA knob's consumer. Both over the whole backend
# directory: the re-key is subsystem 4's bit wherever package D reads it, and the knob has a
# consumer if ANY DirectVulkan source reverts a guard on it - today two do, in two files.
mgl_itest_probe_for_symbol(MGL_ITEST_MAGMA_REKEY
    "${MGL_ITEST_ROOT}/MobileGL/MG_Backend/DirectVulkan" "kMGPipeSubsystemMagmaVertexInput")
if (MGL_ITEST_MAGMA_REKEY)
    message(STATUS "Integration tests: DirectVulkan's vertex input is keyed on {slot, gen} "
                   "(${MGL_ITEST_MAGMA_REKEY})")
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_HANDLE_REKEY_DirectVulkan=1")
else()
    message(STATUS "Integration tests: no DirectVulkan source names kMGPipeSubsystemMagmaVertexInput - "
                   "HandleRecycle.Handles will SKIP on it")
endif()

# P3a's buffer question, and it is NOT the two above. Bits 5/6 re-keyed each backend's
# VERTEX-INPUT memos; a BUFFER only travels as a handle once the resource_* family does
# (P3a for DirectGLES, P7 for DirectVulkan), and until then a buffer's backend twin is
# still reached from the frontend BufferObject. HandleRecycle's buffer case therefore has
# a marker of its own: reading the P2 one would arm its Handles arm on a tree where
# nothing about a buffer is keyed on a handle.
#
# Probed by MGPipeResourceOps - the op table PipeApply.h declares and a backend registers -
# over each backend's whole directory, for the reason the block above gives: the owning
# package picks its own file layout, and a filename probe would answer "no" forever the
# moment it moved the code.
foreach(mglItestResourceBackend DirectGLES DirectVulkan)
    mgl_itest_probe_for_symbol(MGL_ITEST_RESOURCE_OPS
        "${MGL_ITEST_ROOT}/MobileGL/MG_Backend/${mglItestResourceBackend}" "MGPipeResourceOps")
    if (MGL_ITEST_RESOURCE_OPS)
        message(STATUS "Integration tests: ${mglItestResourceBackend} registers a resource op table "
                       "(${MGL_ITEST_RESOURCE_OPS})")
        list(APPEND MGL_ITEST_CAPABILITY_ENV
            "MGITEST_HANDLE_REKEY_RESOURCES_${mglItestResourceBackend}=1")
    else()
        message(STATUS "Integration tests: no ${mglItestResourceBackend} source names "
                       "MGPipeResourceOps - HandleRecycle.Handles' buffer case will SKIP on it")
    endif()
endforeach()

# The client-side emitter of P3a's map-persistent-roundtrips counter, which is what
# StorageBufferRegrow, LargeArenaAdoption and ResourceSubsystemControl read. Same shape and
# same reason as the CSO emitter probe above: the counter is minted in MG_Impl/Pipe (package
# B), header-only today, so the question is "does any source there emit it", not "does a
# named file exist". Until it does, mpr= is structurally zero and an assertion about it
# would be a statement about nothing.
mgl_itest_probe_for_symbol(MGL_ITEST_RESOURCE_EMITTER
    "${MGL_ITEST_ROOT}/MobileGL/MG_Impl/Pipe" "MapPersistentRoundtrips")
if (MGL_ITEST_RESOURCE_EMITTER)
    message(STATUS "Integration tests: map-persistent-roundtrips has an emitter "
                   "(${MGL_ITEST_RESOURCE_EMITTER})")
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_PIPE_RESOURCE_EMITTER_PRESENT=1")
else()
    message(STATUS "Integration tests: no MG_Impl/Pipe source emits MapPersistentRoundtrips - "
                   "StorageBufferRegrow's, LargeArenaAdoption's and ResourceSubsystemControl's "
                   "mpr entries will SKIP")
endif()

# P4a's THIRD re-key question, per backend, and it is not either of the two above. Bits 5/6
# re-keyed each backend's VERTEX-INPUT memos and bit 7/8 the BUFFER's; P4a re-keys six OBJECT
# classes - texture, renderbuffer, framebuffer, sampler CSO, sampler view, shader CSO - behind
# four new subsystem bits, and a backend has to NAME one of those constants to honour
# MOBILEGL_PIPE_PUSH's default mask. A P4a case that read either older marker would arm its
# Handles arm on a tree where nothing about a texture is keyed on a handle: green for a re-key
# that does not exist. Probed by the constants rather than by a file, for the reason the block
# above gives - packages D and E choose their own file layout.
foreach(mglItestObjectBackend DirectGLES DirectVulkan)
    mgl_itest_probe_for_symbol(MGL_ITEST_OBJECT_REKEY
        "${MGL_ITEST_ROOT}/MobileGL/MG_Backend/${mglItestObjectBackend}"
        "kMGPipeSubsystem(Framebuffer|TextureResources|Samplers|Programs)")
    if (MGL_ITEST_OBJECT_REKEY)
        message(STATUS "Integration tests: ${mglItestObjectBackend} is keyed on {slot, gen} for "
                       "P4a's object families (${MGL_ITEST_OBJECT_REKEY})")
        list(APPEND MGL_ITEST_CAPABILITY_ENV
            "MGITEST_HANDLE_REKEY_OBJECTS_${mglItestObjectBackend}=1")
    else()
        message(STATUS "Integration tests: no ${mglItestObjectBackend} source names "
                       "kMGPipeSubsystem{Framebuffer,TextureResources,Samplers,Programs} - "
                       "HandleRecycle's six P4a cases will SKIP their Handles arm on it")
    endif()

    # ...and whether the ABA knob reaches those kinds THERE. A conjunction over the WHOLE
    # backend directory, for the reason mgl_itest_probe_for_two_symbols states: reading the
    # knob is not the same as steering P4a's keys with it, so the weaker single-regex evidence
    # would turn six always-on entries into a permanent red - but requiring one FILE to carry
    # both halves would let a backend satisfy the question and miss the probe, and the six
    # controls would then keep asserting the correct pixels with nothing recording that the
    # flip was forgotten (F-M5).
    mgl_itest_probe_for_two_symbols(MGL_ITEST_OBJECT_ABA
        "${MGL_ITEST_ROOT}/MobileGL/MG_Backend/${mglItestObjectBackend}"
        "PipeHandleAbaControl"
        "kMGPipeSubsystem(Framebuffer|TextureResources|Samplers|Programs)")
    if (MGL_ITEST_OBJECT_ABA)
        message(STATUS "Integration tests: MOBILEGL_PIPE_HANDLE_ABA_CONTROL steers "
                       "${mglItestObjectBackend}'s P4a object keys (${MGL_ITEST_OBJECT_ABA})")
        list(APPEND MGL_ITEST_CAPABILITY_ENV
            "MGITEST_HANDLE_ABA_OBJECTS_${mglItestObjectBackend}=1")
    else()
        message(STATUS "Integration tests: MobileGL/MG_Backend/${mglItestObjectBackend} does not "
                       "both read Features.PipeHandleAbaControl and name a P4a subsystem bit "
                       "somewhere under it - HandleRecycle's six P4a cases will assert the "
                       "CORRECT pixels on the AbaControl arm and say that it is not a control "
                       "for them yet")
    endif()
endforeach()

# The client-side emitter of P4a's four suppressor-visible emission counters, which is what
# ObjectSubsystemControl reads. Same shape and same reason as the CSO and map-persistent
# emitter probes above: the emitters are minted in MG_Impl/Pipe (packages B and C), header-only
# by design (D-P), so the question is "does any source there emit it", not "does a named file
# exist". Until one does, every counter in the emit[] bracket is structurally zero in BOTH arms
# of the A/B and an assertion about their difference would be a statement about nothing.
mgl_itest_probe_for_symbol(MGL_ITEST_OBJECT_EMITTER
    "${MGL_ITEST_ROOT}/MobileGL/MG_Impl/Pipe" "FramebufferEmissions")
if (MGL_ITEST_OBJECT_EMITTER)
    message(STATUS "Integration tests: P4a's object emissions have an emitter "
                   "(${MGL_ITEST_OBJECT_EMITTER})")
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_PIPE_OBJECT_EMITTER_PRESENT=1")
else()
    message(STATUS "Integration tests: no MG_Impl/Pipe source emits FramebufferEmissions - "
                   "ObjectSubsystemControl's emission case will SKIP")
endif()

# ...and the CLIENT half of the texture-upload shape, separately, because it is a different
# question with a different owner's file behind it (review F-m7). ctu= is emitted by PipeStats
# in EVERY push build whether or not anything increments it, so "the counter read zero" and
# "no client emitter exists" are the same number and TextureUploadShape cannot tell them apart
# from the summary line alone. An emitter that STOPPED emitting reads exactly like no emitter
# at all in that field, and the two-sided assertion the scenario exists for would pass while
# comparing nothing. This probe is what separates them.
#
# THAT EMITTER HAS LANDED (P3b/P4b wave 2-D corrects this comment's tense):
# MG_Impl/Pipe/TextureEmit.h's EmitOneLevel increments
# CallClass::ClientTextureUploadEmissions on every piece it emits, so the probe hits and the
# else() arm below is unreachable on this tree. It is KEPT rather than deleted because it is
# what makes the arming decision falsifiable in both directions - a probe that cannot report
# "absent" cannot be shown to be looking for the right symbol.
mgl_itest_probe_for_symbol(MGL_ITEST_CLIENT_TEXTURE_UPLOAD_EMITTER
    "${MGL_ITEST_ROOT}/MobileGL/MG_Impl/Pipe" "ClientTextureUploadEmissions")
if (MGL_ITEST_CLIENT_TEXTURE_UPLOAD_EMITTER)
    message(STATUS "Integration tests: the client texture-upload counter has an emitter "
                   "(${MGL_ITEST_CLIENT_TEXTURE_UPLOAD_EMITTER}) - TextureUploadShape's "
                   "two-sided assertion is live")
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_PIPE_CLIENT_TEXTURE_UPLOAD_EMITTER_PRESENT=1")
else()
    message(STATUS "Integration tests: no MG_Impl/Pipe source emits "
                   "ClientTextureUploadEmissions - TextureUploadShape records the SERVER shape "
                   "only and asserts that ctu= is zero")
endif()

mgl_itest_probe_for_symbol(MGL_ITEST_MAGMA_ABA
    "${MGL_ITEST_ROOT}/MobileGL/MG_Backend/DirectVulkan" "PipeHandleAbaControl")
if (MGL_ITEST_MAGMA_ABA)
    message(STATUS "Integration tests: MOBILEGL_PIPE_HANDLE_ABA_CONTROL has a consumer "
                   "(${MGL_ITEST_MAGMA_ABA})")
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_HANDLE_ABA_IMPLEMENTED=1")
else()
    message(STATUS "Integration tests: no DirectVulkan source names PipeHandleAbaControl - "
                   "HandleRecycle.AbaControl will SKIP")
endif()

mgl_itest_join_environment(MGL_ITEST_GLES_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_ASYNC_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ASYNC_SHADER_COMPILE=1" ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_FORCED_DS_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1" ${MGL_ITEST_COMMON_ENV})

# Environments for AsyncCompileScenario's compiler configurations and ViewportArrayScenario's
# negative control. Each setting is per process: part of what it decides is latched before the
# first GL call (the compile pool, the advertised extension list), and on Android this module
# links the shipping library, which exports no MG_Config, so each is an environment variable on
# a ctest entry of its own. Every list appends MGL_ITEST_COMMON_ENV / MGL_ITEST_VULKAN_ENV; an
# entry naming only its own variable would lose the EGL vendor and Vulkan ICD pinning.
mgl_itest_join_environment(MGL_ITEST_GLES_ASYNC_ON_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_ASYNC_SHADER_COMPILE=1" ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_ASYNC_OFF_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_ASYNC_SHADER_COMPILE=0" ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_ASYNC_ON_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ASYNC_SHADER_COMPILE=1" ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_ASYNC_OFF_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ASYNC_SHADER_COMPILE=0" ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OPTIMISTIC_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_ASYNC_SHADER_COMPILE=1"
    "MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS=1" ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_OPTIMISTIC_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ASYNC_SHADER_COMPILE=1"
    "MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS=1" ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_NO_VIEWPORT_EMULATION_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_ESPRYT_FORCE_VIEWPORT_ARRAY_EMULATION=0" ${MGL_ITEST_COMMON_ENV})
# MOBILEGL_LOG_FILE_PATH alongside the pin, because the arming assertion needs somewhere to
# read the library's own report from. The strip's arming signal is a latched MGLOG_I and there
# is no other way for a test process to learn that it fired - MG_Config is not reachable from
# this module on Android, where it links the shipping library. The path is per-lane so nothing
# else appends to it, and the case only trusts the bytes written after it started.
mgl_itest_join_environment(MGL_ITEST_GLES_UNLOCATED_IO_BLOCKS_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/unlocated-io-blocks.log"
    ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_WIDENED_PACKED16_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_ESPRYT_WIDEN_PACKED16_STORAGE=1" ${MGL_ITEST_COMMON_ENV})
# Same shape as the UnlocatedIoBlocks entry: the log path is where the reroute's latched
# MGLOG_I lands, and the arming case only trusts the bytes written after it started.
mgl_itest_join_environment(MGL_ITEST_VULKAN_PRIMGEN_REROUTE_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/primgen-query-reroute.log"
    ${MGL_ITEST_VULKAN_ENV})
# The command-buffer draw budget pinned far below one frame's draws, with the log path for the
# split's latched MGLOG_I - the same arming reasoning as the two entries above.
mgl_itest_join_environment(MGL_ITEST_VULKAN_WINDOW_PASS_GATE_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_WINDOW_PASS_GATE_LANE=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/window-pass-gate.log"
    ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_SPLIT_RECORDING_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER=64"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/oversized-recording-split.log"
    ${MGL_ITEST_VULKAN_ENV})
# The point-size demotion pinned on, per backend, with a per-lane log file for the arming
# assertion - the same MOBILEGL_LOG_FILE_PATH reasoning as the UnlocatedIoBlocks lane above.
# Two lanes because the demotion runs in the SHARED phase-B chain and each backend then
# consumes it differently (Espryt respells the driver-side capture request, Magma binds the
# SPIR-V Xfb decorations to the carrier).
mgl_itest_join_environment(MGL_ITEST_GLES_POINT_SIZE_DEMOTION_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_POINT_SIZE_DEMOTION=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/point-size-demotion-gles.log"
    ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_POINT_SIZE_DEMOTION_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_POINT_SIZE_DEMOTION=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/point-size-demotion-vulkan.log"
    ${MGL_ITEST_VULKAN_ENV})

# TIMEOUT on every entry: a GPU test that wedges must fail the run, not hang it.
set(MGL_ITEST_TIMEOUT 120)

include(GoogleTest)

# Discovery runs `--gtest_list_tests`, which does not construct the harness and
# so needs no GPU. One registration per backend; TEST_PREFIX keeps the two sets
# of ctest names apart.
# The original ambient names remain present, but mechanism oracles must not
# inherit an inproc environment: they deliberately toggle process-local push
# masks, pointer-key ABA, direct persistent adoption or synchronous stats slots.
# Keep ordinary pixel/byte cases (including the other arena/regrow cases) on the
# caller's transport. A separate label makes every exception inspectable.
set(MGL_ITEST_MONOLITH_MECHANISM_FILTER
    "HandleRecycleScenario.*:CsoContentAddressingScenario.*:ResourceSubsystemControlScenario.*:ObjectSubsystemControlScenario.*:PipeVerifyArmingScenario.*:PoisonOmissionScenario.*:StorageBufferRegrowScenario.NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw:LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip")
# P3b/P4b wave 2-D package D2: TextureViewAliasScenario joins this list rather than the ambient
# one for the same reason TextureViewScenario is in it - every case it carries constructs a
# glTextureView, which DirectGLES withholds unless MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1
# (ConfigLoader.cpp:213). In the ambient entry it would green-skip on every run and the gate
# would be a name in a list.
set(MGL_ITEST_GLES_VIEW_FILTER
    "TextureViewScenario.*:TextureViewAliasScenario.*:F1WireScenario.ComputeImageStoreThroughViewPreservesOtherRootLayer:F1WireScenario.TextureViewClearTargetsItsRootMipAndLayer:F1WireScenario.GenerateMipmapThroughViewKeepsOwnerLevelsAndLayersOutsideItsWindow:InvalidImageUnitScenario.ALevelPastATextureViewsWindowReadsZero")
# Cases that run only in the split-arm entries Harness/SplitLogPaths.cmake.in declares a lane
# for, and skip by construction anywhere else (no split transport, no declared arm). They are
# left out of the ambient, verify and whole-binary Magma registrations for the reason above:
# there they would green-skip on every run.
set(MGL_ITEST_SPLIT_ARM_ONLY_FILTER
    "PersistentCoherentMapScenario.ThePushIsTheOnlyRouteAWriteTakesToTheServer")

# The two lists are named so that SplitLogPaths.cmake.in can reach the ambient CtWireScenario
# entries, which the integration job's inproc leg runs on the transport it exports.
mgl_itest_lane("DirectGLES." "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:${MGL_ITEST_GLES_VIEW_FILTER}:${MGL_ITEST_SPLIT_ARM_ONLY_FILTER}"
    TEST_LIST MGL_AMBIENT_TESTS_GLES ENV "${MGL_ITEST_GLES_ENVIRONMENT}")

mgl_itest_lane("DirectVulkan." "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:${MGL_ITEST_SPLIT_ARM_ONLY_FILTER}"
    TEST_LIST MGL_AMBIENT_TESTS_VULKAN ENV "${MGL_ITEST_VULKAN_ENVIRONMENT}")

# ScenarioIsolationScenario's two cases in ONE process: the first leaves a deleted program current
# and an error unread, the second asserts ScenarioTest::SetUp cleared both. The entries above run
# one case per process, where both cases are green either way. add_test because
# gtest_discover_tests registers cases one at a time; a PLAIN list (see the arena pair below).
foreach(backend DirectGLES DirectVulkan)
    if (backend STREQUAL "DirectGLES")
        string(REPLACE "\\;" ";" mglItestIsolationEnv "${MGL_ITEST_GLES_ENVIRONMENT}")
    else()
        string(REPLACE "\\;" ";" mglItestIsolationEnv "${MGL_ITEST_VULKAN_ENVIRONMENT}")
    endif()
    add_test(NAME "${backend}.ScenarioIsolationScenario.CasesInOneProcess"
        COMMAND MobileGLIntegrationTest "--gtest_filter=ScenarioIsolationScenario.*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties("${backend}.ScenarioIsolationScenario.CasesInOneProcess" PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${mglItestIsolationEnv}")
endforeach()

foreach(backend DirectGLES DirectVulkan)
    if (backend STREQUAL "DirectGLES")
        set(mechanism_environment "${MGL_ITEST_GLES_ENVIRONMENT}")
    else()
        set(mechanism_environment "${MGL_ITEST_VULKAN_ENVIRONMENT}")
    endif()
    mgl_itest_lane("${backend}." "${MGL_ITEST_MONOLITH_MECHANISM_FILTER}"
        LABELS "integration-gpu\;integration-monolith-control" ENV "${mechanism_environment}\;MOBILEGL_TRANSPORT=monolith")
endforeach()

# This is Espryt's existing feature opt-in, independent of transport. Exercise
# the actual view implementation instead of inheriting its default-off skip.
mgl_itest_lane("DirectGLES." "${MGL_ITEST_GLES_VIEW_FILTER}"
    ENV "${MGL_ITEST_GLES_ENVIRONMENT}\;MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1")

# A third registration, of ONE scenario, with asynchronous shader compilation
# pinned on. Not a second code path in the renderer: a second ALLOCATION pattern.
# The async pipeline's job objects change which of the freed blocks the capture
# phase is handed, and that is what decides whether the destroyed-VAO address is
# reached at all - on the ablated (pre-fix) tree async=1 reproduced 3 runs out of
# 3 where the ambient default reproduced 2 of 3. Pinning it here means the
# high-signal configuration runs whatever the shipped default becomes, instead of
# the suite quietly weakening the day that default flips. It must be process-wide
# (the ENVIRONMENT property), not an in-process scope: the compile pool and its
# threads are stood up at initialization, and their allocations are half the
# point. DirectVulkan only - the memo this pins is DirectVulkan's.
mgl_itest_lane("DirectVulkan.AsyncCompile." "XfbAfterClipDistanceScenario.*"
    ENV "${MGL_ITEST_VULKAN_ASYNC_ENVIRONMENT}")

# A fourth registration, of the depth/stencil readback scenarios, with the ES
# shader-sampling emulation forced on. Not paranoia - without it these scenarios are
# UNFALSIFIABLE on the machines this suite runs on: OpenGL ES has no depth or stencil
# readback in core, but llvmpipe accepts the reads anyway, so every one of them
# goes green through a native path that the Adreno device does not have. Deleting the
# entire emulation left all of them passing. With the flag the native spellings are off
# the table and only the path the device actually takes remains. DirectGLES only - the
# emulation is DirectGLES's.
mgl_itest_lane("DirectGLES.ForcedDepthStencilEmulation." "DepthStencilReadback*Scenario.*"
    ENV "${MGL_ITEST_GLES_FORCED_DS_ENVIRONMENT}")

# UnlocatedIoBlockScenario with the interface-block location strip pinned on. llvmpipe handles
# located interface blocks, so the driver check that arms the strip on Mali passes here and the
# ambient entry never runs the strip. This lane, and each lane below that reads its log back,
# holds a RESOURCE_LOCK named after the log: every process in a lane truncates the shared
# MOBILEGL_LOG_FILE_PATH (fopen "w"), so under ctest -j a sibling could empty it while the
# arming case reads it. A narrower filter would also work but would rename entries, and ctest
# names must not disappear (ARCHITECTURE.md §13.2).
mgl_itest_lane("DirectGLES.UnlocatedIoBlocks." "UnlocatedIoBlockScenario.*"
    LOCK unlocated-io-blocks.log ENV "${MGL_ITEST_GLES_UNLOCATED_IO_BLOCKS_ENVIRONMENT}")

# P4a final review C-2: the dirty-then-delete case, with the allocator SCRIBBLING every freed
# block. The defect this pins was a client emitter resolving a dead-but-not-recycled texture
# handle to the freed ITextureObject* and calling a virtual on it from the next verb's drain;
# whether that reads the object's ghost or faults depends on what the allocator did with the
# block, so the ambient registrations above run the case as the application would see it and
# these two run it with MALLOC_PERTURB_ set, where a resolved-but-dead pointer faults rather
# than passes. Both backends: the death path is backend-neutral by ruling (ID-8).
mgl_itest_join_environment(MGL_ITEST_GLES_MALLOC_PERTURB_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MALLOC_PERTURB_=165" ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_MALLOC_PERTURB_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MALLOC_PERTURB_=165" ${MGL_ITEST_VULKAN_ENV})
mgl_itest_lane("DirectGLES.MallocPerturb." "P4aFinalFixScenario.ADirtyTextureDeletedBeforeAnyVerbIsWalkedByTheNextDrain"
    ENV "${MGL_ITEST_GLES_MALLOC_PERTURB_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.MallocPerturb." "P4aFinalFixScenario.ADirtyTextureDeletedBeforeAnyVerbIsWalkedByTheNextDrain"
    ENV "${MGL_ITEST_VULKAN_MALLOC_PERTURB_ENVIRONMENT}")

# AsyncCompileScenario, with asynchronous compilation PINNED ON per backend.
#
# Not a duplicate of what the two ambient registrations already run: they run whatever
# MobileGL's built-in default happens to be, and the day that default flips they would
# stop covering the asynchronous path without anything going red. These entries are the
# ones that keep the asynchronous half tested no matter what ships. They are also the
# only place ExtensionStringMatchesTheConfiguration can assert that the extension IS
# advertised - the case derives its expectation from this variable and nothing else, and
# skips where it is unset, precisely so that it is not asserting the implementation
# against itself.
mgl_itest_lane("DirectGLES.AsyncOn." "AsyncCompileScenario.*" ENV "${MGL_ITEST_GLES_ASYNC_ON_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.AsyncOn." "AsyncCompileScenario.*"
    ENV "${MGL_ITEST_VULKAN_ASYNC_ON_ENVIRONMENT}")

# The other side of the same switch: asynchronous compilation OFF, so
# GL_KHR_parallel_shader_compile must be WITHDRAWN from both spellings of the extension
# list and GL_MAX_SHADER_COMPILER_THREADS_KHR must read 0. Only that one case is
# registered here because it is the only one that has anything to say in this
# configuration - the other four exist to observe worker-built artifacts, and there are
# none - so registering the whole scenario would buy four guaranteed skips per backend.
# Together with the AsyncOn. entries above, one ctest run still covers both flag states,
# which is what the in-process forcing used to be for.
mgl_itest_lane("DirectGLES.AsyncOff." "AsyncCompileScenario.ExtensionStringMatchesTheConfiguration"
    ENV "${MGL_ITEST_GLES_ASYNC_OFF_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.AsyncOff." "AsyncCompileScenario.ExtensionStringMatchesTheConfiguration"
    ENV "${MGL_ITEST_VULKAN_ASYNC_OFF_ENVIRONMENT}")

# The optimistic-status quirk's end-to-end shape. Its own entries and not part of the
# AsyncOn. ones because the quirk is not neutral for the rest of the scenario: with it in
# force glGetShaderiv(GL_COMPILE_STATUS) deliberately answers without joining, which is
# exactly what CompletionStatusPollingThenForcedJoin asserts must NOT happen. Off by
# default and never advertised, so - unlike asynchronous compilation, which announces
# itself through the extension string - the variable is the only thing that can tell the
# case it is in force.
mgl_itest_lane("DirectGLES.OptimisticShaderStatus." "AsyncCompileScenario.IrisShapedTwoPhaseBatchRendersCorrectly"
    ENV "${MGL_ITEST_GLES_OPTIMISTIC_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.OptimisticShaderStatus." "AsyncCompileScenario.IrisShapedTwoPhaseBatchRendersCorrectly"
    ENV "${MGL_ITEST_VULKAN_OPTIMISTIC_ENVIRONMENT}")

# The negative control for the DirectGLES gl_ViewportIndex emulation, in a process that
# has it switched off. One case, because it is the only one the switch may touch: with
# the emulation off the three positive cases in the same fixture describe behaviour the
# backend does not have, so a whole-scenario registration would be three guaranteed reds.
# DirectGLES only - the flag steers nothing on DirectVulkan, which routes natively.
mgl_itest_lane("DirectGLES.NoViewportArrayEmulation." "ViewportArrayScenario.WithoutTheEmulationEveryIndexCollapsesOntoViewportZero"
    ENV "${MGL_ITEST_GLES_NO_VIEWPORT_EMULATION_ENVIRONMENT}")

# PrimitivesGeneratedNoXfbScenario again, with the GL_PRIMITIVES_GENERATED statistics
# reroute PINNED ON. The ambient DirectVulkan registration runs the same cases under the
# bring-up probe's Auto verdict, so between the two entries both accounting paths answer
# the same GL questions and must produce the same numbers - the "two pools must agree"
# gate this machine can hold that the affected device cannot. The pinned entry is also
# the only one whose arming case runs: it asserts the renderer's latched MGLOG_I, so a
# silently-disarmed reroute (an inverted override mapping, a lost gate) fails here
# instead of leaving every equality case vacuously green. DirectVulkan only - the flag
# steers nothing on DirectGLES.
mgl_itest_lane("DirectVulkan.PrimGenReroute." "PrimitivesGeneratedNoXfbScenario.*"
    LOCK primgen-query-reroute.log ENV "${MGL_ITEST_VULKAN_PRIMGEN_REROUTE_ENVIRONMENT}")

# OversizedRecordingSplitScenario with the command-buffer draw budget PINNED to 64. The ambient
# registrations run its cases under the default budget, which never splits them; this entry
# splits every case at least once between the draws that leave state (depth, stencil, a blend
# destination, multisample samples, an open query, an active capture) and the draws that
# consume it, so the results prove a split carries all of it, and the arming assertion (the
# renderer's latched MGLOG_I) proves the split happened at all - on a renderer that records the
# whole frame into one command buffer, it is the assertion that goes red. DirectVulkan only:
# the budget steers nothing on DirectGLES, whose driver flushes on its own.
#
# P11 M2: RESOURCE_LOCK, for the UnlocatedIoBlocks lane's reason above - EVERY case here reads the
# lane's one log back, and the library truncates it at each process's start, so under `ctest -j` a
# sibling case wipes the line a reading case is looking for (measured on this tree: 5 of 8 red at
# -j 6, a different five on the monolith and inproc runs).
mgl_itest_lane("DirectVulkan.SplitRecording." "OversizedRecordingSplitScenario.*"
    LOCK oversized-recording-split.log ENV "${MGL_ITEST_VULKAN_SPLIT_RECORDING_ENVIRONMENT}")

# P15 present wait: WindowPassGateScenario with its arming assertion on. The ambient entries run
# its cases for the pixels; this one also reads the renderer's latched MGLOG_I back, which a
# renderer that submits each frame as one batch behind the acquire wait never writes. DirectVulkan
# only: Espryt's flush ahead of the window buffer needs a window surface, which this harness does
# not have. RESOURCE_LOCK for SplitRecording's reason: every case reads the lane's one log back.
mgl_itest_lane("DirectVulkan.WindowPassGate." "WindowPassGateScenario.*"
    LOCK window-pass-gate.log ENV "${MGL_ITEST_VULKAN_WINDOW_PASS_GATE_ENVIRONMENT}")

# The packed16 copy scenarios again, with the 8-bit storage widening PINNED ON. The ambient
# registrations above cover the narrow storage - on every CI driver the widening's POST
# probe finds no field-order mirror, so Auto keeps the native 16-bit path - which means the
# storage every AFFECTED device will actually run would otherwise execute nowhere at all:
# no CI driver has the Mali bug that arms it. This lane is what proves the widened storage
# is client-invisible (same packed words in and out on every leg the 18 failing CTS bodies
# used, the renderbuffer one included). DirectGLES only - the flag steers nothing on
# DirectVulkan, which has always stored these formats widened.
mgl_itest_lane("DirectGLES.WidenedPacked16." "CopyImagePacked16Scenario.*"
    ENV "${MGL_ITEST_GLES_WIDENED_PACKED16_ENVIRONMENT}")

# PointSizeDemotionScenario with the demotion PINNED ON, per backend, for the reason every
# pinned lane above exists: llvmpipe and lavapipe both HOST gl_PointSize in tessellation and
# geometry stages, so the ambient registrations run these captures through the built-in and
# the demotion - the path every affected Mali device actually takes - would execute nowhere.
# The ambient runs stay the negative control: same scenario, same CPU-computed bytes, native
# path. Both backends, because the demotion is shared phase-B work with two different
# consumers (the ESSL capture respelling vs the SPIR-V Xfb carrier binding).
mgl_itest_lane("DirectGLES.PointSizeDemotion." "PointSizeDemotionScenario.*"
    LOCK point-size-demotion-gles.log ENV "${MGL_ITEST_GLES_POINT_SIZE_DEMOTION_ENVIRONMENT}")

mgl_itest_lane("DirectVulkan.PointSizeDemotion." "PointSizeDemotionScenario.*"
    LOCK point-size-demotion-vulkan.log ENV "${MGL_ITEST_VULKAN_POINT_SIZE_DEMOTION_ENVIRONMENT}")

# --- MOBILEGL_PIPE_VERIFY lanes (ARCHITECTURE.md §13.2), under `if (MOBILEGL_PIPE_VERIFY)` below
# They exist only in a -DMOBILEGL_PIPE_VERIFY=ON build, so `ctest -L integration-verify
# --no-tests=error` fails when the option is lost.
# --- handle ABA: one always-on monolith HandleRecycle.Handles lane per backend -------------
# MGITEST_HANDLE_ARM selects the arm (the scenario skips in the ambient entries). The lanes run
# the default subsystem mask, so they measure the {slot, gen} key that ships.
set(MGL_ITEST_HANDLES_ARM_KNOBS "")

mgl_itest_join_environment(MGL_ITEST_GLES_HANDLE_HANDLES_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_HANDLE_ARM=handles" ${MGL_ITEST_HANDLES_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_HANDLES_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=handles" ${MGL_ITEST_HANDLES_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
# P13 W6: the AbaControlHandles lane pinned the frontend arm (its knob defeats the identity half
# of Magma's frontend-arm keys); that arm is deleted, and the record arm keys every object on the
# applier's {slot, gen}, which the knob does not reach. Removed (docs/ci/ci-cleanup-log.md).
mgl_itest_lane("DirectGLES.HandleRecycle.Handles." "HandleRecycleScenario.*"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_HANDLE_HANDLES_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.HandleRecycle.Handles." "HandleRecycleScenario.*"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_VULKAN_HANDLE_HANDLES_ENVIRONMENT}")
# --- CSO content-addressing negative control ----------------------------------------------
# The On and Off masks differ only in bit 63 of MOBILEGL_PIPE_PUSH
# (kMGPipeBehaviourNoCsoContentAddressing); the rest of each mask is the default 0x3fff, because
# subsystem bits 0-13 are fixed on. The cases read the csom / csob counters from the library log,
# so each lane has a private log path (ARCHITECTURE.md §16.1). MOBILEGL_PIPE_STATS_PERIOD=1 writes
# one summary line per eglSwapBuffers, so two swaps bracket exactly the workload.
mgl_itest_join_environment(MGL_ITEST_GLES_CSO_ON_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_CSO_LANE=content-addressed"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-content-addressed-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_CSO_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_CSO_LANE=no-content-addressing"
    "MOBILEGL_PIPE_PUSH=0x8000000000003fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-no-content-addressing-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_CSO_ON_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_CSO_LANE=content-addressed"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-content-addressed-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_CSO_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_CSO_LANE=no-content-addressing"
    "MOBILEGL_PIPE_PUSH=0x8000000000003fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-no-content-addressing-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

mgl_itest_lane("DirectGLES.CsoContentAddressing.On." "CsoContentAddressingScenario.*"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_CSO_ON_ENVIRONMENT}")
mgl_itest_lane("DirectGLES.CsoContentAddressing.Off." "CsoContentAddressingScenario.*"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_CSO_OFF_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.CsoContentAddressing.On." "CsoContentAddressingScenario.*"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_VULKAN_CSO_ON_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.CsoContentAddressing.Off." "CsoContentAddressingScenario.*"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_VULKAN_CSO_OFF_ENVIRONMENT}")

# P13 W3b (ID-P13-3): THE NAMED-REFUSAL CONTROLS that replace the Off / Refused A/B lanes. Subsystem
# bits 0-13 are fixed on: their pre-handle arms are gone, so a mask that clears one has nothing to
# run and ConfigLoader refuses it at startup with Fatal{PipeSubsystemsFixedOn}. Each control runs
# the scenario its lane used to run, with the lane's own environment and mask, and passes only if
# the process went red AND its log names that refusal with that mask - a loader failure, a missing
# GPU or any other red is not this control's red (Harness/RefusedMaskControl.cmake).
function(mgl_itest_add_refused_mask_control name mask gtestFilter environment)
    string(MAKE_C_IDENTIFIER "${name}" mglRefusedLog)
    set(mglRefusedLog "${CMAKE_CURRENT_BINARY_DIR}/refused-${mglRefusedLog}.log")
    add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:MobileGLIntegrationTest>" "-DFILTER=${gtestFilter}"
        "-DMASK=${mask}" "-DLOG=${mglRefusedLog}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/Harness/RefusedMaskControl.cmake")
    # The lane's environment (joined with escaped separators for gtest_discover_tests), as a plain
    # list, with its log path pointed at a file this control alone reads.
    string(REPLACE "\\;" ";" environment "${environment}")
    string(REGEX REPLACE "MOBILEGL_LOG_FILE_PATH=[^;]*" "MOBILEGL_LOG_FILE_PATH=${mglRefusedLog}"
           environment "${environment}")
    if (NOT environment MATCHES "MOBILEGL_LOG_FILE_PATH=")
        set(environment "${environment};MOBILEGL_LOG_FILE_PATH=${mglRefusedLog}")
    endif()
    # Without it an unusable harness SKIPS and exits 0, which would read as "the mask ran green";
    # with it the refusal's abort is a red the script can then attribute by the log.
    set(environment "${environment};MOBILEGL_ITEST_REQUIRE_GPU=1")
    set_tests_properties("${name}" PROPERTIES
        LABELS "integration-gpu;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        RESOURCE_LOCK "refused-${name}.log"
        ENVIRONMENT "${environment}")
endfunction()

# --- Resource subsystem control and the map-persistent-roundtrip counting lanes ------------
# DirectGLES only, as ResourceSubsystemControlScenario.cpp explains. The On lane runs the default
# mask; 0x7f clears fixed-on subsystem bits, so it is registered as a refused-mask control
# (mgl_itest_add_refused_mask_control above). Each log-reading entry has a private log path and a
# single-case filter (ARCHITECTURE.md §16.1). MOBILEGL_PIPE_STATS_PERIOD=1 writes one summary line
# per eglSwapBuffers, so two swaps bracket exactly the workload.
mgl_itest_join_environment(MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_ON_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_RESOURCE_SUBSYSTEM_LANE=on"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/resource-subsystem-on-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_RESOURCE_SUBSYSTEM_LANE=off"
    "MOBILEGL_PIPE_PUSH=0x7f" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/resource-subsystem-off-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

mgl_itest_lane("DirectGLES.ResourceSubsystemControl.On." "ResourceSubsystemControlScenario.*"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_ON_ENVIRONMENT}")
mgl_itest_add_refused_mask_control("DirectGLES.SubsystemMaskRefusal.ResourceSubsystemControlOff" "0x7f" "ResourceSubsystemControlScenario.*"
    "${MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_OFF_ENVIRONMENT}")

# G10's two counting entries, one per scenario that has a claim about the counter:
# StorageBufferRegrow asserts N definitions cost N round trips (never one per draw), and
# LargeArenaAdoption asserts one adoption costs exactly one. Each names a single case in its
# TEST_FILTER, for the private-log reason above.
mgl_itest_join_environment(MGL_ITEST_GLES_MPR_REGROW_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_MPR_LANE=storage-buffer-regrow"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/mpr-storage-buffer-regrow-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_MPR_ARENA_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_MPR_LANE=large-arena-adoption"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/mpr-large-arena-adoption-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

mgl_itest_lane("DirectGLES.MapPersistentRoundtrips." "StorageBufferRegrowScenario.NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_MPR_REGROW_ENVIRONMENT}")
mgl_itest_lane("DirectGLES.MapPersistentRoundtrips." "LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_MPR_ARENA_ENVIRONMENT}")

# LargeArenaAdoption's three behavioural cases under BOTH arms of the same A/B. This file is where
# an adopted store's whole life is exercised - the NULL-data definition that adopts it, an
# in-flight SubData, a readback and a GPU write - so if the handle path and the legacy
# BufferBackendOps path disagree about any of it, one of these two lanes goes red and names which.
# No log path: none of these three cases reads one, and giving them one would only add a file for
# four processes to truncate. The fourth case skips in both lanes for exactly that reason, saying
# so.
#
# P11 A2: both lanes pin monolith, so both DECLARE the adopted arm (MGITEST_PERSISTENT_MAP_ARM) and
# LargeArenaAdoptionScenario.TheArenaLandsInTheTierItsLaneDeclares asserts the 24 MiB arena really
# adopted on the handle arm (Ops_H_MapPersistentTracked) and on the legacy one. Red once with
# MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION=1 in the job environment (CONTRACT-P11 §2).
mgl_itest_join_environment(MGL_ITEST_GLES_ARENA_SUBSYSTEM_ON_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith" "MGITEST_PERSISTENT_MAP_ARM=adopted"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_PUSH=0x3fff"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_ARENA_SUBSYSTEM_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith" "MGITEST_PERSISTENT_MAP_ARM=adopted"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_PUSH=0x7f"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
# ... and Magma's monolith half: the legacy AcquirePersistentMap (the wire resource table is
# registered only off monolith, VkBufferManager.cpp RegisterWireResourceOps).
mgl_itest_join_environment(MGL_ITEST_VULKAN_ARENA_ARM_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith" "MGITEST_PERSISTENT_MAP_ARM=adopted"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ITEST_REQUIRE_GPU=1"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

mgl_itest_lane("DirectGLES.ResourceSubsystemOn." "LargeArenaAdoptionScenario.*"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_ARENA_SUBSYSTEM_ON_ENVIRONMENT}")
mgl_itest_add_refused_mask_control("DirectGLES.SubsystemMaskRefusal.ResourceSubsystemOff" "0x7f" "LargeArenaAdoptionScenario.*"
    "${MGL_ITEST_GLES_ARENA_SUBSYSTEM_OFF_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.ArenaArm." "LargeArenaAdoptionScenario.TheArenaLandsInTheTierItsLaneDeclares"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_VULKAN_ARENA_ARM_ENVIRONMENT}")

# --- Object subsystem control, its refused masks and the no-consumer check -----------------
# The On lane runs the default mask. 0x1ff, 0x9ff (samplers without texture resources) and 0x5ff
# (texture resources without samplers) clear fixed-on subsystem bits, so they are refused-mask
# controls. The Consumer (DirectGLES) and NoConsumer (DirectVulkan) lanes run the default mask and
# assert that MGPipeApplierState::RefusedNoConsumer never moves: a count there means the client
# emitted an object record no backend consumes, and an accepted one would clear dirty flags that
# nothing uploads. The DirectGLES lane keeps the check from passing on a tree that emits nothing.
# Each log-reading lane has a private log path and one case (ARCHITECTURE.md §16.1).
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_ON_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=on"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-on-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=off"
    "MOBILEGL_PIPE_PUSH=0x1ff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-off-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=refused"
    "MOBILEGL_PIPE_PUSH=0x9ff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-refused-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_TEXTURE_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=refused-texture"
    "MOBILEGL_PIPE_PUSH=0x5ff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-refused-texture-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_CONSUMER_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=consumer"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-consumer-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_OBJECT_SUBSYSTEM_NO_CONSUMER_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_OBJECT_SUBSYSTEM_LANE=no-consumer"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-no-consumer-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

mgl_itest_lane("DirectGLES.ObjectSubsystemControl.On." "ObjectSubsystemControlScenario.ClearingTheP4aBitsStopsTheEmissionsAndNotThePixels"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_ON_ENVIRONMENT}")
mgl_itest_add_refused_mask_control("DirectGLES.SubsystemMaskRefusal.ObjectSubsystemControlOff" "0x1ff" "ObjectSubsystemControlScenario.ClearingTheP4aBitsStopsTheEmissionsAndNotThePixels"
    "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_OFF_ENVIRONMENT}")
mgl_itest_add_refused_mask_control("DirectGLES.SubsystemMaskRefusal.ObjectSubsystemControlRefused" "0x9ff" "ObjectSubsystemControlScenario.ASamplerBitWithoutTheTextureBitIsRefusedAndNamed"
    "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_ENVIRONMENT}")
mgl_itest_add_refused_mask_control("DirectGLES.SubsystemMaskRefusal.ObjectSubsystemControlRefusedTexture" "0x5ff" "ObjectSubsystemControlScenario.ATextureBitWithoutTheSamplerBitIsRefusedAndNamed"
    "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_TEXTURE_ENVIRONMENT}")
mgl_itest_lane("DirectGLES.ObjectSubsystemControl.Consumer." "ObjectSubsystemControlScenario.TheAppliersNoConsumerBeltNeverFiresBehindTheClientsGate"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_CONSUMER_ENVIRONMENT}")
mgl_itest_lane("DirectVulkan.ObjectSubsystemControl.NoConsumer." "ObjectSubsystemControlScenario.TheAppliersNoConsumerBeltNeverFiresBehindTheClientsGate"
    LABELS "integration-gpu\;integration-monolith-control" ENV "${MGL_ITEST_VULKAN_OBJECT_SUBSYSTEM_NO_CONSUMER_ENVIRONMENT}")

# --- Texture upload shape gate (ARCHITECTURE.md §6) ----------------------------------------
# SSIM cannot tell one union box from many rects, so TextureUploadShapeScenario checks the
# server's tex[emit= box= rect= jobs=] and the client's emit[ctu=] against a measured gold row.
# One texture in the counted window is written through a glTextureView, so
# MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 is required; without it the case skips rather than run a
# smaller workload against the same gold row. Private log path and single-case filter:
# ARCHITECTURE.md §16.1.
mgl_itest_join_environment(MGL_ITEST_GLES_TEXTURE_UPLOAD_SHAPE_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_TEXTURE_UPLOAD_SHAPE_LANE=1"
    "MOBILEGL_PIPE_PUSH=0x3fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/texture-upload-shape-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

mgl_itest_lane("DirectGLES.TextureUploadShape." "TextureUploadShapeScenario.TheEmittedUploadShapeIsRecordedAndTheTwoSidesAgree"
    LOCK texture-upload-shape-DirectGLES.log ENV "${MGL_ITEST_GLES_TEXTURE_UPLOAD_SHAPE_ENVIRONMENT}")

# --- TextureParamsWithoutASamplerViewScenario needs no lane of its own ---------------------
# Its observable is a sampled colour, not a counter or a log, and its claim is about the shipping
# configuration, so the ambient DirectGLES. / DirectVulkan. registrations at the default mask are
# the arms it needs. The DirectVulkan entries skip in SetUp: the scenario covers Espryt's
# SyncAttachmentObject / SyncNeccessaryTextures pair.

if (MOBILEGL_PIPE_VERIFY)
    # 900s, not the ambient 120: the comparator re-reads every field of the fill mask at the verb
    # boundary and again at every accessor read, which the design budgets at 5-10x.
    set(MGL_ITEST_VERIFY_TIMEOUT 900)

    # What "the monolith verify lane" means, per backend, in one place: the ambient Verify. entries
    # and every control entry below are this list plus their own log path and markers. So the
    # controls run the configuration the ambient entries run, and an edit that leaves that lane
    # unarmed or non-fatal turns PipeVerifyArmingScenario.Armed red, which asserts both.
    set(MGL_ITEST_GLES_VERIFY_BASE_ENV
        "MOBILEGL_TRANSPORT=monolith" "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1"
        ${MGL_ITEST_COMMON_ENV})
    set(MGL_ITEST_VULKAN_VERIFY_BASE_ENV
        "MOBILEGL_TRANSPORT=monolith" "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_PIPE_VERIFY=1"
        ${MGL_ITEST_VULKAN_ENV})

    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_ENVIRONMENT ${MGL_ITEST_GLES_VERIFY_BASE_ENV}
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-DirectGLES.log")
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_ENVIRONMENT ${MGL_ITEST_VULKAN_VERIFY_BASE_ENV}
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-DirectVulkan.log")

    # The arming assertion's own lane, one case per backend, with a log path nothing else writes to.
    #
    # PipeVerifyArmingScenario.Armed reads the library's log, and the log is a per-LANE resource: it
    # is opened fopen(path, "w"), so every process in a lane truncates it. In the ambient Verify.
    # lane that is 400-odd processes on one path, run `-j 4` in CI, and a whole-file read there
    # races a neighbour's bring-up. Every other log-reading scenario in this file (UnlocatedIoBlocks,
    # the primgen reroute, the point-size demotion) is registered exactly like this for the same
    # reason. MGITEST_PIPE_ARMING_LANE is a harness marker - the library never reads it - and it is
    # what makes the case skip in the ambient lane instead of racing there.
    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_ARMING_ENVIRONMENT ${MGL_ITEST_GLES_VERIFY_BASE_ENV}
        "MGITEST_PIPE_ARMING_LANE=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-arming-DirectGLES.log")
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_ARMING_ENVIRONMENT ${MGL_ITEST_VULKAN_VERIFY_BASE_ENV}
        "MGITEST_PIPE_ARMING_LANE=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-arming-DirectVulkan.log")

    # Negative control A (G4). MOBILEGL_PIPE_VERIFY_FATAL=0 so the process SURVIVES its own
    # divergence and the case can read the report back out of the log; the other half, that with
    # FATAL at its default of 1 a divergence aborts, is PipeInputsTest's
    # VerifyCorruptKnobNamesTheFieldAtEntry.
    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_CORRUPT_ENVIRONMENT ${MGL_ITEST_GLES_VERIFY_BASE_ENV}
        "MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters" "MOBILEGL_PIPE_VERIFY_FATAL=0"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-corrupt-DirectGLES.log")
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_CORRUPT_ENVIRONMENT ${MGL_ITEST_VULKAN_VERIFY_BASE_ENV}
        "MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters" "MOBILEGL_PIPE_VERIFY_FATAL=0"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-corrupt-DirectVulkan.log")

    # Negative control B (G5). The omission skips the STAMP of one field for one verb while still
    # copying its value, which is indistinguishable from a fill row nobody wrote; the scenario
    # forks, so the resulting std::abort() is a datum in waitpid() rather than a dead lane. The
    # pair is a harness marker, MGITEST_POISON_OMIT: the library has no knob for it, and the
    # re-executed child arms it through the verify build's setter (Harness/PoisonOmissionArm.h).
    # P13 W5: the pair is the pack state at ReadPixels, which monolith's record arm still reads from
    # the fill (around its neutral-pack readback). GetActiveTextureUnit at GenerateMipmap was the
    # frontend arm's read; the record arm resolves the texture from the verb's own handle and never
    # reads it, so that pair could no longer go red for its own reason.
    mgl_itest_join_environment(MGL_ITEST_GLES_POISON_OMIT_ENVIRONMENT ${MGL_ITEST_GLES_VERIFY_BASE_ENV}
        "MGITEST_POISON_OMIT=ReadPixels:GetPixelStoreParameters"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-poison-omit-DirectGLES.log")
    mgl_itest_join_environment(MGL_ITEST_VULKAN_POISON_OMIT_ENVIRONMENT ${MGL_ITEST_VULKAN_VERIFY_BASE_ENV}
        "MGITEST_POISON_OMIT=ReadPixels:GetPixelStoreParameters"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-poison-omit-DirectVulkan.log")

    # The whole suite again, per backend, with the comparator armed. Same scenarios, same
    # assertions, but every backend read of frontend state is now checked against a snapshot taken
    # from the live context at the verb boundary - which is what "the 742 integration entries
    # prove push equals pull" means. Labelled integration-gpu as well so a verify build's
    # `ctest -L integration-gpu` still describes the whole registration set.
    mgl_itest_lane("DirectGLES.Verify." "-${MGL_ITEST_GLES_VIEW_FILTER}:${MGL_ITEST_SPLIT_ARM_ONLY_FILTER}"
        LABELS "integration-gpu\;integration-verify\;integration-monolith-control" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} ENV "${MGL_ITEST_GLES_VERIFY_ENVIRONMENT}")
    mgl_itest_lane("DirectGLES.Verify." "${MGL_ITEST_GLES_VIEW_FILTER}"
        LABELS "integration-verify\;integration-monolith-control" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} ENV "${MGL_ITEST_GLES_VERIFY_ENVIRONMENT}\;MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1")
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.Verify."
        TEST_FILTER "-${MGL_ITEST_SPLIT_ARM_ONLY_FILTER}"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_VULKAN_VERIFY_ENVIRONMENT}"
    )

    # The arming assertion, one entry per backend. This is the entry that fails a lane whose library
    # never armed: it runs the same library and the same base list as the ambient entries above,
    # but unlike them it cannot be green against a library with no comparator compiled in, or with
    # one that only logs a divergence. Its log is its own, so `-j 4` cannot make it flake.
    mgl_itest_lane("DirectGLES.VerifyArming." "PipeVerifyArmingScenario.Armed"
        LABELS "integration-gpu\;integration-verify\;integration-monolith-control" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} ENV "${MGL_ITEST_GLES_VERIFY_ARMING_ENVIRONMENT}")
    mgl_itest_lane("DirectVulkan.VerifyArming." "PipeVerifyArmingScenario.Armed"
        LABELS "integration-gpu\;integration-verify\;integration-monolith-control" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} ENV "${MGL_ITEST_VULKAN_VERIFY_ARMING_ENVIRONMENT}")

    # One case each: the knobs are process-wide, so a corrupted or poisoned process cannot also be
    # running the ambient assertions. These four entries are the ones that assert the RED - they
    # pass when the comparator and the poison report, and go red when either stops.
    mgl_itest_lane("DirectGLES.VerifyCorrupted." "PipeVerifyArmingScenario.CorruptedFieldIsReported"
        LABELS "integration-gpu\;integration-verify\;integration-monolith-control" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} ENV "${MGL_ITEST_GLES_VERIFY_CORRUPT_ENVIRONMENT}")
    mgl_itest_lane("DirectVulkan.VerifyCorrupted." "PipeVerifyArmingScenario.CorruptedFieldIsReported"
        LABELS "integration-gpu\;integration-verify\;integration-monolith-control" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} ENV "${MGL_ITEST_VULKAN_VERIFY_CORRUPT_ENVIRONMENT}")
    mgl_itest_lane("DirectGLES.PoisonOmitted." "PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb"
        LABELS "integration-gpu\;integration-verify\;integration-monolith-control" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} ENV "${MGL_ITEST_GLES_POISON_OMIT_ENVIRONMENT}")
    mgl_itest_lane("DirectVulkan.PoisonOmitted." "PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb"
        LABELS "integration-gpu\;integration-verify\;integration-monolith-control" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} ENV "${MGL_ITEST_VULKAN_POISON_OMIT_ENVIRONMENT}")
endif()

# --- Split lanes and the persistent-map arm lanes ------------------------------------------
# Split entries are registered only in a MOBILEGL_BUILD_DISAGGREGATED build and carry a second
# label, so `ctest -L integration-split --no-tests=error` fails in any other build instead of
# passing with no entries (ARCHITECTURE.md §16.1). A Split case arms from the running process,
# not from source text that an edit could change: Harness/SplitRuntimePeek.cpp reads
# MG_Config::Transport, ClientSession::Active() and ImplementedVerbCount(), and the case skips
# naming the first that does not hold. The record-arm half of the peek (dataArmIsRecord) needs no
# transport, so it is compiled in every build.
target_compile_definitions(MobileGLIntegrationTest PRIVATE -DMGITEST_RECORD_ARM_PEEK=1)
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_compile_definitions(MobileGLIntegrationTest PRIVATE -DMGITEST_SPLIT_RUNTIME_PEEK=1)
    message(STATUS "Integration tests: the DirectGLES.Split. lanes arm from the RUNNING PROCESS - "
                   "MG_Config::Transport, ClientSession::Active() and ImplementedVerbCount() - and "
                   "skip naming the first of those that is not true")
endif()

# Q-3, made a build fact rather than documentation. ConfigLoader logs the resolved transport at
# INFO, and that line is what run_trace_case.cmake reads back as proof a retrace really went
# split. At WARN or above it is compiled out and every split retrace reds for a reason that is not
# a defect; at DEBUG, ConfigLoader's unconditional env dump prints a confusable KEY=VALUE line in
# a PULL build too (review M-5, also fixed on the reading side). The integration lanes themselves
# no longer depend on the log level at all - they read the variable - so this is a WARNING.
if (MOBILEGL_BUILD_DISAGGREGATED AND DEFINED MOBILEGL_LOG_ACTIVE_LEVEL
    AND NOT MOBILEGL_LOG_ACTIVE_LEVEL STREQUAL "MOBILEGL_LOG_LEVEL_INFO"
    AND NOT MOBILEGL_LOG_ACTIVE_LEVEL STREQUAL "MOBILEGL_LOG_LEVEL_DEBUG")
    message(WARNING "Integration tests: MOBILEGL_LOG_ACTIVE_LEVEL=${MOBILEGL_LOG_ACTIVE_LEVEL} in a "
                    "disaggregated build. ConfigLoader's transport line is MGLOG_I, so the "
                    "trace-replay split arm's transport-resolution assertion cannot see it and every "
                    "split retrace will red. Use MOBILEGL_LOG_LEVEL_INFO.")
endif()

# Package b1's client-side persistent-map tracker, which is what Harness/PersistentMapPeek.cpp
# asks the membership question of (b1-v1.md 4.1 item 2). Every library builds it since P13 W5 (the
# tracker is record core, MG_Record); the content probe stays so a tree without it skips by name.
mgl_itest_probe_for_symbol(MGL_ITEST_PERSISTENT_MAP_TRACKER
    "${MGL_ITEST_ROOT}/MobileGL/MG_State/GLState/BufferState" "IsLivePersistentMap")
if (MGL_ITEST_PERSISTENT_MAP_TRACKER)
    message(STATUS "Integration tests: package b1's persistent-map tracker is present "
                   "(${MGL_ITEST_PERSISTENT_MAP_TRACKER}) - PersistentCoherentMapScenario's "
                   "membership assertion is live")
    target_compile_definitions(MobileGLIntegrationTest PRIVATE -DMGITEST_PERSISTENT_MAP_TRACKER=1)
else()
    message(STATUS "Integration tests: no source under MobileGL/MG_State/GLState/BufferState names "
                   "IsLivePersistentMap - package b1's tracker has not landed, so "
                   "PersistentCoherentMapScenario's membership assertion SKIPS")
endif()

# ARCHITECTURE.md §15.1 - every new ctest ENVIRONMENT and add_trace_replay_test's SPLIT branch
# carries MOBILEGL_IPC_SERVER_PATH, because the dladdr fallback cannot find the server from a
# binary that links MobileGL_s statically. P5 is inproc-only and nothing reads the value yet; it
# is carried now so that an unparsed variable and a parsed-and-ignored one stop being
# indistinguishable the day P6 consumes it. A generator expression is deliberately NOT used here:
# a $<TARGET_FILE_DIR:...> inside a gtest_discover_tests PROPERTIES value is written into the
# generated ctest include file verbatim and never expanded.
set(MGL_ITEST_SPLIT_SERVER_PATH "${CMAKE_BINARY_DIR}/libMobileGLServer.so")

# The shared GLES split, spawn and tcp environments carry MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1:
# Espryt advertises GL_ARB_texture_view only with it, and without it the texture-view scenarios
# skip on every arm. The DirectVulkan lists leave it out because only Espryt reads it.
# MOBILEGL_ITEST_REQUIRE_GPU=1 makes a failed harness pre-flight (such as a handshake-time Fatal)
# fail every case instead of skipping it, since ctest counts a skip as a pass
# (ARCHITECTURE.md §16.1).
mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc"
    "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
    "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

# The persistent-map counting pair: this monolith entry and DirectGLES.Split.PersistentMapArm.
# further down run the same workload and assert the same mpr constant, so the split arm's count
# equals the monolith arm's while each half can fail on its own. The monolith entry declares no
# arm (which arm a small coherent map takes depends on driver and build; the case records it);
# the split entry declares the emulated one. MOBILEGL_TRANSPORT=monolith is pinned so a job-level
# transport export cannot turn this half into a split entry. Private log, single case and
# RESOURCE_LOCK: ARCHITECTURE.md §16.1.
mgl_itest_join_environment(MGL_ITEST_GLES_PMAP_ARM_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_PMAP_LANE=1" "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/persistent-map-arm-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

mgl_itest_lane("DirectGLES.PersistentMapArm." "PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares"
    LOCK persistent-map-arm-DirectGLES.log ENV "${MGL_ITEST_GLES_PMAP_ARM_ENVIRONMENT}")

if (MOBILEGL_BUILD_DISAGGREGATED)
    # The spawn arm. mgl_itest_register_split_arms emits the inproc, spawn and tcp arms from one
    # filter, so their case-name sets stay identical (scripts/ci/spawn_lane_parity.py checks it).
    # MOBILEGL_TRANSPORT=spawn makes the test process the client and has it launch the server from
    # MOBILEGL_IPC_SERVER_PATH. The server inherits this environment minus the transport and IPC
    # keys (ServerSpawn.cpp), so the texture-view opt-in (see MGL_ITEST_GLES_SPLIT_ENVIRONMENT)
    # reaches both processes.
    mgl_itest_join_environment(MGL_ITEST_GLES_SPAWN_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=spawn"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    # A single supervisor serves one session at a time. Fixture teardown also reaps a
    # child left behind by a failed client; the resource lock serializes this lane.
    # Below Linux's ephemeral range (32768-60999): thousands of loopback client connections run
    # in this lane, and an outgoing socket that happens to own the port makes the supervisor's
    # bind fail (TcpServer.Start.* red in 0.07 s on a hosted runner).
    set(MOBILEGL_ITEST_TCP_ENDPOINT "tcp://127.0.0.1:30613" CACHE STRING "Loopback TCP integration endpoint")
    set(MOBILEGL_ITEST_TCP_DEVICE_ENDPOINT "" CACHE STRING "Optional device TCP integration endpoint")
    set(MOBILEGL_ITEST_TCP_DEVICE_TOKEN "" CACHE STRING "Token for the device TCP integration endpoint")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(MGL_TCP_FIXTURE "${CMAKE_SOURCE_DIR}/scripts/ci/tcp_server_fixture.py")
    set(MGL_TCP_STATE "${CMAKE_CURRENT_BINARY_DIR}/tcp-server.json")
    add_test(NAME TcpServer.Start COMMAND "${Python3_EXECUTABLE}" "${MGL_TCP_FIXTURE}"
        start --server "${MGL_ITEST_SPLIT_SERVER_PATH}" --endpoint "${MOBILEGL_ITEST_TCP_ENDPOINT}"
        --state "${MGL_TCP_STATE}")
    add_test(NAME TcpServer.Stop COMMAND "${Python3_EXECUTABLE}" "${MGL_TCP_FIXTURE}"
        stop --state "${MGL_TCP_STATE}")
    # TcpServer.Start pre-starts the tcp arm's server, so it does not inherit a client entry's
    # environment. tcp_server_fixture.py passes ctest's environment to the supervisor minus its
    # transport and backend keys, so every knob the server reads is set on this entry: the
    # texture-view opt-in and the MGL_ITEST_VULKAN_ENV pins (ICD, lavapipe repair) that Magma reads
    # in the server. Without them the Tcp arm skips texture-view cases or misses goldens that the
    # Split and Spawn arms pass. Espryt's server ignores the Vulkan pins.
    set(mglItestTcpServerEnvironment "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1")
    foreach(mglItestVulkanPin IN LISTS MGL_ITEST_VULKAN_ENV)
        if(NOT mglItestVulkanPin IN_LIST MGL_ITEST_COMMON_ENV)
            list(APPEND mglItestTcpServerEnvironment "${mglItestVulkanPin}")
        endif()
    endforeach()
    set_tests_properties(TcpServer.Start PROPERTIES FIXTURES_SETUP mobilegl-tcp TIMEOUT 20
        ENVIRONMENT "${mglItestTcpServerEnvironment}")
    set_tests_properties(TcpServer.Stop PROPERTIES FIXTURES_CLEANUP mobilegl-tcp TIMEOUT 20)
    add_test(NAME TcpLane.AccountingControls COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/ci/tcp_lane_tools_test.py")
    set_tests_properties(TcpLane.AccountingControls PROPERTIES LABELS unit TIMEOUT 20)
    # P7 wave 0. scripts/ci/tcp_supervisor_smoke.py has been in the tree since P6.5 asserting the
    # supervisor's Busy / token / wire-version / build-policy controls, and was referenced by
    # NOTHING - no ctest entry, no workflow step - so nobody could have said whether those four
    # controls were green. It also now carries Ph slice (1)'s negative control: a session child
    # killed with SIGABRT must be NAMED in the supervisor log by pid and signal, and the next
    # Hello must still be welcomed. It starts its own supervisor on its own free port, so it
    # needs no fixture, but it takes the lane's resource lock because a second supervisor's
    # session child would answer the other's peers with Busy and make both lanes flaky.
    # It needs flatc, which is deliberately not in the build graph (gen_protocol.py's header):
    # with none found the script exits 77 and this entry reports SKIP rather than passing on a
    # test that did not run.
    add_test(NAME TcpLane.SupervisorProtocolControls COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/ci/tcp_supervisor_smoke.py"
        --server "${MGL_ITEST_SPLIT_SERVER_PATH}"
        --out "${CMAKE_CURRENT_BINARY_DIR}/tcp-supervisor-smoke")
    set_tests_properties(TcpLane.SupervisorProtocolControls PROPERTIES
        LABELS integration-tcp TIMEOUT 180 SKIP_RETURN_CODE 77
        RESOURCE_LOCK mobilegl-tcp)
    # Ph fuzz arm 3 (PH-6, ID-P7-2): A PEER THAT STOPS DRAINING SEG_EVENT, on both data planes.
    # MG_Test/Wire/EventForfeitPeerTest.cpp starts its own `--serve` supervisor (an abstract unix
    # endpoint for the shm plane, a free loopback port for the stream plane) with a NON-default
    # MOBILEGL_IPC_EVENT_WAIT_MS, and asserts the whole arm: the server forfeits the reverse
    # channel by name against that knob, counts the drop, ends the session with exit 0 inside the
    # knob rather than the old 30 s Fatal, and the SAME supervisor welcomes the next connection.
    # Each entry also runs the kill-the-peer case (knob 8000, over ServerLoop's 5000 ms join: the
    # server must say PeerGone at once and exit 0), and the shm entry the LaunchServer case (the
    # knob must cross ServerSpawn.cpp's scrub). The gtest filters are per plane. The names are not
    # `DirectGLES.<Arm>.<Scenario>` on purpose: spawn_lane_parity.py compares the scenario arms and
    # this is a supervisor control, one per lane, like the entry above. It needs no fixture (its
    # supervisor is its own), and the CI lane names both entries to junit_tally.py
    # --require-entry-passed so a missing build or a skip is red by name. The Tcp entry still takes
    # the lane's resource lock, as the entry above does: every integration-tcp entry must, or
    # spawn_lane_parity.py and junit_tally.py --discovery refuse the registration.
    if (TARGET EventForfeitPeerTest)
        # A plain list, not mgl_itest_join_environment's escaped one: that form is for values that
        # travel through gtest_discover_tests' PROPERTIES, and these two entries are add_test's.
        set(MGL_ITEST_EVENT_FORFEIT_ENVIRONMENT
            "MOBILEGL_TEST_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" ${MGL_ITEST_COMMON_ENV})
        add_test(NAME SpawnLane.EventForfeitPeer COMMAND $<TARGET_FILE:EventForfeitPeerTest>
            --gtest_filter=EventForfeitPeer.Shm*)
        set_tests_properties(SpawnLane.EventForfeitPeer PROPERTIES
            LABELS integration-spawn TIMEOUT 120
            ENVIRONMENT "${MGL_ITEST_EVENT_FORFEIT_ENVIRONMENT}")
        add_test(NAME TcpLane.EventForfeitPeer COMMAND $<TARGET_FILE:EventForfeitPeerTest>
            --gtest_filter=EventForfeitPeer.Stream*)
        set_tests_properties(TcpLane.EventForfeitPeer PROPERTIES
            LABELS integration-tcp TIMEOUT 120 RESOURCE_LOCK mobilegl-tcp
            ENVIRONMENT "${MGL_ITEST_EVENT_FORFEIT_ENVIRONMENT}")
    endif()
    # P7 F2: Ph fuzz arm 1 (PLAN-PH-P34B-P7.md §1.1, "gate: fuzz arm 1 malformed control
    # frames"). Five malformed control frames - bad identifier, truncated length prefix, oversize
    # length, garbage flatbuffer, wrong union type - sent to a live supervisor as a FIRST frame
    # (answered by name with no child forked, PH-7 (5)) and inside an established session (the
    # session ends with a named line and is not counted as a fault), with a well-formed client
    # welcomed after each phase. Same shape as TcpLane.SupervisorProtocolControls: its own supervisor on its own
    # port, the lane's resource lock, flatc or SKIP (77) - and CI requires it PASSED by name.
    add_test(NAME TcpLane.ControlFrameFuzz COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/ci/ph_fuzz_control_frames.py"
        --server "${MGL_ITEST_SPLIT_SERVER_PATH}"
        --out "${CMAKE_CURRENT_BINARY_DIR}/tcp-control-frame-fuzz")
    set_tests_properties(TcpLane.ControlFrameFuzz PROPERTIES
        LABELS integration-tcp TIMEOUT 180 SKIP_RETURN_CODE 77
        RESOURCE_LOCK mobilegl-tcp)
    # D2: the texture-view opt-in on both TCP lists too, so the three shared GLES split arms
    # carry one configuration. See the note over MGL_ITEST_GLES_SPLIT_ENVIRONMENT.
    #
    # THE TWO TCP LISTS ARE THE CLIENT'S HALF ONLY, and the two arms differ in whether that is
    # enough. The loopback Tcp arm's server is started by the TcpServer.Start fixture below,
    # which this package also gives the knob - so both halves have it. The TcpDevice arm's
    # server is a supervisor already running ON A DEVICE, started outside this build by whoever
    # set MOBILEGL_ITEST_TCP_DEVICE_ENDPOINT; nothing here can reach its environment. If that
    # supervisor was started without MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 it will withhold
    # GL_ARB_texture_view and every texture-view case in the TcpDevice arm will GREEN-SKIP,
    # exactly as the loopback arm did before the fixture entry got the knob. The arm is
    # unconfigured in this repo (the endpoint defaults empty, so the arm is not created at all),
    # so this is a note for whoever configures it rather than a defect today.
    mgl_itest_join_environment(MGL_ITEST_GLES_TCP_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=spawn"
        "MOBILEGL_IPC_CONTROL=${MOBILEGL_ITEST_TCP_ENDPOINT}" "MOBILEGL_IPC_DATA=stream"
        "MOBILEGL_IPC_REQUIRE_SAME_BUILD=1" "MOBILEGL_IPC_LOG_FORWARD=1" "MOBILEGL_IPC_TOKEN="
        "MGITEST_SPLIT_LANE=1" "MGITEST_TCP_LANE=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_GLES_TCPDEVICE_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=spawn"
        "MOBILEGL_IPC_CONTROL=${MOBILEGL_ITEST_TCP_DEVICE_ENDPOINT}" "MOBILEGL_IPC_DATA=stream"
        "MOBILEGL_IPC_REQUIRE_SAME_BUILD=1" "MOBILEGL_IPC_LOG_FORWARD=1"
        "MOBILEGL_IPC_TOKEN=${MOBILEGL_ITEST_TCP_DEVICE_TOKEN}"
        "MGITEST_SPLIT_LANE=1" "MGITEST_TCP_LANE=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    # ---- The same three arms for Magma ----------------------------------------------------
    # They append ${MGL_ITEST_VULKAN_ENV} rather than the common list, so they run on the same ICD
    # and lavapipe repair as the monolith DirectVulkan. arm instead of whatever ICD the loader
    # finds first. MOBILEGL_ITEST_REQUIRE_GPU=1 makes a host without usable Vulkan fail rather than
    # skip every entry (ARCHITECTURE.md §16.1).
    mgl_itest_join_environment(MGL_ITEST_VULKAN_SPLIT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_TRANSPORT=inproc"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_ITEST_REQUIRE_GPU=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_SPAWN_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_TRANSPORT=spawn"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_ITEST_REQUIRE_GPU=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_TCP_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_TRANSPORT=spawn"
        "MOBILEGL_IPC_CONTROL=${MOBILEGL_ITEST_TCP_ENDPOINT}" "MOBILEGL_IPC_DATA=stream"
        "MOBILEGL_IPC_REQUIRE_SAME_BUILD=1" "MOBILEGL_IPC_LOG_FORWARD=1" "MOBILEGL_IPC_TOKEN="
        "MGITEST_SPLIT_LANE=1" "MGITEST_TCP_LANE=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

    # THE TRANSPORT DIMENSION ON ITS OWN, for the Magma lanes further down this file that already
    # have an environment of their OWN FLAVOUR (strict + dual-block state, run-ahead credits,
    # cache pinning) and need only the transport half re-stated per arm. Kept beside the joined
    # lists above so there is ONE place that says what "the spawn arm" and "the tcp arm" mean.
    set(MGL_ITEST_SPLIT_ARM_TRANSPORT_ENV "MOBILEGL_TRANSPORT=inproc"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}")
    set(MGL_ITEST_SPAWN_ARM_TRANSPORT_ENV "MOBILEGL_TRANSPORT=spawn"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}")
    set(MGL_ITEST_TCP_ARM_TRANSPORT_ENV "MOBILEGL_TRANSPORT=spawn"
        "MOBILEGL_IPC_CONTROL=${MOBILEGL_ITEST_TCP_ENDPOINT}" "MOBILEGL_IPC_DATA=stream"
        "MOBILEGL_IPC_REQUIRE_SAME_BUILD=1" "MOBILEGL_IPC_LOG_FORWARD=1" "MOBILEGL_IPC_TOKEN="
        "MGITEST_TCP_LANE=1")

    # mgl_itest_magma_arm_shape(<Split|Spawn|Tcp> [words...]) sets, for a hand-laid Magma lane:
    #   mglItestArmLower         split | spawn | tcp
    #   mglItestArmLogTag        "" on Split, "-spawn" / "-tcp" otherwise, for private log names
    #   mglItestArmProperties    the tcp fixture and resource lock; empty otherwise
    #   mglItestArmTransportEnv  the arm's transport pins, appended to the lane's own environment
    #   mglItestMagmaLabels      integration-magma-<arm> plus one label per word: Split keeps
    #                            integration-magma-<word>, the others integration-magma-<arm>-<word>
    #                            with the arm first (ARCHITECTURE.md §16.1)
    macro(mgl_itest_magma_arm_shape mglItestArm)
        string(TOLOWER "${mglItestArm}" mglItestArmLower)
        set(mglItestArmProperties)
        set(mglItestArmLogTag "")
        set(mglItestArmTransportEnv ${MGL_ITEST_SPLIT_ARM_TRANSPORT_ENV})
        if("${mglItestArm}" STREQUAL "Spawn")
            set(mglItestArmTransportEnv ${MGL_ITEST_SPAWN_ARM_TRANSPORT_ENV})
            set(mglItestArmLogTag "-spawn")
        elseif("${mglItestArm}" STREQUAL "Tcp")
            set(mglItestArmTransportEnv ${MGL_ITEST_TCP_ARM_TRANSPORT_ENV})
            set(mglItestArmLogTag "-tcp")
            set(mglItestArmProperties FIXTURES_REQUIRED mobilegl-tcp RESOURCE_LOCK mobilegl-tcp)
        endif()
        set(mglItestMagmaLabels "integration-magma-${mglItestArmLower}")
        foreach(mglItestWord IN ITEMS ${ARGN})
            if("${mglItestArm}" STREQUAL "Split")
                set(mglItestMagmaLabels "${mglItestMagmaLabels}\\;integration-magma-${mglItestWord}")
            else()
                set(mglItestMagmaLabels
                    "${mglItestMagmaLabels}\\;integration-magma-${mglItestArmLower}-${mglItestWord}")
            endif()
        endforeach()
    endmacro()
    set(MGL_ITEST_MAGMA_ARMS Split Spawn Tcp)

    # scenarioFilter, a PREFIX TAIL (empty for most; "F1." and "Ct." for the two per-slot loops,
    # which name their sub-lane in the test name), then zero or more EXTRA LABEL STEMS. A stem is
    # suffixed with the arm's own name (`integration-clientarrays` becomes `-split` on one arm and
    # `-spawn` on the other), so a lane-scoped label cannot accidentally name both lanes at once.
    # A UNIQUE LIST VARIABLE PER CALL, and the names collected for SplitLogPaths to walk.
    #
    # gtest_discover_tests' TEST_LIST is not a configure-time value: the generated include file
    # sets it when CTEST runs. So one shared variable would hold only the LAST block's entries -
    # measured, and it is why the first version of this macro silently unhooked every private log
    # path but one. The counter gives each call its own name; the names are what get walked.
    set(MGL_SPLIT_ARM_TESTS "")
    set(MGL_SPLIT_ARM_TEST_LISTS "")
    set(MGL_SPLIT_ARM_INDEX 0)
    set(MGL_SPLIT_ARM_CALLS "")
    # mgl_itest_register_split_arms_for_backend registers one filter on every transport arm.
    # DirectGLES entries carry the gated labels: integration-gpu, integration-<arm> and the
    # caller's stems suffixed with the arm. DirectVulkan entries carry only
    # integration-magma-all-<arm>, an informational tier outside every CI gate, with no extra stems
    # (they would collide with the DirectGLES labels) and no TcpDevice arm; Magma's gated lanes are
    # laid out by hand below. `all` sits mid-label so `ctest -L integration-magma-<arm>` cannot
    # select this tier (ARCHITECTURE.md §16.1). mgl_itest_generic_split_filter leaves out
    # configuration-specific cases, which run only in their pinned entries.
    function(mgl_itest_generic_split_filter input backend output)
        set(excluded
            LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip
            LargeArenaAdoptionScenario.AStreamSessionRefusesTheAdoptTierOnceByName
            LargeArenaAdoptionScenario.ASessionThatCannotRunT0FallsBackToT2OnceByName
            FencePollScenario.PacedFrameFenceWaitsBench
            GenerateMipmapServerScenario.Rgb16fDrawnBaseWithARefusedReadbackDeclinesByName
            GenerateMipmapServerScenario.Rgb16fUploadedBaseWithARefusedReadbackIsFilteredFromTheStore
            ViewportArrayScenario.WithoutTheEmulationEveryIndexCollapsesOntoViewportZero
            UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn
            PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn
            WireMipShapeScenario.Depth1DChainWithoutANativeBlitDeclinesByName)
        if(backend STREQUAL "DirectGLES")
            list(APPEND excluded
                PointSizeDemotionScenario.ACaptureSurvivesAStageThatOnlyReadsThePointSize
                WireIndirectDrawScenario.ShaderWrittenCommandsWithoutAnApplicationBarrierAreOrdered
                WireIndirectDispatchScenario.ShaderWrittenGroupCountsWithoutAnApplicationBarrierAreOrdered)
        endif()
        list(JOIN excluded ":" excluded)
        if(input MATCHES "-")
            set(${output} "${input}:${excluded}" PARENT_SCOPE)
        else()
            set(${output} "${input}-${excluded}" PARENT_SCOPE)
        endif()
    endfunction()

    macro(mgl_itest_register_split_arms_for_backend mglItestBackend scenarioFilter prefixTail)
        # QUOTED DEREFERENCE, NOT A BARE NAME: a macro parameter is a textual substitution and
        # not a variable, so `if(mglItestBackend STREQUAL ...)` would compare the literal string
        # "mglItestBackend" and take the else branch on every call.
        if("${mglItestBackend}" STREQUAL "DirectGLES")
            set(mglItestEnvStem GLES)
            set(mglItestLaneStem "integration")
            set(mglItestSharedLabels "integration-gpu\\;")
            set(mglItestArms Split Spawn Tcp)
            if(MOBILEGL_ITEST_TCP_DEVICE_ENDPOINT)
                list(APPEND mglItestArms TcpDevice)
            endif()
        else()
            set(mglItestEnvStem VULKAN)
            set(mglItestLaneStem "integration-magma-all")
            set(mglItestSharedLabels "")
            set(mglItestArms Split Spawn Tcp)
        endif()
        foreach(mglItestArm IN LISTS mglItestArms)
            string(TOLOWER "${mglItestArm}" mglItestArmLower)
            string(TOUPPER "${mglItestArm}" mglItestArmUpper)
            set(mglItestArmProperties)
            if(mglItestArm STREQUAL "Tcp")
                set(mglItestArmProperties FIXTURES_REQUIRED mobilegl-tcp RESOURCE_LOCK mobilegl-tcp)
            elseif(mglItestArm STREQUAL "TcpDevice")
                set(mglItestArmLower tcp-device)
                set(mglItestArmProperties RESOURCE_LOCK mobilegl-tcp-device)
            endif()
            set(mglItestArmLabels "${mglItestSharedLabels}${mglItestLaneStem}-${mglItestArmLower}")
            foreach(mglItestExtra IN ITEMS ${ARGN})
                set(mglItestArmLabels "${mglItestArmLabels}\\;${mglItestExtra}-${mglItestArmLower}")
            endforeach()
            # ONE ACCUMULATED LIST FOR BOTH ARMS, and it is not bookkeeping: SplitLogPaths gives
            # every entry a PRIVATE MOBILEGL_LOG_FILE_PATH, and SplitLogPaths.PrivateAndDistinct
            # reds if one is missing or shared. The per-block TEST_LIST variables this macro
            # replaced were what fed it - dropping them silently unhooked the whole mechanism,
            # which that gate caught. Accumulating here means a block added later is covered
            # without anyone remembering to add its variable to the list.
            #
            # IT MATTERS MORE ON THE SPAWN ARM: there TWO PROCESSES append to the path, so two
            # entries sharing one file is not merely ambiguous, it interleaves two sessions.
            mgl_itest_generic_split_filter("${scenarioFilter}" "${mglItestBackend}" mglItestGenericFilter)
            mgl_itest_lane("${mglItestBackend}.${mglItestArm}.${prefixTail}" "${mglItestGenericFilter}"
                LABELS "${mglItestArmLabels}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_${mglItestEnvStem}_${mglItestArmUpper}_ENVIRONMENT}")
            list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
            math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
        endforeach()
    endmacro()

    # THE NAME EVERY EXISTING CALL SITE USES, unchanged in signature so that this package adds a
    # dimension without touching one registration. It also RECORDS its call, which is what makes
    # tier 2 structural rather than maintained: the informational Magma tier at the end of this
    # file replays the recorded list, so a scenario added to the split arms tomorrow joins the
    # Magma census the same day and nobody has to remember a second edit. `-` stands for an empty
    # prefix tail, because a CMake list cannot tell "one empty element" from "no elements".
    macro(mgl_itest_register_split_arms scenarioFilter prefixTail)
        if("${prefixTail}" STREQUAL "")
            set(mglItestRecordedTail "-")
        else()
            set(mglItestRecordedTail "${prefixTail}")
        endif()
        list(APPEND MGL_SPLIT_ARM_CALLS "${scenarioFilter}|${mglItestRecordedTail}")
        mgl_itest_register_split_arms_for_backend(DirectGLES "${scenarioFilter}" "${prefixTail}"
                                                  ${ARGN})
    endmacro()

    mgl_itest_register_split_arms("ClearThenReadPixelsScenario.*" "")
    mgl_itest_register_split_arms("InPassClearScenario.*" "")
    mgl_itest_register_split_arms("ConvertedVertexStreamScenario.*" "")
    mgl_itest_register_split_arms("WriteRenameScenario.*" "")
    mgl_itest_register_split_arms("VaoShadowScenario.*" "")
    # GL_ARB_shader_objects (P13): ARB-created shader / program objects travel the record arm like
    # core-created ones, so the scenario runs on every split arm as well as both monoliths.
    mgl_itest_register_split_arms("ArbShaderObjectsScenario.*" "")
    # P13 W6: the cube-face readback rules the deleted frontend-arm unit cases pinned.
    mgl_itest_register_split_arms("CubeFaceReadbackScenario.*" "")
    mgl_itest_register_split_arms("TextureUnitSamplingScenario.*" "")
    mgl_itest_register_split_arms("TriangleScenario.*" "")
    mgl_itest_register_split_arms("PersistentCoherentMapScenario.*" "")
    # P11 A2 (docs/archive/contracts/CONTRACT-P11.md §2): the >= 16 MiB arena on all three arms. Its tier case
    # is declared emulated per entry by SplitLogPaths (T2: every acquisition declines); the mpr and
    # adopt-tier assertions are registered only in their configured controls. The Magma arms are the
    # gated `Arena.` block beside `Pbo.` / `Fence.` below, plus the Buffers lane.
    mgl_itest_register_split_arms("LargeArenaAdoptionScenario.*" "")
    # P11 B2 addendum: A BUFFER DELETED WHILE BOUND TO AN INDEXED POINT (storage, uniform, atomic
    # counter, transform feedback), then its slot reused at a newer generation and a draw and a
    # dispatch that declare the point. The monolith half is the ambient DirectGLES./DirectVulkan.
    # entries; Magma's three arms are the `DeletedBound.` block beside `Arena.` below.
    mgl_itest_register_split_arms("DeletedBoundBufferScenario.*" "")
    # ...and the repro it came from, kept as a regression: the two arena cases in ONE process (the
    # harness has one context per process, so the first case's storage binding outlives it). ctest
    # runs one case per process everywhere else, which is how this stayed hidden; add_test because
    # gtest_discover_tests registers cases one at a time. A PLAIN list for add_test: the joined
    # environments are escaped for gtest_discover_tests' PROPERTIES (see SpawnLane.EventForfeitPeer).
    foreach(mglItestPairArm IN ITEMS Split Spawn Tcp)
        string(TOLOWER "${mglItestPairArm}" mglItestPairArmLower)
        string(TOUPPER "${mglItestPairArm}" mglItestPairArmUpper)
        set(mglItestPairProperties)
        if(mglItestPairArm STREQUAL "Tcp")
            set(mglItestPairProperties FIXTURES_REQUIRED mobilegl-tcp RESOURCE_LOCK mobilegl-tcp)
        endif()
        string(REPLACE "\\;" ";" mglItestPairEnv "${MGL_ITEST_GLES_${mglItestPairArmUpper}_ENVIRONMENT}")
        list(APPEND mglItestPairEnv "MGITEST_PERSISTENT_MAP_ARM=emulated"
             "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/split-logs/DirectGLES.${mglItestPairArm}.ArenaPairInOneProcess.log")
        set(mglItestPairName "DirectGLES.${mglItestPairArm}.LargeArenaAdoptionScenario.GpuWriteThenTheTierCaseInOneProcess")
        add_test(NAME "${mglItestPairName}"
            COMMAND MobileGLIntegrationTest
                "--gtest_filter=LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack:LargeArenaAdoptionScenario.TheArenaLandsInTheTierItsLaneDeclares"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties("${mglItestPairName}" PROPERTIES
            LABELS "integration-gpu;integration-${mglItestPairArmLower}"
            TIMEOUT ${MGL_ITEST_TIMEOUT}
            ENVIRONMENT "${mglItestPairEnv}"
            ${mglItestPairProperties})
    endforeach()
    # P11 A1 (CONTRACT-P11 §1): the tcp arm with MOBILEGL_IPC_ADOPT_TIER=0. A stream refuses the
    # tier by name at the client's handshake and runs T2, so the whole scenario is green here and
    # AStreamSessionRefusesTheAdoptTierOnceByName reads exactly one refusal line. TCP ONLY by
    # construction: over shared segments (split, spawn) the same knob dies at the handshake, which
    # is the unit gate RemoteClientControls.AdoptTier*OverSharedSegments*. The `.AdoptTier0.` tail is
    # what keeps these names out of spawn_lane_parity.py's case set (see MAGMA_TCP_ONLY there).
    mgl_itest_lane("DirectGLES.Tcp.AdoptTier0." "LargeArenaAdoptionScenario.*-LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip:LargeArenaAdoptionScenario.ASessionThatCannotRunT0FallsBackToT2OnceByName"
        LABELS "integration-gpu\;integration-tcp" LOCK mobilegl-tcp FIXTURES mobilegl-tcp TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" ENV "${MGL_ITEST_GLES_TCP_ENVIRONMENT}\;MOBILEGL_IPC_ADOPT_TIER=0\;MGITEST_ADOPT_TIER_REFUSAL=T0")
    list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
    math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
    # P11 B2 (CONTRACT-P11 B2): THE SHARED-SEGMENT ARMS WITH MOBILEGL_IPC_ADOPT_TIER=0, which A1 made
    # die at the handshake and B2 turned into an ASK. A host has no AHardwareBuffer (and Espryt cannot
    # hand an in-process client its stores), so the server refuses T0 by name at its first native bind, publishes no kCapAdoptT0, and the client falls
    # back to T2 with one named line at its first map_persistent: every case green, the arena
    # emulated (SplitLogPaths declares it), ASessionThatCannotRunT0FallsBackToT2OnceByName reading
    # the two lines. `.AdoptT0Disallowed.` is the same on the spawn arm with the server's allow switch
    # off - which also proves MOBILEGL_IPC_ALLOW_ADOPT_T0 survives ServerSpawn's environment scrub.
    foreach(mglT0Arm IN ITEMS Split Spawn)
        string(TOUPPER "${mglT0Arm}" mglT0ArmUpper)
        string(TOLOWER "${mglT0Arm}" mglT0ArmLower)
        # Split is one process: Espryt cannot hand its client its stores, and an in-process session
        # never takes the AHardwareBuffer arm, so the refusal names that rather than the platform.
        if(mglT0Arm STREQUAL "Split")
            set(mglT0Fallback "in process")
        else()
            set(mglT0Fallback "no AHardwareBuffer")
        endif()
        mgl_itest_lane("DirectGLES.${mglT0Arm}.AdoptTier0." "LargeArenaAdoptionScenario.*-LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip:LargeArenaAdoptionScenario.AStreamSessionRefusesTheAdoptTierOnceByName"
            LABELS "integration-gpu\;integration-${mglT0ArmLower}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" ENV "${MGL_ITEST_GLES_${mglT0ArmUpper}_ENVIRONMENT}\;MOBILEGL_IPC_ADOPT_TIER=0\;MGITEST_ADOPT_T0_FALLBACK=${mglT0Fallback}")
        list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
        math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
    endforeach()
    mgl_itest_lane("DirectGLES.Spawn.AdoptT0Disallowed." "LargeArenaAdoptionScenario.ASessionThatCannotRunT0FallsBackToT2OnceByName:LargeArenaAdoptionScenario.TheArenaLandsInTheTierItsLaneDeclares"
        LABELS "integration-gpu\;integration-spawn" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" ENV "${MGL_ITEST_GLES_SPAWN_ENVIRONMENT}\;MOBILEGL_IPC_ADOPT_TIER=0\;MOBILEGL_IPC_ALLOW_ADOPT_T0=0\;MGITEST_ADOPT_T0_FALLBACK=disallowed")
    list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
    math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")

    # ---- Transform feedback on the split arms (ARCHITECTURE.md §8.5) ----------------------
    # The scatter reads the destination's pre-capture bytes from the server's staged shadow, so
    # the OrphanedCaptureTarget cases (glBufferData(size, NULL), then a partial or no upload) cover
    # a target the shadow cannot fully answer for. The writeback crosses SEG_EVENT in slices, which
    # the megabyte-range cases exercise, and XfbRepeatedCaptureScenario's gl_NextBuffer case checks
    # the stride and varyings the server takes from the program archive.
    mgl_itest_register_split_arms("XfbAfterClipDistanceScenario.*" "Xfb.")
    mgl_itest_register_split_arms("XfbRepeatedCaptureScenario.*" "Xfb.")

    # ---- P3b/P4b espryt D1 slice 4: THE READBACK FAMILY ON THE SPLIT ARMS -------------------
    #
    # The 回读 half of the plan's "回读 / pack state" row. The PACK half is done - server reads
    # with neutral pack state, the client scatters (ID-49, EmitTables.cpp) - and the READBACK half
    # had no split coverage at all, so every claim about it was a reading of the code rather than
    # a run of it. Arming the lanes is the first half of the work and the census is the second:
    # what reds here IS the remaining worklist, and each red is either D1's to fix or excluded BY
    # NAME with the Fatal quoted, the way the d1 draw block excluded
    # MultiDrawScenario.ClientSideIndicesBatchMatchesUnrolledDraws until P8-A.
    mgl_itest_register_split_arms("PixelStoreSweepScenario.*" "")
    mgl_itest_register_split_arms("PackedWordReadbackScenario.*" "")
    mgl_itest_register_split_arms("LayeredTextureReadbackScenario.*" "")
    # FramebufferChurnScenario shares PixelStoreSweepScenario.cpp and is the other half of its
    # story - ~3300 texture+framebuffer pairs per case, the shape that walked an Adreno driver to
    # 1.2 GB of leaked objects and then answered every readback with someone else's pixels. Under
    # a transport every one of those pairs is also a create/destroy record round trip, so this is
    # the only entry in the readback block that says anything about the OBJECT lifetime half.
    mgl_itest_register_split_arms("FramebufferChurnScenario.*" "")
    # P7 gate 5 (g5-readback): a glReadPixels larger than one SEG_REPLY slot is banded by the
    # client instead of Fatal{ReplyTooLarge}. Exactly 2 MiB (the CTS renderbuffers_storage shape),
    # 2 MiB + one row, more than the whole 16 MiB segment, a non-neutral pack state, a pack
    # buffer, depth and packed depth/stencil, and glGetTexImage's windowed twin. The Magma arms of
    # the same cases are the `Band.` block after the Fm lane below.
    mgl_itest_register_split_arms("BandedReadbackScenario.*" "")
    # P9 (W1, docs/archive/contracts/CONTRACT-P9.md §1): the pack-buffer readback crosses as
    # read_pixels_to_buffer / get_texture_image_to_buffer and is answered by nothing. Six parity
    # cases (every pack parameter, the map reader, a byte swap, a layered glGetTexImage, an upload
    # FROM the buffer the read filled, a later write over part of it) and the wire case (no
    # reply-slot record, one mark per read). The Magma arms are the `Pbo.` block after the Band
    # block below.
    mgl_itest_register_split_arms("PackBufferReadbackScenario.*" "")
    # P10 (A, docs/archive/contracts/CONTRACT-P10.md §1): fence polls answered from kEventFenceSignaled - the
    # idiom's poll loops end (with and without the flush bit), a signaled fence's polls cross
    # nothing, and an idle server still reports a fence that finishes. The Magma arms are the
    # `Fence.` block beside the `Pbo.` block below.
    mgl_itest_register_split_arms("FencePollScenario.*" "")

    # DepthStencilReadbackMatrixScenario GETS ITS OWN BLOCK, because it needs a variable the
    # registration macro cannot carry: MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1.
    #
    # Without it this scenario is UNFALSIFIABLE on the machines this suite runs on, and the
    # ambient DirectGLES.ForcedDepthStencilEmulation. lane says why: OpenGL ES has no depth or
    # stencil readback in core, but llvmpipe accepts the reads anyway, so every case goes
    # green through a native path the Adreno device does not have - deleting the whole emulation
    # left all of them passing. A split arm that inherited the ambient default would be measuring
    # the same non-path three more times. So the pin is MIRRORED here, which is what the plan's
    # row asks for.
    #
    # P8-A: SPLIT AND SPAWN ONLY HERE. The knob is read by the server, and the Tcp arm's server
    # is the lane-wide fixture, so a Tcp copy registered here ran the NATIVE readback under the
    # ForcedDs. name. The same-named Tcp entries are registered in P8Coverage.cmake, against a
    # supervisor started with the knob.
    foreach(mglItestDsArm Split Spawn)
        mgl_itest_magma_arm_shape(${mglItestDsArm})
        string(TOLOWER "${mglItestDsArm}" mglItestDsArmLower)
        mgl_itest_join_environment(mglItestDsEnvironment
            "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_SPLIT_LANE=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
            "MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1"
            ${mglItestArmTransportEnv} ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
        mgl_itest_lane("DirectGLES.${mglItestDsArm}.ForcedDs." "DepthStencilReadbackMatrixScenario.*"
            LABELS "integration-gpu\;integration-${mglItestDsArmLower}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestDsEnvironment}")
        # Into the accumulated list like every other arm entry, so SplitLogPaths gives each of
        # these a private MOBILEGL_LOG_FILE_PATH too - two processes append on the spawn arm.
        list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
        math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
    endforeach()

    # ---- AsyncCompileScenario on the split arms, in its three pinned configurations ---------
    # Each case needs a whole-process compiler configuration (MOBILEGL_ASYNC_SHADER_COMPILE,
    # MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS), so the monolith AsyncOn. / AsyncOff. /
    # OptimisticShaderStatus. entries are mirrored per arm; AsyncOn. leaves out the Iris case, which
    # has its own entry. Only pinned configurations are registered, so a change of the shipped
    # default cannot drop either half. On these arms the server links and draws the worker-built
    # program archives. The switch is client state (ReconcileClientCompileExtensions in
    # CapsMirror.cpp), so the tcp fixture server not seeing it is part of what the Tcp arm checks.
    foreach(mglItestAsyncArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestAsyncArm})
        string(TOUPPER "${mglItestAsyncArm}" mglItestAsyncArmUpper)
        foreach(mglItestAsyncShape AsyncOn AsyncOff OptimisticShaderStatus)
            if(mglItestAsyncShape STREQUAL "AsyncOn")
                set(mglItestAsyncFilter
                    "AsyncCompileScenario.*-AsyncCompileScenario.IrisShapedTwoPhaseBatchRendersCorrectly")
                set(mglItestAsyncKnobs "MOBILEGL_ASYNC_SHADER_COMPILE=1")
            elseif(mglItestAsyncShape STREQUAL "AsyncOff")
                set(mglItestAsyncFilter "AsyncCompileScenario.ExtensionStringMatchesTheConfiguration")
                set(mglItestAsyncKnobs "MOBILEGL_ASYNC_SHADER_COMPILE=0")
            else()
                set(mglItestAsyncFilter "AsyncCompileScenario.IrisShapedTwoPhaseBatchRendersCorrectly")
                set(mglItestAsyncKnobs
                    "MOBILEGL_ASYNC_SHADER_COMPILE=1\;MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS=1")
            endif()
            mgl_itest_lane("DirectGLES.${mglItestAsyncArm}.${mglItestAsyncShape}." "${mglItestAsyncFilter}"
                LABELS "integration-gpu\;integration-${mglItestArmLower}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_GLES_${mglItestAsyncArmUpper}_ENVIRONMENT}\;${mglItestAsyncKnobs}")
            list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
            math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
        endforeach()
    endforeach()


    # ---- The monolith arm inside the gated set --------------------------------------------
    # integration-split names the set the disaggregation gate runs, not "runs inproc". A
    # disaggregated build serves either arm, chosen at runtime by MG_Config::Transport; the Split
    # blocks above all pin inproc and split-only scenarios skip under monolith, so this entry is
    # the monolith arm's case in that set. MOBILEGL_TRANSPORT=monolith is pinned so a job-level
    # transport export cannot turn it into another split entry (ARCHITECTURE.md §16.1). The
    # ambient MonolithAttachmentClearScenario entries stay in integration-gpu.
    mgl_itest_join_environment(MGL_ITEST_GLES_PUSH_MONOLITH_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=monolith"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
    mgl_itest_lane("DirectGLES.PushMonolithArm." "MonolithAttachmentClearScenario.*"
        LABELS "integration-gpu\;integration-split" TEST_LIST MGL_PUSH_MONOLITH_ATTACHMENT_TESTS ENV "${MGL_ITEST_GLES_PUSH_MONOLITH_ENVIRONMENT}")

    # ---- Indexed, instanced, multi-draw and indirect draws on the split arms ----------------
    # Every IndexedDrawFamilyScenario case's picture depends on one of these draws, so a record
    # that did not cross shows as a wrong picture. MultiDrawScenario runs whole: the client
    # flattens every sub-draw's client indices into one owned element buffer before emitting
    # (EmitMultiIndexedDraw in MG_Impl/Pipe/Verb/VerbPort.cpp, OwnedDrawInputs.h), and
    # ClientSideIndicesBatchMatchesUnrolledDraws fails if a multi-draw with client indices reaches
    # the server again (the +CLIENT_INDICES latch in RecordVerbSink.cpp).
    set(MGL_SPLIT_D1_SCENARIOS IndexedDrawFamilyScenario MultiDrawScenario DrawParametersScenario
                               PrimitiveRestartScenario)
    set(MGL_SPLIT_D1_FILTER_IndexedDrawFamilyScenario "IndexedDrawFamilyScenario.*")
    set(MGL_SPLIT_D1_FILTER_MultiDrawScenario "MultiDrawScenario.*")
    set(MGL_SPLIT_D1_FILTER_DrawParametersScenario "DrawParametersScenario.*")
    set(MGL_SPLIT_D1_FILTER_PrimitiveRestartScenario "PrimitiveRestartScenario.*")
    foreach(mglItestD1Scenario IN LISTS MGL_SPLIT_D1_SCENARIOS)
        mgl_itest_register_split_arms("${MGL_SPLIT_D1_FILTER_${mglItestD1Scenario}}" "")
    endforeach()

    # ---- Multi-draw tiers pinned on the split arm -----------------------------------------
    # MultiDrawImpl asks the bound index buffer three questions, and five of the six Espryt tiers
    # ask them from a different point in the batch. MOBILEGL_ESPRYT_MULTIDRAW_MODE is read once per
    # process (ResolveTierOnce in MultiDraw.cpp) and `auto` resolves to `ext` on the CI drivers,
    # which the lanes above cover, so each other tier gets an entry with the mode pinned as a ctest
    # ENVIRONMENT property (ARCHITECTURE.md §16.1). Drop a tier the driver refuses: it would run
    # `ext` again. Each entry gets a private log under split-logs/ (SplitLogPaths.cmake.in), so the
    # tier marker is read from the run that pinned it.
    set(MGL_SPLIT_MULTIDRAW_TIERS basevertex multiindirect indirect drawelements compute)
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_basevertex "BaseVertex")
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_multiindirect "MultiIndirect")
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_indirect "Indirect")
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_drawelements "DrawElements")
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_compute "Compute")
    set(MGL_SPLIT_MULTIDRAW_TIER_FILTER
        "MultiDrawScenario.*:IndexedDrawFamilyScenario.MultiDraw*")
    foreach(mglItestMultiDrawTier IN LISTS MGL_SPLIT_MULTIDRAW_TIERS)
        mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_MULTIDRAW_ENVIRONMENT
            "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc" "MOBILEGL_ITEST_REQUIRE_GPU=1"
            "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
            "MOBILEGL_ESPRYT_MULTIDRAW_MODE=${mglItestMultiDrawTier}"
            ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
        mgl_itest_lane("DirectGLES.Split.MultiDrawTier${MGL_SPLIT_MULTIDRAW_TIER_PREFIX_${mglItestMultiDrawTier}}." "${MGL_SPLIT_MULTIDRAW_TIER_FILTER}"
            LABELS "integration-gpu\;integration-split" TEST_LIST "MGL_SPLIT_MULTIDRAW_${mglItestMultiDrawTier}_TESTS" ENV "${MGL_ITEST_GLES_SPLIT_MULTIDRAW_ENVIRONMENT}")
    endforeach()

    # ---- Image units, compute, copy-image, atomic counters and storage-block bindings -------
    # Each scenario asserts a value that exists only if its verb ran on the apply thread (a texel
    # stored through an image unit, a word glCopyImageSubData moved, an atomic counter, a rebound
    # storage block), so a declined record or a client that fell through to the driver reads back
    # the wrong number. Filters keep only cases that emit records: an armed DirectGLES.Split. case
    # must move the client encoder's record ordinal (ScenarioFixture.h), and the other
    # ProgramPipelineScenario cases draw nothing.
    set(MGL_SPLIT_I1_SCENARIOS
        UnboundImageDescriptorScenario   # bind_shader_image (72) + launch_grid (60), 13 cases
        CopyImageLayeredScenario         # resource_copy_region (53), 6 cases
        AtomicCounterScenario            # launch_grid (60) + memory_barrier (61), 3 cases
        ProgramPipelineScenario)         # set_storage_block_binding (75), the 2 storage-block cases
    set(MGL_SPLIT_I1_FILTER_UnboundImageDescriptorScenario "UnboundImageDescriptorScenario.*")
    set(MGL_SPLIT_I1_FILTER_CopyImageLayeredScenario       "CopyImageLayeredScenario.*")
    set(MGL_SPLIT_I1_FILTER_AtomicCounterScenario          "AtomicCounterScenario.*")
    set(MGL_SPLIT_I1_FILTER_ProgramPipelineScenario        "ProgramPipelineScenario.*StorageBlock*")
    foreach(mglItestI1Scenario IN LISTS MGL_SPLIT_I1_SCENARIOS)
        mgl_itest_register_split_arms("${MGL_SPLIT_I1_FILTER_${mglItestI1Scenario}}" "")
    endforeach()

    # ---- P7 gate 5 (g5-imgwin): AN IMAGE UNIT NAMING A (LEVEL, LAYER) ITS TEXTURE LACKS ------
    #
    # GL 4.6 core 8.26 makes every access through such a unit invalid - zero loads, dropped
    # stores - and never an error. Espryt forwards the unit to the driver, which applies the rule
    # itself; these are its three arms. The Magma arms of the same scenario are laid by hand in
    # the Fm block (`DirectVulkan.<arm>.ImageUnitWindow.`), because that is where the wire arm's
    # gated flavour lives.
    #
    # ALL SIX CASES, THE TEXTURE-VIEW ONE INCLUDED. The view case waited outside the gated arms
    # while every glTextureView under a transport ended the session at Fatal{ProtocolCorruption,
    # "StagedTextureStore.NoteLevelDefined"} (p7/f2's view descriptor carried internalFormat
    # 0xFFFFFFFF); the F2 repair (f2-latch slice A) retired that death, so it joins them here and
    # in the Magma ImageUnitWindow foreach. The shared GLES split lists carry
    # MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1, so it does not green-skip on these arms.
    mgl_itest_register_split_arms("InvalidImageUnitScenario.*" "")

    # ---- P9 W2: A TEXTURE RE-MINT NEEDS NO BYTE FROM THE CLIENT ------------------------------
    #
    # The ROADMAP's `TextureRemintPullScenario`, and it proves the opposite of its name: the
    # server re-mints from the GPU level and its own staged store, so no pull protocol exists
    # (docs/archive/notes/p9/W2-REMINT.md). Espryt's three arms here; the Magma arms are laid by hand in the
    # `DirectVulkan.<arm>.Remint.` block, where the gated Magma flavour lives; both backends' monolith
    # entries come from the whole-binary discovery.
    mgl_itest_register_split_arms("TextureRemintPullScenario.*" "")

    # W2-b's control. MGITEST_ESPRYT_FORCE_REMINT_READBACK_FAILURE is read by the SERVER (the
    # promotion in RequireImageBindableStorageByHandle), so it reaches inproc and spawn - the spawned
    # server copies this process's environment - and never the tcp lane's shared fixture: split and
    # spawn only, the MAGMA_SERVER_ENV_KNOB_NO_TCP reason. The entries carry their own label rather
    # than integration-{split,spawn} so the Espryt spawn-parity sets stay knob-free; integration-gpu
    # is what runs them in the gate. Only the uploaded-only cases: their levels are covered by the
    # store, so the fallback must still produce the right texels and the server log must name it.
    foreach(mglItestArm IN ITEMS Split Spawn)
        string(TOUPPER "${mglItestArm}" mglItestArmUpper)
        string(TOLOWER "${mglItestArm}" mglItestArmLower)
        foreach(remintFallbackCase AnUploadedMutableTextureKeepsItsTexelsAcrossTheImageBind
                                   AWidenedFormatsUploadSurvivesTheCarrierRemint
                                   AnRgba8SnormUploadSurvivesTheImageBind
                                   AnRgba16SnormImmutableUploadSurvivesTheCarrierRemint)
            mgl_itest_lane("DirectGLES.${mglItestArm}.RemintFallback." "TextureRemintPullScenario.${remintFallbackCase}"
                LABELS "integration-gpu\\;integration-remint-fallback-${mglItestArmLower}" ENV "${MGL_ITEST_GLES_${mglItestArmUpper}_ENVIRONMENT}\;MGITEST_ESPRYT_FORCE_REMINT_READBACK_FAILURE=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p9-remint-fallback-${mglItestArmLower}-${remintFallbackCase}.log")
        endforeach()
    endforeach()

    # ---- P11 M: DEV'S TWO GPU-CONTENT SCENARIOS ON THE SPLIT ARMS ----------------------------
    #
    # Merged from dev@c60639db with their monolith entries. ImageBindableRemintScenario is the
    # monolith twin of TextureRemintPullScenario above (568090f0 made Espryt's frontend re-mint
    # read the GPU level back as the server's already did); PartialUploadOverGpuContentScenario is
    # 8966d5c2's sub-rect upload over converted/reshaped levels (the server's handle arm cuts the
    # region from the conversion's buffer too) plus b1d60523's Magma ordering cases, which are
    # plain GL and run here on Espryt as well. The Magma arms are laid by hand in the
    # `DirectVulkan.<arm>.GpuContent.` block, where the gated Magma flavour lives.
    mgl_itest_register_split_arms("ImageBindableRemintScenario.*" "")
    mgl_itest_register_split_arms("PartialUploadOverGpuContentScenario.*" "")

    # ---- P11 M2: DEV'S TWO MAGMA SCENARIOS ON THE SPLIT ARMS ---------------------------------
    #
    # Merged from dev@c14283ee with their monolith entries. Both are plain GL and check pixels on
    # every backend: OversizedRecordingSplitScenario draws frames whose state spans a command-buffer
    # split (d20c2d01), DescriptorPoolGrowthScenario draws a 4096-distinct-set frame and gives it
    # back (c2de6576/53fa16c2). On Espryt neither has anything else to check - the draw budget and
    # the pool census are Magma's - so every case runs on all three arms. The Magma arms are laid by
    # hand below (`DirectVulkan.<arm>.Oversized.`, `.SplitRecording64.`, `DirectVulkan.Split.DescPool.`),
    # where the gated flavour and the per-arm exceptions live.
    mgl_itest_register_split_arms("OversizedRecordingSplitScenario.*" "")
    # P15 present wait: plain GL, pixel checks on every backend and arm (the frame shapes that
    # split around the window pass); the arming assertion stays in DirectVulkan.WindowPassGate.
    mgl_itest_register_split_arms("WindowPassGateScenario.*" "")
    mgl_itest_register_split_arms("DescriptorPoolGrowthScenario.*" "")

    # ---- Transform-feedback spans, the XFB object bind and the patch parameter --------------
    # Both scenarios pass only if those records crossed and the backend applied them.
    # TessellationXfbCaptureScenario sets GL_PATCH_VERTICES, draws GL_PATCHES into a capture and
    # checks the captured bytes: a lost patch parameter changes the capture length, a lost span
    # leaves the scenario's poison value. XfbCaptureBufferReuseScenario runs spans over four buffer
    # lifetimes, so a span whose end crossed without its begin shows the previous span's bytes.
    # On Magma's split arms they run only in the informational integration-magma-all-* and
    # integration-magma-full-* tiers.
    mgl_itest_register_split_arms("TessellationXfbCaptureScenario.*" "")
    mgl_itest_register_split_arms("XfbCaptureBufferReuseScenario.*" "")

    # ---- GL_PRIMITIVES_GENERATED and GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN on one span ----
    # XfbPrimitiveQueryScenario captures with and without overflow (4 generated, 3 written) and
    # reads the records back. Under a transport both answers are server query replies, so a swapped
    # count or a result read before the span's end was applied shows as a wrong number.
    # PrimitivesGeneratedNoXfbScenario is not registered here: every case skips on DirectGLES.
    # The Magma arms of both are the DirectVulkan.<arm>.XfbQuery. / PrimGen. / PrimGenReroute.
    # entries further down.
    mgl_itest_register_split_arms("XfbPrimitiveQueryScenario.*" "")

    # ---- P5e package vi (docs/archive/contracts/CONTRACT-P5E.md §5.1, ID-82 / ruling 2) --------------------
    #
    # Client-memory vertices and indices now cross as owned ordinary buffers.
    # Keep the original names and private logs; all six scenarios join the full
    # GPU and split acceptance labels as well as their focused regression label.
    mgl_itest_register_split_arms("ClientVertexArrayScenario.*" "" integration-clientarrays)

    mgl_itest_register_split_arms("SyncWireScenario.*" "")

    # ---- TextureViewAliasScenario: uploads through a texture view, on all three transports --
    # The client keys every texture upload on the view's storage owner, and the applier keeps a
    # pending-upload set per record, so a flag cleared on the wrong record leaves a server-side
    # level without its texels. The server's clean gate reads the owner's record for a view
    # (ARCHITECTURE.md §6.1.2), which TheOwnerUploadDrainRunsAgainAfterADrawBoundary checks on
    # every arm. Every case draws, so each moves the record ordinal; the arm environments carry
    # the texture-view opt-in.
    mgl_itest_register_split_arms("TextureViewAliasScenario.*" "")

    # ---- TextureUploadShapeScenario on the three transports -------------------------------
    # Written out per arm rather than through mgl_itest_register_split_arms: the case needs
    # MGITEST_TEXTURE_UPLOAD_SHAPE_LANE=1, the stats switches and a log path private to the arm,
    # which the shared arm environments do not carry (ARCHITECTURE.md §16.1). Under spawn and tcp
    # the client's emission count and the server's tex[] come from two processes with their own
    # counters, so their agreement is a real comparison there; under inproc one process writes two
    # role logs, which checks that each role's line is read from its own file.
    set(MGL_D2_TUS_FILTER "TextureUploadShapeScenario.TheEmittedUploadShapeIsRecordedAndTheTwoSidesAgree")
    foreach(mglD2Arm Split Spawn Tcp)
        string(TOLOWER "${mglD2Arm}" mglD2ArmLower)
        set(mglD2Log "${CMAKE_CURRENT_BINARY_DIR}/texture-upload-shape-${mglD2ArmLower}-DirectGLES.log")
        get_filename_component(mglD2LogName "${mglD2Log}" NAME)
        set(mglD2Locks "${mglD2LogName}")
        set(mglD2Extra)
        if(mglD2Arm STREQUAL "Split")
            set(mglD2Transport "MOBILEGL_TRANSPORT=inproc"
                               "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}")
        elseif(mglD2Arm STREQUAL "Spawn")
            set(mglD2Transport "MOBILEGL_TRANSPORT=spawn"
                               "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}")
        else()
            set(mglD2Transport "MOBILEGL_TRANSPORT=spawn"
                               "MOBILEGL_IPC_CONTROL=${MOBILEGL_ITEST_TCP_ENDPOINT}"
                               "MOBILEGL_IPC_DATA=stream" "MOBILEGL_IPC_REQUIRE_SAME_BUILD=1"
                               "MOBILEGL_IPC_LOG_FORWARD=1" "MOBILEGL_IPC_TOKEN="
                               "MGITEST_TCP_LANE=1" "MOBILEGL_ITEST_REQUIRE_GPU=1")
            set(mglD2Extra FIXTURES_REQUIRED mobilegl-tcp)
            # The loopback supervisor serves ONE session (ServerMain.cpp answers a second Hello
            # Refuse{Busy}), and every other Tcp entry holds this lock. Without it `ctest -L
            # integration-gpu -j N` ran this case beside another Tcp case and whichever client
            # connected second died Refuse{Busy} -> Fatal{CapsBeforeFirstSnapshot}.
            # scripts/ci/spawn_lane_parity.py and junit_tally.py --discovery now refuse any entry
            # on the fixture that does not hold it.
            set(mglD2Locks "${mglD2LogName}\;mobilegl-tcp")
        endif()
        mgl_itest_join_environment(MGL_D2_TUS_ENV_${mglD2Arm}
            "MOBILEGL_BACKEND_TYPE=DirectGLES" ${mglD2Transport} "MGITEST_SPLIT_LANE=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
            "MGITEST_TEXTURE_UPLOAD_SHAPE_LANE=1" "MOBILEGL_PIPE_PUSH=0x3fff"
            "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
            "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
            "MOBILEGL_LOG_FILE_PATH=${mglD2Log}"
            ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
        mgl_itest_lane("DirectGLES.${mglD2Arm}.TextureUploadShape." "${MGL_D2_TUS_FILTER}"
            LABELS "integration-gpu\;integration-${mglD2ArmLower}" LOCK "${mglD2Locks}" PROPERTIES ${mglD2Extra} ENV "${MGL_D2_TUS_ENV_${mglD2Arm}}")
    endforeach()

    # Cold-start bring-up on the spawn arm. A spawned server brings its native backend up lazily,
    # inside the first surface creation (Espryt) or the first MakeCurrent (Magma).
    # MOBILEGL_TEST_DELAY_FIRST_BRINGUP_MS holds both ops 6 s, past the 5 s steady reply bound;
    # the client must wait them out (those ops run under MOBILEGL_IPC_COLD_START_MS, and each
    # Wire::SurfaceProgress report restarts the budget). Unit check: ServerSpawnTest's
    # cold-bring-up case. Both entries are expected green, so the Magma one carries integration-gpu
    # too; ~25 s each, as the harness's pre-flight child brings a server up as well. Not on
    # integration-spawn, whose name set must equal the split lane's (spawn_lane_parity.py).
    set(MGL_COLDSTART_FILTER "ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels")
    foreach(mglColdBackend DirectGLES DirectVulkan)
        if(mglColdBackend STREQUAL "DirectVulkan")
            set(mglColdDriverEnv ${MGL_ITEST_VULKAN_ENV})
        else()
            set(mglColdDriverEnv ${MGL_ITEST_COMMON_ENV})
        endif()
        mgl_itest_join_environment(MGL_COLDSTART_ENV_${mglColdBackend}
            "MOBILEGL_BACKEND_TYPE=${mglColdBackend}" ${MGL_ITEST_SPAWN_ARM_TRANSPORT_ENV}
            "MGITEST_SPLIT_LANE=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
            "MOBILEGL_TEST_DELAY_FIRST_BRINGUP_MS=6000"
            "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cold-start-spawn-${mglColdBackend}.log"
            ${MGL_ITEST_CAPABILITY_ENV} ${mglColdDriverEnv})
        mgl_itest_lane("${mglColdBackend}.Spawn.ColdStart." "${MGL_COLDSTART_FILTER}"
            LABELS "integration-gpu\;integration-cold-start" ENV "${MGL_COLDSTART_ENV_${mglColdBackend}}")
    endforeach()

    # Texture and program scenarios on the DirectGLES split arms. Every scenario listed passes on
    # those arms whole; cases that do not are left out and named in the comments below. The
    # readback scenarios (PixelStoreSweep, DepthStencilReadbackMatrix, PackedWordReadback,
    # LayeredTextureReadback) are registered with the readback blocks above. Their DirectVulkan
    # split arms are only the ungated Magma census lanes (integration-magma-all-* replays this
    # call, integration-magma-full-* runs the whole binary).
    set(MGL_SPLIT_D2_SCENARIOS
        TextureViewScenario                       # glTextureView end to end, 10 cases
        TextureParamsWithoutASamplerViewScenario  # G9's four cases; also settles the debt-table row
        SampledSetStalenessScenario               # 2 cases
        ImageSizeAfterRespecScenario              # 1 case
        ImageTargetKindScenario                   # 29 cases
        IntegerBorderColorScenario                # 6 cases
        SwizzleAccessRoutineScenario              # 4 cases
        PostLinkAttachScenario                    # 2 cases
        RelinkStageSetScenario                    # 3 cases
        UniformInitializerScenario                # 3 cases
        Glsl420DeclarationScenario                # 6 cases
        IoBlockNameCollisionScenario)             # 2 cases
    # NO PER-SCENARIO FILTER INDIRECTION, unlike the d1 and i1 blocks above: every scenario here
    # is registered whole, because the census found no case in any of them that needs excluding.
    # The d1/i1 shape (a MGL_SPLIT_<pkg>_FILTER_<Scenario> variable consulted per entry) is what
    # to reach for the day one does; carrying an `if(DEFINED ...)` that is never true today would
    # be a branch no run takes.
    foreach(mglItestD2Scenario IN LISTS MGL_SPLIT_D2_SCENARIOS)
        mgl_itest_register_split_arms("${mglItestD2Scenario}.*" "")
    endforeach()

    # P7 gate 5 (g5-msrbo): AN EMPTY TEXTURE LEVEL. glTexImage*D with a zero extent is legal GL
    # and dEQP's resetStateGLCore issues it for every target after every case; PH-4's extent
    # carrier refused it as ProtocolCorruption on the apply thread, so no KHR-GL case could
    # finish on a split arm. Backend-agnostic (the refusal was in MGPipeApplyResourceRespecify),
    # so the DirectGLES gated arms carry it here and the Magma gated lanes below carry it too.
    mgl_itest_register_split_arms("EmptyTextureLevelScenario.*" "")

    # ProgramPipelineScenario BEYOND *StorageBlock*: the i1 block above already registers the two
    # storage-block cases. The other nine are pure name/state cases that draw nothing, and
    # ScenarioFixture's armed-lane rule (the client encoder's record ordinal must move) makes a
    # case that produced no record RED for having nothing to say - which is the rule working, not
    # a defect. They stay unregistered for that reason and are listed in the census note.

    # P8-E (docs/archive/notes/p8/E.md): glCopyImageSubData into a level the driver will not
    # read back, then glGetTexImage. The knob-free cases on Espryt's three arms; Magma's gated arms
    # are the `DirectVulkan.<arm>.CopyStore.` entries just below; both monolith entries come from
    # the whole-binary discovery.
    mgl_itest_register_split_arms("CopyImageStoreReadbackScenario.*" "")
    # The same cases with MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT pinned: the Espryt server
    # treats a colour level of exactly that extent as one the driver refused to attach (what an
    # Adreno does for RGB16 and 16-bit SNORM), so its staged store has to answer. READ BY THE
    # SERVER, and it reaches all three arms: inproc sees this entry's environment, spawn copies it
    # into the child, and the tcp supervisor carries it in its own fixture ENVIRONMENT (appended to
    # TcpServer.Start here). Lane-wide there is harmless because the knob names an EXTENT and the
    # scenario builds that extent only when it finds the knob in its own environment - every other
    # tcp entry, this scenario's knob-free ones included, reads levels of other sizes. The
    # renderbuffer case is not here: a renderbuffer's texels were never uploaded, so no store can
    # answer for them, and a declined readback ends the session by design.
    set(MGL_P8E_REFUSE_EXTENT "13x7")
    set_property(TEST TcpServer.Start APPEND PROPERTY ENVIRONMENT
        "MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=${MGL_P8E_REFUSE_EXTENT}")
    foreach(mglItestArm IN ITEMS Split Spawn Tcp)
        string(TOUPPER "${mglItestArm}" mglItestArmUpper)
        string(TOLOWER "${mglItestArm}" mglItestArmLower)
        set(mglItestStoreReadProperties)
        if(mglItestArm STREQUAL "Tcp")
            set(mglItestStoreReadProperties FIXTURES_REQUIRED mobilegl-tcp RESOURCE_LOCK mobilegl-tcp)
        endif()
        foreach(storeReadCase AnUploadedLevelReadsBackExactly
                              ACopiedWindowReadsBackOverTheUpload
                              ACopiedWindowReadsBackFromAnImmutableLevel
                              APartialUploadAfterTheCopyKeepsTheCopiedWindow)
            mgl_itest_lane("DirectGLES.${mglItestArm}.StoreRead." "CopyImageStoreReadbackScenario.${storeReadCase}"
                LABELS "integration-gpu\\;integration-${mglItestArmLower}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestStoreReadProperties} ENV "${MGL_ITEST_GLES_${mglItestArmUpper}_ENVIRONMENT}\;MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=${MGL_P8E_REFUSE_EXTENT}")
            # Into the accumulated list, so SplitLogPaths gives each entry a private log: the
            # store-route assertion reads the server's half of it.
            list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
            math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
        endforeach()
        # Magma, the control backend, GATED on its three arms: it reads the VkImage back whatever
        # the format, so it has no store route and no knob - only the copy and the read.
        set(mglItestMagmaCopyStoreProperties)
        if(mglItestArm STREQUAL "Tcp")
            set(mglItestMagmaCopyStoreProperties FIXTURES_REQUIRED mobilegl-tcp RESOURCE_LOCK mobilegl-tcp)
        endif()
        mgl_itest_lane("DirectVulkan.${mglItestArm}.CopyStore." "CopyImageStoreReadbackScenario.*"
            LABELS "integration-magma-${mglItestArmLower}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestMagmaCopyStoreProperties} ENV "${MGL_ITEST_VULKAN_${mglItestArmUpper}_ENVIRONMENT}")
        list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
        math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
    endforeach()

    # The split half of the counting pair. MGITEST_PERSISTENT_MAP_ARM=emulated is R-6: under split
    # the adopt tier is pinned at T2, the resource owner declines every acquisition and the client
    # pushes the mapping's dirty blocks - so pmap must be non-zero and mpr must be the monolith
    # lane's number. pmap == 0 here means MGPipeApplyMapPersistent handed back a pointer, which is
    # exit gate E3(d) failing, and under inproc that failure is invisible in pixels.
    mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_PMAP_ARM_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MGITEST_PMAP_LANE=1" "MGITEST_PERSISTENT_MAP_ARM=emulated"
        "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/persistent-map-arm-split-DirectGLES.log"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    mgl_itest_lane("DirectGLES.Split.PersistentMapArm." "PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares"
        LABELS "integration-gpu\;integration-split" LOCK persistent-map-arm-split-DirectGLES.log ENV "${MGL_ITEST_GLES_SPLIT_PMAP_ARM_ENVIRONMENT}")

    # Small-ring lane: the same split scenarios with SEG_CMD and SEG_STAGE at ConfigLoader's 1 MiB
    # floor (defaults 8 / 32 MiB), so the workload has to wrap the ring and wait for staging space.
    # A ring caps one record at half its size, and 512 KiB is far above any record on these paths.
    # Under MGITEST_SMALL_RING_LANE, TriangleScenario drives past the ring size and asserts a
    # SEG_CMD wrap and a staging back-pressure wait (WireLedger::ExpectSmallRingWrappedAtLeastOnce).
    mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_SMALL_RING_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MGITEST_SMALL_RING_LANE=1" "MOBILEGL_IPC_RING_MB=1" "MOBILEGL_IPC_STAGE_MB=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    set(MGL_SPLIT_SMALL_RING_SCENARIOS ClearThenReadPixelsScenario TriangleScenario
                                      PersistentCoherentMapScenario)
    foreach(mglItestSmallRingScenario IN LISTS MGL_SPLIT_SMALL_RING_SCENARIOS)
        mgl_itest_lane("DirectGLES.Split.SmallRing." "${mglItestSmallRingScenario}.*"
            LABELS "integration-gpu\;integration-split" TEST_LIST "MGL_SPLIT_SMALL_${mglItestSmallRingScenario}_TESTS" ENV "${MGL_ITEST_GLES_SPLIT_SMALL_RING_ENVIRONMENT}")
    endforeach()

    # Strict-errors entries for the per-draw and per-dispatch program reads. Under
    # MOBILEGL_IPC_STRICT_ERRORS=1 a barrier-pulled pipe-input read is Fatal{UnmigratedPipeInput},
    # so these go red if GetProgramForDraw@DrawArrays or GetProgramForDispatch@DispatchCompute is
    # pulled again. The MultiDrawScenario case prepares a draw and checks GL errors, not pixels;
    # AtomicCounterScenario dispatches. Labelled integration-split only: on the monolith arm the
    # knob cannot trip, because the client fills every field at every verb.
    mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_STRICT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_IPC_STRICT_ERRORS=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    mgl_itest_lane("DirectGLES.Split.StrictProgramArm." "MultiDrawScenario.BaseVertexDrawsRejectMalformedArguments"
        LABELS "integration-split" TEST_LIST MGL_SPLIT_PA_STRICT_DRAW_TESTS ENV "${MGL_ITEST_GLES_SPLIT_STRICT_ENVIRONMENT}")
    mgl_itest_lane("DirectGLES.Split.StrictProgramArm." "AtomicCounterScenario.*"
        LABELS "integration-split" TEST_LIST MGL_SPLIT_PA_STRICT_DISPATCH_TESTS ENV "${MGL_ITEST_GLES_SPLIT_STRICT_ENVIRONMENT}")

    # P8-B: glGenerateMipmap ON ESPRYT'S SERVER (docs/archive/notes/p8/B.md). The window a record names (B1) and
    # RGB16F / RGB32F (B2) on the three Espryt arms; the Magma control is the gated
    # `DirectVulkan.<arm>.Mip.` block beside `Remint.`, and both monoliths come from the ambient
    # discovery. The FORCED-CPU entries run the three-channel cases with
    # MGITEST_ESPRYT_FORCE_CPU_MIPMAP=1, which the SERVER reads: inproc and spawn only, because the
    # tcp arm's server is the one shared TcpServer.Start fixture and the knob there would take the
    # native arm off every tcp entry. Their own label keeps the spawn-parity sets knob-free, as the
    # `RemintFallback.` entries do; integration-gpu is what runs them in the gate.
    mgl_itest_register_split_arms("GenerateMipmapServerScenario.*" "")
    foreach(mglItestArm IN ITEMS Split Spawn)
        string(TOUPPER "${mglItestArm}" mglItestArmUpper)
        string(TOLOWER "${mglItestArm}" mglItestArmLower)
        foreach(cpuMipCase Rgb16fUploadedBaseGeneratesItsChain
                           Rgb16fRenderTargetBaseGeneratesFromTheGpuWrittenLevel
                           Rgb32fUploadedBaseGeneratesItsChain
                           Rgb32fRenderTargetBaseGeneratesFromTheGpuWrittenLevel)
            mgl_itest_lane("DirectGLES.${mglItestArm}.CpuMip." "GenerateMipmapServerScenario.${cpuMipCase}"
                LABELS "integration-gpu\\;integration-cpu-mip-${mglItestArmLower}" ENV "${MGL_ITEST_GLES_${mglItestArmUpper}_ENVIRONMENT}\;MGITEST_ESPRYT_FORCE_CPU_MIPMAP=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p8b-cpu-mip-${mglItestArmLower}-${cpuMipCase}.log")
        endforeach()
    endforeach()
    # P13 W4b: MONOLITH TAKES THE SAME KNOBS. On the record arm the monolith runs the server's
    # generation and the server's store read in its own process, so both knobs reach it and each
    # entry asserts its arm's line in its private log. Ambient monolith environment; integration-gpu.
    foreach(cpuMipCase Rgb16fUploadedBaseGeneratesItsChain
                       Rgb16fRenderTargetBaseGeneratesFromTheGpuWrittenLevel
                       Rgb32fUploadedBaseGeneratesItsChain
                       Rgb32fRenderTargetBaseGeneratesFromTheGpuWrittenLevel)
        mgl_itest_lane("DirectGLES.Monolith.CpuMip." "GenerateMipmapServerScenario.${cpuMipCase}"
            ENV "${MGL_ITEST_GLES_ENVIRONMENT}\;MGITEST_ESPRYT_FORCE_CPU_MIPMAP=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p13-cpu-mip-monolith-${cpuMipCase}.log")
    endforeach()
    foreach(storeReadCase AnUploadedLevelReadsBackExactly
                          ACopiedWindowReadsBackOverTheUpload
                          ACopiedWindowReadsBackFromAnImmutableLevel
                          APartialUploadAfterTheCopyKeepsTheCopiedWindow)
        mgl_itest_lane("DirectGLES.Monolith.StoreRead." "CopyImageStoreReadbackScenario.${storeReadCase}"
            ENV "${MGL_ITEST_GLES_ENVIRONMENT}\;MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=${MGL_P8E_REFUSE_EXTENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p13-store-read-monolith-${storeReadCase}.log")
    endforeach()

    # P8-C: draw inputs a compute shader wrote, read on the CPU by the side that applies the draw
    # (restart rewrite, multi-draw rebase, *IndirectCount, indirect gl_BaseVertex, an orphaned
    # indirect store). Split-only source, like F1: the ambient DirectGLES./DirectVulkan. entries
    # of the split build are the monolith control and the inproc A/B; this call adds the three
    # gated Espryt arms and records the Magma tier-2 replay (integration-magma-all-*).
    target_sources(MobileGLIntegrationTest PRIVATE Scenarios/GpuWrittenDrawInputScenario.cpp)
    mgl_itest_register_split_arms("GpuWrittenDrawInputScenario.*" "")

    # The configure_file that used to sit here MOVED TO THE END OF THIS FILE (P6 `t6`).
    #
    # It substitutes @MGL_SPLIT_ARM_TEST_LISTS@, which the registration macro appends to - so
    # running it here captured only the calls made ABOVE this line and silently dropped the F1
    # and Ct blocks below it. SplitLogPaths.PrivateAndDistinct is what caught that, naming
    # F1WireScenario.ClearBufferfiPixels; the generated file now sees every call.
    # Python is mandatory for this split integration gate.
    # Do not silently omit the ownership check when the interpreter is unavailable.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME SplitLogPaths.PrivateAndDistinct
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Harness/split_log_paths.py"
            check "${CMAKE_CTEST_COMMAND}" "${CMAKE_BINARY_DIR}")
    set_tests_properties(SplitLogPaths.PrivateAndDistinct PROPERTIES LABELS "integration-split")

    # P8-F: `Magma:mipmap-shader-format-or-shape` is a named DECLINE on the wire arm now
    # (WireDeclines.def MipmapShaderFormatOrShape), as the monolith arm's GenerateMipmap already
    # declines. The shape is a 3D chain with the native blit forced off - neither shader mip arm
    # takes it - and the knob is read BY THE SERVER, so split + spawn only (the ShaderMip entries'
    # tcp note below). Its own `.ShaderMipDecline.` tail is in MAGMA_SERVER_ENV_KNOB_NO_TCP
    # (scripts/ci/spawn_lane_parity.py: one tail per knob case). The Fm lane's flavour plus the arm.
    foreach(mglItestArm IN ITEMS Split Spawn)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(mglItestP8fMipEnvironment
            "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
            ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV} ${mglItestArmTransportEnv})
        mgl_itest_lane("DirectVulkan.${mglItestArm}.ShaderMipDecline." "F1WireScenario.GenerateMipmapWithoutAShaderArmShapeDeclinesAndKeepsTheSession"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestP8fMipEnvironment}\;MGITEST_MAGMA_FORCE_SHADER_MIPMAP=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p8f-shadermip-decline${mglItestArmLogTag}.log")
    endforeach()
endif()

# f1: split-only source keeps the monolith registry unchanged.
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_sources(MobileGLIntegrationTest PRIVATE Scenarios/F1WireScenario.cpp)
    foreach(f1Slot ClearBufferfi ClearBufferfv ClearBufferiv ClearBufferuiv ClearNamedFramebufferfi ClearNamedFramebufferfv ClearNamedFramebufferiv ClearNamedFramebufferuiv CopyTexImage2D CopyTexSubImage2D CopyTextureSubImage2D GenerateMipmap GenerateTextureMipmap GenerateMipmapPackedFloat GenerateMipmapDepth)
        mgl_itest_register_split_arms("F1WireScenario.${f1Slot}Pixels" "F1.")
    endforeach()
    # THE TWO PACK-STATE CASES OF F1, which are not ${slot}Pixels-shaped (P3b/P4b espryt D1
    # slice 4). ID-49 splits a readback in two: the server reads with NEUTRAL pack state into a
    # tight w*h*bpp reply, and the CLIENT scatters those rows into the application's destination
    # per its own GL_PACK_*. Only the client half has ever been exercised by a unit case over
    # ScatterTightReadbackIntoPackState; these two run the whole seam, one for the padding a
    # non-default pack state must leave untouched and one for the overflow that has to be refused
    # BEFORE anything is written.
    mgl_itest_register_split_arms(
        "F1WireScenario.TextureAndFramebufferReadsPreservePackBufferPadding" "F1.")
    mgl_itest_register_split_arms(
        "F1WireScenario.TextureReadbackRejectsPackedDestinationOverflowBeforeWriting" "F1.")
    # Magma's framebuffer, copy and mipmap verbs on the three transports (the Fm lane). The flavour
    # is fixed and only the arm supplies the transport, so a red on one arm alone is a transport
    # fact. The flavour appends ${MGL_ITEST_VULKAN_ENV}, not ${MGL_ITEST_COMMON_ENV}: it pins
    # VK_ICD_FILENAMES and the lavapipe iterationRP repairs, so this lane measures the same driver
    # as the monolith `DirectVulkan.` arm instead of whatever ICD the loader finds first.
    set(MGL_ITEST_MAGMA_WIRE_FLAVOUR_ENV
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
    # Every entry here is green on the inproc arm, R11F_G11F_B10F packed-float mip generation
    # included: the GPU shader blit retires the named native-format refusal, so
    # GenerateMipmapPackedFloatPixels carries its siblings' label and their pixels assertion.
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(mglItestMagmaWireEnvironment
            ${MGL_ITEST_MAGMA_WIRE_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        foreach(fmSlot ComputeImageStoreThroughViewPreservesOtherRootLayer EnabledVertexBufferDrawKeepsNamedP7Fatal SrgbDrawTracksFramebufferConversion Texture1DFramebufferClearPixels VertexIdSamplerAndScalarUniformPixels ComputeImageStoreFramebufferPixels PartialTextureUploadPreservesGpuClearPixels TextureViewClearTargetsItsRootMipAndLayer ClearBufferfiPixels ClearBufferfvPixels ClearBufferivPixels ClearBufferuivPixels ClearNamedFramebufferfiPixels ClearNamedFramebufferfvPixels ClearNamedFramebufferivPixels ClearNamedFramebufferuivPixels CopyTexImage2DPixels CopyTexSubImage2DPixels CopyTextureSubImage2DPixels GenerateMipmapPixels GenerateTextureMipmapPixels GenerateMipmapPackedFloatPixels GenerateMipmapDepthPixels)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Fm." "F1WireScenario.${fmSlot}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5f-fm${mglItestArmLogTag}-${fmSlot}.log")
        endforeach()
        foreach(fmCase ClipDistanceScenario.AnEnabledClipDistanceRemovesTheNegativeHalf
                       ClipDistanceScenario.ADisabledClipDistanceRemovesNothing
                       ClipDistanceScenario.TheEnablesAreIndependentPerDistance
                       CopyImageLayeredScenario.ArrayToArrayCopiesEverySlice
                       CopyImageLayeredScenario.ArrayToArrayHonoursDifferentLayerOffsets
                       CopyImageLayeredScenario.VolumeToVolumeHonoursNonZeroZ
                       CopyImageLayeredScenario.VolumeToVolumeAtNonZeroMipLevel
                       CopyImageLayeredScenario.ArrayToVolumeCopiesEverySlice
                       CopyImageLayeredScenario.VolumeToArrayCopiesEverySlice)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Fm." "${fmCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5f-fm${mglItestArmLogTag}-${fmCase}.log")
        endforeach()
        # P7 gate 5 (g5-msrbo): the empty-level pair, on the gated Magma lanes as well - the CTS
        # arm gate 5 measures is inproc x DirectVulkan, and every one of its cases ends in the
        # empty-level reset this scenario reproduces.
        foreach(emptyLevelCase TheCtsStateResetDefinesEveryDefaultTargetEmpty
                               ALevelDefinedEmptyThenRedefinedCarriesTheNewTexels)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Fm." "EmptyTextureLevelScenario.${emptyLevelCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-emptylevel${mglItestArmLogTag}-${emptyLevelCase}.log")
        endforeach()
        # ShaderMip<tier>: MGITEST_MAGMA_FORCE_SHADER_MIPMAP makes GenerateWireMipmap skip its
        # native blit (tier 1) and also treat the level as not attachable (tier 2), the arms
        # devices take. Lavapipe blits every colour format this binary creates, R11F_G11F_B10F
        # included, so no format choice reaches them. Own entries: the knob retargets every mipmap
        # in the process. A server-read knob set per entry never reaches the shared tcp fixture's
        # server, so knob entries skip the Tcp arm (mglItestServerEnvKnobArm) and are listed in
        # spawn_lane_parity.py's MAGMA_SERVER_ENV_KNOB_NO_TCP; knob=0 twins keep all three arms.
        # ARCHITECTURE.md §16.1.
        set(mglItestServerEnvKnobArm TRUE)
        if(mglItestArm STREQUAL "Tcp")
            set(mglItestServerEnvKnobArm FALSE)
        endif()
        foreach(shaderMipTier 1 2)
            if(NOT mglItestServerEnvKnobArm)
                continue()
            endif()
            mgl_itest_lane("DirectVulkan.${mglItestArm}.ShaderMip${shaderMipTier}." "F1WireScenario.GenerateMipmapWithoutNativeBlitPixels"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_SHADER_MIPMAP=${shaderMipTier}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-shadermip${mglItestArmLogTag}-tier${shaderMipTier}.log")
        endforeach()
        # The DEPTH half of the same knob (CONTRACT-P7 §5.1 (A)). ONE TIER, not two: the depth
        # pass renders into a depth ATTACHMENT view of the destination level, which a depth
        # image always has usage for - there is no "cannot be attached" shape to force, so
        # tier 2 would register a second copy of tier 1's run. Split+spawn only, for the
        # server-side-knob reason above.
        if(mglItestServerEnvKnobArm)
        mgl_itest_lane("DirectVulkan.${mglItestArm}.DepthMip." "F1WireScenario.GenerateMipmapDepthWithoutNativeBlitPixels"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_SHADER_MIPMAP=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-depthmip${mglItestArmLogTag}.log")
        endif()
        # NO KNOB on the decline: it is decided on the aspect before the blit-support question
        # is asked, so the plain Fm environment reaches it on every driver.
        mgl_itest_lane("DirectVulkan.${mglItestArm}.Fm." "F1WireScenario.GenerateMipmapDepthStencilDeclinesAndKeepsTheSession"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-dsmipdecline${mglItestArmLogTag}.log")
        # The CopyInPlace. entries further down have no monolith `DirectVulkan.` twin: the monolith
        # arm declines every in-place copy, the wire arm performs them.
        # PipelineDelay: MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS=25 adds 25 ms to every pipeline the
        # server creates, as a cold driver cache does (OpenRA's split readback diverged on a device
        # only with a cold cache); the Tcp copy runs undelayed, its server being the lane fixture.
        # They pin that a readback after a draw returns its pixels. A regression gate, not a
        # reproduction: lavapipe does not diverge even without RecordWireReadbackWriteBarrier, as
        # its default framebuffer is tracked GENERAL and that transition waits on ALL_COMMANDS.
        foreach(pipelineDelayCase VertexIdSamplerAndScalarUniformPixels
                                  ComputeImageStoreFramebufferPixels
                                  GenerateMipmapPixels)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.PipelineDelay." "F1WireScenario.${pipelineDelayCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS=25\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-pipelinedelay${mglItestArmLogTag}-${pipelineDelayCase}.log")
        endforeach()
        # ---- P7 wave 2 package B3, EXIT GATE 3: the streamed `glBufferSubData -> draw` pair.
        #
        # TWO ENTRIES OVER ONE CASE, for the reason the ShaderMip pair gives. The plain entry is
        # a regression gate that runs on every driver. The forced entry is the red-once of the
        # DEFENCE term: it makes the serial half of VkBufferManager's busy predicate answer
        # "idle" - the state an unsound completed-serial floor put the wire arm into on the
        # device - so only the submission-fence half is left to order the copy. It does NOT
        # cover the floor fix itself (forcing the serial term bypasses the floor entirely); the
        # floor's lane is the OpenRA split retrace, which run_trace_case.cmake reds on any
        # "MGWIRE-FLOOR unsound-serial-complete" line.
        #
        # ITS OWN ENTRY RATHER THAN A LANE-WIDE FLIP: the knob changes how EVERY streamed buffer
        # write in the process is answered, so putting it in the shared Fm environment would
        # retarget every other draw case in this lane.
        mgl_itest_lane("DirectVulkan.${mglItestArm}.Fm." "F1WireScenario.StreamedBufferSubDataBeforeEachDrawIsOrdered"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-streamedsubdata${mglItestArmLogTag}.log")
        # SPLIT AND SPAWN ONLY - a parity exception, named in scripts/ci/spawn_lane_parity.py
        # (MAGMA_SERVER_ENV_KNOB_NO_TCP, the same list as the B2 knobs above), not an omission.
        # The knob is read by the SERVER, and the tcp arm's server is the lane fixture
        # (scripts/ci/tcp_server_fixture.py) started once from its own environment: an
        # entry-level ENVIRONMENT never reaches it, so a Tcp.StaleSerial. entry would run the
        # plain Fm case under a name that claims the knob, and could never red. The inproc arm
        # hosts the server in the test process and the spawn arm's server inherits the client's
        # environment, so on those two the knob is real.
        if(mglItestServerEnvKnobArm)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.StaleSerial." "F1WireScenario.StreamedBufferSubDataBeforeEachDrawIsOrdered"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_STALE_BUFFER_SERIAL=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-streamedsubdata-stale${mglItestArmLogTag}.log")
        endif()
        # `default-color-blit-shape@P7`'s retirement. TWO ENTRIES OVER ONE CASE, and the second
        # is the only way its arm is reachable here: every surface this lane creates reports
        # VK_SURFACE_TRANSFORM_IDENTITY, so the scratch path that exists FOR rotation would
        # never run. Forced onto an identity surface it must produce the SAME pixels as the
        # fall-through, which is what the shared assertions check.
        # The knob=1 copy is split+spawn only (the server-side-knob note above); knob=0 is the
        # default and means the same thing on all three arms, so it keeps its tcp entry.
        foreach(defaultBlitScratch 0 1)
            if(defaultBlitScratch AND NOT mglItestServerEnvKnobArm)
                continue()
            endif()
            mgl_itest_lane("DirectVulkan.${mglItestArm}.DefaultBlitShape${defaultBlitScratch}." "F1WireScenario.ColorBlitToDefaultFromANonSampleableSourceDegrades"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_DEFAULT_BLIT_SCRATCH=${defaultBlitScratch}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-defaultblit${mglItestArmLogTag}-scratch${defaultBlitScratch}.log")
        endforeach()
        # ---- P7 wave 2-B2: `multisample-blit-shape@P7`'s retirement and its decline ----------
        #
        # SPLIT-ONLY, like the in-place copy pair below and for the same reason: the retired arm
        # does something the monolith arm does not. A scaled multisample colour blit takes the
        # monolith's `vkCmdResolveImage` at 1:1 into the destination's offset - the scale is
        # dropped - so there is no `DirectVulkan.` reading to register these beside, and an
        # equality between the two arms would be an assertion that the wire arm is as wrong.
        #
        # NO KNOB: every driver that can host a 4x multisample RGBA8 renderbuffer reaches both
        # entries with the plain Fm environment, and one that cannot says so as a SKIP.
        foreach(multisampleBlitCase MultisampleColorBlitScalesAndFlipsThroughTheResolveScratch
                                    MultisampleBlitOntoAMultisampleDestinationDeclinesAndKeepsTheSession
                                    MultisampleBlitFromASingleSampleSourceDeclinesAndKeepsTheSession)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.MsBlit." "F1WireScenario.${multisampleBlitCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-msblit${mglItestArmLogTag}-${multisampleBlitCase}.log")
        endforeach()
        # MsResolve<0|1>: a 4x multisample DEPTH24_STENCIL8 renderbuffer resolved with
        # glBlitFramebuffer and both aspects read back; a one-aspect resolve must leave the other
        # aspect of the destination alone. Lavapipe has VK_KHR_create_renderpass2 and
        # VK_KHR_depth_stencil_resolve, so only MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE=1 reaches
        # the shader resolve that devices without them take; both entries expect sample zero.
        # Separate entries rather than a lane-wide flip: the knob changes every depth/stencil
        # resolve in the process.
        foreach(shaderDepthResolve 0 1)
            # Same split+spawn restriction, same reason, same parity exception: the knob is read
            # on the server and the tcp server is the lane fixture. MsResolve0 stays everywhere.
            if(shaderDepthResolve AND NOT mglItestServerEnvKnobArm)
                continue()
            endif()
            # P7 gate 5 (g5-msprobe): MsResolve0 on split and spawn also pins the REAL resolve probe
            # on this host - it must run at the server's bring-up, find lavapipe's no-draw resolve
            # render pass clean, and leave that pass first. MGITEST_EXPECT_DEPTH_RESOLVE_PROBE is a
            # marker only the scenario reads (DepthStencilReadbackMatrixScenario::
            # ExpectTheResolveArmWhereTheLaneAsks), and only where the server's private log belongs
            # to the entry - the tcp server is the lane fixture, so its MsResolve0 asserts pixels only.
            set(msResolveProbeMarker "")
            if(NOT shaderDepthResolve AND mglItestServerEnvKnobArm)
                set(msResolveProbeMarker "\;MGITEST_EXPECT_DEPTH_RESOLVE_PROBE=clean")
            endif()
            mgl_itest_lane("DirectVulkan.${mglItestArm}.MsResolve${shaderDepthResolve}." "DepthStencilReadbackMatrixScenario.AResolvedMultisampleDepthReadsBackFromTheDestination"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE=${shaderDepthResolve}${msResolveProbeMarker}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-msresolve${mglItestArmLogTag}-shader${shaderDepthResolve}.log")
        endforeach()
        # ---- P7 gate 5 (g5-msrbo): the SINGLE-SAMPLED depth/stencil blit ----------------------
        #
        # KHR-GL46.direct_state_access.renderbuffers_storage_multisample's samples == 0 leg, every
        # depth/stencil format: the wire arm moved every single-sample depth/stencil aspect with
        # vkCmdBlitImage where the monolith copies at 1:1, and for one aspect of a packed format
        # that moved the whole native word (a depth-only blit replaced the destination's stencil
        # on lavapipe). NO KNOB: the packed-aspect leg is red on lavapipe without the copy arm
        # (measured there only - a driver whose one-aspect blit keeps the other aspect stays green
        # without it), and nothing in it is read on the server, so all three arms carry it.
        mgl_itest_lane("DirectVulkan.${mglItestArm}.DsBlit." "DepthStencilReadbackMatrixScenario.ASingleSampledDepthStencilBlitCopiesEveryAspect"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-dsblit${mglItestArmLogTag}.log")
        # P7 wave 2-B2 (review round): a FLIPPED multisample depth/stencil resolve is a legal blit
        # that used to end the session, and a SCALED one is INVALID_OPERATION that used to end it
        # too. Both shapes are decided on the rectangles, before any capability question, so this
        # entry needs NO knob and runs on all three arms.
        mgl_itest_lane("DirectVulkan.${mglItestArm}.MsFlip." "DepthStencilReadbackMatrixScenario.AFlippedMultisampleResolveMirrorsTheBandsAndAScaleDeclines"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-msflip${mglItestArmLogTag}.log")
        # ITS KNOB TWIN (review round 2). Knob-free, lavapipe answers every resolve through the
        # VK_KHR_depth_stencil_resolve arm, so the shader arm's min-corner rectangle under a
        # flip - the device WITHOUT the extension - never ran. MsFlip1 forces it. Split+spawn
        # only, the server-side-knob reason above, named in spawn_lane_parity.py's
        # MAGMA_SERVER_ENV_KNOB_NO_TCP; the knob-free MsFlip. keeps all three arms.
        if(mglItestServerEnvKnobArm)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.MsFlip1." "DepthStencilReadbackMatrixScenario.AFlippedMultisampleResolveMirrorsTheBandsAndAScaleDeclines"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-msflip${mglItestArmLogTag}-shader1.log")
        endif()
        # MsResolveBug / MsFlipBug: the resolve arm order on a defective device. Where the server's
        # resolve probe finds the no-draw VK_KHR_depth_stencil_resolve pass leaving its target
        # unwritten (Adreno 830), ResolveWireDepthStencil tries the shader pass first. Lavapipe
        # resolves through both arms, so MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=bug hands the arm choice
        # a canned defect measurement (VulkanRenderer::ArmWireDepthResolveOrder). The cases assert
        # their pixels and, from the server log, that the shader pass resolved
        # (DepthStencilReadbackMatrixScenario::ExpectTheResolveArmWhereTheLaneAsks).
        # Server-read knob: split and spawn only (ARCHITECTURE.md §16.1).
        if(mglItestServerEnvKnobArm)
            foreach(probeBugCase MsResolveBug:AResolvedMultisampleDepthReadsBackFromTheDestination
                                 MsFlipBug:AFlippedMultisampleResolveMirrorsTheBandsAndAScaleDeclines)
                string(REPLACE ":" ";" probeBugPair "${probeBugCase}")
                list(GET probeBugPair 0 probeBugTail)
                list(GET probeBugPair 1 probeBugTest)
                string(TOLOWER "${probeBugTail}" probeBugLog)
                mgl_itest_lane("DirectVulkan.${mglItestArm}.${probeBugTail}." "DepthStencilReadbackMatrixScenario.${probeBugTest}"
                    LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=bug\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-${probeBugLog}${mglItestArmLogTag}.log")
            endforeach()
        endif()
        # MsResolveElide: negative control for the real probe (WireDepthResolveProbe.cpp), whose
        # recording, readback and tally the canned Bug measurement never exercises.
        # MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=elide-subject runs it with the render-pass resolve left
        # unrecorded, so the subject keeps its sentinel as on the Adreno 830 while the shader
        # control resolves. The entry requires verdict=render-pass-resolve-broken and the shader
        # arm (ExpectTheResolveArmWhereTheLaneAsks). Server-read knob: split and spawn only.
        if(mglItestServerEnvKnobArm)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.MsResolveElide." "DepthStencilReadbackMatrixScenario.AResolvedMultisampleDepthReadsBackFromTheDestination"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=elide-subject\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-msresolveelide${mglItestArmLogTag}.log")
        endif()
        foreach(inPlaceCase CopyImageInPlaceAcrossLevelsAndLayers
                            CopyImageInPlaceOverlapDeclinesAndLeavesTheLevelAlone)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.CopyInPlace." "F1WireScenario.${inPlaceCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-copyinplace${mglItestArmLogTag}-${inPlaceCase}.log")
        endforeach()
        # ---- P7 gate 5 (g5-imgwin): the image-unit window ------------------------------------
        #
        # KHR-GL46.shader_image_load_store.incomplete_textures ended the inproc Redmi run at
        # Fatal{UnmigratedVerb, "Magma:image-view-window"}: the unit named level 2 of a texture
        # with only level 0, a monolith Fail turned into a SIGABRT. GL 4.6 core 8.26 makes that
        # access invalid - zero loads, dropped stores - and the wire descriptor resolve now binds a
        # null storage descriptor for it where the device enables VK_EXT_robustness2 nullDescriptor,
        # else a placeholder private to the unit (UniformManager::ResolveWireImageDescriptor and
        # ResolveWirePlaceholderImage). One entry per case for the private log path; NO KNOB,
        # so all three arms. The monolith `DirectVulkan.` entry of the same scenario skips by
        # name (it drops the draw: the gate 5 BASE, and a pull-build .text change to move).
        # ALevelPastATextureViewsWindowReadsZero joined once the F2 repair retired the transport
        # glTextureView death (see the split-arm registration of this scenario).
        foreach(imageWindowCase AMissingMipLevelOfAnIncompleteTextureReadsZeroInADraw
                                AMissingMipLevelOfAnIncompleteTextureReadsZeroInADispatch
                                ALevelPastAnImmutableTexturesStorageReadsZero
                                ALayerPastAnArraysLastSliceReadsZero
                                ATextureWithNoStorageReadsZero
                                ALevelPastATextureViewsWindowReadsZero
                                AStoreThroughAnInvalidUnitIsLoadedThroughNeitherAnotherInvalidUnitNorItself
                                AStoreThroughAnEmptyUnitIsLoadedThroughNeitherAnotherEmptyUnitNorItself)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.ImageUnitWindow." "InvalidImageUnitScenario.${imageWindowCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-imagewindow${mglItestArmLogTag}-${imageWindowCase}.log")
        endforeach()
        # ---- Codex closeout finding 2 (cf-magma): THE ARM WITHOUT A NULL DESCRIPTOR -----------
        #
        # The two AStoreThrough... cases above run the null-descriptor arm on this host: lavapipe
        # enables VK_EXT_robustness2 nullDescriptor, so an invalid or empty storage unit binds a
        # null descriptor. A device without it binds a placeholder PRIVATE to the unit (per shape)
        # instead; MGITEST_MAGMA_FORCE_PRIVATE_IMAGE_PLACEHOLDER=1 puts the server on that arm
        # (UniformManager::SetWireInvalidStorageImageArm), and the client reads the same marker to
        # expect it: B's load zero (no alias), A's own load of its store = the recorded CONTRACT-P7
        # §12 residual, which is also what proves the knob reached the server.
        #
        # Split+spawn only, the server-side-knob reason above, named in spawn_lane_parity.py's
        # MAGMA_SERVER_ENV_KNOB_NO_TCP.
        if(mglItestServerEnvKnobArm)
            foreach(privateCase AStoreThroughAnInvalidUnitIsLoadedThroughNeitherAnotherInvalidUnitNorItself
                                AStoreThroughAnEmptyUnitIsLoadedThroughNeitherAnotherEmptyUnitNorItself)
                mgl_itest_lane("DirectVulkan.${mglItestArm}.ImageUnitPrivate." "InvalidImageUnitScenario.${privateCase}"
                    LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_PRIVATE_IMAGE_PLACEHOLDER=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-imageprivate${mglItestArmLogTag}-${privateCase}.log")
            endforeach()
        endif()
    endforeach()
endif()

# P7 gate 5 (g5-readback): BANDED READBACK ON MAGMA, all three arms, GATED. The flavour is the
# one the device CTS window ran KHR-GL46 under (role-split state, strict errors, run-ahead), so the
# host lane measures the configuration that aborted on the Redmi with
# Fatal{ReplyTooLarge, "ReadPixels 256x512 0x1908/0x1406 2097152 > 2097136"}. One entry per case
# for a private log each - two processes append to it on the spawn arm.
if (MOBILEGL_BUILD_DISAGGREGATED)
    # P11: LargeArenaAdoptionScenario's cases that run on every Magma arm - the five behavioural
    # cases and the tier assertion (the mpr and adopt-tier-refusal cases need lane markers of their
    # own). The `Arena.` block below and the Buffers lane both walk it.
    set(MGL_ITEST_ARENA_CASES
        SubDataAfterAnInFlightDrawReachesTheNextDraw
        RespecifiedVertexArenaKeepsVaoBindings
        RespecifiedIndexArenaKeepsVaoBinding
        ReadbackSeesTheLatestCpuWrite
        GpuWriteIntoTheArenaIsReadBack
        TheArenaLandsInTheTierItsLaneDeclares)
    set(MGL_ITEST_MAGMA_BAND_FLAVOUR_ENV
        "MOBILEGL_BACKEND_TYPE=DirectVulkan"
        "MOBILEGL_IPC_ROLE_SPLIT_STATE=1" "MOBILEGL_IPC_STRICT_ERRORS=1" "MOBILEGL_IPC_RUN_AHEAD=1"
        "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})  # the ICD pin, see the Fm lane above
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(mglItestMagmaBandEnvironment
            ${MGL_ITEST_MAGMA_BAND_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        # Where the >= 16 MiB arena lands on this arm (P11 A2). Split is one process: T0 (the default
        # tier) is granted in process and the arena is the store Magma hands its client - adopted.
        # Across processes a host has no shareable memory, so the session runs T2 - emulated.
        if("${mglItestArm}" STREQUAL "Split")
            set(mglItestArenaArm adopted)
        else()
            set(mglItestArenaArm emulated)
        endif()
        foreach(bandCase RenderbufferReadOfExactlyTwoMiBIsBandedNotRefused
                         TwoMiBPlusOneRowIsBanded
                         AReadLargerThanTheWholeReplySegmentIsBanded
                         BandedReadsHonourPackAlignmentRowLengthAndSkips
                         BandedReadIntoAPixelPackBufferKeepsItsPadding
                         BandedDepthAndDepthStencilReads
                         GetTexImageLargerThanAReplySlotIsWindowed)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Band." "BandedReadbackScenario.${bandCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaBandEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/g5-band${mglItestArmLogTag}-${bandCase}.log")
        endforeach()
        # P9 (W1): the pack-buffer readback on Magma. The server half is backend-neutral - the
        # reply form's read, then the applier's buffer-write gate (WriteWireBuffer here) - so the
        # same seven cases, same flavour.
        foreach(pboCase ReadPixelsIntoAPackBufferMatchesClientMemoryUnderEveryPackState
                        MapBufferRangeAfterAPackBufferReadSeesThePixels
                        SwapBytesIntoAPackBufferMatchesClientMemory
                        GetTexImageIntoAPackBufferMatchesClientMemory
                        APackBufferReadFeedsAnUploadFromTheSameBuffer
                        ALaterBufferSubDataWinsOverThePackReadWhereTheyOverlap
                        APackBufferReadPostsNoReplyAndMarksTheBuffer)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Pbo." "PackBufferReadbackScenario.${pboCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaBandEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p9-pbo${mglItestArmLogTag}-${pboCase}.log")
        endforeach()
        # P10 (A): the fence polls on Magma. The server half is backend-neutral (the backend
        # table's FenceSync / GetSyncStatus / ClientWaitSync), so the same three cases.
        foreach(fenceCase ZeroTimeoutPollLoopsEndInBoundedTime
                          PollsOfASignaledFenceCrossNothing
                          AFenceThatFinishesWhileTheClientOnlyPollsIsReported)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Fence." "FencePollScenario.${fenceCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaBandEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p10-fence${mglItestArmLogTag}-${fenceCase}.log")
        endforeach()
        # P11 A2 (docs/archive/contracts/CONTRACT-P11.md §2): the >= 16 MiB arena on Magma's three arms, in the
        # same flavour, declared per arm (mglItestArenaArm above); DirectVulkan.ArenaArm. is its
        # monolith (adopted) half.
        foreach(arenaCase IN LISTS MGL_ITEST_ARENA_CASES)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Arena." "LargeArenaAdoptionScenario.${arenaCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaBandEnvironment}\;MGITEST_PERSISTENT_MAP_ARM=${mglItestArenaArm}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p11-arena${mglItestArmLogTag}-${arenaCase}.log")
        endforeach()
        # P11 B2 addendum: DeletedBoundBufferScenario on Magma's three arms, same flavour (the
        # Espryt arms come from mgl_itest_register_split_arms). Before the frontend fix Magma did
        # not die: the stale handle resolved to no wire buffer and the draw / dispatch declaring
        # the point was dropped - which these cases' markers catch.
        foreach(deletedCase AStorageBufferDeletedWhileBoundLeavesNoStaleBinding
                            AUniformBufferDeletedWhileBoundLeavesNoStaleBinding
                            AnAtomicCounterBufferDeletedWhileBoundLeavesNoStaleBinding
                            ATransformFeedbackBufferDeletedWhileBoundLeavesNoStaleBinding)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.DeletedBound." "DeletedBoundBufferScenario.${deletedCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaBandEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p11-deleted-bound${mglItestArmLogTag}-${deletedCase}.log")
        endforeach()
        # ...and the one-process arena pair (the Espryt half is beside mgl_itest_register_split_arms).
        string(REPLACE "\\;" ";" mglItestPairEnv "${mglItestMagmaBandEnvironment}")
        list(APPEND mglItestPairEnv "MGITEST_PERSISTENT_MAP_ARM=${mglItestArenaArm}"
             "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p11-arena-pair${mglItestArmLogTag}.log")
        string(REPLACE "\\;" ";" mglItestPairLabels "${mglItestMagmaLabels}")
        set(mglItestPairName "DirectVulkan.${mglItestArm}.Arena.LargeArenaAdoptionScenario.GpuWriteThenTheTierCaseInOneProcess")
        add_test(NAME "${mglItestPairName}"
            COMMAND MobileGLIntegrationTest
                "--gtest_filter=LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack:LargeArenaAdoptionScenario.TheArenaLandsInTheTierItsLaneDeclares"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties("${mglItestPairName}" PROPERTIES
            LABELS "${mglItestPairLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
            ENVIRONMENT "${mglItestPairEnv}"
            ${mglItestArmProperties})
        # P11 A1 + B2 (CONTRACT-P11 §1, B2): MOBILEGL_IPC_ADOPT_TIER=0 on every arm, one case set.
        # On tcp a stream refuses the tier by name at the client's handshake (A1, the stream case
        # reads the line); on spawn the knob ASKS T0, a host has no AHardwareBuffer, and the session
        # falls back to T2 by name (B2, the fallback case reads both lines); on split the client is
        # the server's process and T0 is granted in process (the tier case reads `adopted`).
        # Register only the assertion applicable to each transport; the parity check names these
        # transport-specific exceptions explicitly.
        if(mglItestArm STREQUAL "Tcp")
            set(mglItestAdoptTier0Env "MGITEST_ADOPT_TIER_REFUSAL=T0")
            set(mglItestAdoptTier0Case AStreamSessionRefusesTheAdoptTierOnceByName)
        elseif(mglItestArm STREQUAL "Split")
            set(mglItestAdoptTier0Env "MOBILEGL_IPC_ALLOW_ADOPT_T0=1")
            set(mglItestAdoptTier0Case)
        else()
            set(mglItestAdoptTier0Env "MGITEST_ADOPT_T0_FALLBACK=no AHardwareBuffer")
            set(mglItestAdoptTier0Case ASessionThatCannotRunT0FallsBackToT2OnceByName)
        endif()
        foreach(arenaCase IN LISTS MGL_ITEST_ARENA_CASES ITEMS ${mglItestAdoptTier0Case})
            mgl_itest_lane("DirectVulkan.${mglItestArm}.AdoptTier0." "LargeArenaAdoptionScenario.${arenaCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaBandEnvironment}\;MOBILEGL_IPC_ADOPT_TIER=0\;${mglItestAdoptTier0Env}\;MGITEST_PERSISTENT_MAP_ARM=${mglItestArenaArm}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p11-adopt-tier0${mglItestArmLogTag}-${arenaCase}.log")
        endforeach()
    endforeach()
endif()

# P9 W2: THE TEXTURE RE-MINT ON MAGMA, all three arms, GATED. A storage-usage upgrade recreates
# the VkImage and copies the old one into it GPU to GPU (PreserveTextureContentsOnRecreate), so
# no byte comes from the client here either (docs/archive/notes/p9/W2-REMINT.md). The gated band flavour
# (role-split state, strict errors, run-ahead), one entry per case for a private log each.
if (MOBILEGL_BUILD_DISAGGREGATED)
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(mglItestMagmaRemintEnvironment
            ${MGL_ITEST_MAGMA_BAND_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        foreach(remintCase AnUploadedMutableTextureKeepsItsTexelsAcrossTheImageBind
                           AGpuClearAfterTheUploadSurvivesTheImageBind
                           AnImmutableTexturesGpuClearSurvivesTheImageBind
                           ANullDefinedTextureWrittenOnlyByTheGpuSurvivesTheImageBind
                           APendingPartialUploadMergesOverGpuWrittenTexels
                           AGeneratedMipLevelSurvivesTheImageBind
                           AGpuClearedMipLevelSurvivesTheImageBind
                           ALevelDefinedWithoutDataAfterTheLastSyncDoesNotEndTheSession
                           ALevelRedefinedLargerWithoutDataAfterTheLastSyncDoesNotEndTheSession
                           APartialUploadIntoALevelDefinedAfterTheLastSyncReachesTheImage
                           AWidenedFormatsGpuClearSurvivesTheCarrierRemint
                           AWidenedFormatsUploadSurvivesTheCarrierRemint
                           AnRgba8SnormUploadSurvivesTheImageBind
                           AnRgba16SnormImmutableUploadSurvivesTheCarrierRemint)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Remint." "TextureRemintPullScenario.${remintCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaRemintEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p9-remint${mglItestArmLogTag}-${remintCase}.log")
        endforeach()
    endforeach()
endif()

# P8-B: THE MAGMA CONTROL for GenerateMipmapServerScenario, all three arms, GATED, in the band
# flavour. Magma generates every case on the GPU (GenerateWireMipmap), so these entries say what
# the Espryt arms above must match. One entry per case for a private log each.
if (MOBILEGL_BUILD_DISAGGREGATED)
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(mglItestMagmaMipEnvironment
            ${MGL_ITEST_MAGMA_BAND_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        foreach(mipCase PackedFloatMutableMaxLevelBelowTheChain
                        PackedFloatStorageWithFewerLevelsThanTheChain
                        PackedFloatMutableBaseLevelOneKeepsTheLevelsOutsideItsWindow
                        DepthMutableMaxLevelBelowTheChain
                        DepthStorageWithFewerLevelsThanTheChain
                        DepthMutableBaseLevelOneKeepsTheLevelsOutsideItsWindow
                        Rgb16fUploadedBaseGeneratesItsChain
                        Rgb16fRenderTargetBaseGeneratesFromTheGpuWrittenLevel
                        Rgb32fUploadedBaseGeneratesItsChain
                        Rgb32fRenderTargetBaseGeneratesFromTheGpuWrittenLevel)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Mip." "GenerateMipmapServerScenario.${mipCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaMipEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p8b-mip${mglItestArmLogTag}-${mipCase}.log")
        endforeach()
    endforeach()
endif()

# P11 M: DEV'S TWO GPU-CONTENT SCENARIOS ON MAGMA, all three arms, GATED, in the band flavour -
# ImageBindableRemintScenario (Magma is its control backend: a storage upgrade copies image to
# image) and PartialUploadOverGpuContentScenario (8966d5c2's RGB->RGBA region cut and b1d60523's
# upload-after-clear/draw ordering, both of which run in the server's renderer on these arms).
# One entry per scenario and arm; every entry gets a private log through MGL_SPLIT_ARM_TEST_LISTS.
if (MOBILEGL_BUILD_DISAGGREGATED)
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(mglItestMagmaGpuContentEnvironment
            ${MGL_ITEST_MAGMA_BAND_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        foreach(mglItestGpuContentScenario ImageBindableRemintScenario PartialUploadOverGpuContentScenario)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.GpuContent." "${mglItestGpuContentScenario}.*"
                LABELS "${mglItestMagmaLabels}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaGpuContentEnvironment}")
            list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
            math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
        endforeach()
    endforeach()
endif()

# Magma's oversized-recording split and descriptor pool growth on the gated arms (band flavour);
# every entry gets a private log through MGL_SPLIT_ARM_TEST_LISTS.
#   Oversized.  OversizedRecordingSplitScenario at the default draw budget (16384), all arms:
#       the unsplit control.
#   SplitRecording64.  MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER=64 (server-read) makes every
#       case split and assert the server's once-per-process log line: one entry per case, no tcp.
#   DescPool.  DescriptorPoolGrowthScenario, split only: GetDescriptorPoolCensus reads in process
#       and answers unavailable when the server is another process (ARCHITECTURE.md §16.1).
if (MOBILEGL_BUILD_DISAGGREGATED)
    set(MGL_ITEST_OVERSIZED_CASES
        EveryDrawOfTheFrameLandsAcrossCommandBufferSplits
        ADepthOccluderDrawnBeforeTheSplitHidesADrawAfterIt
        AStencilMarkMadeBeforeTheSplitGatesADrawAfterIt
        BlendingAccumulatesOntoPixelsWrittenBeforeTheSplit
        AMultisampleTargetKeepsItsSamplesAcrossTheSplit
        OcclusionQueriesOpenAcrossTheSplitCountBothSides
        APrimitivesGeneratedQueryOpenAcrossTheSplitCountsBothSides
        ATransformFeedbackCaptureActiveAcrossTheSplitAppendsEveryDraw)
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(mglItestMagmaM2Environment
            ${MGL_ITEST_MAGMA_BAND_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        mgl_itest_lane("DirectVulkan.${mglItestArm}.Oversized." "OversizedRecordingSplitScenario.*"
            LABELS "${mglItestMagmaLabels}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaM2Environment}")
        list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
        math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
        if(NOT mglItestArm STREQUAL "Tcp")
            foreach(mglItestOversizedCase IN LISTS MGL_ITEST_OVERSIZED_CASES)
                mgl_itest_lane("DirectVulkan.${mglItestArm}.SplitRecording64." "OversizedRecordingSplitScenario.${mglItestOversizedCase}"
                    LABELS "${mglItestMagmaLabels}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaM2Environment}\;MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER=64")
                list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
                math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
            endforeach()
        endif()
        if(mglItestArm STREQUAL "Split")
            mgl_itest_lane("DirectVulkan.Split.DescPool." "DescriptorPoolGrowthScenario.*"
                LABELS "${mglItestMagmaLabels}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaM2Environment}")
            list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
            math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
        endif()
    endforeach()
endif()

# Async compilation, the XFB primitive query pair and GL_PRIMITIVES_GENERATED on Magma's gated
# arms (band flavour: role-split state, strict errors, run-ahead); every entry gets a private log
# through MGL_SPLIT_ARM_TEST_LISTS. AsyncOn./AsyncOff./OptimisticShaderStatus. are the monolith
# entries' three AsyncCompileScenario configurations and keep tcp, as the switch is client-side.
# PrimGen. uses the bring-up probe's Auto verdict and leaves out the arming case, which only runs
# with the reroute pinned. PrimGenReroute. pins MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE=1, which the
# server reads, so it has no tcp arm (ARCHITECTURE.md §16.1); each case is its own ctest process,
# which the once-per-process arming log line needs.
if (MOBILEGL_BUILD_DISAGGREGATED)
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(mglItestMagmaP10bEnvironment
            ${MGL_ITEST_MAGMA_BAND_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        set(mglItestP10bShapes AsyncOn AsyncOff OptimisticShaderStatus XfbQuery PrimGen)
        if(NOT mglItestArm STREQUAL "Tcp")
            list(APPEND mglItestP10bShapes PrimGenReroute)
        endif()
        foreach(mglItestP10bShape IN LISTS mglItestP10bShapes)
            set(mglItestP10bKnobs "")
            if(mglItestP10bShape STREQUAL "AsyncOn")
                set(mglItestP10bFilter
                    "AsyncCompileScenario.*-AsyncCompileScenario.IrisShapedTwoPhaseBatchRendersCorrectly")
                set(mglItestP10bKnobs "\;MOBILEGL_ASYNC_SHADER_COMPILE=1")
            elseif(mglItestP10bShape STREQUAL "AsyncOff")
                set(mglItestP10bFilter "AsyncCompileScenario.ExtensionStringMatchesTheConfiguration")
                set(mglItestP10bKnobs "\;MOBILEGL_ASYNC_SHADER_COMPILE=0")
            elseif(mglItestP10bShape STREQUAL "OptimisticShaderStatus")
                set(mglItestP10bFilter "AsyncCompileScenario.IrisShapedTwoPhaseBatchRendersCorrectly")
                set(mglItestP10bKnobs
                    "\;MOBILEGL_ASYNC_SHADER_COMPILE=1\;MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS=1")
            elseif(mglItestP10bShape STREQUAL "XfbQuery")
                set(mglItestP10bFilter "XfbPrimitiveQueryScenario.*")
            elseif(mglItestP10bShape STREQUAL "PrimGen")
                set(mglItestP10bFilter
                    "PrimitivesGeneratedNoXfbScenario.*-PrimitivesGeneratedNoXfbScenario.TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn")
            else()
                set(mglItestP10bFilter "PrimitivesGeneratedNoXfbScenario.*")
                set(mglItestP10bKnobs "\;MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE=1")
            endif()
            mgl_itest_lane("DirectVulkan.${mglItestArm}.${mglItestP10bShape}." "${mglItestP10bFilter}"
                LABELS "${mglItestMagmaLabels}" TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaP10bEnvironment}${mglItestP10bKnobs}")
            list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
            math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
        endforeach()
    endforeach()
endif()

# Magma application buffers: public GL pixels/bytes, with the two role blocks and
# strict field checks forced on. Existing public scenarios supply offset, mutation,
# persistent-map, growth and indirect coverage; no copied backend implementation.
if (MOBILEGL_BUILD_DISAGGREGATED)
    set(MGL_ITEST_MAGMA_BUFFER_FLAVOUR_ENV
        "MOBILEGL_BACKEND_TYPE=DirectVulkan"
        "MOBILEGL_IPC_ROLE_SPLIT_STATE=1" "MOBILEGL_IPC_STRICT_ERRORS=1"
        "MOBILEGL_IPC_ADOPT_TIER=2" "MGITEST_PERSISTENT_MAP_ARM=emulated"
        "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})  # the ICD pin, see the Fm lane above
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
    mgl_itest_magma_arm_shape(${mglItestArm} buffers)
    mgl_itest_join_environment(MGL_ITEST_MAGMA_BUFFER_ENVIRONMENT
        ${MGL_ITEST_MAGMA_BUFFER_FLAVOUR_ENV} ${mglItestArmTransportEnv})
    foreach(bufferCase
            F1WireScenario.EnabledVertexBufferDrawKeepsNamedP7Fatal
            F1WireScenario.UniformBufferRangeAndRebindPixels
            F1WireScenario.ShortUniformBufferRangePadsMissingBytes
            F1WireScenario.SubWordUniformBufferRangePadsMissingBytes
            F1WireScenario.UnalignedAtomicCounterRangeDeclinesAndTheSessionLives
            F1WireScenario.ComputeWrittenVertexAndIndexBuffersDrawAndReadBack
            F1WireScenario.ColorBlitToDefaultIgnoresViewportAndPreservesOrientation
            F1WireScenario.ColorBlitToDefaultHonorsScissorAndReversedRect
            IndexedDrawFamilyScenario.AnElementBufferDrawElementsOffsetSelectsTheRightQuad
            IndexedDrawFamilyScenario.DrawElementsBaseVertexMovesTheSameIndicesToTheOtherQuad
            IndexedDrawFamilyScenario.DrawRangeElementsDrawsTheRangedRun
            IndexedDrawFamilyScenario.DrawElementsInstancedBaseVertexPaintsOneQuadPerInstance
            IndexedDrawFamilyScenario.MultiDrawElementsBaseVertexPaintsEverySubDraw
            IndexedDrawFamilyScenario.MultiDrawElementsIndirectDrawsFromTheIndirectBuffer
            CrossFrameBufferScenario.VertexBufferSubData
            CrossFrameBufferScenario.IndexBufferSubData
            CrossFrameBufferScenario.VertexOrphanAndReupload
            CrossFrameBufferScenario.IndexOrphanAndReupload
            PersistentCoherentMapScenario.TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw
            PersistentCoherentMapScenario.AWriteAfterAFrameBoundaryReachesTheNextFramesDraw
            StorageBufferRegrowScenario.AGrownStoreIsVisibleThroughItsExistingIndexedBinding
            # P11 A2: the >= 16 MiB arena, the flavour's MGITEST_PERSISTENT_MAP_ARM=emulated
            # asserted by its tier case (docs/archive/contracts/CONTRACT-P11.md §2).
            LargeArenaAdoptionScenario.SubDataAfterAnInFlightDrawReachesTheNextDraw
            LargeArenaAdoptionScenario.RespecifiedVertexArenaKeepsVaoBindings
            LargeArenaAdoptionScenario.RespecifiedIndexArenaKeepsVaoBinding
            LargeArenaAdoptionScenario.ReadbackSeesTheLatestCpuWrite
            LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack
            LargeArenaAdoptionScenario.TheArenaLandsInTheTierItsLaneDeclares)
        mgl_itest_lane("DirectVulkan.${mglItestArm}.Buffers." "${bufferCase}"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_BUFFER_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-buffers${mglItestArmLogTag}-${bufferCase}.log")
    endforeach()
    endforeach()
endif()

# Real Magma run-ahead, including client lead and present-credit backpressure.
# The test fixture requires an actually armed production session; a missing cap
# fails rather than turning this dedicated lane into a skip or a lockstep pass.
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_sources(MobileGLIntegrationTest PRIVATE Scenarios/MagmaRunAheadScenario.cpp)
    # P7 package L: THE INPROC ARM ALONE, AND IT IS A PROPERTY OF THE SCENARIO, NOT A GAP.
    # MagmaRunAheadScenario::HoldNextBatch arms the hold through
    # ServerLoopInstance().SetBeforeRetireHookForTesting (Harness/SplitRuntimePeek.cpp:43-50) -
    # a hook on THIS PROCESS's server loop. Under spawn or tcp the server loop is in another
    # process, so the hook is installed on a loop that never runs and the wait times out with
    # "the clear waited for apply instead of returning while its server batch was held".
    # MEASURED, not assumed: registering all three arms made all 14 entries red on both new
    # arms, with that sentence and no Fatal in either role log. An entry that cannot pass for a
    # reason that is not the subject of its lane is the mirror image of R-16's rule, and this
    # file already keeps four DirectGLES lanes (DualBlock, StrictProgramArm, PersistentMapArm,
    # StrictArming) on one arm for exactly this reason. The two-process reading of run-ahead
    # needs a cross-process hold - a server-side knob, not a function pointer - and that is
    # named in this package's report as a debt rather than left as a red nobody can act on.
    foreach(credit 1 3)
      foreach(mglItestArm IN ITEMS Split)
        mgl_itest_magma_arm_shape(${mglItestArm} runahead)
        mgl_itest_join_environment(MGL_ITEST_MAGMA_RUNAHEAD_ENVIRONMENT
            "MOBILEGL_BACKEND_TYPE=DirectVulkan"
            "MOBILEGL_IPC_ROLE_SPLIT_STATE=1" "MOBILEGL_IPC_STRICT_ERRORS=1"
            "MOBILEGL_IPC_RUN_AHEAD=1" "MOBILEGL_IPC_PRESENT_CREDIT=${credit}"
            "MOBILEGL_IPC_SPIN_US=0" "MOBILEGL_IPC_ADOPT_TIER=2"
            "MGITEST_MAGMA_RUNAHEAD_LANE=1" "MGITEST_SPLIT_LANE=1"
            "MOBILEGL_ITEST_REQUIRE_GPU=1"
            ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV}  # the ICD pin, see the Fm lane
            ${mglItestArmTransportEnv})
        foreach(runAheadCase
                QueuedBufferVersionsAndDeletedNameReuseKeepTheirPixels
                QueuedProgramRebindsKeepEachUniformSnapshot
                QueuedGpuWritesReadBackTheLastDispatchBytes
                MixedNativeAndConvertedGpuVertexStreamsKeepTheirReservation
                TextureUploadCannotOvertakeThePreviouslyQueuedDraw
                GpuClearSurvivesTheFirstStorageImageUpgrade
                PresentCreditActuallyParksTheClientAtItsConfiguredLimit)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.RunAhead.Credit${credit}." "MagmaRunAheadScenario.${runAheadCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_RUNAHEAD_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-runahead${mglItestArmLogTag}-credit${credit}-${runAheadCase}.log")
        endforeach()
      endforeach()
    endforeach()
endif()

# Magma wire caches: exact view/attachment identity across public GL mutations.
# Split-only source preserves pull discovery; generic split-build registrations
# retain these names but skip outside this dedicated, strict inproc environment.
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_sources(MobileGLIntegrationTest PRIVATE Scenarios/MagmaWireCacheScenario.cpp)
    set(MGL_ITEST_MAGMA_CACHES_FLAVOUR_ENV
        "MOBILEGL_BACKEND_TYPE=DirectVulkan"
        "MOBILEGL_IPC_ROLE_SPLIT_STATE=1" "MOBILEGL_IPC_STRICT_ERRORS=1"
        "MOBILEGL_IPC_RUN_AHEAD=1" "MOBILEGL_IPC_PRESENT_CREDIT=3"
        "MOBILEGL_IPC_SPIN_US=0" "MOBILEGL_IPC_ADOPT_TIER=2"
        "MGITEST_MAGMA_CACHES_LANE=1" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_ITEST_REQUIRE_GPU=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})  # the ICD pin, see the Fm lane above
    # P7 package L: THE INPROC ARM ALONE, for the run-ahead lane's reason one step more
    # explicit - MagmaWireCacheScenario::SetUp asserts `runtime.transportName == "inproc"`
    # outright (MagmaWireCacheScenario.cpp:53), because the whole lane reads the SERVER's own
    # view and attachment identity out of this process. MEASURED: all 7 entries red on both new
    # arms with `Which is: "non-monolith" ... "inproc"` and no Fatal in either role log.
    foreach(mglItestArm IN ITEMS Split)
    mgl_itest_magma_arm_shape(${mglItestArm} caches)
    mgl_itest_join_environment(MGL_ITEST_MAGMA_CACHES_ENVIRONMENT
        ${MGL_ITEST_MAGMA_CACHES_FLAVOUR_ENV} ${mglItestArmTransportEnv})
    foreach(cacheCase
            SamplerSwizzleAtoBtoAKeepsEachQueuedPixel
            SamplerRespecifyKeepsOldAndNewNativeImages
            SamplerViewsDistinguishMipLayerAndViewType
            FramebuffersAtoBtoAKeepDistinctAttachments
            FramebufferReattachmentDistinguishesMipAndLayer
            FramebufferSrgbPolicyAtoBtoAUsesDistinctFormats
            FramebufferRespecifyRebindsTheNewNativeImage)
        mgl_itest_lane("DirectVulkan.${mglItestArm}.Caches." "MagmaWireCacheScenario.${cacheCase}"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_CACHES_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-caches${mglItestArmLogTag}-${cacheCase}.log")
    endforeach()
    endforeach()
endif()

# =====================================================================================
# P7 WAVE 2 PACKAGE C: THE `vertex-layout` RETIREMENT (CONTRACT-P7 §3.2)
# =====================================================================================
#
# The source is NOT inside the `if (MOBILEGL_BUILD_DISAGGREGATED)` above, and that is the
# point of the case pair: the claim is "the wire arm now does what the MONOLITH arm does",
# so the monolith DirectVulkan arm has to run the very same two cases. The ambient
# `TEST_PREFIX "DirectVulkan."` discovery near the top of this file picks them up from the
# target sources with no entry of its own; the three transports are added below.
target_sources(MobileGLIntegrationTest PRIVATE Scenarios/MagmaVertexLayoutScenario.cpp)

if (MOBILEGL_BUILD_DISAGGREGATED)
    # The flavour block is the buffers lane's minus the persistent-map knobs, which nothing
    # here touches. MOBILEGL_BACKEND_TYPE and the ICD pin are the load-bearing pair: without
    # them the entries run DirectGLES under a DirectVulkan prefix and skip themselves in
    # SetUp, which reads as six green skips and proves nothing (measured, first cut).
    set(MGL_ITEST_MAGMA_VERTEXLAYOUT_FLAVOUR_ENV
        "MOBILEGL_BACKEND_TYPE=DirectVulkan"
        "MOBILEGL_IPC_ROLE_SPLIT_STATE=1" "MOBILEGL_IPC_STRICT_ERRORS=1"
        "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
    # ALL THREE ARMS, unlike the caches and run-ahead lanes above: nothing here reads this
    # process's server loop, so there is no inproc-only property to declare. The scenario's own
    # SetUp skips DirectGLES, so a DirectGLES registration would only add three skips.
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
    mgl_itest_magma_arm_shape(${mglItestArm} vertexlayout)
    mgl_itest_join_environment(MGL_ITEST_MAGMA_VERTEXLAYOUT_ENVIRONMENT
        ${MGL_ITEST_MAGMA_VERTEXLAYOUT_FLAVOUR_ENV} ${mglItestArmTransportEnv})
    foreach(vertexLayoutCase
            AnUnmappableVertexFormatDeclinesTheDrawInsteadOfEndingTheSession
            AnUnreadUnmappableArrayDoesNotStopTheRestOfTheDraw)
        mgl_itest_lane("DirectVulkan.${mglItestArm}.VertexLayout." "MagmaVertexLayoutScenario.${vertexLayoutCase}"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_VERTEXLAYOUT_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-vertexlayout${mglItestArmLogTag}-${vertexLayoutCase}.log")
    endforeach()
    endforeach()
endif()

# =====================================================================================
# Magma wire arm: orphaned buffer stores are reclaimed inside a frame
# =====================================================================================
# A replay with one present never reaches a frame boundary, so the server must reclaim orphaned
# VkBuffers inside the frame. The cases read the server's wbuf[] gauges (PipeStats.h) off its
# `MGPipe stats:` line, so the flavour logs stats every swap; MOBILEGL_IPC_WIRE_DEFERRED_MB=8
# keeps the watermark case at 48 MiB of traffic instead of 400. Split and spawn only: both knobs
# are server-process environment (ARCHITECTURE.md §16.1).
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_sources(MobileGLIntegrationTest PRIVATE Scenarios/MagmaWireReclaimScenario.cpp)
    set(MGL_ITEST_MAGMA_RECLAIM_FLAVOUR_ENV
        "MOBILEGL_BACKEND_TYPE=DirectVulkan"
        "MOBILEGL_IPC_ROLE_SPLIT_STATE=1" "MOBILEGL_IPC_STRICT_ERRORS=1"
        "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
        "MOBILEGL_IPC_WIRE_DEFERRED_MB=8"
        "MGITEST_MAGMA_RECLAIM_LANE=1"
        "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})  # the ICD pin, see the Fm lane above
    foreach(mglItestArm IN ITEMS Split Spawn)
    mgl_itest_magma_arm_shape(${mglItestArm} reclaim)
    mgl_itest_join_environment(MGL_ITEST_MAGMA_RECLAIM_ENVIRONMENT
        ${MGL_ITEST_MAGMA_RECLAIM_FLAVOUR_ENV} ${mglItestArmTransportEnv})
    # Round 2 (ID-P7-43) adds the fourth: the reclaim's own memo hazard. M3 adds the fifth:
    # fixed-store descriptor churn in a long frame. Both use this private-log fixture and two arms.
    foreach(reclaimCase
            RespecifiesWithNoDrawBetweenKeepTheLiveStoreCountBounded
            RespecifyAndDrawEachStoreInOneFrameStaysWithinTheDeferredBudget
            ManySmallRespecifyAndDrawRoundsStayUnderTheStoreCountCeiling
            ADrawAfterTheEarlyReclaimFollowsTheNewStoreNotTheMemoizedHandle
            DescriptorSetsRewindInsideOneLongFrameAfterTheirSubmitRetires)
        mgl_itest_lane("DirectVulkan.${mglItestArm}.Reclaim." "MagmaWireReclaimScenario.${reclaimCase}"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_RECLAIM_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-reclaim${mglItestArmLogTag}-${reclaimCase}.log")
    endforeach()
    endforeach()
endif()

# P5c ct (docs/archive/contracts/CONTRACT-P5C.md §5): the two control records, split-only source for the
# same reason as f1's - the scenario reads the server's control-record counters (`ar`, `od` on the
# server role's PipeStats line), which only a build with a server role produces. One block per case
# with a LOG PATH OF ITS OWN, the F1 pattern: SplitLogPaths.PrivateAndDistinct fails a lane whose
# cases share one file, and the death case's child truncates whatever path it inherits.
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_sources(MobileGLIntegrationTest PRIVATE Scenarios/CtWireScenario.cpp)
    # P7 wave 2 package C adds the fifth: the OQ-10 capability read. It belongs in THIS list
    # rather than in a buffer lane because the thing it reads is a property of the SESSION -
    # what the server published at handshake and the client's mirror received - and this list
    # is the one that is replayed on DirectVulkan by tier 2, so one name here gives the bit a
    # reading on both backends and all three transports.
    foreach(ctCase ApplierResetCrossesAtThePrimedEdgeInSerialOrder
                   TextureDeathCrossesAndTheRecycledSlotAnswersTheNewObject
                   FramebufferDeathCrossesAndTheRecycledSlotAnswersTheNewObject
                   TheServerPublishesTheResidentSubDataCapabilityFromItsOwnTable
                   TheDirectApplierResetCallOnTheGLThreadIsRoleViolation)
        mgl_itest_register_split_arms("CtWireScenario.${ctCase}" "Ct.")
    endforeach()
endif()

# P5b named-blit cases, now green on both backends after P5f fm's server framebuffer
# and texture consumers. Keep the backend-specific label: the Magma lane has its own
# strict + dual-block hard gate, while integration-split keeps the Espryt workset.
# The separate spelling also avoids ctest's substring label match (see below).
if (MOBILEGL_BUILD_DISAGGREGATED)
    foreach(namedBlitBackend DirectGLES DirectVulkan)
        # P7 package L: the Magma half gets the three arms; the Espryt half stays inproc-only,
        # because its spawn and tcp coverage of this scenario comes from the F1. blocks the
        # registration macro already emits, and a second copy would only double the count.
        set(namedBlitArms Split)
        if (namedBlitBackend STREQUAL "DirectVulkan")
            set(namedBlitArms ${MGL_ITEST_MAGMA_ARMS})
        endif()
        # The ICD pin follows the BACKEND here, not the lane: this block registers both, and
        # handing VK_ICD_FILENAMES to the DirectGLES half would pin an ICD for a lane that
        # never opens Vulkan (see the Fm lane above for the ruling).
        if (namedBlitBackend STREQUAL "DirectVulkan")
            set(namedBlitBaseEnv ${MGL_ITEST_VULKAN_ENV})
        else()
            set(namedBlitBaseEnv ${MGL_ITEST_COMMON_ENV})
        endif()
        foreach(mglItestArm IN LISTS namedBlitArms)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(MGL_ITEST_NAMED_BLIT_ENVIRONMENT
            "MOBILEGL_BACKEND_TYPE=${namedBlitBackend}"
            "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
            ${MGL_ITEST_CAPABILITY_ENV} ${namedBlitBaseEnv} ${mglItestArmTransportEnv})
        # THE LABEL IS `integration-magma-split` AND NOT `integration-split-magma`, and that is not
        # a preference: `ctest -L` takes a REGEX, not an exact name, so `-L integration-split`
        # MATCHES a label spelled `integration-split-magma` as a substring. The obvious name would
        # have left both entries in the Espryt lane while every listing showed them relabelled - a
        # split that looks done and is not. Measured: with that spelling
        # `ctest -N -L integration-split` still listed both DirectVulkan entries.
        if (namedBlitBackend STREQUAL "DirectVulkan")
            set(MGL_ITEST_NAMED_BLIT_LABELS "${mglItestMagmaLabels}")
        else()
            set(MGL_ITEST_NAMED_BLIT_LABELS "integration-split")
        endif()
        foreach(namedBlitCase NamedBlitPreservesBindingsAndRestoresNextVerbsPixels NamedBlitDefaultEndpointPixels)
            mgl_itest_lane("${namedBlitBackend}.${mglItestArm}.NamedBlit." "F1WireScenario.${namedBlitCase}"
                LABELS "${MGL_ITEST_NAMED_BLIT_LABELS}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_NAMED_BLIT_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5b-${namedBlitBackend}${mglItestArmLogTag}-${namedBlitCase}.log")
        endforeach()
        endforeach()
    endforeach()
endif()

# =====================================================================================
# Positive control for the strict-errors run of integration-split
# =====================================================================================
# A drawing split entry that reads its own PipeStats window (TriangleScenario.cpp): `vbs` > 0
# proves the server stamped verb boundaries, so strict was armed, and `rsp` must be 0 or scale
# with `draws`; test.yml's strict split step requires it to run. It has its own log path and a
# RESOURCE_LOCK on it, because the library opens MOBILEGL_LOG_FILE_PATH with "w" and entries
# sharing a path under `ctest -j` truncate each other's logs (Harness/PipeStatsWindow.h).
if (MOBILEGL_BUILD_DISAGGREGATED)
    mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_STRICT_ARMING_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MGITEST_STRICT_ARMING_LANE=1"
        "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5e-strict-arming-DirectGLES.log"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    mgl_itest_lane("DirectGLES.Split.StrictArming." "TriangleScenario.TheServerStampedAVerbBoundaryOnThisDrawingFrame"
        LABELS "integration-gpu\;integration-split" LOCK p5e-strict-arming-DirectGLES.log ENV "${MGL_ITEST_GLES_SPLIT_STRICT_ARMING_ENVIRONMENT}")
endif()

# =====================================================================================
# Dual-block negative control (integration-dualblock-split)
# =====================================================================================
# DualBlockScenario asserts that the two roles fill distinct PipeInputs blocks. The entry leaves
# MOBILEGL_IPC_ROLE_SPLIT_STATE unpinned, since a ctest ENVIRONMENT property would override it:
# scripts/ci/dualblock_negative_control.sh runs it with the knob on (green) and off (red on its own
# assertion). Label spelling: ARCHITECTURE.md §16.1. MOBILEGL_IPC_RUN_AHEAD=0 makes every verb
# boundary fill, so the assertion is deterministic.
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_sources(MobileGLIntegrationTest PRIVATE
        Scenarios/DualBlockScenario.cpp Harness/DualBlockPeek.cpp)
    mgl_itest_join_environment(MGL_ITEST_GLES_DUALBLOCK_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc" "MOBILEGL_ITEST_REQUIRE_GPU=1"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_IPC_RUN_AHEAD=0"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
    mgl_itest_lane("DirectGLES.Split.DualBlock." "DualBlockScenario.*"
        LABELS "integration-dualblock-split" ENV "${MGL_ITEST_GLES_DUALBLOCK_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/split-logs/DirectGLES.Split.DualBlock.TheTwoRolesHaveDistinctBlocks.log")
endif()

# P5f exit: observe every one of three real drawing frames on both backend roles.
if (MOBILEGL_BUILD_DISAGGREGATED)
    foreach(backend DirectGLES DirectVulkan)
        # P7 package L: the Magma half on all three transports. `integration-p5f-rsp` itself
        # stays on the INPROC entries alone - the CI step that reads it require_greens exactly
        # the two names it has always had, and `ctest -L` would have matched a suffixed spelling.
        set(p5f_rsp_arms Split)
        set(p5f_rsp_base_env ${MGL_ITEST_COMMON_ENV})
        if (NOT backend STREQUAL "DirectGLES")
            set(p5f_rsp_arms ${MGL_ITEST_MAGMA_ARMS})
            # The ICD pin follows the backend, as in the NamedBlit block (see the Fm lane).
            set(p5f_rsp_base_env ${MGL_ITEST_VULKAN_ENV})
        endif()
        foreach(mglItestArm IN LISTS p5f_rsp_arms)
        mgl_itest_magma_arm_shape(${mglItestArm} p5f-rsp)
        if (backend STREQUAL "DirectGLES")
            set(p5f_rsp_label "integration-split\\;integration-p5f-rsp")
        elseif (mglItestArm STREQUAL "Split")
            set(p5f_rsp_label "integration-magma-split\\;integration-p5f-rsp")
        else()
            set(p5f_rsp_label "${mglItestMagmaLabels}")
        endif()
        mgl_itest_join_environment(p5f_rsp_environment
            "MOBILEGL_BACKEND_TYPE=${backend}" "MGITEST_SPLIT_LANE=1" "MOBILEGL_ITEST_REQUIRE_GPU=1"
            "MGITEST_P5F_RSP_LANE=1" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
            "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5f-rsp-${backend}${mglItestArmLogTag}.log"
            ${MGL_ITEST_CAPABILITY_ENV} ${p5f_rsp_base_env} ${mglItestArmTransportEnv})
        mgl_itest_lane("${backend}.${mglItestArm}.P5fRsp." "F1WireScenario.EachWireFrameHasZeroResidualPulls"
            LABELS "${p5f_rsp_label}" LOCK "p5f-rsp-${backend}${mglItestArmLogTag}.log" PROPERTIES ${mglItestArmProperties} ENV "${p5f_rsp_environment}")
        endforeach()
    endforeach()
endif()

# P8-A
if (MOBILEGL_BUILD_DISAGGREGATED)
    include(${CMAKE_CURRENT_SOURCE_DIR}/P8Coverage.cmake)
endif()

# P8-D: the Magma wire arm issues indirect draws natively (docs/archive/notes/p8/D.md).
# Public GL, so the source joins the binary unguarded and the ambient `DirectGLES.` /
# `DirectVulkan.` monolith discovery runs it too. Split arms: DirectGLES through the registration
# macro (which also replays it on Magma's informational tier), Magma's gated tier by hand, all
# three transports - the counter halves read only on the inproc arm (WireIndirectPeek.h) and the
# pixel halves everywhere, so there is no arm-only property to declare.
target_sources(MobileGLIntegrationTest PRIVATE Harness/WireIndirectPeek.cpp Scenarios/WireIndirectDrawScenario.cpp)
if (MOBILEGL_BUILD_DISAGGREGATED)
    mgl_itest_register_split_arms("WireIndirectDrawScenario.*" "")
    set(MGL_ITEST_MAGMA_INDIRECT_FLAVOUR_ENV
        "MOBILEGL_BACKEND_TYPE=DirectVulkan"
        "MOBILEGL_IPC_ROLE_SPLIT_STATE=1" "MOBILEGL_IPC_STRICT_ERRORS=1"
        "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm} indirect)
        mgl_itest_join_environment(MGL_ITEST_MAGMA_INDIRECT_ENVIRONMENT
            ${MGL_ITEST_MAGMA_INDIRECT_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        foreach(indirectCase
                EveryIndirectFormIsIssuedNativelyFromItsStore
                ComputeWrittenCommandsDrawWithoutAHostWait
                ComputeWrittenCountWordIsTheOneTheGpuReads
                ShaderWrittenCommandsWithoutAnApplicationBarrierAreOrdered)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.Indirect." "WireIndirectDrawScenario.${indirectCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_INDIRECT_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-indirect${mglItestArmLogTag}-${indirectCase}.log")
        endforeach()
    endforeach()
endif()

# P8-SE (docs/archive/notes/p8/SE.md): Espryt split residuals.
if (MOBILEGL_BUILD_DISAGGREGATED)
    # The default framebuffer's depth/stencil format reaches a spawn / tcp client now (the server
    # process has no frontend placeholder, and the publish used to return on it before posting),
    # so the case A left as `pending-fix` joins the three Espryt arms.
    mgl_itest_register_split_arms(
        "DepthStencilReadbackAttachmentShapeScenario.DefaultFramebufferDepthStencilFormatIsBlitCompatible" "")
    # B2's CPU mip arm x P8-E's driver-written mark: the forced-CPU knob plus E's refused-readback
    # knob at the base level's extent (16x16), both read by the SERVER - so inproc and spawn only,
    # and their own label as B's `CpuMip.` entries have (the tcp arm's server is the shared
    # TcpServer.Start fixture, and the refusal there would reach every 16x16 colour readback).
    foreach(mglItestArm IN ITEMS Split Spawn)
        string(TOUPPER "${mglItestArm}" mglItestArmUpper)
        string(TOLOWER "${mglItestArm}" mglItestArmLower)
        foreach(cpuMipCase Rgb16fDrawnBaseWithARefusedReadbackDeclinesByName
                           Rgb16fUploadedBaseWithARefusedReadbackIsFilteredFromTheStore)
            mgl_itest_lane("DirectGLES.${mglItestArm}.CpuMipRefused." "GenerateMipmapServerScenario.${cpuMipCase}"
                LABELS "integration-gpu\\;integration-cpu-mip-${mglItestArmLower}" ENV "${MGL_ITEST_GLES_${mglItestArmUpper}_ENVIRONMENT}\;MGITEST_ESPRYT_FORCE_CPU_MIPMAP=1\;MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=16x16\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p8se-cpu-mip-refused-${mglItestArmLower}-${cpuMipCase}.log")
        endforeach()
    endforeach()
    # PipeCatalogueTest's unmigrated-emulation list against the call sites in the tree (a source
    # lint, so the unit lane runs it wherever the split build's unit tests run; CI also runs it
    # beside split_coverage.py).
    add_test(NAME PipeCatalogue.UnmigratedEmulationSitesSelfTest
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/ci/unmigrated_emulation_sites.py" --self-test)
    add_test(NAME PipeCatalogue.UnmigratedEmulationSitesMatchTheList
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/ci/unmigrated_emulation_sites.py"
            "${CMAKE_SOURCE_DIR}")
    set_tests_properties(PipeCatalogue.UnmigratedEmulationSitesSelfTest
        PipeCatalogue.UnmigratedEmulationSitesMatchTheList PROPERTIES LABELS unit TIMEOUT 60)
endif()

# P8-SV: Magma split residuals (docs/archive/notes/p8/SV.md) - the 3D native mip blit's layer
# count, the 1D depth mip decline, the depth/stencil mip decline's tally row (asserted inside
# F1WireScenario) and the wire arm's native glDispatchComputeIndirect. Public GL, so the sources join
# the binary unguarded and the ambient `DirectGLES.` / `DirectVulkan.` monolith discovery runs them
# too (the Espryt entries are the control). Split arms: DirectGLES through the registration macro
# (which also replays them on Magma's informational tier), Magma's gated tier by hand:
#   `.Mip3D.` and `.IndirectDispatch.` - all three transports; the counter halves read only on the
#       inproc arm (WireIndirectPeek.h, WireDeclinePeek.h), the pixel/grid halves everywhere.
#   `.ShaderMipDepth1D.` - split + spawn: MGITEST_MAGMA_FORCE_SHADER_MIPMAP is read BY THE SERVER,
#       so a tcp copy would run the default arm (its tail is in spawn_lane_parity.py's
#       MAGMA_SERVER_ENV_KNOB_NO_TCP).
#   `.Mip3DValidation.` - inproc only: the 3D chain under VK_LAYER_KHRONOS_validation, red on any
#       vkCmdBlitImage VUID in the output. The server's Vulkan instance must be this process for its
#       stdout to be the entry's and for the case to prove the layer loaded (/proc/self/maps), so it
#       is in MAGMA_INPROC_ONLY. The lane that runs integration-magma-split in CI installs the layer.
target_sources(MobileGLIntegrationTest PRIVATE Harness/WireDeclinePeek.cpp Scenarios/WireMipShapeScenario.cpp
                                               Scenarios/WireIndirectDispatchScenario.cpp)
if (MOBILEGL_BUILD_DISAGGREGATED)
    mgl_itest_register_split_arms("WireMipShapeScenario.*" "")
    mgl_itest_register_split_arms("WireIndirectDispatchScenario.*" "")
    set(MGL_ITEST_MAGMA_SV_FLAVOUR_ENV
        "MOBILEGL_BACKEND_TYPE=DirectVulkan"
        "MOBILEGL_IPC_ROLE_SPLIT_STATE=1" "MOBILEGL_IPC_STRICT_ERRORS=1"
        "MOBILEGL_ITEST_REQUIRE_GPU=1" "MGITEST_SPLIT_LANE=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(MGL_ITEST_MAGMA_SV_ENVIRONMENT
            ${MGL_ITEST_MAGMA_SV_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        mgl_itest_lane("DirectVulkan.${mglItestArm}.Mip3D." "WireMipShapeScenario.VolumeChainIsGeneratedAlongItsDepth"
            LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_SV_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-sv-mip3d${mglItestArmLogTag}.log")
        foreach(dispatchCase
                CpuWrittenGroupCountsAreReadByTheGpuAtTheirOffsets
                ComputeWrittenGroupCountsDispatchWithoutAHostWait
                ShaderWrittenGroupCountsWithoutAnApplicationBarrierAreOrdered)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.IndirectDispatch." "WireIndirectDispatchScenario.${dispatchCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_SV_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-sv-dispatch${mglItestArmLogTag}-${dispatchCase}.log")
        endforeach()
        if (NOT mglItestArm STREQUAL "Tcp")
            mgl_itest_lane("DirectVulkan.${mglItestArm}.ShaderMipDepth1D." "WireMipShapeScenario.Depth1DChainWithoutANativeBlitDeclinesByName"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_SV_ENVIRONMENT}\;MGITEST_MAGMA_FORCE_SHADER_MIPMAP=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-sv-depth1d${mglItestArmLogTag}.log")
        endif()
    endforeach()
    mgl_itest_magma_arm_shape(Split)
    mgl_itest_join_environment(MGL_ITEST_MAGMA_SV_ENVIRONMENT
        ${MGL_ITEST_MAGMA_SV_FLAVOUR_ENV} ${mglItestArmTransportEnv})
    mgl_itest_lane("DirectVulkan.Split.Mip3DValidation." "WireMipShapeScenario.VolumeChainIsGeneratedAlongItsDepth"
        LABELS "${mglItestMagmaLabels}" FAIL_REGEX "VUID-vkCmdBlitImage-" ENV "${MGL_ITEST_MAGMA_SV_ENVIRONMENT}\;VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation\;MGITEST_REQUIRE_VK_VALIDATION_LAYER=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-sv-mip3d-validation.log")
endif()

# P14: texture texels uploaded BETWEEN two draws of one frame that sample the texture, with no wait in
# between (Scenarios/TextureUploadBetweenDrawsScenario.cpp): the first draw sees the old texels, the
# second the new, with and without a chain growth (re-mint + preserve) in between. Magma orders its
# upload batch behind the earlier draws with queue order and the batch's first barrier, not a CPU
# drain. Public GL: the ambient `DirectGLES.` / `DirectVulkan.` monolith discovery runs it, the GLES
# split arms through the macro, Magma's three transports by hand, and `.SyncValidation.` entries
# (inproc, where the server's Vulkan instance is this process) under the Khronos
# synchronization validator, red on any SYNC-HAZARD line whatever the device's timing. Submit-time
# validation is OFF by default in the layer, and without it a hazard across two queue submissions -
# the only kind this case can have - is never reported (measured: the src-scope red-once stayed
# green until it was set).
target_sources(MobileGLIntegrationTest PRIVATE Scenarios/TextureUploadBetweenDrawsScenario.cpp)
if (MOBILEGL_BUILD_DISAGGREGATED)
    mgl_itest_register_split_arms("TextureUploadBetweenDrawsScenario.*" "")
    # One entry PER CASE: every entry needs its own MOBILEGL_LOG_FILE_PATH (SplitLogPaths).
    set(MGL_ITEST_UPLOAD_BETWEEN_DRAWS_CASES
        EachDrawSeesTheTexelsUploadedBeforeIt
        AChainGrowthBetweenTheDrawsPreservesWhatTheFirstOneSampled)
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        mgl_itest_join_environment(MGL_ITEST_MAGMA_UPLOAD_ENVIRONMENT
            ${MGL_ITEST_MAGMA_SV_FLAVOUR_ENV} ${mglItestArmTransportEnv})
        foreach(uploadCase IN LISTS MGL_ITEST_UPLOAD_BETWEEN_DRAWS_CASES)
            mgl_itest_lane("DirectVulkan.${mglItestArm}.UploadBetweenDraws." "TextureUploadBetweenDrawsScenario.${uploadCase}"
                LABELS "${mglItestMagmaLabels}" PROPERTIES ${mglItestArmProperties} ENV "${MGL_ITEST_MAGMA_UPLOAD_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-upload-between-draws${mglItestArmLogTag}-${uploadCase}.log")
        endforeach()
    endforeach()
    # The synchronization-validation entries: inproc, where the server's Vulkan instance is this
    # process (the monolith arm runs the same record-arm upload path and was checked by hand, P14).
    mgl_itest_magma_arm_shape(Split)
    mgl_itest_join_environment(MGL_ITEST_MAGMA_UPLOAD_ENVIRONMENT
        ${MGL_ITEST_MAGMA_SV_FLAVOUR_ENV} ${mglItestArmTransportEnv})
    set(MGL_ITEST_SYNC_VALIDATION_ENV
        "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        "VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION=true"
        "VK_KHRONOS_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC=true"
        "MGITEST_REQUIRE_VK_VALIDATION_LAYER=1")
    mgl_itest_join_environment(MGL_ITEST_SYNC_VALIDATION_ENVIRONMENT ${MGL_ITEST_SYNC_VALIDATION_ENV})
    foreach(uploadCase IN LISTS MGL_ITEST_UPLOAD_BETWEEN_DRAWS_CASES)
        mgl_itest_lane("DirectVulkan.Split.SyncValidation." "TextureUploadBetweenDrawsScenario.${uploadCase}"
            LABELS "${mglItestMagmaLabels}" FAIL_REGEX "SYNC-HAZARD" ENV "${MGL_ITEST_MAGMA_UPLOAD_ENVIRONMENT}\;${MGL_ITEST_SYNC_VALIDATION_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-upload-between-draws-syncval-${uploadCase}.log")
    endforeach()
endif()

# =====================================================================================
# Informational Magma census: every split-arm scenario on DirectVulkan
# =====================================================================================
# Replays every mgl_itest_register_split_arms call on DirectVulkan under
# integration-magma-all-{split,spawn,tcp}. No CI step gates on these labels; the gated Magma lanes
# are the hand-laid integration-magma-{split,spawn,tcp} ones (ARCHITECTURE.md §16.1). Replaying
# the recorded calls keeps this set in step with the Espryt split arms. A case red on spawn and
# tcp but green on inproc is a two-process defect; red on all three, a Magma one.
if (MOBILEGL_BUILD_DISAGGREGATED)
    list(LENGTH MGL_SPLIT_ARM_CALLS mglItestSplitArmCallCount)
    message(STATUS "Integration tests: P7 package L replays ${mglItestSplitArmCallCount} split-arm "
                   "registration(s) on DirectVulkan under the informational "
                   "integration-magma-all-{split,spawn,tcp} labels")
    foreach(mglItestCall IN LISTS MGL_SPLIT_ARM_CALLS)
        # The recorded form is "<filter>|<tail>", with "-" standing for an empty tail. Split by
        # regex rather than by list(GET): a filter is allowed to contain anything but `|`, and a
        # CMake list cannot represent a trailing empty element unambiguously.
        string(REGEX REPLACE "\\|[^|]*$" "" mglItestReplayFilter "${mglItestCall}")
        string(REGEX REPLACE "^.*\\|" "" mglItestReplayTail "${mglItestCall}")
        if(mglItestReplayTail STREQUAL "-")
            set(mglItestReplayTail "")
        endif()
        mgl_itest_register_split_arms_for_backend(DirectVulkan "${mglItestReplayFilter}"
                                                  "${mglItestReplayTail}")
    endforeach()
endif()

# =====================================================================================
# Whole integration binary on Magma split (informational, no CI gate)
# =====================================================================================
# The set the monolith `DirectVulkan.` arm runs (all but MGL_ITEST_MONOLITH_MECHANISM_FILTER,
# oracles that must not inherit a transport, and MGL_ITEST_SPLIT_ARM_ONLY_FILTER), minus
# DualBlockScenario (its own lane). No MGITEST_SPLIT_LANE: query-only cases emit no record
# (ARCHITECTURE.md §16.1). Private log paths
# come through MGL_MAGMA_FULL_TEST_LISTS, not MGL_SPLIT_ARM_TEST_LISTS, whose entries also get
# lane markers that would arm unrelated oracles here; SplitLogPaths.cmake.in walks both.
if (MOBILEGL_BUILD_DISAGGREGATED)
    set(MGL_MAGMA_FULL_TEST_LISTS "")
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        string(TOUPPER "${mglItestArm}" mglItestArmUpper)
        mgl_itest_join_environment(mglItestMagmaFullEnvironment
            "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ITEST_REQUIRE_GPU=1"
            ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV} ${mglItestArmTransportEnv})
        mgl_itest_lane("DirectVulkan.${mglItestArm}.Full." "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:DualBlockScenario.*:${MGL_ITEST_SPLIT_ARM_ONLY_FILTER}"
            LABELS "integration-magma-full-${mglItestArmLower}" TEST_LIST "MGL_MAGMA_FULL_TESTS_${mglItestArmUpper}" PROPERTIES ${mglItestArmProperties} ENV "${mglItestMagmaFullEnvironment}")
        list(APPEND MGL_MAGMA_FULL_TEST_LISTS "MGL_MAGMA_FULL_TESTS_${mglItestArmUpper}")
    endforeach()
    message(STATUS "Integration tests: P7 package L tier 3 - the whole integration binary on "
                   "DirectVulkan x {inproc, spawn, tcp} under the informational "
                   "integration-magma-full-{split,spawn,tcp} labels (no CI gate)")
endif()

# =====================================================================================
# integration-verify-split: the verify comparator on the split (inproc) arm
# =====================================================================================
# Both backends run the monolith verify lane's cases minus MGL_ITEST_MONOLITH_MECHANISM_FILTER
# (arming entry and negative controls are re-registered below), DualBlockScenario (verify turns
# the dual-block rehearsal off) and, on DirectGLES, four Magma decline cases. No MGITEST_SPLIT_LANE
# (ARCHITECTURE.md §16.1): each entry has a private log (MGL_VERIFY_SPLIT_TEST_LISTS), in which
# test.yml's integration-verify-split job checks the inproc transport and the comparator arming.
if (MOBILEGL_BUILD_DISAGGREGATED AND MOBILEGL_PIPE_VERIFY)
    # The transport half, joined onto each backend's own list. One place says what "the verify
    # split arm" means, as MGL_ITEST_SPLIT_ARM_TRANSPORT_ENV does for the Magma lanes.
    set(MGL_ITEST_VERIFY_SPLIT_ARM_ENV
        "MOBILEGL_TRANSPORT=inproc" "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}"
        "MOBILEGL_PIPE_VERIFY=1" "MOBILEGL_ITEST_REQUIRE_GPU=1")
    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_SPLIT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" ${MGL_ITEST_VERIFY_SPLIT_ARM_ENV}
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_SPLIT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" ${MGL_ITEST_VERIFY_SPLIT_ARM_ENV}
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

    # The four Magma wire-arm decline cases are left out of the DirectGLES half. They assert
    # Magma's declines (depth-stencil mip, two multisample blits, overlapping in-place copy),
    # which Espryt does not make, and they skip by backend name in every DirectGLES lane
    # (F1WireScenario.cpp, "Magma wire-arm decline"); the subtraction is redundant and kept so the
    # lane's counts do not move. The DirectVulkan half runs them.
    set(MGL_ITEST_VERIFY_SPLIT_GLES_MAGMA_DECLINE_FILTER
        "F1WireScenario.GenerateMipmapDepthStencilDeclinesAndKeepsTheSession:F1WireScenario.MultisampleBlitOntoAMultisampleDestinationDeclinesAndKeepsTheSession:F1WireScenario.MultisampleBlitFromASingleSampleSourceDeclinesAndKeepsTheSession:F1WireScenario.CopyImageInPlaceOverlapDeclinesAndLeavesTheLevelAlone")

    set(MGL_VERIFY_SPLIT_TEST_LISTS "")
    # The ambient set, per backend. The GLES half is registered TWICE for the monolith verify
    # lane's reason and with its spelling: the texture-view cases need
    # MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 or DirectGLES withholds GL_ARB_texture_view and they
    # green-skip, so they are subtracted from the first registration and re-registered with the
    # knob. "Every existing verify case on the split arm" is false without the second block.
    mgl_itest_lane("DirectGLES.VerifySplit." "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:${MGL_ITEST_GLES_VIEW_FILTER}:DualBlockScenario.*:${MGL_ITEST_VERIFY_SPLIT_GLES_MAGMA_DECLINE_FILTER}:${MGL_ITEST_SPLIT_ARM_ONLY_FILTER}"
        LABELS "integration-verify-split" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} TEST_LIST MGL_VERIFY_SPLIT_TESTS_GLES ENV "${MGL_ITEST_GLES_VERIFY_SPLIT_ENVIRONMENT}")
    mgl_itest_lane("DirectGLES.VerifySplit." "${MGL_ITEST_GLES_VIEW_FILTER}"
        LABELS "integration-verify-split" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} TEST_LIST MGL_VERIFY_SPLIT_TESTS_GLES_VIEW ENV "${MGL_ITEST_GLES_VERIFY_SPLIT_ENVIRONMENT}\;MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1")
    mgl_itest_lane("DirectVulkan.VerifySplit." "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:DualBlockScenario.*:${MGL_ITEST_SPLIT_ARM_ONLY_FILTER}"
        LABELS "integration-verify-split" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} TEST_LIST MGL_VERIFY_SPLIT_TESTS_VULKAN ENV "${MGL_ITEST_VULKAN_VERIFY_SPLIT_ENVIRONMENT}")
    list(APPEND MGL_VERIFY_SPLIT_TEST_LISTS MGL_VERIFY_SPLIT_TESTS_GLES
         MGL_VERIFY_SPLIT_TESTS_GLES_VIEW MGL_VERIFY_SPLIT_TESTS_VULKAN)

    # The arming assertion on this arm, one entry per backend, with a log path nothing else
    # writes to - the monolith VerifyArming. entries' shape and their reason (the case reads the
    # whole file, and a path shared with 500 neighbours races them under `-j 4`). This is the
    # entry that cannot be green against a library with no comparator compiled in, and unlike the
    # monolith pair it also cannot be green against one whose comparator never survived the
    # server's bring-up - which is precisely what the relaxed respecify pin unblocked.
    #
    # MGITEST_PIPE_ARMING_LANE=1 is a harness marker the library never reads; it is what makes
    # the case skip in an ambient lane rather than race there.
    foreach(mglItestVerifyBackend DirectGLES DirectVulkan)
        if (mglItestVerifyBackend STREQUAL "DirectGLES")
            set(mglItestVerifyArmEnv "${MGL_ITEST_GLES_VERIFY_SPLIT_ENVIRONMENT}")
        else()
            set(mglItestVerifyArmEnv "${MGL_ITEST_VULKAN_VERIFY_SPLIT_ENVIRONMENT}")
        endif()
        mgl_itest_lane("${mglItestVerifyBackend}.VerifySplitArming." "PipeVerifyArmingScenario.Armed"
            LABELS "integration-verify-split" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} LOCK "pipe-verify-split-arming-${mglItestVerifyBackend}.log" ENV "${mglItestVerifyArmEnv}\;MGITEST_PIPE_ARMING_LANE=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-split-arming-${mglItestVerifyBackend}.log")

        # THE TWO NEGATIVE CONTROLS ON THIS ARM (CONTRACT-P7 §6), the monolith verify block's
        # four entries with the split environment underneath and nothing else changed: each is
        # green when its control REPORTS, so each goes red the day the comparator or the poison
        # stops reaching the split arm. test.yml's integration-verify-split job requires each of
        # them to pass by name.
        #
        # A (G4): the corruption perturbs the snapshot arm at the CLIENT's entry compare, so the
        # report lands in the client role's log, which is the half LibraryLog() reads.
        mgl_itest_lane("${mglItestVerifyBackend}.VerifySplitCorrupted." "PipeVerifyArmingScenario.CorruptedFieldIsReported"
            LABELS "integration-verify-split" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} LOCK "pipe-verify-split-corrupt-${mglItestVerifyBackend}.log" ENV "${mglItestVerifyArmEnv}\;MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters\;MOBILEGL_PIPE_VERIFY_FATAL=0\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-split-corrupt-${mglItestVerifyBackend}.log")
        # A ON THE OTHER ARM (P7 wave 3, V1). Control A above perturbs EntryCompare's snapshot,
        # which runs on the CLIENT thread at a verb boundary and reports `where=entry`; the
        # compare-at-read hook, which runs on the SERVER's apply thread at every backend read and
        # is the only comparator that arm ever runs, had no control at all until this entry. The
        # knob names GetPixelStoreParameters because ReadPixels is where the hook does its one
        # piece of arm-specific reasoning - the ID-49 neutral pack window, whose oracle it rewrites
        # - so a control aimed anywhere else would leave exactly that branch unproven.
        #
        # FATAL=0 for the corrupt entry's reason and one more: this field IS in kReadback's fill
        # mask (FillPoints.def), so the client's entry compare fires first and with FATAL at its
        # default the process would die before the server reached the read. The case reads the
        # SERVER role's half of its private log and nothing else.
        mgl_itest_lane("${mglItestVerifyBackend}.VerifySplitReadCorrupted." "PipeVerifyArmingScenario.CorruptedFieldIsReportedOnTheServerRead"
            LABELS "integration-verify-split" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} LOCK "pipe-verify-split-read-corrupt-${mglItestVerifyBackend}.log" ENV "${mglItestVerifyArmEnv}\;MOBILEGL_PIPE_VERIFY_CORRUPT=GetPixelStoreParameters\;MOBILEGL_PIPE_VERIFY_FATAL=0\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-split-read-corrupt-${mglItestVerifyBackend}.log")
        # B (G5): the child brings up its own inproc session from the inherited environment, and
        # PoisonOmissionScenario's ReadChildLog reads BOTH role halves of the child's log, because
        # under a transport the poisoned read can fire on either side of the seam.
        #
        # THE MONOLITH ENTRIES' PAIR. ReadPixels:GetPixelStoreParameters is read on the server under
        # the server's stamp, which is the stamp the omission has to withhold on this arm
        # (PipeInputs.cpp's MGPipeServerSetPoisonOmission). GenerateMipmap:GetActiveTextureUnit is
        # inert here: generate_mipmap resolves its texture from the verb's handle and never reads
        # the active unit, so the child exited 0 with that stamp withheld (measured, both backends).
        mgl_itest_lane("${mglItestVerifyBackend}.VerifySplitPoisonOmitted." "PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb"
            LABELS "integration-verify-split" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} LOCK "pipe-poison-omit-split-${mglItestVerifyBackend}.log" ENV "${mglItestVerifyArmEnv}\;MGITEST_POISON_OMIT=ReadPixels:GetPixelStoreParameters\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-poison-omit-split-${mglItestVerifyBackend}.log")
        # B's AMBIENT HALF. `Verify.PoisonOmissionScenario.WithoutOmissionCompletes` runs the
        # scenario with nothing withheld, and the mechanism filter took it out of the ambient set
        # above along with the rest of the scenario. Registered back by hand, with no knob, so
        # this arm's ambient lane still runs it.
        mgl_itest_lane("${mglItestVerifyBackend}.VerifySplit." "PoisonOmissionScenario.WithoutOmissionCompletes"
            LABELS "integration-verify-split" TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT} TEST_LIST "MGL_VERIFY_SPLIT_TESTS_POISON_${mglItestVerifyBackend}" ENV "${mglItestVerifyArmEnv}")
        list(APPEND MGL_VERIFY_SPLIT_TEST_LISTS "MGL_VERIFY_SPLIT_TESTS_POISON_${mglItestVerifyBackend}")
    endforeach()

    # Not on spawn: the comparator compares the applier-written gPipeInputs with the frontend
    # GLContext, which only inproc puts in one process. Under spawn the client's entry compare
    # fails at the first verb (the applier fills the other process's block), and the server's
    # MGPipeVerifyReadHook returns early: the server runs MG_Backend::InitServerRoleForSpawn(),
    # never MG_State::Init(), so it has no GLContext. Spawn needs a cross-process compare, not a
    # link change.

    message(STATUS "Integration tests: P7 wave 3 - the verify comparator on the split (inproc) arm, "
                   "both backends, under integration-verify-split")
endif()

if (MOBILEGL_BUILD_DISAGGREGATED)
    # LAST, so every mgl_itest_register_split_arms call above has already contributed its list
    # name. Apply per-entry paths after GoogleTest discovery has populated the TEST_LISTs.
    configure_file(Harness/SplitLogPaths.cmake.in SplitLogPaths.cmake @ONLY)
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/SplitLogPaths.cmake")

    add_test(NAME SplitLogPaths.ResultAccountingControls
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Harness/test_split_log_paths.py)
    set_tests_properties(SplitLogPaths.ResultAccountingControls PROPERTIES LABELS "unit")

    # The split fixture's transport-claim rule on canned inputs, so the rule itself can go red
    # off a GPU: no GL, no library, just the pure function the fixture calls.
    add_executable(SplitLaneHarnessTest Harness/SplitLaneHarnessTest.cpp)
    target_link_libraries(SplitLaneHarnessTest PRIVATE GTest::gtest_main)
    add_test(NAME SplitLaneHarness.TransportClaimRule COMMAND SplitLaneHarnessTest)
    set_tests_properties(SplitLaneHarness.TransportClaimRule PROPERTIES LABELS "unit")
endif()
