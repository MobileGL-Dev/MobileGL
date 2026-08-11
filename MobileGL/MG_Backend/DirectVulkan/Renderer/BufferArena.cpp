// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/BufferArena.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BufferArena.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    Bool BufferArena::Initialize(const BufferArenaDesc& desc) {
        Shutdown();

        MOBILEGL_ASSERT(desc.allocator != nullptr, "BufferArena::Initialize requires valid allocator");
        MOBILEGL_ASSERT(desc.frameCount > 0, "BufferArena::Initialize requires non-zero frame count");
        MOBILEGL_ASSERT(desc.usage != 0, "BufferArena::Initialize requires non-zero buffer usage");

        m_desc = desc;
        m_frames.clear();
        m_frames.resize(desc.frameCount);
        m_deferredReleases.resize(desc.frameCount);
        return true;
    }

    void BufferArena::Shutdown() {
        for (auto& frame : m_frames) {
            frame.buffer.Destroy();
            frame.writeCursor = 0;
        }
        m_frames.clear();
        m_deferredReleases.clear();
        m_desc = {};
    }

    void BufferArena::BeginFrame(Uint32 frameIndex) {
        CollectDeferredReleases(frameIndex);
        ResetFrame(frameIndex);
    }

    void BufferArena::ResetFrame(Uint32 frameIndex) {
        AssertValidFrameIndex(frameIndex);
        m_frames[frameIndex].writeCursor = 0;
    }

    void BufferArena::CollectDeferredReleases(Uint32 frameIndex) {
        AssertValidFrameIndex(frameIndex);
        m_deferredReleases[frameIndex].clear();
    }

    Bool BufferArena::Allocate(Uint32 frameIndex, VkDeviceSize size, VkDeviceSize alignment, BufferSlice& outSlice) {
        AssertValidFrameIndex(frameIndex);
        MOBILEGL_ASSERT(size > 0, "BufferArena::Allocate requires non-zero size");

        auto& frame = m_frames[frameIndex];
        const VkDeviceSize resolvedAlignment = alignment > 0 ? alignment : 1;
        const VkDeviceSize offset = (frame.writeCursor + resolvedAlignment - 1) & ~(resolvedAlignment - 1);
        const VkDeviceSize endOffset = offset + size;

        if (!EnsureCapacity(frameIndex, endOffset)) {
            return false;
        }

        frame.writeCursor = endOffset;
        outSlice = frame.buffer.GetSlice(offset, size);
        return outSlice.IsValid();
    }

    Bool BufferArena::Upload(Uint32 frameIndex, const void* data, VkDeviceSize size, VkDeviceSize alignment,
                             BufferSlice& outSlice) {
        MOBILEGL_ASSERT(data != nullptr || size == 0, "BufferArena::Upload data pointer is null");
        if (!Allocate(frameIndex, size, alignment, outSlice)) {
            return false;
        }

        if (outSlice.mapped != nullptr) {
            Memcpy(outSlice.mapped, data, static_cast<SizeT>(size));
            return true;
        }

        return m_frames[frameIndex].buffer.Upload(data, size, outSlice.offset);
    }

    VkDeviceSize BufferArena::GetWriteCursor(Uint32 frameIndex) const {
        AssertValidFrameIndex(frameIndex);
        return m_frames[frameIndex].writeCursor;
    }

    Uint32 BufferArena::GetFrameCount() const {
        return static_cast<Uint32>(m_frames.size());
    }

    Bool BufferArena::EnsureCapacity(Uint32 frameIndex, VkDeviceSize requiredEndOffset) {
        AssertValidFrameIndex(frameIndex);
        auto& frame = m_frames[frameIndex];
        auto& buffer = frame.buffer;

        if (buffer.IsValid() && buffer.GetSize() >= requiredEndOffset) {
            return true;
        }

        VkDeviceSize newCapacity = buffer.IsValid() ? buffer.GetSize() : 0;
        if (newCapacity < m_desc.minBufferSize) {
            newCapacity = m_desc.minBufferSize;
        }
        if (newCapacity == 0) {
            newCapacity = requiredEndOffset;
        }
        while (newCapacity < requiredEndOffset) {
            newCapacity *= 2;
        }

        if (buffer.IsValid()) {
            // Outgrown, not dead: every BufferSlice handed out from this frame's arena so far
            // still names it, and those slices stay in service until the frame slot is rewound
            // (VkBufferResource::transientSlice, the converted-vertex-stream cache, the draw
            // memos). The release therefore has to survive every mid-frame reclaim and land on
            // the next ResetFrame of this slot - see VkBufferManager::CollectAllDeferredReleases.
            m_deferredReleases[frameIndex].push_back(std::move(buffer));
        }

        VkBufferObjectDesc bufferDesc{};
        bufferDesc.allocator = m_desc.allocator;
        bufferDesc.size = newCapacity;
        bufferDesc.usage = m_desc.usage;
        bufferDesc.memoryUsage = m_desc.memoryUsage;
        bufferDesc.allocationFlags = m_desc.allocationFlags;
        if (!buffer.Create(bufferDesc)) {
            return false;
        }
        if (m_desc.persistentlyMapped && buffer.Map() == nullptr) {
            buffer.Destroy();
            return false;
        }

        frame.writeCursor = 0;
        return true;
    }

    void BufferArena::AssertValidFrameIndex(Uint32 frameIndex) const {
        MOBILEGL_ASSERT(frameIndex < m_frames.size(), "BufferArena frame index out of range");
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
