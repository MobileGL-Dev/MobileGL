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

        VkBuffer GetHandle() const { return m_buffer; }
        VkDeviceSize GetSize() const { return m_size; }
        BufferSlice GetSlice(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;
        void* GetMappedData() const { return m_mappedData; }
        Bool IsMapped() const { return m_mappedData != nullptr; }
        Bool IsValid() const { return m_allocator != nullptr && m_buffer != VK_NULL_HANDLE && m_allocation != nullptr; }

    private:
        VmaAllocator m_allocator = nullptr;
        VkBuffer m_buffer = VK_NULL_HANDLE;
        VmaAllocation m_allocation = nullptr;
        void* m_mappedData = nullptr;
        VkDeviceSize m_size = 0;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
