// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "BufferSlice.h"
#include "../VkIncludes.h"
#include <Includes.h>
#include <vk_mem_alloc.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    struct VkBufferObjectDesc {
        VmaAllocator allocator = nullptr;
        VkDeviceSize size = 0;
        VkBufferUsageFlags usage = 0;
        VmaMemoryUsage memoryUsage = VMA_MEMORY_USAGE_AUTO;
        VmaAllocationCreateFlags allocationFlags = 0;
        // Memory property bits the allocation MUST satisfy (e.g. HOST_VISIBLE|HOST_COHERENT
        // for a persistently-mapped buffer the app writes into without explicit flushes).
        VkMemoryPropertyFlags requiredFlags = 0;
    };

    class VkBufferObject {
    public:
        VkBufferObject() = default;
        ~VkBufferObject();

        VkBufferObject(const VkBufferObject&) = delete;
        VkBufferObject& operator=(const VkBufferObject&) = delete;
        VkBufferObject(VkBufferObject&& other) noexcept;
        VkBufferObject& operator=(VkBufferObject&& other) noexcept;

        Bool Create(const VkBufferObjectDesc& desc);
        Bool Create(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                    VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocationFlags = 0,
                    VkMemoryPropertyFlags requiredFlags = 0);
        void Destroy();

        void* Map();
        void Unmap();
        Bool Upload(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);
        Bool Invalidate(VkDeviceSize size = VK_WHOLE_SIZE, VkDeviceSize offset = 0);

        VkBuffer GetHandle() const { return m_buffer; }
        VkDeviceSize GetSize() const { return m_size; }
        // Inline: runs on the per-draw acquire path (a resident buffer bind is a
        // GetSlice per binding), where an out-of-line call was measurable.
        BufferSlice GetSlice(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const {
            MOBILEGL_ASSERT(offset <= m_size, "VkBufferObject::GetSlice offset out of range");
            const VkDeviceSize resolvedSize = (size == VK_WHOLE_SIZE) ? (m_size - offset) : size;
            MOBILEGL_ASSERT(offset + resolvedSize <= m_size, "VkBufferObject::GetSlice range out of bounds");

            BufferSlice slice{};
            slice.buffer = m_buffer;
            slice.offset = offset;
            slice.size = resolvedSize;
            slice.mapped = (m_mappedData != nullptr) ? static_cast<Uint8*>(m_mappedData) + offset : nullptr;
            return slice;
        }
        void* GetMappedData() const { return m_mappedData; }
        Bool IsMapped() const { return m_mappedData != nullptr; }
        Bool IsValid() const {
#if MOBILEGL_BUILD_RECORD_ARM
            if (m_externalMemory != VK_NULL_HANDLE) return m_buffer != VK_NULL_HANDLE;
#endif
            return m_allocator != nullptr && m_buffer != VK_NULL_HANDLE && m_allocation != nullptr;
        }

#if MOBILEGL_BUILD_RECORD_ARM
        // P11 B2 (T0): a store whose memory is NOT this allocator's - a client's AHardwareBuffer
        // imported with VK_ANDROID_external_memory_android_hardware_buffer into `memory` (a
        // dedicated allocation bound to `buffer`, mapped persistently at `mapped`). It behaves
        // like any mapped store (GetSlice, Upload, Invalidate), and Destroy frees the buffer and
        // the memory and drops `ahb` - which is why the store goes through the wire arm's
        // serial-gated deferred release like every other: the AHardwareBuffer is let go only once
        // no submission can still name it. `coherent` = the memory type is HOST_COHERENT.
        void AdoptExternal(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize size,
                           void* mapped, Bool coherent, void* ahb);
        Bool IsExternal() const { return m_externalMemory != VK_NULL_HANDLE; }
#endif

    private:
        VmaAllocator m_allocator = nullptr;
        VkBuffer m_buffer = VK_NULL_HANDLE;
        VmaAllocation m_allocation = nullptr;
        void* m_mappedData = nullptr;
        VkDeviceSize m_size = 0;
#if MOBILEGL_BUILD_DISAGGREGATED
        VkDevice m_device = VK_NULL_HANDLE;
        VkDeviceMemory m_externalMemory = VK_NULL_HANDLE;
        void* m_externalAhb = nullptr;
        Bool m_externalCoherent = true;
        void MoveExternalFrom(VkBufferObject& other);
#endif
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
