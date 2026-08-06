// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkBufferObject.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    VkBufferObject::VkBufferObject(VkBufferObject&& other) noexcept {
        m_allocator = other.m_allocator;
        m_buffer = other.m_buffer;
        m_allocation = other.m_allocation;
        m_mappedData = other.m_mappedData;
        m_size = other.m_size;

        other.m_allocator = nullptr;
        other.m_buffer = VK_NULL_HANDLE;
        other.m_allocation = nullptr;
        other.m_mappedData = nullptr;
        other.m_size = 0;
    }

    VkBufferObject& VkBufferObject::operator=(VkBufferObject&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        Destroy();

        m_allocator = other.m_allocator;
        m_buffer = other.m_buffer;
        m_allocation = other.m_allocation;
        m_mappedData = other.m_mappedData;
        m_size = other.m_size;

        other.m_allocator = nullptr;
        other.m_buffer = VK_NULL_HANDLE;
        other.m_allocation = nullptr;
        other.m_mappedData = nullptr;
        other.m_size = 0;
        return *this;
    }

    VkBufferObject::~VkBufferObject() {
        Destroy();
    }

    Bool VkBufferObject::Create(const VkBufferObjectDesc& desc) {
        return Create(desc.allocator, desc.size, desc.usage, desc.memoryUsage, desc.allocationFlags,
                      desc.requiredFlags);
    }

    Bool VkBufferObject::Create(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                                VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocationFlags,
                                VkMemoryPropertyFlags requiredFlags) {
        MOBILEGL_ASSERT(allocator != nullptr, "VkBufferObject::Create requires valid VMA allocator");
        MOBILEGL_ASSERT(size > 0, "VkBufferObject::Create requires non-zero size");

        Destroy();
        m_allocator = allocator;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = memoryUsage;
        allocationInfo.flags = allocationFlags;
        allocationInfo.requiredFlags = requiredFlags;

        const VkResult result =
            vmaCreateBuffer(m_allocator, &bufferInfo, &allocationInfo, &m_buffer, &m_allocation, nullptr);
        if (result != VK_SUCCESS) {
            MGLOG_E("VkBufferObject::Create failed: vmaCreateBuffer returned %d", result);
            m_allocator = nullptr;
            m_buffer = VK_NULL_HANDLE;
            m_allocation = nullptr;
            m_size = 0;
            return false;
        }

        m_size = size;
        return true;
    }

    void VkBufferObject::Destroy() {
        Unmap();
        if (m_allocator != nullptr && m_buffer != VK_NULL_HANDLE && m_allocation != nullptr) {
            vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
        }
        m_buffer = VK_NULL_HANDLE;
        m_allocation = nullptr;
        m_allocator = nullptr;
        m_size = 0;
    }

    void* VkBufferObject::Map() {
        MOBILEGL_ASSERT(IsValid(), "VkBufferObject::Map called on invalid buffer");

        if (m_mappedData != nullptr) {
            return m_mappedData;
        }

        const VkResult mapResult = vmaMapMemory(m_allocator, m_allocation, &m_mappedData);
        if (mapResult != VK_SUCCESS || m_mappedData == nullptr) {
            MGLOG_E("VkBufferObject::Map failed: vmaMapMemory returned %d", mapResult);
            m_mappedData = nullptr;
            return nullptr;
        }

        return m_mappedData;
    }

    void VkBufferObject::Unmap() {
        if (!IsValid() || m_mappedData == nullptr) {
            m_mappedData = nullptr;
            return;
        }

        vmaUnmapMemory(m_allocator, m_allocation);
        m_mappedData = nullptr;
    }

    Bool VkBufferObject::Upload(const void* data, VkDeviceSize size, VkDeviceSize offset) {
        MOBILEGL_ASSERT(IsValid(), "VkBufferObject::Upload called on invalid buffer");
        MOBILEGL_ASSERT(data != nullptr || size == 0, "VkBufferObject::Upload data pointer is null");
        MOBILEGL_ASSERT(offset + size <= m_size, "VkBufferObject::Upload out of range");

        if (size == 0) {
            return true;
        }

        const Bool wasMapped = IsMapped();
        void* mapped = wasMapped ? m_mappedData : Map();
        if (mapped == nullptr) {
            MGLOG_E("VkBufferObject::Upload failed: unable to map buffer");
            return false;
        }

        Memcpy(static_cast<Uint8*>(mapped) + offset, data, static_cast<SizeT>(size));
        const VkResult flushResult = vmaFlushAllocation(m_allocator, m_allocation, offset, size);
        if (flushResult != VK_SUCCESS) {
            MGLOG_E("VkBufferObject::Upload failed: vmaFlushAllocation returned %d", flushResult);
            if (!wasMapped) {
                Unmap();
            }
            return false;
        }
        if (!wasMapped) {
            Unmap();
        }
        return true;
    }

    Bool VkBufferObject::Invalidate(VkDeviceSize size, VkDeviceSize offset) {
        MOBILEGL_ASSERT(IsValid(), "VkBufferObject::Invalidate called on invalid buffer");
        MOBILEGL_ASSERT(IsMapped(), "VkBufferObject::Invalidate requires mapped memory");
        MOBILEGL_ASSERT(offset <= m_size, "VkBufferObject::Invalidate offset out of range");

        const VkDeviceSize resolvedSize = size == VK_WHOLE_SIZE ? m_size - offset : size;
        MOBILEGL_ASSERT(offset + resolvedSize <= m_size, "VkBufferObject::Invalidate range out of bounds");
        if (resolvedSize == 0) {
            return true;
        }

        const VkResult result = vmaInvalidateAllocation(m_allocator, m_allocation, offset, resolvedSize);
        if (result != VK_SUCCESS) {
            MGLOG_E("VkBufferObject::Invalidate failed: vmaInvalidateAllocation returned %d", result);
            return false;
        }
        return true;
    }

} // namespace MobileGL::MG_Backend::DirectVulkan
