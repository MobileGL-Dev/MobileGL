// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRendererSubmit.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRendererInternal.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    VkCommandBuffer VulkanRenderer::AcquireBufferCopyCommandBuffer() {
        if (m_device == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0) {
            return VK_NULL_HANDLE;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        // vkCmdCopyBuffer must be recorded outside a render pass; draws re-begin
        // their render pass lazily, matching the existing blit/clear pattern.
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }
        return frame.commandBuffer;
    }

    Bool VulkanRenderer::IsFrameSerialComplete(Uint64 serial) const {
        return serial <= m_bufferManager.GetCompletedSerial();
    }

    Bool VulkanRenderer::WaitForFrameSerial(Uint64 serial, Uint64 timeoutNs) {
        (void)timeoutNs;
        if (IsFrameSerialComplete(serial)) {
            return true;
        }
        // Work recorded under the current serial has not been submitted yet
        // (submission happens in Present, on this same thread), so blocking
        // can never make progress; the caller reports a timeout instead.
        if (serial >= m_bufferManager.GetFrameSerial()) {
            return false;
        }
        if (m_device == VK_NULL_HANDLE || m_graphicsQueue == VK_NULL_HANDLE) {
            return IsFrameSerialComplete(serial);
        }
        // More than one submit can carry the same frame serial (mid-frame flush,
        // then Present). The completed floor is clamped below any serial still
        // held by an in-flight record; the first matching fence is not proof.
        // OnSubmitsCompletedUpTo erases records, so copy fields and restart the
        // search after every wait instead of continuing an invalidated iterator.
        for (;;) {
            const auto record =
                std::find_if(m_inFlightSubmits.begin(), m_inFlightSubmits.end(), [serial](const auto& candidate) {
                    return candidate.frameSerial >= serial && candidate.fence != VK_NULL_HANDLE;
                });
            if (record == m_inFlightSubmits.end()) break;
            const Uint64 submitIndex = record->submitIndex;
            if (!WaitForSubmitsUpTo(submitIndex, UINT64_MAX)) break; // fall through to the queue drain below
            TryDrainFrameTransients();
            if (IsFrameSerialComplete(serial)) return true;
        }

        // No usable record - fall back to draining the graphics queue. This over-waits (bounded by
        // the in-flight frame count) but never deadlocks.
        const VkResult result = vkQueueWaitIdle(m_graphicsQueue);
        if (result != VK_SUCCESS) {
            NoteDeviceLoss(result, "WaitForFrameSerial vkQueueWaitIdle");
            MGLOG_E_ONCE("WaitForFrameSerial: vkQueueWaitIdle returned %d", result);
            return false;
        }
        m_bufferManager.NotifyDeviceIdle();
        OnSubmitsCompletedUpTo(m_submitCounter);
        // The queue was just drained; take the free frame-boundary drain when
        // nothing is recorded (present-less timer-query loops). No-op otherwise.
        TryDrainFrameTransients();
        return IsFrameSerialComplete(serial);
    }

    Uint64 VulkanRenderer::GetSyncPointSubmitIndex() const {
        // Commands recorded (or still recording) since the last submission are
        // carried by the NEXT submission; a fence created now must wait for it.
        return m_submitCounter + (HasPendingRecordedWork() ? 1 : 0);
    }

    Bool VulkanRenderer::HasPendingRecordedWork() const {
        if (m_frameContext.GetFrameCount() == 0) {
            return false;
        }
        const auto& frame = m_frameContext.GetCurrent();
        return frame.isCommandRecording || frame.hasCommandBufferRecorded || frame.isPreCommandRecording ||
               frame.hasPreCommandBufferRecorded;
    }

    Bool VulkanRenderer::RewindWireDescriptorSetsIfDue() {
        if (!m_uniformManager || m_frameContext.GetFrameCount() == 0) return true;
        const Uint32 frameIndex = m_frameContext.GetCurrentFrameIndex();
        if (!m_uniformManager->WireDescriptorSetBudgetReached(frameIndex)) return true;

        // A set cannot be rewritten while a recorded or submitted command
        // buffer might still read it. This wire-only boundary first submits
        // the current recording, then idles the graphics queue before reuse.
        if (HasPendingRecordedWork() && !FlushPendingCommands()) {
            if (LatchWireDeviceLoss("descriptor-rewind-flush")) return false;
            MagmaWireFatal("descriptor-rewind-flush");
        }
        const Uint64 through = m_submitCounter;
        if (through > m_completedSubmitCounter) {
            // B4 has not yet made every old retirement site aggregate-safe.
            // Queue idle proves even a submission a previous single-fence path
            // prematurely removed from m_inFlightSubmits has finished; only
            // then may any layout cursor be rewound and its sets rewritten.
            if (const VkResult idle = vkQueueWaitIdle(m_graphicsQueue); idle != VK_SUCCESS) {
                NoteDeviceLoss(idle, "descriptor rewind vkQueueWaitIdle");
                if (LatchWireDeviceLoss("descriptor-rewind-wait")) return false;
                MagmaWireFatal("descriptor-rewind-wait");
            }
            OnSubmitsCompletedUpTo(through);
        }
        if (HasPendingRecordedWork() || m_completedSubmitCounter < through) MagmaWireFatal("descriptor-rewind-proof");
        const SizeT cachedSets = m_uniformManager->RewindWireDescriptorSets(frameIndex);
        MGLOG_I("Magma descriptor rewind: frame=%u retiredSubmit=%llu budget=%u cached=%zu", frameIndex,
                static_cast<unsigned long long>(through), UniformManager::kWireDescriptorSetBudget, cachedSets);
        return true;
    }

    Bool VulkanRenderer::IsSubmitIndexComplete(Uint64 submitIndex) {
        if (submitIndex <= m_completedSubmitCounter) {
            return true;
        }
        if (submitIndex > m_submitCounter) {
            return false; // not even submitted; no point polling fences
        }
        // P15: a fence just seen unsignaled is not polled again for a short while. Callers ask per
        // buffer write (FCL MC 1.21.5: vkGetFenceStatus was 0.020 ms/frame), and every one of them
        // treats "not yet" as "still busy" - rename, defer, report unsignaled - so a late "yes" costs
        // at most kUnsignaledPollQuietNs of extra caution, never correctness.
        constexpr Uint64 kUnsignaledPollQuietNs = 50000;
        const Uint64 now = static_cast<Uint64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (!m_inFlightSubmits.empty() && m_unsignaledPollFront == m_inFlightSubmits.front().submitIndex &&
            now - m_unsignaledPollNs < kUnsignaledPollQuietNs) {
            return false;
        }
        RefreshCompletedSubmits();
        if (submitIndex <= m_completedSubmitCounter) return true;
        if (!m_inFlightSubmits.empty()) {
            m_unsignaledPollFront = m_inFlightSubmits.front().submitIndex;
            m_unsignaledPollNs = now;
        }
        return false;
    }

    void VulkanRenderer::RegisterSubmit(VkFence fence, Bool pooledFence) {
        ++m_submitCounter;
        m_inFlightSubmits.push_back({m_submitCounter, m_bufferManager.GetFrameSerial(), fence, pooledFence});
    }

    void VulkanRenderer::RefreshCompletedSubmits() {
        if (m_device == VK_NULL_HANDLE) {
            return;
        }
        // Prefix-only scan: submissions to a single queue complete in order,
        // and stopping at the first unsignaled fence stays conservative even
        // if they did not.
        while (!m_inFlightSubmits.empty()) {
            // Copy before OnSubmitsCompletedUpTo erases the front record.
            const Uint64 frontIndex = m_inFlightSubmits.front().submitIndex;
            if (vkGetFenceStatus(m_device, m_inFlightSubmits.front().fence) != VK_SUCCESS) {
                break;
            }
            OnSubmitsCompletedUpTo(frontIndex);
        }
    }

    Bool VulkanRenderer::WaitForSubmitsUpTo(Uint64 submitIndex, Uint64 timeoutNs) {
        if (submitIndex <= m_completedSubmitCounter) return true;
        if (submitIndex > m_submitCounter || m_device == VK_NULL_HANDLE) return false;
        Vector<VkFence> fences;
        if (!CollectSubmitFencePrefix(m_inFlightSubmits, submitIndex, VkFence{}, fences)) return false;
        const VkResult result =
            vkWaitForFences(m_device, static_cast<Uint32>(fences.size()), fences.data(), VK_TRUE, timeoutNs);
        if (result == VK_SUCCESS) {
            OnSubmitsCompletedUpTo(submitIndex);
            return true;
        }
        NoteDeviceLoss(result, "WaitForSubmitsUpTo vkWaitForFences");
        if (result != VK_TIMEOUT) MGLOG_E_ONCE("WaitForSubmitsUpTo: vkWaitForFences returned %d", result);
        return false;
    }

    void VulkanRenderer::OnSubmitsCompletedUpTo(Uint64 submitIndex) {
        m_completedSubmitCounter = std::max(m_completedSubmitCounter, submitIndex);
        // ---- P7 wave 2 package B3: THE COMPLETED-FRAME-SERIAL FLOOR MUST BE PROVABLE --------
        //
        // "Frame-serial completion piggybacks on submission completion" holds only while a
        // frame serial has ONE submission. On the wire arm it has at least two: the per-frame
        // palette glTexImage2D reaches UploadPendingWireLevels ->
        // FlushWirePendingCommandsForTextureUpdate -> FlushPendingCommands at the first
        // palette-sampling draw and submits S1 under a POOLED fence, and the frame's draws
        // (and the barriered vkCmdCopyBuffer that StagedWireRangeCopy records for every
        // streamed glBufferSubData) then go out in the Present submission S2.
        //
        // S1 is tiny and signals almost immediately. Retiring it used to advance the floor to
        // ITS frameSerial - which is also S2's - so VkBufferManager was told "frame N-1 is
        // complete" while S2(N-1), the submission actually carrying frame N-1's buffer copies,
        // was still executing. The next frame's first write to that buffer then failed
        // WriteWireBuffer's busy predicate and took the unordered host memcpy, and S2(N-1)'s
        // queued copies landed on top of it afterwards: a PREFIX TEAR of one draw's vertex
        // range, no log line, no GL error, no Fatal.
        //
        // The floor now advances only to a serial NO remaining in-flight submission still
        // carries. NotifyFrameSerialComplete is monotone and already refuses the still-
        // recording serial, so this can only ever advance the floor later, never further.
        // On the disaggregated build's MONOLITH arm this is behaviour-identical (one
        // submission per serial). The #else branch below is the pull build's statement,
        // unchanged, which is what keeps G1's .text byte-identical.
        Bool retiredAny = false;
        Uint64 advanceTo = 0;
        while (!m_inFlightSubmits.empty() && m_inFlightSubmits.front().submitIndex <= submitIndex) {
            SubmitRecord record = m_inFlightSubmits.front();
            m_inFlightSubmits.erase(m_inFlightSubmits.begin());
            retiredAny = true;
            advanceTo = std::max(advanceTo, record.frameSerial);
            if (!record.pooledFence || m_device == VK_NULL_HANDLE) {
                continue; // frame-slot fences are reset/destroyed by FrameContext
            }
            if (vkResetFences(m_device, 1, &record.fence) == VK_SUCCESS) {
                m_freeSubmitFences.push_back(record.fence);
            } else {
                vkDestroyFence(m_device, record.fence, nullptr);
            }
        }
        if (retiredAny) {
            // Clamp to one below the lowest serial still in flight.
            for (const auto& remaining : m_inFlightSubmits) {
                advanceTo = std::min(advanceTo, remaining.frameSerial > 0 ? remaining.frameSerial - 1 : Uint64{0});
            }
            // Lens C's probe, and the red-once's reading: an advance that names a serial some
            // remaining submission still carries. Unreachable with the clamp above; the R-16
            // revert is to delete the clamp loop, and then this counts.
            for (const auto& remaining : m_inFlightSubmits) {
                if (remaining.frameSerial <= advanceTo) {
                    WireDeclineTally::CountUnsoundSerialComplete();
                    // Logged per event, not once: this is the red-once's reading and the log is
                    // the only place it can be read from: the renderer this trace builds is
                    // never Shutdown() (the process exits), so a teardown dump reports the
                    // start-up context's zero instead of the replay's count.
                    MGLOG_W("MGWIRE-FLOOR unsound-serial-complete #%llu: advancing the completed "
                            "frame-serial floor to %llu, which submission %llu (serial %llu) still carries",
                            static_cast<unsigned long long>(WireDeclineTally::UnsoundSerialCompleteEvents()),
                            static_cast<unsigned long long>(advanceTo),
                            static_cast<unsigned long long>(remaining.submitIndex),
                            static_cast<unsigned long long>(remaining.frameSerial));
                    break;
                }
            }
            m_bufferManager.NotifyFrameSerialComplete(advanceTo);
        }
        // Mid-frame-flushed command buffers whose submission just completed can
        // be freed now; present-less flush loops have no other reclaim point.
        m_frameContext.FreeRetiredCommandBuffersCompletedUpTo(m_completedSubmitCounter);
        CollectWireObjects(m_completedSubmitCounter);
#if MOBILEGL_BUILD_DISAGGREGATED
        RecycleSharedImageSemaphores(m_completedSubmitCounter);
#endif
    }

    Bool VulkanRenderer::TryDrainFrameTransients() {
        if (m_wirePreparationDepth != 0) return false;
        if (m_device == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0) {
            return false;
        }
        if (m_completedSubmitCounter != m_submitCounter) {
            RefreshCompletedSubmits();
            if (m_completedSubmitCounter != m_submitCounter) {
                return false;
            }
        }
        if (HasPendingRecordedWork()) {
            return false;
        }

        // Texture uploads submit on this queue with their own fences. The
        // renderer watermark alone cannot prove images/views are idle.
        if (m_textureManager && !m_textureManager->WireUploadsAreIdle()) return false;
        // No preparation, recording or GPU work remains. A minimized Present
        // may have abandoned a recording tagged for a submission that will
        // never occur, so reclaim those future-tagged objects as well.
        CollectWireObjects(m_completedSubmitCounter, true);
        ClearAllWireDrawPassCaches();

        // Every submission is complete and nothing recorded references the
        // per-frame transients. Pure-reclaim work runs on every drain: it only
        // releases memory that is provably dead, never invalidates anything a
        // later draw would have to rebuild. Raise the buffer manager's
        // completed floor first so busy-tracking reflects the proven idleness.
        m_bufferManager.NotifyDeviceIdle();

        const Uint32 frameIndex = m_frameContext.GetCurrentFrameIndex();
        m_frameContext.FreeAllRetiredCommandBuffers();
        for (Uint32 slot = 0; slot < m_deferredDepthMipmapCleanup.size(); ++slot) {
            CollectDeferredDepthMipmapCleanup(slot);
        }
        if (m_textureManager) {
            m_textureManager->CollectAllDeferredReleases();
        }
        m_bufferManager.CollectAllDeferredReleases();
        // Descriptor cursors rewind on every drain (the pre-drain readback path
        // already did exactly this), keeping fence/readback loops' set usage bounded.
        if (m_uniformManager) {
            m_uniformManager->BeginFrame(frameIndex);
        }

        // Frame-boundary-equivalent work - transient arena rewind (which invalidates
        // the conversion cache) and the cache-aging clocks - is gated to every 8th
        // drain since the last Present: a presenting app's mid-frame readbacks/waits
        // must neither force re-conversion/re-upload churn for the rest of the frame
        // nor multiply the aging rate (which would shrink the 1024-boundary retire
        // window and thrash periodically-used pipelines/programs), while present-less
        // loops still rewind the arena and age their caches every 8 iterations -
        // bounded by 8 iterations' transient usage.
        ++m_drainsSinceLastPresent;
        if ((m_drainsSinceLastPresent % 8) != 0) {
            return true;
        }
        if (m_textureManager) {
            m_textureManager->BeginFrame(frameIndex);
        }
        m_bufferManager.BeginFrame(frameIndex);
        // The pipeline memo can survive across these boundaries (no per-frame reset
        // on this path), so it must drop whenever the sweep destroys anything.
        if (m_programFactory) {
            m_programFactory->OnFrameBoundary();
        }
        if (m_pipelineFactory && m_pipelineFactory->OnFrameBoundary() > 0) {
            InvalidatePipelineMemo();
        }
        if (m_samplerManager) {
            m_samplerManager->OnFrameBoundary();
        }
        return true;
    }

    VkFence VulkanRenderer::AcquirePooledSubmitFence() {
        if (!m_freeSubmitFences.empty()) {
            VkFence fence = m_freeSubmitFences.back();
            m_freeSubmitFences.pop_back();
            return fence;
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        const VkResult result = vkCreateFence(m_device, &fenceInfo, nullptr, &fence);
        if (result != VK_SUCCESS) {
            MGLOG_E_ONCE("AcquirePooledSubmitFence: vkCreateFence returned %d", result);
            return VK_NULL_HANDLE;
        }
        return fence;
    }

    void VulkanRenderer::DestroySubmitFencePool() {
        // Callers guarantee device idle, so in-flight fences are inert.
        for (const auto& record : m_inFlightSubmits) {
            if (record.pooledFence && m_device != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, record.fence, nullptr);
            }
        }
        m_inFlightSubmits.clear();
        for (auto fence : m_freeSubmitFences) {
            if (m_device != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, fence, nullptr);
            }
        }
        m_freeSubmitFences.clear();
        m_completedSubmitCounter = m_submitCounter;
    }

    Bool VulkanRenderer::SubmitPendingCommandBuffer(FrameContext::FrameData& frame, VkFence fence, Bool pooledFence,
                                                    Bool waitAcquire) {
        RetireWireDrawPass();
        // Batched texture uploads must reach the queue before the frame's
        // commands: the recording being submitted may sample images whose
        // texels only exist in the texture manager's open upload batch.
        if (m_textureManager) {
            m_textureManager->FlushPendingUploads();
        }
        VkPipelineStageFlags waitDstStageMask = FrameContext::kAcquireWaitStages;
        VkSemaphore waitSemaphore = frame.imageAvailableSemaphore;
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        // A recording that never touched the acquired image does not wait for its release: on a
        // queue that runs submissions in order, that wait would hold this work, and everything
        // after it, until the display lets go of the image (SubmitAheadOfAcquiredImage).
        const Bool waitsAcquire =
            !frame.imageAvailableSemaphoreConsumed && (frame.acquiredImageReferenced || waitAcquire);
        if (waitsAcquire) {
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &waitSemaphore;
            submitInfo.pWaitDstStageMask = &waitDstStageMask;
        }
        // The pre-pass stream, when recorded, executes strictly before the
        // frame's commands within the same submission.
        VkCommandBuffer commandBuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        Uint32 commandBufferCount = 0;
        if (frame.hasPreCommandBufferRecorded) {
            commandBuffers[commandBufferCount++] = frame.preCommandBuffer;
        }
        if (frame.hasCommandBufferRecorded) {
            commandBuffers[commandBufferCount++] = frame.commandBuffer;
        }
        submitInfo.commandBufferCount = commandBufferCount;
        submitInfo.pCommandBuffers = commandBuffers;
#if MOBILEGL_BUILD_DISAGGREGATED
        // Shared-image fences staged for the work recorded since the last submission.
        if (LatchIfGpuHung("flush-submit")) return false;
        SharedImageSubmitSync sharedImageSync;
        AttachSharedImageSync(submitInfo, sharedImageSync);
        const GpuProgressMarkers::Bracket bracket = m_progressMarkers.Acquire(m_completedSubmitCounter);
        const VkResult result = GpuProgressMarkers::SubmitBracketed(m_graphicsQueue, submitInfo, fence, bracket);
        if (result == VK_SUCCESS)
            m_progressMarkers.Submitted(bracket, m_submitCounter + 1); // RegisterSubmit below
        else
            m_progressMarkers.Abandon(bracket);
#else
        const VkResult result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, fence);
#endif
        if (result != VK_SUCCESS) {
            NoteDeviceLoss(result, "SubmitPendingCommandBuffer vkQueueSubmit");
            MGLOG_E_ONCE("SubmitPendingCommandBuffer: vkQueueSubmit returned %d", result);
            return false;
        }
        if (waitsAcquire) frame.imageAvailableSemaphoreConsumed = true;
        frame.hasCommandBufferRecorded = false;
        frame.hasPreCommandBufferRecorded = false;
        RegisterSubmit(fence, pooledFence);
        frame.lastSubmitIndex = m_submitCounter;
#if MOBILEGL_BUILD_DISAGGREGATED
        CommitSharedImageSync(sharedImageSync);
#endif
        return true;
    }

    Bool VulkanRenderer::FlushPendingCommands() {
        if (m_device == VK_NULL_HANDLE || m_graphicsQueue == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0) {
            return false;
        }
        // Non-blocking completion poll: gives flush-only workloads (no sync
        // objects, no present) a point where finished submissions retire their
        // pooled fences and mid-frame command buffers.
        RefreshCompletedSubmits();
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording && !frame.hasCommandBufferRecorded && !frame.isPreCommandRecording &&
            !frame.hasPreCommandBufferRecorded) {
            // GL flush semantics still demand batched texture uploads start
            // executing in finite time even when no draw was recorded.
            if (m_textureManager) {
                m_textureManager->FlushPendingUploads();
            }
            return false;
        }
        // Acquire the fence while recording is still open: failing here must
        // not end recording, or the next draw's BeginCommandRecording would
        // reset the command buffer and silently drop the frame's commands.
        VkFence fence = AcquirePooledSubmitFence();
        if (fence == VK_NULL_HANDLE) {
            return false;
        }
        if (frame.isCommandRecording) {
            if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
                VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            }
            m_frameContext.EndCommandRecording();
        }
        m_frameContext.EndPreCommandRecordingIfOpen();
        const Bool submittingPreCommandBuffer = frame.hasPreCommandBufferRecorded;
        if (!SubmitPendingCommandBuffer(frame, fence, /*pooledFence=*/true)) {
            // Submit failure (device loss regime): the ended command buffer
            // stays marked recorded so Present can still try to submit it.
            m_freeSubmitFences.push_back(fence); // still unsignaled, reusable
            return false;
        }

        // Command-buffer boundary: the pipeline memo must not survive it, or a
        // pipeline bound only through memo hits is never re-stamped in the factory
        // cache and the aging sweep could destroy it while the flushed submission
        // still references it. Mirrors the drops at the readback and Present
        // boundaries; costs one full pipeline lookup on the next draw.
        InvalidatePipelineMemo();

        // The submitted command buffer may still be executing; recording must
        // restart on a fresh one. If none can be allocated, fall back to
        // draining this submission so reusing the buffer stays legal.
        const VkResult retireResult = m_frameContext.RetireCurrentCommandBuffer(submittingPreCommandBuffer);
        if (retireResult != VK_SUCCESS) {
            MGLOG_E_ONCE("FlushPendingCommands: RetireCurrentCommandBuffer returned %d; draining submission",
                         retireResult);
            if (WaitForSubmitsUpTo(m_submitCounter, UINT64_MAX)) {
                // Every registered fence through this submission was waited.
            } else if (vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS) {
                m_bufferManager.NotifyDeviceIdle();
                OnSubmitsCompletedUpTo(m_submitCounter);
            } else {
                // Device is effectively lost; the command buffer may still be
                // pending, but no recovery can make reuse legal.
                MGLOG_E_ONCE("FlushPendingCommands: drain failed; command buffer reuse is unsafe");
            }
        }
        return true;
    }

    void VulkanRenderer::SubmitAheadOfAcquiredImage() {
        if (m_frameContext.GetFrameCount() == 0) return;
        auto& frame = m_frameContext.GetCurrent();
        if (frame.imageAvailableSemaphoreConsumed || frame.acquiredImageReferenced) return;
        if (m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended) return;
        // The frame's work so far renders off screen. Submitted together with the window pass it
        // would wait behind the acquire semaphore, which signals only when the display releases
        // the image - about every other frame that is the image it has just stopped scanning out,
        // so the whole frame's GPU work started after the next display commit. Submitted now it
        // runs while the display still holds the image. A failed flush leaves the work recorded,
        // and it goes out later with the wait, as before. Inside a draw's preparation nothing is
        // submitted (the draw may already hold state of the open recording); the image is only
        // marked, so the submission that carries the use still waits.
        if (m_wirePreparationDepth == 0 && HasPendingRecordedWork() && FlushPendingCommands())
            MGLOG_I_ONCE("DirectVulkan: a frame's off-screen work goes to the queue ahead of its window pass; only "
                         "the submission that uses the swapchain image waits for its acquire");
        frame.acquiredImageReferenced = true;
    }

    void VulkanRenderer::NoteDeviceLoss(VkResult result, const char* where) {
        if (result != VK_ERROR_DEVICE_LOST || m_deviceLost) return;
        m_deviceLost = true;
        MGLOG_E("DirectVulkan: the VkDevice is LOST - %s returned VK_ERROR_DEVICE_LOST (the driver reset a GPU "
                "fault or hang); nothing this renderer recorded or submits from here on will execute",
                where != nullptr ? where : "a Vulkan call");
#if MOBILEGL_BUILD_DISAGGREGATED
        // Latch NOW where a session latch exists, so the session stops at its next record however
        // the verb in hand unwinds - including through a caller that only logs a failed submit.
        if (MG_Pipe::MGPipeSessionLatchArmed()) (void)LatchWireDeviceLoss(where);
#endif
    }

    Bool VulkanRenderer::IsDeviceLost() {
        // Only failure paths ask, so the probe's wait never lands on a healthy frame: a device
        // that is merely slow answers VK_SUCCESS once its work drains, a lost one answers at once.
        if (!m_deviceLost && m_device != VK_NULL_HANDLE) {
            NoteDeviceLoss(vkDeviceWaitIdle(m_device), "the device-loss probe's vkDeviceWaitIdle");
        }
        return m_deviceLost;
    }

    Bool VulkanRenderer::LatchWireDeviceLoss(const char* site) {
        if (!IsDeviceLost()) return false;
        if (!m_deviceLossLatched) {
            m_deviceLossLatched = true;
            // Returns (latched) on a served session; dies, as the site's own Fatal would have,
            // where no session latch is armed.
            (void)MG_Pipe::MGPipeSessionLatch(MG_Pipe::MGPipeFatalFamily::DeviceLost,
                                              "MGPipe: Fatal{BackendDeviceLost, \"Magma:%s\"} - this session's "
                                              "VkDevice was lost to a GPU fault or hang; the session ends and its "
                                              "client reads a lost context, every other session keeps running",
                                              site != nullptr ? site : "?");
        }
        return true;
    }

    Bool VulkanRenderer::LatchIfGpuHung(const char* site) {
        if (!m_progressMarkers.Hung()) return false;
        if (!m_deviceLost) {
            // NOT LOST TO THE DRIVER: a submission that a failed preemption leaves running is reset
            // without a device loss, and the next one would stop the GPU again. Treated as lost from
            // here on - nothing more is submitted - and the session ends as one whose device was.
            m_deviceLost = true;
            MGLOG_E("DirectVulkan: the GPU hang watch named this session's VkDevice (a submission ran %llu ms "
                    "without finishing, at %s); the device is treated as LOST and nothing more is submitted",
                    static_cast<unsigned long long>(m_progressMarkers.HungRunningMs()), site != nullptr ? site : "?");
        }
        // Where no session latch is armed (no served session) the device only stays "lost".
        if (!m_deviceLossLatched && MG_Pipe::MGPipeSessionLatchArmed()) {
            m_deviceLossLatched = true;
            (void)MG_Pipe::MGPipeSessionLatch(MG_Pipe::MGPipeFatalFamily::DeviceLost,
                                              "MGPipe: Fatal{BackendDeviceLost, \"Magma:gpu-hang:%s\"} - this "
                                              "session's GPU work ran past the hang budget and stopped the GPU for "
                                              "every process; the session ends and its client reads a lost context, "
                                              "every other session keeps running",
                                              site != nullptr ? site : "?");
        }
        return true;
    }

    Bool VulkanRenderer::FlushForSyncPoint(Uint64 submitIndex) {
        // A flush only helps a sync point whose commands are not submitted
        // yet; for an already-submitted index it would just split the frame's
        // render pass (a full tile load/store on TBDR GPUs) without advancing
        // the fence.
        if (submitIndex <= m_submitCounter) {
            return false;
        }
        return FlushPendingCommands();
    }

    void VulkanRenderer::SplitOversizedRecording() {
        const Uint32 maxDraws = MG_Config::Features.MagmaMaxDrawsPerCommandBuffer;
        if (maxDraws == 0) {
            return;
        }
        // Counted before the check, so a buffer carries at most maxDraws calls before the one
        // that splits it. OnFrameCommandRecordingBegan resets the count on every new recording.
        if (++m_drawsInRecording <= maxDraws || !HasPendingRecordedWork()) {
            return;
        }
        // Nothing of the current call is recorded yet, so the split falls between two GL calls,
        // exactly where a glFlush would put it: the render pass ends with STORE, the next draw
        // re-begins it with LOAD, and queries and transform feedback are bracketed per draw.
        if (!FlushPendingCommands()) {
            return;
        }
        // Fresh command buffer: the same reset every other mid-frame flush site applies.
        ++m_oversizedRecordingSplits;
        MGLOG_I_ONCE("DirectVulkan: a frame recorded more than %u draws into one command buffer; submitting it and "
                     "continuing on a fresh one (MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER, 0 = unbounded)",
                     maxDraws);
        MGLOG_D("DirectVulkan: split an oversized recording (%llu so far, submit index %llu)",
                static_cast<unsigned long long>(m_oversizedRecordingSplits),
                static_cast<unsigned long long>(m_submitCounter));

        // Splitting only bounds the driver's command memory if the submitted buffers are freed
        // as fast as they are produced. A GPU that falls behind would park them all on the
        // retired list, which is the same growth spread over more buffers - so hold the queue
        // to frames-in-flight + 1 outstanding submissions, the steady state of a pipelined
        // frame plus this split. Wait on the fence directly rather than through
        // WaitForSubmitIndex: its opportunistic TryDrainFrameTransients may rewind the transient
        // arena, and this runs at the start of a draw whose caller may already hold slices of it.
        const SizeT maxOutstanding = static_cast<SizeT>(m_frameContext.GetFrameCount()) + 1;
        while (m_inFlightSubmits.size() > maxOutstanding) {
            const SubmitRecord oldest = m_inFlightSubmits.front();
            if (oldest.fence == VK_NULL_HANDLE) {
                break;
            }
            const VkResult waitResult = vkWaitForFences(m_device, 1, &oldest.fence, VK_TRUE, UINT64_MAX);
            if (waitResult != VK_SUCCESS) {
                NoteDeviceLoss(waitResult, "SplitOversizedRecording vkWaitForFences");
                MGLOG_E_ONCE("SplitOversizedRecording: vkWaitForFences returned %d", waitResult);
                break;
            }
            OnSubmitsCompletedUpTo(oldest.submitIndex);
        }
    }

    Bool VulkanRenderer::WaitForSubmitIndex(Uint64 submitIndex, Uint64 timeoutNs, Bool flushIfPending) {
        if (IsSubmitIndexComplete(submitIndex)) {
            return true;
        }
        if (submitIndex > m_submitCounter) {
            if (!flushIfPending) {
                return false;
            }
            FlushPendingCommands();
            if (submitIndex > m_submitCounter) {
                // Nothing could be submitted (empty batch or submit failure);
                // the index cannot complete yet.
                return false;
            }
        }
        if (!WaitForSubmitsUpTo(submitIndex, timeoutNs)) return false;
        // A blocking wait can drain a present-less loop's frame transients.
        TryDrainFrameTransients();
        return true;
    }

    void VulkanRenderer::OnFrameCommandRecordingBegan(VkCommandBuffer commandBuffer) {
        m_drawsInRecording = 0;
        // Dynamic state does not survive a command-buffer boundary.
        ResetDynamicStateShadow();
        if (m_uniformManager) {
            m_uniformManager->OnCommandBufferBoundary();
        }
        // Pre-pass stream bookkeeping: a fresh frame recording references no
        // textures yet.
        if (m_textureManager) {
            m_textureManager->AdvanceRecordingGeneration();
        }
        if (m_timerQueryManager) {
            m_timerQueryManager->OnFrameCommandRecordingBegan(commandBuffer, m_frameContext.GetCurrentFrameIndex(),
                                                              m_bufferManager.GetFrameSerial());
        }
    }

    VkProvokingVertexModeEXT VulkanRenderer::SelectProvokingVertexMode(VkPrimitiveTopology topology,
                                                                       Bool capturesXfbFromGeometryStage) const {
        if (!m_provokingVertexLastEnabled) {
            return VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
        }
        // Measured, and identical on lavapipe and on the NVIDIA Vulkan driver: a geometry shader's
        // emitted triangle strip is already recorded in GL's provoking-last vertex order, so asking
        // for LAST rotates it a second time. The input-assembler path has the opposite problem, and
        // the mode is a single pipeline bit, so the two cannot be satisfied at once: a program that
        // both runs a geometry shader and captures transform feedback keeps Vulkan's own convention,
        // and pays for it with a GL-wrong flat vertex in that one case. Deliberately a link-time
        // program property, not IsTransformFeedbackActive() - see the memo note in the header.
        if (capturesXfbFromGeometryStage) {
            return VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
        }
        // VUID-VkGraphicsPipelineCreateInfo-topology-04884 only bites when
        // transformFeedbackPreservesProvokingVertex is enabled; when it is not, a fan may take LAST.
        if (m_provokingVertexXfbPreserveEnabled && topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN &&
            !m_provokingVertexFanPreserved) {
            return VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
        }
        // Only provokingVertexModePerPipeline lets modes differ inside one render pass instance;
        // elsewhere every pipeline takes GL's default so the render pass stays self-consistent, and
        // glProvokingVertex(GL_FIRST_VERTEX_CONVENTION) goes unhonoured. Honouring it there would
        // mean ending the render pass on every glProvokingVertex change; not worth it until a target
        // device actually lacks the property.
        if (!m_provokingVertexModePerPipeline) {
            return VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
        }
        return (MG_Pipe::gPipeInputs->IsLive() &&
                MG_Pipe::gPipeInputs->GetProvokingVertexMode() == ProvokingVertexMode::FirstVertex)
                   ? VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT
                   : VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    }

    Bool VulkanRenderer::IsTimerQuerySupported() const {
        return m_timerQuerySupported && m_timerQueryManager != nullptr;
    }

    SharedPtr<VkTimerQueryManager::TimestampRecord> VulkanRenderer::WriteTimerQueryTimestamp() {
        if (!IsTimerQuerySupported() || m_device == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0) {
            return nullptr;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        // vkCmdWriteTimestamp is valid both inside and outside a render pass,
        // so any active render pass is left untouched.
        return m_timerQueryManager->WriteTimestamp(frame.commandBuffer, m_frameContext.GetCurrentFrameIndex(),
                                                   m_bufferManager.GetFrameSerial());
    }

    Bool VulkanRenderer::IsTimerQueryResultReady(VkTimerQueryManager::TimestampRecord& record) {
        if (record.harvested) {
            return true;
        }
        if (!m_timerQueryManager) {
            return false;
        }
        // Ask the pool first. It polls with VK_QUERY_RESULT_WITH_AVAILABILITY_BIT and is the
        // authority on whether the timestamp has landed; the frame serial is not, because it only
        // advances at Present and neither completion notifier will mark the CURRENT serial done - so
        // a timestamp written and fence-waited inside one GL frame could never be read back in it.
        if (m_timerQueryManager->TryHarvest(record)) {
            return true;
        }
        return IsFrameSerialComplete(record.frameSerial) && m_timerQueryManager->TryHarvest(record);
    }

    Bool VulkanRenderer::WaitForTimerQueryResult(VkTimerQueryManager::TimestampRecord& record) {
        if (IsTimerQueryResultReady(record)) {
            return true;
        }
        // WaitForFrameSerial refuses serials that cannot complete without
        // further submissions (a timestamp written this frame only executes
        // once Present submits the command buffer), so this returns false
        // instead of deadlocking; the record resolves after a later Present.
        if (!WaitForFrameSerial(record.frameSerial, UINT64_MAX)) {
            return false;
        }
        return IsTimerQueryResultReady(record);
    }

    Uint64 VulkanRenderer::GetTimerQueryElapsedNs(const VkTimerQueryManager::TimestampRecord& begin,
                                                  const VkTimerQueryManager::TimestampRecord& end) const {
        return m_timerQueryManager ? m_timerQueryManager->ElapsedNs(begin, end) : 0;
    }

    Uint64 VulkanRenderer::GetTimerQueryTimestampNs(const VkTimerQueryManager::TimestampRecord& record) const {
        return m_timerQueryManager ? m_timerQueryManager->TimestampNs(record) : 0;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
