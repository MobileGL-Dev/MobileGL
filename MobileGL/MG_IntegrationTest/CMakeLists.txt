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
    Harness/P4aFinalFixPeek.cpp
    Harness/PersistentMapPeek.cpp
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
    Scenarios/PrimitiveRestartScenario.cpp
    Scenarios/FragCoordOriginScenario.cpp
    Scenarios/ClientVertexArrayScenario.cpp
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
    # P3b/P4b wave 2-D package D2: the verification half of the owner-keyed emission cursor.
    Scenarios/TextureViewAliasScenario.cpp
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
    Scenarios/P4aFinalFixScenario.cpp
    # P5's two new scenarios, targets B and C of the reduced path (BRIEF-P5 4). Both are
    # ORDINARY GL scenarios that run in every lane; the DirectGLES.Split. entries further down
    # run the same cases with MOBILEGL_TRANSPORT=inproc.
    Scenarios/TriangleScenario.cpp
    Scenarios/PersistentCoherentMapScenario.cpp
    Scenarios/DefaultFramebufferAcrossSwapScenario.cpp
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
    "TextureViewScenario.*:TextureViewAliasScenario.*:F1WireScenario.ComputeImageStoreThroughViewPreservesOtherRootLayer:F1WireScenario.TextureViewClearTargetsItsRootMipAndLayer:F1WireScenario.GenerateMipmapThroughViewKeepsOwnerLevelsAndLayersOutsideItsWindow")

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES."
    TEST_FILTER "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:${MGL_ITEST_GLES_VIEW_FILTER}"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_ENVIRONMENT}"
)

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan."
    TEST_FILTER "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_ENVIRONMENT}"
)

foreach(backend DirectGLES DirectVulkan)
    if (backend STREQUAL "DirectGLES")
        set(mechanism_environment "${MGL_ITEST_GLES_ENVIRONMENT}")
    else()
        set(mechanism_environment "${MGL_ITEST_VULKAN_ENVIRONMENT}")
    endif()
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "${backend}."
        TEST_FILTER "${MGL_ITEST_MONOLITH_MECHANISM_FILTER}"
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "integration-gpu\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_TIMEOUT}
            ENVIRONMENT "${mechanism_environment}\;MOBILEGL_TRANSPORT=monolith")
endforeach()

# This is Espryt's existing feature opt-in, independent of transport. Exercise
# the actual view implementation instead of inheriting its default-off skip.
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES."
    TEST_FILTER "${MGL_ITEST_GLES_VIEW_FILTER}"
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS integration-gpu TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_ENVIRONMENT}\;MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1")

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
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.MallocPerturb."
    TEST_FILTER "P4aFinalFixScenario.ADirtyTextureDeletedBeforeAnyVerbIsWalkedByTheNextDrain"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_MALLOC_PERTURB_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.MallocPerturb."
    TEST_FILTER "P4aFinalFixScenario.ADirtyTextureDeletedBeforeAnyVerbIsWalkedByTheNextDrain"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_MALLOC_PERTURB_ENVIRONMENT}"
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
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_HANDLE_ARM=handles" ${MGL_ITEST_HANDLES_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_HANDLES_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=handles" ${MGL_ITEST_HANDLES_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_HANDLE_LEGACY_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_HANDLE_ARM=legacy" "MOBILEGL_PIPE_PUSH=0"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_LEGACY_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=legacy" "MOBILEGL_PIPE_PUSH=0"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_ABA_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=aba" "MOBILEGL_PIPE_PUSH=0"
    ${MGL_ITEST_ABA_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_HANDLE_ABA_HANDLES_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_HANDLE_ARM=aba"
    ${MGL_ITEST_HANDLES_ARM_KNOBS} ${MGL_ITEST_ABA_ARM_KNOBS}
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.HandleRecycle.Handles."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_HANDLE_HANDLES_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.HandleRecycle.Handles."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_HANDLE_HANDLES_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.HandleRecycle.Legacy."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_HANDLE_LEGACY_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.HandleRecycle.Legacy."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_HANDLE_LEGACY_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.HandleRecycle.AbaControl."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_HANDLE_ABA_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.HandleRecycle.AbaControlHandles."
    TEST_FILTER "HandleRecycleScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
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
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_CSO_LANE=content-addressed"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-content-addressed-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_CSO_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_CSO_LANE=no-content-addressing"
    "MOBILEGL_PIPE_PUSH=0x8000000000001fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-no-content-addressing-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_CSO_ON_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_CSO_LANE=content-addressed"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-content-addressed-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_CSO_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_CSO_LANE=no-content-addressing"
    "MOBILEGL_PIPE_PUSH=0x8000000000001fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/cso-no-content-addressing-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.CsoContentAddressing.On."
    TEST_FILTER "CsoContentAddressingScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_CSO_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.CsoContentAddressing.Off."
    TEST_FILTER "CsoContentAddressingScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_CSO_OFF_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.CsoContentAddressing.On."
    TEST_FILTER "CsoContentAddressingScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_CSO_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.CsoContentAddressing.Off."
    TEST_FILTER "CsoContentAddressingScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
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
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_RESOURCE_SUBSYSTEM_LANE=on"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/resource-subsystem-on-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_RESOURCE_SUBSYSTEM_LANE=off"
    "MOBILEGL_PIPE_PUSH=0x7f" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/resource-subsystem-off-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ResourceSubsystemControl.On."
    TEST_FILTER "ResourceSubsystemControlScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ResourceSubsystemControl.Off."
    TEST_FILTER "ResourceSubsystemControlScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_RESOURCE_SUBSYSTEM_OFF_ENVIRONMENT}"
)

# G10's two counting entries, one per scenario that has a claim about the counter:
# StorageBufferRegrow asserts N definitions cost N round trips (never one per draw), and
# LargeArenaAdoption asserts one adoption costs exactly one. Each names a single case in its
# TEST_FILTER, for the private-log reason above.
mgl_itest_join_environment(MGL_ITEST_GLES_MPR_REGROW_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_MPR_LANE=storage-buffer-regrow"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/mpr-storage-buffer-regrow-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_MPR_ARENA_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_MPR_LANE=large-arena-adoption"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/mpr-large-arena-adoption-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.MapPersistentRoundtrips."
    TEST_FILTER "StorageBufferRegrowScenario.NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_MPR_REGROW_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.MapPersistentRoundtrips."
    TEST_FILTER "LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
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
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_PUSH=0x1fff"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_GLES_ARENA_SUBSYSTEM_OFF_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_PUSH=0x7f"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ResourceSubsystemOn."
    TEST_FILTER "LargeArenaAdoptionScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_ARENA_SUBSYSTEM_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ResourceSubsystemOff."
    TEST_FILTER "LargeArenaAdoptionScenario.*"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
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
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_OBJECT_SUBSYSTEM_LANE=on"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
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
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-consumer-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
mgl_itest_join_environment(MGL_ITEST_VULKAN_OBJECT_SUBSYSTEM_NO_CONSUMER_ENVIRONMENT
    "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MGITEST_OBJECT_SUBSYSTEM_LANE=no-consumer"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/object-subsystem-no-consumer-DirectVulkan.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.On."
    TEST_FILTER "ObjectSubsystemControlScenario.ClearingTheP4aBitsStopsTheEmissionsAndNotThePixels"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_ON_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.Off."
    TEST_FILTER "ObjectSubsystemControlScenario.ClearingTheP4aBitsStopsTheEmissionsAndNotThePixels"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_OFF_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.Refused."
    TEST_FILTER "ObjectSubsystemControlScenario.ASamplerBitWithoutTheTextureBitIsRefusedAndNamed"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.RefusedTexture."
    TEST_FILTER "ObjectSubsystemControlScenario.ATextureBitWithoutTheSamplerBitIsRefusedAndNamed"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_REFUSED_TEXTURE_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.ObjectSubsystemControl.Consumer."
    TEST_FILTER "ObjectSubsystemControlScenario.TheAppliersNoConsumerBeltNeverFiresBehindTheClientsGate"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_GLES_OBJECT_SUBSYSTEM_CONSUMER_ENVIRONMENT}"
)
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan.ObjectSubsystemControl.NoConsumer."
    TEST_FILTER "ObjectSubsystemControlScenario.TheAppliersNoConsumerBeltNeverFiresBehindTheClientsGate"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "integration-gpu\;integration-monolith-control"
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        ENVIRONMENT "${MGL_ITEST_VULKAN_OBJECT_SUBSYSTEM_NO_CONSUMER_ENVIRONMENT}"
)

# --- The texture upload shape: A GATE as of P3b/P4b R-11 (D-D4) -----------------------
#
# SSIM is blind to the box-versus-rect upload shape and the Mali cliff it hides is ~+6 ms/frame, so
# the shape needs a number - two numbers, in fact, the server's tex[emit= box= rect= jobs=] and the
# client's emit[ctu=], which agreeing is the whole reason both are published (D-L).
#
# P4a RECORDED those numbers rather than gating them, because the per-storage-owner emission cursor
# with view/owner index remapping - what actually decides the shape for a texture uploaded through a
# VIEW - was the next phase's and gating an unfinished shape pins a non-answer. THAT WORK LANDED
# (MG_Impl/Pipe/TextureEmit.h's owner-keyed drain list; TextureObjectView::ToOwner*), so this lane's
# RecordProperty block is now EXPECT_EQ against a gold row measured on this cursor, and the workload
# carries a third texture written through a glTextureView so the remap is inside the counted window.
#
# MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 IS THEREFORE MANDATORY HERE, not decorative: without it the
# case SKIPS rather than dropping the third texture, because a conditional workload behind a fixed
# gold row is a gate that passes by shrinking.
#
# THE MALI FRAME-TIME DELTA D-D4 asks for beside the gold is NOT SCHEDULABLE in this phase - one
# Redmi, an Adreno, no Mali part - and is recorded as a declared deviation in the scenario header
# and in docs/Disaggregated/MEASUREMENTS.md rather than modelled.
#
# Its own log path and its own single-case filter, for the per-lane-log reason above.
mgl_itest_join_environment(MGL_ITEST_GLES_TEXTURE_UPLOAD_SHAPE_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_TEXTURE_UPLOAD_SHAPE_LANE=1"
    "MOBILEGL_PIPE_PUSH=0x1fff" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/texture-upload-shape-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.TextureUploadShape."
    TEST_FILTER "TextureUploadShapeScenario.TheEmittedUploadShapeIsRecordedAndTheTwoSidesAgree"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        RESOURCE_LOCK texture-upload-shape-DirectGLES.log
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
        "MOBILEGL_TRANSPORT=monolith"
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-DirectGLES.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_ENVIRONMENT
        "MOBILEGL_TRANSPORT=monolith"
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
        "MOBILEGL_TRANSPORT=monolith"
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1" "MGITEST_PIPE_ARMING_LANE=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-arming-DirectGLES.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_ARMING_ENVIRONMENT
        "MOBILEGL_TRANSPORT=monolith"
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_PIPE_VERIFY=1" "MGITEST_PIPE_ARMING_LANE=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-arming-DirectVulkan.log"
        ${MGL_ITEST_VULKAN_ENV})

    # Negative control A (G4). MOBILEGL_PIPE_VERIFY_FATAL=0 so the process SURVIVES its own
    # divergence and the case can read the report back out of the log; the CI step that exports
    # the same corruption against the ambient lane, where FATAL keeps its default of 1, asserts
    # the other half - that a divergence aborts and reds the entry.
    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_CORRUPT_ENVIRONMENT
        "MOBILEGL_TRANSPORT=monolith"
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters" "MOBILEGL_PIPE_VERIFY_FATAL=0"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-corrupt-DirectGLES.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_CORRUPT_ENVIRONMENT
        "MOBILEGL_TRANSPORT=monolith"
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters" "MOBILEGL_PIPE_VERIFY_FATAL=0"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-corrupt-DirectVulkan.log"
        ${MGL_ITEST_VULKAN_ENV})

    # Negative control B (G5). The omission skips the STAMP of one field for one verb while still
    # copying its value, which is indistinguishable from a fill row nobody wrote; the scenario
    # forks, so the resulting std::abort() is a datum in waitpid() rather than a dead lane.
    mgl_itest_join_environment(MGL_ITEST_GLES_POISON_OMIT_ENVIRONMENT
        "MOBILEGL_TRANSPORT=monolith"
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-poison-omit-DirectGLES.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_POISON_OMIT_ENVIRONMENT
        "MOBILEGL_TRANSPORT=monolith"
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
        TEST_FILTER "-${MGL_ITEST_GLES_VIEW_FILTER}"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.Verify."
        TEST_FILTER "${MGL_ITEST_GLES_VIEW_FILTER}"
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "integration-verify\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_ENVIRONMENT}\;MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1")
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.Verify."
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
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
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_ARMING_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.VerifyArming."
        TEST_FILTER "PipeVerifyArmingScenario.Armed"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
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
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_CORRUPT_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.VerifyCorrupted."
        TEST_FILTER "PipeVerifyArmingScenario.CorruptedFieldIsReported"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_VULKAN_VERIFY_CORRUPT_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.PoisonOmitted."
        TEST_FILTER "PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_POISON_OMIT_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.PoisonOmitted."
        TEST_FILTER "PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-verify\;integration-monolith-control"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_VULKAN_POISON_OMIT_ENVIRONMENT}"
    )
endif()

# --- P5: the DirectGLES.Split. lanes and the persistent-map ARM lanes ------------------------
#
# THE PRIVATE SECOND LABEL IS WHAT MAKES THE LANE FALSIFIABLE, and it is the verify block above
# copied verbatim for the same argument: `ctest -L integration-split --no-tests=error` in a build
# that forgot -DMOBILEGL_BUILD_DISAGGREGATED=ON matches NOTHING and FAILS, instead of reporting a
# green run of zero entries. Keeping `integration-gpu` as well is why "a split build's
# `ctest -L integration-gpu` still describes the whole registration set" stays true.
#
# REGISTERED INSIDE `if (MOBILEGL_BUILD_DISAGGREGATED)`, and that is what keeps gate G2 (pull and
# push name-for-name identical) untouched: neither of those two builds sets the option, so the
# Split family is absent from BOTH of them rather than present in one. The two SCENARIO FILES the
# lanes point at are added to the source list unconditionally, so their cases DO appear in the
# ambient DirectGLES./DirectVulkan. registrations of every build - they are ordinary GL scenarios
# and their monolith behaviour is the baseline the split arm is compared against.
#
# WHAT ARMS THE SPLIT LANES, AND WHY IT IS NOT A PROBE OVER SOURCE TEXT.
#
# Until a real client session exists, MOBILEGL_TRANSPORT=inproc is parsed (CONTRACT-P5 5) and then
# nothing consumes it, so a Split entry would go green having run monolith end to end - the exact
# failure the lane exists to prevent. The first version of this file answered "has the client
# landed" with a CMake conjunction over c0's stub files: the c1 symbols present AND no
# `Fatal.Unimplemented` surviving. REVIEW FINDING M-1 FALSIFIED IT BY PERFORMING IT - a
# `sed -i 's/Fatal{Unimplemented/Fatal{NotYetImplemented/'` over c0's six stubs, with every entry
# point still aborting and ImplementedVerbCount() still 0, armed all eleven lanes and EIGHT WENT
# GREEN. A single comment line carrying the marker did the opposite and would keep them dark
# forever (M-2), and two stub files outside the two probed directories allowed a partial arm (M-3).
#
# A statement about source text can always be falsified by editing source text, and the people
# most likely to edit it are the ones landing the packages the probe watches for. So the arming
# condition moved into the PROCESS: Harness/SplitRuntimePeek.cpp reads MG_Config::Transport,
# ClientSession::Active() and ImplementedVerbCount() - three values c0 shipped and documented,
# none of which a message edit can move - and every Split case skips, naming the first one that is
# not true. All this file has to decide now is whether that peek can be COMPILED, which is exactly
# "did this build compile MG_Remote", which is exactly the build option.
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
# asks the membership question of (b1-v1.md 4.1 item 2). TWO halves, and both are needed:
# MOBILEGL_BUILD_DISAGGREGATED because a build that never compiled MG_Remote cannot LINK the call,
# and the content probe because b1 may not have landed it yet. __has_include in the peek would
# answer yes in a pull build - the header is in the source tree of every build - and turn a
# healthy pull build into a link error, which is why this decision is made here.
if (MOBILEGL_BUILD_DISAGGREGATED)
    mgl_itest_probe_for_symbol(MGL_ITEST_PERSISTENT_MAP_TRACKER
        "${MGL_ITEST_ROOT}/MobileGL/MG_Remote/Client" "IsLivePersistentMap")
    if (MGL_ITEST_PERSISTENT_MAP_TRACKER)
        message(STATUS "Integration tests: package b1's persistent-map tracker is present "
                       "(${MGL_ITEST_PERSISTENT_MAP_TRACKER}) - PersistentCoherentMapScenario's "
                       "membership assertion is live")
        target_compile_definitions(MobileGLIntegrationTest PRIVATE -DMGITEST_PERSISTENT_MAP_TRACKER=1)
    else()
        message(STATUS "Integration tests: no source under MobileGL/MG_Remote/Client names "
                       "IsLivePersistentMap - package b1's tracker has not landed, so "
                       "PersistentCoherentMapScenario's membership assertion SKIPS")
    endif()
endif()

# ARCHITECTURE.md:543 - every new ctest ENVIRONMENT and add_trace_replay_test's SPLIT branch
# carries MOBILEGL_IPC_SERVER_PATH, because the dladdr fallback cannot find the server from a
# binary that links MobileGL_s statically. P5 is inproc-only and nothing reads the value yet; it
# is carried now so that an unparsed variable and a parsed-and-ignored one stop being
# indistinguishable the day P6 consumes it. A generator expression is deliberately NOT used here:
# a $<TARGET_FILE_DIR:...> inside a gtest_discover_tests PROPERTIES value is written into the
# generated ctest include file verbatim and never expanded.
set(MGL_ITEST_SPLIT_SERVER_PATH "${CMAKE_BINARY_DIR}/libMobileGLServer.so")

# P3b/P4b wave 2-D package D2: MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 IS ON THE SHARED GLES
# SPLIT/SPAWN/TCP LISTS, not on a private entry, and that is the deliberate reading of the plan
# row. The knob is Espryt's FEATURE opt-in (ConfigLoader.cpp:213), independent of transport: it
# advertises GL_ARB_texture_view and nothing else. Without it TextureViewAliasScenario - and
# TextureViewScenario, if it is ever registered here - green-skips in all three arms and the
# gate is a name in a list. The monolith DirectGLES lane has carried it on its own view entry
# since P4a for exactly this reason; the split arms had no equivalent.
#
# It is NOT added to MGL_ITEST_VULKAN_{SPLIT,SPAWN,TCP}_ENVIRONMENT below: the variable is read
# only by ConfigLoader's Espryt feature block, so on DirectVulkan it would be a parsed-and-
# ignored name, and tier 2's informational Magma replay of these registrations wants the two
# backends' arms to differ only where the product differs.
mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc"
    "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
    "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

# The two counting lanes, one per transport, and they are a PAIR: exit gate E3(c) asks that the
# split arm's `mpr` equal the monolith arm's, and one test process can only ever see its own. So
# both lanes run the same workload and assert the same constant, and the equality holds by
# construction with each half able to fail on its own.
#
# Each gets a LOG PATH OF ITS OWN and a TEST_FILTER that selects ONE case, for the reason
# PipeStatsWindow.h gives: the library opens its log fopen(path, "w"), so every process in a lane
# truncates it, and two readers in one lane race under `ctest -j`. RESOURCE_LOCK on top of that,
# for the reason the UnlocatedIoBlocks lane gives - measured on this tree, three runs of the full
# integration-gpu label at -j 8 produced 4, 0 and 2 spurious failures of the log-reading cases
# without it.
#
# The monolith lane declares NO arm: which arm AcquireMemoryRange takes for a sub-16-MiB
# PERSISTENT|WRITE|COHERENT map is a property of the driver and the build, so the case RECORDS it
# there and skips the assertion. Only the split lane - where R-6 pins the adopt tier at T2 -
# declares one.
# MOBILEGL_TRANSPORT=monolith IS NAMED HERE ON PURPOSE, and it is review finding N-6. This entry
# is the MONOLITH half of a pair; without the property a job-level or gate-level
# `MOBILEGL_TRANSPORT=inproc` export reaches it (observed: its own log then carried
# `Config: MOBILEGL_TRANSPORT=inproc`) and, in the inproc arm of the three-arm A/B, BOTH halves of
# the pair were inproc. Nothing failed - each half asserts the same constant - but the pair was
# not the pair its name describes. A ctest ENVIRONMENT property overrides the job environment,
# which is the one case where that is what you want.
mgl_itest_join_environment(MGL_ITEST_GLES_PMAP_ARM_ENVIRONMENT
    "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_PMAP_LANE=1" "MOBILEGL_TRANSPORT=monolith"
    "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
    "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/persistent-map-arm-DirectGLES.log"
    ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES.PersistentMapArm."
    TEST_FILTER "PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS integration-gpu
        TIMEOUT ${MGL_ITEST_TIMEOUT}
        RESOURCE_LOCK persistent-map-arm-DirectGLES.log
        ENVIRONMENT "${MGL_ITEST_GLES_PMAP_ARM_ENVIRONMENT}"
)

if (MOBILEGL_BUILD_DISAGGREGATED)
    # One block per scenario, following the DirectGLES.MapPersistentRoundtrips. precedent: a
    # `:`-separated multi-pattern TEST_FILTER is not used anywhere in this file, so its escaping
    # through gtest_discover_tests' flat PROPERTIES forwarding is unproven, and three blocks cost
    # nothing.
    #
    # ClearThenReadPixelsScenario is target A of the reduced path and already existed; Triangle is
    # target B and PersistentCoherentMap is target C, both new in P5.
    # ---- P6 `t6`: THE SPAWN ARM, beside the inproc one and never instead of it --------------
    #
    # EXIT GATES 9.2 AND 9.3: `integration-spawn`'s case-name set must be IDENTICAL to
    # `integration-split`'s, and it must be green at the same count.
    #
    # THAT IDENTITY IS MADE STRUCTURAL RATHER THAN MAINTAINED. Registering the spawn arm as a
    # second copy of each block would make the two sets agree today and drift the first time
    # somebody adds a scenario to one of them - and a spawn lane covering LESS than the split
    # lane goes green while proving less, with nothing to say so. One macro emits both arms from
    # one filter, so a case added to either is added to both or to neither.
    #
    # ONE ENVIRONMENT DIFFERENCE, AND IT IS THE SUBJECT OF THE LANE. MOBILEGL_TRANSPORT=spawn
    # makes the test process the CLIENT and has it launch a server from MOBILEGL_IPC_SERVER_PATH
    # - the same path the inproc arm already carries, because under inproc the library reads it
    # and decides it does not need one.
    # D2: the texture-view opt-in, for the reason stated over the SPLIT list above. Under spawn
    # the variable has to reach BOTH processes; it does, because the server is spawned from this
    # process's environment.
    mgl_itest_join_environment(MGL_ITEST_GLES_SPAWN_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=spawn"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    # A single supervisor serves one session at a time. Fixture teardown also reaps a
    # child left behind by a failed client; the resource lock serializes this lane.
    set(MOBILEGL_ITEST_TCP_ENDPOINT "tcp://127.0.0.1:40613" CACHE STRING "Loopback TCP integration endpoint")
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
    # P3b/P4b wave 2-D package D2: THE TCP LANE'S SERVER IS PRE-STARTED BY THIS FIXTURE, not
    # spawned from a client that already carries the lane environment, so a FEATURE opt-in has to
    # be set here as well as on MGL_ITEST_GLES_TCP_ENVIRONMENT. tcp_server_fixture.py builds the
    # supervisor's environment from os.environ (:60) and strips only the transport keys (:63-65),
    # so a ctest ENVIRONMENT property on this entry reaches the server process.
    #
    # Measured, not assumed: without it every TextureViewAliasScenario case SKIPPED in the Tcp
    # arm with "GL_ARB_texture_view not advertised" while the identical Split and Spawn arms ran
    # - because under inproc and spawn the server inherits the client's environment and under tcp
    # it does not. A feature knob that reaches two of three arms is the worst of the three states:
    # the lane is green and tests nothing.
    set_tests_properties(TcpServer.Start PROPERTIES FIXTURES_SETUP mobilegl-tcp TIMEOUT 20
        ENVIRONMENT "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1")
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

    # ---- P7 package L: THE SAME THREE ARMS FOR MAGMA ----------------------------------------
    #
    # The three lists above are the DirectGLES half of a dimension that had only one value. P7
    # exit gate 1 asks for integration AND trace green "under Magma push and split", and until
    # this package the shared labels `integration-split` / `-spawn` / `-tcp` held ZERO
    # DirectVulkan entries: every arm of mgl_itest_register_split_arms hardcoded
    # MOBILEGL_BACKEND_TYPE=DirectGLES and MGL_ITEST_MAGMA_WIRE_ENVIRONMENT hardcoded
    # MOBILEGL_TRANSPORT=inproc, so the gate was not merely unmet, it was UNMEASURABLE.
    #
    # THE ONE DIFFERENCE FROM THE GLES TRIO IS ${MGL_ITEST_VULKAN_ENV}, AND IT IS NOT COSMETIC.
    # That list carries VK_ICD_FILENAMES and, on lavapipe, the three iterationRP repairs the
    # monolith `DirectVulkan.` arm has been pinned to since they were added (see its definition
    # near the top of this file). A DirectVulkan lane that appended ${MGL_ITEST_COMMON_ENV}
    # instead would run against whatever ICD the loader found first, which is exactly how a lane
    # measures a different driver from the one the gate it mirrors measures.
    #
    # MOBILEGL_ITEST_REQUIRE_GPU=1 IS PINNED, as the existing Magma lanes pin it: on a host with
    # no usable Vulkan every one of these entries would otherwise SKIP, and a lane that skipped
    # everything and a lane that passed everything are the same green in ctest.
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

    # ---- P7 package L: ONE SHAPE FOR EVERY MAGMA LANE'S THREE ARMS --------------------------
    #
    # The gated Magma tier is six hand-laid blocks (Fm, Buffers, RunAhead, Caches, NamedBlit,
    # P5fRsp) because each carries a flavour of its own. What they share is the ARM: the same
    # three transports, the same tcp fixture, the same label and log-name arithmetic. This macro
    # is that shared half, so a seventh block gets its arms right by calling one line.
    #
    # Sets, for the arm it is given:
    #   mglItestArmLower          split | spawn | tcp
    #   mglItestArmLogTag         "" on the inproc arm, "-spawn"/"-tcp" on the others - THE
    #                             INPROC ARM'S PRIVATE LOG PATHS DO NOT MOVE. magma_cache_checks
    #                             and magma_runahead_checks name entries and re-derive their logs
    #                             from discovery, and P6.5's device evidence cites these paths.
    #   mglItestArmProperties     the tcp fixture + resource lock; empty otherwise
    #   mglItestArmTransportEnv   the arm's transport pins, to append to the lane's flavour
    #   mglItestMagmaLabels       "integration-magma-<arm>" plus one label per WORD in ARGN
    #
    # THE SUB-LABEL SPELLING IS ASYMMETRIC ON PURPOSE. On the inproc arm a word keeps its
    # historical name (`integration-magma-buffers`); on the others the ARM comes first
    # (`integration-magma-spawn-buffers`). `ctest -L` is a regex, so the obvious
    # `integration-magma-buffers-spawn` would be SELECTED by the CI step that runs
    # `-L integration-magma-buffers` and require_greens its nineteen inproc entries by name -
    # the lane would red on "required green entries differ" the day this package landed.
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
    # ---- P7 package L: THE BACKEND IS NOW A PARAMETER, AND THE TWO TIERS IT REGISTERS -------
    #
    # TIER 1, GATED. DirectGLES keeps every label it had - `integration-gpu`,
    # `integration-{split,spawn,tcp,tcp-device}` and the caller's extra stems - so the sets CI
    # gates on and the names G14 ratchets are byte-for-byte what they were. Magma's own gated
    # tier is `integration-magma-{split,spawn,tcp}`, laid out by hand further down this file
    # because each of those lanes carries a FLAVOUR (strict + dual-block state, run-ahead
    # credits, cache pinning) that no shared filter can express.
    #
    # TIER 2, INFORMATIONAL, AND THAT IS THE POINT. DirectVulkan through THIS macro registers
    # `integration-magma-all-{split,spawn,tcp}`: every scenario the split arms run, on Magma, on
    # all three transports. It is deliberately in NO CI gating step. P7's exit gate 1 needs a
    # lane that can be RED - the first run's red list IS the work list wave 2 clusters A/B/C
    # divide up - and a red list is worthless if the only way to record it is to break the
    # build's green. When a cluster retires its share, its cases move from this tier into the
    # gated one, and CI stays honest at every step in between.
    #
    # THE LABEL SPELLINGS ARE CHOSEN AGAINST `ctest -L`'s REGEX MATCH (ID-131's rule, which this
    # file has paid for twice). `-L integration-magma-spawn` must NOT select this tier, so the
    # tier's word goes in the MIDDLE - `integration-magma-all-spawn` does not contain
    # `integration-magma-spawn` as a substring, while the obvious `integration-magma-spawn-all`
    # does and would have quietly pulled ~100 expected-red entries into the gated lane. Checked
    # in both directions for all six spellings.
    #
    # NO `integration-gpu` ON THE MAGMA ARMS, deliberately: that label is a hard CI gate and the
    # existing Magma split entries do not carry it either. NO EXTRA LABEL STEMS either - a stem
    # like `integration-clientarrays` would produce `integration-clientarrays-spawn`, which is
    # the label the DirectGLES spawn arm already owns, and one `ctest -L` would then run two
    # backends under one name.
    #
    # NO TcpDevice ARM ON MAGMA: the device endpoint is one phone, run serially, and doubling
    # its matrix before the host lane is even green buys nothing. Stated rather than implied.
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
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "${mglItestBackend}.${mglItestArm}.${prefixTail}"
                TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}"
                TEST_FILTER "${scenarioFilter}"
                DISCOVERY_TIMEOUT 30
                PROPERTIES
                    LABELS "${mglItestArmLabels}"
                    TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ENVIRONMENT "${MGL_ITEST_${mglItestEnvStem}_${mglItestArmUpper}_ENVIRONMENT}"
                    ${mglItestArmProperties}
            )
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
    mgl_itest_register_split_arms("TriangleScenario.*" "")
    mgl_itest_register_split_arms("PersistentCoherentMapScenario.*" "")

    # ---- P3b/P4b espryt D1 slice 1: THE ORPHANED XFB CAPTURE TARGET -------------------------
    #
    # The two cases whose whole subject is the split arm. A scattered capture is a
    # read-modify-write of the destination's pre-capture bytes, and under a transport those bytes
    # are the SERVER's staged shadow - so a buffer the application orphaned
    # (glBufferData(size, NULL)) and only partly uploaded is the one shape where the shadow
    # legitimately cannot answer for the whole range. The scatter used to read that as "discard
    # this target": silently for a wholly orphaned store, and as
    # Fatal{StageSnapshotTooNarrow, "xfb_scatter_pre_capture"} for a partly uploaded one.
    #
    # ---- P3b/P4b espryt D1 slice 2: THE WHOLE XFB FAMILY, once the writeback is sliced -------
    #
    # Slice 1 registered only OrphanedCaptureTarget* here, because the rest of these two files
    # could not survive a transport: both writeback producers posted the WHOLE bound range as one
    # SEG_EVENT record, and a record above half the ring (128 KiB at the 256 KiB default) is
    # refused outright, which the server answers with Fatal{EventRingOverflow}. With the event
    # sliced, the two scenarios go on in full - including the megabyte-range pair whose whole
    # subject is that slicing, and XfbRepeatedCaptureScenario's gl_NextBuffer compacted-target
    # shape, which is the first thing outside the census to exercise the archive-served stride
    # and varyings (ProgramArchive->Link's xfb fields) on the server.
    mgl_itest_register_split_arms("XfbAfterClipDistanceScenario.*" "Xfb.")
    mgl_itest_register_split_arms("XfbRepeatedCaptureScenario.*" "Xfb.")

    # ---- P3b/P4b espryt D1 slice 4: THE READBACK FAMILY ON THE SPLIT ARMS -------------------
    #
    # The 回读 half of the plan's "回读 / pack state" row. The PACK half is done - server reads
    # with neutral pack state, the client scatters (ID-49, EmitTables.cpp) - and the READBACK half
    # had no split coverage at all, so every claim about it was a reading of the code rather than
    # a run of it. Arming the lanes is the first half of the work and the census is the second:
    # what reds here IS the remaining worklist, and each red is either D1's to fix or excluded BY
    # NAME with the Fatal quoted, the way the d1 draw block excludes
    # MultiDrawScenario.ClientSideIndicesBatchMatchesUnrolledDraws.
    mgl_itest_register_split_arms("PixelStoreSweepScenario.*" "")
    mgl_itest_register_split_arms("PackedWordReadbackScenario.*" "")
    mgl_itest_register_split_arms("LayeredTextureReadbackScenario.*" "")
    # FramebufferChurnScenario shares PixelStoreSweepScenario.cpp and is the other half of its
    # story - ~3300 texture+framebuffer pairs per case, the shape that walked an Adreno driver to
    # 1.2 GB of leaked objects and then answered every readback with someone else's pixels. Under
    # a transport every one of those pairs is also a create/destroy record round trip, so this is
    # the only entry in the readback block that says anything about the OBJECT lifetime half.
    mgl_itest_register_split_arms("FramebufferChurnScenario.*" "")

    # DepthStencilReadbackMatrixScenario GETS ITS OWN BLOCK, because it needs a variable the
    # registration macro cannot carry: MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1.
    #
    # Without it this scenario is UNFALSIFIABLE on the machines this suite runs on, and the
    # ambient DirectGLES.ForcedDepthStencilEmulation. lane says why: OpenGL ES has no depth or
    # stencil readback in core, but Mesa accepts the reads anyway, so on llvmpipe every case goes
    # green through a native path the Adreno device does not have - deleting the whole emulation
    # left all of them passing. A split arm that inherited the ambient default would be measuring
    # the same non-path three more times. So the pin is MIRRORED here, which is what the plan's
    # row asks for.
    foreach(mglItestDsArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestDsArm})
        string(TOLOWER "${mglItestDsArm}" mglItestDsArmLower)
        mgl_itest_join_environment(mglItestDsEnvironment
            "MOBILEGL_BACKEND_TYPE=DirectGLES" "MGITEST_SPLIT_LANE=1"
            "MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1"
            ${mglItestArmTransportEnv} ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectGLES.${mglItestDsArm}.ForcedDs."
            TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}"
            TEST_FILTER "DepthStencilReadbackMatrixScenario.*"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-gpu\;integration-${mglItestDsArmLower}"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                ENVIRONMENT "${mglItestDsEnvironment}"
                ${mglItestArmProperties}
        )
        # Into the accumulated list like every other arm entry, so SplitLogPaths gives each of
        # these a private MOBILEGL_LOG_FILE_PATH too - two processes append on the spawn arm.
        list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
        math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
    endforeach()


    # ---- P5e fix1: THE PUSH-MONOLITH ARM, INSIDE THE GATED SET (ID-107) ----------------------
    #
    # WHY A MONOLITH LANE CARRIES THE `integration-split` LABEL. That label does not mean "runs
    # inproc" - it means "the set the disaggregation gate runs", which is exactly why it is
    # registered inside this `if (MOBILEGL_BUILD_DISAGGREGATED)` and why the block above argues
    # that a build without the option must match NOTHING under it. ID-81 gives the push build TWO
    # server arms, selected at runtime by MG_Config::Transport, and until now only one of them had
    # a gated entry: every Split block above exports MOBILEGL_TRANSPORT=inproc, and every
    # split-only scenario skips itself the moment the runtime is not split (SplitRuntimeSkipReason
    # in their SetUp), so the ambient monolith registration of those files runs zero cases. The
    # arm the phone actually ships on was therefore ungated, and P5e landed a null dereference on
    # it under 2250 green unit entries and 113 green split entries (ID-107: the game died in
    # Lightmap.<init> -> clearColorTexture -> glClear). A second arm needs a second lane, in the
    # same set, or "the gate is green" keeps meaning "one of the two arms is green".
    #
    # MOBILEGL_TRANSPORT=monolith IS NAMED, for review finding N-6's reason exactly as the
    # PersistentMapArm pair above names it: a ctest ENVIRONMENT property overrides the job
    # environment, and without it a gate-level `MOBILEGL_TRANSPORT=inproc` export would quietly
    # turn this block into a fourth copy of the split lane - the one failure mode a lane whose
    # whole purpose is to be the OTHER arm cannot survive.
    #
    # The scenario is registered ambiently as well (its source is in the list above), so
    # `DirectGLES.MonolithAttachmentClearScenario.*` and `DirectVulkan.…` still exist in
    # integration-gpu; this block is the copy the gate can see, under a prefix that says which arm
    # it pins.
    mgl_itest_join_environment(MGL_ITEST_GLES_PUSH_MONOLITH_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=monolith"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.PushMonolithArm."
        TEST_LIST MGL_PUSH_MONOLITH_ATTACHMENT_TESTS
        TEST_FILTER "MonolithAttachmentClearScenario.*"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-split"
            TIMEOUT ${MGL_ITEST_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_PUSH_MONOLITH_ENVIRONMENT}"
    )

    # ---- P5b d1: the indexed / instanced / multi-draw / indirect draw family under inproc ----
    #
    # One block per scenario, the precedent above. IndexedDrawFamilyScenario is d1's own lane
    # case: every case's picture depends on a d1 draw (an element-buffer glDrawElements, a
    # client index array, a base vertex, the Minecraft trace's DrawElementsInstancedBaseVertex,
    # the two multi-draws, an indirect draw), so a record that did not cross is a wrong picture
    # and not a green. MultiDrawScenario, DrawParametersScenario and PrimitiveRestartScenario are
    # the census's own DrawElements / MultiDrawElementsBaseVertex / DrawArraysInstancedBaseInstance
    # / MultiDraw*Indirect* first-blocker scenarios, now at their pictures. The ONE case excluded
    # by filter is MultiDrawScenario's ClientSideIndicesBatchMatchesUnrolledDraws: a multi-draw
    # with client-side indices is refused BY NAME under split (Fatal{UnmigratedVerb,
    # "MultiDrawElementsBaseVertex+CLIENT_INDICES"}, CONTRACT-P5B.md d1 - P8's HostResolve.cpp
    # flattens it), which is its next first blocker and not a lane failure. It is listed in
    # d1-v1.md, not deleted (G14). GuiBatchScenario (the census's other DrawElements scenario) is
    # NOT registered: past its draws it stops at Fatal{UnmigratedVerb, "MemoryBarrier"}, which is
    # i1's to flip; i1 registers it when that lands.
    set(MGL_SPLIT_D1_SCENARIOS IndexedDrawFamilyScenario MultiDrawScenario DrawParametersScenario
                               PrimitiveRestartScenario)
    set(MGL_SPLIT_D1_FILTER_IndexedDrawFamilyScenario "IndexedDrawFamilyScenario.*")
    set(MGL_SPLIT_D1_FILTER_MultiDrawScenario
        "MultiDrawScenario.*-MultiDrawScenario.ClientSideIndicesBatchMatchesUnrolledDraws")
    set(MGL_SPLIT_D1_FILTER_DrawParametersScenario "DrawParametersScenario.*")
    set(MGL_SPLIT_D1_FILTER_PrimitiveRestartScenario "PrimitiveRestartScenario.*")
    foreach(mglItestD1Scenario IN LISTS MGL_SPLIT_D1_SCENARIOS)
        mgl_itest_register_split_arms("${MGL_SPLIT_D1_FILTER_${mglItestD1Scenario}}" "")
    endforeach()

    # ---- P5e mv: THE MULTI-DRAW TIERS, INSIDE THE GATED SET (ID-109's rule, one level down) --
    #
    # WHY FIVE MORE LANES FOR ONE FILE. MultiDrawImpl asks the bound index buffer three questions
    # and P5e (mv) moved all three onto the applier's record. Five of this driver's six tiers ask
    # them from a DIFFERENT place in the batch - `RunIndirect` before any GL work, the rebased
    # tier from the CPU shadow, `FlattenWithCompute` before PrepareForDraw has even run - and the
    # resolved tier is ONE process-wide read of MOBILEGL_ESPRYT_MULTIDRAW_MODE (MultiDraw.cpp's
    # ResolveTierOnce). The d1 block above leaves it at `auto`, and on every driver this gate has
    # run against, `auto` lands on `ext` - a real multi-draw entry point that asks the index
    # buffer nothing at all. Three of the five migrated sites therefore had no gated entry.
    #
    # That is ID-107's shape exactly one level down: code on a real arm, with no entry in the set
    # the gate runs. The phase already paid 137 SEGFAULTs across 38 scenarios for the version of
    # this that lived at the transport level, and the lesson ID-109 drew - "the gate runs the SET,
    # so put the second arm in the set" - does not stop being true because the selector is an env
    # knob rather than MG_Config::Transport. A manual sweep run once by one package decays to
    # nothing by the next one.
    #
    # MOBILEGL_ESPRYT_MULTIDRAW_MODE IS A ctest ENVIRONMENT PROPERTY, not a job-level export, for
    # review finding N-6's reason and the PushMonolithArm block's: the property overrides the job
    # environment, so a gate that exported a mode of its own could not silently collapse these
    # five lanes into five more copies of the `auto` one - the single failure mode a lane whose
    # whole purpose is to pin a tier cannot survive.
    #
    # `ext` IS NOT REGISTERED HERE, deliberately: it is what `auto` already resolves to, so the
    # d1 block above IS the ext lane and a sixth block would only run it twice. Every other tier
    # was confirmed to resolve to ITSELF on this driver rather than falling back
    # (`ResolveTier`'s "MOBILEGL_ESPRYT_MULTIDRAW_MODE=<x> -> <x> (driver supports: ext,
    # basevertex, multiindirect, indirect, drawelements, compute (opt-in))"); a tier the driver
    # refuses would resolve to `ext` and register an entry that quietly proves nothing, so if this
    # list ever runs somewhere poorer, drop the refused tier rather than keeping a green that
    # means "ext again".
    #
    # THE FILTER IS THE TIER-SENSITIVE WORKLOAD and nothing else, which is this file's own rule
    # for an armed Split entry: MultiDrawScenario (its whole point is that a batch matches the
    # unrolled draws, which is precisely what a wrong index buffer breaks quietly) and
    # IndexedDrawFamilyScenario's three MultiDraw cases. The client-side-indices case keeps the
    # d1 block's exclusion - it is refused by name under split before any tier is chosen.
    #
    # NOTE FOR THE STRICT-MARKER CENSUS (PREAMBLE-ADDENDUM-WAVE3 §4): these entries carry a THIRD
    # name segment, so `strict_census2.sh` re-derives their gtest filter from the last two
    # components and re-runs them WITHOUT the mode this block pins - which for these lanes is the
    # whole point of the entry. They join Ct. / F1. / SmallRing. / PersistentMapArm. / NamedBlit.
    # in that blind spot, so SplitLogPaths.cmake.in gives each of them a private log path: read
    # the marker out of `build-split/MobileGL/MG_IntegrationTest/split-logs/<entry>.log`, which
    # the LANE wrote under its own pin, instead of out of a re-run that lost it.
    set(MGL_SPLIT_MULTIDRAW_TIERS basevertex multiindirect indirect drawelements compute)
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_basevertex "BaseVertex")
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_multiindirect "MultiIndirect")
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_indirect "Indirect")
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_drawelements "DrawElements")
    set(MGL_SPLIT_MULTIDRAW_TIER_PREFIX_compute "Compute")
    set(MGL_SPLIT_MULTIDRAW_TIER_FILTER
        "MultiDrawScenario.*:IndexedDrawFamilyScenario.MultiDraw*-MultiDrawScenario.ClientSideIndicesBatchMatchesUnrolledDraws")
    foreach(mglItestMultiDrawTier IN LISTS MGL_SPLIT_MULTIDRAW_TIERS)
        mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_MULTIDRAW_ENVIRONMENT
            "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc"
            "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
            "MOBILEGL_ESPRYT_MULTIDRAW_MODE=${mglItestMultiDrawTier}"
            ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX
                "DirectGLES.Split.MultiDrawTier${MGL_SPLIT_MULTIDRAW_TIER_PREFIX_${mglItestMultiDrawTier}}."
            TEST_LIST "MGL_SPLIT_MULTIDRAW_${mglItestMultiDrawTier}_TESTS"
            TEST_FILTER "${MGL_SPLIT_MULTIDRAW_TIER_FILTER}"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-gpu\;integration-split"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                ENVIRONMENT "${MGL_ITEST_GLES_SPLIT_MULTIDRAW_ENVIRONMENT}"
        )
    endforeach()

    # ---- P5b package i1 (MG_Remote/CONTRACT-P5B.md §2 i1) --------------------------------
    #
    # THE LANE CASE THAT PROVES THE RECORDS CROSSED, and it is a RESULT and not a probe (R-16:
    # a probe may not arm against a stub). Each of these four scenarios asserts a value that
    # only exists if the verb ran on the apply thread - the texel a compute shader stored
    # through an image unit, the word glCopyImageSubData moved, the counter an atomic add left
    # behind, the block a program pipeline rebound - so a sink that DECLINED, or a client that
    # fell through to the driver, is red by the number it reads back and not by a tally.
    #
    # Before i1 every one of these aborted at the client's Fatal{UnmigratedVerb} on the first
    # glBindImageTexture / glDispatchCompute / glCopyImageSubData / glShaderStorageBlockBinding;
    # all four are green under inproc now (the census: 13 + 6 + 11 + 3 DirectGLES entries).
    # RED ONCE BY DOING X: return false from ServerVerbSink::OnLaunchGrid before the
    # gl.DispatchCompute call and UnboundImageDescriptor / AtomicCounter / ProgramPipeline all
    # read back their initial values.
    #
    # THE FILTERS ARE NARROWED TO THE CASES THAT ACTUALLY EMIT, and that is ScenarioFixture's
    # rule rather than a convenience: an armed `DirectGLES.Split.` case must move the client
    # encoder's record ordinal (ScenarioFixture.h:86), because a lane whose workload produced no
    # record is the "resolved the transport and then fell through to the driver" shape that
    # every pixel assertion is blind to. ProgramPipelineScenario's other nine cases are pure
    # name/state cases that draw nothing, so the whole-scenario filter would arm them and they
    # would be red for having nothing to say. The two named here are the ones that were
    # Fatal{UnmigratedVerb, "ShaderStorageBlockBinding"} on the c0b head.
    #
    # One block per scenario, following the three above: a `:`-separated multi-pattern
    # TEST_FILTER is unproven through gtest_discover_tests' flat PROPERTIES forwarding.
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

    # ---- P5b t2: the transform-feedback spans, the XFB object bind, the patch parameter -------
    #
    # MG_Remote/CONTRACT-P5B.md §2 t2. The three P5 lanes above are the REDUCED PATH's targets
    # (a clear, a triangle, a persistent map); these two are the first lane entries whose GREEN
    # DEPENDS ON A t2 RECORD HAVING CROSSED AND BEEN APPLIED BY THE BACKEND, which is the only
    # statement a package flipping a verb can make that a unit case cannot:
    #
    #   TessellationXfbCaptureScenario  covers BOTH halves of t2 in one workload. Every case
    #       calls glPatchParameteri(GL_PATCH_VERTICES, n) and then draws GL_PATCHES into a
    #       transform-feedback capture, and asserts the CAPTURED BYTES. A patch_parameter (73)
    #       that did not reach the server tessellates at the previous patch size and the capture
    #       is the wrong length; a begin/end_stream_output (62/63) that did not reach it leaves
    #       the buffer holding the scenario's poison value, which is what those cases print.
    #   XfbCaptureBufferReuseScenario   is the span family alone, across four buffer lifetimes
    #       (a buffer per span, one immutable-storage buffer, one respecified buffer, a
    #       respecification that changes the capture size). It is the case that would notice a
    #       span whose END crossed but whose BEGIN did not, because the second span's bytes
    #       would be the first span's.
    #
    # Both are DirectGLES only, deliberately, and the reason is measured rather than assumed:
    # under Magma the capture is written into the server's resident slice and there is no route
    # back, because MG_Backend/Init.cpp's ConsumedSubsystemsFor(DirectVulkan) withholds
    # kMGPipeSubsystemResources, so BufferObject::SyncGpuWrites() emits no resource_readback.
    # Every DirectVulkan XFB capture entry in the inproc census fails on exactly that (t2-v1.md
    # §"the 51"), and it retires with P7 (Magma's resource family) / P9 (the readback carrier) -
    # not here. A DirectVulkan arm of this lane would be red for a reason t2 cannot fix.
    mgl_itest_register_split_arms("TessellationXfbCaptureScenario.*" "")
    mgl_itest_register_split_arms("XfbCaptureBufferReuseScenario.*" "")

    # ---- P5e package vi (MG_Remote/CONTRACT-P5E.md §5.1, ID-82 / ruling 2) --------------------
    #
    # Client-memory vertices and indices now cross as owned ordinary buffers.
    # Keep the original names and private logs; all six scenarios join the full
    # GPU and split acceptance labels as well as their focused regression label.
    mgl_itest_register_split_arms("ClientVertexArrayScenario.*" "" integration-clientarrays)

    mgl_itest_register_split_arms("SyncWireScenario.*" "")

    # =========================================================================================
    # P3b/P4b WAVE 2-D PACKAGE D2 - OWN BLOCK. Package D1 (XFB / readback) registers its arms in
    # its own block elsewhere in this file; keep the two apart so the integrator can merge both.
    # =========================================================================================
    #
    # THE OWNER-KEYED EMISSION CURSOR, under all three transports. The claim is about WHICH
    # emission cursor an upload through a texture VIEW lands on, and it is only interesting where
    # the record actually crosses a wire: under monolith the drain's SubDataCount still moves but
    # nothing downstream can disagree with the client's own dirty model, because there is one
    # address space and one MipmapStorage. Under split the applier holds its own per-record
    # pending-upload set, so a cursor that cleared the wrong flag produces a server-side level
    # that never receives the texels - which is the failure this registration exists to reach.
    #
    # Every case draws and reads pixels, so each moves the client encoder's record ordinal and
    # satisfies ScenarioFixture's armed-lane rule. The three GLES arm environments carry
    # MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 (above); without it every case green-skips through
    # TextureViewUsable() rather than failing - which is what the Tcp arm did until the fixture
    # entry below got the same knob.
    #
    # ALL FOUR CASES RUN ON ALL THREE ARMS. TheOwnerUploadDrainRunsAgainAfterADrawBoundary was
    # EXCLUDED BY NAME here by package D2, which found the defect but could not fix it inside its
    # own footprint:
    #
    #   after a view has been SAMPLED once, a later glTexSubImage through the OWNER's own name
    #   never becomes visible through the view.
    #
    # Package D3 fixed it and this is its red-once (rule J): the case is red on Split, Spawn AND
    # Tcp at D2's head a7c06f4f with this exclusion lifted and nothing else changed, and green on
    # all three with the fix. The client half was never at fault - the case's own counter
    # assertions pass on the red run, so the records were emitted and the applier accepted them.
    # The server's per-texture clean gate BackendTextureObject::IsDrawSyncCleanByRecord
    # (MG_Backend/DirectGLES/Managers.cpp) compared the VIEW's record.Serial and
    # record.PendingUploads, while an owner-side resource_subdata moves only the OWNER's
    # (MG_Pipe/PipeApply.cpp ApplyTextureUpload). The gate now RESOLVES THROUGH Desc.ViewOf to the
    # storage owner's record for those two clauses, which is the split equivalent of the monolith
    # gate's GetContentVersion() forwarding (TextureObjectView.cpp:100-102). CONTRACT-P5E.md §5.2
    # states the rule the code now implements.
    mgl_itest_register_split_arms("TextureViewAliasScenario.*" "")

    # ---- D2: THE UPLOAD-SHAPE GOLD, UNDER ALL THREE TRANSPORTS ------------------------------
    #
    # WRITTEN OUT PER ARM RATHER THAN THROUGH mgl_itest_register_split_arms, and the reason is
    # structural rather than stylistic: the macro hands every arm the SHARED
    # MGL_ITEST_GLES_<ARM>_ENVIRONMENT, and this case needs four things no shared list carries -
    # MGITEST_TEXTURE_UPLOAD_SHAPE_LANE=1 (without it the case skips), MOBILEGL_PIPE_STATS=1 with
    # PERIOD=1, MOBILEGL_PIPE_PUSH=0x1fff, and a MOBILEGL_LOG_FILE_PATH PRIVATE TO THE ARM. That
    # last one is not negotiable: the library opens the log fopen(path, "w"), so two entries
    # sharing a path truncate each other under `ctest -j` and the failure looks exactly like "the
    # counter was never emitted". The PersistentMapArm pair above is the same shape for the same
    # reason. A macro call here would produce three entries that all green-skip.
    #
    # WHAT THE SPLIT ARMS ADD OVER THE MONOLITH LANE. The monolith lane reads one log, because
    # there is one role. Under SPAWN and TCP the client and the server are two PROCESSES with two
    # independent counter sets - the client's line carries ctu= and the server's carries tex[] -
    # and the `ctu == emit` comparison the whole scenario exists for is only a real comparison
    # there. Under inproc it is one process with two log FILES, which is the arm that proves the
    # role-derived path reading without needing two counter sets.
    set(MGL_D2_TUS_FILTER "TextureUploadShapeScenario.TheEmittedUploadShapeIsRecordedAndTheTwoSidesAgree")
    foreach(mglD2Arm Split Spawn Tcp)
        string(TOLOWER "${mglD2Arm}" mglD2ArmLower)
        set(mglD2Log "${CMAKE_CURRENT_BINARY_DIR}/texture-upload-shape-${mglD2ArmLower}-DirectGLES.log")
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
        endif()
        mgl_itest_join_environment(MGL_D2_TUS_ENV_${mglD2Arm}
            "MOBILEGL_BACKEND_TYPE=DirectGLES" ${mglD2Transport} "MGITEST_SPLIT_LANE=1"
            "MGITEST_TEXTURE_UPLOAD_SHAPE_LANE=1" "MOBILEGL_PIPE_PUSH=0x1fff"
            "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
            "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
            "MOBILEGL_LOG_FILE_PATH=${mglD2Log}"
            ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
        get_filename_component(mglD2LogName "${mglD2Log}" NAME)
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectGLES.${mglD2Arm}.TextureUploadShape."
            TEST_FILTER "${MGL_D2_TUS_FILTER}"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-gpu\;integration-${mglD2ArmLower}"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                RESOURCE_LOCK "${mglD2LogName}"
                ${mglD2Extra}
                ENVIRONMENT "${MGL_D2_TUS_ENV_${mglD2Arm}}"
        )
    endforeach()

    # ---- D2: THE TEXTURE / PROGRAM SPLIT-SCENARIO CENSUS ------------------------------------
    #
    # PLAN-PH-P34B-P7.md 1.2, "门：纹理 / program 场景 split 覆盖". Each candidate was run once
    # under MOBILEGL_TRANSPORT=inproc on build-split and its FIRST BLOCKER recorded; the row per
    # scenario x backend x blocker is in notes/p34b/split-scenario-census.md. What follows is the
    # result of that census, not a guess: every scenario listed here passed, and every case that
    # did not is excluded BY NAME below with its blocker.
    #
    # D1 owns PixelStoreSweep, DepthStencilReadbackMatrix, PackedWordReadback and
    # LayeredTextureReadback - they are not here.
    #
    # DIRECTVULKAN ARMS DEFAULT TO NONE, and that is not an omission. The macro records every
    # call and the informational tier-2 block at the end of this file replays all of them on
    # DirectVulkan under integration-magma-all-{split,spawn,tcp} - so these scenarios DO get a
    # Magma reading, just not a gating one. Gating them there would be asserting P7 wave 2's
    # exit gate 1 from inside a P3b/P4b package, on a backend whose own @P7 named refusals are
    # still being retired in waves A/B/C.
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

    # ProgramPipelineScenario BEYOND *StorageBlock*: the i1 block above already registers the two
    # storage-block cases. The other nine are pure name/state cases that draw nothing, and
    # ScenarioFixture's armed-lane rule (the client encoder's record ordinal must move) makes a
    # case that produced no record RED for having nothing to say - which is the rule working, not
    # a defect. They stay unregistered for that reason and are listed in the census note.

    # The split half of the counting pair. MGITEST_PERSISTENT_MAP_ARM=emulated is R-6: under split
    # the adopt tier is pinned at T2, the resource owner declines every acquisition and the client
    # pushes the mapping's dirty blocks - so pmap must be non-zero and mpr must be the monolith
    # lane's number. pmap == 0 here means MGPipeApplyMapPersistent handed back a pointer, which is
    # exit gate E3(d) failing, and under inproc that failure is invisible in pixels.
    mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_PMAP_ARM_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MGITEST_PMAP_LANE=1" "MGITEST_PERSISTENT_MAP_ARM=emulated"
        "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/persistent-map-arm-split-DirectGLES.log"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.Split.PersistentMapArm."
        TEST_FILTER "PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-split"
            TIMEOUT ${MGL_ITEST_TIMEOUT}
            RESOURCE_LOCK persistent-map-arm-split-DirectGLES.log
            ENVIRONMENT "${MGL_ITEST_GLES_SPLIT_PMAP_ARM_ENVIRONMENT}"
    )

    # --- EXIT GATE E3(e): the same checks with a ring small enough to force back-pressure -----
    #
    # BRIEF 7 E3(e) - "以上五条再跑一遍 ring 小到足以至少发生一次背压等待的配置" - was missing from
    # the first cut of this package and missing from its report (review finding M-7). It is a
    # LANE, not a new scenario: the same three split scenarios with SEG_CMD and SEG_STAGE at their
    # floor, so a workload that fits comfortably in the 8 MiB / 32 MiB defaults has to wrap and
    # wait at least once.
    #
    # 1 MiB is ConfigLoader's minimum for both (Config.h / ConfigLoader.cpp:363-364, floors of 1
    # rather than 0 because a ring caps ONE record at half its size). That caps a record at 512
    # KiB, which every record on the reduced path is far under, so the ring is legal and small
    # rather than unusable.
    #
    # WHAT THIS LANE DOES NOT YET ASSERT, stated rather than implied: that a back-pressure wait
    # ACTUALLY happened. That needs a counter only package s1 can publish (a producer-parked or
    # ring-full tally on RingControl); until it exists this lane proves the five checks survive a
    # small ring, not that the small ring bit. The gap is named in t1-v1.md's debts rather than
    # left for someone to discover from a green.
    mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_SMALL_RING_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MGITEST_SMALL_RING_LANE=1" "MOBILEGL_IPC_RING_MB=1" "MOBILEGL_IPC_STAGE_MB=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    set(MGL_SPLIT_SMALL_RING_SCENARIOS ClearThenReadPixelsScenario TriangleScenario
                                      PersistentCoherentMapScenario)
    foreach(mglItestSmallRingScenario IN LISTS MGL_SPLIT_SMALL_RING_SCENARIOS)
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectGLES.Split.SmallRing."
            TEST_LIST "MGL_SPLIT_SMALL_${mglItestSmallRingScenario}_TESTS"
            TEST_FILTER "${mglItestSmallRingScenario}.*"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-gpu\;integration-split"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                ENVIRONMENT "${MGL_ITEST_GLES_SPLIT_SMALL_RING_ENVIRONMENT}"
        )
    endforeach()

    # ---- P5e package pa: THE STRICT LANE GETS ENTRIES OF ITS OWN ------------------------------
    #
    # pa retired the two per-draw program rows - Fatal{UnmigratedPipeInput,
    # "GetProgramForDraw@DrawArrays"} and "...GetProgramForDispatch@DispatchCompute", 69 of the
    # 109 entries the strict lane was red on. NOTHING GATED THEM. MOBILEGL_IPC_STRICT_ERRORS=1 is
    # a knob the phase's gate driver sets by hand, no REGISTERED entry exports it, and package
    # pg's declared red-once for this exact marker never landed - so the row could come back and
    # every gate would stay green. That is ID-109's finding in miniature: the gap was never a
    # missing test case, it was a missing gated ENTRY for a whole reading.
    #
    # WHICH SCENARIOS MAY JOIN is decided by the knob itself: under it every BARRIER-PULLED read
    # is Fatal, and most split scenarios end in a readback, whose GetFramebufferBindingSlot@ReadPixels
    # is an allowlisted row that only P7 retires. So a scenario is eligible exactly when it
    # reaches PrepareForDraw / PrepareForCompute and then does NOT read pixels back. Two do, one
    # per half of pa, and both were red with pa's own marker before it landed:
    #   * MultiDrawScenario.BaseVertexDrawsRejectMalformedArguments - the DRAW half; it prepares a
    #     draw and then asserts on GL errors rather than on pixels.
    #   * AtomicCounterScenario.* - the DISPATCH half, and all three cases were
    #     GetProgramForDispatch@DispatchCompute entries.
    #
    # LABELS IS integration-split ALONE, deliberately, and it is not the hole ID-109 names. That
    # hole was a runtime ARM with no entry anywhere; here the monolith arm of these same scenarios
    # is registered under DirectGLES. already and this knob is a no-op on it - a monolith client
    # fills and stamps all 63 fields at every verb, so no read is unfresh and nothing can trip.
    # An entry that cannot fail for the reason it exists is worse than none (R-16), so this lane
    # names the arm it is for in its own prefix. The F1. lane sets the same precedent.
    mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_STRICT_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_IPC_STRICT_ERRORS=1"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.Split.StrictProgramArm."
        TEST_LIST MGL_SPLIT_PA_STRICT_DRAW_TESTS
        TEST_FILTER "MultiDrawScenario.BaseVertexDrawsRejectMalformedArguments"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-split"
            TIMEOUT ${MGL_ITEST_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_SPLIT_STRICT_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.Split.StrictProgramArm."
        TEST_LIST MGL_SPLIT_PA_STRICT_DISPATCH_TESTS
        TEST_FILTER "AtomicCounterScenario.*"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-split"
            TIMEOUT ${MGL_ITEST_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_SPLIT_STRICT_ENVIRONMENT}"
    )
    # The configure_file that used to sit here MOVED TO THE END OF THIS FILE (P6 `t6`).
    #
    # It substitutes @MGL_SPLIT_ARM_TEST_LISTS@, which the registration macro appends to - so
    # running it here captured only the calls made ABOVE this line and silently dropped the F1
    # and Ct blocks below it. SplitLogPaths.PrivateAndDistinct is what caught that, naming
    # F1WireScenario.ClearBufferfiPixels; the generated file now sees every call.
    # Python is mandatory for this split integration gate (also used by the CI controls).
    # Do not silently omit the ownership check when the interpreter is unavailable.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME SplitLogPaths.PrivateAndDistinct
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Harness/split_log_paths.py"
            check "${CMAKE_CTEST_COMMAND}" "${CMAKE_BINARY_DIR}")
    set_tests_properties(SplitLogPaths.PrivateAndDistinct PROPERTIES LABELS "integration-split")
endif()

# f1: split-only source keeps the monolith registry unchanged.
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_sources(MobileGLIntegrationTest PRIVATE Scenarios/F1WireScenario.cpp)
    foreach(f1Slot ClearBufferfi ClearBufferfv ClearBufferiv ClearBufferuiv ClearNamedFramebufferfi ClearNamedFramebufferfv ClearNamedFramebufferiv ClearNamedFramebufferuiv CopyTexImage2D CopyTexSubImage2D GenerateMipmap GenerateMipmapPackedFloat GenerateMipmapDepth)
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
    # P5f fm: the framebuffer/copy/mipmap verbs now consume server records on Magma too.
    #
    # P7 package L: THE SAME CASES ON ALL THREE TRANSPORTS. The flavour is everything this lane
    # is except the transport, which the arm now supplies - so the inproc arm is byte-for-byte
    # the lane P6.5 left green (same names, same labels, same log paths) and the two new arms
    # differ from it in exactly one dimension. That is what makes a red here readable: it is a
    # TRANSPORT fact, because nothing else changed.
    #
    # ${MGL_ITEST_VULKAN_ENV} AND NOT ${MGL_ITEST_COMMON_ENV} (P7 package L, integrator ruling).
    # Until this ruling the six Magma lanes appended the COMMON list, so the monolith
    # `DirectVulkan.` arm ran with VK_ICD_FILENAMES pinned and the three lavapipe iterationRP
    # repairs forced on while these 73 entries ran against whatever ICD the loader happened to
    # find first, with the repairs at their device auto-detection default. That is not a
    # semantic difference between two lanes, it is a latent flake: the day a runner or a
    # workstation grows a second ICD, this lane measures a different driver from the gate it
    # mirrors and nothing in its output says so. Consistency over "meaning unchanged" - and the
    # re-run after the change is part of the ruling: integration-magma-split stayed 71/0/2.
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
        foreach(fmSlot ComputeImageStoreThroughViewPreservesOtherRootLayer EnabledVertexBufferDrawKeepsNamedP7Fatal SrgbDrawTracksFramebufferConversion Texture1DFramebufferClearPixels VertexIdSamplerAndScalarUniformPixels ComputeImageStoreFramebufferPixels PartialTextureUploadPreservesGpuClearPixels TextureViewClearTargetsItsRootMipAndLayer ClearBufferfiPixels ClearBufferfvPixels ClearBufferivPixels ClearBufferuivPixels ClearNamedFramebufferfiPixels ClearNamedFramebufferfvPixels ClearNamedFramebufferivPixels ClearNamedFramebufferuivPixels CopyTexImage2DPixels CopyTexSubImage2DPixels GenerateMipmapPixels GenerateMipmapPackedFloatPixels GenerateMipmapDepthPixels)
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.Fm."
                TEST_FILTER "F1WireScenario.${fmSlot}"
                DISCOVERY_TIMEOUT 30
                PROPERTIES
                    LABELS "${mglItestMagmaLabels}"
                    TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5f-fm${mglItestArmLogTag}-${fmSlot}.log")
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
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.Fm."
                TEST_FILTER "${fmCase}"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5f-fm${mglItestArmLogTag}-${fmCase}.log")
        endforeach()
        # ---- P7 wave 2-B: THE TWO SHADER MIP ARMS, FORCED ------------------------------------
        #
        # WHY A KNOB AND NOT A FORMAT. The refusal these two entries cover
        # (`Magma:mipmap-shader-format-or-shape`) killed two Iris cases on an Adreno 830v2 and
        # has never once fired on this host, because lavapipe reports BLIT_SRC|BLIT_DST for
        # every colour format the integration binary can create - so GenerateWireMipmap takes
        # its NATIVE arm for all of them and neither shader arm is reachable by choosing a
        # format. `GenerateMipmapPackedFloatPixels` next door looks like it covers the shader
        # arm and does not: R11F_G11F_B10F blits natively here too.
        # MGITEST_MAGMA_FORCE_SHADER_MIPMAP drops that arm instead, one tier per shape, so the
        # host lane exercises the code the device actually runs.
        #
        # ITS OWN ENTRIES RATHER THAN A LANE-WIDE FLIP, for the reason the strict-arming entry
        # gives: the knob changes how EVERY mipmap in the process is generated, so putting it in
        # the shared Fm environment would silently retarget GenerateMipmapPixels,
        # GenerateMipmapPackedFloatPixels, GenerateMipmapDepthPixels and the two base/view cases
        # as well - four green entries that would stop measuring the arm they were written for.
        # ---- P7 wave 2-B2: A SERVER-SIDE KNOB HAS NO TCP ARM, AND THAT IS STRUCTURAL ---------
        #
        # Every MGITEST_MAGMA_FORCE_* knob in this block is read BY THE SERVER
        # (WireFramebuffer.inc's three `std::getenv` readers run on the apply thread). A ctest
        # ENVIRONMENT property reaches the process ctest starts - the CLIENT - and the two arms
        # differ in where the server comes from:
        #   inproc  the server is a thread of that same process, so it sees the knob;
        #   spawn   ServerSpawn.cpp copies ::environ into the child, so it sees the knob;
        #   tcp     the server is the LANE-WIDE TcpServer.Start fixture, started once from the
        #           fixture's own os.environ (scripts/ci/tcp_server_fixture.py) long before any
        #           case runs, and it forks a session child per Hello. Nothing a per-test
        #           ENVIRONMENT says can reach it.
        #
        # So a tcp copy of a knob entry does not exercise the arm it names - it runs the
        # DEFAULT arm under a name that claims otherwise, which for ShaderMip is precisely what
        # that case's own skip message says must not happen ("this case would assert the native
        # blit twice over"). MEASURED, not deduced: a temporary fatal in each of the three
        # readers killed all five knob=1 entries on split and on spawn and left ALL FIVE GREEN
        # on tcp.
        #
        # Registering them on split+spawn only is therefore a coverage statement, not a gap, and
        # it is NAMED in scripts/ci/spawn_lane_parity.py (MAGMA_SERVER_ENV_KNOB_NO_TCP) the way
        # MAGMA_INPROC_ONLY and MONOLITH_ONLY are, so the parity gate keeps its teeth for
        # everything else. The knob=0 twins (DefaultBlitShape0, MsResolve0) DO stay on tcp: zero
        # is the default, so those entries mean the same thing on all three arms.
        #
        # Making the tcp lane able to carry these would need the fixture to take a per-case
        # server environment - a supervisor control, not a ctest property - and that is a debt,
        # recorded in notes/p7/magma-b2.md §6.
        set(mglItestServerEnvKnobArm TRUE)
        if(mglItestArm STREQUAL "Tcp")
            set(mglItestServerEnvKnobArm FALSE)
        endif()
        foreach(shaderMipTier 1 2)
            if(NOT mglItestServerEnvKnobArm)
                continue()
            endif()
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.ShaderMip${shaderMipTier}."
                TEST_FILTER "F1WireScenario.GenerateMipmapWithoutNativeBlitPixels"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_SHADER_MIPMAP=${shaderMipTier}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-shadermip${mglItestArmLogTag}-tier${shaderMipTier}.log")
        endforeach()
        # The DEPTH half of the same knob (CONTRACT-P7 §5.1 (A)). ONE TIER, not two: the depth
        # pass renders into a depth ATTACHMENT view of the destination level, which a depth
        # image always has usage for - there is no "cannot be attached" shape to force, so
        # tier 2 would register a second copy of tier 1's run. Split+spawn only, for the
        # server-side-knob reason above.
        if(mglItestServerEnvKnobArm)
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectVulkan.${mglItestArm}.DepthMip."
            TEST_FILTER "F1WireScenario.GenerateMipmapDepthWithoutNativeBlitPixels"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                ${mglItestArmProperties}
                ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_SHADER_MIPMAP=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-depthmip${mglItestArmLogTag}.log")
        endif()
        # NO KNOB on the decline: it is decided on the aspect before the blit-support question
        # is asked, so the plain Fm environment reaches it on every driver.
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectVulkan.${mglItestArm}.Fm."
            TEST_FILTER "F1WireScenario.GenerateMipmapDepthStencilDeclinesAndKeepsTheSession"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                ${mglItestArmProperties}
                ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-dsmipdecline${mglItestArmLogTag}.log")
        # `copy-image-in-place@P7`'s retirement and its one surviving decline. SPLIT-ONLY in the
        # strongest sense: the monolith arm declines every in-place copy, so there is no
        # DirectVulkan. entry to register these beside - the wire arm is the one that performs
        # them, which is what §3.2 asks for.
        # ---- P7 wave 2-B, EXIT GATE 3: the readback must not depend on how long a pipeline
        # takes to create. --------------------------------------------------------------------
        #
        # The Redmi's OpenRA divergence reproduces only with a COLD driver pipeline cache (three
        # cleared-cache runs: a bit-identical ssim 0.976494 / 14658 mismatched pixels; warm:
        # 1.000000 from the second run on). What a cold cache changes is how long
        # vkCreateGraphicsPipelines takes on the apply thread, and
        # MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS buys that time on a host whose cache is warm.
        #
        # THESE ENTRIES ARE A REGRESSION GATE, NOT THE REPRODUCTION. The delay alone does NOT
        # make lavapipe diverge, and the reason is in WireFramebuffer.inc's
        # RecordWireReadbackWriteBarrier: the host's default framebuffer is tracked as
        # VK_IMAGE_LAYOUT_GENERAL at the readback, which takes GetImageTransitionSourceState's
        # `default:` arm (ALL_COMMANDS / MEMORY_WRITE) and orders the copy by accident. What these
        # entries do pin is that a readback which follows a draw still returns that draw's pixels
        # when every pipeline creation on the path costs 25 ms - i.e. that the answer does not
        # move with timing.
        foreach(pipelineDelayCase VertexIdSamplerAndScalarUniformPixels
                                  ComputeImageStoreFramebufferPixels
                                  GenerateMipmapPixels)
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.PipelineDelay."
                TEST_FILTER "F1WireScenario.${pipelineDelayCase}"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS=25\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-pipelinedelay${mglItestArmLogTag}-${pipelineDelayCase}.log")
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
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectVulkan.${mglItestArm}.Fm."
            TEST_FILTER "F1WireScenario.StreamedBufferSubDataBeforeEachDrawIsOrdered"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                ${mglItestArmProperties}
                ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-streamedsubdata${mglItestArmLogTag}.log")
        # SPLIT AND SPAWN ONLY - a parity exception, named in scripts/ci/spawn_lane_parity.py
        # (MAGMA_SERVER_ENV_KNOB_NO_TCP, the same list as the B2 knobs above), not an omission.
        # The knob is read by the SERVER, and the tcp arm's server is the lane fixture
        # (scripts/ci/tcp_server_fixture.py) started once from its own environment: an
        # entry-level ENVIRONMENT never reaches it, so a Tcp.StaleSerial. entry would run the
        # plain Fm case under a name that claims the knob, and could never red. The inproc arm
        # hosts the server in the test process and the spawn arm's server inherits the client's
        # environment, so on those two the knob is real.
        if(mglItestServerEnvKnobArm)
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.StaleSerial."
                TEST_FILTER "F1WireScenario.StreamedBufferSubDataBeforeEachDrawIsOrdered"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_STALE_BUFFER_SERIAL=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-streamedsubdata-stale${mglItestArmLogTag}.log")
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
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.DefaultBlitShape${defaultBlitScratch}."
                TEST_FILTER "F1WireScenario.ColorBlitToDefaultFromANonSampleableSourceDegrades"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_DEFAULT_BLIT_SCRATCH=${defaultBlitScratch}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-defaultblit${mglItestArmLogTag}-scratch${defaultBlitScratch}.log")
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
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.MsBlit."
                TEST_FILTER "F1WireScenario.${multisampleBlitCase}"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-msblit${mglItestArmLogTag}-${multisampleBlitCase}.log")
        endforeach()
        # ---- P7 wave 2-B2: `multisample-blit-aspect@P7`'s retirement, BOTH ARMS ---------------
        #
        # THE CASE IS PROMOTED, NOT WRITTEN. DepthStencilReadbackMatrixScenario already owns the
        # observable - clear a 4x multisample DEPTH24_STENCIL8 renderbuffer, resolve it into a
        # single-sampled framebuffer with glBlitFramebuffer, read both aspects back, and check
        # that a depth-only resolve left the destination's stencil alone and vice versa - and it
        # already ran on these three arms in the INFORMATIONAL tier (`…Full.`). What it did not
        # have was a gated entry, which is what CONTRACT-P7 §3.2's aspect row needs.
        #
        # TWO ENTRIES OVER ONE CASE, and the second is the only way its arm is reachable here:
        # lavapipe carries VK_KHR_create_renderpass2 AND VK_KHR_depth_stencil_resolve, so the
        # extension arm answers every resolve on this host and the shader pass - which exists FOR
        # the device that has neither, where the session used to end at
        # `Magma:multisample-depth-resolve-capability` - would never execute.
        # MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE drops the extension arm, and the two entries
        # then assert the same depth values and the same stencil indices, which is the whole
        # claim: sample zero either way.
        #
        # ITS OWN ENTRIES RATHER THAN A LANE-WIDE FLIP, the shader-mip block's reason: the knob
        # changes how EVERY depth/stencil resolve in the process is performed.
        foreach(shaderDepthResolve 0 1)
            # Same split+spawn restriction, same reason, same parity exception: the knob is read
            # on the server and the tcp server is the lane fixture. MsResolve0 stays everywhere.
            if(shaderDepthResolve AND NOT mglItestServerEnvKnobArm)
                continue()
            endif()
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.MsResolve${shaderDepthResolve}."
                TEST_FILTER "DepthStencilReadbackMatrixScenario.AResolvedMultisampleDepthReadsBackFromTheDestination"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE=${shaderDepthResolve}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-msresolve${mglItestArmLogTag}-shader${shaderDepthResolve}.log")
        endforeach()
        # P7 wave 2-B2 (review round): a FLIPPED multisample depth/stencil resolve is a legal blit
        # that used to end the session, and a SCALED one is INVALID_OPERATION that used to end it
        # too. Both shapes are decided on the rectangles, before any capability question, so this
        # entry needs NO knob and runs on all three arms.
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectVulkan.${mglItestArm}.MsFlip."
            TEST_FILTER "DepthStencilReadbackMatrixScenario.AFlippedMultisampleResolveMirrorsTheBandsAndAScaleDeclines"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                ${mglItestArmProperties}
                ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-msflip${mglItestArmLogTag}.log")
        foreach(inPlaceCase CopyImageInPlaceAcrossLevelsAndLayers
                            CopyImageInPlaceOverlapDeclinesAndLeavesTheLevelAlone)
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.CopyInPlace."
                TEST_FILTER "F1WireScenario.${inPlaceCase}"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}" TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${mglItestMagmaWireEnvironment}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p7-copyinplace${mglItestArmLogTag}-${inPlaceCase}.log")
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
            StorageBufferRegrowScenario.AGrownStoreIsVisibleThroughItsExistingIndexedBinding)
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectVulkan.${mglItestArm}.Buffers."
            TEST_FILTER "${bufferCase}"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${mglItestMagmaLabels}"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                ${mglItestArmProperties}
                ENVIRONMENT "${MGL_ITEST_MAGMA_BUFFER_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-buffers${mglItestArmLogTag}-${bufferCase}.log")
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
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "DirectVulkan.${mglItestArm}.RunAhead.Credit${credit}."
                TEST_FILTER "MagmaRunAheadScenario.${runAheadCase}"
                DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS "${mglItestMagmaLabels}"
                    TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${MGL_ITEST_MAGMA_RUNAHEAD_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-runahead${mglItestArmLogTag}-credit${credit}-${runAheadCase}.log")
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
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectVulkan.${mglItestArm}.Caches."
            TEST_FILTER "MagmaWireCacheScenario.${cacheCase}"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${mglItestMagmaLabels}"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                ${mglItestArmProperties}
                ENVIRONMENT "${MGL_ITEST_MAGMA_CACHES_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-caches${mglItestArmLogTag}-${cacheCase}.log")
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
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectVulkan.${mglItestArm}.VertexLayout."
            TEST_FILTER "MagmaVertexLayoutScenario.${vertexLayoutCase}"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${mglItestMagmaLabels}"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                ${mglItestArmProperties}
                ENVIRONMENT "${MGL_ITEST_MAGMA_VERTEXLAYOUT_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/magma-vertexlayout${mglItestArmLogTag}-${vertexLayoutCase}.log")
    endforeach()
    endforeach()
endif()

# P5c ct (MG_Remote/CONTRACT-P5C.md §5): the two control records, split-only source for the
# same reason as f1's - the scenario reads the SERVER sink's tallies, which a monolith build
# has no symbol for. One block per case with a LOG PATH OF ITS OWN, the F1 pattern:
# SplitLogPaths.PrivateAndDistinct fails a lane whose cases share one file, and the death
# case's child truncates whatever path it inherits.
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
            gtest_discover_tests(MobileGLIntegrationTest
                TEST_PREFIX "${namedBlitBackend}.${mglItestArm}.NamedBlit."
                TEST_FILTER "F1WireScenario.${namedBlitCase}"
                DISCOVERY_TIMEOUT 30
                PROPERTIES
                    LABELS "${MGL_ITEST_NAMED_BLIT_LABELS}"
                    TIMEOUT ${MGL_ITEST_TIMEOUT}
                    ${mglItestArmProperties}
                    ENVIRONMENT "${MGL_ITEST_NAMED_BLIT_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5b-${namedBlitBackend}${mglItestArmLogTag}-${namedBlitCase}.log")
        endforeach()
        endforeach()
    endforeach()
endif()

# =====================================================================================
# P5e (gl), ID-115 / ID-119: THE STRICT LANE'S POSITIVE CONTROL, AND THE ONE COUNTING ENTRY
# =====================================================================================
#
# ONE ENTRY, TWO JOBS, AND BOTH NEED THE SAME THING - a drawing split scenario that can read its
# own PipeStats window.
#
#   ID-115: prove strict was ARMED. Of the seven entries that passed the strict lane before this
#   phase, three were monolith transport (where nothing stamps a verb boundary and the whole
#   mechanism is structurally unreachable), two self-skipped, one was a death test and one was a
#   Python check - so "116/116" and "strict never ran on anything that draws" were the same
#   observation. `vbs` on the summary line is the arming proof and this entry is where it is read.
#
#   ID-119: the `rsp` pin. The old wording ("rsp = 0 on unbarriered records") was true of every
#   possible implementation, because CountBarrierPull's unbarriered arm is [[noreturn]] and runs
#   BEFORE the counter. The checkable statement is the relation to `draws`, which is what the
#   device measured (rsp ~= the draw count under inproc, 0 under monolith).
#
# IT IS ITS OWN ENTRY WITH ITS OWN LOG PATH, NOT A WHOLE-LANE ENV FLIP. The library opens
# MOBILEGL_LOG_FILE_PATH with "w", so every process in a lane TRUNCATES it; two entries sharing
# one path under `ctest -j` race into an empty read that looks exactly like "the counter was
# never emitted" (Harness/PipeStatsWindow.h:19-24 has the long version). RESOURCE_LOCK is the
# same belt as the PersistentMapArm entry's.
#
# BLOCK PLACED AT THE END OF THE FILE ON PURPOSE (wave-3 integration): `pa`'s
# DirectGLES.Split.StrictProgramArm. block and `mv`'s multi-draw tiers are added inside the big
# disaggregated section above, so keeping this one textually apart lets all three merge cleanly.
if (MOBILEGL_BUILD_DISAGGREGATED)
    mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_STRICT_ARMING_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MGITEST_STRICT_ARMING_LANE=1"
        "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5e-strict-arming-DirectGLES.log"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})

    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.Split.StrictArming."
        TEST_FILTER "TriangleScenario.TheServerStampedAVerbBoundaryOnThisDrawingFrame"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-gpu\;integration-split"
            TIMEOUT ${MGL_ITEST_TIMEOUT}
            RESOURCE_LOCK p5e-strict-arming-DirectGLES.log
            ENVIRONMENT "${MGL_ITEST_GLES_SPLIT_STRICT_ARMING_ENVIRONMENT}"
    )
endif()

# =====================================================================================
# P5f (f1), P5F-WIRE-COMPLETENESS.md §4/§6: THE DUAL-BLOCK REHEARSAL'S OWN LANE
# =====================================================================================
#
# The lane has TWO halves, and only one of them registers entries here:
#
#   - the CENSUS half is an environment-variable lane, exactly like strict: the gate runs the
#     EXISTING `integration-split` entries under MOBILEGL_IPC_ROLE_SPLIT_STATE=1 +
#     MOBILEGL_IPC_STRICT_ERRORS=1, and every BARRIER-PULLED read there aborts with a named
#     Fatal{UnmigratedPipeInput, "<field>@<verb>"} - the red IS the census, collected and
#     ratcheted against Harness/dualblock-expected-fatals.txt by split_log_paths.py's
#     `expect-fatal` mode. Re-registering the whole set under a second prefix would double the
#     discovered test count for zero new information.
#
#   - the CONTROL half is the one entry below: DualBlockScenario is the mechanism's negative
#     control (§6: turning the knob OFF must turn a named case from green to red), so it needs
#     an entry of its own, under a label of its own.
#
# THE LABEL IS `integration-dualblock-split`, and the spelling is load-bearing twice over
# (ID-131): `ctest -L` is a REGEX, so a label CONTAINING `integration-split` as a substring
# would be selected by the main lane's runs - and this one is expected-red there, which must
# never poison the hard-green gate. Checked both directions: `-L integration-split` does not
# match it (`dualblock` stands between the two words), and `-L integration-dualblock-split`
# matches nothing else in the tree. It does not carry `integration-gpu` either: that label must
# be able to go green, and this entry goes green only under the armed knob.
#
# THE KNOB IS NOT PINNED IN THE ENTRY'S ENVIRONMENT, deliberately, and that is the ONE deviation
# from the N-6 pinning rule every other split entry follows: a ctest ENVIRONMENT property
# overrides the process environment, so pinning MOBILEGL_IPC_ROLE_SPLIT_STATE=1 here would make
# the knob=0 form - which IS the negative control - unreachable through ctest. The failure
# direction is safe: a bare `-L integration-dualblock-split` run without the knob is RED by
# name, never silently green.
#
# MOBILEGL_IPC_RUN_AHEAD=0 IS pinned: the serial-separation half asserts on the fill side's
# behaviour per verb boundary, and under run-ahead an unbarriered verb fills nothing, which
# would make the assertion timing-dependent instead of deterministic.
if (MOBILEGL_BUILD_DISAGGREGATED)
    target_sources(MobileGLIntegrationTest PRIVATE
        Scenarios/DualBlockScenario.cpp Harness/DualBlockPeek.cpp)
    mgl_itest_join_environment(MGL_ITEST_GLES_DUALBLOCK_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_TRANSPORT=inproc"
        "MOBILEGL_IPC_SERVER_PATH=${MGL_ITEST_SPLIT_SERVER_PATH}" "MGITEST_SPLIT_LANE=1"
        "MOBILEGL_IPC_RUN_AHEAD=0"
        ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV})
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.Split.DualBlock."
        TEST_FILTER "DualBlockScenario.*"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-dualblock-split"
            TIMEOUT ${MGL_ITEST_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_DUALBLOCK_ENVIRONMENT}\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/split-logs/DirectGLES.Split.DualBlock.TheTwoRolesHaveDistinctBlocks.log"
    )
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
            "MOBILEGL_BACKEND_TYPE=${backend}" "MGITEST_SPLIT_LANE=1"
            "MGITEST_P5F_RSP_LANE=1" "MOBILEGL_PIPE_STATS=1" "MOBILEGL_PIPE_STATS_PERIOD=1"
            "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/p5f-rsp-${backend}${mglItestArmLogTag}.log"
            ${MGL_ITEST_CAPABILITY_ENV} ${p5f_rsp_base_env} ${mglItestArmTransportEnv})
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "${backend}.${mglItestArm}.P5fRsp."
            TEST_FILTER "F1WireScenario.EachWireFrameHasZeroResidualPulls"
            DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS "${p5f_rsp_label}"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                RESOURCE_LOCK "p5f-rsp-${backend}${mglItestArmLogTag}.log"
                ${mglItestArmProperties}
                ENVIRONMENT "${p5f_rsp_environment}")
        endforeach()
    endforeach()
endif()

# =====================================================================================
# P7 PACKAGE L: THE INFORMATIONAL MAGMA CENSUS - TIER 2 OF TWO
# =====================================================================================
#
# THE TWO TIERS, STATED ONCE AND IN ONE PLACE.
#
#   TIER 1 - GATED. `integration-magma-split` (inproc, and its meaning is unchanged: the same
#   73 entries, names, labels and private log paths P6.5 left green), plus the two arms this
#   package adds beside it, `integration-magma-spawn` and `integration-magma-tcp`. They run the
#   SIX HAND-LAID MAGMA LANES - Fm, Buffers, RunAhead, Caches, NamedBlit, P5fRsp - because each
#   carries a flavour (strict + dual-block state, adopt tier, run-ahead credits, cache pinning)
#   that a shared scenario filter cannot express. A label here may enter a CI gating step only
#   after it has been measured green.
#
#   TIER 2 - INFORMATIONAL, THIS BLOCK. Every scenario the DirectGLES split arms run, on
#   DirectVulkan, on all three transports: `integration-magma-all-split` / `-spawn` / `-tcp`.
#   It is in NO CI gating step and must not be added to one wholesale. Its first run's RED LIST
#   IS THE P7 WORK LIST - the cases wave 2's clusters A (UniformManager alignment), B
#   (blit/copy/mip) and C (vertex / death ops / resident subdata / reflection) divide between
#   them - and a red list you cannot record without breaking the build's green is a red list
#   nobody writes down. As a cluster retires its share, its cases move UP into tier 1 with the
#   commit that retires them, and the gate is honest at every step in between.
#
# WHY IT REPLAYS A RECORDED LIST INSTEAD OF NAMING SCENARIOS. The same argument the spawn arm
# won in P6 (`t6`), one dimension over: a hand-written second list agrees with the first today
# and drifts the first time somebody adds a scenario to one of them, and a Magma census covering
# LESS than the Espryt lane looks exactly like a Magma census covering all of it. The
# registration macro records every call it is given; this replays them, so a scenario added to
# the split arms tomorrow joins the Magma census the same day.
#
# THE THREE ARMS INCLUDE INPROC, and that is the block's diagnostic half: a case red on spawn
# and tcp but GREEN on inproc is a two-process defect (plumbing, a death notice, a reply that
# never crossed), while a case red on all three is Magma's own missing verb and belongs to a
# wave 2 cluster. Without the inproc arm every red would need a hand re-run to tell those apart.
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
# P7 PACKAGE L: THE WHOLE INTEGRATION BINARY UNDER MAGMA SPLIT - TIER 3, INFORMATIONAL
# =====================================================================================
#
# WHY A THIRD TIER, AND WHY IT IS THE ONE THAT ANSWERS THE EXIT GATE. Tier 2 above replays the
# split-arm registrations, which is ~100 scenarios - the set Espryt's disaggregation work has
# been carrying since P5. It measured GREEN on Magma, all three transports, and that is a real
# result but it is NOT exit gate 1. The gate says the INTEGRATION BINARY is green under Magma
# split, and the set that phrase names is the one the monolith `DirectVulkan.` arm runs: the
# whole suite minus MGL_ITEST_MONOLITH_MECHANISM_FILTER (the mechanism oracles, which toggle
# process-local push masks, pointer-key ABA, direct persistent adoption or synchronous stats
# slots and therefore must not inherit a transport). 511 cases, against tier 2's 102.
#
# THIS IS WHERE THE @P7 REFUSALS LIVE. None of tier 2's scenarios reaches one; the readback
# matrices, the same-texture copy shapes and the odd-offset uniform ranges do, and they are in
# this set and nowhere else in the disaggregated registry. THE FIRST RUN'S RED LIST IS WHAT
# WAVE 2's CLUSTERS ARE SCHEDULED FROM - see notes/p7/magma-two-process-first-run.md.
#
# IN NO CI GATING STEP, and that is the whole design: a lane that must be green cannot carry a
# census. A case leaves this tier for a gated one with the commit that retires its blocker.
# The label spelling follows tier 2's rule (`ctest -L` is a regex, ID-131): the tier word goes
# in the MIDDLE, so `-L integration-magma-split` and `-L integration-magma-all-split` cannot
# reach `integration-magma-full-split` and vice versa.
#
# THE SAME PRIVATE-LOG DISCIPLINE as every other split-family entry, through a list of its own
# rather than the shared MGL_SPLIT_ARM_TEST_LISTS: that list ALSO carries the per-entry lane
# markers (strict-arming, pmap, server counters) and applying them to a census would arm three
# unrelated oracles on cases that are here to be read, not to assert. SplitLogPaths.cmake.in
# walks this list for the log path alone.
#
# PLACED AFTER TIER 2 AND BEFORE THE configure_file, for that file's stated reason.
#
# TWO DIFFERENCES FROM TIERS 1 AND 2's ENVIRONMENT, BOTH MEASURED RATHER THAN ASSUMED.
#
#   1. NO MGITEST_SPLIT_LANE=1. That marker arms ScenarioFixture's destructor assertion
#      (ScenarioFixture.h:85-100): a case in an armed split lane must have MOVED the client
#      encoder's record ordinal, because "the workload drew, cleared and read pixels, so
#      records were due". That is exactly right for a CURATED lane, where every entry draws.
#      It is wrong for a whole-binary census, which also contains query-only cases: the first
#      run of this tier with the marker set produced 13 reds - AdvertisedLimitsScenario (8),
#      SpirvShaderBinaryScenario (4) and ProgramPipelineScenario.AReservedName… - every one of
#      them `EmitSeq was 20 at SetUp and is 20 now`, because a limits query is answered from
#      the client's CapsMirror and a shader-binary error surface never reaches a verb. Those
#      13 would red on a DirectGLES census too; they are a property of the census, not of
#      Magma, and leaving them in would have buried the real list under noise.
#      WHAT THIS GIVES UP, stated rather than discovered later: this tier does not prove its
#      records crossed the wire, and the tcp arm's "Dial=Connect created no local server child"
#      topology proof (ScenarioFixture.h:160-171, inside the same armed block) is inert here.
#      Tiers 1 and 2 keep both, and they are the tiers that gate. MGITEST_TCP_LANE is still in
#      the shared tcp fragment - it simply has no reader in this tier.
#   2. DualBlockScenario.* IS FILTERED OUT. Its green REQUIRES MOBILEGL_IPC_ROLE_SPLIT_STATE=1,
#      and a census must not set that knob - under it every barrier-pulled read aborts, which
#      would replace this tier's subject with a different experiment. Without the knob the case
#      is RED BY NAME (`MGPipeRoleSplitRehearsalActive() is false`), which is its documented
#      negative-control behaviour and not a finding. The monolith `DirectVulkan.` arm has the
#      same case and it SKIPS there, because that runtime is not split at all. Its own lane,
#      `integration-dualblock-split`, is where it is read.
if (MOBILEGL_BUILD_DISAGGREGATED)
    set(MGL_MAGMA_FULL_TEST_LISTS "")
    foreach(mglItestArm IN LISTS MGL_ITEST_MAGMA_ARMS)
        mgl_itest_magma_arm_shape(${mglItestArm})
        string(TOUPPER "${mglItestArm}" mglItestArmUpper)
        mgl_itest_join_environment(mglItestMagmaFullEnvironment
            "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_ITEST_REQUIRE_GPU=1"
            ${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_VULKAN_ENV} ${mglItestArmTransportEnv})
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectVulkan.${mglItestArm}.Full."
            TEST_LIST "MGL_MAGMA_FULL_TESTS_${mglItestArmUpper}"
            TEST_FILTER "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:DualBlockScenario.*"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-magma-full-${mglItestArmLower}"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                ${mglItestArmProperties}
                ENVIRONMENT "${mglItestMagmaFullEnvironment}"
        )
        list(APPEND MGL_MAGMA_FULL_TEST_LISTS "MGL_MAGMA_FULL_TESTS_${mglItestArmUpper}")
    endforeach()
    message(STATUS "Integration tests: P7 package L tier 3 - the whole integration binary on "
                   "DirectVulkan x {inproc, spawn, tcp} under the informational "
                   "integration-magma-full-{split,spawn,tcp} labels (no CI gate)")
endif()

# =====================================================================================
# P7 WAVE 3 (V1): integration-verify-split - THE COMPARATOR ON THE SPLIT ARM
# =====================================================================================
#
# CONTRACT-P7 §6, exit gate 2: "verify x {monolith, split(inproc)} x Magma, zero divergence".
# Until this block the verify block near the top of this file registered `MOBILEGL_TRANSPORT=
# monolith` on every one of its 1136 entries, so the gate's second column had no entries at all
# - not merely unmet, UNMEASURABLE, exactly as package L found exit gate 1's Magma column.
#
# IT HAD A PREREQUISITE AND THE PREREQUISITE IS PAID. A verify build under `inproc` used to die
# on the server apply thread before one case could arm:
#
#   Fatal{PipeRespecifyScope} resource_respecify {slot=12, gen=0}: the descriptor carries a
#   per-level respecify scope (target=258, level=0), and no path in this phase may set one
#
# That pin is now "a per-level producer must cover the range it declares"
# (MG_Pipe/PipeApply.cpp), which is the invariant it was actually guarding.
#
# BOTH BACKENDS, AND THE MAGMA HALF IS THE HALF THE GATE NAMES. `${MGL_ITEST_VULKAN_ENV}` and
# not `${MGL_ITEST_COMMON_ENV}` on the DirectVulkan entries, for package L's reason: that list
# carries VK_ICD_FILENAMES and the three lavapipe iterationRP repairs the monolith `DirectVulkan.`
# arm has been pinned to, and a lane that appended the common list would measure a different
# driver from the gate it is compared against. `MOBILEGL_ITEST_REQUIRE_GPU=1` is pinned for the
# same reason every split-family lane pins it: a lane that skipped everything and a lane that
# passed everything are the same green in ctest, and this lane is here to be an ARM.
#
# THE CASE SET IS THE MONOLITH VERIFY LANE'S, MINUS THREE THINGS, the first two because
# this arm carries a TRANSPORT and that lane does not:
#
#   1. ${MGL_ITEST_MONOLITH_MECHANISM_FILTER}. Those oracles toggle process-local push masks,
#      pointer-key ABA, direct persistent adoption or synchronous stats slots and must not
#      inherit a transport - which is why the ambient DirectGLES./DirectVulkan. registrations
#      subtract them too and re-register them with MOBILEGL_TRANSPORT=monolith pinned. The
#      monolith verify lane keeps them because there the pin and the lane agree. NOTE that the
#      filter names PipeVerifyArmingScenario.* and PoisonOmissionScenario.*, which is why the
#      arming entry and the two negative controls below are registered by hand, one per backend,
#      exactly as the monolith verify block registers its own.
#   2. DualBlockScenario.*. Its green REQUIRES MOBILEGL_IPC_ROLE_SPLIT_STATE=1, and pinning that
#      knob here would replace this lane's subject (the comparator) with a different experiment;
#      without it the case is red by name. Tier 3 above filters it for the same reason and says
#      so at more length. Its own lane, `integration-dualblock-split`, is where it is read.
#   3. On the DirectGLES half only, four Magma wire-arm decline cases that are red on DirectGLES
#      x inproc with the comparator OFF (MGL_ITEST_VERIFY_SPLIT_GLES_MAGMA_DECLINE_FILTER below
#      says which and why).
#
# NO MGITEST_SPLIT_LANE=1, AND THIS IS THE ONE PLACE THAT COSTS SOMETHING, so it is stated
# rather than discovered. That marker arms ScenarioFixture's destructor assertion "this case
# must have MOVED the client encoder's record ordinal", which is right for a curated lane where
# every entry draws and wrong for a whole-binary set: tier 3 measured 13 reds from it
# (AdvertisedLimitsScenario x8, SpirvShaderBinaryScenario x4, ProgramPipelineScenario's reserved
# name), every one of them a query answered from the client's CapsMirror without a verb. They
# would red on a DirectGLES set too - a property of the census, not of the transport.
#
# WHAT REPLACES IT IS STRONGER HERE THAN IT IS IN TIER 3, and it is why this lane is armed
# rather than green-skipping: EVERY ENTRY GETS A PRIVATE LOG PATH (below, through
# MGL_VERIFY_SPLIT_TEST_LISTS), and `.github/workflows/test.yml`'s integration-verify-split job
# reads all of them: ConfigLoader's resolved `MOBILEGL_TRANSPORT=inproc` line is required in EVERY
# client log, so a lane that silently ran monolith fails that step by name. The comparator's
# `MGPipe: verify armed` line is latched at a process's FIRST FILL, so it is absent - honestly -
# from the processes that never reach a verb (limits queries, CapsMirror answers, the poison
# parent that only forks; 155 of 1064 on the landing run, 99 of them skips); the step therefore
# requires it in both VerifySplitArming. logs and holds the per-entry count to a stated floor.
# The VerifySplitArming. entries assert it from inside the process as well, which is the shape
# the monolith lane's VerifyArming. entries already have.
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

    # THE FOUR MAGMA DECLINE CASES ARE SUBTRACTED FROM THE DirectGLES HALF ONLY, and not because
    # of the comparator. They assert Magma's wire-arm decline contract (`@P7` retirements: the
    # depth-stencil mip decline, the two multisample blit declines, the overlapping in-place copy)
    # and skip on every monolith arm, so the only arm that registers them is DirectVulkan's
    # split family. On DirectGLES x inproc they are RED WITH THE COMPARATOR OFF - measured on
    # this build with MOBILEGL_PIPE_VERIFY=0 in the same environment, the same four assertions
    # (GL error 0x0502 where the decline promises none, or none where it promises one; the
    # overlap copy writes the level the decline leaves alone) - because Espryt does not decline
    # those shapes at all. No DirectGLES split lane had ever run them; that is a finding about the
    # DirectGLES whole-binary set on split, not about verify, and it is recorded in
    # notes/p7/verify-split.md rather than hidden in a green. The DirectVulkan half keeps them.
    set(MGL_ITEST_VERIFY_SPLIT_GLES_MAGMA_DECLINE_FILTER
        "F1WireScenario.GenerateMipmapDepthStencilDeclinesAndKeepsTheSession:F1WireScenario.MultisampleBlitOntoAMultisampleDestinationDeclinesAndKeepsTheSession:F1WireScenario.MultisampleBlitFromASingleSampleSourceDeclinesAndKeepsTheSession:F1WireScenario.CopyImageInPlaceOverlapDeclinesAndLeavesTheLevelAlone")

    set(MGL_VERIFY_SPLIT_TEST_LISTS "")
    # The ambient set, per backend. The GLES half is registered TWICE for the monolith verify
    # lane's reason and with its spelling: the texture-view cases need
    # MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1 or DirectGLES withholds GL_ARB_texture_view and they
    # green-skip, so they are subtracted from the first registration and re-registered with the
    # knob. "Every existing verify case on the split arm" is false without the second block.
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.VerifySplit."
        TEST_LIST MGL_VERIFY_SPLIT_TESTS_GLES
        TEST_FILTER "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:${MGL_ITEST_GLES_VIEW_FILTER}:DualBlockScenario.*:${MGL_ITEST_VERIFY_SPLIT_GLES_MAGMA_DECLINE_FILTER}"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-verify-split"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_SPLIT_ENVIRONMENT}"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.VerifySplit."
        TEST_LIST MGL_VERIFY_SPLIT_TESTS_GLES_VIEW
        TEST_FILTER "${MGL_ITEST_GLES_VIEW_FILTER}"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-verify-split"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_SPLIT_ENVIRONMENT}\;MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1"
    )
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.VerifySplit."
        TEST_LIST MGL_VERIFY_SPLIT_TESTS_VULKAN
        TEST_FILTER "-${MGL_ITEST_MONOLITH_MECHANISM_FILTER}:DualBlockScenario.*"
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS "integration-verify-split"
            TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
            ENVIRONMENT "${MGL_ITEST_VULKAN_VERIFY_SPLIT_ENVIRONMENT}"
    )
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
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "${mglItestVerifyBackend}.VerifySplitArming."
            TEST_FILTER "PipeVerifyArmingScenario.Armed"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-verify-split"
                TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
                ENVIRONMENT "${mglItestVerifyArmEnv}\;MGITEST_PIPE_ARMING_LANE=1\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-split-arming-${mglItestVerifyBackend}.log"
                RESOURCE_LOCK "pipe-verify-split-arming-${mglItestVerifyBackend}.log"
        )

        # THE TWO NEGATIVE CONTROLS ON THIS ARM (CONTRACT-P7 §6), the monolith verify block's
        # four entries with the split environment underneath and nothing else changed: each is
        # green when its control REPORTS, so each goes red the day the comparator or the poison
        # stops reaching the split arm. Their CI halves - the same knob exported against the
        # ambient VerifySplit. entries, where ctest has to FAIL - live in test.yml's
        # integration-verify-split job.
        #
        # A (G4): the corruption perturbs the snapshot arm at the CLIENT's entry compare, so the
        # report lands in the client role's log, which is the half LibraryLog() reads.
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "${mglItestVerifyBackend}.VerifySplitCorrupted."
            TEST_FILTER "PipeVerifyArmingScenario.CorruptedFieldIsReported"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-verify-split"
                TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
                ENVIRONMENT "${mglItestVerifyArmEnv}\;MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters\;MOBILEGL_PIPE_VERIFY_FATAL=0\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-split-corrupt-${mglItestVerifyBackend}.log"
                RESOURCE_LOCK "pipe-verify-split-corrupt-${mglItestVerifyBackend}.log"
        )
        # B (G5): the child brings up its own inproc session from the inherited environment, and
        # PoisonOmissionScenario's ReadChildLog reads BOTH role halves of the child's log, because
        # under a transport the poisoned read can fire on either side of the seam.
        #
        # A DIFFERENT PAIR FROM THE MONOLITH ENTRIES', and the monolith pair is not "also fine
        # here": GenerateMipmap:GetActiveTextureUnit is inert on this arm - the split backend's
        # generate_mipmap resolves its texture from the verb's handle and never reads the active
        # unit, so the child exited 0 with that stamp withheld (measured, both backends).
        # ReadPixels:GetPixelStoreParameters is read on the server under the server's stamp,
        # which is the stamp the knob has to withhold on this arm (PipeInputs.cpp's
        # ServerPoisonOmission). PoisonOmissionScenario.cpp's kOmittedPairs carries both.
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "${mglItestVerifyBackend}.VerifySplitPoisonOmitted."
            TEST_FILTER "PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-verify-split"
                TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
                ENVIRONMENT "${mglItestVerifyArmEnv}\;MOBILEGL_PIPE_POISON_OMIT=ReadPixels:GetPixelStoreParameters\;MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-poison-omit-split-${mglItestVerifyBackend}.log"
                RESOURCE_LOCK "pipe-poison-omit-split-${mglItestVerifyBackend}.log"
        )
        # B's AMBIENT TARGET. The monolith verify lane's control-B step aims its knob at
        # `Verify.PoisonOmissionScenario.WithoutOmissionCompletes`, the one integration entry that
        # calls glGenerateMipmap at all - and the mechanism filter took that entry out of the
        # ambient set above along with the rest of the scenario. Registered back by hand, with no
        # knob, so the CI step has an entry on THIS arm to turn red.
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "${mglItestVerifyBackend}.VerifySplit."
            TEST_LIST "MGL_VERIFY_SPLIT_TESTS_POISON_${mglItestVerifyBackend}"
            TEST_FILTER "PoisonOmissionScenario.WithoutOmissionCompletes"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-verify-split"
                TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
                ENVIRONMENT "${mglItestVerifyArmEnv}"
        )
        list(APPEND MGL_VERIFY_SPLIT_TEST_LISTS "MGL_VERIFY_SPLIT_TESTS_POISON_${mglItestVerifyBackend}")
    endforeach()

    # NOT ON SPAWN, AND THIS IS A PROPERTY OF THE INSTRUMENT, NOT A GAP IN THE REGISTRATION. The
    # comparator is a same-address-space oracle: its two arms are the applier-written gPipeInputs
    # and the frontend GLContext, and only inproc puts them in one process. Measured on this build
    # (VerifySplitArming's DirectGLES entry with MOBILEGL_TRANSPORT=spawn): the client's entry
    # compare aborts at the first verb with Fatal{PipeVerifyDiffer, "GetClearColor@Clear", verb=1,
    # where=entry}, because the fill leaves every record-supplied field to the applier and the
    # applier now writes ANOTHER process's block; and the server process runs no compare at all,
    # because MG_Impl is not in it and the read hook finds no live context. A spawn verify lane
    # would be red at bring-up on the first half and blind on the second.

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
endif()
