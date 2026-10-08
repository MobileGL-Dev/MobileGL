// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderPassGuard.h"
#include "VkBufferManager.h"
#include "../DirectVulkan.h"
#include "VulkanRenderer.h"

#include "MG_Util/Metrics/PipeStats.h"
#include "MG_Pipe/MGPipeCallbacks.h"
#include "MG_Pipe/PipeApply.h"
#include <MG_Backend/Record/StagedShadow.h>
#if MOBILEGL_BUILD_DISAGGREGATED
#include "MG_Remote/Transport/AdoptT0.h"
#endif
#if MOBILEGL_BUILD_DISAGGREGATED && defined(__ANDROID__)
#include <android/hardware_buffer.h>
#endif
#include <Config.h>
#include <chrono>
#include <cstdlib>

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
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

        using MG_State::GLState::BufferObject;

        // The manager owned by the active VulkanRenderer; immediate ops route here.
        SessionLocal<VkBufferManager*> g_activeBufferManager;  // per Magma session (MagmaSession.h)

        // The session's manager, or null once its renderer is gone - a client's records can still
        // arrive after its eglTerminate released it (or, surfaceless, before its first surface built
        // one). Such a record is declined with one line rather than taking the whole server down.
        VkBufferManager* WireManager(const char* op) {
            VkBufferManager* manager = g_activeBufferManager.Get();
            if (manager == nullptr) {
                MGLOG_E_ONCE("Magma: a wire buffer %s reached a session with no renderer; declined", op);
            }
            return manager;
        }

        const MG_Pipe::MGPipeResourceOps g_vulkanWireResourceOps = {
            .Create = [](auto res, const auto& desc) { if (auto* m = WireManager("create")) m->CreateWireBuffer(res, desc); },
            .Respecify = [](auto res, const auto& desc, const void* bytes) {
                if (auto* m = WireManager("respecify")) m->RespecifyWireBuffer(res, desc, bytes);
            },
            .SubData = [](auto res, const auto& record, const void* bytes) {
                if (auto* m = WireManager("write"))
                    m->WriteWireBuffer(res, MG_Pipe::MGPipeSubDataBufferOffset(record),
                                       MG_Pipe::MGPipeSubDataBufferSize(record), bytes);
            },
            .SubDataResident = [](auto res, const auto& record, const void* bytes) {
                if (auto* m = WireManager("write"))
                    m->WriteWireBuffer(res, MG_Pipe::MGPipeSubDataBufferOffset(record),
                                       MG_Pipe::MGPipeSubDataBufferSize(record), bytes);
            },
            .FlushRange = [](auto res, const auto& record, const void* bytes) {
                if (auto* m = WireManager("flush")) m->FlushWireBuffer(res, record.Offset, record.Size, bytes);
            },
            .Readback = [](auto res, const auto& record) {
                if (auto* m = WireManager("readback")) m->ReadbackWireBuffer(res, record.Offset, record.Size);
            },
            .Destroy = [](auto res) { if (auto* m = WireManager("destroy")) m->DestroyWireBuffer(res); },
            // Split mapping remains T2: the client owns its map and pushes exact
            // modified ranges. A server pointer is never donated across the wire. Monolith's record
            // arm (P13 W4) is one process: there the store IS handed over, as the legacy arm's
            // AcquirePersistentMap always did.
            .MapPersistent = [](MG_Pipe::MGPipeHandle res, Uint64 size, const void* seed) -> void* {
                if (!MG_Config::RecordArmAliasesFrontend()) return nullptr;
                auto* m = WireManager("map_persistent");
                return m != nullptr ? m->DonateWireBuffer(res, size, seed) : nullptr;
            },
            .UnmapPersistent = [](MG_Pipe::MGPipeHandle) {},
#if defined(__ANDROID__)
            // P11 B2 (T0): the one door a server pointer is still never donated through stays shut;
            // what T0 adds is the other direction - the CLIENT's pages imported as the store.
            .ImportExternal = [](MG_Pipe::MGPipeHandle res, void* ahb, Uint64 size) -> Bool {
                auto* m = WireManager("import");
                return m != nullptr && m->ImportWireBuffer(res, ahb, size);
            },
            .SelfTestExternal = [](char* why, Uint64 whyBytes) -> Bool {
                return VkBufferManager::SelfTestWireImport(why, whyBytes);
            },
#endif
        };
    } // namespace

    void VkBufferManager::RegisterWireResourceOps() {
        MG_Pipe::MGPipeSetResourceOps(&g_vulkanWireResourceOps);
    }

    namespace {
        // Diagnostic memory for DescribeWireBufferHistory: the last handles created and destroyed.
        struct WireBufferHistory {
            static constexpr SizeT kDepth = 256;
            MG_Pipe::MGPipeHandle created[kDepth]{};
            MG_Pipe::MGPipeHandle destroyed[kDepth]{};
            SizeT createdCount = 0;
            SizeT destroyedCount = 0;
        };
        SessionLocal<WireBufferHistory> g_wireBufferHistory;
    } // namespace

    VkBufferManager::WireBufferResource* VkBufferManager::FindWireBuffer(MG_Pipe::MGPipeHandle res) {
        const SizeT slot = res.Slot;
        if (slot < m_wireBufferLookup.size()) {
            const auto& lookup = m_wireBufferLookup[slot];
            if (lookup.resource != nullptr && lookup.gen == res.Gen) return lookup.resource;
        }
        const auto found = m_wireBuffers.find(WireBufferKey(res));
        if (found == m_wireBuffers.end()) return nullptr;
        if (slot < kWireBufferLookupMaxSlots) {
            if (slot >= m_wireBufferLookup.size()) m_wireBufferLookup.resize(slot + 1);
            m_wireBufferLookup[slot] = {res.Gen, &found->second};
        }
        return &found->second;
    }

    void VkBufferManager::CreateWireBuffer(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc) {
        if (FindWireBuffer(res) != nullptr) {
            MGLOG_F("Magma: Fatal{ProtocolCorruption, \"duplicate-buffer-create\"} {slot=%u, gen=%u}",
                    res.Slot, res.Gen);
            std::abort();
        }
        m_wireBuffers.try_emplace(WireBufferKey(res));
        {
            auto& history = *g_wireBufferHistory;
            history.created[history.createdCount++ % 256] = res;
        }
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
        resource->contentSerial = ++m_wireContentSerial;
        // P11 B2: a T0 store is ALWAYS released through the serial-gated list (never "at once"),
        // so the client's AHardwareBuffer outlives every submission that can still name it however
        // the client's own release (at this respecify) raced the frames in flight.
        DeferWireRelease(std::move(resource->buffer),
                         resource->imported ? T0ReleaseSerial(orphanedUseSerial) : orphanedUseSerial);
        resource->imported = false;
        resource->size = desc.Width;
        resource->lastUseSerial = 0;
        resource->lastUseSubmitIndex = 0;  // M2 r2: the new store carries no submission yet (B3's stamp is per store)
        resource->gpuWritesPending = false;
        resource->indirectReadBarrierPending = false;
        resource->stagedCoverage.clear();
        DropWireShadow(*resource);
        ReleaseRenameSpares(*resource);
        if (resource->size == 0) return;

        if (!CreateWireStoreBuffer(*resource)) {
            MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-storage\"} {slot=%u, gen=%u, size=%llu}",
                    res.Slot, res.Gen, static_cast<unsigned long long>(resource->size));
            std::abort();
        }
        ++m_wireStoreCount;
        NoteWireStorePeaks();
        PublishWireReclaimGauges();
        // P15 S0 (M4): a small store starts a host shadow. Its bytes are undefined until written
        // (glBufferData without data), so zeros are as good a starting copy as the new memory's.
        constexpr SizeT kWireShadowMaxStore = 64u << 10;
        constexpr SizeT kWireShadowBudget = 64u << 20;
        if (MG_Config::Features.MagmaWriteRename != MG_Config::QuirkOverride::ForceOff &&
            resource->size <= kWireShadowMaxStore && m_wireShadowBytes + resource->size <= kWireShadowBudget) {
            resource->shadow.assign(static_cast<SizeT>(resource->size), 0);
            m_wireShadowBytes += resource->shadow.size();
        }
        // Under transport, defined bytes follow as resource_subdata records. An
        // undefined store has no shadow to upload and no implicit zero snapshot.
        if (initialBytes != nullptr && desc.HasDefinedContent) {
            WriteWireBuffer(res, 0, resource->size, initialBytes);
        }
    }

    Bool VkBufferManager::CreateWireStoreBuffer(WireBufferResource& resource) {
        VkBufferUsageFlags usage = kPersistentBackedUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (m_initInfo.transformFeedbackUsageEnabled) usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
        return resource.buffer.Create({
                   .allocator = m_initInfo.allocator,
                   .size = resource.size,
                   .usage = usage,
                   .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                   .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                   .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               }) &&
               resource.buffer.Map() != nullptr;
    }

    void VkBufferManager::ReleaseRenameSpares(WireBufferResource& resource) {
        for (auto& spare : resource.renameSpares)
            DeferWireRelease(std::move(spare.buffer), spare.lastUseSerial != 0 ? spare.lastUseSerial : m_frameSerial);
        resource.renameSpares.clear();
    }

    void VkBufferManager::DropWireShadow(WireBufferResource& resource) {
        if (resource.shadow.empty()) return;
        m_wireShadowBytes -= resource.shadow.size();
        resource.shadow.clear();
        resource.shadow.shrink_to_fit();
    }

    // P15 S0 (M4): WRITE RENAMING. A busy store (the GPU may still read it: an earlier draw of
    // this frame, or one in flight) gets a fresh VkBuffer holding the shadow - which already has
    // this write in it - and the old one is released once everything that can name it completes,
    // exactly as a glBufferData orphan is. Draws recorded before keep the old buffer and its old
    // bytes; draws after bind the new one (every bind site compares VkBuffer handles, and the
    // release goes through the destroy epoch the handle-keyed memos fold in). No copy is recorded,
    // so the render pass stays open and no barrier is needed.
    Bool VkBufferManager::RenameBusyWireStore(WireBufferResource& resource) {
        // A shadow outlives only GPU writes whose bytes it has (this manager's own staged copies):
        // every other GPU write drops it. So a pending GPU write does not bar the rename.
        if (resource.shadow.size() != resource.size || resource.imported ||
            MG_Config::Features.MagmaWriteRename == MG_Config::QuirkOverride::ForceOff)
            return false;
        VkBufferObject old = std::move(resource.buffer);
        Bool reused = false;
        for (SizeT i = 0; pVulkanRenderer != nullptr && i < resource.renameSpares.size(); ++i) {
            if (!pVulkanRenderer->IsSubmitIndexComplete(resource.renameSpares[i].submitIndex)) continue;
            resource.buffer = std::move(resource.renameSpares[i].buffer);
            resource.renameSpares.erase(resource.renameSpares.begin() + static_cast<std::ptrdiff_t>(i));
            reused = true;
            break;
        }
        if (!reused) {
            if (!CreateWireStoreBuffer(resource)) {
                resource.buffer.Destroy();
                resource.buffer = std::move(old);
                return false;
            }
            ++m_wireStoreCount;
        }
        if (!resource.buffer.Upload(resource.shadow.data(), static_cast<VkDeviceSize>(resource.size), 0)) {
            // Keep the store on its old buffer; the new one goes the deferred way (it is idle).
            DeferWireRelease(std::move(resource.buffer), 0);
            resource.buffer = std::move(old);
            return false;
        }
        // Busy means a GPU command recorded so far may name the old buffer: it waits behind this
        // frame's serial and the current sync point, whatever its own stamps said - as a spare
        // for the next rename, or (past the bound) through DeferWireRelease.
        constexpr SizeT kRenameSpares = 4;
        if (resource.renameSpares.size() >= kRenameSpares) {
            auto& oldest = resource.renameSpares.front();
            DeferWireRelease(std::move(oldest.buffer), oldest.lastUseSerial);
            resource.renameSpares.erase(resource.renameSpares.begin());
        }
        WireBufferResource::RenameSpare spare;
        spare.buffer = std::move(old);
        spare.submitIndex = pVulkanRenderer != nullptr ? pVulkanRenderer->GetSyncPointSubmitIndex() : 0;
        spare.lastUseSerial = m_frameSerial;
        resource.renameSpares.push_back(std::move(spare));
        resource.lastUseSerial = 0;
        resource.lastUseSubmitIndex = 0;
        resource.gpuWritesPending = false;
        ++m_wireRenames;
        NoteWireStorePeaks();
        PublishWireReclaimGauges();
        return true;
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
        EndActiveRenderPassOn(commands);
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        // P8-D: counted here, past the idle early-out, so `whw` is waits TAKEN, not calls.
        ++m_wireHostWaits;
        const Bool timed = MG_Util::PipeStats::Enabled();
        const auto waitStart = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const Bool waited =
            pVulkanRenderer->WaitForSubmitIndex(pVulkanRenderer->GetSyncPointSubmitIndex(), UINT64_MAX, true);
        if (timed) {
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::WireHostWaits, 1);
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::WireHostWaitMicros,
                static_cast<Uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - waitStart).count()));
        }
        if (!waited) {
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
        resource->contentSerial = ++m_wireContentSerial;
        if (!resource->shadow.empty())
            Memcpy(resource->shadow.data() + offset, bytes, static_cast<SizeT>(size));
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
        if ((busyBySerial || busyBySubmission) && !resource->shadow.empty() && RenameBusyWireStore(*resource)) {
            uploaded = true; // the new buffer was filled from the shadow, this write included
        } else if (busyBySerial || busyBySubmission) {
            // Vulkan buffer copies require four-byte aligned ranges. An odd GL
            // byte update waits, then writes only the exact range; rounding it
            // from a CPU shadow could overwrite neighbouring GPU-written bytes.
            if (((offset | size) & 3u) == 0) {
                uploaded = StagedWireRangeCopy(*resource, bytes, static_cast<SizeT>(offset),
                                                static_cast<SizeT>(size));
            }
            if (!uploaded && !WaitForWireBufferHostAccess(*resource)) {
                if (LatchedOnDeviceLoss("buffer-host-write")) return;
                WireBufferSyncFatal("host-write");
            }
        }
        if (!uploaded) {
            uploaded = resource->buffer.Upload(bytes, size, offset);
            if (uploaded && MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer, size);
            }
        }
        if (!uploaded) {
            if (LatchedOnDeviceLoss("buffer-upload")) return;
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
        MG_Record::StagedShadowStore::CoverageAdd(resource->stagedCoverage,
                                                         static_cast<SizeT>(offset),
                                                         static_cast<SizeT>(offset + size));
    }

    void VkBufferManager::FlushWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size,
                                           const void* bytes) {
        if (bytes != nullptr) {
            WriteWireBuffer(res, offset, size, bytes);
            return;
        }
        const auto* resource = FindWireBuffer(res);
        if (!resource || offset > resource->size || size > resource->size - offset ||
            !MG_Record::StagedShadowStore::CoverageHas(resource->stagedCoverage,
                static_cast<SizeT>(offset), static_cast<SizeT>(offset + size))) {
            MGLOG_F("Magma: Fatal{StageSnapshotTooNarrow, \"buffer-flush\"} {slot=%u, gen=%u}",
                    res.Slot, res.Gen);
            std::abort();
        }
        // SubData has already copied the owned record bytes into GPU storage,
        // ordered after earlier uses. There is no stale shadow to replay here.
    }

    Bool VkBufferManager::LookupConvertedStream(const ConvertedStreamKey& key, SizeT elementCount,
                                                BufferSlice& outSlice) {
        for (auto& entry : m_convertedStreams) {
            if (!(entry.key == key) || entry.elementCount < elementCount) continue;
            entry.lastUseFrameSerial = m_frameSerial;
            outSlice = entry.buffer.GetSlice();
            outSlice.mapped = nullptr;
            return true;
        }
        return false;
    }

    void VkBufferManager::RetireConvertedStream(SizeT index) {
        auto& entry = m_convertedStreams[index];
        m_convertedStreamBytes -= static_cast<SizeT>(entry.buffer.GetSize());
        // Its last use may be this frame: the slot's next BeginFrame is after this frame completed.
        if (m_currentFrameIndex < m_deferredBufferReleases.size())
            m_deferredBufferReleases[m_currentFrameIndex].push_back(std::move(entry.buffer));
        m_convertedStreams.erase(m_convertedStreams.begin() + static_cast<std::ptrdiff_t>(index));
    }

    Bool VkBufferManager::StoreConvertedStream(const ConvertedStreamKey& key, MG_Pipe::MGPipeHandle source,
                                               SizeT elementCount, const void* bytes, SizeT size,
                                               BufferSlice& outSlice) {
        constexpr SizeT kConvertedStreamBudget = 192ull << 20;
        if (size == 0 || size > kConvertedStreamBudget / 2 || m_initInfo.allocator == nullptr ||
            m_deferredBufferReleases.empty())
            return false;
        // A newer conversion of the same store and layout supersedes the old one (its content
        // serial can never come back); an entry unused for many frames (a deleted store's, a
        // layout no longer drawn) goes too.
        constexpr Uint64 kConvertedStreamIdleFrames = 256;
        for (SizeT i = m_convertedStreams.size(); i-- > 0;) {
            const auto& entry = m_convertedStreams[i];
            if (entry.lastUseFrameSerial + kConvertedStreamIdleFrames < m_frameSerial) {
                RetireConvertedStream(i);
                continue;
            }
            if (entry.source == source && entry.key.baseOffset == key.baseOffset && entry.key.stride == key.stride &&
                entry.key.type == key.type && entry.key.size == key.size && entry.key.conversion == key.conversion)
                RetireConvertedStream(i);
        }
        while (!m_convertedStreams.empty() && m_convertedStreamBytes + size > kConvertedStreamBudget) {
            SizeT oldest = 0;
            for (SizeT i = 1; i < m_convertedStreams.size(); ++i)
                if (m_convertedStreams[i].lastUseFrameSerial < m_convertedStreams[oldest].lastUseFrameSerial) oldest = i;
            RetireConvertedStream(oldest);
        }
        ConvertedStream entry;
        entry.key = key;
        entry.source = source;
        entry.elementCount = elementCount;
        entry.lastUseFrameSerial = m_frameSerial;
        if (!entry.buffer.Create({
                .allocator = m_initInfo.allocator,
                .size = static_cast<VkDeviceSize>(size),
                .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            }) || entry.buffer.Map() == nullptr || !entry.buffer.Upload(bytes, static_cast<VkDeviceSize>(size), 0))
            return false;
        m_convertedStreamBytes += size;
        outSlice = entry.buffer.GetSlice();
        outSlice.mapped = nullptr;
        m_convertedStreams.push_back(std::move(entry));
        return true;
    }

    Bool VkBufferManager::GetWireBufferContentSerial(MG_Pipe::MGPipeHandle res, Uint64& serial) {
        auto* resource = FindWireBuffer(res);
        if (!resource || resource->imported || !resource->buffer.IsValid()) return false;
        serial = resource->contentSerial;
        return true;
    }

    Bool VkBufferManager::AcquireWireSlice(BufferKind kind, MG_Pipe::MGPipeHandle res, BufferSlice& outSlice) {
        // The store carries every buffer usage, so changing roles never orphans it.
        outSlice = {};
        auto* resource = FindWireBuffer(res);
        if (!resource || !resource->buffer.IsValid() || resource->size == 0) return false;
        resource->lastUseSerial = m_frameSerial;
        // A slice a shader may write through: whatever it writes is a new content state.
        if (kind == BufferKind::ShaderStorage || kind == BufferKind::TextureBuffer) {
            resource->contentSerial = ++m_wireContentSerial;
            DropWireShadow(*resource);
        }
        // B3: the draw this slice is being acquired for is normally recorded into the NEXT
        // submission - normally, not always: a mid-draw flush (SyncWireTextureShape's preserve
        // path) can submit this index without the draw. This stamp is the defence term's input
        // (see WriteWireBuffer); the floor behind lastUseSerial is the guarantee. Read off the
        // owning renderer's counter (its GetWireNextSubmitIndex()) when it was handed one.
        if (m_submitCounter != nullptr) resource->lastUseSubmitIndex = *m_submitCounter + 1;
        else if (pVulkanRenderer) resource->lastUseSubmitIndex = pVulkanRenderer->GetWireNextSubmitIndex();
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
        // A shadowed store's host copy is current, staged copies included (see RenameBusyWireStore).
        if (resource->shadow.size() == resource->size) {
            Memcpy(dst, resource->shadow.data() + offset, static_cast<SizeT>(size));
            return true;
        }
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
        EndActiveRenderPassOn(commands);
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        const VkBufferCopy copy{offset, dst.offset, size};
        EndActiveRenderPassOn(commands);
        vkCmdCopyBuffer(commands, resource->buffer.GetHandle(), dst.buffer, 1, &copy);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        EndActiveRenderPassOn(commands);
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
        EndActiveRenderPassOn(commands);
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        const VkBufferCopy widen{alignedOffset, staging.offset, stagedSize};
        EndActiveRenderPassOn(commands);
        vkCmdCopyBuffer(commands, resource->buffer.GetHandle(), staging.buffer, 1, &widen);
        // The shift reads what the widening copy just wrote, so it needs its own edge even
        // though both halves are transfers on one command buffer.
        VkMemoryBarrier staged{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        staged.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        staged.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        EndActiveRenderPassOn(commands);
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &staged, 0, nullptr, 0, nullptr);
        const VkBufferCopy shift{staging.offset + headPad, dst.offset + dstSkip, size};
        EndActiveRenderPassOn(commands);
        vkCmdCopyBuffer(commands, staging.buffer, dst.buffer, 1, &shift);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        EndActiveRenderPassOn(commands);
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
        resource->lastUseSerial = m_frameSerial;
        return true;
    }

    void VkBufferManager::ReadbackWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size) {
        // P11 B2: A T0 STORE'S READBACK IS "DONE, NO BYTES". Its pages are the client's
        // AHardwareBuffer, so what the client needs before it reads them is this server's GPU
        // having finished every write to them - the resident copies WriteWireBuffer recorded and
        // any shader write - and nothing crossing SEG_EVENT. The wait is the store's own host-access
        // wait (a barrier to host, then the sync point's submission); the reply then says "done".
        // One death for both shapes: the store could not be made readable.
        auto* resource = FindWireBuffer(res);
        const Bool t0 = resource != nullptr && resource->imported;
        Vector<Uint8> bytes(t0 ? 0 : static_cast<SizeT>(size));
        if (t0 ? !WaitForWireBufferHostAccess(*resource) : !ReadWireBuffer(res, offset, size, bytes.data())) {
            // A lost device: the session is latched and closing; its client reads a lost context.
            if (LatchedOnDeviceLoss("buffer-readback")) return;
            MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-readback\"} {slot=%u, gen=%u}", res.Slot, res.Gen);
            std::abort();
        }
        if (t0) {
            return;
        }
        if (MG_Pipe::gMGPipeCallbacks.OnBufferWriteback == nullptr) {
            MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-writeback-callback\"}");
            std::abort();
        }
        // The reverse producer synchronously copies into SEG_EVENT before
        // this owned vector is released; no mapped Vulkan pointer crosses roles.
        MG_Pipe::gMGPipeCallbacks.OnBufferWriteback(res, offset,
            {reinterpret_cast<Uint64>(bytes.data()), size, MG_Pipe::kMGHostSpanSegNone, 0});
    }

    void VkBufferManager::MarkWireBufferGpuWritten(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size) {
        auto* resource = FindWireBuffer(res);
        if (!resource || offset > resource->size) return;
        if (size == MG_Pipe::kMGPipeWholeBuffer) size = resource->size - offset;
        if (size == 0 || size > resource->size - offset) return;
        resource->contentSerial = ++m_wireContentSerial;
        DropWireShadow(*resource);
        resource->lastUseSerial = m_frameSerial;
        resource->stagedCoverage.clear();
        resource->gpuWritesPending = true;
        resource->indirectReadBarrierPending = true;
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

    Bool VkBufferManager::TakeWireIndirectReadBarrier(MG_Pipe::MGPipeHandle res) {
        auto* resource = FindWireBuffer(res);
        if (!resource || !resource->indirectReadBarrierPending) return false;
        resource->indirectReadBarrierPending = false;
        return true;
    }

    // ---- P11 B2: T0 - THE CLIENT'S AHardwareBuffer AS A WIRE STORE -----------------------------

    // The serial a T0 store is released under: its last use, or - when no GPU command has named it
    // since a host-access wait - the current frame's, so the release still waits for the sync
    // point rather than taking DeferWireRelease's "never named, destroy now" arm. Conservative by
    // at most one submission.
    Uint64 VkBufferManager::T0ReleaseSerial(Uint64 lastUseSerial) const {
        if (lastUseSerial != 0) return lastUseSerial;
        return m_frameSerial != 0 ? m_frameSerial : 1;
    }

#if defined(__ANDROID__)
    namespace {
        Int32 T0PickMemoryType(const VkPhysicalDeviceMemoryProperties& props, Uint32 bits,
                               VkMemoryPropertyFlags want) {
            for (Uint32 i = 0; i < props.memoryTypeCount; ++i) {
                if ((bits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & want) == want)
                    return static_cast<Int32>(i);
            }
            return -1;
        }

        struct T0ImportedBuffer {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void* mapped = nullptr;
            Bool coherent = true;
        };

        void T0DestroyImported(VkDevice device, T0ImportedBuffer& imported) {
            if (imported.mapped != nullptr) vkUnmapMemory(device, imported.memory);
            if (imported.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, imported.buffer, nullptr);
            if (imported.memory != VK_NULL_HANDLE) vkFreeMemory(device, imported.memory, nullptr);
            imported = T0ImportedBuffer{};
        }

        // A VkBuffer on a dedicated allocation IMPORTED from `ahb`, in a host-visible memory type
        // (coherent preferred), mapped whole. The server's own host map is what WriteWireBuffer's
        // idle path and every CPU reader of a wire store use, so a type without HOST_VISIBLE is a
        // refusal, not a degraded import. Nothing is left behind on failure.
        Bool T0ImportAhb(const VulkanRenderer::WireAhbImport& ctx, void* ahb, Uint64 size, VkBufferUsageFlags usage,
                         T0ImportedBuffer& out, String& why) {
            out = T0ImportedBuffer{};
            const auto getProperties =
                reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(ctx.getAhbProperties);
            VkAndroidHardwareBufferPropertiesANDROID properties{
                VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
            VkResult result = getProperties(ctx.device, static_cast<const AHardwareBuffer*>(ahb), &properties);
            if (result != VK_SUCCESS) {
                why = "vkGetAndroidHardwareBufferPropertiesANDROID -> " + std::to_string(static_cast<int>(result));
                return false;
            }
            VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
            external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufferInfo.pNext = &external;
            bufferInfo.size = size;
            bufferInfo.usage = usage;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            result = vkCreateBuffer(ctx.device, &bufferInfo, nullptr, &out.buffer);
            if (result != VK_SUCCESS) {
                out.buffer = VK_NULL_HANDLE;
                why = "vkCreateBuffer(external AHB) -> " + std::to_string(static_cast<int>(result));
                return false;
            }
            Int32 type = T0PickMemoryType(ctx.memory, properties.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            out.coherent = type >= 0;
            if (type < 0)
                type = T0PickMemoryType(ctx.memory, properties.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            if (type < 0) {
                why = "no host-visible memory type can import it (memoryTypeBits 0x" +
                      std::to_string(properties.memoryTypeBits) + ")";
                T0DestroyImported(ctx.device, out);
                return false;
            }
            VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicated.buffer = out.buffer;
            VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
            import.buffer = static_cast<AHardwareBuffer*>(ahb);
            import.pNext = &dedicated;
            VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocateInfo.pNext = &import;
            allocateInfo.allocationSize = properties.allocationSize;
            allocateInfo.memoryTypeIndex = static_cast<Uint32>(type);
            result = vkAllocateMemory(ctx.device, &allocateInfo, nullptr, &out.memory);
            if (result != VK_SUCCESS) {
                out.memory = VK_NULL_HANDLE;
                why = "vkAllocateMemory(import AHB) -> " + std::to_string(static_cast<int>(result));
                T0DestroyImported(ctx.device, out);
                return false;
            }
            result = vkBindBufferMemory(ctx.device, out.buffer, out.memory, 0);
            if (result == VK_SUCCESS) result = vkMapMemory(ctx.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
            if (result != VK_SUCCESS || out.mapped == nullptr) {
                out.mapped = nullptr;
                why = "vkBindBufferMemory/vkMapMemory -> " + std::to_string(static_cast<int>(result));
                T0DestroyImported(ctx.device, out);
                return false;
            }
            return true;
        }
    } // namespace
#endif

    // P13 W4: THE IN-PROCESS DONATION. Every wire store is already host-visible, coherent and
    // persistently mapped (RespecifyWireBuffer), so in one process the client can write straight
    // into it, exactly as the legacy arm's persistent maps did. The store is then the client's
    // pages - T0's shape without an AHardwareBuffer - so it takes T0's flag: a readback is "done, no
    // bytes" (the client reads its own pointer once this side's GPU is through with it) and a
    // respecify retires the store behind the serial. It is seeded from the client's shadow, after
    // any GPU use of the store has drained, because the shadow is what the application sees.
    void* VkBufferManager::DonateWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 size, const void* seedBytes) {
        auto* resource = FindWireBuffer(res);
        if (resource == nullptr || size == 0 || size != resource->size) return nullptr;
        void* mapped = resource->buffer.GetMappedData();
        if (mapped == nullptr) return nullptr;
        if (resource->imported) return mapped; // already donated: the same pointer
        resource->contentSerial = ++m_wireContentSerial;
        DropWireShadow(*resource);
        if (!WaitForWireBufferHostAccess(*resource)) return nullptr;
        if (seedBytes != nullptr) std::memcpy(mapped, seedBytes, static_cast<SizeT>(size));
        resource->imported = true;
        resource->gpuWritesPending = false;
        resource->stagedCoverage.clear();
        MG_Record::StagedShadowStore::CoverageAdd(resource->stagedCoverage, 0, static_cast<SizeT>(size));
        return mapped;
    }

    Bool VkBufferManager::ImportWireBuffer(MG_Pipe::MGPipeHandle res, void* ahb, Uint64 size) {
#if defined(__ANDROID__) && MOBILEGL_BUILD_DISAGGREGATED // T0 is the transport's adoption tier
        auto* resource = FindWireBuffer(res);
        if (resource == nullptr || ahb == nullptr || size == 0 || size != resource->size) return false;
        VulkanRenderer::WireAhbImport ctx;
        if (pVulkanRenderer == nullptr || !pVulkanRenderer->GetWireAhbImport(ctx)) {
            MGLOG_W_ONCE("Magma T0: this device did not take VK_ANDROID_external_memory_android_hardware_buffer");
            return false;
        }
        VkBufferUsageFlags usage = kPersistentBackedUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (m_initInfo.transformFeedbackUsageEnabled) usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
        T0ImportedBuffer imported;
        String why;
        if (!T0ImportAhb(ctx, ahb, size, usage, imported, why)) {
            MGLOG_W("Magma T0: store {slot=%u, gen=%u} (%llu bytes) not imported - %s", res.Slot, res.Gen,
                    static_cast<unsigned long long>(size), why.c_str());
            return false;
        }
        // The T2 store it replaces goes the way a respecify's does (and any draw still naming it
        // keeps it until its submission completes).
        DeferWireRelease(std::move(resource->buffer), resource->lastUseSerial);
        MG_Remote::Transport::AdoptT0::AcquireImported(ahb);
        resource->buffer.AdoptExternal(ctx.device, imported.buffer, imported.memory, size, imported.mapped,
                                       imported.coherent, ahb);
        resource->imported = true;
        DropWireShadow(*resource);
        resource->lastUseSerial = 0;
        resource->lastUseSubmitIndex = 0;
        resource->gpuWritesPending = false;
        // The whole store holds bytes the client supplied (its AHB, seeded from its shadow).
        resource->stagedCoverage.clear();
        MG_Record::StagedShadowStore::CoverageAdd(resource->stagedCoverage, 0, static_cast<SizeT>(size));
        ++m_wireStoreCount;
        NoteWireStorePeaks();
        PublishWireReclaimGauges();
        MGLOG_D("Magma T0: imported {slot=%u, gen=%u} (%llu bytes, %s memory)", res.Slot, res.Gen,
                static_cast<unsigned long long>(size), imported.coherent ? "coherent" : "non-coherent");
        return true;
#else
        (void)res;
        (void)ahb;
        (void)size;
        return false;
#endif
    }

    // THE POST (B2; decided by probe, never by driver name). The AHB API promises CPU coherence only
    // across an unlock/lock, and T0 holds the client's lock for the store's life, so this exercises
    // that pattern on THIS device before kCapAdoptT0 may be published:
    //   hold - a 64 KiB BLOB allocated and locked by the test, never unlocked while it runs;
    //   read - this server's own host map of the import reads what the CPU wrote through the lock;
    //   gpu  - one submission copies that pattern out of the import (the GPU read) and fills another
    //          region of it (the GPU write), then a transfer->host barrier and a fence wait;
    //   back - the fill is read through the HELD pointer, the copy from a host-visible staging buffer;
    //   steady - the same again with bytes the CPU wrote AFTER the import and its first GPU use (the
    //          block at the end says why the first round alone is not enough).
    Bool VkBufferManager::SelfTestWireImport(char* whyOut, Uint64 whyBytes) {
        const auto answer = [&](Bool ok, const String& text) {
            std::snprintf(whyOut, static_cast<SizeT>(whyBytes), "%s", text.c_str());
            return ok;
        };
#if defined(__ANDROID__) && MOBILEGL_BUILD_DISAGGREGATED // T0 is the transport's adoption tier
        VulkanRenderer::WireAhbImport ctx;
        if (pVulkanRenderer == nullptr || !pVulkanRenderer->GetWireAhbImport(ctx))
            return answer(false, "the device did not take VK_ANDROID_external_memory_android_hardware_buffer");
        constexpr Uint64 kBytes = 64 * 1024;
        constexpr Uint32 kWords = 1024;          // one 4 KiB region
        constexpr Uint64 kFillAt = 4 * 4096;     // region 4
        constexpr Uint32 kFillWord = 0x6C0FFEE0u;
        Vector<Uint32> pattern(kBytes / 4);
        for (SizeT i = 0; i < pattern.size(); ++i) pattern[i] = 0x5A000000u ^ static_cast<Uint32>(i * 2654435761u);
        MG_Remote::Transport::AdoptT0::HeldStore held;
        String why;
        if (!MG_Remote::Transport::AdoptT0::AllocateHeld(kBytes, pattern.data(), held, why))
            return answer(false, "hold: " + why);
        T0ImportedBuffer imported, staging;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        String verdict;
        Bool ok = false;
        do {
            if (!T0ImportAhb(ctx, held.ahb, kBytes,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             imported, why)) {
                verdict = "import: " + why;
                break;
            }
            if (!imported.coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = imported.memory;
                range.size = VK_WHOLE_SIZE;
                vkInvalidateMappedMemoryRanges(ctx.device, 1, &range);
            }
            if (std::memcmp(imported.mapped, pattern.data(), kWords * 4) != 0) {
                verdict = "read: the server's host map of the import does not hold the CPU's pattern";
                break;
            }
            // A host-visible staging buffer for the GPU read's destination.
            VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            stagingInfo.size = kWords * 4;
            stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(ctx.device, &stagingInfo, nullptr, &staging.buffer) != VK_SUCCESS) {
                staging.buffer = VK_NULL_HANDLE;
                verdict = "staging buffer";
                break;
            }
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(ctx.device, staging.buffer, &requirements);
            const Int32 stagingType = T0PickMemoryType(ctx.memory, requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo stagingAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            stagingAlloc.allocationSize = requirements.size;
            stagingAlloc.memoryTypeIndex = static_cast<Uint32>(stagingType < 0 ? 0 : stagingType);
            if (stagingType < 0 || vkAllocateMemory(ctx.device, &stagingAlloc, nullptr, &staging.memory) != VK_SUCCESS ||
                vkBindBufferMemory(ctx.device, staging.buffer, staging.memory, 0) != VK_SUCCESS ||
                vkMapMemory(ctx.device, staging.memory, 0, VK_WHOLE_SIZE, 0, &staging.mapped) != VK_SUCCESS) {
                verdict = "staging memory";
                break;
            }
            std::memset(staging.mapped, 0, kWords * 4);
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.queueFamilyIndex = ctx.queueFamily;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commandInfo.commandBufferCount = 1;
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
                pool = VK_NULL_HANDLE;
                verdict = "command pool";
                break;
            }
            commandInfo.commandPool = pool;
            if (vkAllocateCommandBuffers(ctx.device, &commandInfo, &commands) != VK_SUCCESS ||
                vkCreateFence(ctx.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
                verdict = "command buffer / fence";
                break;
            }
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(commands, &begin);
            VkMemoryBarrier hostWrites{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            hostWrites.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            hostWrites.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            EndActiveRenderPassOn(commands);
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                 &hostWrites, 0, nullptr, 0, nullptr);
            VkBufferCopy region{0, 0, kWords * 4};
            EndActiveRenderPassOn(commands);
            vkCmdCopyBuffer(commands, imported.buffer, staging.buffer, 1, &region);
            EndActiveRenderPassOn(commands);
            vkCmdFillBuffer(commands, imported.buffer, kFillAt, kWords * 4, kFillWord);
            VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            EndActiveRenderPassOn(commands);
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost,
                                 0, nullptr, 0, nullptr);
            vkEndCommandBuffer(commands);
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commands;
            if (vkQueueSubmit(ctx.queue, 1, &submit, fence) != VK_SUCCESS ||
                vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) {
                verdict = "gpu: the submission did not complete within 2 s";
                break;
            }
            Uint32 gpuRead = 0, heldSaw = 0;
            const auto* copied = static_cast<const Uint32*>(staging.mapped);
            const auto* heldWords = static_cast<const Uint32*>(held.ptr);
            for (Uint32 i = 0; i < kWords; ++i) {
                if (copied[i] == pattern[i]) ++gpuRead;
                if (heldWords[kFillAt / 4 + i] == kFillWord) ++heldSaw;
            }
            ok = gpuRead == kWords && heldSaw == kWords;
            verdict = "64 KiB AHB, lock held throughout: host map read ok (" +
                      String(imported.coherent ? "coherent" : "non-coherent") + " memory), GPU read " +
                      std::to_string(gpuRead) + "/1024 words of the CPU's pattern, GPU write seen through the "
                      "held lock " + std::to_string(heldSaw) + "/1024 words";
            if (!ok) break;

            // STEADY STATE. The round above reads bytes the CPU wrote BEFORE the import, and an import
            // can clean the CPU's caches on its way in, so it passes on a device whose GPU never sees a
            // write made through the held lock afterwards - which is all of T0's real traffic (Mali-G1
            // on a Dimensity: the first round passed, then Flywheel's staging scatter read stale bytes
            // and the create-indirect trace lost every instanced object). So the CPU now writes a fresh
            // pattern into region 1 and reads region 4 back into its cache, and one more submission
            // copies region 1 out and overwrites region 4.
            constexpr Uint64 kSteadyAt = 1 * 4096; // region 1
            constexpr Uint32 kSteadyFillWord = 0x3D5EA11Eu;
            auto* heldWrite = static_cast<volatile Uint32*>(held.ptr);
            Uint32 cached = 0;
            for (Uint32 i = 0; i < kWords; ++i) {
                heldWrite[kSteadyAt / 4 + i] = 0xA5000000u ^ (i * 40503u);
                cached ^= heldWrite[kFillAt / 4 + i];
            }
            (void)cached;
            std::memset(staging.mapped, 0, kWords * 4);
            if (vkResetFences(ctx.device, 1, &fence) != VK_SUCCESS ||
                vkResetCommandPool(ctx.device, pool, 0) != VK_SUCCESS ||
                vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
                ok = false;
                verdict = "steady: the second submission could not be recorded";
                break;
            }
            EndActiveRenderPassOn(commands);
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                 &hostWrites, 0, nullptr, 0, nullptr);
            VkBufferCopy steadyRegion{kSteadyAt, 0, kWords * 4};
            EndActiveRenderPassOn(commands);
            vkCmdCopyBuffer(commands, imported.buffer, staging.buffer, 1, &steadyRegion);
            EndActiveRenderPassOn(commands);
            vkCmdFillBuffer(commands, imported.buffer, kFillAt, kWords * 4, kSteadyFillWord);
            EndActiveRenderPassOn(commands);
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost,
                                 0, nullptr, 0, nullptr);
            vkEndCommandBuffer(commands);
            if (vkQueueSubmit(ctx.queue, 1, &submit, fence) != VK_SUCCESS ||
                vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) {
                ok = false;
                verdict = "steady: the second submission did not complete within 2 s";
                break;
            }
            Uint32 steadyRead = 0, steadySaw = 0;
            for (Uint32 i = 0; i < kWords; ++i) {
                if (copied[i] == (0xA5000000u ^ (i * 40503u))) ++steadyRead;
                if (heldWords[kFillAt / 4 + i] == kSteadyFillWord) ++steadySaw;
            }
            ok = steadyRead == kWords && steadySaw == kWords;
            verdict += "; with the import in use, GPU read " + std::to_string(steadyRead) +
                       "/1024 words the CPU wrote after it, GPU write seen through the held lock " +
                       std::to_string(steadySaw) + "/1024 words";
        } while (false);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(ctx.device, fence, nullptr);
        if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx.device, pool, nullptr);
        T0DestroyImported(ctx.device, staging);
        T0DestroyImported(ctx.device, imported);
        MG_Remote::Transport::AdoptT0::ReleaseHeld(held);
        return answer(ok, verdict);
#else
        return answer(false, "no AHardwareBuffer on this platform");
#endif
    }


    void VkBufferManager::DescribeWireBufferHistory(MG_Pipe::MGPipeHandle res) const {
        const auto& history = *g_wireBufferHistory;
        Int createdAgo = -1, destroyedAgo = -1;
        for (SizeT i = 0; i < std::min(history.createdCount, WireBufferHistory::kDepth); ++i) {
            const auto& h = history.created[(history.createdCount - 1 - i) % WireBufferHistory::kDepth];
            if (h.Slot == res.Slot && h.Gen == res.Gen) { createdAgo = static_cast<Int>(i); break; }
        }
        for (SizeT i = 0; i < std::min(history.destroyedCount, WireBufferHistory::kDepth); ++i) {
            const auto& h = history.destroyed[(history.destroyedCount - 1 - i) % WireBufferHistory::kDepth];
            if (h.Slot == res.Slot && h.Gen == res.Gen) { destroyedAgo = static_cast<Int>(i); break; }
        }
        MGLOG_E("Magma wire buffer {slot=%u, gen=%u}: live stores %zu; created %d creates ago, destroyed %d "
                "destroys ago (-1 = not in the last %zu); %zu creates, %zu destroys in this session",
                res.Slot, res.Gen, m_wireBuffers.size(), createdAgo, destroyedAgo, WireBufferHistory::kDepth,
                history.createdCount, history.destroyedCount);
    }

    void VkBufferManager::DestroyWireBuffer(MG_Pipe::MGPipeHandle res) {
        {
            auto& history = *g_wireBufferHistory;
            history.destroyed[history.destroyedCount++ % WireBufferHistory::kDepth] = res;
        }
        const auto found = m_wireBuffers.find(WireBufferKey(res));
        if (found == m_wireBuffers.end()) return;
        DropWireShadow(found->second);
        ReleaseRenameSpares(found->second);
        DeferWireRelease(std::move(found->second.buffer),
                         found->second.imported ? T0ReleaseSerial(found->second.lastUseSerial)
                                                : found->second.lastUseSerial); // P11 B2, as Respecify
        ForgetWireBufferLookup(found->first);
        m_wireBuffers.erase(found);
        PublishWireReclaimGauges();
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
            ++m_wireStoreDestroyEpoch;
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
        NoteWireStorePeaks();
        // EVERY PARK SWEEPS. The frame boundary is not a reclaim point this arm can lean on - a
        // pbuffer replay that snapshot-exits delivers ONE present for 1.3 M calls (ID-P7-32) -
        // so the only cadence that tracks the workload is the workload itself.
        SweepDeferredWireReleases();
        EnforceWireDeferredWatermark();
    }

    void VkBufferManager::EnforceWireDeferredWatermark() {
        const Uint64 budget = static_cast<Uint64>(MG_Config::RecordArm.WireDeferredMb) * 1024u * 1024u;
        // TWO TRIGGERS, ONE SWITCH. Bytes are what the knob names, but a VkBuffer costs per
        // OBJECT as well as per byte, and small orphans never reach a byte budget: measured on
        // bsl-esc-menu-854 (spawn, lavapipe), one stretch with no submission parked 12,498 stores
        // in 39.5 MB against 64 MiB. So a fixed count ceiling rides the same sync point; the
        // knob's 0 turns both off, which keeps it one negative control.
        if (budget == 0) return;
        if (m_deferredWireBytes <= budget && m_deferredWireReleases.size() <= kWireDeferredCountCeiling) return;
        // What is left after the sweep is named by work that is recorded but unsubmitted, or
        // submitted but unretired. Every entry is tagged at or below the sync point taken HERE,
        // so waiting it out - the host-access wait's own call, which flushes the recording first
        // - proves all of them dead. A mid-frame flush is what WaitForWireBufferHostAccess and a
        // glClientWaitSync already do to this command stream; it costs one submission per
        // budget's worth of orphans, not one per glBufferData.
        if (pVulkanRenderer == nullptr ||
            !pVulkanRenderer->WaitForSubmitIndex(pVulkanRenderer->GetSyncPointSubmitIndex(), UINT64_MAX, true)) {
            if (LatchedOnDeviceLoss("buffer-deferred-watermark")) return;
            WireBufferSyncFatal("deferred-watermark");
        }
        ++m_wireDeferredSyncs;
        SweepDeferredWireReleases();
    }

    void VkBufferManager::NoteWireStorePeaks() {
        m_wireStoreCountPeak = std::max(m_wireStoreCountPeak, m_wireStoreCount);
        m_deferredWireBytesPeak = std::max(m_deferredWireBytesPeak, m_deferredWireBytes);
    }

    void VkBufferManager::PublishWireReclaimGauges() {
        if (!MG_Util::PipeStats::Enabled()) return;
        using MG_Util::PipeStats::Gauge;
        MG_Util::PipeStats::PublishGauge(Gauge::WireBuffers, static_cast<Uint64>(m_wireBuffers.size()));
        MG_Util::PipeStats::PublishGauge(Gauge::WireStoresPeak, m_wireStoreCountPeak);
        MG_Util::PipeStats::PublishGauge(Gauge::WireDeferredBytesPeak, m_deferredWireBytesPeak);
        MG_Util::PipeStats::PublishGauge(Gauge::WireDeferredSyncs, m_wireDeferredSyncs);
    }

    Bool VkBufferManager::LatchedOnDeviceLoss(const char* site) {
        return pVulkanRenderer != nullptr && pVulkanRenderer->LatchWireDeviceLoss(site);
    }

    void VkBufferManager::WireBufferSyncFatal(const char* site) {
        MGLOG_F("Magma: Fatal{ResourceUnavailable, \"buffer-write-sync\"} {site=%s}", site);
        std::abort();
    }

    SizeT VkBufferManager::SweepDeferredWireReleases() {
        if (m_deferredWireReleases.empty()) return 0;
        SizeT dead = 0;
        if (pVulkanRenderer == nullptr ||
            pVulkanRenderer->IsSubmitIndexComplete(pVulkanRenderer->GetSyncPointSubmitIndex())) {
            // Idle: nothing recorded, every submission retired - or no renderer, so no queue.
            // Every parked store is dead, including one tagged for a batch that was abandoned
            // instead of submitted.
            dead = m_deferredWireReleases.size();
        } else {
            // The list is in park order and GetSyncPointSubmitIndex() steps back only when a
            // pending recording is abandoned instead of submitted (RecreateSwapchain and the
            // minimized Present force-clear the recording flags, so `m_submitCounter + 1`
            // becomes `m_submitCounter`). Everywhere else the submit indices are non-decreasing
            // and the dead entries are a PREFIX: the first entry the renderer will not call
            // complete proves none after it is either. Across an abandonment a later entry can
            // carry the LOWER index; the walk then stops at the earlier, higher one and holds
            // both until the next submission takes that index and retires - conservative, never
            // early, and the idle rule above retires them regardless. That bounds the walk to a
            // couple of fence polls, which matters because this runs on every glBufferData the
            // wire carries.
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
        // One bump per sweep that destroyed anything is enough: what a memo needs to know
        // is "some handle it may name has been freed since", not how many.
        ++m_wireStoreDestroyEpoch;
        m_deferredWireReleases.erase(m_deferredWireReleases.begin(),
                                     m_deferredWireReleases.begin() + static_cast<std::ptrdiff_t>(dead));
        const Uint64 destroyedCount = static_cast<Uint64>(dead);
        m_wireStoreCount = destroyedCount <= m_wireStoreCount ? m_wireStoreCount - destroyedCount : 0;
        return dead;
    }

    void VkBufferManager::DestroyAllDeferredWireReleases() {
        if (!m_deferredWireReleases.empty()) ++m_wireStoreDestroyEpoch;
        for (auto& entry : m_deferredWireReleases) {
            entry.buffer.Destroy();
            if (m_wireStoreCount > 0) --m_wireStoreCount;
        }
        m_deferredWireReleases.clear();
        m_deferredWireBytes = 0;
    }

    Bool VkBufferManager::Initialize(const VkBufferManagerInitInfo& initInfo) {
        Shutdown();

        MOBILEGL_ASSERT(initInfo.allocator != nullptr, "VkBufferManager::Initialize requires valid allocator");
        MOBILEGL_ASSERT(initInfo.frameCount > 0, "VkBufferManager::Initialize requires non-zero frame count");

        m_initInfo = initInfo;
        m_deferredBufferReleases.resize(initInfo.frameCount);
        m_currentFrameIndex = 0;
        m_frameSerial = 1;
        m_completedSerialFloor = 0;
        if (!InitializeTransientArenas()) {
            return false;
        }
        g_activeBufferManager.Get() = this;
        RegisterWireResourceOps();
        return true;
    }

    void VkBufferManager::Shutdown() {
        if (g_activeBufferManager.Get() == this) {
            g_activeBufferManager.Get() = nullptr;
            // Under a transport the table is the PROCESS's and serves every session: it dispatches
            // to the calling session's own manager (WireManager), so one session's renderer going
            // away must not take it from the others - their buffer creates would be dropped from
            // then on, and every buffer born after that would be a handle the server never saw.
            if (MG_Config::Transport == MG_Config::TransportMode::Monolith &&
                MG_Pipe::MGPipeGetResourceOps() == &g_vulkanWireResourceOps) {
                MG_Pipe::MGPipeSetResourceOps(nullptr);
            }
        }
        m_transientUploadArena.Shutdown();
        m_unboundStorageBuffer.Destroy();
        m_unboundTexelBuffer.Destroy();
        DestroyAllDeferredReleases();
        // DestroyAllDeferredReleases above emptied the parked list; this destroys the stores the
        // records still hold, so nothing this arm minted outlives the count.
        if (!m_wireBuffers.empty()) ++m_wireStoreDestroyEpoch;
        m_wireBufferLookup.clear();
        m_wireBuffers.clear();
        m_wireShadowBytes = 0;
        m_wireStoreCount = 0;
        m_wireStoreCountPeak = 0;
        m_deferredWireBytesPeak = 0;
        m_wireDeferredSyncs = 0;
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
        m_currentFrameIndex = 0;
        return InitializeTransientArenas();
    }

    void VkBufferManager::BeginFrame(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_deferredBufferReleases.size(),
                        "VkBufferManager::BeginFrame frame index out of range");
        m_currentFrameIndex = frameIndex;
        ++m_frameSerial;
        if (WireBufProbeEnabled()) {
            MGLOG_I("WBUF BEGINFRAME idx=%u newSerial=%llu floor=%llu (arena slot rewound)", frameIndex,
                    static_cast<unsigned long long>(m_frameSerial),
                    static_cast<unsigned long long>(m_completedSerialFloor));
        }
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
        // exactly this way). Most mid-frame drains do not advance m_frameSerial; every
        // eighth does, but only through BeginFrame after the arena/memo boundary work.
        // The drain's per-resource sweep must not free arena storage: ResetFrame/BeginFrame is where
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
                     (VkBufferUsageFlags{VK_BUFFER_USAGE_TRANSFER_DST_BIT}) |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .memoryUsage = m_initInfo.transientMemoryUsage,
            .allocationFlags = m_initInfo.transientAllocationFlags,
            .minBufferSize = m_initInfo.minUploadBytes,
            .persistentlyMapped = m_initInfo.transientPersistentMapping,
        });
    }

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
        EndActiveRenderPassOn(commandBuffer);
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &beforeBarrier, 0, nullptr, 0, nullptr);

        VkBufferCopy region{};
        region.srcOffset = staging.offset;
        region.dstOffset = static_cast<VkDeviceSize>(offset);
        region.size = static_cast<VkDeviceSize>(size);
        EndActiveRenderPassOn(commandBuffer);
        vkCmdCopyBuffer(commandBuffer, staging.buffer, resource.buffer.GetHandle(), 1, &region);

        VkMemoryBarrier afterBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        afterBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        afterBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        EndActiveRenderPassOn(commandBuffer);
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                             &afterBarrier, 0, nullptr, 0, nullptr);

        resource.lastUseSerial = m_frameSerial;
        // B3: the copy just recorded rides the next submission too, so a later host write to
        // the same bytes must wait for it exactly as a draw's read must.
        if (pVulkanRenderer) resource.lastUseSubmitIndex = pVulkanRenderer->GetWireNextSubmitIndex();
        resource.gpuWritesPending = true;
        return true;
    }


    void VkBufferManager::CollectDeferredReleases(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_deferredBufferReleases.size(),
                        "VkBufferManager::CollectDeferredReleases frame index out of range");
        m_deferredBufferReleases[frameIndex].clear();
        // The wire list is not per-slot and does not wait for a frame boundary; a boundary is
        // just one more point to sweep at (BeginFrame after its slot fence, and every slot of
        // the idle drain, where the renderer-idle rule empties the list).
        SweepDeferredWireReleases();
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

    void VkBufferManager::DestroyAllDeferredReleases() {
        // Both callers (Shutdown, RecreateTransientArenas) have proven the device idle.
        DestroyAllDeferredWireReleases();
        for (auto& stream : m_convertedStreams) stream.buffer.Destroy();
        m_convertedStreams.clear();
        m_convertedStreamBytes = 0;
        for (auto& releases : m_deferredBufferReleases) {
            for (auto& buffer : releases) {
                buffer.Destroy();
            }
            releases.clear();
        }
        m_deferredBufferReleases.clear();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
