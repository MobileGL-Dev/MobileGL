// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRendererPresent.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRendererInternal.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    void VulkanRenderer::Present() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // A lost device presents nothing (its session is latched and closing).
        if (m_deviceLost && LatchWireDeviceLoss("present")) return;
        // Present has its own submit path. Tag its last wire pass before that
        // submission, rather than retaining it until a draw in the next frame.
        RetireWireDrawPass();
        // A window clear no pass drew after still has to reach the presented image; a frame that
        // cannot be presented drops it with the rest of the frame.
        if (m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended)
            m_defaultWirePendingClear = {};
        else
            MaterializeDefaultWirePendingClear();
#endif
        if (m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended) {
            // No usable swapchain: the window was zero-area at initialization, or
            // presentation was suspended when the window minimized. Try to bring a
            // swapchain up now that the window may have a real size; until then, drop
            // this frame's recording instead of submitting - a submit would wait on a
            // never-signaled acquire semaphore and reuse a still-signaled fence.
            if (!RecreateSwapchain()) {
                auto& suspendedFrame = m_frameContext.GetCurrent();
                if (VkRenderPassManager::GetActiveRenderPass()) {
                    VkRenderPassManager::EndRenderPass(suspendedFrame.commandBuffer);
                }
                if (suspendedFrame.isCommandRecording) {
                    m_frameContext.EndCommandRecording();
                }
                m_frameContext.AbandonPreCommandRecording();
                suspendedFrame.isCommandRecording = false;
                suspendedFrame.hasCommandBufferRecorded = false;
                InvalidatePipelineMemo();
#if MOBILEGL_BUILD_DISAGGREGATED
                AbandonSharedImageReads();
#endif
                // The dropped recording is never submitted, so once the fence
                // poll shows the pre-suspension submissions complete the frame
                // transients (descriptor sets, transient arenas, deferred
                // releases, conversion caches) can rewind; without this a
                // minimized-window app accumulates them for the whole
                // suspension.
                TryDrainFrameTransients();
                // The application counted this swap; no image did.
                m_swapchainObject.OnFrameDropped();
                MGLOG_D("Present skipped: no usable swapchain (zero-area window)");
                return;
            }
            m_presentSuspended = false;
            // A slot's Present fence alone does not prove its older pooled
            // flushes retired. Wait the full prefix before FrameContext may
            // free its retired command buffers and reset the slot fence.
            const Uint64 slotSubmit = m_frameContext.GetCurrent().lastSubmitIndex;
            if (slotSubmit > m_completedSubmitCounter && !WaitForSubmitsUpTo(slotSubmit, UINT64_MAX)) {
                if (LatchWireDeviceLoss("deferred-acquire-submit-wait")) return;
                MagmaWireFatal("deferred-acquire-submit-wait");
            }
            const VkResult acquireResult =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
            if (acquireResult == VK_SUBOPTIMAL_KHR) {
                // Usable image with its acquire signal already armed; a rebuild is scheduled
                // only if the surface genuinely no longer matches (see step 4 of Present).
                m_swapchainResizeRequested = m_swapchainResizeRequested || SwapchainIsOutOfDate();
            } else {
                VK_VERIFY(acquireResult, "Present, deferred first WaitAndAcquireNextImage");
            }
        }
        MOBILEGL_ASSERT(m_imageIndexAcquired < m_swapchainObject.GetImageCount(),
                        "Present, acquired image index out of range");
        // A frame that never drew into the window still presents its image: what it recorded
        // goes out first, unwaited, and the present's own submission waits for the release.
        SubmitAheadOfAcquiredImage();
        // A real presented frame is the canonical aging cadence; mid-frame drains
        // count against this and only age when presents stop coming.
        m_drainsSinceLastPresent = 0;
        // Age the content-addressed caches on the same frame-boundary cadence. Each
        // keeps its own internal 256-sweep gate, so the per-frame cost is one counter
        // increment and compare per cache; entries used by this frame's still-
        // unsubmitted recording were stamped this boundary (every command-buffer
        // boundary drops the pipeline memo, so the first draw of each recording
        // performs a real, stamping lookup) and can never age out.
        m_programFactory->OnFrameBoundary();
        if (m_pipelineFactory->OnFrameBoundary() > 0) {
            InvalidatePipelineMemo(); // an aged-out pipeline may still be memoized
            // A recreated pipeline could reuse a freed handle value and alias
            // the bind-dedup shadow; force the next draw to re-bind.
            g_dynamicStateShadow->graphicsPipelineValid = false;
        }
        m_samplerManager->OnFrameBoundary();
        auto& frame = m_frameContext.GetCurrent();
        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        if (activeRenderPass) VkRenderPassManager::EndRenderPass(frame.commandBuffer);
#if MOBILEGL_BUILD_DISAGGREGATED
        // The frame boundary of the shared images this frame read (WireSharedImage.cpp): handed
        // back to the foreign family in this recording, and this submission signals the read
        // fence their next writer waits for.
        ReleaseHeldSharedImages();
        if (m_sharedImageSyncFd && m_sharedImageReads.Pending() > 0 && m_sharedImageSignal == VK_NULL_HANDLE)
            m_sharedImageSignal = TakeSharedImageSemaphore(/*exportable=*/true);
#endif

        // Transition while this frame's recording is still open. A frame that
        // rendered only into FBOs has no default-framebuffer render pass, and that
        // pass's finalLayout is the only other thing that carries the swapchain
        // image to PRESENT_SRC_KHR - so closing the buffer first, which made
        // TransitionToPresent refuse to record, handed the image to
        // vkQueuePresentKHR in the layout it was acquired in (UNDEFINED on a fresh
        // swapchain). The SetImageLayout below then made the tracker's
        // disagreement with reality permanent for that image index.
        const auto acquiredImageLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
        m_frameContext.TransitionToPresent(m_swapchainObject.GetImage(m_imageIndexAcquired), acquiredImageLayout);

        if (frame.isCommandRecording) {
            m_frameContext.EndCommandRecording();
            frame.hasCommandBufferRecorded = true;
            InvalidatePipelineMemo(); // command-buffer boundary: drop the pipeline memo
        }
        m_frameContext.EndPreCommandRecordingIfOpen();

        const Bool shouldSubmitCommandBuffer = frame.hasCommandBufferRecorded;

        // 1) Submit current frame work (the pre-pass stream, when recorded,
        //    rides the same submission strictly ahead of the frame commands).
        //    Batched texture uploads go first: the frame's commands may sample
        //    images whose texels only exist in the open upload batch, and
        //    flushing here also bounds upload latency to one frame.
        if (m_textureManager) {
            m_textureManager->FlushPendingUploads();
        }
        auto submitPacket = m_frameContext.GetSubmitInfo(shouldSubmitCommandBuffer, m_imageIndexAcquired);
#if MOBILEGL_BUILD_DISAGGREGATED
        if (LatchIfGpuHung("present-submit")) return;
        SharedImageSubmitSync sharedImageSync;
        AttachSharedImageSync(submitPacket.submitInfo, sharedImageSync);
        const GpuProgressMarkers::Bracket bracket = m_progressMarkers.Acquire(m_completedSubmitCounter);
        const VkResult presentSubmit = GpuProgressMarkers::SubmitBracketed(m_graphicsQueue, submitPacket.submitInfo,
                                                                           frame.imageInFlightFence, bracket);
        if (presentSubmit == VK_SUCCESS)
            m_progressMarkers.Submitted(bracket, m_submitCounter + 1); // RegisterSubmit below
        else
            m_progressMarkers.Abandon(bracket);
#else
        const VkResult presentSubmit =
            vkQueueSubmit(m_graphicsQueue, 1, &submitPacket.submitInfo, frame.imageInFlightFence);
#endif
        NoteDeviceLoss(presentSubmit, "Present vkQueueSubmit");
        if (presentSubmit != VK_SUCCESS && LatchWireDeviceLoss("present-submit")) return;
        VK_VERIFY(presentSubmit);
        RegisterSubmit(frame.imageInFlightFence, /*pooledFence=*/false);
        frame.lastSubmitIndex = m_submitCounter;
#if MOBILEGL_BUILD_DISAGGREGATED
        CommitSharedImageSync(sharedImageSync);
        PublishSharedImageReads();
#endif
        frame.isCommandRecording = false;
        frame.hasCommandBufferRecorded = false;
        frame.hasPreCommandBufferRecorded = false;
        m_swapchainObject.SetImageLayout(m_imageIndexAcquired, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // 2) Present current frame.
        auto presentPacket = m_frameContext.GetPresentInfo(m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        auto result = vkQueuePresentKHR(m_presentQueue, &presentPacket.presentInfo);
        if (result == VK_SUBOPTIMAL_KHR) {
            // Suboptimal is not a reason to rebuild on its own: a driver may report it for a
            // surface whose size and orientation still match what we built from (Android does
            // this routinely), and rebuilding on it alone destroys every pipeline and
            // reallocates the default framebuffer once per frame - flicker, then garbage.
            // Defer to the surface-capabilities comparison below.
            result = VK_SUCCESS;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            MGLOG_D("Present, vkQueuePresentKHR got %d, recreating swapchain", result);
            if (!RecreateSwapchain()) {
                // Window went zero-area (minimize) with the swapchain out of date:
                // stop submitting/acquiring until it has a size again.
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window with out-of-date swapchain; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
            result = VK_SUCCESS;
        }
        NoteDeviceLoss(result, "vkQueuePresentKHR");
        if (result != VK_SUCCESS && LatchWireDeviceLoss("present-queue")) return;
        VK_VERIFY(result, "Present, vkQueuePresentKHR");
        // EGL swap semantics: the presented color buffer's content is undefined the
        // next time this image is acquired (EGL_BUFFER_DESTROYED, the default swap
        // behavior), and EVERY ancillary depth/stencil buffer's content is
        // undefined after any swap. The render-pass manager turns the undefined
        // attachments' next tile loads into LOAD_OP_DONT_CARE.
        //
        // EGL_EXT_buffer_age changes the first half: once the client has asked for an age it
        // repaints only what changed since the image it is given was last shown, so a presented
        // image keeps its content (the wire passes load it; Vulkan preserves an image's content
        // through a present and a layout transition out of PRESENT_SRC) and its age is counted.
        m_swapchainObject.OnImagePresented(m_imageIndexAcquired);
        if (!m_swapchainObject.BufferAgeAsked()) m_swapchainObject.SetImageContentDefined(m_imageIndexAcquired, false);
        m_swapchainObject.SetAllDepthStencilContentUndefined();
        // The authoritative check, done here - after the frame is presented, before the next
        // acquire. This is what makes a launcher-side resolution change take effect: shrinking
        // the window's buffer (SurfaceHolder.setFixedSize) moves currentExtent, the swapchain
        // follows, and the compositor scales the smaller image up to the view for free.
        if (!m_swapchainResizeRequested && SwapchainIsOutOfDate()) {
            m_swapchainResizeRequested = true;
        }
        // A new eglSwapInterval lands here too, between frames: this one is on screen and the
        // next has recorded nothing. Only a different present mode is worth the rebuild (it
        // drops every pipeline); otherwise the live swapchain already paces as asked.
        if (m_config.SwapInterval != m_swapchainSwapInterval) {
            if (SwapchainObject::ChooseSwapchainPresentMode(m_swapchainObject.GetSupportedPresentModes(),
                                                            m_config.SwapInterval) !=
                m_swapchainObject.GetPresentMode()) {
                m_swapchainResizeRequested = true;
            } else {
                MGLOG_D("DirectVulkan: swap interval %d -> %s", *m_config.SwapInterval,
                        SwapchainObject::GetPresentModeName(m_swapchainObject.GetPresentMode()));
                m_swapchainSwapInterval = m_config.SwapInterval;
            }
        }
        if (m_swapchainResizeRequested) {
            MGLOG_D("Present, processing requested swapchain rebuild (resize or swap interval)");
            if (!RecreateSwapchain()) {
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window on requested resize; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
        }

        // 3) Advance frame slot.
        m_frameContext.AdvanceToNext();

        // FrameContext waits then resets the slot fence and frees retired
        // command buffers. A later Present fence is not aggregate proof for
        // earlier pooled flushes, so wait every registered submission in this
        // slot's prefix BEFORE the reset/free happens.
        const Uint64 slotSubmit = m_frameContext.GetCurrent().lastSubmitIndex;
        if (slotSubmit > m_completedSubmitCounter && !WaitForSubmitsUpTo(slotSubmit, UINT64_MAX)) {
            if (LatchWireDeviceLoss("present-slot-submit-wait")) return;
            MagmaWireFatal("present-slot-submit-wait");
        }
        // 4) Wait/reset/acquire for next frame.
        result = m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        if (result == VK_SUBOPTIMAL_KHR) {
            // An image WAS acquired and its signal is armed on this slot's
            // imageAvailableSemaphore, so the frame proceeds normally. Whether a rebuild is
            // actually needed is decided by the surface-capabilities comparison at the next
            // Present - suboptimal alone must not schedule one, or a driver that reports it
            // every frame would rebuild every frame.
            m_swapchainResizeRequested = m_swapchainResizeRequested || SwapchainIsOutOfDate();
            result = VK_SUCCESS;
        } else if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Nothing acquired, nothing signaled: safe to rebuild and re-acquire.
            MGLOG_D("Present, vkAcquireNextImageKHR got %d, recreating swapchain", result);
            if (!RecreateSwapchain()) {
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window on next-frame acquire; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
            result =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        }
        NoteDeviceLoss(result, "vkAcquireNextImageKHR");
        if (result != VK_SUCCESS && LatchWireDeviceLoss("present-acquire")) return;
        VK_VERIFY(result, "Present, vkAcquireNextImageKHR");
        // Pull keeps its historical slot-fence inference. In a disaggregated
        // build the aggregate wait above already retired the registered prefix
        // before FrameContext reset the slot fence; this repeat is idempotent.
        OnSubmitsCompletedUpTo(m_frameContext.GetCurrent().lastSubmitIndex);
        CollectDeferredDepthMipmapCleanup(m_frameContext.GetCurrentFrameIndex());
        // Its previous recording has completed. Drop cached attachment views
        // before the texture manager can release their retired images.
        ClearWireDrawPassCache(m_frameContext.GetCurrentFrameIndex());
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        // Descriptor-set reuse cursors rewind exactly once per frame, here,
        // after the slot's fence wait proved its previous sets GPU-idle. (The
        // per-draw-path lazy rewind missed frames whose recording was opened
        // by a staged buffer copy or timer-query timestamp, leaking a fresh
        // descriptor set per draw for the whole frame; it would also be unsafe
        // after a mid-frame FlushPendingCommands, which does not wait.)
        m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
    }

    Int32 VulkanRenderer::CurrentDrawBufferAge() {
        if (m_surface == VK_NULL_HANDLE || m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended)
            return 0;
        m_swapchainObject.NoteBufferAgeAsked();
        if (m_imageIndexAcquired >= m_swapchainObject.GetImageCount()) return 0;
        // The image the next frame's first draw makes the default framebuffer's (it was acquired at
        // the end of the last present). Its content is a frame only if nothing undefined it since.
        if (!m_swapchainObject.IsImageContentDefined(m_imageIndexAcquired)) return 0;
        return m_swapchainObject.BufferAgeOf(m_imageIndexAcquired);
    }

    void VulkanRenderer::ShutdownSwapchain() {
        MOBILEGL_ASSERT(m_renderPassManager != nullptr, "ShutdownSwapchain: render pass manager is null");
        m_renderPassManager->Shutdown();

        m_swapchainObject.Shutdown(m_device);
    }

    Bool VulkanRenderer::RecreateSwapchain() {
        // No surface target is active (its surface was released): nothing to build on.
        if (m_surface == VK_NULL_HANDLE) return false;
        // Handle cases like minimize on Windows, where swapchain could return a 0x0 extent
        const auto swapchainCapabilities =
            SwapchainObject::GetSwapchainCapabilities(m_physicalDevice.handle, m_surface);
        if (swapchainCapabilities.capabilities.currentExtent.width == 0 ||
            swapchainCapabilities.capabilities.currentExtent.height == 0) {
            return false;
        }

        vkDeviceWaitIdle(m_device);
        OnSubmitsCompletedUpTo(m_submitCounter);
        // The old swapchain will be destroyed below, including recordings that
        // are being abandoned instead of submitted. Release all their views now.
        DestroyWireDrawPass();
        CollectWireObjects(m_submitCounter, true);
        ClearAllWireDrawPassCaches();

        if (m_timerQueryManager) {
            // The in-progress command buffer is abandoned below (its recording
            // flags are force-cleared), so timestamp writes recorded into it
            // will never execute; resolve or invalidate all pending records now
            // to keep later waits from hanging on never-available queries.
            m_timerQueryManager->InvalidatePendingRecords();
        }

        DestroyDeferredDepthMipmapCleanup();
        m_deferredDepthMipmapCleanup.assign(m_frameContext.GetFrameCount(), {});

        const VkExtent2D previousExtent = m_swapchainObject.GetSurfaceExtent();
        ShutdownSwapchain();

        // The new swapchain publishes its extent (SwapchainObject::Create), which is how a client
        // sizing its viewport from eglQuerySurface learns that the window moved.
        CreateSwapchain();
        const VkExtent2D extent = m_swapchainObject.GetSurfaceExtent();
        if (extent.width != previousExtent.width || extent.height != previousExtent.height) {
            MGLOG_I("DirectVulkan: swapchain rebuilt at %ux%u (was %ux%u); the new extent is published", extent.width,
                    extent.height, previousExtent.width, previousExtent.height);
        }
        // Every image of the fresh swapchain holds garbage, so the default framebuffer restarts
        // at index 0: the next write into it re-points the GL-visible index at whatever the next
        // acquire returns (see m_defaultFramebufferImageIndex). Without this the index could name
        // an image of the swapchain just destroyed.
        m_defaultFramebufferImageIndex = 0;
        VK_VERIFY(m_frameContext.InitializeSwapchainSemaphores(m_device,
                                                               static_cast<Uint32>(m_swapchainObject.GetImageCount())),
                  "RecreateSwapchain, InitializeSwapchainSemaphores");
        MOBILEGL_ASSERT(m_renderPassManager != nullptr, "RecreateSwapchain: render pass manager is null");
        Bool ok = m_renderPassManager->Initialize();
        MOBILEGL_ASSERT(ok, "RecreateSwapchain: render pass manager initialization failed");
        if (m_pipelineFactory) {
            m_pipelineFactory->DestroyAll();
        }
        InvalidatePipelineMemo(); // pipelines freed -> the memoized handle would dangle
        g_dynamicStateShadow->graphicsPipelineValid = false;
        DestroyComputePipelines();
        if (m_frameContext.GetFrameCount() > 0) {
            m_frameContext.GetCurrent().isCommandRecording = false;
            m_frameContext.GetCurrent().hasCommandBufferRecorded = false;
            // The pre-pass stream paired with the abandoned recording is
            // dropped with it (its next Begin resets the buffer).
            m_frameContext.GetCurrent().isPreCommandRecording = false;
            m_frameContext.GetCurrent().hasPreCommandBufferRecorded = false;
        }
        const Bool okArena = m_bufferManager.RecreateTransientArenas(m_frameContext.GetFrameCount());
        MOBILEGL_ASSERT(okArena, "RecreateSwapchain: buffer manager transient arena initialization failed");
        if (m_frameContext.GetFrameCount() > 0) {
            if (m_textureManager) {
                m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
            }
            m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }
        return true;
    }

    const PhysicalDevice& VulkanRenderer::GetPhysicalDevice() const {
        return m_physicalDevice;
    }

    Bool VulkanRenderer::SwapchainIsOutOfDate() {
        if (m_surface == VK_NULL_HANDLE || m_swapchainObject.GetHandle() == VK_NULL_HANDLE) {
            return false;
        }
        VkSurfaceCapabilitiesKHR surfaceCaps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice.handle, m_surface, &surfaceCaps) != VK_SUCCESS) {
            return false;
        }
        // A driver-defined currentExtent (UINT32_MAX) means the surface takes its size from the
        // swapchain, so there is nothing to compare against - the app's requested size wins and
        // only an explicit RequestSwapchainResize can change it.
        if (surfaceCaps.currentExtent.width == UINT32_MAX || surfaceCaps.currentExtent.height == UINT32_MAX) {
            return false;
        }
        // Compare in SURFACE space against the extent the live swapchain was created from. Using
        // the swapchain's own (quarter-turn swapped) extent here would report a difference on
        // every rotated frame and rebuild forever.
        const VkExtent2D builtFrom = m_swapchainObject.GetSurfaceExtent();
        const Bool extentChanged =
            surfaceCaps.currentExtent.width != builtFrom.width || surfaceCaps.currentExtent.height != builtFrom.height;
        const Bool transformChanged = surfaceCaps.currentTransform != m_swapchainObject.GetPreTransform();
        if (!extentChanged && !transformChanged) {
            return false;
        }
        MGLOG_D("Swapchain out of date: surface %ux%u transform %u -> %ux%u transform %u", builtFrom.width,
                builtFrom.height, static_cast<Uint32>(m_swapchainObject.GetPreTransform()),
                surfaceCaps.currentExtent.width, surfaceCaps.currentExtent.height,
                static_cast<Uint32>(surfaceCaps.currentTransform));
        return true;
    }

    void VulkanRenderer::RequestSwapchainResize(Uint32 width, Uint32 height) {
        width = std::max<Uint32>(width, 1);
        height = std::max<Uint32>(height, 1);
        if (m_config.SurfaceWidth == width && m_config.SurfaceHeight == height) {
            return;
        }
        m_config.SurfaceWidth = width;
        m_config.SurfaceHeight = height;
        m_swapchainResizeRequested = true;
    }

    void VulkanRenderer::SetSwapInterval(Int interval) {
        if (m_presentsToAppWindow) {
            m_config.SwapInterval = interval;
        }
    }

    void VulkanRenderer::ExchangeActiveTarget(SurfaceTarget& target) {
        std::swap(m_activeTargetSerial, target.serial);
        std::swap(m_window, target.window);
        std::swap(m_platformDisplay, target.platformDisplay);
        std::swap(m_platformLibrary, target.platformLibrary);
        std::swap(m_platformCloseDisplay, target.platformCloseDisplay);
        std::swap(m_fallbackImageReader, target.fallbackImageReader);
        std::swap(m_config.SurfaceWidth, target.surfaceWidth);
        std::swap(m_config.SurfaceHeight, target.surfaceHeight);
        std::swap(m_config.SwapInterval, target.swapInterval);
        std::swap(m_presentsToAppWindow, target.presentsToAppWindow);
        std::swap(m_swapchainSwapInterval, target.swapchainSwapInterval);
        std::swap(m_swapchainResizeRequested, target.swapchainResizeRequested);
        std::swap(m_presentSuspended, target.presentSuspended);
        std::swap(m_surface, target.surface);
        std::swap(m_swapchainObject, target.swapchain);
        m_frameContext.ExchangeSwapchainSemaphores(target.renderFinishedSemaphores);
        std::swap(m_imageIndexAcquired, target.imageIndexAcquired);
        std::swap(m_defaultFramebufferImageIndex, target.defaultFramebufferImageIndex);
    }

    void VulkanRenderer::ParkActiveTarget() {
        if (m_surface == VK_NULL_HANDLE) return;
        // The active surface's recorded work goes to the queue now, while its images are still
        // the default framebuffer's.
        FlushPendingCommands();
        auto& frame = m_frameContext.GetCurrent();
        // Its acquired image keeps its acquire for when the surface comes back, but the signal
        // lives on THIS slot's semaphore, which the next surface acquires with. Wait it out with
        // an empty submission; the image is then simply held, and its next submit waits nothing.
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE && !m_presentSuspended &&
            !frame.imageAvailableSemaphoreConsumed) {
            const VkFence fence = AcquirePooledSubmitFence();
            if (fence == VK_NULL_HANDLE ||
                !SubmitPendingCommandBuffer(frame, fence, /*pooledFence=*/true, /*waitAcquire=*/true)) {
                MGLOG_E("DirectVulkan: a parked surface's pending acquire could not be waited out");
            }
        }
        SurfaceTarget parked;
        ExchangeActiveTarget(parked);
        m_parkedTargets[m_activeTargetKey] = std::move(parked);
        m_activeTargetKey = 0;
    }

    void VulkanRenderer::OnActiveTargetChanged() {
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE) {
            m_swapchainObject.PublishDefaultFramebufferInfo();
            if (m_programFactory) m_programFactory->SetDefaultFramebufferHeight(m_swapchainObject.GetExtent().height);
        }
        if (m_renderPassManager) m_renderPassManager->SetDefaultFramebufferTarget(m_activeTargetSerial);
        InvalidatePipelineMemo();
        ResetDynamicStateShadow();
    }

    Bool VulkanRenderer::ActivateSurfaceTarget(Uint64 key, NativeWindowType window,
                                               const VulkanRendererConfig& surfaceConfig) {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) return true;
        ParkActiveTarget();
        auto parked = m_parkedTargets.find(key);
        if (parked != m_parkedTargets.end()) {
            ExchangeActiveTarget(parked->second);
            m_parkedTargets.erase(parked);
            // Its acquire was waited when it was parked (ParkActiveTarget).
            m_frameContext.GetCurrent().imageAvailableSemaphoreConsumed = true;
        } else {
            m_activeTargetSerial = m_nextTargetSerial++;
            m_window = window;
            m_presentsToAppWindow = window != NativeWindowType{};
            m_config.SurfaceWidth = surfaceConfig.SurfaceWidth;
            m_config.SurfaceHeight = surfaceConfig.SurfaceHeight;
            m_config.SwapInterval = m_presentsToAppWindow ? surfaceConfig.SwapInterval : Nullopt;
            m_swapchainSwapInterval.reset();
            m_swapchainResizeRequested = false;
            m_presentSuspended = false;
            if (!CreateSurface()) {
                // Nothing of the half-built target stays active: its image reader and library go
                // with it, and the renderer has no target until the caller activates another.
                SurfaceTarget failed;
                ExchangeActiveTarget(failed);
                DestroyParkedTarget(failed);
                m_activeTargetKey = 0;
                MGLOG_E("DirectVulkan: surface target for key %llu could not be built (%s)",
                        static_cast<unsigned long long>(key), window != NativeWindowType{} ? "window" : "pbuffer");
                return false;
            }
            CreateSwapchain();
            m_defaultFramebufferImageIndex = 0;
            VK_VERIFY(m_frameContext.InitializeSwapchainSemaphores(
                          m_device, static_cast<Uint32>(m_swapchainObject.GetImageCount())),
                      "ActivateSurfaceTarget, InitializeSwapchainSemaphores");
            if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE) {
                VkResult result = m_frameContext.AcquireNextImageInCurrentSlot(m_device, m_swapchainObject.GetHandle(),
                                                                               m_imageIndexAcquired);
                if (result == VK_SUBOPTIMAL_KHR) result = VK_SUCCESS;
                VK_VERIFY(result, "ActivateSurfaceTarget, AcquireNextImageInCurrentSlot");
                m_defaultFramebufferImageIndex = m_imageIndexAcquired;
            } else {
                m_presentSuspended = true;
            }
            MGLOG_I("DirectVulkan: surface target %llu built (%ux%u, %s)",
                    static_cast<unsigned long long>(m_activeTargetSerial), m_swapchainObject.GetExtent().width,
                    m_swapchainObject.GetExtent().height, m_presentsToAppWindow ? "window" : "pbuffer");
        }
        m_activeTargetKey = key;
        OnActiveTargetChanged();
        return true;
    }

    void VulkanRenderer::DestroyParkedTarget(SurfaceTarget& target) {
        target.swapchain.Shutdown(m_device);
        for (VkSemaphore semaphore : target.renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_device, semaphore, nullptr);
        }
        target.renderFinishedSemaphores.clear();
        if (target.surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, target.surface, nullptr);
            target.surface = VK_NULL_HANDLE;
        }
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (target.fallbackImageReader != nullptr && target.platformLibrary != nullptr) {
            using AImageReaderDeleteFn = void (*)(void*);
            auto* imageReaderDelete =
                reinterpret_cast<AImageReaderDeleteFn>(dlsym(target.platformLibrary, "AImageReader_delete"));
            if (imageReaderDelete) imageReaderDelete(target.fallbackImageReader);
            target.fallbackImageReader = nullptr;
            dlclose(target.platformLibrary);
            target.platformLibrary = nullptr;
        }
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
        if (target.platformDisplay != nullptr) {
            using XCloseDisplayFn = int (*)(Display*);
            auto* closeDisplay = reinterpret_cast<XCloseDisplayFn>(target.platformCloseDisplay);
            if (closeDisplay) closeDisplay(static_cast<Display*>(target.platformDisplay));
            target.platformDisplay = nullptr;
        }
        if (target.platformLibrary != nullptr) {
            dlclose(target.platformLibrary);
            target.platformLibrary = nullptr;
        }
#endif
    }

    void VulkanRenderer::DestroySurfaceTarget(Uint64 key) {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) ParkActiveTarget();
        auto parked = m_parkedTargets.find(key);
        if (parked == m_parkedTargets.end()) return;
        // Whatever is recorded may name the target's images (a blit out of it, say): submit it,
        // then let the GPU finish with everything before the views go.
        FlushPendingCommands();
        VK_VERIFY(vkDeviceWaitIdle(m_device));
        OnSubmitsCompletedUpTo(m_submitCounter);
        DestroyWireDrawPass();
        CollectWireObjects(m_submitCounter, true);
        ClearAllWireDrawPassCaches();
        InvalidatePipelineMemo();
        ResetDynamicStateShadow();
        DestroyParkedTarget(parked->second);
        m_parkedTargets.erase(parked);
    }

    Bool VulkanRenderer::FollowActiveWindowResize() {
        if (m_surface == VK_NULL_HANDLE || !m_presentsToAppWindow) return false;
        const auto& frame = m_frameContext.GetCurrent();
        if (frame.isCommandRecording || frame.hasCommandBufferRecorded || frame.isPreCommandRecording ||
            frame.hasPreCommandBufferRecorded) {
            m_swapchainResizeRequested = true;
            MGLOG_I("DirectVulkan: the window resized mid-frame; its swapchain is rebuilt at this frame's present");
            return true;
        }
        // The surface already reports the swapchain's extent: nothing to rebuild.
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE && !SwapchainIsOutOfDate()) return true;
        // The target's own rebuild - the path a surface's re-activation takes: its pending acquire waited
        // out, a new VkSurfaceKHR and swapchain at the surface's current extent and pre-transform, a new
        // target serial for the render passes, and the extent published.
        const Uint64 key = m_activeTargetKey;
        const NativeWindowType window = m_window;
        VulkanRendererConfig config;
        config.SurfaceWidth = m_config.SurfaceWidth;
        config.SurfaceHeight = m_config.SurfaceHeight;
        config.SwapInterval = m_config.SwapInterval;
        DestroySurfaceTarget(key);
        return ActivateSurfaceTarget(key, window, config);
    }

    VkExtent2D VulkanRenderer::SurfaceTargetExtent(Uint64 key) const {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) {
            return m_swapchainObject.GetHandle() != VK_NULL_HANDLE ? m_swapchainObject.GetSurfaceExtent()
                                                                   : VkExtent2D{0, 0};
        }
        const auto parked = m_parkedTargets.find(key);
        if (parked == m_parkedTargets.end() || parked->second.swapchain.GetHandle() == VK_NULL_HANDLE) {
            return VkExtent2D{0, 0};
        }
        return parked->second.swapchain.GetSurfaceExtent();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
