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
    Harness/PipeSlotPeek.cpp
    Harness/PipeApplyPeek.cpp
    Harness/P4aSeamPeek.cpp
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
    Scenarios/PipeVerifyArmingScenario.cpp
    Scenarios/PoisonOmissionScenario.cpp
    Scenarios/HandleRecycleScenario.cpp
    Scenarios/CsoContentAddressingScenario.cpp
    Scenarios/ResourceSubsystemControlScenario.cpp
    Scenarios/TextureParamsWithoutASamplerViewScenario.cpp
    Scenarios/TextureUploadShapeScenario.cpp
    Scenarios/ObjectSubsystemControlScenario.cpp
    Scenarios/P4aSeamAuditScenario.cpp
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

# --- what THIS TREE implements, answered by the build rather than by a person ----------
#
# Two P2 entries assert something that only EXISTS once another P2 package has landed:
# HandleRecycleScenario's Handles arm needs a backend keyed on {slot, gen} (packages C and D),
# its AbaControl arm needs a consumer for MOBILEGL_PIPE_HANDLE_ABA_CONTROL (package D), and
# CsoContentAddressingScenario needs the client-side tracker that mints CSOs at all (package B).
# The gates package is written and merged FIRST, against the P2 contract commit, precisely so
# that the AbaControl red is on the record before either backend is touched - so for a while
# those entries have nothing to assert.
#
# The honest report for that is a SKIP naming what is missing, never a deleted registration and
# never a green that means "the thing I test does not exist yet". What decides the skip is
# THIS block, so that nobody has to remember to remove a hand-written guard:
#
#   * two of the three answers are pure EXISTENCE checks, through file(GLOB CONFIGURE_DEPENDS).
#     Ninja re-evaluates such a glob before every build and reconfigures only when the RESULT
#     changes, so these cost nothing until the file appears - and then they arm themselves.
#   * the third has to read a file's CONTENTS, because package D re-keys inside an existing
#     source rather than adding one. VertexInputStateFactory.cpp is the one file both of D's
#     answers live in (ComputeHash's buffer key is what the re-key changes AND what the ABA
#     knob reverts), it is small, and it is watched by name - so an edit to it reconfigures and
#     an edit anywhere else in the backend does not.
#
# Every verdict is printed at configure time: a marker that silently answered "no" for a tree
# that does implement the thing would turn a real gate into a permanent skip.
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
# outcome than four entries that skip.
if (MOBILEGL_PIPE_PUSH)
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_PIPE_PUSH_BUILD=1")
endif()

# THE THREE MARKERS BELOW ANSWER A QUESTION ABOUT THE SOURCE TREE, so each is only a true
# statement about THIS LIBRARY while this build compiles the arm the source implements - and all
# three arms are `#if MOBILEGL_PIPE_PUSH`. A pull build has no {slot, gen} key (the slot tables
# and the re-keyed memos are push-only) and no Features.PipeHandleAbaControl at all (Config.h
# declares the field inside `#if MOBILEGL_PIPE_PUSH` and ConfigLoader parses it in the same arm).
# A source-only probe would therefore arm the PULL build's lanes the moment packages C and D
# land: the AbaControl lane would go hard red on a gate G2 requires green (the guards it means to
# defeat are still in force, so the scenario's "expect the stale pixels" assertion fails), and the
# Handles lane would report green against a library that contains no re-key at all - the
# "test that cannot fail" this scenario exists to avoid.
#
# So the whole block sits under the same `if (MOBILEGL_PIPE_PUSH)` as MGITEST_PIPE_PUSH_BUILD, and
# HandleRecycleScenario re-checks that marker before either arm asserts, so a hand-forced
# environment cannot arm an arm this build does not have either.
#
# ALL FOUR MARKERS ARE CONTENT PROBES, AND NONE OF THEM NAMES A FILE. A probe for a filename asks
# the wrong question: the owning package chooses its own file layout, so the moment it moves the
# code the probe answers "no" forever and the arm skips with a reason that has become false - a
# test quietly measuring nothing, which is the one outcome this whole scenario exists to prevent.
# The CSO probe was rewritten for exactly that reason once already; the magma probe still read one
# hard-coded .cpp, and package D already keeps one of its two Features.PipeHandleAbaControl
# consumers in a different file of the same directory (Renderer/VulkanRenderer.cpp), so it was one
# refactor away from a permanent AbaControl skip. So all four now ask "does any source in the
# directory the owning package owns name this symbol?", which is the thing each arm actually needs.
#
# Staleness cannot creep in from either side: the GLOB is CONFIGURE_DEPENDS (a file added or
# removed re-runs it) and every file it finds is appended to CMAKE_CONFIGURE_DEPENDS (an edit to
# one re-runs it).
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

# The same probe, over a CONJUNCTION: BOTH regexes matched ANYWHERE UNDER the directory, not
# necessarily in the same file. outVar is set to "<file matching A> + <file matching B>" when both
# were found and to the empty string otherwise.
#
# P4a needs it for exactly one question and the question cannot be asked any other way. "Does
# MOBILEGL_PIPE_HANDLE_ABA_CONTROL defeat the identity of P4a's OBJECT kinds on this backend?" is
# not answered by "some source reads Features.PipeHandleAbaControl" - MagmaPipeArms.h does, and its
# consumers are Magma's VERTEX-INPUT keys, so a single-regex probe would arm the six P4a ABA
# controls on a backend where the knob cannot reach a texture, a framebuffer, a sampler, a view or a
# program, and every one of them would assert a corruption nothing on the tree can produce - a hard
# red on an always-on integration-gpu lane. Nor is it answered by "some source names a P4a subsystem
# bit", which will become true for a backend that honours the mask long before anyone wires the
# knob. What the arm needs is BOTH FACTS TO BE TRUE OF THE BACKEND.
#
# DIRECTORY-WIDE RATHER THAN PER FILE, and that is review finding F-M5 rather than a preference.
# Requiring one file to carry both makes the arming depend on the FILE LAYOUT a later package
# chooses: a backend that wires the knob in Managers.cpp while its P4a subsystem constants live in
# SlotTables.h satisfies the question and fails the probe, the six controls keep printing wired=0
# and asserting the correct pixels, and NOTHING fails, warns or records that the expected flip did
# not happen - the one failure mode a control whose flip is in the future has. The false-positive
# this trades against is a backend that reads the knob somewhere and names a P4a bit somewhere else
# without connecting them; that costs a red lane an engineer must look at, which is the direction
# that gets noticed. Both spellings of the answer are printed, so the configure log says which file
# supplied which half.
#
# Same staleness guarantees as the single-regex probe: CONFIGURE_DEPENDS on the glob, and every file
# it finds appended to CMAKE_CONFIGURE_DEPENDS.
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

if (MOBILEGL_PIPE_PUSH)
    # DirectGLES' Track H arm, probed by the subsystem bit it is gated on rather than by
    # SlotTables.h existing: the bit is declared in the contract (MG_Pipe/MGPipe.h:77) and the
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
    # from the summary line alone. Once package B's texture emitter lands, an emitter that stopped
    # emitting would read exactly like no emitter at all and the two-sided assertion the scenario
    # exists for would pass while comparing nothing. This probe is what separates them.
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
else()
    message(STATUS "Integration tests: pull build - HandleRecycle.{Handles,AbaControl}, "
                   "CsoContentAddressing, ResourceSubsystemControl, ObjectSubsystemControl and "
                   "TextureUploadShape stay registered (G2) and SKIP: every arm they assert is "
                   "compiled only under MOBILEGL_PIPE_PUSH")
endif()

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
# RESOURCE_LOCK, on this lane and on the three below it, and it is a bug fix rather than a
# precaution. Each of these lanes gives a WHOLE SCENARIO one MOBILEGL_LOG_FILE_PATH, and one case
# in each reads that log back to prove the pinned emulation actually armed. The library opens the
# log fopen(path, "w"), so every process in the lane TRUNCATES it - and under `ctest -j` a sibling
# case of the same lane can truncate it while the arming case is reading, which reads back as "the
# log carries no arming line" and fails a healthy lane. Measured on this tree: three runs of the
# full integration-gpu label at -j 8 produced 4 failures, 0 and 2, always one of these arming
# cases, never the same set twice.
#
# The rule stated above the verify block - a case that reads the log needs a lane whose filter
# selects it alone - would fix it by re-filtering, but that would RENAME the arming entries, and
# an existing ctest name may never disappear (gate G14). A ctest RESOURCE_LOCK named after the log
# is the same guarantee without touching a name: ctest never runs two entries holding the same lock
# at once, so the only processes that can truncate a lane's log are ones nobody is reading it for.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.UnlocatedIoBlocks."
    TEST_FILTER "UnlocatedIoBlockScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        RESOURCE_LOCK unlocated-io-blocks.log
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
        RESOURCE_LOCK primgen-query-reroute.log
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
        RESOURCE_LOCK point-size-demotion-gles.log
        ENVIRONMENT "${MGL_ITEST_GLES_POINT_SIZE_DEMOTION_ENVIRONMENT}"
)

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.PointSizeDemotion."
    TEST_FILTER "PointSizeDemotionScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        RESOURCE_LOCK point-size-demotion-vulkan.log
        ENVIRONMENT "${MGL_ITEST_VULKAN_POINT_SIZE_DEMOTION_ENVIRONMENT}"
)

# --- the third CI mode: MOBILEGL_PIPE_VERIFY -----------------------------------
#
# ARCHITECTURE.md 13.2-(2) asks for a THIRD build mode next to pull and push: two state models in
# one address space, compared field by field at every verb boundary and again at every accessor
# read, 5-10x slower and never shipped. These entries are that mode's lane. They exist only when
# the library was configured with -DMOBILEGL_PIPE_VERIFY=ON, which is deliberate and is half of
# what makes the lane falsifiable: `ctest -L integration-verify --no-tests=error` in a build that
# forgot the option matches NO tests and fails, instead of reporting a green run of nothing.
#
# The other half is PipeVerifyArmingScenario.Armed, which asserts the library's own arming line -
# because MOBILEGL_PIPE_VERIFY=1 in the environment of a library that never compiled the
# comparator in is a silent no-op that looks exactly like a clean pass.
#
# Three things about the ENVIRONMENT properties below, each of which has already gone wrong once
# in this file:
#   * every list APPENDS ${MGL_ITEST_COMMON_ENV} / ${MGL_ITEST_VULKAN_ENV}. A ctest ENVIRONMENT
#     entry overrides the job environment for the names it lists, so an entry that named only its
#     own knobs would lose the EGL vendor and Vulkan ICD pinning and run against whichever driver
#     the loader found first.
#   * the ambient Verify. entries name NEITHER MOBILEGL_PIPE_VERIFY_CORRUPT NOR
#     MOBILEGL_PIPE_POISON_OMIT. That is what lets CI's two always-on negative-control steps
#     export those knobs in the JOB environment and have them reach the test processes; a
#     property entry of the same name would silently win and the controls would prove nothing.
#   * MOBILEGL_LOG_FILE_PATH is per lane, and "per lane" is the exact limit of what it proves. It
#     is the only channel a test process has for reading the library's own report (MG_Config is not
#     reachable from this module), but the log is opened fopen(path, "w"), so every process in a
#     lane TRUNCATES it: after an ambient lane of 400-odd entries the file holds the LAST process
#     and nothing else. Reading it is therefore only sound in a filtered, one-entry lane - which is
#     why the arming case has a lane and a log of its own below, and why neither this file nor CI
#     may read the ambient logs as evidence about the entries that ran before the last one. The
#     ambient path is kept for post-mortems (and to keep library chatter out of ctest's capture).
# --- G8: the handle ABA, three always-on arms -----------------------------------------
#
# ALWAYS ON, in every build mode, which is deliberate: the Legacy arm asserts today's
# lifetimeId + weak_ptr guards and is meaningful in a pull build, and `ctest -R HandleRecycle`
# has to name the same entries whichever build directory it is pointed at (P2 brief G8 runs it
# against build-verify; D.3 part 1 runs it again as part of the interface-purity gate).
#
# One lane per arm, and each lane names MGITEST_HANDLE_ARM: the arm is not a property of the
# test body, it is the (MOBILEGL_PIPE_PUSH, MOBILEGL_PIPE_LEGACY_MEMOS, MOBILEGL_PIPE_HANDLE_ABA_CONTROL)
# triple the process was launched with, and the scenario skips in the ambient entries because
# none of that is configured there.
#
# Every list APPENDS the common/Vulkan environment for the reason spelled out above the verify
# block: a ctest ENVIRONMENT property REPLACES the job environment for the names it lists, so an
# entry naming only its own knobs would lose the EGL vendor and Vulkan ICD pinning.
#
# The AbaControl arm is DirectVulkan only. The knob defeats the object-identity half of
# DirectVulkan's vertex-input memo keys (VertexInputStateFactory::ComputeHash, its per-VAO memo
# table, and LookupVaoDrawMemo); it steers nothing on DirectGLES, and a lane that configured it
# there would be a permanent skip claiming to be a control.
#
# SO THE BUFFER ABA's CORRUPTION EVIDENCE IS MAGMA-ONLY, AND THAT IS RECORDED RATHER THAN LEFT
# AS AN ABSENCE (gates m10, closed here as a statement rather than as a lane). P3a re-keys the
# BUFFER on DirectGLES - resource_* now dispatches by handle - so the obvious next move is a
# DirectGLES `.AbaControl` lane over ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents.
# It is not a registration-only change and it is therefore not made here: the knob has exactly
# one consumer in the tree (MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h's
# MagmaPipeAbaControlDefeatsIdentity, which is what the MGL_ITEST_MAGMA_ABA probe above looks
# for), so a DirectGLES lane would run with the knob inert, fail to reproduce the corruption it
# asserts, and go RED in an always-on integration-gpu lane - the exact failure mode the header
# of HandleRecycleScenario.cpp records this file already having had once.
#
# What that costs, stated so the next reader does not have to re-derive it: on DirectGLES the
# buffer case's Handles arm proves the re-key does NOT alias, and nothing proves the reproducer
# could still see an aliasing that was reintroduced there. The positive control for that is a
# Features.PipeHandleAbaControl consumer over Espryt's resource slot table - one `if` in
# GetOrCreate / FindByHandle, the way MagmaPipeClaimSlotMemos is Magma's - and it belongs with
# whoever next touches that table, not with a lane registration.
#
# It gets TWO lanes, because there are two arms and the control has to reach the one P2 SHIPS.
# `AbaControl` is D18's lane verbatim (MOBILEGL_PIPE_PUSH=0, the pre-handle arm) and defeats the
# lifetime-id/address guards; `AbaControlHandles` runs the handle arm (MOBILEGL_PIPE_LEGACY_MEMOS=0,
# the default push mask) and defeats the object identity that SELECTS THE SLOT - the key the handle
# arm ships. With only the first lane the control says nothing at all about the re-key: the handle
# arm is not executed under MOBILEGL_PIPE_PUSH=0, so every guard it would have to defeat is in
# another branch.
#
# NEITHER lane exercises the GENERATION half of {slot, gen}, and no lane of this shape can. Magma's
# mint has no death notification and returns a slot only through its age sweep (256/1024 boundaries,
# MagmaPipeArms.h), so the five frame boundaries this scenario issues always hand the replacement a
# brand-new slot at Gen 1; a real reuse needs >= 1024 idle boundaries, which puts the two draws in
# different frames - where the only pixel-visible memo declines by design. The generation is covered
# by the unit suite MG_Test/Pipe/MagmaPipeIdentityTest.cpp instead, which drives a real
# retire -> reuse; MagmaPipeAbaControlDefeatsIdentity carries the measurement.
#
# The two PUSH-ONLY knobs of those arms are set only in a push build, and the lane NAMES are
# unaffected by that (an ENVIRONMENT property is not part of a test's name, so G2 still sees the
# same list in both builds). MOBILEGL_PIPE_LEGACY_MEMOS=0 says "never enter the legacy arm"; in a
# pull build the legacy arm is the ONLY arm and every Track-H subsystem bit is clear, which is
# precisely D14's startup Fatal{PipeLegacyMemosDisabled} condition - so a lane that set it there
# would abort the process before the scenario could report its skip. MOBILEGL_PIPE_HANDLE_ABA_CONTROL
# has no field to parse into in a pull build at all (Config.h declares it under #if MOBILEGL_PIPE_PUSH).
# A lane that needs an arm pins EVERY knob that selects it. A ctest ENVIRONMENT property overrides
# only the variables it names; the rest leak in from the job. The five-part gate's all-pull control
# arm runs `MOBILEGL_PIPE_PUSH=0 ctest -L integration-gpu` over the whole label, and without the
# explicit bitmask below that leaked PUSH=0 turned this lane's LEGACY_MEMOS=0 into D14's armless
# combination: the bring-up aborted, on purpose, and the lane went red for a reason that was never
# about handles. The mask is the PHASE default, not a hand-picked bit, so the lane keeps measuring
# the shape that ships: it was kMGPipeSubsystemsMigratedAtP2 (0x7f), then
# kMGPipeSubsystemsMigratedAtP3a (0x1ff), and is now kMGPipeSubsystemsMigratedAtP4a (0x1fff,
# MG_Pipe/MGPipe.h), which adds bits 9-12 - framebuffer, texture resources, samplers and programs.
# Pinning it at 0x1ff after P4a would leave the Handles arm asserting the P3a shape while the six
# OBJECT handles it is now also about stayed switched off - a lane that still passes and no longer
# measures the key that ships. Each phase's constant survives as the NEXT phase's A/B control, which
# is what ResourceSubsystemControl's Off lane uses 0x7f for and ObjectSubsystemControl's uses 0x1ff.
if (MOBILEGL_PIPE_PUSH)
    set(MGL_ITEST_HANDLES_ARM_KNOBS "MOBILEGL_PIPE_LEGACY_MEMOS=0" "MOBILEGL_PIPE_PUSH=0x1fff")
    set(MGL_ITEST_ABA_ARM_KNOBS "MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1")
else()
    set(MGL_ITEST_HANDLES_ARM_KNOBS "")
    set(MGL_ITEST_ABA_ARM_KNOBS "")
endif()

mgl_itest_join_environment(MGL_ITEST_GLES_HANDLE_HANDLES_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_HANDLE_ARM=handles" ${MGL_ITEST_HANDLES_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_HANDLES_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=handles" ${MGL_ITEST_HANDLES_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_HANDLE_LEGACY_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_HANDLE_ARM=legacy" "MOBILEGL_PIPE_PUSH=0"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_LEGACY_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=legacy" "MOBILEGL_PIPE_PUSH=0"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_ABA_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=aba" "MOBILEGL_PIPE_PUSH=0"
    ${MGL_ITEST_ABA_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_ABA_HANDLES_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=aba"
    ${MGL_ITEST_HANDLES_ARM_KNOBS} ${MGL_ITEST_ABA_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.HandleRecycle.Handles."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_HANDLE_HANDLES_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.HandleRecycle.Handles."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_HANDLE_HANDLES_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.HandleRecycle.Legacy."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_HANDLE_LEGACY_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.HandleRecycle.Legacy."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_HANDLE_LEGACY_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.HandleRecycle.AbaControl."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_HANDLE_ABA_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.HandleRecycle.AbaControlHandles."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_HANDLE_ABA_HANDLES_ENVIRONMENT}"
)

# --- G12: the CSO content-addressing negative control ---------------------------------
#
# PUSH BUILDS ONLY, and that is the honest scope rather than a convenience: the two counters the
# control reads (CallClass::RenderStateCsoMints / RenderStateCsoBinds) and the `cso[...]` bracket
# of the summary line are both `#if MOBILEGL_PIPE_PUSH` (PipeStats.h, PipeStats.cpp), so in a pull
# build there is no channel to read and an entry here would be a permanent skip.
#
# Each arm gets a LOG PATH OF ITS OWN. The library opens its log fopen(path, "w") - every process
# in a lane truncates it - and these two cases READ that log, so a shared path would have them
# reading a neighbour's bring-up under `ctest -j 4`. Same rule as the arming lane below.
#
# MOBILEGL_PIPE_STATS_PERIOD=1 makes one summary line per eglSwapBuffers, which is what lets the
# workload be bracketed by two swaps and read back as a window covering exactly itself.
#
# Registered in EVERY build, including the pull build where there is no CSO at all, so that
# `ctest -L integration-gpu` stays name-for-name identical between pull and push (gate G2). In a
# pull build MGITEST_PIPE_PUSH_BUILD is absent and both cases skip saying so.
#
# THE MASK IS THE PHASE DEFAULT AND THE CONTROL IS BIT 63, and the two must not be confused. The
# only thing these four lanes are an A/B about is `no CSO content addressing` (bit 63,
# kMGPipeBehaviourNoCsoContentAddressing), which is what separates the On lane from the Off lane.
# Every
# other bit is the build's shipping mask, so it moves with the phase: 0x7f
# (kMGPipeSubsystemsMigratedAtP2), then 0x1ff (kMGPipeSubsystemsMigratedAtP3a), now 0x1fff
# (kMGPipeSubsystemsMigratedAtP4a) for the same reason MGL_ITEST_HANDLES_ARM_KNOBS above moved. Left
# pinned at 0x1ff after P4a these lanes would keep passing while running on a configuration nothing
# ships - bits 9-12 (framebuffer, texture resources, samplers, programs) cleared - which is the
# "still green, no longer measuring the shape that ships" failure the Handles-arm comment above
# rejects. NOTE THAT THE OFF LANE'S MASK MOVED TOO, from 0x80000000000001ff to
# 0x8000000000001fff: the mask moves with the phase, the control is bit 63, and the two must not be
# confused. The counters they read (csom / csob) are render-state and
# are steered by neither bit, so raising the mask is behaviour-preserving for what they assert;
# what it buys is that a CSO regression that only shows up with the P3a subsystems on can reach
# them. (contract-review-v1.md item 11, closed here for all three lane families.)
mgl_itest_join_environment(MGL_ITEST_GLES_CSO_ON_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_CSO_LANE=content-addressed"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-content-addressed-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_CSO_OFF_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_CSO_LANE=no-content-addressing"
    "MOBILEGL_PIPE_PUSH=0x8000000000001fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-no-content-addressing-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_CSO_ON_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_CSO_LANE=content-addressed"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-content-addressed-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_CSO_OFF_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_CSO_LANE=no-content-addressing"
    "MOBILEGL_PIPE_PUSH=0x8000000000001fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-no-content-addressing-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.CsoContentAddressing.On."
    TEST_FILTER "CsoContentAddressingScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_CSO_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.CsoContentAddressing.Off."
    TEST_FILTER "CsoContentAddressingScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_CSO_OFF_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.CsoContentAddressing.On."
    TEST_FILTER "CsoContentAddressingScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_CSO_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.CsoContentAddressing.Off."
    TEST_FILTER "CsoContentAddressingScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_CSO_OFF_ENVIRONMENT}"
)

# --- G12: the P3a subsystem A/B, and G10's map-persistent-roundtrips lanes ------------
#
# THE MASKS ARE PHASE CONSTANTS, not hand-picked bits: 0x1fff is
# kMGPipeSubsystemsMigratedAtP4a (the push build's default as of P4a) and 0x7f is
# kMGPipeSubsystemsMigratedAtP2, which is that default with bits 7 (resources), 8 (vertex input)
# and 9-12 (P4a's four object families) cleared. MGPipe.h keeps each phase's constant alive as the
# next phase's A/B control for this reason, and a lane that spelled its own bit pattern would stop
# being the shape that ships the first time the default moved. THE ON LANE MOVED TO 0x1fff WITH THE
# PHASE and the OFF LANE'S 0x7f DID NOT: this A/B is about bit 7, and pinning its On arm at 0x1ff
# after P4a would measure the resource emitter on a configuration nothing ships. P4a's own A/B -
# 0x1fff against 0x1ff - is ObjectSubsystemControl's, further down.
#
# DirectGLES only. P3a migrates Espryt's buffer and VAO paths; Magma's buffer path is P7 and
# registers no MGPipeResourceOps, so a DirectVulkan arm would be measuring the client emitter
# against a backend nobody asked to change.
#
# Each reading entry gets a LOG PATH OF ITS OWN, and its ctest entry selects ONE case: the library
# opens the log fopen(path, "w") - every process in a lane truncates it - and these cases READ it.
# Two readers in one lane race under `ctest -j`, and the failure looks exactly like "the counter
# was never emitted". Same rule as the CSO lanes above and the verify arming lane below.
#
# MOBILEGL_PIPE_STATS_PERIOD=1 makes one summary line per eglSwapBuffers, which is what lets a
# workload be bracketed by two swaps and read back as a window covering exactly itself.
#
# Registered in EVERY build, including the pull build where none of the counters exists, so that
# `ctest -L integration-gpu` stays name-for-name identical between pull and push (gate G2). In a
# pull build MGITEST_PIPE_PUSH_BUILD is absent and every one of these cases skips saying so; a
# MOBILEGL_PIPE_PUSH value in the environment of a pull library steers nothing (Config.h declares
# the field inside the push guard), exactly as the HandleRecycle Legacy lanes already rely on.
mgl_itest_join_environment(MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_ON_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_RESOURCE_SUBSYSTEM_LANE=on"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/resource-subsystem-on-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_OFF_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_RESOURCE_SUBSYSTEM_LANE=off"
    "MOBILEGL_PIPE_PUSH=0x7f" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/resource-subsystem-off-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ResourceSubsystemControl.On."
    TEST_FILTER "ResourceSubsystemControlScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ResourceSubsystemControl.Off."
    TEST_FILTER "ResourceSubsystemControlScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_OFF_ENVIRONMENT}"
)

# G10's two counting entries, one per scenario that has a claim about the counter:
# StorageBufferRegrow asserts N definitions cost N round trips (never one per draw), and
# LargeArenaAdoption asserts one adoption costs exactly one. Each names a single case in its
# TEST_FILTER, for the private-log reason above.
mgl_itest_join_environment(MGL_ITEST_GLES_MPR_REGROW_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_MPR_LANE=storage-buffer-regrow"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/mpr-storage-buffer-regrow-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_MPR_ARENA_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_MPR_LANE=large-arena-adoption"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/mpr-large-arena-adoption-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.MapPersistentRoundtrips."
    TEST_FILTER "StorageBufferRegrowScenario.NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_MPR_REGROW_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.MapPersistentRoundtrips."
    TEST_FILTER "LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_MPR_ARENA_ENVIRONMENT}"
)

# LargeArenaAdoption's three behavioural cases under BOTH arms of the same A/B. This file is where
# an adopted store's whole life is exercised - the NULL-data definition that adopts it, an
# in-flight SubData, a readback and a GPU write - so if the handle path and the legacy
# BufferBackendOps path disagree about any of it, one of these two lanes goes red and names which.
# No log path: none of these three cases reads one, and giving them one would only add a file for
# four processes to truncate. The fourth case skips in both lanes for exactly that reason, saying
# so.
mgl_itest_join_environment(MGL_ITEST_GLES_ARENA_SUBSYSTEM_ON_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_PUSH=0x1fff"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_ARENA_SUBSYSTEM_OFF_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_PUSH=0x7f"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ResourceSubsystemOn."
    TEST_FILTER "LargeArenaAdoptionScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_ARENA_SUBSYSTEM_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ResourceSubsystemOff."
    TEST_FILTER "LargeArenaAdoptionScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_ARENA_SUBSYSTEM_OFF_ENVIRONMENT}"
)

# --- G12: the P4a subsystem A/B, and its dependency refusal ---------------------------
#
# THE MASKS ARE THE TWO PHASE CONSTANTS, exactly as the P3a block above: 0x1fff is
# kMGPipeSubsystemsMigratedAtP4a (the push build's default) and 0x1ff is
# kMGPipeSubsystemsMigratedAtP3a, which is that default with bits 9 (framebuffer), 10 (texture
# resources), 11 (samplers) and 12 (programs) cleared. THIS is the phase's "new 0x1ff off-lane": the
# constant P3a shipped survives as P4a's A/B control, and a lane that spelled its own bit pattern
# would stop being the shape that ships the first time the default moved.
#
# THE THIRD AND FOURTH LANES ARE THE DEPENDENCY REFUSALS (D-K2) and they are the entries in this
# family that are not vacuous before the emitters land, because both are decisions made from the
# bitmask alone. A half-honoured mask is invisible in the pixels by construction, which is why each
# needs an entry rather than a code comment.
#
#   0x9ff sets the sampler subsystem (bit 11) WITHOUT the texture resource subsystem (bit 10) that
#         every MGPBoundView::Texture and MGPImageView::Res depends on.
#   0x5ff sets the texture resource subsystem (bit 10) WITHOUT the sampler subsystem (bit 11):
#         D-K2's fourth row (ID-15). MGPTextureParams::BuiltinSampler is a SamplerCso handle and
#         only bit 11 mints sampler CSOs, so bit 10 alone would emit a null there and the applier's
#         Fatal is the next thing that happens. The brief called this pair harmless; P4a as built
#         says otherwise, and the refusal belongs in the texture family's arm resolver.
#
# In both, the bring-up must log ONE error naming both bits and run the legacy arm.
#
# ONE CASE PER LANE, through TEST_FILTER, and it is a constraint rather than a preference: each case
# READS the library's log and the log is a per-LANE resource (the library opens it fopen(path, "w"),
# so every process in a lane truncates it). Two entries in one lane race under `ctest -j` with a
# failure indistinguishable from "the counter was never emitted". Same rule as the CSO lanes, the
# map-persistent lanes and the verify arming lane.
#
# THE FIRST FOUR LANES ARE DirectGLES ONLY. P4a migrates Espryt's framebuffer, texture, sampler
# and program paths; Magma's are P7 and register nothing here, so a DirectVulkan lane that measured
# the emit[] bracket would be measuring the client emitters against a backend nobody asked to
# change.
#
# THE FIFTH AND SIXTH LANES INVERT THAT, and the inversion is the point (c0f, ID-39/ID-40). "Magma
# registers no consumer" stopped being a reason to have no lane the moment it became a THING THE
# CLIENT MUST CHECK: P4a's families were wired without P3a's "a backend registered
# MGPipeResourceOps" conjunct, so on Magma the client emitted, the applier ACCEPTED, the client
# cleared its dirty flags on that acceptance and Magma's legacy path found nothing to upload - 66
# DirectVulkan cases red, every one of them texture-upload shaped, every one of them green at
# 0x1ff. The fix's belt (the applier refusing and counting RefusedNoConsumer) is invisible in
# pixels and invisible in the emit[] bracket, so it needs its own entry, and it needs it on the
# backend WITHOUT the consumer. The DirectGLES twin is the control that keeps the assertion from
# being vacuously true of a tree where nothing emits anywhere.
#
# Both run at the PHASE DEFAULT (0x1fff) rather than a hand-picked mask, for the reason the whole
# block gives: the shape that ships is the shape worth measuring.
#
# Registered in EVERY build, including the pull build where none of the counters exists, so that
# `ctest -L integration-gpu` stays name-for-name identical between pull and push (gate G2). In a
# pull build MGITEST_PIPE_PUSH_BUILD is absent and every one of these cases skips saying so.
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_ON_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=on"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-on-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_OFF_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=off"
    "MOBILEGL_PIPE_PUSH=0x1ff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-off-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=refused"
    "MOBILEGL_PIPE_PUSH=0x9ff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-refused-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_TEXTURE_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=refused-texture"
    "MOBILEGL_PIPE_PUSH=0x5ff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-refused-texture-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_OBJECT_SUBSYSTEM_CONSUMER_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=consumer"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-consumer-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_OBJECT_SUBSYSTEM_NO_CONSUMER_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_OBJECT_SUBSYSTEM_LANE=no-consumer"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-no-consumer-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.On."
    TEST_FILTER "ObjectSubsystemControlScenario.ClearingTheP4aBitsStopsTheEmissionsAndNotThePixels"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.Off."
    TEST_FILTER "ObjectSubsystemControlScenario.ClearingTheP4aBitsStopsTheEmissionsAndNotThePixels"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_OFF_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.Refused."
    TEST_FILTER "ObjectSubsystemControlScenario.ASamplerBitWithoutTheTextureBitIsRefusedAndNamed"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.RefusedTexture."
    TEST_FILTER "ObjectSubsystemControlScenario.ATextureBitWithoutTheSamplerBitIsRefusedAndNamed"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_TEXTURE_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.Consumer."
    TEST_FILTER "ObjectSubsystemControlScenario.TheAppliersNoConsumerBeltNeverFiresBehindTheClientsGate"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_CONSUMER_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.ObjectSubsystemControl.NoConsumer."
    TEST_FILTER "ObjectSubsystemControlScenario.TheAppliersNoConsumerBeltNeverFiresBehindTheClientsGate"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_OBJECT_SUBSYSTEM_NO_CONSUMER_ENVIRONMENT}"
)

# --- The texture upload shape: RECORDED, NOT GATED in P4a (D-D4) ----------------------
#
# SSIM is blind to the box-versus-rect upload shape and the Mali cliff it hides is ~+6 ms/frame, so
# the shape needs a number - two numbers, in fact, the server's tex[emit= box= rect= jobs=] and the
# client's emit[ctu=], which agreeing is the whole reason both are published (D-L). What this lane
# asserts is that the numbers could be READ, that the server bracket's own arithmetic holds and that
# the two sides agree when both are non-zero; WHICH shape each texture took is RecordProperty'd and
# printed for MEASUREMENTS.md. P3b/P4b turns it into a gate against a gold standard, with the Mali
# frame-time delta published beside it - gating a shape this phase has not finished deciding would
# either pin today's shape as the answer or fail on the change that is the next phase's point.
#
# Its own log path and its own single-case filter, for the per-lane-log reason above.
mgl_itest_join_environment(MGL_ITEST_GLES_TEXTURE_UPLOAD_SHAPE_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_TEXTURE_UPLOAD_SHAPE_LANE=1"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/texture-upload-shape-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.TextureUploadShape."
    TEST_FILTER "TextureUploadShapeScenario.TheEmittedUploadShapeIsRecordedAndTheTwoSidesAgree"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_TEXTURE_UPLOAD_SHAPE_ENVIRONMENT}"
)

# --- G9: the mandatory red-before scenario -------------------------------------------
#
# TextureParamsWithoutASamplerViewScenario needs NO lane of its own, and that is deliberate rather
# than an omission. It reads no counter and no log - its observable is a sampled colour - and its
# claim is about the SHIPPING configuration, so the two ambient registrations at the top of this
# file (which run at the build's default mask) are exactly the arms it wants. A lane here would pin
# a mask and make the entry stop describing what ships the next time the default moved. The
# DirectVulkan ambient entries skip in the scenario's SetUp, naming the backend: the gap it is about
# is Espryt's SyncAttachmentObject / SyncNeccessaryTextures pair and P4a touches no DirectVulkan
# source but MagmaPipeArms.h (D-Q).
#
# D-E3 expects its second case, AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver, to be
# RED until package esprytobj lands. MEASURED ON THE CONTRACT COMMIT IT IS GREEN, and the scenario's
# header carries the mechanism: a texture parameter's only public-GL observable is a sample, and the
# sample repairs the state it was meant to catch (the unit sync list re-syncs whenever the params
# version moved). Nothing about any of that is expressed in this file - a registration that
# "expected" a red would be a gate that could never go green - and the ruling on the missing
# artefact is the integrator's, recorded in the gates result document.

if (MOBILEGL_PIPE_VERIFY)
    # 900s, not the ambient 120: the comparator re-reads every field of the fill mask at the verb
    # boundary and again at every accessor read, which the design budgets at 5-10x.
    set(MGL_ITEST_VERIFY_TIMEOUT 900)

    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-DirectGLES.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-DirectVulkan.log"
        ${MGL_ITEST_VULKAN_ENV})

    # The arming assertion's own lane, one case per backend, with a log path nothing else writes to.
    #
    # PipeVerifyArmingScenario.Armed reads the library's log, and the log is a per-LANE resource: it
    # is opened fopen(path, "w"), so every process in a lane truncates it. In the ambient Verify.
    # lane that is 400-odd processes on one path, run `-j 4` in CI, and a whole-file read there
    # races a neighbour's bring-up. Every other log-reading scenario in this file (UnlocatedIoBlocks,
    # the primgen reroute, the point-size demotion) is registered exactly like this for the same
    # reason. MGITEST_PIPE_ARMING_LANE is a harness marker - the library never reads it - and it is
    # what makes the case skip in the ambient lane instead of racing there.
    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_ARMING_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1" "MGITEST_PIPE_ARMING_LANE=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-arming-DirectGLES.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_ARMING_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_PIPE_VERIFY=1" "MGITEST_PIPE_ARMING_LANE=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-arming-DirectVulkan.log"
        ${MGL_ITEST_VULKAN_ENV})

    # Negative control A (G4). MOBILEGL_PIPE_VERIFY_FATAL=0 so the process SURVIVES its own
    # divergence and the case can read the report back out of the log; the CI step that exports
    # the same corruption against the ambient lane, where FATAL keeps its default of 1, asserts
    # the other half - that a divergence aborts and reds the entry.
    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_CORRUPT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters" "MOBILEGL_PIPE_VERIFY_FATAL=0"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-corrupt-DirectGLES.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_CORRUPT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters" "MOBILEGL_PIPE_VERIFY_FATAL=0"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-corrupt-DirectVulkan.log"
        ${MGL_ITEST_VULKAN_ENV})

    # Negative control B (G5). The omission skips the STAMP of one field for one verb while still
    # copying its value, which is indistinguishable from a fill row nobody wrote; the scenario
    # forks, so the resulting std::abort() is a datum in waitpid() rather than a dead lane.
    mgl_itest_join_environment(MGL_ITEST_GLES_POISON_OMIT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-poison-omit-DirectGLES.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_POISON_OMIT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-poison-omit-DirectVulkan.log"
        ${MGL_ITEST_VULKAN_ENV})

    # The whole suite again, per backend, with the comparator armed. Same scenarios, same
    # assertions, but every backend read of frontend state is now checked against a snapshot taken
    # from the live context at the verb boundary - which is what "the 742 integration entries
    # prove push equals pull" means. Labelled integration-gpu as well so a verify build's
    # `ctest -L integration-gpu` still describes the whole registration set.
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.Verify."
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.Verify."
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_VULKAN_VERIFY_ENVIRONMENT}"
    )

    # The arming assertion, one entry per backend. This is the entry that fails a lane whose library
    # never armed: it runs the same library and the same MOBILEGL_PIPE_VERIFY=1 as the ambient
    # entries above, but unlike them it cannot be green against a library with no comparator
    # compiled in. Its log is its own, so `-j 4` cannot make it flake.
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.VerifyArming."
        TEST_FILTER "PipeVerifyArmingScenario.Armed"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_ARMING_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.VerifyArming."
        TEST_FILTER "PipeVerifyArmingScenario.Armed"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_VULKAN_VERIFY_ARMING_ENVIRONMENT}"
    )

    # One case each: the knobs are process-wide, so a corrupted or poisoned process cannot also be
    # running the ambient assertions. These four entries are the ones that assert the RED - they
    # pass when the comparator and the poison report, and go red when either stops.
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.VerifyCorrupted."
        TEST_FILTER "PipeVerifyArmingScenario.CorruptedFieldIsReported"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_CORRUPT_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.VerifyCorrupted."
        TEST_FILTER "PipeVerifyArmingScenario.CorruptedFieldIsReported"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_VULKAN_VERIFY_CORRUPT_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.PoisonOmitted."
        TEST_FILTER "PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_POISON_OMIT_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.PoisonOmitted."
        TEST_FILTER "PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_VULKAN_POISON_OMIT_ENVIRONMENT}"
    )
endif()
