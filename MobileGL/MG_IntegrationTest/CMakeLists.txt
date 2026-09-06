cmake_minimum_required(VERSION 3.24)

# MobileGL headless GPU integration tests.
#
# These are not unit tests: each scenario brings up a real EGL context on a
# pbuffer, renders real frames through a real backend and asserts on
# glReadPixels output. They need a GPU, so the module is OFF by default
# (MOBILEGL_BUILD_INTEGRATION_TEST) and every scenario skips cleanly - never
# fails, never hangs - on a machine without one. "Cleanly" is not a hope: the
# harness runs the whole bring-up in a forked child first, because MobileGL
# ABORTS rather than returning an error on an unusable platform (HeadlessGL.cpp).
#
# A clean skip is also indistinguishable from a pass, so set
# MOBILEGL_ITEST_REQUIRE_GPU wherever the machine is supposed to have a GPU.
#
# Backend selection is latched at initialization from MOBILEGL_BACKEND_TYPE, so
# one process is one backend: the same binary is registered twice, once per
# backend, under the `integration-gpu` label.

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
    Scenarios/OrientationScenario.cpp
    Scenarios/CrossFrameBufferScenario.cpp
    Scenarios/ResidentIndexScenario.cpp
    Scenarios/MultiDrawScenario.cpp
    Scenarios/DrawParametersScenario.cpp
    Scenarios/AsyncCompileScenario.cpp
    Scenarios/XfbAfterClipDistanceScenario.cpp
    Scenarios/UnwrittenPositionOutputScenario.cpp
    Scenarios/SampleMaskScopeScenario.cpp
    Scenarios/SampledSetStalenessScenario.cpp
    Scenarios/ThreeChannelAttachmentScenario.cpp
    Scenarios/SnormAttachmentScenario.cpp
    Scenarios/PipelineFailureScenario.cpp
    Scenarios/AdvertisedLimitsScenario.cpp
    Scenarios/PixelStoreSweepScenario.cpp
    Scenarios/PrimitiveRestartScenario.cpp
    Scenarios/FragCoordOriginScenario.cpp
    Scenarios/ClearThenReadPixelsScenario.cpp
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
    Scenarios/NonCoreImageFormatScenario.cpp
    Scenarios/ImageSizeAfterRespecScenario.cpp
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
    Scenarios/TextureViewScenario.cpp
    Scenarios/PackedWordReadbackScenario.cpp
    Scenarios/LayeredAttachmentBarrierScenario.cpp
    Scenarios/LayeredAttachmentShapeScenario.cpp
    Scenarios/LayeredTextureReadbackScenario.cpp
    Scenarios/AtomicCounterScenario.cpp
    Scenarios/LargeArenaAdoptionScenario.cpp
    Scenarios/SsboArrayDynamicIndexScenario.cpp
    Scenarios/StorageBufferRegrowScenario.cpp
    Scenarios/SpirvShaderBinaryScenario.cpp
    Scenarios/RelinkStageSetScenario.cpp
    Scenarios/GuiBatchScenario.cpp
    Scenarios/UnboundImageDescriptorScenario.cpp
    Scenarios/IntegerBorderColorScenario.cpp
    Scenarios/ClearTexImageUndefinedLevelZeroScenario.cpp
    Scenarios/RenderbufferBlendFormatScenario.cpp
    Scenarios/DualSourceBlendScenario.cpp
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
# usually Mesa/llvmpipe - a software rasteriser silently replacing the GPU under
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
        "Mesa/llvmpipe - the scenarios would then go green against a software rasteriser instead of the GPU. "
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
            "MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH=1"
            "MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS=1"
            "MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER=1")
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

mgl_itest_join_environment(MGL_ITEST_GLES_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_ASYNC_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ASYNC_SHADER_COMPILE=1" ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_FORCED_DS_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1" ${MGL_ITEST_COMMON_ENV})

# The shader-compiler configurations AsyncCompileScenario needs, and the one
# ViewportArrayScenario's negative control needs.
#
# These used to be poked into MG_Config::Features from inside the test bodies. They
# cannot be any more - on Android this module links the SHIPPING libMobileGL.so, which
# exports nothing internal - and they should not have been anyway: half of what each of
# them decides is latched before the first GL call (the compile pool and its threads;
# the advertised extension list, which a backend builds once from the configuration in
# force at its first use), so an in-process write could only ever have moved the other
# half. Every one of them is a whole-process property, and a whole-process property is
# spelled with an environment variable and a ctest entry of its own.
#
# Note the shape of every list here: it APPENDS to MGL_ITEST_COMMON_ENV /
# MGL_ITEST_VULKAN_ENV rather than standing alone. A ctest ENVIRONMENT property REPLACES
# the job environment rather than adding to it, so an entry that lists only its mode
# variable would silently lose the EGL vendor and Vulkan ICD pinning and run against
# whatever the loader found first.
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
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES."
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_ENVIRONMENT}"
)

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan."
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_ENVIRONMENT}"
)

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
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.AsyncCompile."
    TEST_FILTER "XfbAfterClipDistanceScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_ASYNC_ENVIRONMENT}"
)

# A fourth registration, of the depth/stencil readback scenarios, with the ES
# shader-sampling emulation forced on. Not paranoia - without it these scenarios are
# UNFALSIFIABLE on the machines this suite runs on: OpenGL ES has no depth or stencil
# readback in core, but Mesa accepts the reads anyway, so on llvmpipe every one of them
# goes green through a native path that the Adreno device does not have. Deleting the
# entire emulation left all of them passing. With the flag the native spellings are off
# the table and only the path the device actually takes remains. DirectGLES only - the
# emulation is DirectGLES's.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ForcedDepthStencilEmulation."
    TEST_FILTER "DepthStencilReadback*Scenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_FORCED_DS_ENVIRONMENT}"
)

# UnlocatedIoBlockScenario with the interface-block location strip PINNED ON, for the same
# reason the depth/stencil entry above pins its emulation: without it this scenario is
# UNFALSIFIABLE on the machines this suite runs on. llvmpipe carries a located interface block
# correctly, so the driver POST that arms the strip on Mali answers "healthy" here and the
# emulation never runs - the ambient registration would be exercising the un-stripped path
# twice and calling it coverage. With the variable set, the blocks really are emitted with no
# location and the assertion is about the spelling the device gets.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.UnlocatedIoBlocks."
    TEST_FILTER "UnlocatedIoBlockScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_UNLOCATED_IO_BLOCKS_ENVIRONMENT}"
)

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
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.AsyncOn."
    TEST_FILTER "AsyncCompileScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_ASYNC_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.AsyncOn."
    TEST_FILTER "AsyncCompileScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_ASYNC_ON_ENVIRONMENT}"
)

# The other side of the same switch: asynchronous compilation OFF, so
# GL_KHR_parallel_shader_compile must be WITHDRAWN from both spellings of the extension
# list and GL_MAX_SHADER_COMPILER_THREADS_KHR must read 0. Only that one case is
# registered here because it is the only one that has anything to say in this
# configuration - the other four exist to observe worker-built artifacts, and there are
# none - so registering the whole scenario would buy four guaranteed skips per backend.
# Together with the AsyncOn. entries above, one ctest run still covers both flag states,
# which is what the in-process forcing used to be for.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.AsyncOff."
    TEST_FILTER "AsyncCompileScenario.ExtensionStringMatchesTheConfiguration"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_ASYNC_OFF_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.AsyncOff."
    TEST_FILTER "AsyncCompileScenario.ExtensionStringMatchesTheConfiguration"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_ASYNC_OFF_ENVIRONMENT}"
)

# The optimistic-status quirk's end-to-end shape. Its own entries and not part of the
# AsyncOn. ones because the quirk is not neutral for the rest of the scenario: with it in
# force glGetShaderiv(GL_COMPILE_STATUS) deliberately answers without joining, which is
# exactly what CompletionStatusPollingThenForcedJoin asserts must NOT happen. Off by
# default and never advertised, so - unlike asynchronous compilation, which announces
# itself through the extension string - the variable is the only thing that can tell the
# case it is in force.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.OptimisticShaderStatus."
    TEST_FILTER "AsyncCompileScenario.IrisShapedTwoPhaseBatchRendersCorrectly"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OPTIMISTIC_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.OptimisticShaderStatus."
    TEST_FILTER "AsyncCompileScenario.IrisShapedTwoPhaseBatchRendersCorrectly"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_OPTIMISTIC_ENVIRONMENT}"
)

# The negative control for the DirectGLES gl_ViewportIndex emulation, in a process that
# has it switched off. One case, because it is the only one the switch may touch: with
# the emulation off the three positive cases in the same fixture describe behaviour the
# backend does not have, so a whole-scenario registration would be three guaranteed reds.
# DirectGLES only - the flag steers nothing on DirectVulkan, which routes natively.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.NoViewportArrayEmulation."
    TEST_FILTER "ViewportArrayScenario.WithoutTheEmulationEveryIndexCollapsesOntoViewportZero"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_NO_VIEWPORT_EMULATION_ENVIRONMENT}"
)

# PrimitivesGeneratedNoXfbScenario again, with the GL_PRIMITIVES_GENERATED statistics
# reroute PINNED ON. The ambient DirectVulkan registration runs the same cases under the
# bring-up probe's Auto verdict, so between the two entries both accounting paths answer
# the same GL questions and must produce the same numbers - the "two pools must agree"
# gate this machine can hold that the affected device cannot. The pinned entry is also
# the only one whose arming case runs: it asserts the renderer's latched MGLOG_I, so a
# silently-disarmed reroute (an inverted override mapping, a lost gate) fails here
# instead of leaving every equality case vacuously green. DirectVulkan only - the flag
# steers nothing on DirectGLES.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.PrimGenReroute."
    TEST_FILTER "PrimitivesGeneratedNoXfbScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_PRIMGEN_REROUTE_ENVIRONMENT}"
)

# The packed16 copy scenarios again, with the 8-bit storage widening PINNED ON. The ambient
# registrations above cover the narrow storage - on every CI driver the widening's POST
# probe finds no field-order mirror, so Auto keeps the native 16-bit path - which means the
# storage every AFFECTED device will actually run would otherwise execute nowhere at all:
# no CI driver has the Mali bug that arms it. This lane is what proves the widened storage
# is client-invisible (same packed words in and out on every leg the 18 failing CTS bodies
# used, the renderbuffer one included). DirectGLES only - the flag steers nothing on
# DirectVulkan, which has always stored these formats widened.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.WidenedPacked16."
    TEST_FILTER "CopyImagePacked16Scenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_WIDENED_PACKED16_ENVIRONMENT}"
)

# PointSizeDemotionScenario with the demotion PINNED ON, per backend, for the reason every
# pinned lane above exists: llvmpipe and lavapipe both HOST gl_PointSize in tessellation and
# geometry stages, so the ambient registrations run these captures through the built-in and
# the demotion - the path every affected Mali device actually takes - would execute nowhere.
# The ambient runs stay the negative control: same scenario, same CPU-computed bytes, native
# path. Both backends, because the demotion is shared phase-B work with two different
# consumers (the ESSL capture respelling vs the SPIR-V Xfb carrier binding).
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.PointSizeDemotion."
    TEST_FILTER "PointSizeDemotionScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_POINT_SIZE_DEMOTION_ENVIRONMENT}"
)

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.PointSizeDemotion."
    TEST_FILTER "PointSizeDemotionScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_POINT_SIZE_DEMOTION_ENVIRONMENT}"
)
