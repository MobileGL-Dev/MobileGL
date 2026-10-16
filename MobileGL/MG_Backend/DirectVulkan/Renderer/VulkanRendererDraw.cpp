// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRendererDraw.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRendererInternal.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    // Keyed on the frontend's never-reused lifetime id, NOT on the GL name. The name is
    // recycled the moment glDeleteTransformFeedbacks gives it back, so a name-keyed slot
    // handed a brand-new object the counter group - and the m_xfbCountersValid /
    // m_xfbLastSeenGeneration entries - of the object that died under that name.
    //
    // Slots are never handed back (there is no backend entry telling this renderer that a span
    // closed - registering the EndTransformFeedback one would flip the "captures through its own
    // driver" test FixupGsStripCaptureOrder makes of it), so once all sixteen are owned a new
    // object has to take one over. The victim is chosen among owners with NO OPEN SPAN: an object
    // whose span is closed, or which no longer exists at all, can never resume, so its counter
    // bytes are dead. Least-recently-used ALONE would be exactly the wrong rule - GL only permits
    // another object to capture while this one is PAUSED, so the paused span whose counters the
    // slots exist to protect is by construction the least recently used entry. Taking a group over
    // resets its counter state, because those bytes describe the previous owner's span.
    Uint32 VulkanRenderer::CurrentXfbCounterSlot() {
        constexpr Uint32 kNoSlot = static_cast<Uint32>(kXfbCounterObjectSlots);
        const Uint64 identity = MG_Pipe::gPipeInputs->GetBoundTransformFeedbackLifetimeId();
        MOBILEGL_ASSERT(identity != 0,
                        "transform feedback object reported the free-slot sentinel (0) as its identity - "
                        "every slot would then read as 'mine' without ever being claimed");
        Uint32 freeSlot = kNoSlot;
        for (Uint32 slot = 0; slot < kNoSlot; ++slot) {
            if (m_xfbCounterSlotOwner[slot] == identity) {
                m_xfbCounterSlotLastUse[slot] = ++m_xfbCounterSlotUseSerial;
                return slot;
            }
            if (m_xfbCounterSlotOwner[slot] == 0 && freeSlot == kNoSlot) {
                freeSlot = slot;
            }
        }
        Uint32 slot = freeSlot;
        if (slot == kNoSlot) {
            for (Uint32 candidate = 0; candidate < kNoSlot; ++candidate) {
                if (m_xfbCounterSlotOwner[candidate] != 0 &&
                    MG_Pipe::MGPipeApplier().StreamOutputSpans.count(m_xfbCounterSlotOwner[candidate]) != 0) {
                    continue;
                }
                if (slot == kNoSlot || m_xfbCounterSlotLastUse[candidate] < m_xfbCounterSlotLastUse[slot]) {
                    slot = candidate;
                }
            }
        }
        if (slot == kNoSlot) {
            // Sixteen capture spans open at once. Whatever is taken loses its resume offset and
            // restarts at byte 0 of its capture buffers, which is a wrong picture rather than a
            // slow one - hence a report rather than a silent choice.
            MGLOG_E_ONCE("CurrentXfbCounterSlot: all %zu counter groups belong to transform feedback objects "
                         "with an open capture span; the least recently used one is taken over and that span "
                         "will restart at offset 0 instead of appending",
                         kXfbCounterObjectSlots);
            slot = 0;
            for (Uint32 candidate = 1; candidate < kNoSlot; ++candidate) {
                if (m_xfbCounterSlotLastUse[candidate] < m_xfbCounterSlotLastUse[slot]) {
                    slot = candidate;
                }
            }
        }
        m_xfbCounterSlotOwner[slot] = identity;
        m_xfbCounterSlotLastUse[slot] = ++m_xfbCounterSlotUseSerial;
        m_xfbCountersValid[slot] = false;
        m_xfbLastSeenGeneration[slot] = 0;
        return slot;
    }

    Bool VulkanRenderer::BeginXfbCaptureForDraw(FrameContext::FrameData& frame) {
        if (!m_transformFeedbackFeatureEnabled || !MG_Pipe::gPipeInputs->IsLive() ||
            !MG_Pipe::gPipeInputs->IsTransformFeedbackActive()) {
            return false;
        }
        // A paused span captures nothing, and the counter buffers keep their values, so the
        // next resumed draw appends exactly where the last captured one stopped - which is
        // what pause/resume means (ARB_transform_feedback2).
        if (MG_Pipe::gPipeInputs->IsTransformFeedbackPaused()) {
            return false;
        }
        // The bound pipeline's last pre-rasterization stage has to have been declared with Xfb
        // (VUID-vkCmdBeginTransformFeedbackEXT-None-04128). Everything above this line reads GL
        // state, which cannot answer that: a program can be built as a capture variant and still
        // end up with a module carrying no Xfb mode - the clip/XFB validation backstop rewinding
        // past the decoration, or XfbCaptureDecoratePass resolving none of the requested varyings
        // and changing nothing. Declining the span leaves the capture buffers untouched, which is
        // the same nothing the driver would have written, without the undefined behaviour.
        if (m_currentDrawXfbCaptureDeclined) {
            MGLOG_E_ONCE("BeginXfbCaptureForDraw: declining the capture span - the bound program's last "
                         "pre-rasterization stage carries no Xfb execution mode, so recording one would be "
                         "undefined behaviour rather than a capture");
            return false;
        }
        return BeginWireXfbCaptureForDraw(frame);
    }

    Bool VulkanRenderer::BeginXfbCaptureWithBuffers(FrameContext::FrameData& frame, Uint32 bufferCount,
            const VkBuffer* buffers, const VkDeviceSize* offsets, const VkDeviceSize* sizes) {
        if (!m_xfbCounterBuffer.IsValid()) {
            if (!m_xfbCounterBuffer.Create({
                    .allocator = m_allocator,
                    .size = 16 * kXfbCounterObjectSlots,
                    .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                })) {
                MGLOG_E_ONCE("BeginXfbCaptureForDraw: failed to create the counter buffer");
                return false;
            }
        }
        m_currentDrawXfbBufferCount = bufferCount;
        m_currentDrawXfbBufferMask = 0;
        for (Uint32 i = 0; i < bufferCount; ++i) {
            if (buffers[i] == VK_NULL_HANDLE) continue;
            m_currentDrawXfbBufferMask |= 1u << i;
            s_vkCmdBindTransformFeedbackBuffersEXT(frame.commandBuffer, i, 1, buffers + i,
                                                   offsets + i, sizes + i);
        }

        const Uint32 counterSlot = CurrentXfbCounterSlot();
        const Uint64 generation = MG_Pipe::gPipeInputs->GetTransformFeedbackGeneration();
        const Bool resume = m_xfbCountersValid[counterSlot] && m_xfbLastSeenGeneration[counterSlot] == generation;
        m_xfbLastSeenGeneration[counterSlot] = generation;

        VkBuffer counterBuffers[4] = {};
        VkDeviceSize counterOffsets[4] = {};
        for (SizeT i = 0; i < bufferCount; ++i) {
            if (!(m_currentDrawXfbBufferMask & (1u << i))) continue;
            counterBuffers[i] = m_xfbCounterBuffer.GetHandle();
            counterOffsets[i] = static_cast<VkDeviceSize>(counterSlot) * 16 + static_cast<VkDeviceSize>(i) * 4;
        }
        if (resume) {
            s_vkCmdBeginTransformFeedbackEXT(frame.commandBuffer, 0, static_cast<Uint32>(bufferCount),
                                             counterBuffers, counterOffsets);
        } else {
            s_vkCmdBeginTransformFeedbackEXT(frame.commandBuffer, 0, 0, nullptr, nullptr);
        }
        return true;
    }

    void VulkanRenderer::EndXfbCaptureForDraw(FrameContext::FrameData& frame, Bool began) {
        if (!began) {
            return;
        }
        // Exactly the targets used by this draw's Begin, independent of frontend
        // objects and of whether the span came from the wire or monolith state.
        const Uint32 bufferCount = m_currentDrawXfbBufferCount;
        const Uint32 counterSlot = CurrentXfbCounterSlot();
        VkBuffer counterBuffers[4] = {};
        VkDeviceSize counterOffsets[4] = {};
        for (SizeT i = 0; i < bufferCount; ++i) {
            if (!(m_currentDrawXfbBufferMask & (1u << i))) continue;
            counterBuffers[i] = m_xfbCounterBuffer.GetHandle();
            counterOffsets[i] = static_cast<VkDeviceSize>(counterSlot) * 16 + static_cast<VkDeviceSize>(i) * 4;
        }
        s_vkCmdEndTransformFeedbackEXT(frame.commandBuffer, 0, static_cast<Uint32>(bufferCount), counterBuffers,
                                       counterOffsets);
        m_xfbCountersValid[counterSlot] = true;
        m_xfbWritesPendingVisibility = true;
    }

    // GL makes transform feedback results visible to every later command on their own, with no
    // glMemoryBarrier in between - unlike shader storage writes, which is why the barrier the
    // Vulkan memory model requires has to be supplied here rather than by the application. It
    // cannot be recorded where the write happens (inside the capturing draw's render pass, which
    // declares no self-dependency), so it is emitted at the next point that could read the
    // captured buffer: the following draw, or a readback.
    void VulkanRenderer::MakeXfbWritesVisible() {
        if (!m_xfbWritesPendingVisibility) {
            return;
        }
        m_xfbWritesPendingVisibility = false;
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }
        VkMemoryBarrier memoryBarrier{};
        memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask =
                VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
        // Every way a captured buffer can be read back: replayed as vertex attributes or indices
        // by glDrawTransformFeedback, sampled through a uniform or storage binding, sourced as an
        // indirect command, copied out, or mapped.
        memoryBarrier.dstAccessMask =
                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT |
                VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
    }

    void VulkanRenderer::DrawArrays(const DrawCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        if (!SetupDraw(frame, payload.mode, 0, payload.params)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        VkCommandBuffer& commandBuffer = frame.commandBuffer;

        const Bool xfbActive = BeginXfbCaptureForDraw(frame);
        BeginXfbQueryForDraw(commandBuffer, xfbActive);
        const Bool occlusionActive = BeginOcclusionForDraw(commandBuffer);
        vkCmdDraw(commandBuffer,
            payload.params.vertexCount,
            payload.params.instanceCount,
            payload.params.firstVertex,
            payload.params.firstInstance);
        EndOcclusionForDraw(commandBuffer, occlusionActive);
        EndXfbCaptureForDraw(frame, xfbActive);
        EndXfbQueryForDraw(commandBuffer);
    }

    Bool VulkanRenderer::StartOcclusionQueryCapture() {
        if (!m_hostQueryResetEnabled || s_vkResetQueryPool == nullptr) {
            return false;
        }
        if (m_occlusionQueryPool == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
            poolInfo.queryCount = kOcclusionQuerySlots;
            if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_occlusionQueryPool) != VK_SUCCESS) {
                MGLOG_E_ONCE("StartOcclusionQueryCapture: vkCreateQueryPool failed");
                m_occlusionQueryPool = VK_NULL_HANDLE;
                return false;
            }
            s_vkResetQueryPool(m_device, m_occlusionQueryPool, 0, kOcclusionQuerySlots);
        }
        m_occlusionActiveSlots.clear();
        m_occlusionCaptureActive = true;
        return true;
    }

    void VulkanRenderer::StopOcclusionQueryCapture(Vector<Uint32>& outSlots) {
        outSlots = Move(m_occlusionActiveSlots);
        m_occlusionActiveSlots.clear();
        m_occlusionCaptureActive = false;
    }

    Bool VulkanRenderer::ResolveOcclusionQueryResult(const Vector<Uint32>& slots, Uint64& outSamples) {
        outSamples = 0;
        if (slots.empty()) {
            return true;
        }
        if (m_occlusionQueryPool == VK_NULL_HANDLE) {
            return true;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (frame.isCommandRecording) {
            if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
                VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            }
            if (!SubmitReadbackCommandsAndWait(frame)) {
                return false;
            }
        }
        for (const Uint32 slot : slots) {
            Uint64 value = 0;
            const VkResult result =
                vkGetQueryPoolResults(m_device, m_occlusionQueryPool, slot, 1, sizeof(value), &value, sizeof(value),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (result == VK_SUCCESS) {
                outSamples += value;
            }
            s_vkResetQueryPool(m_device, m_occlusionQueryPool, slot, 1);
        }
        return true;
    }

    Bool VulkanRenderer::StartXfbQueryCapture(Uint32 kind) {
        if (!m_xfbQueriesSupported || !m_hostQueryResetEnabled || s_vkResetQueryPool == nullptr ||
            s_vkCmdBeginQueryIndexedEXT == nullptr || kind > 1) {
            return false;
        }
        if (m_xfbQueryPool == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            poolInfo.queryType = VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT;
            poolInfo.queryCount = kXfbQuerySlots;
            if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_xfbQueryPool) != VK_SUCCESS) {
                MGLOG_E_ONCE("StartXfbQueryCapture: vkCreateQueryPool failed");
                m_xfbQueryPool = VK_NULL_HANDLE;
                return false;
            }
            s_vkResetQueryPool(m_device, m_xfbQueryPool, 0, kXfbQuerySlots);
        }
        // The reroute pool, on the first GENERATED span that needs it. A creation
        // failure disarms rather than failing the capture: the stream path still
        // answers (with the driver's defect), which beats answering nothing.
        if (kind == 1 && m_primGenRerouteKind != MG_Util::SelfTest::PrimGenRerouteKind::None &&
            m_primGenReroutePool == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            poolInfo.queryCount = kXfbQuerySlots;
            if (m_primGenRerouteKind == MG_Util::SelfTest::PrimGenRerouteKind::PrimitivesGeneratedExt) {
                // The query Vulkan defines for this GL target; counts vertex stream 0
                // when begun with plain vkCmdBeginQuery.
                poolInfo.queryType = VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT;
            } else {
                poolInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                // The clipping-stage INVOCATION counter: one per primitive reaching
                // primitive clipping (GL's CLIPPING_INPUT_PRIMITIVES) - post-tess/GS,
                // pre-clip, and per spec still counted under rasterizer discard, which
                // is exactly the set GL_PRIMITIVES_GENERATED is defined over. The
                // stage's OUTPUT count (CLIPPING_PRIMITIVES_BIT) would be wrong:
                // clipping may drop or split primitives.
                poolInfo.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT;
            }
            if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_primGenReroutePool) != VK_SUCCESS) {
                MGLOG_E_ONCE("StartXfbQueryCapture: reroute pool creation failed; the "
                             "PRIMITIVES_GENERATED reroute is disarmed and XFB-inactive draws keep "
                             "the stream query");
                m_primGenReroutePool = VK_NULL_HANDLE;
                m_primGenRerouteKind = MG_Util::SelfTest::PrimGenRerouteKind::None;
            } else {
                s_vkResetQueryPool(m_device, m_primGenReroutePool, 0, kXfbQuerySlots);
            }
        }
        m_xfbQueryActiveSlots[kind].clear();
        m_xfbQueryCaptureActive[kind] = true;
        if (kind == 1) {
            m_primGenRerouteActiveSlots.clear();
        }
        return true;
    }

    Bool VulkanRenderer::ArePausedDrawsGpuCounted() const {
        // Exactly the gate BeginXfbQueryForDraw applies per draw, so a span told "armed"
        // really does get a reroute slot for every draw with no open capture - a paused
        // span's draws included.
        const Bool rerouteArmed = m_primGenRerouteKind != MG_Util::SelfTest::PrimGenRerouteKind::None &&
                                  m_primGenReroutePool != VK_NULL_HANDLE;
        // Otherwise the paused draw takes a stream slot, which is an exact count of it
        // on a driver the probe measured as counting capture-less draws.
        return rerouteArmed || m_primGenStreamCountsXfbInactiveDraws;
    }

    void VulkanRenderer::StopXfbQueryCapture(Uint32 kind, Vector<Uint32>& outSlots,
                                             Vector<Uint32>& outRerouteSlots) {
        if (kind > 1) {
            return;
        }
        outSlots = Move(m_xfbQueryActiveSlots[kind]);
        m_xfbQueryActiveSlots[kind].clear();
        m_xfbQueryCaptureActive[kind] = false;
        outRerouteSlots.clear();
        if (kind == 1) {
            outRerouteSlots = Move(m_primGenRerouteActiveSlots);
            m_primGenRerouteActiveSlots.clear();
        }
    }

    Bool VulkanRenderer::ResolveXfbQueryResult(const Vector<Uint32>& slots, const Vector<Uint32>& rerouteSlots,
                                               Bool wantGenerated, Uint64& outPrimitives) {
        outPrimitives = 0;
        const Bool haveStreamSlots = !slots.empty() && m_xfbQueryPool != VK_NULL_HANDLE;
        // Reroute slots only ever accumulate the GENERATED target (see
        // BeginXfbQueryForDraw); WRITTEN never opens one.
        const Bool haveRerouteSlots =
            wantGenerated && !rerouteSlots.empty() && m_primGenReroutePool != VK_NULL_HANDLE;
        if (!haveStreamSlots && !haveRerouteSlots) {
            return true;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (frame.isCommandRecording) {
            if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
                VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            }
            if (!SubmitReadbackCommandsAndWait(frame)) {
                return false;
            }
        }
        if (haveStreamSlots) {
            for (const Uint32 slot : slots) {
                Uint64 pair[2] = {0, 0}; // {primitivesWritten, primitivesNeeded}
                const VkResult result =
                    vkGetQueryPoolResults(m_device, m_xfbQueryPool, slot, 1, sizeof(pair), pair, sizeof(pair),
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                if (result == VK_SUCCESS) {
                    outPrimitives += pair[wantGenerated ? 1 : 0];
                }
            }
        }
        if (haveRerouteSlots) {
            for (const Uint32 slot : rerouteSlots) {
                // Both reroute pool kinds answer one 64-bit primitive count per slot.
                Uint64 generated = 0;
                const VkResult result = vkGetQueryPoolResults(
                    m_device, m_primGenReroutePool, slot, 1, sizeof(generated), &generated,
                    sizeof(generated), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                if (result == VK_SUCCESS) {
                    outPrimitives += generated;
                }
            }
        }
        return true;
    }

    void VulkanRenderer::BeginXfbQueryForDraw(VkCommandBuffer commandBuffer, Bool xfbActive) {
        m_xfbQuerySlotOpen = false;
        m_primGenRerouteSlotOpen = false;
        if ((!m_xfbQueryCaptureActive[0] && !m_xfbQueryCaptureActive[1]) || m_xfbQueryPool == VK_NULL_HANDLE) {
            return;
        }
        // Every draw with no OPEN capture is the stream query's silent case, and that
        // includes a draw made while the GL span is merely PAUSED (the pause closes the
        // capture, so BeginXfbCaptureForDraw already answered false for it). Paused
        // draws are rerouted like any other: the frontend's CPU paused-primitive
        // counter cannot stand in for them - it is written by only 3 of the ~15 draw
        // entry points (never the instanced, indirect or multi-draw ones) and answers 0
        // for GL_PATCHES by design, since the tessellator's amplification is not
        // knowable on the CPU - which is exactly the CTS's shape. Double counting is
        // prevented on the other side instead: a GENERATED span opened while this
        // reroute is armed ignores that CPU counter entirely (see
        // ArePausedDrawsGpuCounted and DirectVulkan.cpp's XfbGenerated resolve), so
        // every XFB-inactive draw in the span is priced exactly once, by this pool.
        const Bool rerouteGenerated = m_xfbQueryCaptureActive[1] &&
                                      m_primGenRerouteKind != MG_Util::SelfTest::PrimGenRerouteKind::None &&
                                      m_primGenReroutePool != VK_NULL_HANDLE && !xfbActive;
        // The stream slot stays for WRITTEN whatever the reroute does (with capture
        // inactive its primitivesWritten is 0, which is the correct WRITTEN answer),
        // and for GENERATED wherever this draw is not rerouted - so one GL query span
        // may accumulate stream slots (XFB-active draws) and reroute slots
        // (XFB-inactive draws) side by side.
        const Bool wantStreamSlot =
            m_xfbQueryCaptureActive[0] || (m_xfbQueryCaptureActive[1] && !rerouteGenerated);
        if (wantStreamSlot) {
            const Uint32 slot = m_xfbQuerySlotCursor;
            m_xfbQuerySlotCursor = (m_xfbQuerySlotCursor + 1) % kXfbQuerySlots;
            // Slots are never host-reset at read time (both GL targets may reference one
            // slot); recycle them here instead.
            s_vkResetQueryPool(m_device, m_xfbQueryPool, slot, 1);
            s_vkCmdBeginQueryIndexedEXT(commandBuffer, m_xfbQueryPool, slot, 0, 0);
            if (m_xfbQueryCaptureActive[0]) {
                m_xfbQueryActiveSlots[0].push_back(slot);
            }
            if (m_xfbQueryCaptureActive[1] && !rerouteGenerated) {
                m_xfbQueryActiveSlots[1].push_back(slot);
            }
            m_xfbQuerySlotOpen = true;
            m_xfbQueryOpenSlot = slot;
        }
        if (rerouteGenerated) {
            // Latched at INFO on purpose: it is the pinned integration lane's arming
            // observable (the shape UnlocatedIoBlockScenario asserts), and the builds
            // CI runs compile INFO in.
            MGLOG_I_ONCE("PRIMITIVES_GENERATED reroute engaged: an XFB-inactive draw accumulates "
                         "through the %s pool",
                         m_primGenRerouteKind ==
                                 MG_Util::SelfTest::PrimGenRerouteKind::PrimitivesGeneratedExt
                             ? "VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT"
                             : "clipping-invocations statistics");
            const Uint32 slot = m_primGenRerouteSlotCursor;
            m_primGenRerouteSlotCursor = (m_primGenRerouteSlotCursor + 1) % kXfbQuerySlots;
            // Same recycle-at-begin discipline as the stream pool. Both pool kinds
            // are begun with plain vkCmdBeginQuery (a PRIMITIVES_GENERATED_EXT
            // query begun this way counts vertex stream 0).
            s_vkResetQueryPool(m_device, m_primGenReroutePool, slot, 1);
            vkCmdBeginQuery(commandBuffer, m_primGenReroutePool, slot, 0);
            m_primGenRerouteActiveSlots.push_back(slot);
            m_primGenRerouteSlotOpen = true;
            m_primGenRerouteOpenSlot = slot;
        }
    }

    void VulkanRenderer::EndXfbQueryForDraw(VkCommandBuffer commandBuffer) {
        if (m_xfbQuerySlotOpen) {
            s_vkCmdEndQueryIndexedEXT(commandBuffer, m_xfbQueryPool, m_xfbQueryOpenSlot, 0);
            m_xfbQuerySlotOpen = false;
        }
        if (m_primGenRerouteSlotOpen) {
            vkCmdEndQuery(commandBuffer, m_primGenReroutePool, m_primGenRerouteOpenSlot);
            m_primGenRerouteSlotOpen = false;
        }
    }

    Bool VulkanRenderer::BeginOcclusionForDraw(VkCommandBuffer commandBuffer) {
        if (!m_occlusionCaptureActive || m_occlusionQueryPool == VK_NULL_HANDLE) {
            return false;
        }
        const Uint32 slot = m_occlusionSlotCursor;
        m_occlusionSlotCursor = (m_occlusionSlotCursor + 1) % kOcclusionQuerySlots;
        // Slots recycle after their read; a wrapped-past unread slot is stale, so
        // reset it here (host reset - the slot's prior GPU use has long retired).
        s_vkResetQueryPool(m_device, m_occlusionQueryPool, slot, 1);
        vkCmdBeginQuery(commandBuffer, m_occlusionQueryPool, slot,
                        m_occlusionQueryPreciseEnabled ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        m_occlusionActiveSlots.push_back(slot);
        return true;
    }

    void VulkanRenderer::EndOcclusionForDraw(VkCommandBuffer commandBuffer, Bool began) {
        if (!began) {
            return;
        }
        vkCmdEndQuery(commandBuffer, m_occlusionQueryPool, m_occlusionActiveSlots.back());
    }

    void VulkanRenderer::DrawElements(const DrawIndexedCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        DrawCmdParam vertexRange{};
        vertexRange.vertexCount = payload.params.indexCount + (payload.params.vertexOffset > 0
                                                                   ? static_cast<Uint32>(payload.params.vertexOffset)
                                                                   : 0);
        vertexRange.instanceCount = payload.params.instanceCount;
        vertexRange.firstVertex = 0;
        vertexRange.firstInstance = static_cast<Uint32>(payload.params.firstInstance);
        vertexRange.baseVertex = payload.params.vertexOffset;
        // Direct DrawElements fetches exactly the indices in its view, so vertex-stream
        // conversion may bound its work by scanning them.
        vertexRange.indexRangeIsExactView = true;

        if (!SetupDraw(frame, payload.mode, DrawSetupAspect::IndexBuffer, vertexRange,
                       &payload.indexBufferView)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        VkCommandBuffer& commandBuffer = frame.commandBuffer;

        const Bool xfbActive = BeginXfbCaptureForDraw(frame);
        BeginXfbQueryForDraw(commandBuffer, xfbActive);
        const Bool occlusionActive = BeginOcclusionForDraw(commandBuffer);
        vkCmdDrawIndexed(commandBuffer,
            payload.params.indexCount,
            payload.params.instanceCount,
            payload.params.firstIndex,
            payload.params.vertexOffset,
            payload.params.firstInstance);
        EndOcclusionForDraw(commandBuffer, occlusionActive);
        EndXfbCaptureForDraw(frame, xfbActive);
        EndXfbQueryForDraw(commandBuffer);
    }

    void VulkanRenderer::MultiDrawArrays(const MultiDrawCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        // One state/pipeline setup covering the union of all sub-draw vertex ranges, then a vkCmdDraw
        // per range -- mirrors MultiDrawElements.
        DrawCmdParam vertexRange{};
        for (Uint32 idraw = 0; idraw < payload.drawCount; ++idraw) {
            vertexRange.vertexCount = std::max(vertexRange.vertexCount,
                                               payload.pParams[idraw].firstVertex + payload.pParams[idraw].vertexCount);
            vertexRange.instanceCount = std::max(vertexRange.instanceCount, payload.pParams[idraw].instanceCount);
            vertexRange.firstInstance = std::max(vertexRange.firstInstance, payload.pParams[idraw].firstInstance);
        }

        if (!SetupDraw(frame, payload.mode, 0, vertexRange)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        EmitMultiDraw(frame.commandBuffer, payload.pParams, payload.drawCount);
    }

    // The tier-2 indirect batch uploads the param arrays as-is: the leading members of the
    // renderer's draw-parameter structs are exactly Vulkan's indirect command layouts, and
    // vkCmdDraw(Indexed)Indirect accepts any 4-aligned stride >= the command size, so the
    // trailing CPU-side metadata rides along unread instead of forcing a repack.
    static_assert(sizeof(DrawIndexedCmdParam) == sizeof(VkDrawIndexedIndirectCommand) &&
                      offsetof(DrawIndexedCmdParam, indexCount) == offsetof(VkDrawIndexedIndirectCommand, indexCount) &&
                      offsetof(DrawIndexedCmdParam, instanceCount) ==
                          offsetof(VkDrawIndexedIndirectCommand, instanceCount) &&
                      offsetof(DrawIndexedCmdParam, firstIndex) == offsetof(VkDrawIndexedIndirectCommand, firstIndex) &&
                      offsetof(DrawIndexedCmdParam, vertexOffset) ==
                          offsetof(VkDrawIndexedIndirectCommand, vertexOffset) &&
                      offsetof(DrawIndexedCmdParam, firstInstance) ==
                          offsetof(VkDrawIndexedIndirectCommand, firstInstance),
                  "DrawIndexedCmdParam must alias VkDrawIndexedIndirectCommand for the tier-2 multi-draw upload");
    static_assert(sizeof(DrawCmdParam) % 4 == 0 && sizeof(DrawCmdParam) >= sizeof(VkDrawIndirectCommand) &&
                      offsetof(DrawCmdParam, vertexCount) == offsetof(VkDrawIndirectCommand, vertexCount) &&
                      offsetof(DrawCmdParam, instanceCount) == offsetof(VkDrawIndirectCommand, instanceCount) &&
                      offsetof(DrawCmdParam, firstVertex) == offsetof(VkDrawIndirectCommand, firstVertex) &&
                      offsetof(DrawCmdParam, firstInstance) == offsetof(VkDrawIndirectCommand, firstInstance),
                  "DrawCmdParam must lead with VkDrawIndirectCommand for the tier-2 multi-draw upload");

    void VulkanRenderer::EmitMultiDraw(VkCommandBuffer commandBuffer, const DrawCmdParam* pParams, Uint32 drawCount) {
        if (drawCount == 0) {
            return;
        }
        if (drawCount == 1) {
            vkCmdDraw(commandBuffer, pParams[0].vertexCount, pParams[0].instanceCount, pParams[0].firstVertex,
                      pParams[0].firstInstance);
            return;
        }

        // Tier 1: VK_EXT_multi_draw. vkCmdDrawMultiEXT shares one instanceCount/firstInstance
        // across the whole batch, so the batch must be uniform in both (GL's glMultiDrawArrays
        // always is: 1/0).
        if (m_multiDrawAllowExt) {
            Bool uniformInstances = true;
            for (Uint32 idraw = 1; idraw < drawCount; ++idraw) {
                if (pParams[idraw].instanceCount != pParams[0].instanceCount ||
                    pParams[idraw].firstInstance != pParams[0].firstInstance) {
                    uniformInstances = false;
                    break;
                }
            }
            if (uniformInstances) {
                static thread_local Vector<VkMultiDrawInfoEXT> infos;
                infos.resize(drawCount);
                for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
                    infos[idraw].firstVertex = pParams[idraw].firstVertex;
                    infos[idraw].vertexCount = pParams[idraw].vertexCount;
                }
                for (Uint32 base = 0; base < drawCount; base += m_maxMultiDrawCount) {
                    const Uint32 chunk = std::min(drawCount - base, m_maxMultiDrawCount);
                    s_vkCmdDrawMultiEXT(commandBuffer, chunk, infos.data() + base, pParams[0].instanceCount,
                                        pParams[0].firstInstance, sizeof(VkMultiDrawInfoEXT));
                }
                return;
            }
        }

        // Tier 2: multiDrawIndirect - one vkCmdDrawIndirect over a transient command array.
        // A sub-draw with firstInstance != 0 is illegal in an indirect command without the
        // drawIndirectFirstInstance feature; such a batch falls to the unrolled tier.
        if (m_multiDrawAllowIndirect) {
            Bool firstInstanceLegal = m_drawIndirectFirstInstanceFeatureEnabled;
            if (!firstInstanceLegal) {
                firstInstanceLegal = true;
                for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
                    if (pParams[idraw].firstInstance != 0) {
                        firstInstanceLegal = false;
                        break;
                    }
                }
            }
            const Uint32 maxIndirectCount = m_physicalDevice.properties.limits.maxDrawIndirectCount;
            if (firstInstanceLegal && maxIndirectCount > 0) {
                BufferSlice commandSlice{};
                if (m_bufferManager.UploadTransient(BufferKind::Indirect, m_frameContext.GetCurrentFrameIndex(),
                                                    pParams,
                                                    static_cast<VkDeviceSize>(drawCount) * sizeof(DrawCmdParam),
                                                    sizeof(Uint32), commandSlice)) {
                    for (Uint32 base = 0; base < drawCount; base += maxIndirectCount) {
                        const Uint32 chunk = std::min(drawCount - base, maxIndirectCount);
                        vkCmdDrawIndirect(commandBuffer, commandSlice.buffer,
                                          commandSlice.offset +
                                              static_cast<VkDeviceSize>(base) * sizeof(DrawCmdParam),
                                          chunk, sizeof(DrawCmdParam));
                    }
                    return;
                }
                // Transient arena refused the upload: fall through to the unrolled tier.
            }
        }

        // Tier 3: unrolled loop, byte-identical fallback (and the only tier where a SPIR-V
        // DrawIndex consumer sees 0 for every sub-draw instead of the sub-draw index).
        for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
            vkCmdDraw(commandBuffer, pParams[idraw].vertexCount, pParams[idraw].instanceCount,
                      pParams[idraw].firstVertex, pParams[idraw].firstInstance);
        }
    }

    void VulkanRenderer::EmitMultiDrawIndexed(VkCommandBuffer commandBuffer, const DrawIndexedCmdParam* pParams,
                                              Uint32 drawCount) {
        if (drawCount == 0) {
            return;
        }
        if (drawCount == 1) {
            vkCmdDrawIndexed(commandBuffer, pParams[0].indexCount, pParams[0].instanceCount, pParams[0].firstIndex,
                             pParams[0].vertexOffset, pParams[0].firstInstance);
            return;
        }

        // Tier 1: VK_EXT_multi_draw. VkMultiDrawIndexedInfoEXT carries per-draw
        // firstIndex/indexCount/vertexOffset (pVertexOffset = nullptr keeps the per-draw
        // offsets), but instanceCount/firstInstance are batch-wide, so the batch must be
        // uniform in both (GL's glMultiDrawElements* always is: 1/0).
        if (m_multiDrawAllowExt) {
            Bool uniformInstances = true;
            for (Uint32 idraw = 1; idraw < drawCount; ++idraw) {
                if (pParams[idraw].instanceCount != pParams[0].instanceCount ||
                    pParams[idraw].firstInstance != pParams[0].firstInstance) {
                    uniformInstances = false;
                    break;
                }
            }
            if (uniformInstances) {
                static thread_local Vector<VkMultiDrawIndexedInfoEXT> infos;
                infos.resize(drawCount);
                for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
                    infos[idraw].firstIndex = pParams[idraw].firstIndex;
                    infos[idraw].indexCount = pParams[idraw].indexCount;
                    infos[idraw].vertexOffset = pParams[idraw].vertexOffset;
                }
                for (Uint32 base = 0; base < drawCount; base += m_maxMultiDrawCount) {
                    const Uint32 chunk = std::min(drawCount - base, m_maxMultiDrawCount);
                    s_vkCmdDrawMultiIndexedEXT(commandBuffer, chunk, infos.data() + base,
                                               pParams[0].instanceCount,
                                               static_cast<Uint32>(pParams[0].firstInstance),
                                               sizeof(VkMultiDrawIndexedInfoEXT), nullptr);
                }
                return;
            }
        }

        // Tier 2: multiDrawIndirect - one vkCmdDrawIndexedIndirect over a transient command
        // array (DrawIndexedCmdParam aliases VkDrawIndexedIndirectCommand, see static_assert).
        if (m_multiDrawAllowIndirect) {
            Bool firstInstanceLegal = m_drawIndirectFirstInstanceFeatureEnabled;
            if (!firstInstanceLegal) {
                firstInstanceLegal = true;
                for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
                    if (pParams[idraw].firstInstance != 0) {
                        firstInstanceLegal = false;
                        break;
                    }
                }
            }
            const Uint32 maxIndirectCount = m_physicalDevice.properties.limits.maxDrawIndirectCount;
            if (firstInstanceLegal && maxIndirectCount > 0) {
                BufferSlice commandSlice{};
                if (m_bufferManager.UploadTransient(BufferKind::Indirect, m_frameContext.GetCurrentFrameIndex(),
                                                    pParams,
                                                    static_cast<VkDeviceSize>(drawCount) *
                                                        sizeof(DrawIndexedCmdParam),
                                                    sizeof(Uint32), commandSlice)) {
                    for (Uint32 base = 0; base < drawCount; base += maxIndirectCount) {
                        const Uint32 chunk = std::min(drawCount - base, maxIndirectCount);
                        vkCmdDrawIndexedIndirect(commandBuffer, commandSlice.buffer,
                                                 commandSlice.offset +
                                                     static_cast<VkDeviceSize>(base) * sizeof(DrawIndexedCmdParam),
                                                 chunk, sizeof(DrawIndexedCmdParam));
                    }
                    return;
                }
            }
        }

        // Tier 3: unrolled loop, byte-identical fallback (and the only tier where a SPIR-V
        // DrawIndex consumer sees 0 for every sub-draw instead of the sub-draw index).
        for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
            vkCmdDrawIndexed(commandBuffer, pParams[idraw].indexCount, pParams[idraw].instanceCount,
                             pParams[idraw].firstIndex, pParams[idraw].vertexOffset, pParams[idraw].firstInstance);
        }
    }

    void VulkanRenderer::MultiDrawElements(const MultiDrawIndexedCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        DrawCmdParam vertexRange{};
        for (Uint32 idraw = 0; idraw < payload.drawCount; ++idraw) {
            vertexRange.vertexCount = std::max(vertexRange.vertexCount, payload.pParams[idraw].indexCount);
            vertexRange.instanceCount = std::max(vertexRange.instanceCount, payload.pParams[idraw].instanceCount);
            vertexRange.firstInstance = std::max(vertexRange.firstInstance,
                                                 static_cast<Uint32>(payload.pParams[idraw].firstInstance));
        }

        if (!SetupDraw(frame, payload.mode, DrawSetupAspect::IndexBuffer, vertexRange,
                  &payload.indexBufferView)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        // Collapse contiguous sub-draw runs BEFORE tier dispatch: merging shrinks the
        // param span every tier consumes (fewer VkMultiDrawIndexedInfoEXT entries, a
        // smaller transient command array, fewer unrolled vkCmdDrawIndexed). Per-sub-draw
        // command emission in the driver dominates a Sodium-shaped multi-draw
        // (steady-state profile: >60% of the case inside the Vulkan driver's
        // vkCmdDrawIndexed encoding for 132x32 sub-draws/frame), and a chunk
        // renderer's sub-draws are runs of adjacent index ranges over one buffer.
        // Two draws are one iff they concatenate to an identical index stream:
        //  - a LIST topology (points/lines/triangles). Strips/fans/loops would
        //    weld primitives across the seam.
        //  - the accumulated count ends on a primitive boundary, otherwise GL
        //    discards the dangling indices at the sub-draw's end but the merged
        //    stream would assemble them with the next sub-draw's indices.
        //  - primitive restart is off: with restart on, a sentinel mid-stream
        //    resets assembly, so a partial primitive before the seam would
        //    otherwise be discarded per sub-draw (same dangling-index argument).
        //  - identical baseVertex/instancing and firstIndex adjacency, so the
        //    merged range fetches exactly the two sub-draws' indices in order.
        Uint32 mergeGranularity = 0;
        switch (payload.mode) {
            case GL_POINTS:    mergeGranularity = 1; break;
            case GL_LINES:     mergeGranularity = 2; break;
            case GL_TRIANGLES: mergeGranularity = 3; break;
            default: break;
        }
        if (mergeGranularity != 0) {
            const RenderStateParameters& rsp = MG_Pipe::gPipeInputs->GetRenderStateParameters();
            if (rsp.PrimitiveRestartEnabled || rsp.PrimitiveRestartFixedIndexEnabled) {
                mergeGranularity = 0;
            }
        }
        const DrawIndexedCmdParam* pParams = payload.pParams;
        Uint32 drawCount = payload.drawCount;
        static thread_local Vector<DrawIndexedCmdParam> mergedParams;
        if (mergeGranularity != 0) {
            mergedParams.clear();
            mergedParams.reserve(drawCount);
            Uint32 idraw = 0;
            while (idraw < drawCount) {
                DrawIndexedCmdParam head = pParams[idraw];
                ++idraw;
                if (head.indexCount == 0) {
                    continue; // draws nothing, contributes nothing to a run
                }
                if (head.instanceCount == 1) {
                    while (idraw < drawCount) {
                        const DrawIndexedCmdParam& next = pParams[idraw];
                        if (next.indexCount == 0) {
                            ++idraw;
                            continue;
                        }
                        if (head.indexCount % mergeGranularity != 0 ||
                            next.instanceCount != 1 ||
                            next.vertexOffset != head.vertexOffset ||
                            next.firstInstance != head.firstInstance ||
                            next.firstIndex != head.firstIndex + head.indexCount ||
                            head.indexCount + next.indexCount < head.indexCount) {
                            break;
                        }
                        head.indexCount += next.indexCount;
                        ++idraw;
                    }
                }
                mergedParams.push_back(head);
            }
            pParams = mergedParams.data();
            drawCount = static_cast<Uint32>(mergedParams.size());
        }

        EmitMultiDrawIndexed(frame.commandBuffer, pParams, drawCount);
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
