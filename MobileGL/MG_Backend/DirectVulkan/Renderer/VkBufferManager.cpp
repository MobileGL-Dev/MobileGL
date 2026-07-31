// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkBufferManager.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        constexpr VmaAllocationCreateFlags kResidentBufferAllocationFlags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        constexpr SizeT kLiveResourcePruneThreshold = 256;

        // A zero-copy persistent buffer is created once and never recreated (the app holds
        // its mapped pointer), and may be bound to any role, so it carries every usage.
        // TRANSFER_DST is added by CreateResidentStorage.
        constexpr VkBufferUsageFlags kPersistentBackedUsage =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
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

        void Ops_FlushMappedRange(BufferObject& bufferObject, Range1D range,
                                  Flags<BufferMappingAccessBit> appAccess) {
            if (g_activeBufferManager) {
                g_activeBufferManager->OnFlushMappedRange(bufferObject, range, appAccess);
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
            .FlushMappedRange = Ops_FlushMappedRange,
            .OnDestroy = Ops_OnDestroy,
            .AcquirePersistentMap = Ops_AcquirePersistentMap,
        };
    } // namespace

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
        return true;
    }

    void VkBufferManager::Shutdown() {
        if (g_activeBufferManager == this) {
            g_activeBufferManager = nullptr;
            if (MG_State::GLState::GetBufferBackendOps() == &g_vulkanBufferBackendOps) {
                MG_State::GLState::SetBufferBackendOps(nullptr);
            }
        }
        m_transientUploadArena.Shutdown();
        DestroyAllDeferredReleases();
        ReleaseAllLiveResources();
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
        CollectDeferredReleases(frameIndex);
        m_transientUploadArena.BeginFrame(frameIndex);
    }

    void VkBufferManager::CollectAllDeferredReleases() {
        for (Uint32 frameIndex = 0; frameIndex < m_deferredBufferReleases.size(); ++frameIndex) {
            CollectDeferredReleases(frameIndex);
        }
        for (Uint32 frameIndex = 0; frameIndex < m_transientUploadArena.GetFrameCount(); ++frameIndex) {
            m_transientUploadArena.CollectDeferredReleases(frameIndex);
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
        (void)kind;
        return m_transientUploadArena.Upload(frameIndex, data, size, alignment, outSlice);
    }

    Bool VkBufferManager::InitializeTransientArenas() {
        return m_transientUploadArena.Initialize({
            .allocator = m_initInfo.allocator,
            .frameCount = m_initInfo.frameCount,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
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
        if (m_liveResources.size() >= kLiveResourcePruneThreshold) {
            std::erase_if(m_liveResources, [](const WeakPtr<VkBufferResource>& weak) { return weak.expired(); });
        }
        m_liveResources.push_back(resource);
    }

    void VkBufferManager::ReleaseAllLiveResources() {
        for (auto& weak : m_liveResources) {
            if (auto resource = weak.lock()) {
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
            MGLOG_E("VkBufferManager::CreateResidentStorage failed (size=%llu)",
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
            MGLOG_E("VkBufferManager::SwapStorageAndUploadAll: upload failed");
            resource.pendingFullUpload = true;
            return false;
        }
        resource.pendingFullUpload = false;
        return true;
    }

    Bool VkBufferManager::StagedRangeCopy(VkBufferResource& resource, MG_State::GLState::BufferObject& bufferObject,
                                          SizeT offset, SizeT size) {
        if (!m_copyProvider) {
            return false;
        }
        BufferSlice staging{};
        if (!m_transientUploadArena.Upload(m_currentFrameIndex, bufferObject.MappedData() + offset,
                                           static_cast<VkDeviceSize>(size), 16, staging)) {
            return false;
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

    void VkBufferManager::OnRespecify(MG_State::GLState::BufferObject& bufferObject) {
        auto* resource = ResourceOf(bufferObject);
        if (!resource) {
            return; // lazy: AcquireResidentSlice performs a full upload on creation
        }
        // Any cached streaming slice refers to the previous contents.
        resource->transientFrameSerial = 0;
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
            MGLOG_E("VkBufferManager::OnRespecify: in-place upload failed");
            resource->pendingFullUpload = true;
        }
    }

    void VkBufferManager::OnSubData(MG_State::GLState::BufferObject& bufferObject, SizeT offset, SizeT size) {
        auto* resource = ResourceOf(bufferObject);
        if (!resource) {
            return;
        }
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
                MGLOG_E("VkBufferManager::OnSubData: host upload failed");
                resource->pendingFullUpload = true;
            }
            return;
        }

        // Busy partial write: stage + GPU copy preserves GL ordering within the
        // frame and leaves bytes outside the range (possibly GPU-written, e.g.
        // SSBO) intact. Fall back to a storage swap if staging is unavailable.
        if (!StagedRangeCopy(*resource, bufferObject, offset, size)) {
            SwapStorageAndUploadAll(*resource, bufferObject);
        }
    }

    void VkBufferManager::OnFlushMappedRange(MG_State::GLState::BufferObject& bufferObject, Range1D range,
                                             Flags<BufferMappingAccessBit> appAccess) {
        auto* resource = ResourceOf(bufferObject);
        if (!resource) {
            return;
        }
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
                MGLOG_E("VkBufferManager::OnFlushMappedRange: host upload failed");
                resource->pendingFullUpload = true;
            }
            return;
        }

        if (!StagedRangeCopy(*resource, bufferObject, offset, size)) {
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
            MGLOG_E("VkBufferManager::AcquireResidentSlice failed: buffer size is zero");
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
                MGLOG_E("VkBufferManager::AcquireResidentSlice failed: initial upload failed");
                resource->buffer.Destroy();
                resource->storageSize = 0;
                resource->usageFlags = 0;
                return false;
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

        const VkDeviceSize size = static_cast<VkDeviceSize>(bufferObject->GetSize());
        if (size == 0) {
            MGLOG_E("VkBufferManager::AcquireStreamedSlice failed: buffer size is zero");
            return false;
        }

        const Uint64 changeSerial = bufferObject->GetChangeSerial();
        if (resource->transientFrameSerial == m_frameSerial && resource->transientChangeSerial == changeSerial &&
            resource->transientSize == size && resource->transientSlice.IsValid()) {
            outSlice = resource->transientSlice;
            return true;
        }

        if (!m_transientUploadArena.Upload(m_currentFrameIndex, bufferObject->MappedData(), size, 16,
                                           outSlice)) {
            return false;
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
            return VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
        case BufferKind::ShaderStorage:
            return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        case BufferKind::Indirect:
            return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        default:
            return 0;
        }
    }

    void VkBufferManager::DestroyAllDeferredReleases() {
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
