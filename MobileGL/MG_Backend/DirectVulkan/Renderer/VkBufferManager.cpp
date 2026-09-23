// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkBufferManager.h"
#include "../DirectVulkan.h"
#include "VulkanRenderer.h"

#include "MG_Util/Metrics/PipeStats.h"
#if MOBILEGL_BUILD_DISAGGREGATED
#include "MG_Pipe/MGPipeCallbacks.h"
#include "MG_Pipe/PipeApply.h"
#include "MG_Remote/Server/StagedShadow.h"
#include <Config.h>
#include <cstdlib>
#endif

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
#if MOBILEGL_BUILD_DISAGGREGATED
        // P7 wave 2 package B3, INVESTIGATION PROBE (split-only, read once, not in
        // ConfigLoader's accepted-env table - package B's MGITEST_MAGMA_FORCE_SHADER_MIPMAP
        // shape). MOBILEGL_MAGMA_WIREBUF_PROBE=1 prints one line per streamed-buffer event so
        // the `glBufferSubData -> draw` ordering of a trace can be read off a log instead of
        // inferred. Costs a branch on a cached bool when unset.
        Bool WireBufProbeEnabled() {
            static const Bool enabled = [] {
                const char* value = std::getenv("MOBILEGL_MAGMA_WIREBUF_PROBE");
                return value && value[0] == '1';
            }();
            return enabled;
        }

        // P7 wave 2 package B3, FORCING KNOB (split-only, read once, not in ConfigLoader's
        // accepted-env table - package B's MGITEST_MAGMA_FORCE_SHADER_MIPMAP shape).
        //
        // WriteWireBuffer has exactly one unsafe shape: taking the immediate host memcpy
        // because `lastUseSerial > GetCompletedSerial()` said the buffer is idle. That
        // predicate's first term is PURE COUNTING (`m_frameSerial - frameCount`, no fence),
        // so on any arm where the frame serial advances without the GPU having proved the
        // frame's submissions complete, the answer is a guess. Host lanes never reach that
        // state on their own (measured: 0 mid-frame frame-boundary drains over the whole
        // OpenRA replay), so the lane cannot arm against it without forcing it.
        //
        // 0/unset = off. 1 = the serial term answers "idle" for every write it would have
        // ordered. N>1 = only the Nth such write (1-based), which is how one
        // `glBufferSubData -> draw` pair is poisoned in isolation and the device's "one draw
        // contributed nothing" signature is reproduced rather than the whole frame destroyed.
        //
        // The knob forces ONLY the counting term to lie. It does NOT bypass the ordered copy,
        // so it stays a valid red-once across the fix: before the submission term existed the
        // lie reached the host memcpy (red); with the submission term the lie is caught and
        // the copy is still ordered (green).
        Uint64 ForceStaleSerialSelector() {
            static const Uint64 selector = [] {
                const char* value = std::getenv("MGITEST_MAGMA_FORCE_STALE_BUFFER_SERIAL");
                if (!value || !value[0]) return Uint64{0};
                return static_cast<Uint64>(std::strtoull(value, nullptr, 10));
            }();
            return selector;
        }
        // Counts only the writes the busy predicate wanted to ORDER, so the selector indexes
        // the streamed subdata -> draw pairs and nothing else.
        Uint64 g_orderedWireWriteCounter = 0;
#endif
        constexpr VmaAllocationCreateFlags kResidentBufferAllocationFlags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        constexpr SizeT kLiveResourcePruneThreshold = 256;

        // See VkBufferManager::AcquireUnboundStorageDescriptor. 256 bytes: comfortably past
        // every minStorageBufferOffsetAlignment in the wild, and free.
        constexpr VkDeviceSize kUnboundStorageDescriptorBytes = 256;
        // See VkBufferManager::AcquireUnboundTexelBufferDescriptor. The same 256 bytes, for the
        // same reason plus one: a texel buffer view's range must be a whole number of texels of
        // whatever format the placeholder is asked for, and 256 divides by every texel size in
        // the GL image-format table (1, 2, 4, 8 and 16 bytes).
        constexpr VkDeviceSize kUnboundTexelBufferDescriptorBytes = 256;

        // A zero-copy persistent buffer is created once and never recreated (the app holds
        // its mapped pointer), and may be bound to any role, so it carries every usage.
        // TRANSFER_DST is added by CreateResidentStorage.
        constexpr VkBufferUsageFlags kPersistentBackedUsage =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
            // "Every usage" has to mean every usage: a buffer texture reached through an IMAGE
            // unit takes a VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER descriptor, and the write is
            // invalid unless the buffer was created with this bit. Nothing asked for it until
            // imageBuffer support existed, so the omission was invisible.
            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        // Appended to kPersistentBackedUsage when VK_EXT_transform_feedback is enabled
        // (see VkBufferManagerInitInfo::transformFeedbackUsageEnabled).
        constexpr VkBufferUsageFlags kTransformFeedbackUsage =
            VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
        // The app writes into the persistent map with no explicit flush, so its memory must
        // be host-coherent (Adreno host-visible memory is; requiring it keeps us portable).
        constexpr VkMemoryPropertyFlags kPersistentBackedRequiredFlags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        using MG_State::GLState::BackendBufferResource;
        using MG_State::GLState::BufferBackendOps;
        using MG_State::GLState::BufferObject;

        // The manager owned by the active VulkanRenderer; immediate ops route here.
        VkBufferManager* g_activeBufferManager = nullptr;

        void Ops_Respecify(BufferObject& bufferObject) {
            if (g_activeBufferManager) {
                g_activeBufferManager->OnRespecify(bufferObject);
            }
        }

        void Ops_SubData(BufferObject& bufferObject, SizeT offset, SizeT size) {
            if (g_activeBufferManager) {
                g_activeBufferManager->OnSubData(bufferObject, offset, size);
            }
        }

        void Ops_ResidentSubData(BufferObject& bufferObject, SizeT offset, DataPtr data) {
            if (g_activeBufferManager) {
                g_activeBufferManager->OnResidentSubData(bufferObject, offset, data);
            }
        }

        void Ops_FlushMappedRange(BufferObject& bufferObject, Range1D range,
                                  Flags<BufferMappingAccessBit> appAccess) {
            if (g_activeBufferManager) {
                g_activeBufferManager->OnFlushMappedRange(bufferObject, range, appAccess);
            }
        }

        // The CPU is about to read a buffer a shader wrote. Its bytes live in coherent
        // host-visible GPU storage (EnsureGpuResidentStorage adopts it when the buffer is
        // bound as a shader storage buffer), so nothing needs copying - but coherence only
        // says the writes are visible once they have happened, so the work has to retire
        // first, including copies already submitted by a sync-point flush.
        void Ops_ReadbackFromGpu(BufferObject& bufferObject) {
            (void)bufferObject;
            if (pVulkanRenderer) {
                pVulkanRenderer->WaitForSubmitIndex(
                    pVulkanRenderer->GetSyncPointSubmitIndex(), UINT64_MAX, true);
            }
        }

        void* Ops_AcquirePersistentMap(BufferObject& bufferObject) {
            if (g_activeBufferManager) {
                return g_activeBufferManager->AcquirePersistentMap(bufferObject);
            }
            return nullptr;
        }

        void Ops_OnDestroy(SharedPtr<BackendBufferResource>&& resource) {
            if (g_activeBufferManager) {
                g_activeBufferManager->OnResourceDestroyed(std::move(resource));
            }
            // No active manager: the device/allocator is gone or going away and
            // Shutdown() already destroyed the storage; dropping the handle here
            // must not touch Vulkan. VkBufferResource's dtor destroys via VMA only
            // when the allocation is still valid, which Shutdown() cleared.
        }

        const BufferBackendOps g_vulkanBufferBackendOps = {
            .Respecify = Ops_Respecify,
            .SubData = Ops_SubData,
            .ResidentSubData = Ops_ResidentSubData,
            .FlushMappedRange = Ops_FlushMappedRange,
            .OnDestroy = Ops_OnDestroy,
            .AcquirePersistentMap = Ops_AcquirePersistentMap,
            .ReadbackFromGpu = Ops_ReadbackFromGpu,
        };

#if MOBILEGL_BUILD_DISAGGREGATED
        VkBufferManager& WireManager() {
            if (g_activeBufferManager == nullptr) {
                MGLOG_F("Magma: Fatal{ResourceUnavailable, \"wire-buffer-manager\"}");
                std::abort();
            }
            return *g_activeBufferManager;
        }

        const MG_Pipe::MGPipeResourceOps g_vulkanWireResourceOps = {
            .Create = [](auto res, const auto& desc) { WireManager().CreateWireBuffer(res, desc); },
            .Respecify = [](auto res, const auto& desc, const void* bytes) {
                WireManager().RespecifyWireBuffer(res, desc, bytes);
            },
            .SubData = [](auto res, const auto& record, const void* bytes) {
                WireManager().WriteWireBuffer(res, MG_Pipe::MGPipeSubDataBufferOffset(record),
                                               MG_Pipe::MGPipeSubDataBufferSize(record), bytes);
            },
            .SubDataResident = [](auto res, const auto& record, const void* bytes) {
                WireManager().WriteWireBuffer(res, MG_Pipe::MGPipeSubDataBufferOffset(record),
                                               MG_Pipe::MGPipeSubDataBufferSize(record), bytes);
            },
            .FlushRange = [](auto res, const auto& record, const void* bytes) {
                WireManager().FlushWireBuffer(res, record.Offset, record.Size, bytes);
            },
            .Readback = [](auto res, const auto& record) {
                WireManager().ReadbackWireBuffer(res, record.Offset, record.Size);
            },
            .Destroy = [](auto res) { WireManager().DestroyWireBuffer(res); },
            // Split mapping remains T2: the client owns its map and pushes exact
            // modified ranges. A server pointer is never donated across the wire.
            .MapPersistent = [](MG_Pipe::MGPipeHandle, Uint64, const void*) -> void* { return nullptr; },
            .UnmapPersistent = [](MG_Pipe::MGPipeHandle) {},
        };
#endif
    } // namespace

#if MOBILEGL_BUILD_DISAGGREGATED
    void VkBufferManager::RegisterWireResourceOps() {
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            MG_Pipe::MGPipeSetResourceOps(&g_vulkanWireResourceOps);
        }
    }

    VkBufferManager::WireBufferResource* VkBufferManager::FindWireBuffer(MG_Pipe::MGPipeHandle res) {
        const auto found = m_wireBuffers.find(WireBufferKey(res));
        return found == m_wireBuffers.end() ? nullptr : &found->second;
    }

    void VkBufferManager::CreateWireBuffer(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc) {
        if (FindWireBuffer(res) != nullptr) {
            MGLOG_F("Magma: Fatal{ProtocolCorruption, \"duplicate-buffer-create\"} {slot=%u, gen=%u}",
                    res.Slot, res.Gen);
            std::abort();
        }
        m_wireBuffers.try_emplace(WireBufferKey(res));
        RespecifyWireBuffer(res, desc, nullptr);
    }

    void VkBufferManager::RespecifyWireBuffer(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc,
                                               const void* initialBytes) {
        auto* resource = FindWireBuffer(res);
        if (!resource) {
            MGLOG_F("Magma: Fatal{ProtocolCorruption, \"unknown-buffer-respecify\"} {slot=%u, gen=%u}",
                    res.Slot, res.Gen);
            std::abort();
        }
        // THE SERIAL IS READ BEFORE THE RESET BELOW ZEROES IT. Whether it is zero - whether any
        // GPU command has named this store - decides whether the orphan can go at once or has
        // to wait for a submission (DeferredWireRelease), and three lines from now the record
        // no longer carries it.
        const Uint64 orphanedUseSerial = resource->lastUseSerial;
        DeferWireRelease(std::move(resource->buffer), orphanedUseSerial);
        resource->size = desc.Width;
        resource->lastUseSerial = 0;
        resource->gpuWritesPending = false;
        resource->stagedCoverage.clear();
        ++m_sliceEpochCounter;
        if (resource->size == 0) return;

        VkBufferUsageFlags usage = kPersistentBackedUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (m_initInfo.transformFeedbackUsageEnabled) usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
        if (!resource->buffer.Create({
                .allocator = m_initInfo.allocator,
                .size = resource->size,
                .usage = usage,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            }) || resource->buffer.Map() == nullptr) {
            MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-storage\"} {slot=%u, gen=%u, size=%llu}",
                    res.Slot, res.Gen, static_cast<unsigned long long>(resource->size));
            std::abort();
        }
        ++m_wireStoreCount;
        // Under transport, defined bytes follow as resource_subdata records. An
        // undefined store has no shadow to upload and no implicit zero snapshot.
        if (initialBytes != nullptr && desc.HasDefinedContent) {
            WriteWireBuffer(res, 0, resource->size, initialBytes);
        }
    }

    Bool VkBufferManager::WaitForWireBufferHostAccess(WireBufferResource& resource) {
        // B3 review: the early return must consult the same submission term WriteWireBuffer
        // does, or a caller that reaches here with the serial term idle skips the wait the
        // term would have demanded.
        if (resource.lastUseSerial <= GetCompletedSerial() && !resource.gpuWritesPending &&
            (pVulkanRenderer == nullptr || pVulkanRenderer->IsSubmitIndexComplete(resource.lastUseSubmitIndex))) {
            return true;
        }
        if (!m_copyProvider || !pVulkanRenderer) return false;
        const VkCommandBuffer commands = m_copyProvider->AcquireBufferCopyCommandBuffer();
        if (commands == VK_NULL_HANDLE) return false;
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        if (!pVulkanRenderer->WaitForSubmitIndex(pVulkanRenderer->GetSyncPointSubmitIndex(), UINT64_MAX, true)) {
            return false;
        }
        resource.lastUseSerial = 0;
        // B3: the wait above proved every submission at or below the sync point complete, so
        // the submission term must be cleared with the serial or it would keep answering busy.
        resource.lastUseSubmitIndex = 0;
        resource.gpuWritesPending = false;
        return true;
    }

    void VkBufferManager::WriteWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size,
                                           const void* bytes) {
        auto* resource = FindWireBuffer(res);
        if (!resource || offset > resource->size || size > resource->size - offset || (size && !bytes)) {
            MGLOG_F("Magma: Fatal{ProtocolCorruption, \"buffer-write-range\"} {slot=%u, gen=%u}",
                    res.Slot, res.Gen);
            std::abort();
        }
        if (size == 0) return;
        Bool uploaded = false;
        const Uint64 probeLastUse = resource->lastUseSerial;
        const Uint64 probeCompleted = GetCompletedSerial();

        // ---- P7 wave 2 package B3: WHAT EACH TERM OF THIS PREDICATE IS FOR -----------------
        //
        // A streamed `glBufferSubData -> draw` pair is only correct if the write is ordered
        // against every draw that still reads the bytes it overwrites; a write this predicate
        // answers "idle" is applied as an unsynchronised host memcpy, and a draw still reading
        // the range then renders bytes that are not its own - silently, with no log line, no GL
        // error and no Fatal. That was the OpenRA device divergence (magma-b3.md §2).
        //
        // THE GUARANTEE IS THE SERIAL TERM, and it is sound because of the FLOOR, not because
        // of anything here. GetCompletedSerial() is max(m_frameSerial - frameCount,
        // m_completedSerialFloor). The floor used to be raised to a retired submission's frame
        // serial even while another submission carrying the SAME serial was still executing -
        // on the wire arm a serial has two (a mid-frame FlushPendingCommands under a pooled
        // fence, then Present) - so "frame N-1 complete" was asserted on a fence nobody waited.
        // VulkanRenderer::OnSubmitsCompletedUpTo now raises it only to a serial no in-flight
        // submission still carries, and run_trace_case.cmake reds a split retrace on any
        // "MGWIRE-FLOOR unsound-serial-complete" line, which is that fix's lane.
        //
        // THE SUBMISSION TERM IS A PROBE AND A DEFENCE, NOT A SECOND GUARANTEE. It is not sound
        // on its own: lastUseSubmitIndex is stamped at AcquireWireSlice with the next submission
        // index, but the draw that acquired the slice can still be pushed into a LATER
        // submission - BindProgramUniformBuffers -> SyncWireTextureShape's preserve path ->
        // FlushWirePendingCommandsForTextureUpdate submits the stamped index without the draw -
        // so the stamp can name S1 while the draw rides S2. What it does do: with the floor
        // sound it should never be the term that says "busy" (the WBUF probe reports bySubmit
        // separately, so a device that shows one has found a floor hole), and it is the only
        // thing the MGITEST_MAGMA_FORCE_STALE_BUFFER_SERIAL knob leaves standing, which is what
        // the StaleSerial. entries pin.
        Bool busyBySerial = resource->lastUseSerial > GetCompletedSerial();
        if (busyBySerial && ForceStaleSerialSelector() != 0) {
            const Uint64 index = ++g_orderedWireWriteCounter;
            const Uint64 selector = ForceStaleSerialSelector();
            if (selector == 1 || selector == index) {
                busyBySerial = false;
                MGLOG_W("MGITEST_MAGMA_FORCE_STALE_BUFFER_SERIAL: write #%llu {slot=%u, gen=%u} off=%llu "
                        "size=%llu - the counting term is forced to answer \"idle\"",
                        static_cast<unsigned long long>(index), res.Slot, res.Gen,
                        static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));
            }
        }
        const Bool busyBySubmission =
            pVulkanRenderer != nullptr && !pVulkanRenderer->IsSubmitIndexComplete(resource->lastUseSubmitIndex);
        if (busyBySerial || busyBySubmission) {
            // Vulkan buffer copies require four-byte aligned ranges. An odd GL
            // byte update waits, then writes only the exact range; rounding it
            // from a CPU shadow could overwrite neighbouring GPU-written bytes.
            if (((offset | size) & 3u) == 0) {
                uploaded = StagedWireRangeCopy(*resource, bytes, static_cast<SizeT>(offset),
                                                static_cast<SizeT>(size));
            }
            if (!uploaded && !WaitForWireBufferHostAccess(*resource)) {
                MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-write-sync\"}");
                std::abort();
            }
        }
        if (!uploaded) {
            uploaded = resource->buffer.Upload(bytes, size, offset);
            if (uploaded && MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer, size);
            }
        }
        if (!uploaded) {
            MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-upload\"}");
            std::abort();
        }
        if (WireBufProbeEnabled()) {
            MGLOG_I("WBUF write slot=%u gen=%u off=%llu size=%llu branch=%s bySerial=%d bySubmit=%d "
                    "lastUse=%llu completed=%llu frameSerial=%llu floor=%llu lastUseSubmit=%llu",
                    res.Slot, res.Gen, static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(size),
                    (busyBySerial || busyBySubmission) ? "STAGED-or-WAIT" : "HOST-IMMEDIATE",
                    busyBySerial ? 1 : 0, busyBySubmission ? 1 : 0,
                    static_cast<unsigned long long>(probeLastUse), static_cast<unsigned long long>(probeCompleted),
                    static_cast<unsigned long long>(m_frameSerial),
                    static_cast<unsigned long long>(m_completedSerialFloor),
                    static_cast<unsigned long long>(resource->lastUseSubmitIndex));
        }
        MG_Remote::Server::StagedShadowStore::CoverageAdd(resource->stagedCoverage,
                                                         static_cast<SizeT>(offset),
                                                         static_cast<SizeT>(offset + size));
        ++m_sliceEpochCounter;
    }

    void VkBufferManager::FlushWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size,
                                           const void* bytes) {
        if (bytes != nullptr) {
            WriteWireBuffer(res, offset, size, bytes);
            return;
        }
        const auto* resource = FindWireBuffer(res);
        if (!resource || offset > resource->size || size > resource->size - offset ||
            !MG_Remote::Server::StagedShadowStore::CoverageHas(resource->stagedCoverage,
                static_cast<SizeT>(offset), static_cast<SizeT>(offset + size))) {
            MGLOG_F("Magma: Fatal{StageSnapshotTooNarrow, \"buffer-flush\"} {slot=%u, gen=%u}",
                    res.Slot, res.Gen);
            std::abort();
        }
        // SubData has already copied the owned record bytes into GPU storage,
        // ordered after earlier uses. There is no stale shadow to replay here.
    }

    Bool VkBufferManager::AcquireWireSlice(BufferKind kind, MG_Pipe::MGPipeHandle res, BufferSlice& outSlice) {
        (void)kind; // The store carries every buffer usage, so changing roles never orphans it.
        outSlice = {};
        auto* resource = FindWireBuffer(res);
        if (!resource || !resource->buffer.IsValid() || resource->size == 0) return false;
        resource->lastUseSerial = m_frameSerial;
        // B3: the draw this slice is being acquired for is normally recorded into the NEXT
        // submission - normally, not always: a mid-draw flush (SyncWireTextureShape's preserve
        // path) can submit this index without the draw. This stamp is the defence term's input
        // (see WriteWireBuffer); the floor behind lastUseSerial is the guarantee.
        if (pVulkanRenderer) resource->lastUseSubmitIndex = pVulkanRenderer->GetWireNextSubmitIndex();
        outSlice = resource->buffer.GetSlice(0, resource->size);
        outSlice.mapped = nullptr;
        if (WireBufProbeEnabled()) {
            MGLOG_I("WBUF bind  slot=%u gen=%u frameSerial=%llu", res.Slot, res.Gen,
                    static_cast<unsigned long long>(m_frameSerial));
        }
        return true;
    }

    Bool VkBufferManager::ReadWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size, void* dst) {
        auto* resource = FindWireBuffer(res);
        if (!resource || offset > resource->size || size > resource->size - offset || (size && !dst)) return false;
        if (size == 0) return true;
        // Concurrent GPU reads do not prevent a CPU read. Only staged copies or
        // shader writes need a host visibility barrier and a submission wait;
        // ordinary index/vertex inspection must not stall once per draw.
        if (resource->gpuWritesPending) {
            if (!WaitForWireBufferHostAccess(*resource)) return false;
            // A draw may already hold a native slice of this store while another
            // attribute needs a CPU conversion. The wait completes previous GPU
            // work, not the draw that will bind that previously acquired slice.
            // Preserve its reservation even if the idle drain advanced the frame.
            resource->lastUseSerial = m_frameSerial;
            // B3 review: and the submission half of the same reservation - the wait cleared it,
            // and the draw holding the slice has not been submitted yet.
            if (pVulkanRenderer) resource->lastUseSubmitIndex = pVulkanRenderer->GetWireNextSubmitIndex();
        }
        if (!resource->buffer.Invalidate(size, offset)) return false;
        Memcpy(dst, static_cast<const Uint8*>(resource->buffer.GetMappedData()) + offset, static_cast<SizeT>(size));
        return true;
    }

    Bool VkBufferManager::CopyWireBufferRangeToSlice(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size,
                                                     const BufferSlice& dst) {
        auto* resource = FindWireBuffer(res);
        if (!resource || offset > resource->size || size > resource->size - offset || size > dst.size) return false;
        if (size == 0) return true;
        if (!m_copyProvider || dst.buffer == VK_NULL_HANDLE || ((offset | size | dst.offset) & 3u) != 0) return false;
        const VkCommandBuffer commands = m_copyProvider->AcquireBufferCopyCommandBuffer();
        if (commands == VK_NULL_HANDLE) return false;
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        const VkBufferCopy copy{offset, dst.offset, size};
        vkCmdCopyBuffer(commands, resource->buffer.GetHandle(), dst.buffer, 1, &copy);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
        resource->lastUseSerial = m_frameSerial;
        return true;
    }

    Bool VkBufferManager::CopyWireBufferSubWordRangeToSlice(MG_Pipe::MGPipeHandle res, Uint64 offset,
                                                            Uint64 size, Uint32 frameIndex,
                                                            const BufferSlice& dst, Uint64 dstSkip) {
        auto* resource = FindWireBuffer(res);
        if (!resource || offset > resource->size || size > resource->size - offset) return false;
        if (dstSkip > dst.size || size > dst.size - dstSkip) return false;
        if (size == 0) return true;
        if (!m_copyProvider || dst.buffer == VK_NULL_HANDLE) return false;

        // Round the read of the application's store OUT to whole words. The tail can only be
        // clipped by the end of the store itself, and [offset, offset + size) already fits
        // inside it, so the widened window still covers every byte the caller asked for.
        const Uint64 alignedOffset = offset & ~Uint64{3};
        const Uint64 stagedEnd = std::min<Uint64>((offset + size + 3) & ~Uint64{3}, resource->size);
        const Uint64 headPad = offset - alignedOffset;
        const Uint64 stagedSize = stagedEnd - alignedOffset;
        if (stagedSize < headPad + size) return false;

        // Uninitialized on purpose: the widening copy below writes every byte of it. The
        // arena can grow to satisfy this, which parks (never frees) the buffer `dst` may
        // already name - see BufferArena::EnsureCapacity - so the two slices are allowed to
        // sit in different VkBuffers and the copy names each slice's own handle.
        BufferSlice staging{};
        if (!m_transientUploadArena.Allocate(frameIndex, stagedSize, 4, staging) || !staging.IsValid())
            return false;

        const VkCommandBuffer commands = m_copyProvider->AcquireBufferCopyCommandBuffer();
        if (commands == VK_NULL_HANDLE) return false;
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        const VkBufferCopy widen{alignedOffset, staging.offset, stagedSize};
        vkCmdCopyBuffer(commands, resource->buffer.GetHandle(), staging.buffer, 1, &widen);
        // The shift reads what the widening copy just wrote, so it needs its own edge even
        // though both halves are transfers on one command buffer.
        VkMemoryBarrier staged{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        staged.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        staged.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &staged, 0, nullptr, 0, nullptr);
        const VkBufferCopy shift{staging.offset + headPad, dst.offset + dstSkip, size};
        vkCmdCopyBuffer(commands, staging.buffer, dst.buffer, 1, &shift);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
        resource->lastUseSerial = m_frameSerial;
        return true;
    }

    void VkBufferManager::ReadbackWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size) {
        Vector<Uint8> bytes(static_cast<SizeT>(size));
        if (!ReadWireBuffer(res, offset, size, bytes.data())) {
            MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-readback\"} {slot=%u, gen=%u}", res.Slot, res.Gen);
            std::abort();
        }
        if (MG_Pipe::gMGPipeCallbacks.OnBufferWriteback == nullptr) {
            MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-writeback-callback\"}");
            std::abort();
        }
        // The reverse producer synchronously copies into SEG_EVENT before
        // this owned vector is released; no mapped Vulkan pointer crosses roles.
        MG_Pipe::gMGPipeCallbacks.OnBufferWriteback(res, offset,
            {reinterpret_cast<Uint64>(bytes.data()), size, MG_Pipe::kMGHostSpanSegNone, 0});
        ++m_sliceEpochCounter;
    }

    void VkBufferManager::MarkWireBufferGpuWritten(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size) {
        auto* resource = FindWireBuffer(res);
        if (!resource || offset > resource->size) return;
        if (size == MG_Pipe::kMGPipeWholeBuffer) size = resource->size - offset;
        if (size == 0 || size > resource->size - offset) return;
        resource->lastUseSerial = m_frameSerial;
        resource->stagedCoverage.clear();
        resource->gpuWritesPending = true;
        ++m_sliceEpochCounter;
        // The existing client dirty-state protocol accepts one whole-store
        // notification. The store remains on the GPU and exact later SubData
        // only overwrites its own range, preserving every other GPU-written byte.
        const MG_Pipe::MGPRange range{0, MG_Pipe::kMGPipeWholeBuffer};
        if (MG_Pipe::gMGPipeCallbacks.OnGpuWritten == nullptr) {
            MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-gpu-written-callback\"}");
            std::abort();
        }
        MG_Pipe::gMGPipeCallbacks.OnGpuWritten(res, 1, &range);
    }

    void VkBufferManager::DestroyWireBuffer(MG_Pipe::MGPipeHandle res) {
        const auto found = m_wireBuffers.find(WireBufferKey(res));
        if (found == m_wireBuffers.end()) return;
        DeferWireRelease(std::move(found->second.buffer), found->second.lastUseSerial);
        m_wireBuffers.erase(found);
        ++m_sliceEpochCounter;
    }

    void VkBufferManager::DeferWireRelease(VkBufferObject&& buffer, Uint64 lastUseSerial) {
        if (!buffer.IsValid()) return;
        // Two stores go NOW rather than into the list. With no frame slots (DeferRelease's own
        // teardown guard, and for its reason) there is no manager left to reclaim anything
        // later. And a store whose last-use serial is 0 was never named by a GPU command since
        // it was minted or since a host-access wait retired everything recorded before it - see
        // DeferredWireRelease - so there is nothing to wait for.
        if (m_deferredBufferReleases.empty() || lastUseSerial == 0) {
            buffer.Destroy();
            if (m_wireStoreCount > 0) --m_wireStoreCount;
            return;
        }
        DeferredWireRelease entry;
        entry.lastUseSerial = lastUseSerial;
        // The last submission that can name this store: see DeferredWireRelease. Zero when there
        // is no renderer, which is also when there is no queue and nothing to wait for.
        entry.submitIndex = pVulkanRenderer != nullptr ? pVulkanRenderer->GetSyncPointSubmitIndex() : 0;
        entry.bytes = static_cast<Uint64>(buffer.GetSize());
        entry.buffer = std::move(buffer);
        m_deferredWireBytes += entry.bytes;
        m_deferredWireReleases.push_back(std::move(entry));
        // EVERY PARK SWEEPS. The frame boundary is not a reclaim point this arm can lean on - a
        // pbuffer replay that snapshot-exits delivers ONE present for 1.3 M calls (ID-P7-32) -
        // so the only cadence that tracks the workload is the workload itself.
        SweepDeferredWireReleases();
    }

    SizeT VkBufferManager::SweepDeferredWireReleases() {
        if (m_deferredWireReleases.empty() || pVulkanRenderer == nullptr) return 0;
        SizeT dead = 0;
        if (pVulkanRenderer->IsSubmitIndexComplete(pVulkanRenderer->GetSyncPointSubmitIndex())) {
            // Idle: nothing recorded, every submission retired. Every parked store is dead,
            // including one tagged for a batch that was abandoned instead of submitted.
            dead = m_deferredWireReleases.size();
        } else {
            // The list is in park order and GetSyncPointSubmitIndex() never decreases, so the
            // submit indices are non-decreasing and the dead entries are a PREFIX: the first
            // entry the renderer will not call complete proves none after it is either. That
            // bounds the walk to a couple of fence polls, which matters because this runs on
            // every glBufferData the wire carries.
            while (dead < m_deferredWireReleases.size() &&
                   pVulkanRenderer->IsSubmitIndexComplete(m_deferredWireReleases[dead].submitIndex)) {
                ++dead;
            }
        }
        if (dead == 0) return 0;
        for (SizeT i = 0; i < dead; ++i) {
            DeferredWireRelease& entry = m_deferredWireReleases[i];
            m_deferredWireBytes -= entry.bytes;
            entry.buffer.Destroy();
        }
        m_deferredWireReleases.erase(m_deferredWireReleases.begin(),
                                     m_deferredWireReleases.begin() + static_cast<std::ptrdiff_t>(dead));
        const Uint64 destroyedCount = static_cast<Uint64>(dead);
        m_wireStoreCount = destroyedCount <= m_wireStoreCount ? m_wireStoreCount - destroyedCount : 0;
        return dead;
    }

    void VkBufferManager::DestroyAllDeferredWireReleases() {
        for (auto& entry : m_deferredWireReleases) {
            entry.buffer.Destroy();
            if (m_wireStoreCount > 0) --m_wireStoreCount;
        }
        m_deferredWireReleases.clear();
        m_deferredWireBytes = 0;
    }
#endif

    Bool VkBufferManager::Initialize(const VkBufferManagerInitInfo& initInfo) {
        Shutdown();

        MOBILEGL_ASSERT(initInfo.allocator != nullptr, "VkBufferManager::Initialize requires valid allocator");
        MOBILEGL_ASSERT(initInfo.frameCount > 0, "VkBufferManager::Initialize requires non-zero frame count");

        m_initInfo = initInfo;
        m_deferredBufferReleases.resize(initInfo.frameCount);
        m_deferredResourceReleases.resize(initInfo.frameCount);
        m_currentFrameIndex = 0;
        m_frameSerial = 1;
        m_completedSerialFloor = 0;
        if (!InitializeTransientArenas()) {
            return false;
        }
        g_activeBufferManager = this;
        MG_State::GLState::SetBufferBackendOps(&g_vulkanBufferBackendOps);
#if MOBILEGL_BUILD_DISAGGREGATED
        RegisterWireResourceOps();
#endif
        return true;
    }

    void VkBufferManager::Shutdown() {
        if (g_activeBufferManager == this) {
            g_activeBufferManager = nullptr;
#if MOBILEGL_BUILD_DISAGGREGATED
            if (MG_Pipe::MGPipeGetResourceOps() == &g_vulkanWireResourceOps) {
                MG_Pipe::MGPipeSetResourceOps(nullptr);
            }
#endif
            if (MG_State::GLState::GetBufferBackendOps() == &g_vulkanBufferBackendOps) {
                MG_State::GLState::SetBufferBackendOps(nullptr);
            }
        }
        m_transientUploadArena.Shutdown();
        m_unboundStorageBuffer.Destroy();
        m_unboundTexelBuffer.Destroy();
        DestroyAllDeferredReleases();
        ReleaseAllLiveResources();
#if MOBILEGL_BUILD_DISAGGREGATED
        // DestroyAllDeferredReleases above emptied the parked list; this destroys the stores the
        // records still hold, so nothing this arm minted outlives the count.
        m_wireBuffers.clear();
        m_wireStoreCount = 0;
#endif
        m_copyProvider = nullptr;
        m_initInfo = {};
        m_currentFrameIndex = 0;
        m_frameSerial = 1;
        m_completedSerialFloor = 0;
    }

    Bool VkBufferManager::RecreateTransientArenas(Uint32 frameCount) {
        MOBILEGL_ASSERT(m_initInfo.allocator != nullptr,
                        "VkBufferManager::RecreateTransientArenas requires initialized manager");
        MOBILEGL_ASSERT(frameCount > 0, "VkBufferManager::RecreateTransientArenas requires non-zero frame count");

        // Callers guarantee the device is idle around arena recreation.
        NotifyDeviceIdle();
        m_transientUploadArena.Shutdown();
        m_initInfo.frameCount = frameCount;
        DestroyAllDeferredReleases();
        m_deferredBufferReleases.resize(frameCount);
        m_deferredResourceReleases.resize(frameCount);
        m_currentFrameIndex = 0;
        return InitializeTransientArenas();
    }

    void VkBufferManager::BeginFrame(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_deferredBufferReleases.size(),
                        "VkBufferManager::BeginFrame frame index out of range");
        m_currentFrameIndex = frameIndex;
        ++m_frameSerial;
#if MOBILEGL_BUILD_DISAGGREGATED
        if (WireBufProbeEnabled()) {
            MGLOG_I("WBUF BEGINFRAME idx=%u newSerial=%llu floor=%llu (arena slot rewound)", frameIndex,
                    static_cast<unsigned long long>(m_frameSerial),
                    static_cast<unsigned long long>(m_completedSerialFloor));
        }
#endif
        CollectDeferredReleases(frameIndex);
        m_transientUploadArena.BeginFrame(frameIndex);
    }

    void VkBufferManager::CollectAllDeferredReleases() {
        // Per-resource releases only. Every one of them was deferred behind a BumpSliceEpoch,
        // so no memo can still name the handle, and the caller has proved the GPU is idle.
        //
        // The transient arena's releases are deliberately NOT collected here. A buffer lands
        // there when the arena outgrows it mid-frame (BufferArena::EnsureCapacity), and at
        // that moment every slice already handed out from this frame's arena still names it -
        // VkBufferResource::transientSlice above all, which AcquireStreamedSlice keeps
        // serving for the whole frame serial on the strength of transientFrameSerial alone.
        // Nothing bumps the slice epoch for those other resources, so freeing the buffer
        // here left the streamed memo handing a destroyed VkBuffer to vkCmdBindIndexBuffer
        // (llvmpipe then faulted inside the draw; the Create/Flywheel indirect retrace died
        // exactly this way). Mid-frame drains do not advance m_frameSerial, so they must not
        // free arena storage either: the arena's own ResetFrame/BeginFrame is the point where
        // the slot's slices stop being reachable, and that is where these releases land.
        for (Uint32 frameIndex = 0; frameIndex < m_deferredBufferReleases.size(); ++frameIndex) {
            CollectDeferredReleases(frameIndex);
        }
    }

    void VkBufferManager::NotifyDeviceIdle() {
        // Everything submitted so far has completed. Work recorded for the
        // current frame has not been submitted yet, so the current serial
        // remains busy.
        if (m_frameSerial > 0) {
            m_completedSerialFloor = m_frameSerial - 1;
        }
    }

    void VkBufferManager::NotifyFrameSerialComplete(Uint64 serial) {
        // The current serial's work is still being recorded; a completion
        // report for it (or beyond) can only come from a stale caller.
        if (serial >= m_frameSerial) {
            return;
        }
        m_completedSerialFloor = std::max(m_completedSerialFloor, serial);
    }

    void VkBufferManager::SetCopyCommandProvider(IBufferCopyCommandProvider* provider) {
        m_copyProvider = provider;
    }

    Uint64 VkBufferManager::GetCompletedSerial() const {
        const Uint64 frameCount = m_initInfo.frameCount > 0 ? m_initInfo.frameCount : 1;
        const Uint64 completed = m_frameSerial > frameCount ? m_frameSerial - frameCount : 0;
        return std::max(completed, m_completedSerialFloor);
    }

    Bool VkBufferManager::IsResourceBusy(const VkBufferResource& resource) const {
        return resource.lastUseSerial > GetCompletedSerial();
    }

    Bool VkBufferManager::UploadTransient(BufferKind kind, Uint32 frameIndex, const void* data,
                                          VkDeviceSize size, VkDeviceSize alignment, BufferSlice& outSlice) {
        if (!m_transientUploadArena.Upload(frameIndex, data, size, alignment, outSlice)) {
            return false;
        }
        if (MG_Util::PipeStats::Enabled()) {
            // The single chokepoint for Magma's per-draw staging. Uniform is deliberately
            // absent: its bytes are counted by the caller, which is the only place that
            // knows whether the payload is the default block (stage-ubo-global) or a named
            // one repacked into the ring (stage-ubo-named), and counting here as well would
            // double every uniform byte.
            switch (kind) {
            case BufferKind::Vertex:
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageVertexClient,
                                             static_cast<Uint64>(size));
                break;
            case BufferKind::Index:
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageIndexClient,
                                             static_cast<Uint64>(size));
                break;
            case BufferKind::Indirect:
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageIndirectCmd,
                                             static_cast<Uint64>(size));
                break;
            case BufferKind::TextureBuffer:
            case BufferKind::ShaderStorage:
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                             static_cast<Uint64>(size));
                break;
            case BufferKind::Uniform:
                break;
            }
        }
        return true;
    }

    Bool VkBufferManager::InitializeTransientArenas() {
        return m_transientUploadArena.Initialize({
            .allocator = m_initInfo.allocator,
            .frameCount = m_initInfo.frameCount,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
#if MOBILEGL_BUILD_DISAGGREGATED
                     (MG_Config::Transport != MG_Config::TransportMode::Monolith ? VkBufferUsageFlags{VK_BUFFER_USAGE_TRANSFER_DST_BIT} : 0u) |
#endif
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .memoryUsage = m_initInfo.transientMemoryUsage,
            .allocationFlags = m_initInfo.transientAllocationFlags,
            .minBufferSize = m_initInfo.minUploadBytes,
            .persistentlyMapped = m_initInfo.transientPersistentMapping,
        });
    }

    VkBufferResource* VkBufferManager::ResourceOf(MG_State::GLState::BufferObject& bufferObject) {
        return static_cast<VkBufferResource*>(bufferObject.GetBackendResource().get());
    }

    VkBufferResource* VkBufferManager::GetOrCreateResource(
        const SharedPtr<MG_State::GLState::BufferObject>& bufferObject) {
        // Return by raw pointer: the resource is owned for its whole lifetime by the BufferObject's
        // backend-resource SharedPtr (already set, or set below), so callers that only dereference
        // it avoid a static_pointer_cast + SharedPtr refcount inc/dec on every per-draw buffer bind.
        const auto& existing = bufferObject->GetBackendResource();
        if (existing) {
            return static_cast<VkBufferResource*>(existing.get());
        }
        auto resource = MakeShared<VkBufferResource>();
        VkBufferResource* raw = resource.get();
        bufferObject->SetBackendResource(resource);
        TrackLiveResource(resource);
        return raw;
    }

    void VkBufferManager::TrackLiveResource(const SharedPtr<VkBufferResource>& resource) {
        // Sweep on a doubling watermark rather than on every insert past the threshold. The old
        // form walked the whole vector for each new buffer once the list passed 256, and when the
        // buffers are all live the walk removes nothing and the list grows by one - so creating N
        // live buffers cost ~N^2/2 expired() checks. Reclamation semantics are unchanged: the sweep
        // still removes exactly the expired entries, just less often and with the same bound on how
        // much dead weight can accumulate (at most as many entries as were live at the last sweep).
        if (m_liveResources.size() >= std::max<SizeT>(kLiveResourcePruneThreshold, 2 * m_liveResourcesLastPruned)) {
            std::erase_if(m_liveResources, [](const WeakPtr<VkBufferResource>& weak) { return weak.expired(); });
            m_liveResourcesLastPruned = m_liveResources.size();
        }
        m_liveResources.push_back(resource);
    }

    void VkBufferManager::ReleaseAllLiveResources() {
        for (auto& weak : m_liveResources) {
            if (auto resource = weak.lock()) {
                BumpSliceEpoch(*resource);
                resource->buffer.Destroy();
                resource->storageSize = 0;
                resource->usageFlags = 0;
                resource->lastUseSerial = 0;
                resource->pendingFullUpload = true;
                resource->transientSlice = {};
                resource->transientFrameSerial = 0;
            }
        }
        m_liveResources.clear();
    }

    Bool VkBufferManager::CreateResidentStorage(VkBufferResource& resource, VkDeviceSize size,
                                                VkBufferUsageFlags usage, VkMemoryPropertyFlags requiredFlags) {
        // The only place a resident VkBuffer handle is minted, so every resident slice
        // change funnels through here (callers release the old handle first).
        BumpSliceEpoch(resource);
        // Staged range copies write resident storage with vkCmdCopyBuffer.
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const Bool created = resource.buffer.Create({
            .allocator = m_initInfo.allocator,
            .size = size,
            .usage = usage,
            .memoryUsage = VMA_MEMORY_USAGE_AUTO,
            .allocationFlags = kResidentBufferAllocationFlags,
            .requiredFlags = requiredFlags,
        });
        if (!created || resource.buffer.Map() == nullptr) {
            MGLOG_E_ONCE("VkBufferManager::CreateResidentStorage failed (size=%llu)",
                    static_cast<unsigned long long>(size));
            resource.buffer.Destroy();
            resource.storageSize = 0;
            resource.usageFlags = 0;
            return false;
        }
        resource.storageSize = size;
        resource.usageFlags = usage;
        return true;
    }

    Bool VkBufferManager::SwapStorageAndUploadAll(VkBufferResource& resource,
                                                  MG_State::GLState::BufferObject& bufferObject) {
        const VkDeviceSize size = static_cast<VkDeviceSize>(bufferObject.GetSize());
        const VkBufferUsageFlags usage = resource.usageFlags;
        DeferRelease(std::move(resource.buffer));
        if (!CreateResidentStorage(resource, size, usage)) {
            resource.pendingFullUpload = true;
            return false;
        }
        if (!resource.buffer.Upload(bufferObject.MappedData(), size, 0)) {
            MGLOG_E_ONCE("VkBufferManager::SwapStorageAndUploadAll: upload failed");
            resource.pendingFullUpload = true;
            return false;
        }
        if (MG_Util::PipeStats::Enabled()) {
            MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer, static_cast<Uint64>(size));
        }
        resource.pendingFullUpload = false;
        return true;
    }

    Bool VkBufferManager::StagedRangeCopy(VkBufferResource& resource, const void* data,
                                          SizeT offset, SizeT size) {
        if (!m_copyProvider) {
            return false;
        }
        BufferSlice staging{};
        if (!m_transientUploadArena.Upload(m_currentFrameIndex, data,
                                           static_cast<VkDeviceSize>(size), 16, staging)) {
            return false;
        }
        if (MG_Util::PipeStats::Enabled()) {
            // The staging fill is the host copy; the vkCmdCopyBuffer below is the device
            // half of the same bytes and is not counted twice.
            MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer, static_cast<Uint64>(size));
        }
        VkCommandBuffer commandBuffer = m_copyProvider->AcquireBufferCopyCommandBuffer();
        if (commandBuffer == VK_NULL_HANDLE) {
            return false;
        }

        // Order the copy after every prior read/write of this buffer, both from
        // in-flight frames (submission order) and from commands already recorded
        // in this frame's command buffer.
        VkMemoryBarrier beforeBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        beforeBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        beforeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &beforeBarrier, 0, nullptr, 0, nullptr);

        VkBufferCopy region{};
        region.srcOffset = staging.offset;
        region.dstOffset = static_cast<VkDeviceSize>(offset);
        region.size = static_cast<VkDeviceSize>(size);
        vkCmdCopyBuffer(commandBuffer, staging.buffer, resource.buffer.GetHandle(), 1, &region);

        VkMemoryBarrier afterBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        afterBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        afterBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                             &afterBarrier, 0, nullptr, 0, nullptr);

        resource.lastUseSerial = m_frameSerial;
        return true;
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    Bool VkBufferManager::StagedWireRangeCopy(WireBufferResource& resource, const void* data,
                                          SizeT offset, SizeT size) {
        if (!m_copyProvider) {
            return false;
        }
        BufferSlice staging{};
        if (!m_transientUploadArena.Upload(m_currentFrameIndex, data,
                                           static_cast<VkDeviceSize>(size), 16, staging)) {
            return false;
        }
        if (MG_Util::PipeStats::Enabled()) {
            // The staging fill is the host copy; the vkCmdCopyBuffer below is the device
            // half of the same bytes and is not counted twice.
            MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer, static_cast<Uint64>(size));
        }
        VkCommandBuffer commandBuffer = m_copyProvider->AcquireBufferCopyCommandBuffer();
        if (commandBuffer == VK_NULL_HANDLE) {
            return false;
        }

        // Order the copy after every prior read/write of this buffer, both from
        // in-flight frames (submission order) and from commands already recorded
        // in this frame's command buffer.
        VkMemoryBarrier beforeBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        beforeBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        beforeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &beforeBarrier, 0, nullptr, 0, nullptr);

        VkBufferCopy region{};
        region.srcOffset = staging.offset;
        region.dstOffset = static_cast<VkDeviceSize>(offset);
        region.size = static_cast<VkDeviceSize>(size);
        vkCmdCopyBuffer(commandBuffer, staging.buffer, resource.buffer.GetHandle(), 1, &region);

        VkMemoryBarrier afterBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        afterBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        afterBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                             &afterBarrier, 0, nullptr, 0, nullptr);

        resource.lastUseSerial = m_frameSerial;
        // B3: the copy just recorded rides the next submission too, so a later host write to
        // the same bytes must wait for it exactly as a draw's read must.
        if (pVulkanRenderer) resource.lastUseSubmitIndex = pVulkanRenderer->GetWireNextSubmitIndex();
        resource.gpuWritesPending = true;
        return true;
    }

#endif

    void VkBufferManager::OnRespecify(MG_State::GLState::BufferObject& bufferObject) {
        auto* resource = ResourceOf(bufferObject);
        if (!resource) {
            return; // lazy: AcquireResidentSlice performs a full upload on creation
        }
        // A respecify can change the size, the usage hint (so the resident/streamed
        // route), and the contents at once; retire every memo before deciding what to
        // do about the storage.
        BumpSliceEpoch(*resource);
        // Any cached streaming slice refers to the previous contents.
        resource->transientFrameSerial = 0;
        // Redefining the store hands any adopted mapping back to the CPU shadow
        // (BufferObject::RedefineStorage), so a buffer that reaches here persistent-mapped
        // is an ordinary resident one again: it needs the busy-tracking and conditional
        // orphan below, and the next AcquirePersistentMap has to mint storage for the new
        // store rather than hand back a mapping of the old one.
        resource->persistentMapped = false;
        if (!resource->buffer.IsValid()) {
            return; // streaming-only resource: shadow + serial are enough
        }

        const VkDeviceSize size = static_cast<VkDeviceSize>(bufferObject.GetSize());
        if (size == 0) {
            DeferRelease(std::move(resource->buffer));
            resource->storageSize = 0;
            resource->pendingFullUpload = false;
            return;
        }

        if (size != resource->storageSize || IsResourceBusy(*resource)) {
            // Conditional orphan: only swap the storage when the old one is
            // still referenced by the GPU (or no longer fits).
            SwapStorageAndUploadAll(*resource, bufferObject);
            return;
        }

        if (!resource->buffer.Upload(bufferObject.MappedData(), size, 0)) {
            MGLOG_E_ONCE("VkBufferManager::OnRespecify: in-place upload failed");
            resource->pendingFullUpload = true;
        } else if (MG_Util::PipeStats::Enabled()) {
            MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer, static_cast<Uint64>(size));
        }
    }

    void VkBufferManager::OnSubData(MG_State::GLState::BufferObject& bufferObject, SizeT offset, SizeT size) {
        auto* resource = ResourceOf(bufferObject);
        if (!resource) {
            return;
        }
        // Drops the streaming memo below and may end in a storage swap or a deferred
        // full re-upload, so no memoised slice survives this.
        BumpSliceEpoch(*resource);
        resource->transientFrameSerial = 0;
        if (!resource->buffer.IsValid() || resource->pendingFullUpload) {
            return;
        }
        if (static_cast<VkDeviceSize>(bufferObject.GetSize()) != resource->storageSize) {
            resource->pendingFullUpload = true;
            return;
        }

        if (!IsResourceBusy(*resource)) {
            if (!resource->buffer.Upload(bufferObject.MappedData() + offset,
                                         static_cast<VkDeviceSize>(size), static_cast<VkDeviceSize>(offset))) {
                MGLOG_E_ONCE("VkBufferManager::OnSubData: host upload failed");
                resource->pendingFullUpload = true;
            } else if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                             static_cast<Uint64>(size));
            }
            return;
        }

        // Busy partial write: stage + GPU copy preserves GL ordering within the
        // frame and leaves bytes outside the range (possibly GPU-written, e.g.
        // SSBO) intact. Fall back to a storage swap if staging is unavailable.
        if (!StagedRangeCopy(*resource, bufferObject.MappedData() + offset, offset, size)) {
            SwapStorageAndUploadAll(*resource, bufferObject);
        }
    }

    void VkBufferManager::OnResidentSubData(MG_State::GLState::BufferObject& bufferObject,
                                           SizeT offset, DataPtr data) {
        auto* resource = ResourceOf(bufferObject);
        MOBILEGL_ASSERT(resource && resource->persistentMapped && resource->buffer.IsValid(),
                        "OnResidentSubData requires adopted Vulkan storage");
        // The mapping is also the GPU's storage. Copy the supplied bytes onto the
        // command timeline before touching it: earlier draws must keep seeing the
        // old contents, including draws recorded but not yet submitted. The buffer
        // cannot be orphaned because the application may hold its mapped pointer.
        if (StagedRangeCopy(*resource, data.data, offset, data.size)) {
            return;
        }
        // Allocation failure: a host write is safe only after all prior work retires.
        if (pVulkanRenderer && pVulkanRenderer->WaitForSubmitIndex(
                pVulkanRenderer->GetSyncPointSubmitIndex(), UINT64_MAX, true)) {
            resource->buffer.Upload(data.data, data.size, offset);
        } else {
            MGLOG_E_ONCE("VkBufferManager::OnResidentSubData: ordered upload failed");
        }
    }

    void VkBufferManager::OnFlushMappedRange(MG_State::GLState::BufferObject& bufferObject, Range1D range,
                                             Flags<BufferMappingAccessBit> appAccess) {
        auto* resource = ResourceOf(bufferObject);
        if (!resource) {
            return;
        }
        BumpSliceEpoch(*resource);
        resource->transientFrameSerial = 0;
        if (!resource->buffer.IsValid() || resource->pendingFullUpload) {
            return;
        }
        if (static_cast<VkDeviceSize>(bufferObject.GetSize()) != resource->storageSize) {
            resource->pendingFullUpload = true;
            return;
        }

        const SizeT offset = range.start;
        const SizeT size = range.end - range.start;
        // GL_MAP_UNSYNCHRONIZED_BIT: the app guarantees it does not overwrite
        // data the GPU is still reading; honour it with a direct host write.
        if ((appAccess & BufferMappingAccessBit::Unsynchronized) || !IsResourceBusy(*resource)) {
            if (!resource->buffer.Upload(bufferObject.MappedData() + offset,
                                         static_cast<VkDeviceSize>(size), static_cast<VkDeviceSize>(offset))) {
                MGLOG_E_ONCE("VkBufferManager::OnFlushMappedRange: host upload failed");
                resource->pendingFullUpload = true;
            } else if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                             static_cast<Uint64>(size));
            }
            return;
        }

        if (!StagedRangeCopy(*resource, bufferObject.MappedData() + offset, offset, size)) {
            SwapStorageAndUploadAll(*resource, bufferObject);
        }
    }

    void VkBufferManager::OnResourceDestroyed(SharedPtr<MG_State::GLState::BackendBufferResource>&& resource) {
        if (!resource) {
            return;
        }
        auto vkResource = std::static_pointer_cast<VkBufferResource>(std::move(resource));
        if (!vkResource->buffer.IsValid()) {
            return;
        }
        if (m_deferredResourceReleases.empty()) {
            vkResource->buffer.Destroy();
            return;
        }
        MOBILEGL_ASSERT(m_currentFrameIndex < m_deferredResourceReleases.size(),
                        "VkBufferManager::OnResourceDestroyed current frame index out of range");
        // Keep the whole resource alive until this frame slot's fence has been
        // waited, then the storage is destroyed with it.
        m_deferredResourceReleases[m_currentFrameIndex].push_back(std::move(vkResource));
    }

    void* VkBufferManager::AcquirePersistentMap(MG_State::GLState::BufferObject& bufferObject) {
        const VkDeviceSize size = static_cast<VkDeviceSize>(bufferObject.GetSize());
        if (size == 0) {
            return nullptr;
        }

        auto resource = std::static_pointer_cast<VkBufferResource>(bufferObject.GetBackendResource());
        if (!resource) {
            resource = MakeShared<VkBufferResource>();
            bufferObject.SetBackendResource(resource);
            TrackLiveResource(resource);
        }

        // Bumped for the request, not just for the storage it may create. This is the
        // one call the frontend makes when a buffer becomes persistently mapped for
        // writing (BufferObject::AcquireMemoryRange), and a map the backend declines
        // keeps mutating its shadow with no further API call - so it is what lets
        // GetSliceEpochCounter stand for "no buffer needs a persistent-map range push".
        BumpSliceEpoch(*resource);

        // Idempotent: an already-backed buffer returns the same mapped base.
        if (resource->persistentMapped && resource->buffer.IsValid() && resource->storageSize == size) {
            return resource->buffer.GetMappedData();
        }

        // One-time creation of HOST_VISIBLE + HOST_COHERENT, persistently mapped storage
        // carrying every usage (never recreated, so the app's pointer never dangles). Seed
        // it from the current shadow - MappedData() is still the shadow here because the
        // frontend adopts (and drops) the shadow only after this returns.
        DeferRelease(std::move(resource->buffer));
        const VkBufferUsageFlags persistentUsage =
            kPersistentBackedUsage |
            (m_initInfo.transformFeedbackUsageEnabled ? kTransformFeedbackUsage : 0);
        if (!CreateResidentStorage(*resource, size, persistentUsage, kPersistentBackedRequiredFlags)) {
            resource->persistentMapped = false;
            resource->storageSize = 0;
            resource->usageFlags = 0;
            return nullptr;
        }
        const Uint8* seed = bufferObject.MappedData();
        if (seed != nullptr) {
            resource->buffer.Upload(seed, size, 0);
            if (MG_Util::PipeStats::Enabled()) {
                // The one-time seed of a persistent map. Everything the app writes AFTER
                // this goes straight through the mapping and is persistent-map-push
                // territory (unwired, D4/D-B4), not this class.
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                             static_cast<Uint64>(size));
            }
        }
        resource->persistentMapped = true;
        resource->pendingFullUpload = false;
        resource->storageSize = size;
        resource->lastUseSerial = 0;
        return resource->buffer.GetMappedData();
    }

    Bool VkBufferManager::AcquireResidentSlice(BufferKind kind,
                                               const SharedPtr<MG_State::GLState::BufferObject>& bufferObject,
                                               BufferSlice& outSlice) {
        const VkBufferUsageFlags requiredUsage = GetVkBufferUsage(kind);
        MOBILEGL_ASSERT(requiredUsage != 0, "VkBufferManager::AcquireResidentSlice unsupported buffer kind");
        MOBILEGL_ASSERT(bufferObject != nullptr, "VkBufferManager::AcquireResidentSlice requires valid buffer object");

        auto resource = GetOrCreateResource(bufferObject);
        bufferObject->SyncPersistentMappedRange();

        const VkDeviceSize size = static_cast<VkDeviceSize>(bufferObject->GetSize());
        if (size == 0) {
            MGLOG_E_ONCE("VkBufferManager::AcquireResidentSlice failed: buffer size is zero");
            return false;
        }

        // Zero-copy persistent buffers already hold the app's live coherent writes in
        // host-visible storage carrying every usage; bind directly, no re-upload/staging.
        if (resource->persistentMapped && resource->buffer.IsValid() && resource->storageSize == size) {
            resource->lastUseSerial = m_frameSerial;
            outSlice = resource->buffer.GetSlice(0, size);
            return outSlice.IsValid();
        }

        const Bool needsRecreate = !resource->buffer.IsValid() || resource->storageSize != size ||
                                   ((resource->usageFlags & requiredUsage) != requiredUsage) ||
                                   resource->pendingFullUpload;
        if (needsRecreate) {
            const VkBufferUsageFlags usage = resource->usageFlags | requiredUsage;
            DeferRelease(std::move(resource->buffer));
            if (!CreateResidentStorage(*resource, size, usage)) {
                return false;
            }
            if (!resource->buffer.Upload(bufferObject->MappedData(), size, 0)) {
                MGLOG_E_ONCE("VkBufferManager::AcquireResidentSlice failed: initial upload failed");
                resource->buffer.Destroy();
                resource->storageSize = 0;
                resource->usageFlags = 0;
                return false;
            }
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                             static_cast<Uint64>(size));
            }
            resource->pendingFullUpload = false;
        }

        resource->lastUseSerial = m_frameSerial;
        outSlice = resource->buffer.GetSlice(0, size);
        return true;
    }

    Bool VkBufferManager::AcquireStreamedSlice(BufferKind kind,
                                               const SharedPtr<MG_State::GLState::BufferObject>& bufferObject,
                                               BufferSlice& outSlice) {
        (void)kind;
        MOBILEGL_ASSERT(bufferObject != nullptr, "VkBufferManager::AcquireStreamedSlice requires valid buffer object");

        auto resource = GetOrCreateResource(bufferObject);
        bufferObject->SyncPersistentMappedRange();

        // A persistently mapped resource's storage IS the application's copy of the bytes -
        // the frontend adopted it in place of the shadow and hands out pointers into it, and
        // a shader can have written bytes the shadow never saw (a transform feedback
        // capture). Streaming a second copy would feed this draw the stale shadow, and the
        // downgrade below would release the storage the application still points at,
        // breaking the "never recreated" promise AcquirePersistentMap makes.
        if (resource->persistentMapped) {
            return AcquireResidentSlice(kind, bufferObject, outSlice);
        }

        const VkDeviceSize size = static_cast<VkDeviceSize>(bufferObject->GetSize());
        if (size == 0) {
            MGLOG_E_ONCE("VkBufferManager::AcquireStreamedSlice failed: buffer size is zero");
            return false;
        }

        const Uint64 changeSerial = bufferObject->GetChangeSerial();
        if (resource->transientFrameSerial == m_frameSerial && resource->transientChangeSerial == changeSerial &&
            resource->transientSize == size && resource->transientSlice.IsValid()) {
            outSlice = resource->transientSlice;
            return true;
        }

        // Idle-content promotion: see the field comments in VkBufferResource. The
        // streak counts frame BOUNDARIES survived unchanged (the same-frame memo
        // above swallows repeat draws), so a promotion needs the content stable
        // for kStreamedPromotionStreak whole frames - one no-op frame does not
        // trigger the resident round-trip, whose creation upload is itself a
        // staged copy worth avoiding for content that is about to change again.
        constexpr Uint32 kStreamedPromotionStreak = 2;
        if (resource->promotedResident) {
            if (resource->promotedChangeSerial == changeSerial &&
                static_cast<VkDeviceSize>(bufferObject->GetSize()) == size) {
                return AcquireResidentSlice(kind, bufferObject, outSlice);
            }
            resource->promotedResident = false;
            resource->unchangedStreak = 0;
        } else if (resource->transientChangeSerial == changeSerial && resource->transientSize == size &&
                   resource->transientFrameSerial != 0) {
            if (++resource->unchangedStreak >= kStreamedPromotionStreak) {
                // Promotion moves the buffer off the arena and onto resident storage.
                resource->promotedResident = true;
                resource->promotedChangeSerial = changeSerial;
                BumpSliceEpoch(*resource);
                if (AcquireResidentSlice(kind, bufferObject, outSlice)) {
                    return true;
                }
                resource->promotedResident = false; // resident creation failed: stream as before
            }
        } else {
            resource->unchangedStreak = 0;
        }

        // A fresh arena allocation: a different slice than the last call handed back,
        // and (below) the point where a promoted buffer's resident storage is released.
        // The stable-promotion exit above returns before this, so a buffer the app has
        // stopped touching keeps one slice for as long as it keeps its resident storage.
        BumpSliceEpoch(*resource);
        if (!m_transientUploadArena.Upload(m_currentFrameIndex, bufferObject->MappedData(), size, 16,
                                           outSlice)) {
            return false;
        }
        if (MG_Util::PipeStats::Enabled()) {
            MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer, static_cast<Uint64>(size));
        }
        resource->transientSlice = outSlice;
        resource->transientFrameSerial = m_frameSerial;
        resource->transientChangeSerial = changeSerial;
        resource->transientSize = size;

        // Streaming path is authoritative now; release resident storage so we do
        // not keep a second, stale copy alive (downgrade).
        if (resource->buffer.IsValid()) {
            DeferRelease(std::move(resource->buffer));
            resource->storageSize = 0;
        }
        return true;
    }

    void VkBufferManager::DeferRelease(VkBufferObject&& buffer) {
        if (!buffer.IsValid()) {
            return;
        }

        if (m_deferredBufferReleases.empty()) {
            buffer.Destroy();
            return;
        }

        MOBILEGL_ASSERT(m_currentFrameIndex < m_deferredBufferReleases.size(),
                        "VkBufferManager::DeferRelease current frame index out of range");
        m_deferredBufferReleases[m_currentFrameIndex].push_back(std::move(buffer));
    }

    void VkBufferManager::CollectDeferredReleases(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_deferredBufferReleases.size(),
                        "VkBufferManager::CollectDeferredReleases frame index out of range");
        m_deferredBufferReleases[frameIndex].clear();
        m_deferredResourceReleases[frameIndex].clear();
#if MOBILEGL_BUILD_DISAGGREGATED
        // The wire list is not per-slot and does not wait for a frame boundary; a boundary is
        // just one more point to sweep at (BeginFrame after its slot fence, and every slot of
        // the idle drain, where the renderer-idle rule empties the list).
        SweepDeferredWireReleases();
#endif
    }

    BufferSlice VkBufferManager::AcquireUnboundStorageDescriptor() {
        if (!m_unboundStorageBuffer.IsValid()) {
            if (m_initInfo.allocator == nullptr) {
                return {};
            }
            // Host-visible so the zero fill needs no command buffer: this can be reached from
            // descriptor resolution, which runs inside an already-open recording and must not
            // start a copy of its own. The size is a whole minStorageBufferOffsetAlignment-safe
            // block rather than 4 bytes so that a shader which does read the block gets a
            // plausible unsized-array length instead of one that rounds to zero.
            const Bool created = m_unboundStorageBuffer.Create({
                .allocator = m_initInfo.allocator,
                .size = kUnboundStorageDescriptorBytes,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                   VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            });
            if (!created) {
                MGLOG_E_ONCE("VkBufferManager::AcquireUnboundStorageDescriptor: placeholder creation failed");
                m_unboundStorageBuffer.Destroy();
                return {};
            }
            if (void* mapped = m_unboundStorageBuffer.GetMappedData()) {
                Memset(mapped, 0, static_cast<SizeT>(kUnboundStorageDescriptorBytes));
            }
        }
        return m_unboundStorageBuffer.GetSlice();
    }

    BufferSlice VkBufferManager::AcquireUnboundTexelBufferDescriptor() {
        if (!m_unboundTexelBuffer.IsValid()) {
            if (m_initInfo.allocator == nullptr) {
                return {};
            }
            // A SECOND placeholder rather than more usage bits on the storage-block one. The two
            // are independent failure domains: a device that refuses this allocation must not
            // take the storage-block placeholder - and with it the fix this one is a sibling of -
            // down with it. Host-visible and zero-filled for the same reason as that one: this is
            // reached from descriptor resolution, inside an already-open recording, which must
            // not start a copy of its own.
            const Bool created = m_unboundTexelBuffer.Create({
                .allocator = m_initInfo.allocator,
                .size = kUnboundTexelBufferDescriptorBytes,
                .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                   VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            });
            if (!created) {
                MGLOG_E_ONCE("VkBufferManager::AcquireUnboundTexelBufferDescriptor: placeholder creation failed");
                m_unboundTexelBuffer.Destroy();
                return {};
            }
            if (void* mapped = m_unboundTexelBuffer.GetMappedData()) {
                Memset(mapped, 0, static_cast<SizeT>(kUnboundTexelBufferDescriptorBytes));
            }
        }
        return m_unboundTexelBuffer.GetSlice();
    }

    VkBufferUsageFlags VkBufferManager::GetVkBufferUsage(BufferKind kind) {
        switch (kind) {
        case BufferKind::Vertex:
        case BufferKind::Index:
            // A GL buffer can be rebound between ARRAY_BUFFER and ELEMENT_ARRAY_BUFFER,
            // and may even be used as both within the same draw setup. Keep resident
            // vertex/index buffers compatible with both roles from the start so we
            // never need to recreate a buffer after it has already been bound.
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        case BufferKind::Uniform:
            return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        case BufferKind::TextureBuffer:
            // Both texel roles, for the same reason vertex/index carry both bits: one GL buffer
            // texture can be read as a samplerBuffer and written as an imageBuffer, and which of
            // the two it is only becomes known when a shader that uses it is bound - long after
            // the resident buffer was created. A VkBufferView for a storage-texel descriptor is
            // invalid unless the buffer was created with the storage bit, so a buffer that
            // acquired only the uniform bit could never be given one.
            return VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
        case BufferKind::ShaderStorage:
            return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        case BufferKind::Indirect:
            return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        default:
            return 0;
        }
    }

    void VkBufferManager::DestroyAllDeferredReleases() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // Both callers (Shutdown, RecreateTransientArenas) have proven the device idle.
        DestroyAllDeferredWireReleases();
#endif
        for (auto& releases : m_deferredBufferReleases) {
            for (auto& buffer : releases) {
                buffer.Destroy();
            }
            releases.clear();
        }
        m_deferredBufferReleases.clear();
        for (auto& releases : m_deferredResourceReleases) {
            for (auto& resource : releases) {
                resource->buffer.Destroy();
            }
            releases.clear();
        }
        m_deferredResourceReleases.clear();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
