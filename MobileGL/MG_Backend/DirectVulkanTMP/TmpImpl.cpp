// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/TmpImpl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TmpImpl.h"

#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Util/ShaderTranspiler/Types.h>
#include "MG_Util/Debug/Log.h"
#include <xxhash.h>

#define DEBUG_TRACE_POINT() MGLOG_D("DV TmpImpl Trace: %s:%d", __func__, __LINE__)
namespace MobileGL::MG_Backend::DirectVulkanTMP::TmpImpl {
    class PendingClearInfo;
    void BeginRendering(GLbitfield clearMask, const PendingClearInfo* clearInfo);
    void BeginCommandBuffer();
    void SubmitImmediate(const std::function<void(VkCommandBuffer)>& record);

    VulkanState g;

    void ClearFrameTrashBuffers();
    void ClearFrameTrashImages();

    Uint32 FindMemoryType(Uint32 typeFilter, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(g.ctx->GetPhysicalDevice(), &memProps);
        for (Uint32 i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
                return i;
            }
        }
        throw RuntimeError("No suitable memory type");
    }

    void DestroyBuffer(BufferResource& res) {
        if (res.mapped && res.memory != VK_NULL_HANDLE) {
            vkUnmapMemory(g.ctx->GetDevice(), res.memory);
            res.mapped = nullptr;
        }
        if (res.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g.ctx->GetDevice(), res.buffer, nullptr);
            res.buffer = VK_NULL_HANDLE;
        }
        if (res.memory != VK_NULL_HANDLE) {
            vkFreeMemory(g.ctx->GetDevice(), res.memory, nullptr);
            res.memory = VK_NULL_HANDLE;
        }
        res.size = 0;
        res.offset = 0;
        res.fromRing = false;
    }

    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, BufferResource& out) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_VERIFY(vkCreateBuffer(g.ctx->GetDevice(), &bci, nullptr, &out.buffer), "vkCreateBuffer");

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(g.ctx->GetDevice(), out.buffer, &req);

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
        VK_VERIFY(vkAllocateMemory(g.ctx->GetDevice(), &mai, nullptr, &out.memory), "vkAllocateMemory");
        VK_VERIFY(vkBindBufferMemory(g.ctx->GetDevice(), out.buffer, out.memory, 0), "vkBindBufferMemory");

        out.size = size;
        out.usage = usage;
        out.props = props;
        if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            VK_VERIFY(vkMapMemory(g.ctx->GetDevice(), out.memory, 0, req.size, 0, &out.mapped), "vkMapMemory");
        }

        MOBILEGL_ASSERT(out.buffer != VK_NULL_HANDLE || out.memory != VK_NULL_HANDLE,
                        "CreateBuffer: buffer or memory is null");
    }

    BufferResource AcquireStagingBuffer(VkDeviceSize size) {
        constexpr VkDeviceSize kStagingRingDefaultSize = 8ull * 1024ull * 1024ull;
        constexpr VkDeviceSize kStagingRingAlignment = 16ull;
        if (!g.frames.empty() && g.currentFrame < g.stagingRings.size()) {
            auto& ring = g.stagingRings[g.currentFrame];
            if (ring.buffer.buffer == VK_NULL_HANDLE) {
                VkDeviceSize cap = std::max(kStagingRingDefaultSize, size);
                CreateBuffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ring.buffer);
                ring.capacity = cap;
                ring.head = 0;
            }

            VkDeviceSize alignedHead = (ring.head + (kStagingRingAlignment - 1)) & ~(kStagingRingAlignment - 1);
            if (alignedHead + size <= ring.capacity) {
                BufferResource res{};
                res.buffer = ring.buffer.buffer;
                res.memory = ring.buffer.memory;
                res.size = size;
                res.offset = alignedHead;
                res.usage = ring.buffer.usage;
                res.props = ring.buffer.props;
                res.mapped = static_cast<void*>(reinterpret_cast<Uint8*>(ring.buffer.mapped) + alignedHead);
                res.fromRing = true;
                ring.head = alignedHead + size;
                return res;
            }
        }

        for (auto it = g.stagingPool.begin(); it != g.stagingPool.end(); ++it) {
            if (it->size >= size) {
                BufferResource res = *it;
                g.stagingPool.erase(it);
                res.offset = 0;
                res.fromRing = false;
                return res;
            }
        }
        BufferResource res{};
        CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, res);
        res.offset = 0;
        res.fromRing = false;
        return res;
    }

    void ReleaseStagingBuffer(BufferResource& res) {
        if (res.fromRing) {
            res = BufferResource{};
            return;
        }
        if (res.buffer == VK_NULL_HANDLE && res.memory == VK_NULL_HANDLE) return;
        if (g.frames.empty()) {
            DestroyBuffer(res);
            return;
        }
        if (g.stagingPending.empty()) {
            g.stagingPending.resize(g.frames.size());
        }
        g.stagingPending[g.currentFrame].push_back(res);
        res = BufferResource{};
    }

    void QueueBufferCopy(VkBuffer src, VkBuffer dst, const VkBufferCopy& region) {
        if (src == VK_NULL_HANDLE || dst == VK_NULL_HANDLE || region.size == 0) return;
        for (auto& batch : g.pendingBufferCopies) {
            if (batch.src == src && batch.dst == dst) {
                batch.regions.push_back(region);
                return;
            }
        }
        PendingBufferCopyBatch batch{};
        batch.src = src;
        batch.dst = dst;
        batch.regions.push_back(region);
        g.pendingBufferCopies.push_back(Move(batch));
    }

    void FlushPendingBufferCopies(VkCommandBuffer cmd) {
        if (g.pendingBufferCopies.empty()) return;

        Vector<VkBufferMemoryBarrier> barriers;
        barriers.reserve(g.pendingBufferCopies.size());

        for (auto& batch : g.pendingBufferCopies) {
            if (batch.regions.empty()) continue;
            vkCmdCopyBuffer(cmd, batch.src, batch.dst, static_cast<Uint32>(batch.regions.size()), batch.regions.data());

            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = batch.dst;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            barriers.push_back(barrier);
        }

        if (!barriers.empty()) {
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, static_cast<Uint32>(barriers.size()), barriers.data(), 0, nullptr);
        }

        g.pendingBufferCopies.clear();
    }

    void UpdateBufferFromObject(BufferResource& res, BufferObject* obj) {
        auto dataPtr = obj->GetDataReadOnly();
        if (!dataPtr || dataPtr->empty()) {
            obj->ClearDirty();
            return;
        }
        auto changeBits = obj->GetChangeBits();
        if (!(changeBits & BufferChangeBits::DirtyBit)) return;

        SizeT copySize = obj->GetSize();
        if (copySize == 0) copySize = dataPtr->size();

        const auto& dirtyRanges = obj->GetDirtyRanges();
        if (dirtyRanges.empty()) {
            Memcpy(res.mapped, dataPtr->data(), copySize);
        } else {
            for (const auto& range : dirtyRanges) {
                if (range.end <= range.start) continue;
                SizeT end = range.end;
                if (end > copySize) end = copySize;
                if (end <= range.start) continue;
                Memcpy(reinterpret_cast<Uint8*>(res.mapped) + range.start, dataPtr->data() + range.start,
                       end - range.start);
            }
        }
        obj->ClearDirty();
    }

    BufferResource& SyncBuffer(BufferObject* obj) {
        auto& set = g.buffers[obj];
        if (set.perFrame.empty()) {
            set.perFrame.resize(1);
        }
        auto& res = set.perFrame[0];

        VkDeviceSize size = obj->GetSize();
        if (size == 0) {
            auto dataPtr = obj->GetDataReadOnly();
            if (dataPtr && !dataPtr->empty()) {
                size = static_cast<VkDeviceSize>(dataPtr->size());
            }
            return res;
        }

        const auto changeBits = obj->GetChangeBits();
        Bool preferRealloc = changeBits & BufferChangeBits::PreferReallocationBit;
        Bool needCreate = res.buffer == VK_NULL_HANDLE || res.size < size || preferRealloc;
        if (needCreate) {
            if (!g.frames.empty() && (res.buffer != VK_NULL_HANDLE || res.memory != VK_NULL_HANDLE)) {
                auto& frame = *g.frames[g.currentFrame];
                frame.TrashBuffers.push_back({res.buffer, res.memory, res.mapped != nullptr});
            } else {
                DestroyBuffer(res);
            }
            res = BufferResource{};
            VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            CreateBuffer(size, usage, props, res);
        }

        if (!(changeBits & BufferChangeBits::DirtyBit)) return res;

        auto dataPtr = obj->GetDataReadOnly();
        if (!dataPtr || dataPtr->empty()) {
            obj->ClearDirty();
            return res;
        }

        if (res.mapped) {
            if (needCreate && dataPtr && !dataPtr->empty()) {
                Memcpy(res.mapped, dataPtr->data(), static_cast<SizeT>(size));
                obj->ClearDirty();
            } else {
                UpdateBufferFromObject(res, obj);
            }
            return res;
        }

        Vector<Range1D> ranges = obj->GetDirtyRanges();
        if (ranges.empty()) {
            ranges.push_back({0, static_cast<SizeT>(size)});
        }

        VkDeviceSize totalSize = 0;
        for (const auto& r : ranges) {
            if (r.end <= r.start) continue;
            totalSize += static_cast<VkDeviceSize>(r.end - r.start);
        }
        if (totalSize == 0) {
            obj->ClearDirty();
            return res;
        }

        BufferResource staging = AcquireStagingBuffer(totalSize);
        auto* dst = reinterpret_cast<Uint8*>(staging.mapped);
        VkDeviceSize cursor = 0;
        Vector<VkBufferCopy> regions;
        regions.reserve(ranges.size());
        for (const auto& r : ranges) {
            if (r.end <= r.start) continue;
            VkDeviceSize len = static_cast<VkDeviceSize>(r.end - r.start);
            Memcpy(dst + cursor, dataPtr->data() + r.start, static_cast<SizeT>(len));
            VkBufferCopy region{};
            region.srcOffset = staging.offset + cursor;
            region.dstOffset = r.start;
            region.size = len;
            regions.push_back(region);
            cursor += len;
        }

        if (g.frames.empty()) {
            SubmitImmediate([&](VkCommandBuffer cmd) {
                vkCmdCopyBuffer(cmd, staging.buffer, res.buffer, static_cast<Uint32>(regions.size()), regions.data());
            });
        } else {
            for (const auto& region : regions) {
                QueueBufferCopy(staging.buffer, res.buffer, region);
            }
        }

        obj->ClearDirty();
        ReleaseStagingBuffer(staging);
        return res;
    }

    void DestroyImage(TextureResource& res) {
        if (res.view != VK_NULL_HANDLE) {
            vkDestroyImageView(g.ctx->GetDevice(), res.view, nullptr);
            res.view = VK_NULL_HANDLE;
        }
        if (!res.mipViews.empty()) {
            for (auto& [_, view] : res.mipViews) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(g.ctx->GetDevice(), view, nullptr);
            }
            res.mipViews.clear();
        }
        if (res.image != VK_NULL_HANDLE) {
            vkDestroyImage(g.ctx->GetDevice(), res.image, nullptr);
            res.image = VK_NULL_HANDLE;
        }
        if (res.memory != VK_NULL_HANDLE) {
            vkFreeMemory(g.ctx->GetDevice(), res.memory, nullptr);
            res.memory = VK_NULL_HANDLE;
        }
        res.format = VK_FORMAT_UNDEFINED;
        res.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        res.extent = {0, 0, 1};
        res.mipLevels = 1;
        res.valid = false;
    }

    void TrashImageResource(TextureResource& res) {
        if (res.view == VK_NULL_HANDLE && res.image == VK_NULL_HANDLE && res.memory == VK_NULL_HANDLE &&
            res.mipViews.empty()) {
            return;
        }
        auto& frame = *g.frames[g.currentFrame];
        TrashImage trash{};
        trash.view = res.view;
        trash.image = res.image;
        trash.memory = res.memory;
        if (!res.mipViews.empty()) {
            trash.mipViews.reserve(res.mipViews.size());
            for (auto& [_, view] : res.mipViews) {
                if (view != VK_NULL_HANDLE) trash.mipViews.push_back(view);
            }
        }
        frame.TrashImages.push_back(Move(trash));

        res.view = VK_NULL_HANDLE;
        res.image = VK_NULL_HANDLE;
        res.memory = VK_NULL_HANDLE;
        res.mipViews.clear();
        res.format = VK_FORMAT_UNDEFINED;
        res.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        res.extent = {0, 0, 1};
        res.mipLevels = 1;
        res.valid = false;
    }

    void DestroyRenderbuffer(RenderbufferResource& res) {
        if (res.view != VK_NULL_HANDLE) {
            vkDestroyImageView(g.ctx->GetDevice(), res.view, nullptr);
            res.view = VK_NULL_HANDLE;
        }
        if (res.image != VK_NULL_HANDLE) {
            vkDestroyImage(g.ctx->GetDevice(), res.image, nullptr);
            res.image = VK_NULL_HANDLE;
        }
        if (res.memory != VK_NULL_HANDLE) {
            vkFreeMemory(g.ctx->GetDevice(), res.memory, nullptr);
            res.memory = VK_NULL_HANDLE;
        }
        res.format = VK_FORMAT_UNDEFINED;
        res.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        res.extent = {0, 0};
        res.valid = false;
    }

    VkFormat ToVkFormat(TextureInternalFormat format) {
        switch (format) {
        case TextureInternalFormat::R8:
            return VK_FORMAT_R8_UNORM;
        case TextureInternalFormat::R8Snorm:
            return VK_FORMAT_R8_SNORM;
        case TextureInternalFormat::R8I:
            return VK_FORMAT_R8_SINT;
        case TextureInternalFormat::R8UI:
            return VK_FORMAT_R8_UINT;
        case TextureInternalFormat::RG8:
            return VK_FORMAT_R8G8_UNORM;
        case TextureInternalFormat::RG8Snorm:
            return VK_FORMAT_R8G8_SNORM;
        case TextureInternalFormat::RG8I:
            return VK_FORMAT_R8G8_SINT;
        case TextureInternalFormat::RG8UI:
            return VK_FORMAT_R8G8_UINT;
        case TextureInternalFormat::RGB8:
            return VK_FORMAT_R8G8B8_UNORM;
        case TextureInternalFormat::RGB8Snorm:
            return VK_FORMAT_R8G8B8_SNORM;
        case TextureInternalFormat::RGBA8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureInternalFormat::RGBA8Snorm:
            return VK_FORMAT_R8G8B8A8_SNORM;
        case TextureInternalFormat::SRGB8:
            return VK_FORMAT_R8G8B8_SRGB;
        case TextureInternalFormat::SRGB8Alpha8:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case TextureInternalFormat::R16F:
            return VK_FORMAT_R16_SFLOAT;
        case TextureInternalFormat::RG16F:
            return VK_FORMAT_R16G16_SFLOAT;
        case TextureInternalFormat::RGB16F:
            return VK_FORMAT_R16G16B16_SFLOAT;
        case TextureInternalFormat::RGBA16F:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureInternalFormat::R32F:
            return VK_FORMAT_R32_SFLOAT;
        case TextureInternalFormat::RG32F:
            return VK_FORMAT_R32G32_SFLOAT;
        case TextureInternalFormat::RGBA32F:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case TextureInternalFormat::DepthComponent16:
            return VK_FORMAT_D16_UNORM;
        case TextureInternalFormat::DepthComponent24:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case TextureInternalFormat::DepthComponent32F:
            return VK_FORMAT_D32_SFLOAT;
        case TextureInternalFormat::Depth24Stencil8:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case TextureInternalFormat::Depth32FStencil8:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
        default:
            return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    VkImageAspectFlags AspectForFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    Bool IsDepthFormat(VkFormat format) {
        return (AspectForFormat(format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    }

    VkImageAspectFlags SampleAspectForFormat(VkFormat format) {
        if (IsDepthFormat(format)) return VK_IMAGE_ASPECT_DEPTH_BIT;
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }

    VkImageLayout SampledLayoutForFormat(VkFormat format) {
        if (IsDepthFormat(format)) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkImageUsageFlags UsageForTextureFormat(VkFormat format) {
        VkImageUsageFlags usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (IsDepthFormat(format)) {
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        } else {
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        return usage;
    }

    VkImageUsageFlags UsageForRenderbufferFormat(VkFormat format) {
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (IsDepthFormat(format)) {
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        } else {
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        return usage;
    }

    VkImageLayout LayoutForFramebufferAttachment(FramebufferAttachmentType type, VkFormat format) {
        if (IsDepthFormat(format) || type == FramebufferAttachmentType::Depth ||
            type == FramebufferAttachmentType::Stencil) {
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    Uint64 ComputeRenderingCompatHash(const Vector<VkFormat>& colorFormats, VkFormat depthFormat,
                                      VkFormat stencilFormat) {
        Vector<Uint64> compatData;
        compatData.reserve(3 + colorFormats.size());
        compatData.push_back(static_cast<Uint64>(colorFormats.size()));
        for (auto fmt : colorFormats) {
            compatData.push_back(static_cast<Uint64>(fmt));
        }
        compatData.push_back(static_cast<Uint64>(depthFormat));
        compatData.push_back(static_cast<Uint64>(stencilFormat));
        return XXH64(compatData.data(), compatData.size() * sizeof(Uint64), 0xA11CE5EDu);
    }

    Uint64 ComputeRenderingConfigHash(const VkExtent2D& extent, const Vector<VkImageView>& colorViews,
                                      const Vector<VkImageLayout>& colorLayouts, VkImageView depthView,
                                      VkImageLayout depthLayout, VkImageView stencilView, VkImageLayout stencilLayout) {
        Vector<Uint64> hashData;
        hashData.reserve(8 + colorViews.size() * 2);
        hashData.push_back((static_cast<Uint64>(extent.width) << 32) | static_cast<Uint64>(extent.height));
        hashData.push_back(static_cast<Uint64>(colorViews.size()));
        for (SizeT i = 0; i < colorViews.size(); ++i) {
            VkImageLayout layout = (i < colorLayouts.size()) ? colorLayouts[i] : VK_IMAGE_LAYOUT_UNDEFINED;
            hashData.push_back(static_cast<Uint64>(reinterpret_cast<uintptr_t>(colorViews[i])));
            hashData.push_back(static_cast<Uint64>(layout));
        }
        hashData.push_back(static_cast<Uint64>(reinterpret_cast<uintptr_t>(depthView)));
        hashData.push_back(static_cast<Uint64>(depthLayout));
        hashData.push_back(static_cast<Uint64>(reinterpret_cast<uintptr_t>(stencilView)));
        hashData.push_back(static_cast<Uint64>(stencilLayout));
        return XXH64(hashData.data(), hashData.size() * sizeof(Uint64), 0x9A37B15Eu);
    }

    Uint64 ComputeDefaultRenderingCompatHash() {
        Vector<VkFormat> colorFormats = {g.swapchain->GetFormat()};
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
        if (g.depthFormat != VK_FORMAT_UNDEFINED) {
            VkImageAspectFlags aspect = AspectForFormat(g.depthFormat);
            if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) depthFormat = g.depthFormat;
            if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) stencilFormat = g.depthFormat;
        }
        return ComputeRenderingCompatHash(colorFormats, depthFormat, stencilFormat);
    }

    Uint64 GetActiveRenderingCompatHash() {
        if (g.activeIsDefault) return g.defaultRenderingCompatHash;
        if (g.activeBackendFBO) return g.activeBackendFBO->renderingCompatHash;
        return 0;
    }

    Bool DefaultFboColorWritesEnabled(const SharedPtr<FramebufferObject>& fbo) {
        if (!fbo) return true;
        const auto& drawBuffers = fbo->GetDrawBuffers();
        for (Uint32 i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
            if (drawBuffers[i] != FramebufferAttachmentType::None) return true;
        }
        return false;
    }

    void CreateImage(VkExtent3D extent, VkFormat format, VkImageUsageFlags usage, VkImage& image,
                     VkDeviceMemory& memory, Uint32 mipLevels = 1) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = extent;
        ici.mipLevels = mipLevels;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_VERIFY(vkCreateImage(g.ctx->GetDevice(), &ici, nullptr, &image), "vkCreateImage");

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(g.ctx->GetDevice(), image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_VERIFY(vkAllocateMemory(g.ctx->GetDevice(), &mai, nullptr, &memory), "vkAllocateMemory image");
        VK_VERIFY(vkBindImageMemory(g.ctx->GetDevice(), image, memory, 0), "vkBindImageMemory");
    }

    VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect, Uint32 baseLevel,
                                Uint32 levelCount) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = format;
        ivci.subresourceRange.aspectMask = aspect;
        ivci.subresourceRange.baseMipLevel = baseLevel;
        ivci.subresourceRange.levelCount = levelCount;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateImageView(g.ctx->GetDevice(), &ivci, nullptr, &view), "vkCreateImageView");
        return view;
    }

    VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect) {
        return CreateImageView(image, format, aspect, 0, 1);
    }

    VkImageView GetTextureViewForLevel(TextureResource& res, Uint32 level, VkImageAspectFlags aspect) {
        if (level == 0) return res.view;
        if (level >= res.mipLevels) {
            MGLOG_W("Requested mip level %u out of range (mipLevels=%u), using level 0", level, res.mipLevels);
            return res.view;
        }
        auto it = res.mipViews.find(level);
        if (it != res.mipViews.end()) return it->second;
        VkImageView view = CreateImageView(res.image, res.format, aspect, level, 1);
        res.mipViews[level] = view;
        return view;
    }

    VkImageView GetTextureViewForSampling(TextureResource& res, Uint32 level, VkImageAspectFlags aspect) {
        VkImageAspectFlags fullAspect = AspectForFormat(res.format);
        if (level == 0 && aspect == fullAspect) return res.view;
        if (level >= res.mipLevels) {
            MGLOG_W("Requested mip level %u out of range (mipLevels=%u), using level 0", level, res.mipLevels);
            return res.view;
        }
        constexpr Uint32 kSampleViewKeyFlag = 0x80000000u;
        Uint32 key = kSampleViewKeyFlag | (level & 0xFFFFu) | ((static_cast<Uint32>(aspect) & 0xFFFFu) << 16);
        auto it = res.mipViews.find(key);
        if (it != res.mipViews.end()) return it->second;
        VkImageView view = CreateImageView(res.image, res.format, aspect, level, 1);
        res.mipViews[key] = view;
        return view;
    }

    void CmdTransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                  VkImageAspectFlags aspect) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask =
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = 0;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = 0;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
                   newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = 0;
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = 0;
            srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }

        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void SubmitImmediate(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBufferAllocateInfo abci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        abci.commandPool = g.commandPool;
        abci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        abci.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VK_VERIFY(vkAllocateCommandBuffers(g.ctx->GetDevice(), &abci, &cmd), "vkAllocateCommandBuffers");

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_VERIFY(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        record(cmd);
        VK_VERIFY(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;

        if (g.timelineSemaphore != VK_NULL_HANDLE) {
            Uint64 signalValue = ++g.timelineValue;
            VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            tsi.signalSemaphoreValueCount = 1;
            tsi.pSignalSemaphoreValues = &signalValue;
            si.pNext = &tsi;
            VkSemaphore signalSem = g.timelineSemaphore;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &signalSem;

            VK_VERIFY(vkQueueSubmit(g.ctx->GetGraphicsQueue(), 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");

            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &g.timelineSemaphore;
            wi.pValues = &signalValue;
            MOBILEGL_ASSERT(g.pfnWaitSemaphores != nullptr, "vkWaitSemaphores function pointer is null");
            VK_VERIFY(g.pfnWaitSemaphores(g.ctx->GetDevice(), &wi, UINT64_MAX), "vkWaitSemaphores immediate");
        } else {
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence submitFence = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateFence(g.ctx->GetDevice(), &fci, nullptr, &submitFence), "vkCreateFence immediate");
            VK_VERIFY(vkQueueSubmit(g.ctx->GetGraphicsQueue(), 1, &si, submitFence), "vkQueueSubmit");
            VK_VERIFY(vkWaitForFences(g.ctx->GetDevice(), 1, &submitFence, VK_TRUE, UINT64_MAX),
                      "vkWaitForFences immediate");
            vkDestroyFence(g.ctx->GetDevice(), submitFence, nullptr);
        }

        vkFreeCommandBuffers(g.ctx->GetDevice(), g.commandPool, 1, &cmd);
    }

    Bool HasActiveRenderingAttachments() {
        if (g.activeExtent.width == 0 || g.activeExtent.height == 0) return false;
        for (auto view : g.activeColorViews) {
            if (view != VK_NULL_HANDLE) return true;
        }
        return g.activeDepthView != VK_NULL_HANDLE || g.activeStencilView != VK_NULL_HANDLE;
    }

    void EnsureActiveColorLayoutsForRendering(VkCommandBuffer cmd) {
        if (!g.activeIsDefault || g.activeSwapchainImageIndex == ~0u) return;
        if (g.activeSwapchainImageIndex >= g.swapchainImageLayouts.size()) return;
        const auto& images = g.swapchain->GetImages();
        if (g.activeSwapchainImageIndex >= images.size()) return;

        auto& currentLayout = g.swapchainImageLayouts[g.activeSwapchainImageIndex];
        if (currentLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) return;
        CmdTransitionImageLayout(cmd, images[g.activeSwapchainImageIndex], currentLayout,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    void LoadDynamicRenderingCommands() {
        auto device = g.ctx ? g.ctx->GetDevice() : VK_NULL_HANDLE;
        if (device == VK_NULL_HANDLE) return;

        g.pfnCmdBeginRendering =
            reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(device, "vkCmdBeginRendering"));
        g.pfnCmdEndRendering =
            reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(device, "vkCmdEndRendering"));

        if (!g.pfnCmdBeginRendering) {
            g.pfnCmdBeginRendering =
                reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
        }
        if (!g.pfnCmdEndRendering) {
            g.pfnCmdEndRendering =
                reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));
        }

        g.pfnWaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(vkGetDeviceProcAddr(device, "vkWaitSemaphores"));
        if (!g.pfnWaitSemaphores) {
            g.pfnWaitSemaphores =
                reinterpret_cast<PFN_vkWaitSemaphores>(vkGetDeviceProcAddr(device, "vkWaitSemaphoresKHR"));
        }
    }

    void EndActiveRendering() {
        if (!g.recording) return;
        auto& frame = *g.frames[g.currentFrame];
        MOBILEGL_ASSERT(g.pfnCmdEndRendering != nullptr, "vkCmdEndRendering function pointer is null");
        g.pfnCmdEndRendering(frame.CommandBuffer);
        g.recording = false;
        g.recordingRenderingConfigHash = 0;
    }

    bool RecordTransferCommands(const std::function<void(VkCommandBuffer)>& record, bool restartIfRecording) {
        if (g.frames.empty()) {
            SubmitImmediate(record);
            return false;
        }
        BeginCommandBuffer();
        auto& frame = *g.frames[g.currentFrame];
        Bool wasRecording = g.recording;
        EndActiveRendering();
        FlushPendingBufferCopies(frame.CommandBuffer);

        record(frame.CommandBuffer);

        if (wasRecording && restartIfRecording && HasActiveRenderingAttachments()) {
            EnsureActiveColorLayoutsForRendering(frame.CommandBuffer);
            BeginRendering(0, nullptr);
        }
        return true;
    }

    void EnsureImageLayout(VkImage image, VkImageLayout& currentLayout, VkImageLayout desiredLayout,
                           VkImageAspectFlags aspect) {
        if (currentLayout == desiredLayout) return;
        RecordTransferCommands(
            [&](VkCommandBuffer cmd) { CmdTransitionImageLayout(cmd, image, currentLayout, desiredLayout, aspect); },
            false);
        currentLayout = desiredLayout;
    }

    void EnsureImageLayoutForDescriptor(TextureResource& res, VkImageAspectFlags aspect, VkImageLayout desiredLayout) {
        if (res.image == VK_NULL_HANDLE) return;
        if (res.layout == desiredLayout) return;
        if (!g.cmdBufferBegun) {
            EnsureImageLayout(res.image, res.layout, desiredLayout, aspect);
            return;
        }

        auto& frame = *g.frames[g.currentFrame];
        Bool wasRecording = g.recording;
        EndActiveRendering();

        CmdTransitionImageLayout(frame.CommandBuffer, res.image, res.layout, desiredLayout, aspect);
        res.layout = desiredLayout;

        if (wasRecording && HasActiveRenderingAttachments()) {
            EnsureActiveColorLayoutsForRendering(frame.CommandBuffer);
            BeginRendering(0, nullptr);
        }
    }

    void UploadTexture(ITextureObject* tex, TextureResource& res, bool force) {
        if (!tex) return;
        if (tex->GetStorageType() != TextureStorageType::Mipmap) return;
        auto* mip = dynamic_cast<TextureObjectMipmap*>(tex);
        if (!mip) return;

        TextureUploadTarget target = TextureUploadTarget::Texture2D;
        if (tex->GetTarget() == TextureTarget::Texture2D) {
            target = TextureUploadTarget::Texture2D;
        }

        if (!force && !mip->IsStorageDirty(target, 0)) return;
        auto size = mip->GetMipmapByteSize(target, 0);
        if (size == 0) return;

        void* data = mip->MapMipmapData(target, 0);
        if (!data) return;

        BufferResource staging = AcquireStagingBuffer(size);
        Memcpy(staging.mapped, data, size);

        VkExtent3D extent = res.extent;
        VkImageAspectFlags aspect = AspectForFormat(res.format);
        VkImageLayout sampledLayout = SampledLayoutForFormat(res.format);

        RecordTransferCommands(
            [&](VkCommandBuffer cmd) {
                CmdTransitionImageLayout(cmd, res.image, res.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect);

                VkBufferImageCopy region{};
                region.bufferOffset = staging.offset;
                region.imageSubresource.aspectMask = aspect;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = extent;
                vkCmdCopyBufferToImage(cmd, staging.buffer, res.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);

                CmdTransitionImageLayout(cmd, res.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, sampledLayout, aspect);
            },
            true);

        res.layout = sampledLayout;
        mip->MarkStorageDirty(target, 0, false);

        ReleaseStagingBuffer(staging);
    }

    void CopyImageMipLevels(const TextureResource& src, TextureResource& dst) {
        VkImageAspectFlags aspect = AspectForFormat(src.format);
        VkImageLayout finalLayout =
            (src.layout != VK_IMAGE_LAYOUT_UNDEFINED) ? src.layout : SampledLayoutForFormat(src.format);

        RecordTransferCommands(
            [&](VkCommandBuffer cmd) {
                CmdTransitionImageLayout(cmd, src.image, src.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect);
                CmdTransitionImageLayout(cmd, dst.image, dst.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect);

                Uint32 copyLevels = std::min(src.mipLevels, dst.mipLevels);
                for (Uint32 level = 0; level < copyLevels; ++level) {
                    VkImageCopy region{};
                    region.srcSubresource.aspectMask = aspect;
                    region.srcSubresource.mipLevel = level;
                    region.srcSubresource.baseArrayLayer = 0;
                    region.srcSubresource.layerCount = 1;
                    region.dstSubresource = region.srcSubresource;
                    region.extent = {std::max(1u, dst.extent.width >> level), std::max(1u, dst.extent.height >> level),
                                     1};
                    vkCmdCopyImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                }

                CmdTransitionImageLayout(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, finalLayout, aspect);
            },
            true);

        dst.layout = finalLayout;
    }

    TextureResource& SyncTexture(ITextureObject* tex) {
        if (!tex || !tex->IsComplete()) return g.defaultTexture;
        auto& res = g.textures[tex];
        VkFormat fmt = ToVkFormat(tex->GetFormat());
        auto size = tex->GetBaseSize();
        VkExtent3D extent{static_cast<Uint32>(std::max(1, size.x())), static_cast<Uint32>(std::max(1, size.y())), 1};
        Uint32 mipLevels = 1;
        if (tex->GetStorageType() == TextureStorageType::Mipmap) {
            auto* mip = dynamic_cast<TextureObjectMipmap*>(tex);
            if (mip) {
                mipLevels = std::max<Uint32>(1, static_cast<Uint32>(mip->GetMipmapLevelCount()));
            }
        }

        Bool needCreate = !res.valid || res.format != fmt || res.extent.width != extent.width ||
                          res.extent.height != extent.height || res.mipLevels != mipLevels;
        if (needCreate) {
            TextureResource oldRes = res;
            Bool canCopy = oldRes.valid && oldRes.image != VK_NULL_HANDLE && oldRes.format == fmt &&
                           oldRes.extent.width == extent.width && oldRes.extent.height == extent.height &&
                           oldRes.layout != VK_IMAGE_LAYOUT_UNDEFINED;
            res = TextureResource{};
            res.format = fmt;
            res.extent = extent;
            res.mipLevels = mipLevels;
            CreateImage(extent, fmt, UsageForTextureFormat(fmt), res.image, res.memory, res.mipLevels);
            res.view = CreateImageView(res.image, fmt, AspectForFormat(fmt));
            res.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            res.valid = true;
            res.paramsVersion = tex->GetTextureParamsVersion();

            if (canCopy) {
                CopyImageMipLevels(oldRes, res);
            }
            if (oldRes.image != VK_NULL_HANDLE || oldRes.view != VK_NULL_HANDLE || oldRes.memory != VK_NULL_HANDLE) {
                TrashImageResource(oldRes);
            }
        }

        UploadTexture(tex, res, false);

        if (tex->GetTextureParamsVersion() != res.paramsVersion) {
            res.paramsVersion = tex->GetTextureParamsVersion();
        }
        return res;
    }

    RenderbufferResource& SyncRenderbuffer(RenderbufferObject* rbo) {
        auto& res = g.renderbuffers[rbo];
        if (!rbo || !rbo->IsAllocated()) return res;
        VkFormat fmt = ToVkFormat(rbo->GetInternalFormat());
        VkExtent2D extent{static_cast<Uint32>(std::max(1, rbo->GetWidth())),
                          static_cast<Uint32>(std::max(1, rbo->GetHeight()))};

        Bool needCreate =
            !res.valid || res.format != fmt || res.extent.width != extent.width || res.extent.height != extent.height;
        if (needCreate) {
            DestroyRenderbuffer(res);
            res.format = fmt;
            res.extent = extent;
            VkExtent3D imgExtent{extent.width, extent.height, 1};
            CreateImage(imgExtent, fmt, UsageForRenderbufferFormat(fmt), res.image, res.memory, 1);
            res.view = CreateImageView(res.image, fmt, AspectForFormat(fmt));
            res.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            res.valid = true;
        }
        return res;
    }

    Bool SameAttachmentObject(const FramebufferAttachmentObject& a, const FramebufferAttachmentObject& b) {
        if (a.IsTexture() && b.IsTexture()) {
            return a.GetTexture().get() == b.GetTexture().get();
        }
        if (a.IsRenderbuffer() && b.IsRenderbuffer()) {
            return a.GetRenderbuffer().get() == b.GetRenderbuffer().get();
        }
        return false;
    }

    void DestroyBackendFramebuffer(BackendFramebufferObject& backend) {
        backend.attachments.clear();
        backend.colorAttachmentFormats.clear();
        backend.attachmentIndex.fill(-1);
        backend.extent = {0, 0};
        backend.colorAttachmentCount = 0;
        backend.hasDepth = false;
        backend.depthFormat = VK_FORMAT_UNDEFINED;
        backend.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        backend.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        backend.renderingCompatHash = 0;
        backend.configHash = 0;
        backend.drawBufferAttachmentIndices.fill(-1);
    }

    Bool SyncFramebufferObject(BackendFramebufferObject& backend, const SharedPtr<FramebufferObject>& stateFBO) {
        if (!stateFBO) return false;

        if (!stateFBO->CheckCompleteness()) {
            MGLOG_E("Framebuffer %u is incomplete, skipping Vulkan sync", stateFBO->GetExternalIndex());
            return false;
        }

        const auto& allAttachments = stateFBO->GetAllAttachmentObjects();
        const auto& drawBuffers = stateFBO->GetDrawBuffers();

        const auto& depthAtt = allAttachments[static_cast<SizeT>(FramebufferAttachmentType::Depth)];
        const auto& stencilAtt = allAttachments[static_cast<SizeT>(FramebufferAttachmentType::Stencil)];
        Bool hasDepth = depthAtt.IsValid();
        Bool hasStencil = stencilAtt.IsValid();
        Bool mergeDepthStencil = hasDepth && hasStencil && SameAttachmentObject(depthAtt, stencilAtt);
        if (hasDepth && hasStencil && !mergeDepthStencil) {
            MGLOG_W("Framebuffer %u has separate depth/stencil attachments; using depth only",
                    stateFBO->GetExternalIndex());
            hasStencil = false;
        }

        Vector<AttachmentInfo> newAttachments;
        Array<Int32, static_cast<SizeT>(FramebufferAttachmentType::FramebufferAttachmentTypeCount)> newIndex = {};
        newIndex.fill(-1);
        VkExtent2D extent{0, 0};
        Bool extentSet = false;
        Bool valid = true;

        auto addAttachment = [&](FramebufferAttachmentType type, const FramebufferAttachmentObject& att) {
            if (!att.IsValid()) return;
            IntVec3 size = att.GetSize();
            if (size.x() <= 0 || size.y() <= 0) {
                MGLOG_E("Framebuffer %u attachment %d has invalid size", stateFBO->GetExternalIndex(),
                        static_cast<int>(type));
                valid = false;
                return;
            }
            if (!extentSet) {
                extent = {static_cast<Uint32>(size.x()), static_cast<Uint32>(size.y())};
                extentSet = true;
            } else if (extent.width != static_cast<Uint32>(size.x()) ||
                       extent.height != static_cast<Uint32>(size.y())) {
                MGLOG_E("Framebuffer %u attachment sizes mismatch", stateFBO->GetExternalIndex());
                valid = false;
                return;
            }

            if (att.IsTexture()) {
                auto tex = att.GetTexture();
                if (!tex || !tex->IsComplete()) {
                    MGLOG_E("Framebuffer %u texture attachment incomplete", stateFBO->GetExternalIndex());
                    valid = false;
                    return;
                }
                auto& texRes = SyncTexture(tex.get());
                VkFormat fmt = texRes.format;
                VkImageAspectFlags aspect = AspectForFormat(fmt);
                VkImageLayout finalLayout = LayoutForFramebufferAttachment(type, fmt);
                EnsureImageLayout(texRes.image, texRes.layout, finalLayout, aspect);
                VkImageView view = GetTextureViewForLevel(texRes, att.GetTextureLevel(), aspect);

                AttachmentInfo info{};
                info.type = type;
                info.view = view;
                info.format = fmt;
                info.aspect = aspect;
                info.finalLayout = finalLayout;
                info.texture = tex.get();
                newIndex[static_cast<SizeT>(type)] = static_cast<Int32>(newAttachments.size());
                newAttachments.push_back(info);
            } else if (att.IsRenderbuffer()) {
                auto rbo = att.GetRenderbuffer();
                if (!rbo || !rbo->IsAllocated()) {
                    MGLOG_E("Framebuffer %u renderbuffer attachment incomplete", stateFBO->GetExternalIndex());
                    valid = false;
                    return;
                }
                auto& rbRes = SyncRenderbuffer(rbo.get());
                if (!rbRes.valid) {
                    MGLOG_E("Framebuffer %u renderbuffer backend invalid", stateFBO->GetExternalIndex());
                    valid = false;
                    return;
                }
                VkFormat fmt = rbRes.format;
                VkImageAspectFlags aspect = AspectForFormat(fmt);
                VkImageLayout finalLayout = LayoutForFramebufferAttachment(type, fmt);
                EnsureImageLayout(rbRes.image, rbRes.layout, finalLayout, aspect);

                AttachmentInfo info{};
                info.type = type;
                info.view = rbRes.view;
                info.format = fmt;
                info.aspect = aspect;
                info.finalLayout = finalLayout;
                info.renderbuffer = rbo.get();
                newIndex[static_cast<SizeT>(type)] = static_cast<Int32>(newAttachments.size());
                newAttachments.push_back(info);
            }
        };

        for (int i = 0; i < 32; ++i) {
            FramebufferAttachmentType type =
                static_cast<FramebufferAttachmentType>(static_cast<int>(FramebufferAttachmentType::Color0) + i);
            addAttachment(type, allAttachments[static_cast<SizeT>(type)]);
        }

        if (hasDepth) {
            addAttachment(FramebufferAttachmentType::Depth, depthAtt);
        } else if (hasStencil) {
            addAttachment(FramebufferAttachmentType::Stencil, stencilAtt);
        }

        if (!valid || newAttachments.empty() || !extentSet) {
            return false;
        }

        Uint32 nEffectiveBuffers = 0;
        for (Uint32 i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
            if (drawBuffers[i] != FramebufferAttachmentType::None) nEffectiveBuffers = i + 1;
        }

        Array<Int32, FramebufferObject::MAX_DRAW_BUFFERS> drawAttachmentIndices = {};
        drawAttachmentIndices.fill(-1);
        for (Uint32 i = 0; i < nEffectiveBuffers; ++i) {
            auto buf = drawBuffers[i];
            if (buf == FramebufferAttachmentType::None) {
                continue;
            }
            Int32 idx = newIndex[static_cast<SizeT>(buf)];
            if (idx < 0) {
                MGLOG_W("Framebuffer %u draw buffer %u refers to missing attachment", stateFBO->GetExternalIndex(), i);
                continue;
            }
            drawAttachmentIndices[i] = idx;
        }

        Bool useDepthAttachment = hasDepth || hasStencil;
        if (useDepthAttachment) {
            FramebufferAttachmentType depthType =
                hasDepth ? FramebufferAttachmentType::Depth : FramebufferAttachmentType::Stencil;
            Int32 idx = newIndex[static_cast<SizeT>(depthType)];
            if (idx < 0) {
                useDepthAttachment = false;
            }
        }

        Vector<VkFormat> colorAttachmentFormats;
        colorAttachmentFormats.resize(nEffectiveBuffers, VK_FORMAT_UNDEFINED);
        for (Uint32 i = 0; i < nEffectiveBuffers; ++i) {
            Int32 idx = drawAttachmentIndices[i];
            if (idx < 0 || static_cast<SizeT>(idx) >= newAttachments.size()) continue;
            colorAttachmentFormats[i] = newAttachments[static_cast<SizeT>(idx)].format;
        }

        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkFormat stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        if (useDepthAttachment) {
            FramebufferAttachmentType depthType =
                hasDepth ? FramebufferAttachmentType::Depth : FramebufferAttachmentType::Stencil;
            Int32 idx = newIndex[static_cast<SizeT>(depthType)];
            if (idx >= 0 && static_cast<SizeT>(idx) < newAttachments.size()) {
                const auto& info = newAttachments[static_cast<SizeT>(idx)];
                depthFormat = info.format;
                if (info.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) depthAttachmentFormat = info.format;
                if (info.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) stencilAttachmentFormat = info.format;
            }
        }

        Uint64 compatHash =
            ComputeRenderingCompatHash(colorAttachmentFormats, depthAttachmentFormat, stencilAttachmentFormat);

        Vector<Uint64> hashData;
        hashData.reserve(8 + newAttachments.size() * 3 + FramebufferObject::MAX_DRAW_BUFFERS);
        hashData.push_back((static_cast<Uint64>(extent.width) << 32) | static_cast<Uint64>(extent.height));
        hashData.push_back(static_cast<Uint64>(nEffectiveBuffers));
        hashData.push_back(static_cast<Uint64>(useDepthAttachment));
        for (Uint32 i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
            hashData.push_back(static_cast<Uint64>(drawBuffers[i]));
        }
        for (const auto& info : newAttachments) {
            hashData.push_back(static_cast<Uint64>(info.type));
            hashData.push_back(static_cast<Uint64>(info.format));
            hashData.push_back(static_cast<Uint64>(reinterpret_cast<uintptr_t>(info.view)));
        }
        Uint64 newHash = XXH64(hashData.data(), hashData.size() * sizeof(Uint64), 0xF00DF00Du);

        if (backend.configHash == newHash && backend.IsValid()) {
            backend.objectVersion = stateFBO->GetObjectVersion();
            backend.colorAttachmentFormats = colorAttachmentFormats;
            backend.depthFormat = depthFormat;
            backend.depthAttachmentFormat = depthAttachmentFormat;
            backend.stencilAttachmentFormat = stencilAttachmentFormat;
            backend.renderingCompatHash = compatHash;
            backend.frontendDrawBuffers = drawBuffers;
            backend.drawBufferAttachmentIndices = drawAttachmentIndices;
            backend.frontendReadBuffer = stateFBO->GetReadBuffer();
            backend.syncedAttachmentVersions = stateFBO->GetAllFramebufferAttachmentVersions();
            return true;
        }

        DestroyBackendFramebuffer(backend);

        backend.attachments = Move(newAttachments);
        backend.attachmentIndex = Move(newIndex);
        backend.extent = extent;
        backend.colorAttachmentCount = nEffectiveBuffers;
        backend.colorAttachmentFormats = Move(colorAttachmentFormats);
        backend.hasDepth = useDepthAttachment;
        backend.depthFormat = depthFormat;
        backend.depthAttachmentFormat = depthAttachmentFormat;
        backend.stencilAttachmentFormat = stencilAttachmentFormat;
        backend.configHash = newHash;
        backend.renderingCompatHash = compatHash;
        backend.frontendDrawBuffers = drawBuffers;
        backend.drawBufferAttachmentIndices = drawAttachmentIndices;
        backend.frontendReadBuffer = stateFBO->GetReadBuffer();
        backend.syncedAttachmentVersions = stateFBO->GetAllFramebufferAttachmentVersions();
        backend.objectVersion = stateFBO->GetObjectVersion();

        return true;
    }

    Bool SyncDrawFramebuffer() {
        auto& slot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw);
        auto currentFBO = slot.GetBoundObject();
        if (!currentFBO) {
            MGLOG_E("No draw framebuffer bound");
            return false;
        }

        if (currentFBO == MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo->defaultFBO) {
            g.activeIsDefault = true;
            g.activeStateFBO = currentFBO;
            g.activeBackendFBO = nullptr;
            g.activeDefaultColorWrites = DefaultFboColorWritesEnabled(currentFBO);
            g.activeExtent = g.swapchain->GetExtent();
            g.activeSwapchainImageIndex = ~0u;
            g.activeColorAttachmentCount = 1;
            g.activeColorViews.assign(1, VK_NULL_HANDLE);
            g.activeColorLayouts.assign(1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            g.activeColorFormats.assign(1, g.swapchain->GetFormat());
            g.activeColorAttachmentIndices.assign(1, 0);

            if (!g.frames.empty()) {
                auto& frame = *g.frames[g.currentFrame];
                const auto& imageViews = g.swapchain->GetImageViews();
                if (frame.CurrentImageIndex < imageViews.size()) {
                    g.activeColorViews[0] = imageViews[frame.CurrentImageIndex];
                    g.activeSwapchainImageIndex = frame.CurrentImageIndex;
                }
            }

            g.activeHasDepth = g.depthView != VK_NULL_HANDLE;
            g.activeDepthFormat = g.depthFormat;
            g.activeDepthView = g.depthView;
            g.activeDepthLayout = g.depthView != VK_NULL_HANDLE ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                                : VK_IMAGE_LAYOUT_UNDEFINED;

            VkImageAspectFlags depthAspect =
                (g.depthFormat != VK_FORMAT_UNDEFINED) ? AspectForFormat(g.depthFormat) : 0;
            g.activeDepthAttachmentFormat = ((depthAspect & VK_IMAGE_ASPECT_DEPTH_BIT) && g.depthView != VK_NULL_HANDLE)
                                                ? g.depthFormat
                                                : VK_FORMAT_UNDEFINED;
            g.activeStencilView = ((depthAspect & VK_IMAGE_ASPECT_STENCIL_BIT) && g.depthView != VK_NULL_HANDLE)
                                      ? g.depthView
                                      : VK_NULL_HANDLE;
            g.activeStencilLayout = g.activeStencilView != VK_NULL_HANDLE
                                        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED;
            g.activeStencilAttachmentFormat =
                (g.activeStencilView != VK_NULL_HANDLE) ? g.depthFormat : VK_FORMAT_UNDEFINED;

            g.activeRenderingCompatHash = g.defaultRenderingCompatHash;
            g.activeRenderingConfigHash =
                ComputeRenderingConfigHash(g.activeExtent, g.activeColorViews, g.activeColorLayouts, g.activeDepthView,
                                           g.activeDepthLayout, g.activeStencilView, g.activeStencilLayout);
            return g.activeColorViews[0] != VK_NULL_HANDLE;
        }

        auto it = g.framebuffers.find(currentFBO);
        SharedPtr<BackendFramebufferObject> backend;
        if (it == g.framebuffers.end()) {
            backend = MakeShared<BackendFramebufferObject>();
            g.framebuffers[currentFBO] = backend;
        } else {
            backend = it->second;
        }

        if (!SyncFramebufferObject(*backend, currentFBO)) return false;

        g.activeIsDefault = false;
        g.activeStateFBO = currentFBO;
        g.activeBackendFBO = backend;
        g.activeDefaultColorWrites = true;
        g.activeSwapchainImageIndex = ~0u;
        g.activeExtent = backend->extent;
        g.activeColorAttachmentCount = backend->colorAttachmentCount;
        g.activeColorViews.assign(g.activeColorAttachmentCount, VK_NULL_HANDLE);
        g.activeColorLayouts.assign(g.activeColorAttachmentCount, VK_IMAGE_LAYOUT_UNDEFINED);
        g.activeColorFormats = backend->colorAttachmentFormats;
        g.activeColorAttachmentIndices.assign(g.activeColorAttachmentCount, -1);
        for (Uint32 i = 0; i < g.activeColorAttachmentCount; ++i) {
            Int32 idx = backend->drawBufferAttachmentIndices[i];
            g.activeColorAttachmentIndices[i] = idx;
            if (idx < 0 || static_cast<SizeT>(idx) >= backend->attachments.size()) continue;
            const auto& att = backend->attachments[static_cast<SizeT>(idx)];
            g.activeColorViews[i] = att.view;
            g.activeColorLayouts[i] = att.finalLayout;
        }

        g.activeHasDepth = backend->hasDepth;
        g.activeDepthFormat = backend->depthFormat;
        g.activeDepthAttachmentFormat = backend->depthAttachmentFormat;
        g.activeStencilAttachmentFormat = backend->stencilAttachmentFormat;
        g.activeDepthView = VK_NULL_HANDLE;
        g.activeDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        g.activeStencilView = VK_NULL_HANDLE;
        g.activeStencilLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (backend->hasDepth) {
            Int32 depthIdx = backend->attachmentIndex[static_cast<SizeT>(FramebufferAttachmentType::Depth)];
            if (depthIdx < 0) {
                depthIdx = backend->attachmentIndex[static_cast<SizeT>(FramebufferAttachmentType::Stencil)];
            }
            if (depthIdx >= 0 && static_cast<SizeT>(depthIdx) < backend->attachments.size()) {
                const auto& depthAtt = backend->attachments[static_cast<SizeT>(depthIdx)];
                if (depthAtt.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
                    g.activeDepthView = depthAtt.view;
                    g.activeDepthLayout = depthAtt.finalLayout;
                }
                if (depthAtt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
                    g.activeStencilView = depthAtt.view;
                    g.activeStencilLayout = depthAtt.finalLayout;
                }
            }
        }
        g.activeRenderingCompatHash = backend->renderingCompatHash;
        g.activeRenderingConfigHash =
            ComputeRenderingConfigHash(g.activeExtent, g.activeColorViews, g.activeColorLayouts, g.activeDepthView,
                                       g.activeDepthLayout, g.activeStencilView, g.activeStencilLayout);
        return backend->IsValid();
    }

    VkSamplerAddressMode ToVkAddressMode(SamplerWrapMode mode) {
        switch (mode) {
        case SamplerWrapMode::ClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SamplerWrapMode::MirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SamplerWrapMode::Repeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SamplerWrapMode::ClampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case SamplerWrapMode::MirrorClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }

    VkFilter ToVkFilter(SamplerFilterMode mode) {
        switch (mode) {
        case SamplerFilterMode::Nearest:
            return VK_FILTER_NEAREST;
        case SamplerFilterMode::Linear:
            return VK_FILTER_LINEAR;
        default:
            return VK_FILTER_LINEAR;
        }
    }

    VkSamplerMipmapMode ToVkMipmapMode(SamplerMipmapMode mode) {
        switch (mode) {
        case SamplerMipmapMode::Nearest:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case SamplerMipmapMode::Linear:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        case SamplerMipmapMode::None:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        default:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
    }

    VkCompareOp ToVkCompareOp(SamplerCompareFunc func) {
        switch (func) {
        case SamplerCompareFunc::Never:
            return VK_COMPARE_OP_NEVER;
        case SamplerCompareFunc::Less:
            return VK_COMPARE_OP_LESS;
        case SamplerCompareFunc::Equal:
            return VK_COMPARE_OP_EQUAL;
        case SamplerCompareFunc::LessEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case SamplerCompareFunc::Greater:
            return VK_COMPARE_OP_GREATER;
        case SamplerCompareFunc::NotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case SamplerCompareFunc::GreaterEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case SamplerCompareFunc::Always:
            return VK_COMPARE_OP_ALWAYS;
        default:
            return VK_COMPARE_OP_ALWAYS;
        }
    }

    SamplerResource& SyncSampler(SamplerObject* sampler) {
        if (!sampler) return g.defaultSampler;
        auto& res = g.samplers[sampler];
        Uint16 version = sampler->GetVersion();
        if (res.sampler != VK_NULL_HANDLE && res.version == version) return res;

        if (res.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(g.ctx->GetDevice(), res.sampler, nullptr);
            res.sampler = VK_NULL_HANDLE;
        }

        auto params = sampler->GetAllSamplerParameters();
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = ToVkFilter(params.magFilter);
        sci.minFilter = ToVkFilter(params.minFilter);
        sci.mipmapMode = ToVkMipmapMode(params.mipmapMode);
        sci.addressModeU = ToVkAddressMode(params.wrapS);
        sci.addressModeV = ToVkAddressMode(params.wrapT);
        sci.addressModeW = ToVkAddressMode(params.wrapR);
        sci.mipLodBias = params.lodBias;
        sci.minLod = params.minLod;
        sci.maxLod = params.maxLod;
        sci.anisotropyEnable = VK_FALSE;
        sci.compareEnable = params.compareMode == SamplerCompareMode::CompareToTexture ? VK_TRUE : VK_FALSE;
        sci.compareOp = ToVkCompareOp(params.compareFunc);
        VK_VERIFY(vkCreateSampler(g.ctx->GetDevice(), &sci, nullptr, &res.sampler), "vkCreateSampler");

        res.version = version;
        res.params = params;
        return res;
    }

    VkPrimitiveTopology ToVkTopology(GLenum mode) {
        switch (mode) {
        case GL_TRIANGLES:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case GL_TRIANGLE_STRIP:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case GL_TRIANGLE_FAN:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case GL_LINES:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case GL_LINE_STRIP:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case GL_POINTS:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        default:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }
    }

    VkCullModeFlags ToVkCullMode(CullFaceMode mode) {
        switch (mode) {
        case CullFaceMode::Front:
            return VK_CULL_MODE_FRONT_BIT;
        case CullFaceMode::Back:
            return VK_CULL_MODE_BACK_BIT;
        case CullFaceMode::FrontAndBack:
            return VK_CULL_MODE_FRONT_AND_BACK;
        default:
            return VK_CULL_MODE_BACK_BIT;
        }
    }

    VkCompareOp ToVkCompareOp(DepthTestFunc func) {
        switch (func) {
        case DepthTestFunc::Never:
            return VK_COMPARE_OP_NEVER;
        case DepthTestFunc::Less:
            return VK_COMPARE_OP_LESS;
        case DepthTestFunc::Equal:
            return VK_COMPARE_OP_EQUAL;
        case DepthTestFunc::LessEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case DepthTestFunc::Greater:
            return VK_COMPARE_OP_GREATER;
        case DepthTestFunc::NotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case DepthTestFunc::GreaterEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case DepthTestFunc::Always:
            return VK_COMPARE_OP_ALWAYS;
        default:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        }
    }

    VkBlendFactor ToVkBlendFactor(BlendFactor factor) {
        switch (factor) {
        case BlendFactor::Zero:
            return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:
            return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor:
            return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::ConstantColor:
            return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case BlendFactor::OneMinusConstantColor:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::ConstantAlpha:
            return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case BlendFactor::OneMinusConstantAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default:
            return VK_BLEND_FACTOR_ONE;
        }
    }

    Uint32 SizeOfDataType(DataType type) {
        switch (type) {
        case DataType::Int8:
        case DataType::Uint8:
            return 1;
        case DataType::Int16:
        case DataType::Uint16:
        case DataType::Float16:
            return 2;
        case DataType::Int32:
        case DataType::Uint32:
        case DataType::Float32:
        case DataType::Fixed32:
            return 4;
        case DataType::Float64:
            return 8;
        default:
            return 4;
        }
    }

    VkFormat ToVkVertexFormat(DataType type, int size, Bool normalized, Bool isInteger) {
        if (size < 1 || size > 4) return VK_FORMAT_UNDEFINED;

        auto pick = [&](VkFormat r, VkFormat rg, VkFormat rgb, VkFormat rgba) {
            switch (size) {
            case 1:
                return r;
            case 2:
                return rg;
            case 3:
                return rgb;
            case 4:
                return rgba;
            default:
                return VK_FORMAT_UNDEFINED;
            }
        };

        if (isInteger) {
            switch (type) {
            case DataType::Int8:
                return pick(VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT);
            case DataType::Uint8:
                return pick(VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT);
            case DataType::Int16:
                return pick(VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT,
                            VK_FORMAT_R16G16B16A16_SINT);
            case DataType::Uint16:
                return pick(VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT,
                            VK_FORMAT_R16G16B16A16_UINT);
            case DataType::Int32:
                return pick(VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT,
                            VK_FORMAT_R32G32B32A32_SINT);
            case DataType::Uint32:
                return pick(VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                            VK_FORMAT_R32G32B32A32_UINT);
            default:
                return VK_FORMAT_UNDEFINED;
            }
        }

        if (normalized) {
            switch (type) {
            case DataType::Int8:
                return pick(VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM);
            case DataType::Uint8:
                return pick(VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM);
            case DataType::Int16:
                return pick(VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM,
                            VK_FORMAT_R16G16B16A16_SNORM);
            case DataType::Uint16:
                return pick(VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM,
                            VK_FORMAT_R16G16B16A16_UNORM);
            default:
                break;
            }
        }

        switch (type) {
        case DataType::Float16:
            return pick(VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT,
                        VK_FORMAT_R16G16B16A16_SFLOAT);
        case DataType::Float32:
        case DataType::Fixed32:
            return pick(VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                        VK_FORMAT_R32G32B32A32_SFLOAT);
        case DataType::Float64:
            return pick(VK_FORMAT_R64_SFLOAT, VK_FORMAT_R64G64_SFLOAT, VK_FORMAT_R64G64B64_SFLOAT,
                        VK_FORMAT_R64G64B64A64_SFLOAT);
        case DataType::Int8:
            return pick(VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT);
        case DataType::Uint8:
            return pick(VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT);
        case DataType::Int16:
            return pick(VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT,
                        VK_FORMAT_R16G16B16A16_SINT);
        case DataType::Uint16:
            return pick(VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT,
                        VK_FORMAT_R16G16B16A16_UINT);
        case DataType::Int32:
            return pick(VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT,
                        VK_FORMAT_R32G32B32A32_SINT);
        case DataType::Uint32:
            return pick(VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                        VK_FORMAT_R32G32B32A32_UINT);
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    VkIndexType ToVkIndexType(GLenum type) {
        switch (type) {
        case GL_UNSIGNED_SHORT:
            return VK_INDEX_TYPE_UINT16;
        case GL_UNSIGNED_INT:
            return VK_INDEX_TYPE_UINT32;
        default:
            return VK_INDEX_TYPE_UINT16;
        }
    }

    Uint32 IndexTypeSize(GLenum type) {
        switch (type) {
        case GL_UNSIGNED_SHORT:
            return 2;
        case GL_UNSIGNED_INT:
            return 4;
        case GL_UNSIGNED_BYTE:
            return 1;
        default:
            return 2;
        }
    }

    Uint64 ComputeProgramSpvHash(ProgramObject* program) {
        XXH64_state_t* state = XXH64_createState();
        XXH64_reset(state, 0xC0FFEEu);
        auto& spirvs = program->GetGeneratedSpirv();
        for (const auto& spv : spirvs) {
            if (spv.empty()) continue;
            XXH64_update(state, spv.data(), spv.size() * sizeof(Uint));
        }
        Uint64 hash = XXH64_digest(state);
        XXH64_freeState(state);
        return hash;
    }

    void DestroyProgramResource(ProgramResource& res) {
        auto device = g.ctx->GetDevice();
        for (auto& kv : res.pipelines) {
            if (kv.second != VK_NULL_HANDLE) vkDestroyPipeline(device, kv.second, nullptr);
        }
        res.pipelines.clear();

        for (auto& pool : res.uboPools) {
            for (auto& buf : pool.buffers) {
                DestroyBuffer(buf);
            }
            pool.buffers.clear();
            pool.cursor = 0;
        }
        res.uboPools.clear();

        if (res.pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, res.pipelineLayout, nullptr);
            res.pipelineLayout = VK_NULL_HANDLE;
        }
        if (res.setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, res.setLayout, nullptr);
            res.setLayout = VK_NULL_HANDLE;
        }
    }

    VkShaderStageFlagBits ToVkShaderStage(ShaderStage stage) {
        switch (stage) {
        case ShaderStage::Vertex:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Geometry:
            return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::TessControl:
            return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::TessEval:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderStage::Compute:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        default:
            return VK_SHADER_STAGE_ALL_GRAPHICS;
        }
    }

    String NormalizeUniformName(const String& name) {
        auto pos = name.find('[');
        if (pos == String::npos) return name;
        return name.substr(0, pos);
    }

    Int FindUniformLocationByName(ProgramObject* program, const String& name) {
        if (!program) return -1;
        Int loc = program->GetUniformLocation(name);
        if (loc >= 0) return loc;
        String norm = NormalizeUniformName(name);
        if (norm != name) {
            loc = program->GetUniformLocation(norm);
            if (loc >= 0) return loc;
        }
        Uint maxLoc = program->GetMaxUniformLocation();
        for (Uint i = 0; i < maxLoc; ++i) {
            const String& uName = program->GetUniformName(i);
            if (NormalizeUniformName(uName) == norm) return static_cast<Int>(i);
        }
        return -1;
    }

    void ReflectProgramResources(ProgramObject* program, ProgramResource& out) {
        UnorderedMap<String, UniformBindingInfo> uniformMap;
        UnorderedMap<String, SamplerBindingInfo> samplerMap;

        auto& shaders = program->GetAttachedShaders();
        auto& spirvs = program->GetGeneratedSpirv();
        for (SizeT i = 0; i < spirvs.size(); ++i) {
            auto& spv = spirvs[i];
            if (spv.empty()) continue;

            VkShaderStageFlags stageFlag = VK_SHADER_STAGE_ALL_GRAPHICS;
            if (i < shaders.size()) stageFlag = ToVkShaderStage(shaders[i]->GetShaderStage());

            spvc_context context = nullptr;
            spvc_context_create(&context);
            spvc_parsed_ir ir = nullptr;
            spvc_context_parse_spirv(context, spv.data(), spv.size(), &ir);
            spvc_compiler compiler = nullptr;
            spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler);
            spvc_resources resources = nullptr;
            spvc_compiler_create_shader_resources(compiler, &resources);

            const spvc_reflected_resource* list = nullptr;
            size_t count = 0;

            spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &count);
            for (size_t r = 0; r < count; ++r) {
                if (spvc_compiler_has_decoration(compiler, list[r].id, SpvDecorationBuiltIn)) continue;
                String name = list[r].name ? list[r].name : "";
                Uint32 binding = spvc_compiler_get_decoration(compiler, list[r].id, SpvDecorationBinding);
                Uint32 set = spvc_compiler_get_decoration(compiler, list[r].id, SpvDecorationDescriptorSet);
                if (set != 0) continue;

                auto& info = uniformMap[name];
                info.name = name;
                info.binding = binding;
                info.set = set;
                info.stages |= stageFlag;
            }

            spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &list, &count);
            for (size_t r = 0; r < count; ++r) {
                if (spvc_compiler_has_decoration(compiler, list[r].id, SpvDecorationBuiltIn)) continue;
                String name = list[r].name ? list[r].name : "";
                String normName = NormalizeUniformName(name);
                Uint32 binding = spvc_compiler_get_decoration(compiler, list[r].id, SpvDecorationBinding);
                Uint32 set = spvc_compiler_get_decoration(compiler, list[r].id, SpvDecorationDescriptorSet);
                if (set != 0) continue;

                auto& info = samplerMap[normName];
                info.name = normName;
                info.binding = binding;
                info.set = set;
                info.stages |= stageFlag;
            }

            spvc_context_destroy(context);
        }

        out.uniformBindings.clear();
        out.samplerBindings.clear();
        out.samplerBindingByName.clear();

        for (auto& [name, info] : uniformMap) {
            if (name == MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME) {
                out.uboBinding = static_cast<Int32>(info.binding);
                continue;
            }
            auto blockIndex = program->GetUniformBlockIndex(name.c_str());
            info.blockIndex = blockIndex;
            out.uniformBindings.push_back(info);
        }

        for (auto& [name, info] : samplerMap) {
            out.samplerBindings.push_back(info);
            out.samplerBindingByName[name] = info.binding;
        }
    }

    constexpr Uint32 kBaseDescriptorPoolSets = 256;

    DescriptorPoolConfig MakeDescriptorPoolConfig(Uint32 requiredUniform, Uint32 requiredSampler) {
        DescriptorPoolConfig cfg{};
        cfg.maxSets = kBaseDescriptorPoolSets;
        cfg.uniformCount = std::max(kBaseDescriptorPoolSets, requiredUniform);
        cfg.samplerCount = std::max(kBaseDescriptorPoolSets, requiredSampler);
        return cfg;
    }

    VkDescriptorPool CreateDescriptorPool(const DescriptorPoolConfig& cfg) {
        VkDescriptorPoolSize sizes[2] = {};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = cfg.uniformCount;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = cfg.samplerCount;

        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = cfg.maxSets;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateDescriptorPool(g.ctx->GetDevice(), &dpci, nullptr, &pool), "vkCreateDescriptorPool");
        return pool;
    }

    void DestroyDescriptorPools() {
        auto device = g.ctx->GetDevice();
        for (auto& framePools : g.frameDescriptorPools) {
            for (auto pool : framePools.pools) {
                if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
            }
            framePools.pools.clear();
            framePools.activePool = 0;
            framePools.config = {};
        }
        g.frameDescriptorPools.clear();
    }

    void ResetFrameDescriptorPools(Uint32 frameIndex) {
        if (frameIndex >= g.frameDescriptorPools.size()) return;
        auto& framePools = g.frameDescriptorPools[frameIndex];
        for (auto pool : framePools.pools) {
            if (pool != VK_NULL_HANDLE) {
                VK_VERIFY(vkResetDescriptorPool(g.ctx->GetDevice(), pool, 0), "vkResetDescriptorPool");
            }
        }
        framePools.activePool = 0;
    }

    void ResetProgramUboPools(Uint32 frameIndex) {
        for (auto& [_, prog] : g.programs) {
            if (frameIndex < prog.uboPools.size()) {
                prog.uboPools[frameIndex].cursor = 0;
            }
        }
    }

    VkDescriptorSet AllocateDescriptorSetForDraw(ProgramResource& prog, Uint32 frameIndex) {
        if (prog.setLayout == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        if (frameIndex >= g.frameDescriptorPools.size()) return VK_NULL_HANDLE;

        auto& framePools = g.frameDescriptorPools[frameIndex];
        if (framePools.config.maxSets == 0) {
            framePools.config = MakeDescriptorPoolConfig(prog.uniformDescriptorCount, prog.samplerDescriptorCount);
        }

        auto ensurePool = [&]() {
            if (framePools.activePool < framePools.pools.size()) return;
            DescriptorPoolConfig cfg = framePools.config;
            cfg.uniformCount = std::max(cfg.uniformCount, prog.uniformDescriptorCount);
            cfg.samplerCount = std::max(cfg.samplerCount, prog.samplerDescriptorCount);
            framePools.pools.push_back(CreateDescriptorPool(cfg));
        };

        for (;;) {
            ensurePool();
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorPool pool = framePools.pools[framePools.activePool];
            VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsai.descriptorPool = pool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &prog.setLayout;
            VkResult res = vkAllocateDescriptorSets(g.ctx->GetDevice(), &dsai, &set);
            if (res == VK_SUCCESS) return set;
            if (res == VK_ERROR_OUT_OF_POOL_MEMORY || res == VK_ERROR_FRAGMENTED_POOL) {
                framePools.activePool++;
                if (framePools.activePool >= framePools.pools.size()) {
                    framePools.config.maxSets *= 2;
                    framePools.config.uniformCount =
                        std::max(framePools.config.uniformCount * 2, prog.uniformDescriptorCount);
                    framePools.config.samplerCount =
                        std::max(framePools.config.samplerCount * 2, prog.samplerDescriptorCount);
                }
                continue;
            }
            VK_VERIFY(res, "vkAllocateDescriptorSets");
            return VK_NULL_HANDLE;
        }
    }

    void CreateNullUbo() {
        if (g.nullUbo.buffer != VK_NULL_HANDLE) return;
        CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g.nullUbo);
        Memset(g.nullUbo.mapped, 0, 256);
    }

    VkManager::ShaderTransformFlags GetShaderTransformFlags() {
        VkManager::ShaderTransformFlags flags = VkManager::ShaderTransformBit::PositionZRemap;
        const auto& currentDrawFBO =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        if (currentDrawFBO == MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo->defaultFBO) {
            flags |= VkManager::ShaderTransformBit::PositionYFlip;
        }
        return flags;
    }

    ProgramResource& GetProgramResource(ProgramObject* program) {
        auto& slot = g.programs[program];
        Uint64 newHash = ComputeProgramSpvHash(program);
        if (slot.program && slot.spvHash == newHash) return slot;

        if (slot.program) {
            DestroyProgramResource(slot);
            slot = ProgramResource{};
        }

        slot.program = program;
        slot.spvHash = newHash;

        VkManager::ShaderTransformFlags transformFlags = GetShaderTransformFlags();
        if (!g.programMgr) g.programMgr = MakeUnique<DV::ProgramManager>(*g.ctx);
        slot.shaderStages = &g.programMgr->CreatePipelineShaderStages(program, transformFlags);
        slot.programHash = g.programMgr->ComputeProgramHash(program, transformFlags);

        ReflectProgramResources(program, slot);

        Vector<VkDescriptorSetLayoutBinding> bindings;
        if (slot.uboBinding >= 0 && program->GetUBOSize() > 0) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = static_cast<Uint32>(slot.uboBinding);
            b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(b);
            slot.uboSize = program->GetUBOSize();
        }

        for (auto& ub : slot.uniformBindings) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = ub.binding;
            b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b.descriptorCount = 1;
            b.stageFlags = ub.stages ? ub.stages : VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(b);
        }

        for (auto& sb : slot.samplerBindings) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = sb.binding;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags = sb.stages ? sb.stages : VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(b);
        }

        if (!bindings.empty()) {
            std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) {
                if (a.binding != b.binding) return a.binding < b.binding;
                return a.descriptorType < b.descriptorType;
            });

            bindings.erase(std::unique(bindings.begin(), bindings.end(),
                                       [](const auto& a, const auto& b) {
                                           return a.binding == b.binding && a.descriptorType == b.descriptorType;
                                       }),
                           bindings.end());

            VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dlci.bindingCount = bindings.size();
            dlci.pBindings = bindings.data();
            VK_VERIFY(vkCreateDescriptorSetLayout(g.ctx->GetDevice(), &dlci, nullptr, &slot.setLayout),
                      "vkCreateDescriptorSetLayout");
        }

        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if (slot.setLayout != VK_NULL_HANDLE) {
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &slot.setLayout;
        }
        VK_VERIFY(vkCreatePipelineLayout(g.ctx->GetDevice(), &plci, nullptr, &slot.pipelineLayout),
                  "vkCreatePipelineLayout");

        CreateNullUbo();

        slot.uniformDescriptorCount =
            static_cast<Uint32>(slot.uniformBindings.size() + ((slot.uboSize > 0 && slot.uboBinding >= 0) ? 1 : 0));
        slot.samplerDescriptorCount = static_cast<Uint32>(slot.samplerBindings.size());

        if (slot.uboSize > 0 && slot.uboBinding >= 0) {
            slot.uboPools.resize(g.frames.size());
            for (SizeT i = 0; i < g.frames.size(); ++i) {
                BufferResource ubo{};
                CreateBuffer(slot.uboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ubo);
                slot.uboPools[i].buffers.push_back(ubo);
            }
        }

        return slot;
    }

    Uint64 HashVertexLayout(const VertexArrayObject* vao) {
        VertexAttribKey keys[VertexArrayObject::MAX_VERTEX_ATTRIBS];
        Memset(keys, 0, sizeof(keys));
        if (!vao) return XXH64(keys, sizeof(keys), 0xBADC0FFEEu);

        for (Uint32 i = 0; i < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
            const auto& attr = vao->GetAttribute(i);
            auto& key = keys[i];
            key.enabled = attr.Enabled ? 1 : 0;
            key.size = static_cast<Uint8>(attr.Size);
            key.type = static_cast<Uint8>(attr.Type);
            key.normalized = attr.Normalized ? 1 : 0;
            key.isInteger = attr.IsInteger ? 1 : 0;
            key.divisor = static_cast<Uint8>(attr.Divisor);
            key.stride = static_cast<Uint32>(attr.Stride);
            key.offset = static_cast<Uint64>(attr.Offset);
        }

        return XXH64(keys, sizeof(keys), 0xBADC0FFEEu);
    }

    VkPipeline GetOrCreatePipeline(ProgramResource& prog, const VertexArrayObject* vao, const RenderStateParameters& rs,
                                   VkPrimitiveTopology topology, const Vector<VkFormat>& colorAttachmentFormats,
                                   VkFormat depthAttachmentFormat, VkFormat stencilAttachmentFormat,
                                   Uint64 renderingKey) {
        if (!g.programMgr) g.programMgr = MakeUnique<DV::ProgramManager>(*g.ctx);
        prog.shaderStages = &g.programMgr->CreatePipelineShaderStages(prog.program, GetShaderTransformFlags());
        if (!prog.shaderStages || prog.shaderStages->empty()) {
            MGLOG_E("Vulkan: shader stages are empty for program %u (hash=%llu)", prog.program->GetExternalIndex(),
                    static_cast<unsigned long long>(prog.programHash));
            return VK_NULL_HANDLE;
        }
        struct PipelineKey {
            Uint64 programHash = 0;
            Uint64 vertexHash = 0;
            Uint32 topology = 0;
            Uint32 blendEnable = 0;
            Uint32 depthTest = 0;
            Uint32 depthWrite = 0;
            Uint32 depthFunc = 0;
            Uint32 cullEnable = 0;
            Uint32 cullMode = 0;
            Uint32 colorMask = 0;
            Uint32 srcRGB = 0;
            Uint32 dstRGB = 0;
            Uint32 srcA = 0;
            Uint32 dstA = 0;
            Uint64 blendHash = 0;
            Uint64 renderingKey = 0;
            Uint32 colorAttachments = 0;
            Uint32 depthFormat = 0;
            Uint32 stencilFormat = 0;
        } key{};

        DEBUG_TRACE_POINT();
        key.programHash = prog.programHash;
        key.vertexHash = HashVertexLayout(vao);
        key.topology = topology;
        key.blendEnable = rs.BlendStates[0].Enabled ? 1u : 0u;
        key.depthTest = rs.DepthTestEnabled ? 1u : 0u;
        key.depthWrite = rs.DepthMask ? 1u : 0u;
        key.depthFunc = static_cast<Uint32>(rs.DepthFunc);
        key.cullEnable = 0;
        key.cullMode = 0;
        key.colorMask = (rs.ColorMask.x() ? 1u : 0u) | (rs.ColorMask.y() ? 2u : 0u) | (rs.ColorMask.z() ? 4u : 0u) |
                        (rs.ColorMask.w() ? 8u : 0u);
        key.srcRGB = static_cast<Uint32>(rs.BlendStates[0].SrcFactorRGB);
        key.dstRGB = static_cast<Uint32>(rs.BlendStates[0].DstFactorRGB);
        key.srcA = static_cast<Uint32>(rs.BlendStates[0].SrcFactorAlpha);
        key.dstA = static_cast<Uint32>(rs.BlendStates[0].DstFactorAlpha);
        key.blendHash = XXH64(rs.BlendStates.data(), sizeof(rs.BlendStates), 0xBEEFCAFEu);
        key.renderingKey = renderingKey;
        key.colorAttachments = static_cast<Uint32>(colorAttachmentFormats.size());
        key.depthFormat = static_cast<Uint32>(depthAttachmentFormat);
        key.stencilFormat = static_cast<Uint32>(stencilAttachmentFormat);

        DEBUG_TRACE_POINT();
        Uint64 hash = XXH64(&key, sizeof(key), 0x12345678u);
        auto it = prog.pipelines.find(hash);
        if (it != prog.pipelines.end()) return it->second;

        DEBUG_TRACE_POINT();
        Vector<VkVertexInputBindingDescription> bindings;
        Vector<VkVertexInputAttributeDescription> attrs;
        if (vao) {
            for (Uint32 i = 0; i < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
                const auto& attr = vao->GetAttribute(i);
                if (!attr.Enabled || !attr.Buffer) continue;

                VkVertexInputBindingDescription b{};
                b.binding = i;
                b.stride = attr.Stride > 0 ? attr.Stride : (attr.Size * SizeOfDataType(attr.Type));
                b.inputRate = attr.Divisor > 0 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
                bindings.push_back(b);

                VkVertexInputAttributeDescription a{};
                a.location = i;
                a.binding = i;
                a.format = ToVkVertexFormat(attr.Type, attr.Size, attr.Normalized, attr.IsInteger);
                a.offset = static_cast<Uint32>(attr.Offset);
                if (a.format != VK_FORMAT_UNDEFINED) {
                    attrs.push_back(a);
                }
            }
        }

        DEBUG_TRACE_POINT();
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = bindings.size();
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = attrs.size();
        vertexInput.pVertexAttributeDescriptions = attrs.data();

        DEBUG_TRACE_POINT();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = topology;
        ia.primitiveRestartEnable = VK_FALSE;

        DEBUG_TRACE_POINT();
        VkPipelineViewportStateCreateInfo vpci{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vpci.viewportCount = 1;
        vpci.scissorCount = 1;

        DEBUG_TRACE_POINT();
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        DEBUG_TRACE_POINT();
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        DEBUG_TRACE_POINT();
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        Bool hasDepthAttachment = depthAttachmentFormat != VK_FORMAT_UNDEFINED;
        Bool hasStencilAttachment = stencilAttachmentFormat != VK_FORMAT_UNDEFINED;
        Bool hasDepthStencilAttachment = hasDepthAttachment || hasStencilAttachment;
        depth.depthTestEnable = (hasDepthAttachment && rs.DepthTestEnabled) ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = (hasDepthAttachment && rs.DepthMask) ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = ToVkCompareOp(rs.DepthFunc);
        depth.stencilTestEnable = VK_FALSE;

        DEBUG_TRACE_POINT();
        Vector<VkPipelineColorBlendAttachmentState> colorAtts;
        Uint32 colorAttachmentCount = static_cast<Uint32>(colorAttachmentFormats.size());
        if (colorAttachmentCount > 0) {
            colorAtts.resize(colorAttachmentCount);
            for (Uint32 i = 0; i < colorAttachmentCount; ++i) {
                const auto& bs = rs.BlendStates[i];
                VkPipelineColorBlendAttachmentState ca{};
                ca.colorWriteMask = (rs.ColorMask.x() ? VK_COLOR_COMPONENT_R_BIT : 0) |
                                    (rs.ColorMask.y() ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                    (rs.ColorMask.z() ? VK_COLOR_COMPONENT_B_BIT : 0) |
                                    (rs.ColorMask.w() ? VK_COLOR_COMPONENT_A_BIT : 0);
                ca.blendEnable = bs.Enabled ? VK_TRUE : VK_FALSE;
                ca.srcColorBlendFactor = ToVkBlendFactor(bs.SrcFactorRGB);
                ca.dstColorBlendFactor = ToVkBlendFactor(bs.DstFactorRGB);
                ca.colorBlendOp = VK_BLEND_OP_ADD;
                ca.srcAlphaBlendFactor = ToVkBlendFactor(bs.SrcFactorAlpha);
                ca.dstAlphaBlendFactor = ToVkBlendFactor(bs.DstFactorAlpha);
                ca.alphaBlendOp = VK_BLEND_OP_ADD;
                colorAtts[i] = ca;
            }
        }

        DEBUG_TRACE_POINT();
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = colorAtts.size();
        blend.pAttachments = colorAtts.empty() ? nullptr : colorAtts.data();

        DEBUG_TRACE_POINT();
        VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamics;

        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = colorAttachmentCount;
        rendering.pColorAttachmentFormats = colorAttachmentFormats.empty() ? nullptr : colorAttachmentFormats.data();
        rendering.depthAttachmentFormat = depthAttachmentFormat;
        rendering.stencilAttachmentFormat = stencilAttachmentFormat;

        DEBUG_TRACE_POINT();
        VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi.pNext = &rendering;
        gpi.stageCount = prog.shaderStages ? prog.shaderStages->size() : 0;
        gpi.pStages = prog.shaderStages ? prog.shaderStages->data() : nullptr;
        gpi.pVertexInputState = &vertexInput;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vpci;
        gpi.pRasterizationState = &raster;
        gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = hasDepthStencilAttachment ? &depth : nullptr;
        gpi.pColorBlendState = &blend;
        gpi.pDynamicState = &dyn;
        gpi.layout = prog.pipelineLayout;
        gpi.renderPass = VK_NULL_HANDLE;
        gpi.subpass = 0;

        DEBUG_TRACE_POINT();
        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateGraphicsPipelines(g.ctx->GetDevice(), VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline),
                  "vkCreateGraphicsPipelines");

        DEBUG_TRACE_POINT();
        prog.pipelines.emplace(hash, pipeline);
        return pipeline;
    }

    VkDescriptorSet UpdateDescriptorSetsForDraw(ProgramResource& prog, Uint32 frameIndex) {
        if (prog.setLayout == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        VkDescriptorSet activeSet = AllocateDescriptorSetForDraw(prog, frameIndex);
        if (activeSet == VK_NULL_HANDLE) return VK_NULL_HANDLE;

        DEBUG_TRACE_POINT();
        Vector<VkWriteDescriptorSet> writes;
        Vector<VkDescriptorBufferInfo> bufferInfos;
        Vector<VkDescriptorImageInfo> imageInfos;

        DEBUG_TRACE_POINT();
        bufferInfos.reserve(prog.uniformBindings.size() + (prog.uboSize > 0 ? 1 : 0));
        imageInfos.reserve(prog.samplerBindings.size());
        writes.reserve(bufferInfos.capacity() + imageInfos.capacity());

        DEBUG_TRACE_POINT();
        if (prog.uboSize > 0 && prog.uboBinding >= 0) {
            if (frameIndex >= prog.uboPools.size()) return VK_NULL_HANDLE;
            DEBUG_TRACE_POINT();
            auto& pool = prog.uboPools[frameIndex];
            if (pool.cursor >= pool.buffers.size()) {
                BufferResource ubo{};
                CreateBuffer(prog.uboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ubo);
                pool.buffers.push_back(ubo);
            }
            BufferResource& ubo = pool.buffers[pool.cursor++];
            Memcpy(ubo.mapped, prog.program->MapUBO(), prog.uboSize);

            DEBUG_TRACE_POINT();
            VkDescriptorBufferInfo info{};
            info.buffer = ubo.buffer;
            info.offset = 0;
            info.range = prog.uboSize;
            bufferInfos.push_back(info);

            DEBUG_TRACE_POINT();
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = activeSet;
            w.dstBinding = static_cast<Uint32>(prog.uboBinding);
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.descriptorCount = 1;
            w.pBufferInfo = &bufferInfos.back();
            writes.push_back(w);
        }
        DEBUG_TRACE_POINT();

        for (const auto& ub : prog.uniformBindings) {
            DEBUG_TRACE_POINT();
            if (ub.blockIndex == 0xFFFFFFFFu) continue;
            DEBUG_TRACE_POINT();
            Int bindingPoint = prog.program->GetUniformBlockBinding(ub.blockIndex);
            if (bindingPoint < 0) continue;

            DEBUG_TRACE_POINT();
            auto& slot = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::Uniform, bindingPoint);
            auto bufferObj = slot.GetBoundObject();
            BufferResource* res = &g.nullUbo;
            VkDeviceSize offset = 0;
            VkDeviceSize range = g.nullUbo.size;

            DEBUG_TRACE_POINT();
            if (bufferObj) {
                DEBUG_TRACE_POINT();
                auto& synced = SyncBuffer(bufferObj.get());
                res = &synced;
                auto r = slot.GetRange();
                offset = r.start;
                if (r.end == ~0u || r.end <= r.start) {
                    range = synced.size > offset ? synced.size - offset : VK_WHOLE_SIZE;
                } else {
                    range = r.end - r.start;
                }
            }

            DEBUG_TRACE_POINT();
            VkDescriptorBufferInfo info{};
            info.buffer = res->buffer;
            info.offset = offset;
            info.range = range;
            bufferInfos.push_back(info);

            DEBUG_TRACE_POINT();
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = activeSet;
            w.dstBinding = ub.binding;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.descriptorCount = 1;
            w.pBufferInfo = &bufferInfos.back();
            writes.push_back(w);
        }

        DEBUG_TRACE_POINT();
        for (const auto& sb : prog.samplerBindings) {
            DEBUG_TRACE_POINT();
            Int loc = FindUniformLocationByName(prog.program, sb.name);
            Int unit = (loc >= 0) ? prog.program->GetUniformSamplerOrImageUnitIndex(loc) : -1;
            if (unit < 0) unit = 0;

            DEBUG_TRACE_POINT();
            auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
            SharedPtr<ITextureObject> texObj = nullptr;
            auto& slot2d = textureUnit.GetBindingSlot(TextureTarget::Texture2D);
            texObj = slot2d.GetBoundObject();
            if (!texObj) {
                DEBUG_TRACE_POINT();
                for (auto& slot : textureUnit.GetAllBindingSlots()) {
                    DEBUG_TRACE_POINT();
                    if (slot.GetBoundObject()) {
                        DEBUG_TRACE_POINT();
                        texObj = slot.GetBoundObject();
                        break;
                    }
                }
            }

            DEBUG_TRACE_POINT();
            TextureResource& texRes = texObj ? SyncTexture(texObj.get()) : g.defaultTexture;
            VkImageView sampleView = texRes.view;
            VkImageLayout sampleLayout = SampledLayoutForFormat(texRes.format);
            if (texRes.image != VK_NULL_HANDLE) {
                VkImageAspectFlags aspect = SampleAspectForFormat(texRes.format);
                EnsureImageLayoutForDescriptor(texRes, aspect, sampleLayout);
                sampleView = GetTextureViewForSampling(texRes, 0, aspect);
            }
            SamplerObject* samplerObj = nullptr;
            if (auto s = textureUnit.GetSamplerObject()) samplerObj = s.get();
            if (!samplerObj && texObj) samplerObj = texObj->GetSamplerObject().get();
            SamplerResource& sampRes = SyncSampler(samplerObj);

            DEBUG_TRACE_POINT();
            VkDescriptorImageInfo info{};
            info.sampler = sampRes.sampler != VK_NULL_HANDLE ? sampRes.sampler : g.defaultSampler.sampler;
            info.imageView = sampleView;
            info.imageLayout = sampleLayout;
            imageInfos.push_back(info);

            DEBUG_TRACE_POINT();
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = activeSet;
            w.dstBinding = sb.binding;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.descriptorCount = 1;
            w.pImageInfo = &imageInfos.back();
            writes.push_back(w);
        }

        DEBUG_TRACE_POINT();
        if (!writes.empty()) {
            DEBUG_TRACE_POINT();
            vkUpdateDescriptorSets(g.ctx->GetDevice(), writes.size(), writes.data(), 0, nullptr);
        }
        return activeSet;
    }

    void CreateDefaultResources() {
        if (!g.defaultSampler.sampler) {
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            VK_VERIFY(vkCreateSampler(g.ctx->GetDevice(), &sci, nullptr, &g.defaultSampler.sampler),
                      "vkCreateSampler default");
        }

        if (!g.defaultTexture.valid) {
            g.defaultTexture.format = VK_FORMAT_R8G8B8A8_UNORM;
            g.defaultTexture.extent = {1, 1, 1};
            g.defaultTexture.mipLevels = 1;
            CreateImage(g.defaultTexture.extent, g.defaultTexture.format,
                        UsageForTextureFormat(g.defaultTexture.format), g.defaultTexture.image, g.defaultTexture.memory,
                        g.defaultTexture.mipLevels);
            g.defaultTexture.view =
                CreateImageView(g.defaultTexture.image, g.defaultTexture.format, VK_IMAGE_ASPECT_COLOR_BIT);
            g.defaultTexture.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            g.defaultTexture.valid = true;

            Uint32 pixel = 0xFFFFFFFFu;
            BufferResource staging = AcquireStagingBuffer(sizeof(pixel));
            Memcpy(staging.mapped, &pixel, sizeof(pixel));

            RecordTransferCommands(
                [&](VkCommandBuffer cmd) {
                    CmdTransitionImageLayout(cmd, g.defaultTexture.image, g.defaultTexture.layout,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

                    VkBufferImageCopy region{};
                    region.bufferOffset = staging.offset;
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.mipLevel = 0;
                    region.imageSubresource.baseArrayLayer = 0;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = {1, 1, 1};
                    vkCmdCopyBufferToImage(cmd, staging.buffer, g.defaultTexture.image,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                    CmdTransitionImageLayout(cmd, g.defaultTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
                },
                false);

            g.defaultTexture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ReleaseStagingBuffer(staging);
        }
    }

    VkFormat FindDepthFormat() {
        VkFormat candidates[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT,
                                 VK_FORMAT_D16_UNORM};
        for (auto fmt : candidates) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(g.ctx->GetPhysicalDevice(), fmt, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                return fmt;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    void DestroyDepthResources() {
        if (g.depthView != VK_NULL_HANDLE) {
            vkDestroyImageView(g.ctx->GetDevice(), g.depthView, nullptr);
            g.depthView = VK_NULL_HANDLE;
        }
        if (g.depthImage != VK_NULL_HANDLE) {
            vkDestroyImage(g.ctx->GetDevice(), g.depthImage, nullptr);
            g.depthImage = VK_NULL_HANDLE;
        }
        if (g.depthMemory != VK_NULL_HANDLE) {
            vkFreeMemory(g.ctx->GetDevice(), g.depthMemory, nullptr);
            g.depthMemory = VK_NULL_HANDLE;
        }
    }

    void CreateDepthResources() {
        DestroyDepthResources();
        g.depthFormat = FindDepthFormat();
        if (g.depthFormat == VK_FORMAT_UNDEFINED) return;

        VkExtent2D extent = g.swapchain->GetExtent();
        VkExtent3D depthExtent{extent.width, extent.height, 1};
        CreateImage(depthExtent, g.depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, g.depthImage,
                    g.depthMemory);
        g.depthView = CreateImageView(g.depthImage, g.depthFormat, AspectForFormat(g.depthFormat));

        RecordTransferCommands(
            [&](VkCommandBuffer cmd) {
                CmdTransitionImageLayout(cmd, g.depthImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                         AspectForFormat(g.depthFormat));
            },
            false);
    }

    void RefreshSwapchainStateForRendering() {
        const auto& imageViews = g.swapchain->GetImageViews();
        g.swapchainImageLayouts.clear();
        g.swapchainImageLayouts.resize(imageViews.size(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        g.defaultRenderingCompatHash = ComputeDefaultRenderingCompatHash();
    }

    void DestroyFrameResources() {
        DestroyDescriptorPools();
        g.pendingBufferCopies.clear();
        for (auto& pending : g.stagingPending) {
            for (auto& buf : pending) {
                g.stagingPool.push_back(buf);
            }
        }
        g.stagingPending.clear();
        for (auto& f : g.frames) {
            if (f) f->Cleanup(*g.ctx);
        }
        g.frames.clear();

        for (auto& ring : g.stagingRings) {
            DestroyBuffer(ring.buffer);
            ring.capacity = 0;
            ring.head = 0;
        }
        g.stagingRings.clear();
    }

    void CreateFrameResources() {
        DestroyFrameResources();
        Uint32 imageCount = static_cast<Uint32>(g.swapchain->GetImageViews().size());
        if (imageCount == 0) throw RuntimeError("Swapchain has zero images");
        Uint32 frames = std::min<Uint32>(g.maxFramesInFlight, imageCount);
        for (Uint32 i = 0; i < frames; ++i) {
            auto fr = MakeUnique<DV::FrameContext>();
            fr->Initialize(*g.ctx, g.commandPool);
            g.frames.push_back(Move(fr));
        }
        g.frameDescriptorPools.clear();
        g.frameDescriptorPools.resize(frames);
        g.stagingPending.clear();
        g.stagingPending.resize(frames);
        g.stagingRings.clear();
        g.stagingRings.resize(frames);
        g.currentFrame = 0;
    }

    void CreateCommandPool() {
        if (g.commandPool != VK_NULL_HANDLE) return;
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = g.ctx->GetGraphicsQueueFamily();
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_VERIFY(vkCreateCommandPool(g.ctx->GetDevice(), &cpci, nullptr, &g.commandPool), "vkCreateCommandPool");
    }

    void BeginCommandBuffer() {
        if (g.cmdBufferBegun) return;
        auto& frame = *g.frames[g.currentFrame];
        VK_VERIFY(vkWaitForFences(g.ctx->GetDevice(), 1, &frame.InFlightFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        VK_VERIFY(vkResetFences(g.ctx->GetDevice(), 1, &frame.InFlightFence), "vkResetFences");
        if (g.currentFrame < g.stagingRings.size()) {
            g.stagingRings[g.currentFrame].head = 0;
        }
        if (!g.stagingPending.empty()) {
            auto& pending = g.stagingPending[g.currentFrame];
            for (auto& buf : pending) {
                g.stagingPool.push_back(buf);
            }
            pending.clear();
        }
        ClearFrameTrashBuffers();
        ClearFrameTrashImages();
        VK_VERIFY(vkResetCommandBuffer(frame.CommandBuffer, 0), "vkResetCommandBuffer");
        ResetFrameDescriptorPools(g.currentFrame);
        ResetProgramUboPools(g.currentFrame);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_VERIFY(vkBeginCommandBuffer(frame.CommandBuffer, &bi), "vkBeginCommandBuffer");
        g.cmdBufferBegun = true;
    }

    void BeginRendering(GLbitfield clearMask, const PendingClearInfo* clearInfo) {
        if (g.recording) return;
        auto& frame = *g.frames[g.currentFrame];
        Vector<VkRenderingAttachmentInfo> colorAttachments;
        colorAttachments.resize(g.activeColorAttachmentCount);
        for (Uint32 i = 0; i < g.activeColorAttachmentCount; ++i) {
            auto& color = colorAttachments[i];
            color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = g.activeColorViews[i];
            if (color.imageView == VK_NULL_HANDLE) {
                color.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                continue;
            }

            color.imageLayout = g.activeColorLayouts[i];
            Bool canClearColor = false;
            if ((clearMask & GL_COLOR_BUFFER_BIT) && clearInfo) {
                if (g.activeIsDefault) {
                    canClearColor = g.activeDefaultColorWrites;
                } else if (i < g.activeColorAttachmentIndices.size()) {
                    canClearColor = g.activeColorAttachmentIndices[i] >= 0;
                }
            }
            color.loadOp = canClearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (canClearColor) {
                color.clearValue.color = {clearInfo->clearColor.x(), clearInfo->clearColor.y(),
                                          clearInfo->clearColor.z(), clearInfo->clearColor.w()};
            }
        }

        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingAttachmentInfo stencilAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Bool hasDepthAttachment =
            g.activeDepthView != VK_NULL_HANDLE && g.activeDepthAttachmentFormat != VK_FORMAT_UNDEFINED;
        Bool hasStencilAttachment =
            g.activeStencilView != VK_NULL_HANDLE && g.activeStencilAttachmentFormat != VK_FORMAT_UNDEFINED;
        if (hasDepthAttachment) {
            Bool clearDepth = (clearMask & GL_DEPTH_BUFFER_BIT) && clearInfo;
            depthAttachment.imageView = g.activeDepthView;
            depthAttachment.imageLayout = g.activeDepthLayout;
            depthAttachment.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.clearValue.depthStencil = {clearInfo ? clearInfo->clearDepth : 1.0f, 0};
        }
        if (hasStencilAttachment) {
            Bool clearStencil = (clearMask & GL_STENCIL_BUFFER_BIT) && clearInfo;
            stencilAttachment.imageView = g.activeStencilView;
            stencilAttachment.imageLayout = g.activeStencilLayout;
            stencilAttachment.loadOp = clearStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            stencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            stencilAttachment.clearValue.depthStencil = {clearInfo ? clearInfo->clearDepth : 1.0f, 0};
        }

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.offset = {0, 0};
        rendering.renderArea.extent = g.activeExtent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = static_cast<Uint32>(colorAttachments.size());
        rendering.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
        rendering.pDepthAttachment = hasDepthAttachment ? &depthAttachment : nullptr;
        rendering.pStencilAttachment = hasStencilAttachment ? &stencilAttachment : nullptr;

        MOBILEGL_ASSERT(g.pfnCmdBeginRendering != nullptr, "vkCmdBeginRendering function pointer is null");
        g.pfnCmdBeginRendering(frame.CommandBuffer, &rendering);
        g.recording = true;
        g.recordingRenderingConfigHash = g.activeRenderingConfigHash;
    }

    Bool EnsureRenderingBound() {
        if (!SyncDrawFramebuffer()) return false;
        if (!HasActiveRenderingAttachments()) return false;

        GLbitfield pendingMask = 0;
        PendingClearInfo pendingInfo{};
        auto pendingIt = g.pendingClears.end();
        if (g.activeStateFBO) {
            pendingIt = g.pendingClears.find(g.activeStateFBO);
            if (pendingIt != g.pendingClears.end() && pendingIt->second.mask) {
                pendingMask = pendingIt->second.mask;
                pendingInfo = pendingIt->second;
            }
        }

        GLbitfield effectiveMask = 0;
        if (pendingMask & GL_COLOR_BUFFER_BIT) {
            Bool hasColor = false;
            if (g.activeIsDefault) {
                hasColor = g.activeDefaultColorWrites && !g.activeColorViews.empty() &&
                           g.activeColorViews[0] != VK_NULL_HANDLE;
            } else {
                for (Uint32 i = 0; i < g.activeColorAttachmentCount; ++i) {
                    if (g.activeColorViews[i] != VK_NULL_HANDLE && i < g.activeColorAttachmentIndices.size() &&
                        g.activeColorAttachmentIndices[i] >= 0) {
                        hasColor = true;
                        break;
                    }
                }
            }
            if (hasColor) effectiveMask |= GL_COLOR_BUFFER_BIT;
        }
        VkImageAspectFlags depthAspect =
            g.activeDepthFormat != VK_FORMAT_UNDEFINED ? AspectForFormat(g.activeDepthFormat) : 0;
        if ((pendingMask & GL_DEPTH_BUFFER_BIT) && (depthAspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
            g.activeDepthView != VK_NULL_HANDLE) {
            effectiveMask |= GL_DEPTH_BUFFER_BIT;
        }
        if ((pendingMask & GL_STENCIL_BUFFER_BIT) && (depthAspect & VK_IMAGE_ASPECT_STENCIL_BIT) &&
            g.activeStencilView != VK_NULL_HANDLE) {
            effectiveMask |= GL_STENCIL_BUFFER_BIT;
        }

        if (pendingMask && effectiveMask == 0 && pendingIt != g.pendingClears.end()) {
            g.pendingClears.erase(pendingIt);
            pendingIt = g.pendingClears.end();
            pendingMask = 0;
        }

        BeginCommandBuffer();
        auto& frame = *g.frames[g.currentFrame];
        EnsureActiveColorLayoutsForRendering(frame.CommandBuffer);
        if (!g.pendingBufferCopies.empty()) {
            if (g.recording) {
                EndActiveRendering();
            }
            FlushPendingBufferCopies(frame.CommandBuffer);
        }

        if (g.recording && g.recordingRenderingConfigHash == g.activeRenderingConfigHash && pendingMask == 0) {
            return true;
        }

        bool forceRestart = pendingMask && effectiveMask != 0;
        if (!g.recording) {
            BeginRendering(effectiveMask, effectiveMask ? &pendingInfo : nullptr);
            if (pendingIt != g.pendingClears.end()) g.pendingClears.erase(pendingIt);
            return true;
        }
        if (forceRestart || g.recordingRenderingConfigHash != g.activeRenderingConfigHash) {
            EndActiveRendering();
            BeginRendering(effectiveMask, effectiveMask ? &pendingInfo : nullptr);
            if (pendingIt != g.pendingClears.end()) g.pendingClears.erase(pendingIt);
        }
        return true;
    }

    void CmdClearAttachments(VkCommandBuffer cmd, GLbitfield mask) {
        Vector<VkClearAttachment> atts;

        if (mask & GL_COLOR_BUFFER_BIT) {
            if (g.activeIsDefault || !g.activeBackendFBO) {
                if (!g.activeDefaultColorWrites) {
                    // Default FBO draw buffer is NONE, skip color clears.
                } else {
                    for (Uint32 i = 0; i < g.activeColorAttachmentCount; ++i) {
                        VkClearAttachment color{};
                        color.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        color.colorAttachment = i;
                        color.clearValue.color = {g.clearColor.x(), g.clearColor.y(), g.clearColor.z(),
                                                  g.clearColor.w()};
                        atts.push_back(color);
                    }
                }
            } else {
                for (Uint32 i = 0; i < g.activeColorAttachmentCount; ++i) {
                    if (g.activeBackendFBO->drawBufferAttachmentIndices[i] < 0) continue;
                    VkClearAttachment color{};
                    color.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    color.colorAttachment = i;
                    color.clearValue.color = {g.clearColor.x(), g.clearColor.y(), g.clearColor.z(), g.clearColor.w()};
                    atts.push_back(color);
                }
            }
        }
        if ((mask & GL_DEPTH_BUFFER_BIT) && g.activeHasDepth) {
            VkClearAttachment depth{};
            depth.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depth.clearValue.depthStencil = {g.clearDepth, 0};
            atts.push_back(depth);
        }
        if ((mask & GL_STENCIL_BUFFER_BIT) && g.activeHasDepth) {
            VkClearAttachment stencil{};
            stencil.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            stencil.clearValue.depthStencil = {g.clearDepth, 0};
            atts.push_back(stencil);
        }

        if (atts.empty()) return;
        VkClearRect rect{};
        rect.rect.offset = {0, 0};
        rect.rect.extent = g.activeExtent;
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;

        vkCmdClearAttachments(cmd, atts.size(), atts.data(), 1, &rect);
    }

    void EndRecordingAndSubmit() {
        auto& frame = *g.frames[g.currentFrame];
        EndActiveRendering();
        FlushPendingBufferCopies(frame.CommandBuffer);

        if (frame.CurrentImageIndex < g.swapchainImageLayouts.size()) {
            const auto& images = g.swapchain->GetImages();
            if (frame.CurrentImageIndex < images.size()) {
                auto& currentLayout = g.swapchainImageLayouts[frame.CurrentImageIndex];
                if (currentLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                    CmdTransitionImageLayout(frame.CommandBuffer, images[frame.CurrentImageIndex], currentLayout,
                                             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
                    currentLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                }
            }
        }

        VK_VERIFY(vkEndCommandBuffer(frame.CommandBuffer), "vkEndCommandBuffer");

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkSemaphore waitSemaphores[] = {frame.ImageAvailable};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = waitSemaphores;
        si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &frame.CommandBuffer;

        VkSemaphore signalSemaphores[2] = {frame.RenderFinished, VK_NULL_HANDLE};
        Uint32 signalCount = 1;

        Uint64 waitValues[1] = {0};
        Uint64 signalValues[2] = {0, 0};
        VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        if (g.timelineSemaphore != VK_NULL_HANDLE) {
            signalSemaphores[signalCount] = g.timelineSemaphore;
            signalValues[signalCount] = ++g.timelineValue;
            signalCount++;
            tsi.waitSemaphoreValueCount = si.waitSemaphoreCount;
            tsi.pWaitSemaphoreValues = waitValues;
            tsi.signalSemaphoreValueCount = signalCount;
            tsi.pSignalSemaphoreValues = signalValues;
            si.pNext = &tsi;
        }

        si.signalSemaphoreCount = signalCount;
        si.pSignalSemaphores = signalSemaphores;

        VK_VERIFY(vkQueueSubmit(g.ctx->GetGraphicsQueue(), 1, &si, frame.InFlightFence), "vkQueueSubmit");
        g.frameSubmitted = true;
        g.cmdBufferBegun = false;
        g.recordingRenderingConfigHash = 0;
    }

    void RecreateSwapchain() {
        vkDeviceWaitIdle(g.ctx->GetDevice());
        g.recording = false;
        g.cmdBufferBegun = false;
        g.frameSubmitted = false;
        g.recordingRenderingConfigHash = 0;

        DestroyFrameResources();
        DestroyDepthResources();

        g.swapchain->SetViewportSize(g.viewportSize);
        g.swapchain->Recreate();
        CreateDepthResources();
        RefreshSwapchainStateForRendering();
        CreateFrameResources();

        for (auto& [_, prog] : g.programs) {
            DestroyProgramResource(prog);
            prog = ProgramResource{};
        }
    }

    void FrameBeginInternal() {
        auto& frame = *g.frames[g.currentFrame];
        auto& imagesInFlight = g.swapchain->GetImagesInFlight();
        Uint32 imageIndex = 0;
        VkResult res = vkAcquireNextImageKHR(g.ctx->GetDevice(), g.swapchain->GetSwapchain(), UINT64_MAX,
                                             frame.ImageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            return;
        }
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(g.ctx->GetDevice(), 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[imageIndex] = frame.InFlightFence;
        frame.CurrentImageIndex = imageIndex;
        VK_VERIFY(res, "vkAcquireNextImageKHR");
    }

    void EnsureInitialized() {
        if (g.initialized) return;
        throw RuntimeError("Vulkan backend not initialized");
    }

    void ClearFrameTrashBuffers() {
        auto& frame = *g.frames[g.currentFrame];
        for (auto& buf : frame.TrashBuffers) {
            if (buf.mapped) {
                vkUnmapMemory(g.ctx->GetDevice(), buf.memory);
            }
            vkDestroyBuffer(g.ctx->GetDevice(), buf.buffer, nullptr);
            vkFreeMemory(g.ctx->GetDevice(), buf.memory, nullptr);
        }
        frame.TrashBuffers.clear();
    }

    void ClearFrameTrashImages() {
        auto& frame = *g.frames[g.currentFrame];
        for (auto& img : frame.TrashImages) {
            if (img.view != VK_NULL_HANDLE) {
                vkDestroyImageView(g.ctx->GetDevice(), img.view, nullptr);
            }
            for (auto view : img.mipViews) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(g.ctx->GetDevice(), view, nullptr);
            }
            if (img.image != VK_NULL_HANDLE) {
                vkDestroyImage(g.ctx->GetDevice(), img.image, nullptr);
            }
            if (img.memory != VK_NULL_HANDLE) {
                vkFreeMemory(g.ctx->GetDevice(), img.memory, nullptr);
            }
        }
        frame.TrashImages.clear();
    }

    void FrameBegin() {
        EnsureInitialized();
        FrameBeginInternal();
    }

    void Present() {
        EnsureInitialized();
        auto& frame = *g.frames[g.currentFrame];

        {
            auto& slot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw);
            auto currentFBO = slot.GetBoundObject();
            if (currentFBO) {
                auto it = g.pendingClears.find(currentFBO);
                if (it != g.pendingClears.end() && it->second.mask) {
                    EnsureRenderingBound();
                }
            }
        }

        if (g.cmdBufferBegun) {
            EndRecordingAndSubmit();
        }

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        VkSemaphore waitSemaphores[] = {g.frameSubmitted ? frame.RenderFinished : frame.ImageAvailable};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = waitSemaphores;
        VkSwapchainKHR scs[] = {g.swapchain->GetSwapchain()};
        pi.swapchainCount = 1;
        pi.pSwapchains = scs;
        pi.pImageIndices = &frame.CurrentImageIndex;
        VkResult pres = vkQueuePresentKHR(g.ctx->GetGraphicsQueue(), &pi);
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
            RecreateSwapchain();
        } else {
            VK_VERIFY(pres, "vkQueuePresentKHR");
        }

        g.frameSubmitted = false;
        g.currentFrame = (g.currentFrame + 1) % g.frames.size();
        FrameBeginInternal();
    }

    const VulkanState& GetVulkanState() {
        EnsureInitialized();
        return g;
    }

    void InitVulkan(ANativeWindow* window) {
        if (g.initialized) return;

        g.ctx = MakeUnique<DV::VulkanContext>();
        g.ctx->Initialize(window, "MobileGL-DirectVulkanTMP-Tmp");

        g.swapchain = MakeUnique<DV::SwapchainManager>(*g.ctx);
        g.swapchain->Initialize();
        LoadDynamicRenderingCommands();
        if (!g.pfnCmdBeginRendering || !g.pfnCmdEndRendering) {
            throw RuntimeError("Dynamic Rendering commands are not available on this Vulkan loader/device");
        }
        if (g.timelineSemaphore == VK_NULL_HANDLE) {
            VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            stci.initialValue = 0;
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            sci.pNext = &stci;
            VkResult semRes = vkCreateSemaphore(g.ctx->GetDevice(), &sci, nullptr, &g.timelineSemaphore);
            if (semRes != VK_SUCCESS) {
                g.timelineSemaphore = VK_NULL_HANDLE;
            } else {
                g.timelineValue = 0;
            }
        }

        CreateCommandPool();
        CreateDepthResources();
        RefreshSwapchainStateForRendering();
        CreateFrameResources();

        CreateNullUbo();
        CreateDefaultResources();

        g.initialized = true;
        FrameBeginInternal();
    }

    void ApplyDrawState(DV::FrameContext* frame, const RenderStateParameters* rs, VkPipeline pipeline,
                        VkDescriptorSet drawSet, ProgramResource* progRes) {
        if (!frame || !rs || pipeline == VK_NULL_HANDLE) return;

        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<Float>(g.activeExtent.width);
        vp.height = static_cast<Float>(g.activeExtent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(frame->CommandBuffer, 0, 1, &vp);

        VkRect2D scissor{};
        if (rs->ScissorTestEnabled) {
            scissor.offset = {rs->ScissorBox.x(), rs->ScissorBox.y()};
            scissor.extent = {static_cast<Uint32>(rs->ScissorBox.z()), static_cast<Uint32>(rs->ScissorBox.w())};
        } else {
            scissor.offset = {0, 0};
            scissor.extent = g.activeExtent;
        }
        vkCmdSetScissor(frame->CommandBuffer, 0, 1, &scissor);

        vkCmdBindPipeline(frame->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (drawSet != VK_NULL_HANDLE && progRes && progRes->pipelineLayout != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(frame->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, progRes->pipelineLayout, 0,
                                    1, &drawSet, 0, nullptr);
        }
    }

    Bool PrepareForDraw(GLenum mode, VkPipeline& pipeline, VkDescriptorSet& drawSet, VertexArrayObject*& vao,
                        const RenderStateParameters*& rs, ProgramResource*& progRes, DV::FrameContext*& frame) {
        EnsureInitialized();
        auto program = MG_State::pGLContext->GetCurrentProgram();
        if (!program || !program->GetLinkStatus()) return false;
        auto& shaders = program->GetAttachedShaders();
        for (SizeT i = 0; i < shaders.size(); ++i) {
            auto& sh = shaders[i];
            if (!sh) continue;
        }
        vao = MG_State::pGLContext->GetBoundVertexArray().get();
        rs = &MG_State::pGLContext->GetRenderStateParameters();

        if (!SyncDrawFramebuffer()) return false;
        BeginCommandBuffer();
        progRes = &GetProgramResource(program.get());
        RenderStateParameters rsForPipeline = *rs;
        if (g.activeIsDefault && !g.activeDefaultColorWrites) {
            rsForPipeline.ColorMask = {false, false, false, false};
        }
        pipeline = GetOrCreatePipeline(*progRes, vao, rsForPipeline, ToVkTopology(mode), g.activeColorFormats,
                                       g.activeDepthAttachmentFormat, g.activeStencilAttachmentFormat,
                                       GetActiveRenderingCompatHash());
        if (pipeline == VK_NULL_HANDLE) {
            MGLOG_E("Vulkan: failed to create pipeline (no shader stages)");
            return false;
        }

        frame = g.frames.empty() ? nullptr : g.frames[g.currentFrame].get();
        if (!frame) return false;

        drawSet = UpdateDescriptorSetsForDraw(*progRes, g.currentFrame);
        if (progRes->setLayout != VK_NULL_HANDLE && drawSet == VK_NULL_HANDLE) {
            MGLOG_E("Vulkan: failed to allocate descriptor set for draw");
            return false;
        }
        g.viewportSize = {(Uint)rs->Viewport.z(), (Uint)rs->Viewport.w()};

        return true;
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet drawSet = VK_NULL_HANDLE;
        VertexArrayObject* vao = nullptr;
        const RenderStateParameters* rs = nullptr;
        ProgramResource* prog = nullptr;
        DV::FrameContext* frame = nullptr;
        if (!PrepareForDraw(mode, pipeline, drawSet, vao, rs, prog, frame)) return;

        if (vao) {
            for (Uint32 i = 0; i < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
                const auto& attr = vao->GetAttribute(i);
                if (!attr.Enabled || !attr.Buffer) continue;
                SyncBuffer(attr.Buffer.get());
            }
        }

        VkIndexType indexType = ToVkIndexType(type);
        Uint32 indexSize = IndexTypeSize(type);
        VkDeviceSize indexOffset = 0;

        BufferResource* iboRes = nullptr;
        if (vao) {
            auto ibo = vao->GetIndexBufferBindingSlot().GetBoundObject();
            if (ibo) {
                iboRes = &SyncBuffer(ibo.get());
                indexOffset = reinterpret_cast<uintptr_t>(indices);
            }
        }

        BufferResource tempIndex;
        if (!iboRes && indices) {
            VkDeviceSize sz = static_cast<VkDeviceSize>(count) * indexSize;
            CreateBuffer(sz, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, tempIndex);
            Memcpy(tempIndex.mapped, indices, sz);
            iboRes = &tempIndex;
            indexOffset = 0;
        }

        if (iboRes && iboRes->buffer != VK_NULL_HANDLE) {
            if (!EnsureRenderingBound()) return;
            ApplyDrawState(frame, rs, pipeline, drawSet, prog);

            if (vao) {
                for (Uint32 i = 0; i < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
                    const auto& attr = vao->GetAttribute(i);
                    if (!attr.Enabled || !attr.Buffer) continue;
                    auto& vbo = SyncBuffer(attr.Buffer.get());
                    VkDeviceSize offset = 0;
                    VkBuffer buf = vbo.buffer;
                    vkCmdBindVertexBuffers(frame->CommandBuffer, i, 1, &buf, &offset);
                }
            }

            vkCmdBindIndexBuffer(frame->CommandBuffer, iboRes->buffer, 0, indexType);
            Uint32 firstIndex = indexSize ? static_cast<Uint32>(indexOffset / indexSize) : 0;
            vkCmdDrawIndexed(frame->CommandBuffer, count, 1, firstIndex, 0, 0);
        }

        if (tempIndex.buffer != VK_NULL_HANDLE || tempIndex.memory != VK_NULL_HANDLE) {
            frame->TrashBuffers.push_back({tempIndex.buffer, tempIndex.memory, tempIndex.mapped != nullptr});
        }
    }

    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter) {
        EnsureInitialized();
        GLbitfield validMask = mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (!validMask) return;

        auto& drawSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw);
        auto& readSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read);
        auto drawFBO = drawSlot.GetBoundObject();
        auto readFBO = readSlot.GetBoundObject();
        if (!drawFBO || !readFBO) {
            MGLOG_E("BlitFramebuffer: draw/read framebuffer is not bound");
            return;
        }

        auto defaultFBO = MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo->defaultFBO;

        auto& pending = g.pendingClears;
        auto pendingDrawIt = pending.find(drawFBO);
        if (pendingDrawIt != pending.end() && pendingDrawIt->second.mask) {
            if (!EnsureRenderingBound()) return;
        }

        auto ensureBackendFbo = [&](const SharedPtr<FramebufferObject>& fbo, SharedPtr<BackendFramebufferObject>& out,
                                    Bool& isDefault) -> Bool {
            if (fbo == defaultFBO) {
                isDefault = true;
                out = nullptr;
                return true;
            }

            isDefault = false;
            auto it = g.framebuffers.find(fbo);
            if (it == g.framebuffers.end()) {
                auto backend = MakeShared<BackendFramebufferObject>();
                g.framebuffers[fbo] = backend;
                out = backend;
            } else {
                out = it->second;
            }
            if (!SyncFramebufferObject(*out, fbo)) return false;
            return out->IsValid();
        };

        SharedPtr<BackendFramebufferObject> readBackend = nullptr;
        SharedPtr<BackendFramebufferObject> drawBackend = nullptr;
        Bool readIsDefault = false;
        Bool drawIsDefault = false;
        if (!ensureBackendFbo(readFBO, readBackend, readIsDefault)) return;
        if (!ensureBackendFbo(drawFBO, drawBackend, drawIsDefault)) return;

        BeginCommandBuffer();
        auto& frame = *g.frames[g.currentFrame];
        EndActiveRendering();

        struct BlitImageRef {
            VkImage image = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = 0;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout restoreLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout* trackedLayout = nullptr;
            VkExtent2D extent{0, 0};
        };

        auto stageForLayout = [](VkImageLayout layout) -> VkPipelineStageFlags {
            switch (layout) {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return VK_PIPELINE_STAGE_TRANSFER_BIT;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            default:
                return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            }
        };

        auto accessForLayout = [](VkImageLayout layout) -> VkAccessFlags {
            switch (layout) {
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return VK_ACCESS_TRANSFER_READ_BIT;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return VK_ACCESS_TRANSFER_WRITE_BIT;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return VK_ACCESS_SHADER_READ_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            default:
                return 0;
            }
        };

        auto transitionForBlit = [&](BlitImageRef& ref, VkImageLayout newLayout, VkImageAspectFlags aspectMask) {
            if (ref.image == VK_NULL_HANDLE || ref.layout == newLayout || aspectMask == 0) return;
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = ref.layout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = ref.image;
            barrier.subresourceRange.aspectMask = aspectMask;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = accessForLayout(ref.layout);
            barrier.dstAccessMask = accessForLayout(newLayout);

            vkCmdPipelineBarrier(frame.CommandBuffer, stageForLayout(ref.layout), stageForLayout(newLayout), 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            ref.layout = newLayout;
            if (ref.trackedLayout) *ref.trackedLayout = newLayout;
        };

        auto resolveDefaultColor = [&](BlitImageRef& out) -> Bool {
            if (g.frames.empty()) return false;
            const auto& images = g.swapchain->GetImages();
            if (g.swapchainImageLayouts.size() != images.size()) return false;
            Uint32 imageIndex = frame.CurrentImageIndex;
            if (imageIndex >= images.size()) return false;
            out.image = images[imageIndex];
            out.format = g.swapchain->GetFormat();
            out.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            out.layout = g.swapchainImageLayouts[imageIndex];
            out.restoreLayout = out.layout;
            out.trackedLayout = &g.swapchainImageLayouts[imageIndex];
            out.extent = g.swapchain->GetExtent();
            return out.image != VK_NULL_HANDLE;
        };

        auto resolveDefaultDepthStencil = [&](BlitImageRef& out) -> Bool {
            if (g.depthImage == VK_NULL_HANDLE || g.depthFormat == VK_FORMAT_UNDEFINED) return false;
            out.image = g.depthImage;
            out.format = g.depthFormat;
            out.aspect = AspectForFormat(g.depthFormat) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            out.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            out.restoreLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            out.trackedLayout = nullptr;
            out.extent = g.swapchain->GetExtent();
            return out.aspect != 0;
        };

        auto resolveBackendAttachment = [&](const SharedPtr<BackendFramebufferObject>& backend,
                                            FramebufferAttachmentType type, BlitImageRef& out) -> Bool {
            if (!backend) return false;
            Int32 idx = backend->attachmentIndex[static_cast<SizeT>(type)];
            if (idx < 0 || static_cast<SizeT>(idx) >= backend->attachments.size()) return false;
            const auto& info = backend->attachments[static_cast<SizeT>(idx)];
            if (info.texture) {
                auto& tex = SyncTexture(info.texture);
                if (!tex.valid || tex.image == VK_NULL_HANDLE) return false;
                out.image = tex.image;
                out.format = tex.format;
                out.aspect = AspectForFormat(tex.format);
                out.layout = tex.layout;
                out.restoreLayout = LayoutForFramebufferAttachment(type, tex.format);
                out.trackedLayout = &tex.layout;
                out.extent = {tex.extent.width, tex.extent.height};
                return true;
            }
            if (info.renderbuffer) {
                auto& rb = SyncRenderbuffer(info.renderbuffer);
                if (!rb.valid || rb.image == VK_NULL_HANDLE) return false;
                out.image = rb.image;
                out.format = rb.format;
                out.aspect = AspectForFormat(rb.format);
                out.layout = rb.layout;
                out.restoreLayout = LayoutForFramebufferAttachment(type, rb.format);
                out.trackedLayout = &rb.layout;
                out.extent = rb.extent;
                return true;
            }
            return false;
        };

        auto clampCoord = [](GLint v, Uint32 upper) -> Int32 { return std::clamp(v, 0, static_cast<GLint>(upper)); };

        auto hasBlitFeature = [&](VkFormat fmt, VkFormatFeatureFlagBits bit) -> Bool {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(g.ctx->GetPhysicalDevice(), fmt, &props);
            return (props.optimalTilingFeatures & bit) != 0;
        };

        auto blitOrCopyColor = [&](BlitImageRef src, BlitImageRef dst) {
            if (src.image == VK_NULL_HANDLE || dst.image == VK_NULL_HANDLE) return;
            if (src.image == dst.image) {
                MGLOG_W("BlitFramebuffer: skip color blit on the same image");
                return;
            }

            Int32 sx0 = clampCoord(srcX0, src.extent.width);
            Int32 sy0 = clampCoord(srcY0, src.extent.height);
            Int32 sx1 = clampCoord(srcX1, src.extent.width);
            Int32 sy1 = clampCoord(srcY1, src.extent.height);
            GLint dstY0Resolved = dstY0;
            GLint dstY1Resolved = dstY1;
            if (drawIsDefault) {
                GLint h = static_cast<GLint>(dst.extent.height);
                dstY0Resolved = h - dstY0;
                dstY1Resolved = h - dstY1;
            }
            Int32 dx0 = clampCoord(dstX0, dst.extent.width);
            Int32 dy0 = clampCoord(dstY0Resolved, dst.extent.height);
            Int32 dx1 = clampCoord(dstX1, dst.extent.width);
            Int32 dy1 = clampCoord(dstY1Resolved, dst.extent.height);
            if (sx0 == sx1 || sy0 == sy1 || dx0 == dx1 || dy0 == dy1) return;

            VkFilter vkFilter = (filter == GL_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            if (filter != GL_NEAREST && filter != GL_LINEAR) {
                MGLOG_W("BlitFramebuffer: unsupported filter 0x%x, fallback to GL_NEAREST", filter);
                vkFilter = VK_FILTER_NEAREST;
            }
            if (vkFilter == VK_FILTER_LINEAR &&
                !hasBlitFeature(src.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
                vkFilter = VK_FILTER_NEAREST;
            }

            Bool canBlit = hasBlitFeature(src.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                           hasBlitFeature(dst.format, VK_FORMAT_FEATURE_BLIT_DST_BIT);
            Bool srcRectPositive = sx1 > sx0 && sy1 > sy0;
            Bool dstRectPositive = dx1 > dx0 && dy1 > dy0;
            Bool sameExtent = (sx1 - sx0) == (dx1 - dx0) && (sy1 - sy0) == (dy1 - dy0);
            Bool canCopy = src.format == dst.format && srcRectPositive && dstRectPositive && sameExtent;

            transitionForBlit(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transitionForBlit(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            if (canBlit) {
                VkImageBlit region{};
                region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.srcSubresource.mipLevel = 0;
                region.srcSubresource.baseArrayLayer = 0;
                region.srcSubresource.layerCount = 1;
                region.srcOffsets[0] = {sx0, sy0, 0};
                region.srcOffsets[1] = {sx1, sy1, 1};
                region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.dstSubresource.mipLevel = 0;
                region.dstSubresource.baseArrayLayer = 0;
                region.dstSubresource.layerCount = 1;
                region.dstOffsets[0] = {dx0, dy0, 0};
                region.dstOffsets[1] = {dx1, dy1, 1};
                vkCmdBlitImage(frame.CommandBuffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, vkFilter);
            } else if (canCopy) {
                VkImageCopy region{};
                region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.srcSubresource.mipLevel = 0;
                region.srcSubresource.baseArrayLayer = 0;
                region.srcSubresource.layerCount = 1;
                region.srcOffset = {sx0, sy0, 0};
                region.dstSubresource = region.srcSubresource;
                region.dstOffset = {dx0, dy0, 0};
                region.extent = {static_cast<Uint32>(sx1 - sx0), static_cast<Uint32>(sy1 - sy0), 1};
                vkCmdCopyImage(frame.CommandBuffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            } else {
                MGLOG_W("BlitFramebuffer: color blit unsupported for current source/destination format or region");
            }

            transitionForBlit(src, src.restoreLayout, VK_IMAGE_ASPECT_COLOR_BIT);
            transitionForBlit(dst, dst.restoreLayout, VK_IMAGE_ASPECT_COLOR_BIT);
        };

        auto blitOrCopyDepthStencil = [&](BlitImageRef src, BlitImageRef dst, VkImageAspectFlags requestedAspect) {
            VkImageAspectFlags opAspect = requestedAspect & src.aspect & dst.aspect;
            if (!opAspect || src.image == VK_NULL_HANDLE || dst.image == VK_NULL_HANDLE) return;
            if (src.image == dst.image) {
                MGLOG_W("BlitFramebuffer: skip depth/stencil blit on the same image");
                return;
            }

            Int32 sx0 = clampCoord(srcX0, src.extent.width);
            Int32 sy0 = clampCoord(srcY0, src.extent.height);
            Int32 sx1 = clampCoord(srcX1, src.extent.width);
            Int32 sy1 = clampCoord(srcY1, src.extent.height);
            GLint dstY0Resolved = dstY0;
            GLint dstY1Resolved = dstY1;
            if (drawIsDefault) {
                GLint h = static_cast<GLint>(dst.extent.height);
                dstY0Resolved = h - dstY0;
                dstY1Resolved = h - dstY1;
            }
            Int32 dx0 = clampCoord(dstX0, dst.extent.width);
            Int32 dy0 = clampCoord(dstY0Resolved, dst.extent.height);
            Int32 dx1 = clampCoord(dstX1, dst.extent.width);
            Int32 dy1 = clampCoord(dstY1Resolved, dst.extent.height);
            if (sx0 == sx1 || sy0 == sy1 || dx0 == dx1 || dy0 == dy1) return;

            Bool canBlit = hasBlitFeature(src.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                           hasBlitFeature(dst.format, VK_FORMAT_FEATURE_BLIT_DST_BIT);
            Bool srcRectPositive = sx1 > sx0 && sy1 > sy0;
            Bool dstRectPositive = dx1 > dx0 && dy1 > dy0;
            Bool sameExtent = (sx1 - sx0) == (dx1 - dx0) && (sy1 - sy0) == (dy1 - dy0);
            Bool canCopy = src.format == dst.format && srcRectPositive && dstRectPositive && sameExtent;

            transitionForBlit(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, opAspect);
            transitionForBlit(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, opAspect);

            if (canBlit) {
                VkImageBlit region{};
                region.srcSubresource.aspectMask = opAspect;
                region.srcSubresource.mipLevel = 0;
                region.srcSubresource.baseArrayLayer = 0;
                region.srcSubresource.layerCount = 1;
                region.srcOffsets[0] = {sx0, sy0, 0};
                region.srcOffsets[1] = {sx1, sy1, 1};
                region.dstSubresource.aspectMask = opAspect;
                region.dstSubresource.mipLevel = 0;
                region.dstSubresource.baseArrayLayer = 0;
                region.dstSubresource.layerCount = 1;
                region.dstOffsets[0] = {dx0, dy0, 0};
                region.dstOffsets[1] = {dx1, dy1, 1};
                vkCmdBlitImage(frame.CommandBuffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
            } else if (canCopy) {
                VkImageCopy region{};
                region.srcSubresource.aspectMask = opAspect;
                region.srcSubresource.mipLevel = 0;
                region.srcSubresource.baseArrayLayer = 0;
                region.srcSubresource.layerCount = 1;
                region.srcOffset = {sx0, sy0, 0};
                region.dstSubresource = region.srcSubresource;
                region.dstOffset = {dx0, dy0, 0};
                region.extent = {static_cast<Uint32>(sx1 - sx0), static_cast<Uint32>(sy1 - sy0), 1};
                vkCmdCopyImage(frame.CommandBuffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            } else {
                MGLOG_W("BlitFramebuffer: depth/stencil blit unsupported for current format or region");
            }

            transitionForBlit(src, src.restoreLayout, opAspect);
            transitionForBlit(dst, dst.restoreLayout, opAspect);
        };

        if (validMask & GL_COLOR_BUFFER_BIT) {
            BlitImageRef srcColor{};
            Bool srcColorValid = false;
            if (readIsDefault) {
                srcColorValid = resolveDefaultColor(srcColor);
            } else {
                FramebufferAttachmentType readType = readFBO->GetReadBuffer();
                if (readType >= FramebufferAttachmentType::Color0 && readType <= FramebufferAttachmentType::Color31) {
                    srcColorValid = resolveBackendAttachment(readBackend, readType, srcColor);
                } else {
                    MGLOG_W("BlitFramebuffer: read buffer is not a color attachment");
                }
            }

            if (srcColorValid) {
                Vector<BlitImageRef> dstColors;
                if (drawIsDefault) {
                    BlitImageRef dstColor{};
                    if (resolveDefaultColor(dstColor)) dstColors.push_back(dstColor);
                } else {
                    const auto& drawBuffers = drawFBO->GetDrawBuffers();
                    for (Uint32 i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
                        FramebufferAttachmentType buf = drawBuffers[i];
                        if (buf < FramebufferAttachmentType::Color0 || buf > FramebufferAttachmentType::Color31) {
                            continue;
                        }
                        BlitImageRef dstColor{};
                        if (!resolveBackendAttachment(drawBackend, buf, dstColor)) continue;
                        bool duplicated = false;
                        for (const auto& existing : dstColors) {
                            if (existing.image == dstColor.image) {
                                duplicated = true;
                                break;
                            }
                        }
                        if (!duplicated) dstColors.push_back(dstColor);
                    }
                }

                for (const auto& dst : dstColors) {
                    blitOrCopyColor(srcColor, dst);
                }
            }
        }

        VkImageAspectFlags requestedDsAspect = 0;
        if (validMask & GL_DEPTH_BUFFER_BIT) requestedDsAspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (validMask & GL_STENCIL_BUFFER_BIT) requestedDsAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (requestedDsAspect != 0) {
            if (filter == GL_LINEAR) {
                MGLOG_W("BlitFramebuffer: GL_LINEAR is invalid for depth/stencil blit, forcing nearest");
            }

            BlitImageRef srcDs{};
            BlitImageRef dstDs{};
            Bool srcValid = false;
            Bool dstValid = false;

            if (readIsDefault) {
                srcValid = resolveDefaultDepthStencil(srcDs);
            } else {
                if ((requestedDsAspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
                    resolveBackendAttachment(readBackend, FramebufferAttachmentType::Depth, srcDs)) {
                    srcValid = true;
                } else if ((requestedDsAspect & VK_IMAGE_ASPECT_STENCIL_BIT) &&
                           resolveBackendAttachment(readBackend, FramebufferAttachmentType::Stencil, srcDs)) {
                    srcValid = true;
                }
            }

            if (drawIsDefault) {
                dstValid = resolveDefaultDepthStencil(dstDs);
            } else {
                if ((requestedDsAspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
                    resolveBackendAttachment(drawBackend, FramebufferAttachmentType::Depth, dstDs)) {
                    dstValid = true;
                } else if ((requestedDsAspect & VK_IMAGE_ASPECT_STENCIL_BIT) &&
                           resolveBackendAttachment(drawBackend, FramebufferAttachmentType::Stencil, dstDs)) {
                    dstValid = true;
                }
            }

            if (srcValid && dstValid) {
                blitOrCopyDepthStencil(srcDs, dstDs, requestedDsAspect);
            }
        }
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex) {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet drawSet = VK_NULL_HANDLE;
        VertexArrayObject* vao = nullptr;
        const RenderStateParameters* rs = nullptr;
        ProgramResource* prog = nullptr;
        DV::FrameContext* frame = nullptr;
        if (!PrepareForDraw(mode, pipeline, drawSet, vao, rs, prog, frame)) return;

        if (vao) {
            for (Uint32 i = 0; i < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
                const auto& attr = vao->GetAttribute(i);
                if (!attr.Enabled || !attr.Buffer) continue;
                SyncBuffer(attr.Buffer.get());
            }
        }

        VkIndexType indexType = ToVkIndexType(type);
        Uint32 indexSize = IndexTypeSize(type);
        VkDeviceSize indexOffset = 0;

        BufferResource* iboRes = nullptr;
        if (vao) {
            auto ibo = vao->GetIndexBufferBindingSlot().GetBoundObject();
            if (ibo) {
                iboRes = &SyncBuffer(ibo.get());
                indexOffset = reinterpret_cast<uintptr_t>(indices);
            }
        }

        BufferResource tempIndex;
        if (!iboRes && indices) {
            VkDeviceSize sz = static_cast<VkDeviceSize>(count) * indexSize;
            CreateBuffer(sz, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, tempIndex);
            Memcpy(tempIndex.mapped, indices, sz);
            iboRes = &tempIndex;
            indexOffset = 0;
        }

        if (iboRes && iboRes->buffer != VK_NULL_HANDLE) {
            if (!EnsureRenderingBound()) return;
            ApplyDrawState(frame, rs, pipeline, drawSet, prog);

            if (vao) {
                for (Uint32 i = 0; i < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
                    const auto& attr = vao->GetAttribute(i);
                    if (!attr.Enabled || !attr.Buffer) continue;
                    auto& vbo = SyncBuffer(attr.Buffer.get());
                    VkDeviceSize offset = 0;
                    VkBuffer buf = vbo.buffer;
                    vkCmdBindVertexBuffers(frame->CommandBuffer, i, 1, &buf, &offset);
                }
            }

            vkCmdBindIndexBuffer(frame->CommandBuffer, iboRes->buffer, 0, indexType);
            Uint32 firstIndex = indexSize ? static_cast<Uint32>(indexOffset / indexSize) : 0;
            vkCmdDrawIndexed(frame->CommandBuffer, count, 1, firstIndex, basevertex, 0);
        }

        if (tempIndex.buffer != VK_NULL_HANDLE || tempIndex.memory != VK_NULL_HANDLE) {
            frame->TrashBuffers.push_back({tempIndex.buffer, tempIndex.memory, tempIndex.mapped != nullptr});
        }
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet drawSet = VK_NULL_HANDLE;
        VertexArrayObject* vao = nullptr;
        const RenderStateParameters* rs = nullptr;
        ProgramResource* prog = nullptr;
        DV::FrameContext* frame = nullptr;
        if (!PrepareForDraw(mode, pipeline, drawSet, vao, rs, prog, frame)) return;

        if (vao) {
            for (Uint32 i = 0; i < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
                const auto& attr = vao->GetAttribute(i);
                if (!attr.Enabled || !attr.Buffer) continue;
                SyncBuffer(attr.Buffer.get());
            }
        }

        if (!EnsureRenderingBound()) return;
        ApplyDrawState(frame, rs, pipeline, drawSet, prog);
        if (vao) {
            for (Uint32 i = 0; i < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
                const auto& attr = vao->GetAttribute(i);
                if (!attr.Enabled || !attr.Buffer) continue;
                auto& vbo = SyncBuffer(attr.Buffer.get());
                VkDeviceSize offset = 0;
                VkBuffer buf = vbo.buffer;
                vkCmdBindVertexBuffers(frame->CommandBuffer, i, 1, &buf, &offset);
            }
        }
        vkCmdDraw(frame->CommandBuffer, count, 1, first, 0);
    }

    void Clear(GLbitfield mask) {
        EnsureInitialized();
        GLbitfield validMask = mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (!validMask) return;

        auto& slot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw);
        auto currentFBO = slot.GetBoundObject();
        if (!currentFBO) {
            MGLOG_E("No draw framebuffer bound");
            return;
        }

        if (currentFBO == g.activeStateFBO && g.recording) {
            EndActiveRendering();
        }

        auto& clearInfo = g.pendingClears[currentFBO];
        if (validMask & GL_COLOR_BUFFER_BIT) {
            clearInfo.clearColor = MG_State::pGLContext->GetClearColor();
        }
        if (validMask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
            clearInfo.clearDepth = MG_State::pGLContext->GetClearDepth();
        }
        clearInfo.mask |= validMask;
    }
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::TmpImpl
