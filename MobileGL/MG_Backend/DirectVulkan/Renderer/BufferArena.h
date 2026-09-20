// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/BufferArena.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "BufferSlice.h"
#include "VkBufferObject.h"
#include "../VkIncludes.h"
#include <Includes.h>
#include <vk_mem_alloc.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    struct BufferArenaDesc {
        VmaAllocator allocator = nullptr;
        Uint32 frameCount = 0;
        VkBufferUsageFlags usage = 0;
        VmaMemoryUsage memoryUsage = VMA_MEMORY_USAGE_AUTO;
        VmaAllocationCreateFlags allocationFlags = 0;
        VkDeviceSize minBufferSize = 0;
        Bool persistentlyMapped = false;
    };

    class BufferArena {
    public:
        Bool Initialize(const BufferArenaDesc& desc);
        void Shutdown();

        void BeginFrame(Uint32 frameIndex);
        void ResetFrame(Uint32 frameIndex);
        void CollectDeferredReleases(Uint32 frameIndex);

        Bool Allocate(Uint32 frameIndex, VkDeviceSize size, VkDeviceSize alignment, BufferSlice& outSlice);
        Bool Upload(Uint32 frameIndex, const void* data, VkDeviceSize size, VkDeviceSize alignment, BufferSlice& outSlice);

        VkDeviceSize GetWriteCursor(Uint32 frameIndex) const;
        Uint32 GetFrameCount() const;

        // Diagnostic: cumulative bytes ever uploaded through any arena.
        static Uint64 GetCumulativeUploadedBytes();

    private:
        struct FrameResources {
            VkBufferObject buffer;
            VkDeviceSize writeCursor = 0;
        };

        Bool EnsureCapacity(Uint32 frameIndex, VkDeviceSize requiredEndOffset);
        void AssertValidFrameIndex(Uint32 frameIndex) const;

        BufferArenaDesc m_desc{};
        Vector<FrameResources> m_frames;
        Vector<Vector<VkBufferObject>> m_deferredReleases;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
