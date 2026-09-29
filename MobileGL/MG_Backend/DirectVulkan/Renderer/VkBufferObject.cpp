// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkBufferObject.h"

#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Remote/Transport/AdoptT0.h>
#endif

namespace MobileGL::MG_Backend::DirectVulkan {
#if MOBILEGL_BUILD_DISAGGREGATED
    void VkBufferObject::MoveExternalFrom(VkBufferObject& other) {
        m_device = other.m_device;
        m_externalMemory = other.m_externalMemory;
        m_externalAhb = other.m_externalAhb;
        m_externalCoherent = other.m_externalCoherent;
        other.m_device = VK_NULL_HANDLE;
        other.m_externalMemory = VK_NULL_HANDLE;
        other.m_externalAhb = nullptr;
        other.m_externalCoherent = true;
    }

    void VkBufferObject::AdoptExternal(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize size,
                                       void* mapped, Bool coherent, void* ahb) {
        Destroy();
        m_device = device;
        m_buffer = buffer;
        m_externalMemory = memory;
        m_mappedData = mapped;
        m_size = size;
        m_externalCoherent = coherent;
        m_externalAhb = ahb;
    }
#endif

    VkBufferObject::VkBufferObject(VkBufferObject&& other) noexcept {
        m_allocator = other.m_allocator;
        m_buffer = other.m_buffer;
        m_allocation = other.m_allocation;
        m_mappedData = other.m_mappedData;
        m_size = other.m_size;
#if MOBILEGL_BUILD_DISAGGREGATED
        MoveExternalFrom(other);
#endif

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
#if MOBILEGL_BUILD_DISAGGREGATED
        MoveExternalFrom(other);
#endif

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
            MGLOG_E_ONCE("VkBufferObject::Create failed: vmaCreateBuffer returned %d", result);
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
#if MOBILEGL_BUILD_DISAGGREGATED
        if (m_externalMemory != VK_NULL_HANDLE) {
            // P11 B2 (T0): an imported AHardwareBuffer store. Reached through the wire arm's
            // deferred release, i.e. once no submission can still name it.
            if (m_mappedData != nullptr) vkUnmapMemory(m_device, m_externalMemory);
            if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
            vkFreeMemory(m_device, m_externalMemory, nullptr);
            MG_Remote::Transport::AdoptT0::ReleaseImported(m_externalAhb);
            m_mappedData = nullptr;
            m_buffer = VK_NULL_HANDLE;
            m_externalMemory = VK_NULL_HANDLE;
            m_externalAhb = nullptr;
            m_device = VK_NULL_HANDLE;
            m_externalCoherent = true;
            m_size = 0;
            return;
        }
#endif
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
            MGLOG_E_ONCE("VkBufferObject::Map failed: vmaMapMemory returned %d", mapResult);
            m_mappedData = nullptr;
            return nullptr;
        }

        return m_mappedData;
    }

    void VkBufferObject::Unmap() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // An imported store stays mapped for its life (Destroy unmaps it).
        if (m_externalMemory != VK_NULL_HANDLE) return;
#endif
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
            MGLOG_E_ONCE("VkBufferObject::Upload failed: unable to map buffer");
            return false;
        }

        Memcpy(static_cast<Uint8*>(mapped) + offset, data, static_cast<SizeT>(size));
#if MOBILEGL_BUILD_DISAGGREGATED
        if (m_externalMemory != VK_NULL_HANDLE) {
            if (m_externalCoherent) return true;
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = m_externalMemory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            return vkFlushMappedMemoryRanges(m_device, 1, &range) == VK_SUCCESS;
        }
#endif
        const VkResult flushResult = vmaFlushAllocation(m_allocator, m_allocation, offset, size);
        if (flushResult != VK_SUCCESS) {
            MGLOG_E_ONCE("VkBufferObject::Upload failed: vmaFlushAllocation returned %d", flushResult);
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
#if MOBILEGL_BUILD_DISAGGREGATED
        if (m_externalMemory != VK_NULL_HANDLE) {
            if (m_externalCoherent) return true;
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = m_externalMemory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            return vkInvalidateMappedMemoryRanges(m_device, 1, &range) == VK_SUCCESS;
        }
#endif

        const VkResult result = vmaInvalidateAllocation(m_allocator, m_allocation, offset, resolvedSize);
        if (result != VK_SUCCESS) {
            MGLOG_E_ONCE("VkBufferObject::Invalidate failed: vmaInvalidateAllocation returned %d", result);
            return false;
        }
        return true;
    }

} // namespace MobileGL::MG_Backend::DirectVulkan
