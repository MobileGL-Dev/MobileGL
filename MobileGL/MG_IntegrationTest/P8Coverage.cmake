# P8-A (ID-P8-2): the monolith cases' split, spawn and tcp registrations.
#
# Included ONCE from CMakeLists.txt, inside `if (MOBILEGL_BUILD_DISAGGREGATED)`, and the place is
# load-bearing twice over: it must come BEFORE the Magma tier-2 replay (every
# mgl_itest_register_split_arms call here is replayed onto DirectVulkan's informational
# integration-magma-all-{split,spawn,tcp} arms from MGL_SPLIT_ARM_CALLS) and BEFORE the
# SplitLogPaths configure_file at the end of that file (every entry here gets its private
# MOBILEGL_LOG_FILE_PATH from MGL_SPLIT_ARM_TEST_LISTS, or SplitLogPaths.PrivateAndDistinct reds).
#
# THE GATE IS scripts/ci/split_coverage.py: after the arm and tail segments come off, every
# monolith DirectGLES. / DirectVulkan. case is on a gated split arm or is named, with a reason
# class, in scripts/data/split_coverage_exemptions.txt. Deleting a line below reds it BY NAME.
# The per-case census this list was cut from (inproc / spawn / tcp, junit per arm) is
# docs/Disaggregated/notes/p8/A.md.

# ---- (a) THE AMBIENT CASES -----------------------------------------------------------------
#
# Every case here was registered on monolith only, and on all three arms under the armed split
# environment it either PASSED (records crossed: ScenarioFixture's EmitSeq assertion is armed on
# these arms) or SKIPPED for the same reason its monolith entry skips on this host (ClipDistance,
# DualSourceBlend, IterationRP*, SsboDeclarationForm's two known gaps, and the knob-pinned cases
# the (a') block below runs with their knob). Cases that belong to monolith by nature are in the
# exemption table instead, never here: no-records (limits / shader-binary queries answered from
# the caps mirror), single-backend (Magma-only lanes and declines), mechanism, pending-fix.
#
# ONE FILTER, not one call per scenario: each call is three discovery runs plus three more for
# the Magma replay. Explicit names where a scenario is split between registered and exempt
# cases; `Scenario.*` only where every case of the scenario is here.
set(MGL_P8A_AMBIENT_CASES
    AdvertisedLimitsScenario.ImageUnitBindingsAreReportedFromTheFrontendState
    AdvertisedLimitsScenario.IndexedBufferBindingsAreReportedVerbatimOnBothWidths
    BufferTextureScenario.*
    ClearTexImageUndefinedLevelZeroScenario.*
    ClipDistanceScenario.*
    CopyImageLevelRangeScenario.*
    CopyImagePacked16Scenario.*
    CrossFrameBufferScenario.*
    DefaultFramebufferAcrossSwapScenario.*
    DepthStencilReadbackAttachmentShapeScenario.DepthOfALayeredCubeAttachmentReadsBack
    DepthStencilReadbackAttachmentShapeScenario.DepthOfAPlainTexture2DAttachmentReadsBack
    DepthStencilReadbackAttachmentShapeScenario.DepthOfAnArrayLayerAttachmentReadsBack
    DepthStencilReadbackAttachmentShapeScenario.PackedArrayLayerAttachmentReadsBackBothAspects
    DepthStencilReadbackScenario.*
    DoublePrecisionScenario.*
    DualSourceBlendScenario.*
    EmptyScissorScenario.*
    F1WireScenario.ColorBlitToDefaultFromANonSampleableSourceDegrades
    F1WireScenario.ColorBlitToDefaultHonorsScissorAndReversedRect
    F1WireScenario.ColorBlitToDefaultIgnoresViewportAndPreservesOrientation
    F1WireScenario.ComputeImageStoreFramebufferPixels
    F1WireScenario.ComputeImageStoreThroughViewPreservesOtherRootLayer
    F1WireScenario.ComputeWrittenVertexAndIndexBuffersDrawAndReadBack
    F1WireScenario.CopyImageInPlaceAcrossLevelsAndLayers
    F1WireScenario.EnabledVertexBufferDrawKeepsNamedP7Fatal
    F1WireScenario.GenerateMipmapHonorsMutableBaseAndMaxWithoutChangingOtherLevels
    F1WireScenario.GenerateMipmapThroughViewKeepsOwnerLevelsAndLayersOutsideItsWindow
    F1WireScenario.MultisampleColorBlitScalesAndFlipsThroughTheResolveScratch
    F1WireScenario.PartialTextureUploadPreservesGpuClearPixels
    F1WireScenario.SrgbDrawTracksFramebufferConversion
    F1WireScenario.StreamedBufferSubDataBeforeEachDrawIsOrdered
    F1WireScenario.Texture1DFramebufferClearPixels
    F1WireScenario.TextureReadbackLargerThanAReplySlotContainsGpuWrites
    F1WireScenario.TextureViewClearTargetsItsRootMipAndLayer
    F1WireScenario.UniformBufferRangeAndRebindPixels
    F1WireScenario.VertexIdSamplerAndScalarUniformPixels
    FormatlessImageBakeScenario.*
    FragCoordOriginScenario.*
    FragmentOutputArrayIndexScenario.*
    GeometryDrawModeScenario.*
    GuiBatchScenario.*
    # dev f973008c merge: the empty-binding uniform block (853c5f12 + P11 7784d6bb)
    UnboundUniformBlockScenario.*
    ImageFormatQualifierScenario.*
    ImageLoadStoreSsoScenario.PerElementImageUnitsReachAPipelineDraw
    ImageLoadStoreSsoScenario.ReassigningAnImageUnitBetweenDrawsReachesTheNextDraw
    IterationRPFirstReductionScenario.*
    IterationRPProgram203Scenario.*
    IterationRPScratchFixScenario.*
    LayeredAttachmentBarrierScenario.*
    LayeredAttachmentShapeScenario.*
    NonCoreImageFormatScenario.*
    OrientationScenario.*
    P4aFinalFixScenario.*
    P4aSeamAuditScenario.*
    PipelineFailureScenario.*
    PointSizeDemotionScenario.*
    ProgramPipelineScenario.ASamplerUnitRewrittenBetweenDrawsKeepsPaintingTheRightTexture
    ProgramPipelineScenario.AStageProgramsUniformBlockBindingReachesThePipelineDraw
    ProgramPipelineScenario.ATwoStagePipelinePaintsWhatItsStagesDescribe
    ProgramPipelineScenario.AUniformDeclaredInTwoStagesKeepsTheValueTheWrittenStageHolds
    ProgramPipelineScenario.ComputeAndGraphicsStagesShareOnePipeline
    ProgramPipelineScenario.RebindingAUniformBlockBetweenDrawsReachesTheNextDraw
    ProgramPipelineScenario.SwitchingBetweenAPipelineAndAMonolithicProgramLeavesNoError
    ProgramPipelineScenario.UniformsGoToTheActiveShaderProgram
    RenderbufferBlendFormatScenario.*
    ResidentIndexScenario.*
    SampleMaskScopeScenario.*
    SampleVariablesScenario.*
    SnormAttachmentScenario.*
    SpirvShaderBinaryScenario.SpecializedModulesLinkAndRenderWithTheirConstantsApplied
    SsboArrayDynamicIndexScenario.*
    SsboArrayLengthScenario.*
    SsboDeclarationFormScenario.*
    StorageBufferRegrowScenario.AGrownStoreIsVisibleThroughItsExistingIndexedBinding
    StreamedArenaScenario.*
    TessellationDrawModeScenario.*
    ThreeChannelAttachmentScenario.*
    UnboundCounterBlockScenario.*
    UnlocatedIoBlockScenario.*
    UnwrittenPositionOutputScenario.ARedeclaredButUnwrittenPositionStillDraws
    UnwrittenPositionOutputScenario.AShaderWithNoPositionBlockStillDraws
    UnwrittenPositionOutputScenario.AWrittenRedeclaredPositionStillDraws
    UnwrittenPositionOutputScenario.CapturingAWrittenPositionStillDraws
    VertexArrayEnableDisableScenario.*
    VertexAttribBindingScenario.*
    ViewportArrayScenario.*
)
list(JOIN MGL_P8A_AMBIENT_CASES ":" mglP8aAmbientFilter)
mgl_itest_register_split_arms("${mglP8aAmbientFilter}" "")

# ---- (a') THE SERVER-READ KNOB LANES, EACH WITH A TCP SUPERVISOR OF ITS OWN ------------------
#
# The monolith file pins five whole-process knobs in lanes of their own (PointSizeDemotion.,
# UnlocatedIoBlocks., NoViewportArrayEmulation., WidenedPacked16., ForcedDepthStencilEmulation.),
# because without them the cases are unfalsifiable on llvmpipe or skip. Every one of the five is
# read through ConfigLoader IN THE PROCESS THAT RUNS THE BACKEND - the server. Under inproc that
# is this process and under spawn the child inherits ::environ, so the entry's ENVIRONMENT
# reaches it; under tcp the server is a supervisor started by a fixture, and the lane-wide
# TcpServer.Start supervisor never sees a per-entry variable. MEASURED on this tree before this
# block existed: with the knob in the client's environment only, the Tcp copies of
# PointSizeDemotion's and UnlocatedIoBlocks' arming cases read no arming line and
# NoViewportArrayEmulation's negative control saw the emulation still on - red, where the Split
# and Spawn copies were green.
#
# THE KNOB CANNOT GO ON THE SHARED FIXTURE (the IC TcpServer.Start precedent, which works for
# MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW only because every arm sets that one): it would retarget
# every other tcp case - viewport emulation off breaks ViewportArrayScenario's positive cases,
# the IO-block strip rewrites every interface block. So each lane gets a supervisor of its own,
# on its own port, started by its own fixture with the knob in the fixture's ENVIRONMENT.
#
# THE PORTS: the lane endpoint's port + MOBILEGL_ITEST_TCP_KNOB_PORT_OFFSET + the lane's index.
# The default offset (1000) keeps the five clear of every per-tree lane endpoint in use (40613
# CI, 407xx / 409xx split and verify builds, 484x0 package trees). One overlap remains: a tree
# whose split and verify builds sit one port apart (409x3 / 409x4) gets knob ports one apart as
# well, so the two builds must not run integration-tcp at the same moment - set the cache
# variable differently in one of them if they ever do.
#
# THE LOCK IS STILL mobilegl-tcp: spawn_lane_parity.py / junit_tally.py require it of every
# integration-tcp entry, and one tcp session at a time is what every other tcp entry assumes.
#
# NOT FOR DirectVulkan: all five knobs are Espryt's (Magma routes viewports natively, has no
# IO-block strip, no packed-16 widening, no DS readback emulation; point-size demotion's Magma
# lane is monolith-only and stays so here).
set(MOBILEGL_ITEST_TCP_KNOB_PORT_OFFSET 1000 CACHE STRING
    "Port offset (from MOBILEGL_ITEST_TCP_ENDPOINT's) of the per-knob loopback supervisors")
if(NOT MOBILEGL_ITEST_TCP_ENDPOINT MATCHES "^(tcp://.+):([0-9]+)$")
    message(FATAL_ERROR "P8-A: MOBILEGL_ITEST_TCP_ENDPOINT '${MOBILEGL_ITEST_TCP_ENDPOINT}' is not "
                        "tcp://<host>:<port>; the per-knob tcp supervisors derive their ports from it")
endif()
set(mglP8aTcpHost "${CMAKE_MATCH_1}")
set(mglP8aTcpPort "${CMAKE_MATCH_2}")

set(MGL_P8A_KNOB_LANES PointSizeDemotion UnlocatedIoBlocks NoViewportArrayEmulation WidenedPacked16
                       ForcedDs)
set(MGL_P8A_KNOB_FILTER_PointSizeDemotion "PointSizeDemotionScenario.*")
set(MGL_P8A_KNOB_ENV_PointSizeDemotion "MOBILEGL_POINT_SIZE_DEMOTION=1")
set(MGL_P8A_KNOB_FILTER_UnlocatedIoBlocks "UnlocatedIoBlockScenario.*")
set(MGL_P8A_KNOB_ENV_UnlocatedIoBlocks "MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS=1")
# One case, as on monolith: the three positive cases describe behaviour the backend does not
# have with the emulation off.
set(MGL_P8A_KNOB_FILTER_NoViewportArrayEmulation
    "ViewportArrayScenario.WithoutTheEmulationEveryIndexCollapsesOntoViewportZero")
set(MGL_P8A_KNOB_ENV_NoViewportArrayEmulation "MOBILEGL_ESPRYT_FORCE_VIEWPORT_ARRAY_EMULATION=0")
set(MGL_P8A_KNOB_FILTER_WidenedPacked16 "CopyImagePacked16Scenario.*")
set(MGL_P8A_KNOB_ENV_WidenedPacked16 "MOBILEGL_ESPRYT_WIDEN_PACKED16_STORAGE=1")
# ForcedDs keeps the spelling the existing Split/Spawn DepthStencilReadbackMatrix block uses. The
# default-framebuffer case was left out here while it was red on spawn and tcp; P8-SE fixed it
# (notes/p8/SE.md) and it runs under the knob on every arm like the rest.
set(MGL_P8A_KNOB_FILTER_ForcedDs
    "DepthStencilReadbackScenario.*:DepthStencilReadbackAttachmentShapeScenario.*")
set(MGL_P8A_KNOB_ENV_ForcedDs "MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1")
# The Matrix scenario's Tcp copy moves HERE from the ForcedDs block (same names): that block
# put the knob in the client's environment only, so its Tcp arm ran the native readback path.
set(MGL_P8A_KNOB_TCP_FILTER_ForcedDs
    "DepthStencilReadbackMatrixScenario.*:${MGL_P8A_KNOB_FILTER_ForcedDs}")

set(mglP8aKnobIndex 0)
foreach(mglP8aLane IN LISTS MGL_P8A_KNOB_LANES)
    string(TOLOWER "${mglP8aLane}" mglP8aLaneLower)
    math(EXPR mglP8aKnobPort "${mglP8aTcpPort} + ${MOBILEGL_ITEST_TCP_KNOB_PORT_OFFSET} + ${mglP8aKnobIndex}")
    math(EXPR mglP8aKnobIndex "${mglP8aKnobIndex} + 1")
    set(mglP8aKnobEndpoint "${mglP8aTcpHost}:${mglP8aKnobPort}")
    set(mglP8aKnobFixture "mobilegl-tcp-p8a-${mglP8aLaneLower}")
    set(mglP8aKnobState "${CMAKE_CURRENT_BINARY_DIR}/tcp-server-p8a-${mglP8aLaneLower}.json")
    set(mglP8aKnob "${MGL_P8A_KNOB_ENV_${mglP8aLane}}")
    add_test(NAME TcpServer.Start.P8a${mglP8aLane} COMMAND "${Python3_EXECUTABLE}" "${MGL_TCP_FIXTURE}"
        start --server "${MGL_ITEST_SPLIT_SERVER_PATH}" --endpoint "${mglP8aKnobEndpoint}"
        --state "${mglP8aKnobState}")
    add_test(NAME TcpServer.Stop.P8a${mglP8aLane} COMMAND "${Python3_EXECUTABLE}" "${MGL_TCP_FIXTURE}"
        stop --state "${mglP8aKnobState}")
    # The texture-view opt-in as on the shared fixture, so this supervisor differs from it in
    # the knob alone.
    set_tests_properties(TcpServer.Start.P8a${mglP8aLane} PROPERTIES
        FIXTURES_SETUP ${mglP8aKnobFixture} TIMEOUT 20
        ENVIRONMENT "MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1;${mglP8aKnob}")
    set_tests_properties(TcpServer.Stop.P8a${mglP8aLane} PROPERTIES
        FIXTURES_CLEANUP ${mglP8aKnobFixture} TIMEOUT 20)

    foreach(mglP8aArm Split Spawn Tcp)
        string(TOLOWER "${mglP8aArm}" mglP8aArmLower)
        string(TOUPPER "${mglP8aArm}" mglP8aArmUpper)
        set(mglP8aEnvironment "${MGL_ITEST_GLES_${mglP8aArmUpper}_ENVIRONMENT}")
        set(mglP8aFilter "${MGL_P8A_KNOB_FILTER_${mglP8aLane}}")
        set(mglP8aProperties)
        if(mglP8aArm STREQUAL "Tcp")
            string(REPLACE "MOBILEGL_IPC_CONTROL=${MOBILEGL_ITEST_TCP_ENDPOINT}"
                           "MOBILEGL_IPC_CONTROL=${mglP8aKnobEndpoint}"
                           mglP8aEnvironment "${mglP8aEnvironment}")
            if(mglP8aEnvironment STREQUAL MGL_ITEST_GLES_TCP_ENVIRONMENT)
                message(FATAL_ERROR "P8-A: the Tcp arm environment names no "
                                    "MOBILEGL_IPC_CONTROL=${MOBILEGL_ITEST_TCP_ENDPOINT} to retarget")
            endif()
            set(mglP8aProperties FIXTURES_REQUIRED ${mglP8aKnobFixture} RESOURCE_LOCK mobilegl-tcp)
            if(DEFINED MGL_P8A_KNOB_TCP_FILTER_${mglP8aLane})
                set(mglP8aFilter "${MGL_P8A_KNOB_TCP_FILTER_${mglP8aLane}}")
            endif()
        endif()
        gtest_discover_tests(MobileGLIntegrationTest
            TEST_PREFIX "DirectGLES.${mglP8aArm}.${mglP8aLane}."
            TEST_LIST "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}"
            TEST_FILTER "${mglP8aFilter}"
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "integration-gpu\;integration-${mglP8aArmLower}"
                TIMEOUT ${MGL_ITEST_TIMEOUT}
                ENVIRONMENT "${mglP8aEnvironment}\;${mglP8aKnob}"
                ${mglP8aProperties}
        )
        list(APPEND MGL_SPLIT_ARM_TEST_LISTS "MGL_SPLIT_ARM_TESTS_${MGL_SPLIT_ARM_INDEX}")
        math(EXPR MGL_SPLIT_ARM_INDEX "${MGL_SPLIT_ARM_INDEX} + 1")
    endforeach()
endforeach()
message(STATUS "Integration tests: P8-A registers ${mglP8aKnobIndex} server-knob lane(s) on "
               "split/spawn/tcp, each tcp arm on its own supervisor")
