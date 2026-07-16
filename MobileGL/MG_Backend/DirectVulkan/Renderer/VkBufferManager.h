// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "BufferArena.h"
#include "MG_State/GLState/BufferState/BufferObject.h"
#include "../VkIncludes.h"
#include <Includes.h>
#include <vk_mem_alloc.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    enum class BufferKind : Uint8 {
        Vertex,
        Index,
        Uniform,
        TextureBuffer,
        ShaderStorage,
        Indirect,
    };

    struct VkBufferManagerInitInfo {
        VmaAllocator allocator = nullptr;
        Uint32 frameCount = 0;
        VkDeviceSize minUploadBytes = 4 * 1024 * 1024;
        VmaMemoryUsage transientMemoryUsage = VMA_MEMORY_USAGE_AUTO;
        VmaAllocationCreateFlags transientAllocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        Bool transientPersistentMapping = false;
    };

    // The DirectVulkan storage behind one frontend buffer (pipe_resource analogue).
    // Owned (refcounted) by the frontend BufferObject; the manager holds only weak
    // references (for shutdown) plus strong references on deferred-release lists.
    class VkBufferResource : public MG_State::GLState::BackendBufferResource {
    public:
        ~VkBufferResource() override = default;

        // Resident storage (may be invalid for streaming-only buffers).
        VkBufferObject buffer;
        VkDeviceSize storageSize = 0;
        VkBufferUsageFlags usageFlags = 0;
        // Frame serial of the last GPU reference; drives busy tracking.
        Uint64 lastUseSerial = 0;
        // Set when an immediate op could not be applied; forces a full re-upload
        // on the next AcquireResidentSlice.
        Bool pendingFullUpload = false;
        // Backs a zero-copy coherent persistent map (PipeResource GPU residency): the
        // buffer is HOST_VISIBLE+COHERENT, persistently mapped, carries every usage and is
        // never orphaned or recreated. Draw-time acquire binds it directly, no re-upload.
        Bool persistentMapped = false;

        // Cached transient (streaming) slice for the current frame.
        BufferSlice transientSlice{};
        Uint64 transientFrameSerial = 0;
        Uint64 transientChangeSerial = 0;
        VkDeviceSize transientSize = 0;
    };

    // Supplies a command buffer that is recording and outside any render pass,
    // for staged buffer-range copies. Implemented by VulkanRenderer.
    class IBufferCopyCommandProvider {
    public:
        virtual ~IBufferCopyCommandProvider() = default;
        virtual VkCommandBuffer AcquireBufferCopyCommandBuffer() = 0;
    };

    class VkBufferManager {
    public:
        Bool Initialize(const VkBufferManagerInitInfo& initInfo);
        void Shutdown();

        // Recreate all per-frame transient arenas
        Bool RecreateTransientArenas(Uint32 frameCount);
        void BeginFrame(Uint32 frameIndex);
        // All previously submitted GPU work has completed (vkDeviceWaitIdle).
        void NotifyDeviceIdle();
        // A frame slot's submission fence has been waited: every serial up to
        // and including `serial` is complete. Raises the completed floor so
        // GetCompletedSerial reflects real fence progress instead of only the
        // frameSerial-minus-frameCount inference.
        void NotifyFrameSerialComplete(Uint64 serial);
        void SetCopyCommandProvider(IBufferCopyCommandProvider* provider);

        Bool UploadTransient(BufferKind kind, Uint32 frameIndex, const void* data, VkDeviceSize size,
                             VkDeviceSize alignment, BufferSlice& outSlice);

        // Draw-time acquire for resident (device-storage) buffers: ensures the
        // resource exists and is fully uploaded, marks it used this frame.
        Bool AcquireResidentSlice(BufferKind kind, const SharedPtr<MG_State::GLState::BufferObject>& bufferObject,
                                  BufferSlice& outSlice);
        // Draw-time acquire for streamed buffers: uploads the whole shadow into
        // the per-frame arena (cached by change serial), releasing any resident
        // storage the buffer may still own.
        Bool AcquireStreamedSlice(BufferKind kind, const SharedPtr<MG_State::GLState::BufferObject>& bufferObject,
                                  BufferSlice& outSlice);

        // Zero-copy persistent map (PipeResource GPU residency): create (once) a
        // HOST_VISIBLE+COHERENT, persistently mapped resident buffer carrying every usage,
        // seed it from the shadow, and return its mapped base for the app to write into
        // directly. Idempotent. Returns nullptr on failure (frontend keeps its shadow).
        void* AcquirePersistentMap(MG_State::GLState::BufferObject& bufferObject);

        // Immediate ops, dispatched from the frontend BufferBackendOps table.
        void OnRespecify(MG_State::GLState::BufferObject& bufferObject);
        void OnSubData(MG_State::GLState::BufferObject& bufferObject, SizeT offset, SizeT size);
        void OnFlushMappedRange(MG_State::GLState::BufferObject& bufferObject, Range1D range,
                                Flags<BufferMappingAccessBit> appAccess);
        void OnResourceDestroyed(SharedPtr<MG_State::GLState::BackendBufferResource>&& resource);

        Uint64 GetFrameSerial() const { return m_frameSerial; }
        // Highest frame serial whose GPU work is known complete; serials at or
        // below it may be considered signaled. Drives IsResourceBusy and the
        // backend GL fence objects.
        Uint64 GetCompletedSerial() const;
        // Busy = potentially referenced by GPU work that has not been fenced yet
        // (including commands recorded for the current, unsubmitted frame).
        Bool IsResourceBusy(const VkBufferResource& resource) const;

    private:
        Bool InitializeTransientArenas();
        static VkBufferUsageFlags GetVkBufferUsage(BufferKind kind);
        VkBufferResource* GetOrCreateResource(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject);
        static VkBufferResource* ResourceOf(MG_State::GLState::BufferObject& bufferObject);
        Bool CreateResidentStorage(VkBufferResource& resource, VkDeviceSize size, VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags requiredFlags = 0);
        // Swap storage (conditional orphan) and refill it from the shadow copy.
        Bool SwapStorageAndUploadAll(VkBufferResource& resource, MG_State::GLState::BufferObject& bufferObject);
        // Record a staging-slice copy into the resident storage, ordered against
        // in-flight and already-recorded GPU work.
        Bool StagedRangeCopy(VkBufferResource& resource, MG_State::GLState::BufferObject& bufferObject,
                             SizeT offset, SizeT size);
        void DeferRelease(VkBufferObject&& buffer);
        void CollectDeferredReleases(Uint32 frameIndex);
        void DestroyAllDeferredReleases();
        void TrackLiveResource(const SharedPtr<VkBufferResource>& resource);
        void ReleaseAllLiveResources();

        VkBufferManagerInitInfo m_initInfo{};
        BufferArena m_transientUploadArena;
        IBufferCopyCommandProvider* m_copyProvider = nullptr;
        Vector<Vector<VkBufferObject>> m_deferredBufferReleases;
        Vector<Vector<SharedPtr<VkBufferResource>>> m_deferredResourceReleases;
        Vector<WeakPtr<VkBufferResource>> m_liveResources;
        Uint32 m_currentFrameIndex = 0;
        Uint64 m_frameSerial = 1;
        Uint64 m_completedSerialFloor = 0;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
