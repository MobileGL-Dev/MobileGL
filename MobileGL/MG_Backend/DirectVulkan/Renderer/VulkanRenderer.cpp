// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRendererInternal.h"

namespace MobileGL::MG_Backend::DirectVulkan {
#if defined(__APPLE__)
    namespace {
        void SendVoid(id receiver, const char* selector) {
            ObjcMsgSend<void (*)(id, SEL)>()(receiver, sel_registerName(selector));
        }

        void Release(id object) {
            if (object) {
                SendVoid(object, "release");
            }
        }
    } // namespace
#endif

    static Bool IsPowerVRDevice(const VkPhysicalDeviceProperties& properties) {
        return std::strstr(properties.deviceName, "PowerVR") != nullptr;
    }

    // ---- D12.3: DynamicTailKey's inputs against the P2 chunk table ----
    //
    // DynamicTailKey's inventory (declared above, one line per reader) is an exact,
    // hand-maintained enumeration of what the six Apply* in the tail read. The P2 chunk table
    // (MG_Pipe/MGPipeRenderStateSpans.h) is an independent, offsetof-derived statement of
    // which bytes of RenderStateParameters are dynamic state. The two were written for
    // different reasons, so making them check each other is free evidence: if a later chunk
    // edit demotes or promotes one of these members, the mismatch is a BUILD BREAK here rather
    // than a tail that silently stops being re-run when its input moves.
    //
    // The brief (P2 D12.3) expects every input to be dynamic; the tree says otherwise for
    // exactly one, and the tree is right - see the ScissorTestEnabledMask note below.
    //
    // These assertions ARE D19's DynamicChunksCoverMagmasDynamicTailKey, in the only file this
    // package owns. D19 names it as a case in MG_Test/Pipe/RenderStateSpansTest.cpp, which
    // belongs to package A (C.5). INTEGRATOR: make sure the outcome is not "neither" - if
    // package A did not land that case, this static_assert block is the whole gate, and if it
    // did, the two are redundant on purpose and both should stay.

    namespace {
        // Is [begin, begin + size) covered entirely by DYNAMIC chunks?
        constexpr Bool MagmaRenderStateRangeIsDynamic(SizeT begin, SizeT size) {
            const SizeT end = begin + size;
            for (SizeT i = 0; i < MG_Pipe::kMGPipeRenderStateChunkCount; ++i) {
                const SizeT chunkBegin = MG_Pipe::kMGPipeRenderStateChunkBoundaries[i];
                const SizeT chunkEnd = MG_Pipe::kMGPipeRenderStateChunkBoundaries[i + 1];
                if (end <= chunkBegin || begin >= chunkEnd) continue; // disjoint
                if (MG_Pipe::MGPipeRenderStateChunkIsPipeline(i)) return false;
            }
            return true;
        }
        using MagmaTailRsp = RenderStateParameters;

#define MAGMA_TAIL_INPUT_IS_DYNAMIC(Member)                                                                            \
    static_assert(MagmaRenderStateRangeIsDynamic(offsetof(MagmaTailRsp, Member), sizeof(MagmaTailRsp::Member)),        \
                  "ApplyDynamicDrawStateTail reads " #Member                                                           \
                  ", which the P2 chunk table no longer calls dynamic state: a change to it would "                    \
                  "move the pipeline version, not the parameters version, and the tail would stop "                    \
                  "being re-run for it")

        // Viewports, DepthRanges and ScissorBoxes are asserted over the WHOLE array while the
        // tail reads only element 0. That is deliberately stricter than the reader needs: the
        // chunk table has no per-element granularity today, so an array that is dynamic at all
        // is dynamic entirely, and asserting the whole of it says so. If a later phase ever
        // splits a per-viewport chunk out, this is a build break by design - narrow the assert
        // to element 0 then, and say why in the same commit.
        MAGMA_TAIL_INPUT_IS_DYNAMIC(Viewports);           // ApplyGLViewportState: Viewports[0]
        MAGMA_TAIL_INPUT_IS_DYNAMIC(DepthRanges);         // ApplyGLViewportState: DepthRanges[0]
        MAGMA_TAIL_INPUT_IS_DYNAMIC(BlendColor);          // ApplyBlendConstants
        MAGMA_TAIL_INPUT_IS_DYNAMIC(PolygonOffsetFactor); // ApplyPolygonOffsetState
        MAGMA_TAIL_INPUT_IS_DYNAMIC(PolygonOffsetUnits);  // ApplyPolygonOffsetState
        MAGMA_TAIL_INPUT_IS_DYNAMIC(PolygonOffsetClamp);  // ApplyPolygonOffsetState
        MAGMA_TAIL_INPUT_IS_DYNAMIC(LineWidth);           // ApplyLineWidthState
        MAGMA_TAIL_INPUT_IS_DYNAMIC(ScissorBoxes);        // the scissor rect: ScissorBoxes[0]
#undef MAGMA_TAIL_INPUT_IS_DYNAMIC

        // ApplyStencilState reads three of the seven members of each face, and D6 splits
        // StencilFaceState at sub-member granularity for exactly this reason: Ref, ValueMask
        // and WriteMask are VK_DYNAMIC_STATE_STENCIL_{REFERENCE,COMPARE_MASK,WRITE_MASK}, while
        // Func and the three ops are baked into the pipeline. Asserted per member, per face,
        // because the split runs THROUGH the struct rather than around it.
        constexpr SizeT kMagmaStencilFace1 = offsetof(MagmaTailRsp, StencilStates) + sizeof(StencilFaceState);
#define MAGMA_TAIL_STENCIL_IS_DYNAMIC(Member)                                                                          \
    static_assert(                                                                                                     \
        MagmaRenderStateRangeIsDynamic(offsetof(MagmaTailRsp, StencilStates) + offsetof(StencilFaceState, Member),     \
                                       sizeof(StencilFaceState::Member)),                                              \
        "ApplyStencilState reads the FRONT face's " #Member " as dynamic state");                                      \
    static_assert(MagmaRenderStateRangeIsDynamic(kMagmaStencilFace1 + offsetof(StencilFaceState, Member),              \
                                                 sizeof(StencilFaceState::Member)),                                    \
                  "ApplyStencilState reads the BACK face's " #Member " as dynamic state")

        MAGMA_TAIL_STENCIL_IS_DYNAMIC(Ref);
        MAGMA_TAIL_STENCIL_IS_DYNAMIC(ValueMask);
        MAGMA_TAIL_STENCIL_IS_DYNAMIC(WriteMask);
#undef MAGMA_TAIL_STENCIL_IS_DYNAMIC

        // THE ONE INPUT THAT IS NOT DYNAMIC, and the brief's D12.3 says it should be.
        // The tree wins, and it is right: the split's only rule is "a byte is pipeline state
        // iff a public setter that calls BumpVersions() writes it", and ScissorTestEnabledMask
        // is written by SetCapability(ScissorTest), which does. It sits in pipeline chunk P6
        // with the other capability bools. The tail reads it only to decide between the
        // scissor box and a full-extent rect, and it is HARMLESS there for a reason worth
        // stating: a pipeline-half write moves the pipeline version, and the pipeline version
        // moves only together with the parameters version (BumpVersions bumps both), so the
        // tail's version gate is invalidated by it just the same. A DYNAMIC member promoted
        // into the pipeline half would break that direction, which is what the asserts above
        // are for; this one is pinned in the opposite direction so that DEMOTING it - which
        // would be a real G7 violation - is also a build break.
        static_assert(!MagmaRenderStateRangeIsDynamic(offsetof(MagmaTailRsp, ScissorTestEnabledMask),
                                                      sizeof(MagmaTailRsp::ScissorTestEnabledMask)),
                      "ScissorTestEnabledMask is written by SetCapability(ScissorTest), which calls "
                      "BumpVersions(), so the chunk table must keep it in the pipeline half");
    } // namespace

    namespace {
        static constexpr Uint32 kDescriptorSetsPerFrame = 64;

        struct BlitImageBinding {
            VkImage image = VK_NULL_HANDLE;
            VkImageLayout* trackedLayout = nullptr;
            VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_NONE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
            IntVec2 extent = {0, 0};
            Uint32 mipLevel = 0;
            Uint32 mipLevelCount = 1;
            Uint32 baseArrayLayer = 0;
            Uint32 layerCount = 1;
            // z slice for a VK_IMAGE_TYPE_3D source; array attachments use baseArrayLayer instead.
            Uint32 depthOffset = 0;
            const char* label = nullptr;
        };

        static Uint32 ComputeMaxProgramBindings(const VkPhysicalDeviceProperties& properties,
                                                const ProgramFactory::UpdateAfterBindLimits& updateAfterBindLimits) {
            const auto& limits = properties.limits;
            static constexpr Uint32 kMinProgramBindings = 16;
            static constexpr Uint32 kMaxProgramBindingsCap = 256;
            const Uint32 maxCombinedImageSamplers =
                std::min(limits.maxPerStageDescriptorSamplers, limits.maxDescriptorSetSamplers);
            const Uint32 maxSampledImages =
                std::min(limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages);
            const Uint32 maxDynamicUniformBuffers =
                std::min(limits.maxPerStageDescriptorUniformBuffers, limits.maxDescriptorSetUniformBuffersDynamic);

            Uint32 maxBindings = limits.maxPerStageResources;
            maxBindings = std::min(maxBindings, maxCombinedImageSamplers);
            maxBindings = std::min(maxBindings, maxSampledImages + maxDynamicUniformBuffers);

            if (updateAfterBindLimits.enabled) {
                const Uint32 updateAfterBindSamplers =
                    std::min(updateAfterBindLimits.maxPerStageSamplers, updateAfterBindLimits.maxSetSamplers);
                const Uint32 updateAfterBindSampledImages =
                    std::min(updateAfterBindLimits.maxPerStageSampledImages, updateAfterBindLimits.maxSetSampledImages);
                const Uint32 updateAfterBindDynamicUniformBuffers = std::min(
                    updateAfterBindLimits.maxPerStageUniformBuffers, updateAfterBindLimits.maxSetUniformBuffersDynamic);
                Uint32 updateAfterBindBindings = updateAfterBindLimits.maxPerStageResources;
                updateAfterBindBindings = std::min(updateAfterBindBindings, updateAfterBindSamplers);
                updateAfterBindBindings = std::min(updateAfterBindBindings,
                                                   updateAfterBindSampledImages + updateAfterBindDynamicUniformBuffers);
                maxBindings = std::max(maxBindings, updateAfterBindBindings);
            }

            maxBindings = std::max(kMinProgramBindings, maxBindings);
            maxBindings = std::min(kMaxProgramBindingsCap, maxBindings);
            return maxBindings;
        }
    } // namespace

    VkBool32 VulkanRenderer::DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                           VkDebugUtilsMessageTypeFlagsEXT messageType,
                                           const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
        auto typeToString = [](VkDebugUtilsMessageTypeFlagsEXT messageType) {
            switch (messageType) {
            case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
                return "General";
            case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
                return "Validation";
            case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
                return "Performance";
            case VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT:
                return "DeviceAddressBinding";
            default:
                return "Other";
            }
        };

        switch (messageSeverity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            MGLOG_E_ONCE("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            MGLOG_W_ONCE("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            MGLOG_D("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            MGLOG_D("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        default:
            break;
        }
        return VK_FALSE;
    }

    VulkanRenderer::VulkanRenderer(NativeWindowType window, const VulkanRendererConfig& cfg)
        : m_window(window), m_config(cfg), m_presentsToAppWindow(window != NativeWindowType{}) {
        if (!m_presentsToAppWindow) {
            m_config.SwapInterval.reset();
        }
        // Initialize();
    }

    VulkanRenderer::~VulkanRenderer() {
        Shutdown();
    }

    void VulkanRenderer::Initialize() {
        // P2 D14, and it belongs HERE rather than on a draw: "a Track-H subsystem whose bit is
        // clear is a STARTUP Fatal{PipeLegacyMemosDisabled}". Checks Magma's own bit only, and
        // only once this backend is the one being brought up, so an Espryt-side bitmask cannot
        // kill a Magma run and vice versa.
        MagmaPipeValidateSubsystemConfiguration();
        CreateInstance();
        if (!CreateSurface()) throw RuntimeError("DirectVulkan: the initial surface could not be created");
        PickPhysicalDevice();
        CreateLogicalDeviceAndQueues();
        CreateAllocator();

        CreateCommandPool();
        m_progressMarkers.Init(m_device, m_physicalDevice.queueFamilies.graphicsFamily);

        // Frames-in-flight is a request, not a guarantee: it also seeds the swapchain image
        // count (SwapchainObject clamps the hint into [minImageCount, maxImageCount]). Not every
        // driver/surface supports >= 3 swapchain images, and keeping more frame slots than the
        // surface can present would leave the surplus slots stalling on vkAcquireNextImageKHR.
        // So clamp to the surface's real limits here, before any per-frame resource is sized off
        // it. (The standalone driver POST is headless and has no surface, so this check lives at
        // renderer init.) Existing logs already report the swapchain's min/actual image count;
        // this one adds the frames-in-flight decision itself.
        {
            // Desired depth comes from MOBILEGL_MAGMA_FRAMESINFLIGHT, parsed once by ConfigLoader
            // with a default of 3 when the variable is unset or invalid.
            Uint32 requestedFramesInFlight = MG_Config::Features.FramesInFlight;
            MGLOG_I("MaxFramesInFlight: configured request=%u", requestedFramesInFlight);

            VkSurfaceCapabilitiesKHR surfaceCaps{};
            const VkResult capsResult =
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice.handle, m_surface, &surfaceCaps);
            if (capsResult != VK_SUCCESS) {
                MGLOG_W("MaxFramesInFlight: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (VkResult=%d); "
                        "keeping requested %u",
                        static_cast<Int>(capsResult), requestedFramesInFlight);
            } else {
                // Frames-in-flight is the CPU pipeline depth; it only needs to stay <= the number
                // of swapchain images the surface can provide (maxImageCount), so the extra slots
                // never stall on vkAcquireNextImageKHR. It must NOT be forced up to minImageCount:
                // the swapchain independently gets >= minImageCount images (SwapchainObject raises
                // the count), and inflating the CPU depth would only add latency + memory.
                Uint32 chosenFramesInFlight = requestedFramesInFlight;
                if (surfaceCaps.maxImageCount != 0 && chosenFramesInFlight > surfaceCaps.maxImageCount) {
                    chosenFramesInFlight = surfaceCaps.maxImageCount; // 0 == no upper bound
                }
                if (chosenFramesInFlight < 2) {
                    chosenFramesInFlight = 2; // never drop below double buffering
                }
                m_config.MaxFramesInFlight = chosenFramesInFlight;
                if (chosenFramesInFlight != requestedFramesInFlight) {
                    MGLOG_W("MaxFramesInFlight: requested %u unsupported by surface (minImageCount=%u, "
                            "maxImageCount=%u); using %u",
                            requestedFramesInFlight, surfaceCaps.minImageCount, surfaceCaps.maxImageCount,
                            chosenFramesInFlight);
                } else {
                    MGLOG_I("MaxFramesInFlight: using %u (surface minImageCount=%u, maxImageCount=%u)",
                            chosenFramesInFlight, surfaceCaps.minImageCount, surfaceCaps.maxImageCount);
                }
            }
        }

        VK_VERIFY(m_frameContext.Initialize(m_device, m_commandPool, m_config.MaxFramesInFlight),
                  "CreateFrameContexts");
        MGLOG_I("CreateFrameContexts completed");
        auto succeeded = false;
        succeeded = m_bufferManager.Initialize({
            .allocator = m_allocator,
            .frameCount = m_frameContext.GetFrameCount(),
            .minUploadBytes = 4 * 1024 * 1024,
            .transientMemoryUsage = VMA_MEMORY_USAGE_AUTO,
            .transientAllocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .transientPersistentMapping = true,
            .transformFeedbackUsageEnabled = m_transformFeedbackFeatureEnabled,
        });
        MOBILEGL_ASSERT(succeeded, "VkBufferManager initialization failed.");
        m_bufferManager.SetCopyCommandProvider(this);
        m_bufferManager.SetSubmitCounterSource(&m_submitCounter);
        if (m_timerQuerySupported) {
            m_timerQueryManager = MakeUnique<VkTimerQueryManager>();
            if (m_timerQueryManager->Initialize({.device = m_device,
                                                 .frameCount = m_frameContext.GetFrameCount(),
                                                 .timestampValidBits = m_timestampValidBits,
                                                 .timestampPeriodNs = m_timestampPeriodNs})) {
                m_frameContext.SetRecordingObserver(this);
            } else {
                MGLOG_W("VkTimerQueryManager initialization failed; timer queries disabled");
                m_timerQueryManager.reset();
                m_timerQuerySupported = false;
            }
        }
        m_textureManager = MakeUnique<VkTextureManager>();
        MOBILEGL_ASSERT(m_textureManager != nullptr, "VkTextureManager creation failed.");
        succeeded = m_textureManager->Initialize({m_device, m_physicalDevice.handle, m_allocator, m_commandPool,
                                                  m_graphicsQueue, m_frameContext.GetFrameCount(),
                                                  m_imageFormatListExtensionEnabled, m_sampledReadStageMask,
                                                  static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily)});
        MOBILEGL_ASSERT(succeeded, "VkTextureManager initialization failed.");
        m_textureManager->SetWireE5RawCopyKit(GetWireE5RawCopyKit());
        m_renderPassManager = MakeUnique<VkRenderPassManager>(m_device, m_physicalDevice.handle, m_allocator, m_config,
                                                              *m_textureManager, m_swapchainObject);
        MOBILEGL_ASSERT(m_renderPassManager != nullptr, "VkRenderPassManager creation failed.");
        succeeded = m_renderPassManager->Initialize();
        MOBILEGL_ASSERT(succeeded, "VkRenderPassManager initialization failed.");

        const Uint32 maxProgramBindings =
            ComputeMaxProgramBindings(m_physicalDevice.properties, m_updateAfterBindLimits);
        MGLOG_I("DirectVulkan: using %u program descriptor bindings", maxProgramBindings);
        if (IsPowerVRDevice(m_physicalDevice.properties)) {
            m_config.DisablePipelineCache = true;
            MGLOG_W("DirectVulkan: disabling pipeline cache on PowerVR device %s",
                    m_physicalDevice.properties.deviceName);
        }

        RecreateSwapchain();

        m_pipelineFactory = MakeUnique<PipelineFactory>(m_device, m_config);
        MOBILEGL_ASSERT(m_pipelineFactory != nullptr, "PipelineFactory creation failed.");
        {
            // Qualcomm's pipeline compiler does not keep vertex positions invariant across
            // the pipelines of a multi-pass depth-equality chain (even with the SPIR-V
            // Invariant decoration), so a blended depth-writing prepass makes later
            // equality-compare passes drop whole primitives (MC 26.3 improved-transparency
            // clouds flicker black). Suppress depth writes on accumulation-blended pipelines
            // there (see PipelineFactory::ShouldSuppressDepthWrite for the exact scope);
            // MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE forces the quirk on or off on any
            // driver.
            const MG_Config::QuirkOverride quirkOverride = MG_Config::Features.MagmaDisableBlendedDepthWriteQuirk;
            const Bool suppressBlendedDepthWrite = PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(
                quirkOverride, m_physicalDevice.properties.vendorID);
            if (suppressBlendedDepthWrite) {
                MGLOG_I("DirectVulkan: suppressing depth writes on accumulation-blended pipelines "
                        "(driver lacks cross-pipeline position invariance)%s",
                        quirkOverride == MG_Config::QuirkOverride::ForceOn ? " (forced on)" : "");
            }
            PipelineFactory::SetSuppressBlendedDepthWrite(suppressBlendedDepthWrite);
        }
        ProgramFactory::SubgroupLoweringPolicy subgroupPolicy{};
        subgroupPolicy.emulateSubgroups = ShouldEmulateSubgroups(m_nativeSubgroupSupported);
        subgroupPolicy.fixIterationRPSubgroupScratch =
            UsesNativeSubgroups(m_nativeSubgroupSupported) && ShouldApplyIterationRPFixes();
        subgroupPolicy.fixIterationRPBarrier = ShouldApplyIterationRPFixes();
        subgroupPolicy.deriveNumSubgroups = UsesNativeSubgroups(m_nativeSubgroupSupported);
        subgroupPolicy.requireFullSubgroups = m_computeFullSubgroupsFeatureEnabled;
        subgroupPolicy.nativeSubgroupSize = m_nativeSubgroupSize;
        subgroupPolicy.maxComputeWorkgroupSubgroups = m_maxComputeWorkgroupSubgroups;
        subgroupPolicy.maxComputeSharedMemoryBytes = m_physicalDevice.properties.limits.maxComputeSharedMemorySize;
        m_programFactory = MakeUnique<ProgramFactory>(
            m_device, m_config, maxProgramBindings, m_shaderDrawParametersFeatureEnabled,
            m_unformattedFloatStorageImagesEnabled, m_tessellationAndGeometryPointSizeFeatureEnabled,
            MG_Config::Features.EnableSpirvValidation, m_updateAfterBindLimits, subgroupPolicy);
        MOBILEGL_ASSERT(m_programFactory != nullptr, "ProgramFactory creation failed.");
        // The swapchain already exists at this point (Initialize creates it first), so seed the
        // height the factory could not be told about from CreateSwapchain.
        m_programFactory->SetDefaultFramebufferHeight(m_swapchainObject.GetExtent().height);
        // Aging evictions (render passes and program entries) must purge the dependent
        // pipeline / compute-pipeline / descriptor-set caches in the same step; both
        // sweeps only run from the frame-boundary seams, long after initialization.
        m_programFactory->SetEvictionObserver(this);

        m_samplerManager = MakeUnique<VkSamplerManager>();
        MOBILEGL_ASSERT(m_samplerManager != nullptr, "VkSamplerManager creation failed.");
        succeeded = m_samplerManager->Initialize({m_device, &m_config, m_samplerAnisotropyFeatureEnabled,
                                                  m_physicalDevice.properties.limits.maxSamplerAnisotropy,
                                                  m_customBorderColorFeatureEnabled, m_maxCustomBorderColorSamplers});
        MOBILEGL_ASSERT(succeeded, "VkSamplerManager initialization failed.");
        succeeded = InitializeBlitResources();
        MOBILEGL_ASSERT(succeeded, "Blit pipeline resource initialization failed.");
        succeeded = InitializeDepthMipmapResources();
        MOBILEGL_ASSERT(succeeded, "Depth mipmap pipeline resource initialization failed.");

        m_uniformManager = MakeUnique<UniformManager>();
        MOBILEGL_ASSERT(m_uniformManager != nullptr, "UniformDescriptorBinder creation failed.");
        succeeded = m_uniformManager->Initialize(
            m_device, m_physicalDevice.handle, &m_bufferManager, m_programFactory.get(),
            m_physicalDevice.properties.limits.minUniformBufferOffsetAlignment, m_config.MaxFramesInFlight,
            maxProgramBindings, kDescriptorSetsPerFrame, m_textureManager.get(), m_samplerManager.get());
        MOBILEGL_ASSERT(succeeded, "UniformDescriptorBinder initialization failed.");
        m_uniformManager->SetWireInvalidStorageImageArm(m_wireNullDescriptor);
        m_vertexInputStateFactory = MakeUnique<VertexInputStateFactory>(m_config, m_physicalDevice.handle);
        MOBILEGL_ASSERT(m_vertexInputStateFactory != nullptr, "VertexInputStateFactory creation failed.");

        // Prime the first frame so Render() always targets an acquired swapchain image.
        // A zero-area window (GLFW's hidden helper window during the WGL bootstrap, or a
        // window that is already minimized) legitimately yields no swapchain here; defer
        // the first acquire to Present in that case instead of acquiring from a null
        // swapchain handle.
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE) {
            VkResult acquireResult =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
            if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
                // Nothing was acquired and no semaphore signal was armed, so
                // rebuilding and re-acquiring on the same semaphore is safe.
                MGLOG_D("Initialize, vkAcquireNextImageKHR got %d, recreating swapchain", acquireResult);
                RecreateSwapchain();
                acquireResult = m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(),
                                                                       m_imageIndexAcquired);
            } else if (acquireResult == VK_SUBOPTIMAL_KHR) {
                // The image is usable, and its acquire signal is already armed on
                // imageAvailableSemaphore. Re-acquiring here would arm a second signal on a
                // binary semaphore whose first one nobody has waited on yet; keep the image.
                // Only a real surface change schedules a rebuild.
                m_swapchainResizeRequested = m_swapchainResizeRequested || SwapchainIsOutOfDate();
                acquireResult = VK_SUCCESS;
            }
            VK_VERIFY(acquireResult, "Initialize, WaitAndAcquireNextImage");
            // The first acquired image is the one the opening frame renders into, so the
            // default framebuffer addresses it from the start (see
            // m_defaultFramebufferImageIndex).
            m_defaultFramebufferImageIndex = m_imageIndexAcquired;
        } else {
            MGLOG_W("DirectVulkan: no swapchain at initialization (zero-area window); deferring first acquire");
        }
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());

        MGLOG_D("VulkanRenderer initialized");
    }

    void VulkanRenderer::Shutdown() {
        if (m_instance == VK_NULL_HANDLE && m_device == VK_NULL_HANDLE && m_surface == VK_NULL_HANDLE) {
            return;
        }

        if (m_device != VK_NULL_HANDLE) {
            VK_VERIFY(vkDeviceWaitIdle(m_device));
        }
        m_progressMarkers.Shutdown();
        // Parked surface targets (ActivateSurfaceTarget) go before the device and instance.
        for (auto& [_, target] : m_parkedTargets)
            DestroyParkedTarget(target);
        m_parkedTargets.clear();
        // P7 wave 2 package B3: the session's decline tally. Silent when nothing declined, so a
        // green lane stays quiet and a lane that dropped a draw cannot (rule I's observable).
        WireDeclineTally::Dump("shutdown");
        // The SPIR-V screen's engagement proof: a decline-free session above reads as "screened
        // and valid" only with this count beside it (distinct modules, process-wide).
        MGLOG_I("MGWIRE-VCONV[shutdown] converted=%llu reused=%llu renamedWrites=%llu",
                static_cast<unsigned long long>(m_wireStreamsConverted),
                static_cast<unsigned long long>(m_wireStreamsReused),
                static_cast<unsigned long long>(m_bufferManager.GetWireRenameCount()));
        MGLOG_I("MGWIRE-SPIRV-SCREEN[shutdown] modulesValidated=%llu",
                static_cast<unsigned long long>(ProgramFactory::DriverModuleValidatorRuns()));
        DestroyWireDrawPass();
        CollectWireObjects(m_submitCounter, true);
        ClearAllWireDrawPassCaches();
        DestroyWireColorBlitResources();
#if MOBILEGL_BUILD_DISAGGREGATED
        DestroyWireYuvResources();
#endif
        DestroyWireDepthMipmapResources();
        DestroyWireMultisampleResolveResources();
        if (m_textureManager) m_textureManager->SetWireE5RawCopyKit(nullptr);
        DestroyWireE5RawCopyKit(m_device, m_wireE5RawCopyKit);
        m_wireE5RawReads = false;
#if MOBILEGL_BUILD_DISAGGREGATED
        DestroySharedImagePresentTargets(false);
        DestroySharedImageSync();
#endif
        OnSubmitsCompletedUpTo(m_submitCounter);
        DestroySubmitFencePool();

        DestroyDeferredDepthMipmapCleanup();
        DestroyMultisampleResolveScratchImage();
        DestroyComputePipelines();

        // No sweep runs during teardown, but the observers point at this renderer
        // and the factories die at different times below; disconnect them first.
        if (m_renderPassManager) {}
        if (m_programFactory) {
            m_programFactory->SetEvictionObserver(nullptr);
        }

        m_pipelineFactory.reset();
        ShutdownBlitResources();
        ShutdownDepthMipmapResources();
        if (m_samplerManager) {
            m_samplerManager->Shutdown();
            m_samplerManager.reset();
        }
        if (m_textureManager) {
            m_textureManager->Shutdown();
            m_textureManager.reset();
        }
        m_vertexInputStateFactory.reset();
        m_xfbCounterBuffer.Destroy();
        m_xfbCounterSlotOwner.fill(0);
        m_xfbCounterSlotLastUse.fill(0);
        m_xfbCounterSlotUseSerial = 0;
        m_xfbCountersValid.fill(false);
        m_xfbLastSeenGeneration.fill(0);
        if (m_occlusionQueryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_occlusionQueryPool, nullptr);
            m_occlusionQueryPool = VK_NULL_HANDLE;
        }
        if (m_xfbQueryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_xfbQueryPool, nullptr);
            m_xfbQueryPool = VK_NULL_HANDLE;
        }
        if (m_primGenReroutePool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_primGenReroutePool, nullptr);
            m_primGenReroutePool = VK_NULL_HANDLE;
        }
        m_primGenRerouteActiveSlots.clear();
        m_primGenRerouteSlotCursor = 0;
        m_primGenRerouteSlotOpen = false;
        // Not sticky across renderers: the next bring-up re-decides both (from the
        // per-process probe memo, so it re-decides without re-probing).
        m_primGenRerouteKind = MG_Util::SelfTest::PrimGenRerouteKind::None;
        m_primGenStreamCountsXfbInactiveDraws = false;
        m_bufferManager.Shutdown();

        // Device is idle (vkDeviceWaitIdle above); query pools can be destroyed.
        m_frameContext.SetRecordingObserver(nullptr);
        if (m_timerQueryManager) {
            m_timerQueryManager->Shutdown();
            m_timerQueryManager.reset();
        }

        if (m_device != VK_NULL_HANDLE) {
            m_frameContext.Destroy(m_device, m_commandPool);
        }

        if (m_uniformManager) {
            m_uniformManager->Shutdown();
            m_uniformManager.reset();
        }
        m_programFactory.reset();

        if (m_renderPassManager) {
            ShutdownSwapchain();
        } else if (m_device != VK_NULL_HANDLE) {
            m_swapchainObject.Shutdown(m_device);
        }
        m_renderPassManager.reset();
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }

        DestroyAllocator();

        if (m_device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        s_vkCmdDrawIndexedIndirectCount = nullptr;
        s_vkCmdWireDrawIndirectCount = nullptr;
        s_vkCmdDrawMultiEXT = nullptr;
        s_vkCmdDrawMultiIndexedEXT = nullptr;
        s_vkCmdSetFragmentShadingRateKHR = nullptr;
        m_fragmentShadingRate = {};

        if (m_instance != VK_NULL_HANDLE && m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }

#if defined(VK_USE_PLATFORM_METAL_EXT)
        if (m_platformLibrary != nullptr) {
            Release(reinterpret_cast<id>(m_platformLibrary));
            m_platformLibrary = nullptr;
        }
        if (m_platformDisplay != nullptr) {
            Release(reinterpret_cast<id>(m_platformDisplay));
            m_platformDisplay = nullptr;
        }
#endif

#if defined(VK_USE_PLATFORM_XLIB_KHR)
        if (m_platformDisplay != nullptr) {
            // No fallback window to destroy any more: the display here is only ever
            // one this renderer opened for a REAL window surface, and that window is
            // the caller's to own. The hidden-window pbuffer fallback that used to be
            // cleaned up here is gone (see CreateSurface).
            using XCloseDisplayFn = int (*)(Display*);
            auto* closeDisplay = reinterpret_cast<XCloseDisplayFn>(m_platformCloseDisplay);
            if (closeDisplay) {
                closeDisplay(static_cast<Display*>(m_platformDisplay));
            }
            m_platformDisplay = nullptr;
        }
        m_platformCloseDisplay = nullptr;
        if (m_platformLibrary != nullptr) {
            dlclose(m_platformLibrary);
            m_platformLibrary = nullptr;
        }
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        // The AImageReader owns the ANativeWindow the pbuffer fallback handed to the
        // WSI, so it outlives the surface and is released only here.
        if (m_fallbackImageReader != nullptr && m_platformLibrary != nullptr) {
            using AImageReaderDeleteFn = void (*)(void*);
            auto* imageReaderDelete =
                reinterpret_cast<AImageReaderDeleteFn>(dlsym(m_platformLibrary, "AImageReader_delete"));
            if (imageReaderDelete) {
                imageReaderDelete(m_fallbackImageReader);
            }
            m_fallbackImageReader = nullptr;
            m_window = 0;
            dlclose(m_platformLibrary);
            m_platformLibrary = nullptr;
        }
#endif

        if (m_debugMessenger != VK_NULL_HANDLE) {
            DestroyDebugMessenger();
            m_debugMessenger = VK_NULL_HANDLE;
        }
        DestroyDebugReportCallback();

        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
        MGLOG_I("VulkanRenderer shut down completed");
    }

    Bool VulkanRenderer::InitializeBlitResources() {
        // P5f fv gave this an early return for every non-monolith transport, because the hidden
        // programs are frontend objects and creating them on the server would retain a client
        // compiler/allocator dependency. P7 wave 2-B2 (CONTRACT-P7 §5.2, (B')) takes the last
        // step: in a disaggregated build there is NOTHING TO INITIALIZE on any transport,
        // because the monolith arm now blits with WireColorBlit.cpp's baked module too.
        //
        // AN `#if` AND NOT A RUNTIME BRANCH, which is the whole point (§4.2): the early return
        // left every ShaderObject / ProgramObject / SamplerObject symbol REFERENCED by this
        // object file, so the link-closure ratchet counted them whether or not the branch ever
        // ran. Compiling the construction out is what makes them fall.
        return true;
    }

    void VulkanRenderer::ShutdownBlitResources() {
        // P7 wave 2-B2: TWO OF P5e RULING 12'S FOUR APPLY-THREAD ALLOCATOR DEBTS ARE GONE, and
        // the scope that named them with them. The debt was real when it was written: the hidden
        // blit program and its samplers are FRONTEND objects, and under an active transport the
        // server backend created them on the apply thread and destroyed them here on it, where
        // their destructors run the client death helper and its lifetime-id probe.
        //
        // What retires it is not a new exemption but the absence of the objects. P5f fv gave
        // InitializeBlitResources an early return for every non-monolith transport, so off the
        // monolith arm `m_blitResources` never holds anything and this assignment destroys
        // nothing - MEASURED, not argued: a temporary probe here and in
        // ShutdownDepthMipmapResources failed the session if either resource was non-null under
        // a non-monolith transport, and integration-magma-{split,spawn,tcp} (93/72/74),
        // integration-magma-full-{split,spawn} (524/524), integration-split (187) and
        // integration-magma-buffers (21) all stayed green with it armed. Dropping its transport
        // test killed EVERY monolith `DirectVulkan.` entry at teardown and left the split
        // entries green, which is what makes the first run evidence rather than an absence.
        //
        // The TYPE stays: MG_Test/Wire/RemoteClientTest's RemoteGuards
        // .BarrieredLegacyScopesCannotExemptAllocator and
        // .BarrieredFrontendRegistryMembersRefuseBothLegacyScopes use it as a NEGATIVE CONTROL -
        // they construct it on the apply thread and assert the allocator still refuses, which is
        // the thing P5f (fr) changed and the only remaining reason for it to exist.
        m_blitResources = {};
    }

    Bool VulkanRenderer::InitializeDepthMipmapResources() {
        // P7 wave 2-B2 (CONTRACT-P7 §5.2, (B')): the depth-mip half of the same step. In a
        // disaggregated build the monolith arm generates its depth chain with
        // WireDepthMipmap.cpp's baked pass, so there is no hidden GL program to build on any
        // transport. See InitializeBlitResources above for why this is an `#if`.
        return true;
    }

    void VulkanRenderer::ShutdownDepthMipmapResources() {
        // P7 wave 2-B2: the second of the pair, retired for ShutdownBlitResources's reason and
        // proved by the same probe. InitializeDepthMipmapResources carries the same non-monolith
        // early return, so off the monolith arm this assignment destroys nothing.
        m_depthMipmapResources = {};
    }

    void VulkanRenderer::CollectDeferredDepthMipmapCleanup(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_deferredDepthMipmapCleanup.size(),
                        "CollectDeferredDepthMipmapCleanup: frame index %u out of range (size=%zu)", frameIndex,
                        m_deferredDepthMipmapCleanup.size());
        if (m_device == VK_NULL_HANDLE) {
            return;
        }

        auto& cleanup = m_deferredDepthMipmapCleanup[frameIndex];
        for (auto framebuffer : cleanup.framebuffers) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(m_device, framebuffer, nullptr);
            }
        }
        for (auto pipeline : cleanup.pipelines) {
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, pipeline, nullptr);
            }
        }
        for (auto renderPass : cleanup.renderPasses) {
            if (renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(m_device, renderPass, nullptr);
            }
        }
        for (auto imageView : cleanup.imageViews) {
            if (imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, imageView, nullptr);
            }
        }

        cleanup.framebuffers.clear();
        cleanup.pipelines.clear();
        cleanup.renderPasses.clear();
        cleanup.imageViews.clear();
    }

    void VulkanRenderer::DestroyDeferredDepthMipmapCleanup() {
        for (Uint32 frameIndex = 0; frameIndex < m_deferredDepthMipmapCleanup.size(); ++frameIndex) {
            CollectDeferredDepthMipmapCleanup(frameIndex);
        }
        m_deferredDepthMipmapCleanup.clear();
    }

    VkInstance VulkanRenderer::GetInstance() const {
        return m_instance;
    }

    Bool VulkanRenderer::GetWireAhbImport(WireAhbImport& out) const {
        out = WireAhbImport{};
        if (!m_wireAhbImport || m_device == VK_NULL_HANDLE || m_wireGetAhbProperties == nullptr) return false;
        out.device = m_device;
        out.queue = m_graphicsQueue;
        out.queueFamily = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice.handle, &out.memory);
        out.getAhbProperties = m_wireGetAhbProperties;
        return true;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
