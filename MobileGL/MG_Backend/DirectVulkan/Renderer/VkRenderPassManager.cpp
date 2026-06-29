// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkRenderPassManager.h"

#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_State/GLState/TextureState/TextureObject2D.h"
#include "MG_Util/Converters/MGToStr/FramebufferEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Metrics/TextureMetrics.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    static Bool TryResolveSampleCountFlagBits(Int requestedSamples, VkSampleCountFlagBits& outSampleCount) {
        switch (requestedSamples <= 0 ? 1 : requestedSamples) {
        case 1:
            outSampleCount = VK_SAMPLE_COUNT_1_BIT;
            return true;
        case 2:
            outSampleCount = VK_SAMPLE_COUNT_2_BIT;
            return true;
        case 4:
            outSampleCount = VK_SAMPLE_COUNT_4_BIT;
            return true;
        case 8:
            outSampleCount = VK_SAMPLE_COUNT_8_BIT;
            return true;
        case 16:
            outSampleCount = VK_SAMPLE_COUNT_16_BIT;
            return true;
        case 32:
            outSampleCount = VK_SAMPLE_COUNT_32_BIT;
            return true;
        case 64:
            outSampleCount = VK_SAMPLE_COUNT_64_BIT;
            return true;
        default:
            return false;
        }
    }

    static VkImageAspectFlags ResolveImageAspectMaskForFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    static Float ResolveColorClearAlpha(const MG_State::GLState::ITextureObject* texture, Float requestedAlpha) {
        if (texture != nullptr && MG_Util::GetBaseInternalFormatComponentCount(texture->GetFormat()) == 3) {
            return 1.0f;
        }
        return requestedAlpha;
    }

    static Bool IsCubeMapFaceUploadTarget(TextureUploadTarget target) {
        return target >= TextureUploadTarget::CubeMapPositiveX &&
               target <= TextureUploadTarget::CubeMapNegativeZ;
    }

    static Uint32 ResolveAttachmentBaseArrayLayer(const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        const TextureUploadTarget uploadTarget = attachment.GetTextureUploadTarget();
        if (!IsCubeMapFaceUploadTarget(uploadTarget)) {
            return 0;
        }
        return static_cast<Uint32>(uploadTarget) - static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
    }

    static Uint32 ResolveAttachmentLayerCount(const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        static_cast<void>(attachment);
        return 1u;
    }

    static VkImageViewType ResolveAttachmentViewType(
        const MG_State::GLState::FramebufferAttachmentObject& attachment,
        const VkTextureManager::TextureResource& resource) {
        return IsCubeMapFaceUploadTarget(attachment.GetTextureUploadTarget()) ?
            VK_IMAGE_VIEW_TYPE_2D :
            resource.viewType;
    }

    static MG_State::GLState::ITextureObject* ResolveCompleteColorAttachmentTexture(
        const MG_State::GLState::FramebufferObject& fbo,
        FramebufferAttachmentType attachmentType,
        Uint32 drawBufferIndex) {
        if (attachmentType == FramebufferAttachmentType::None) {
            return nullptr;
        }

        const auto& attachment = fbo.GetAttachment(attachmentType);
        if (!attachment.IsTexture()) {
            if (attachment.IsEmpty()) {
                MGLOG_D("GetOrCreateRenderPass: draw buffer slot %u (%s) on FBO %u has no bound color attachment; using VK_ATTACHMENT_UNUSED",
                        drawBufferIndex,
                        MG_Util::ConvertFramebufferAttachmentTypeToString(attachmentType).c_str(),
                        fbo.GetExternalIndex());
            }
            return nullptr;
        }

        if (!attachment.IsComplete()) {
            MGLOG_W("GetOrCreateRenderPass: draw buffer slot %u (%s) on FBO %u has an incomplete texture attachment; using VK_ATTACHMENT_UNUSED",
                    drawBufferIndex,
                    MG_Util::ConvertFramebufferAttachmentTypeToString(attachmentType).c_str(),
                    fbo.GetExternalIndex());
            return nullptr;
        }

        auto* texture = attachment.GetTexture().get();
        if (texture == nullptr) {
            MGLOG_W("GetOrCreateRenderPass: draw buffer slot %u (%s) on FBO %u resolved to a null texture; using VK_ATTACHMENT_UNUSED",
                    drawBufferIndex,
                    MG_Util::ConvertFramebufferAttachmentTypeToString(attachmentType).c_str(),
                    fbo.GetExternalIndex());
            return nullptr;
        }

        return texture;
    }

    DepthStencilAttachmentLoadInfo ResolveDepthStencilAttachmentLoadInfo(
        VkImageLayout trackedLayout, Bool clearDepth, Bool clearStencil) {
        DepthStencilAttachmentLoadInfo info{};
        info.depthLoadOp = clearDepth
            ? VK_ATTACHMENT_LOAD_OP_CLEAR
            : (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                          : VK_ATTACHMENT_LOAD_OP_LOAD);
        info.stencilLoadOp = clearStencil
            ? VK_ATTACHMENT_LOAD_OP_CLEAR
            : (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                          : VK_ATTACHMENT_LOAD_OP_LOAD);
        info.initialLayout =
            (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED || (clearDepth && clearStencil)) ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                                         : trackedLayout;
        return info;
    }

    IntVec2 ResolveRenderPassFramebufferExtent(Bool isDefaultFbo, const TextureSize& attachmentExtent,
                                               VkExtent2D swapchainExtent) {
        if (isDefaultFbo) {
            return {static_cast<Int>(swapchainExtent.width), static_cast<Int>(swapchainExtent.height)};
        }
        return {attachmentExtent.x(), attachmentExtent.y()};
    }

    void VkRenderPassManager::RenderbufferResource::Destroy(VkDevice device, VmaAllocator allocator) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
        if (image != VK_NULL_HANDLE && allocation != nullptr) {
            vmaDestroyImage(allocator, image, allocation);
        }
        renderbuffer.reset();
        image = VK_NULL_HANDLE;
        allocation = nullptr;
        view = VK_NULL_HANDLE;
        layout = VK_IMAGE_LAYOUT_UNDEFINED;
        format = VK_FORMAT_UNDEFINED;
        aspect = VK_IMAGE_ASPECT_NONE;
        extent = {0, 0};
        sampleCount = VK_SAMPLE_COUNT_1_BIT;
        internalFormat = TextureInternalFormat::Unknown;
        samples = 0;
    }

    VkRenderPassManager::VkRenderPassManager(VkDevice device,
        VkPhysicalDevice physicalDevice, VmaAllocator allocator, const VulkanRendererConfig& config,
        VkClearManager& clearManager, VkTextureManager& textureManager, SwapchainObject& swapchainObject):
        m_device(device), m_physicalDevice(physicalDevice), m_allocator(allocator), m_config(config),
        m_clearManager(clearManager), m_textureManager(textureManager), m_swapchainObject(swapchainObject) {
        RenderPassEntry::s_device = m_device;
        s_clearManager = &m_clearManager;
        s_textureManager = &m_textureManager;
        s_swapchainObject = &m_swapchainObject;
        s_renderPassManager = this;
    }

    VkRenderPassManager::~VkRenderPassManager() {}

    Bool VkRenderPassManager::Initialize() {
        return true;
    }

    void VkRenderPassManager::Shutdown() {
        m_renderPasses.clear();
        for (auto& [_, resource] : m_renderbufferResources) {
            resource.Destroy(m_device, m_allocator);
        }
        m_renderbufferResources.clear();
        m_pendingRenderbufferClears.clear();
        RenderPassEntry::s_textureResourcesScratch.clear();
        s_activeRenderPass = {};
        s_hasActiveRenderPass = false;
    }

    void VkRenderPassManager::CollectRenderbufferGarbage() {
        Vector<MG_State::GLState::RenderbufferObject*> deadRenderbuffers;
        deadRenderbuffers.reserve(m_renderbufferResources.size());
        for (auto& [renderbuffer, resource] : m_renderbufferResources) {
            const auto liveRenderbuffer = resource.renderbuffer.lock();
            if (!liveRenderbuffer || liveRenderbuffer.get() != renderbuffer) {
                deadRenderbuffers.emplace_back(renderbuffer);
            }
        }
        for (auto* renderbuffer : deadRenderbuffers) {
            auto resourceIt = m_renderbufferResources.find(renderbuffer);
            if (resourceIt != m_renderbufferResources.end()) {
                resourceIt->second.Destroy(m_device, m_allocator);
                m_renderbufferResources.erase(resourceIt);
            }
            m_pendingRenderbufferClears.erase(renderbuffer);
        }
    }

    VkRenderPassManager::RenderbufferResource* VkRenderPassManager::GetOrCreateRenderbufferResource(
        const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbuffer) {
        if (!renderbuffer || !renderbuffer->IsAllocated()) {
            return nullptr;
        }

        CollectRenderbufferGarbage();

        VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
        if (!TryResolveSampleCountFlagBits(renderbuffer->GetSamples(), sampleCount)) {
            MGLOG_E("GetOrCreateRenderbufferResource: unsupported renderbuffer sample count %d for renderbuffer %u",
                    renderbuffer->GetSamples(),
                    renderbuffer->GetExternalIndex());
            return nullptr;
        }

        const auto internalFormat = renderbuffer->GetInternalFormat();
        const VkFormat format = MG_Util::ConvertTextureInternalFormatToVkEnum(internalFormat);
        const VkImageAspectFlags aspect = ResolveImageAspectMaskForFormat(format);
        if ((aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
            MGLOG_E("GetOrCreateRenderbufferResource: color renderbuffer %u is not supported by DirectVulkan render passes yet",
                    renderbuffer->GetExternalIndex());
            return nullptr;
        }

        auto& resource = m_renderbufferResources[renderbuffer.get()];
        const Bool needsCreate =
            resource.image == VK_NULL_HANDLE ||
            resource.format != format ||
            resource.extent.width != static_cast<Uint32>(renderbuffer->GetWidth()) ||
            resource.extent.height != static_cast<Uint32>(renderbuffer->GetHeight()) ||
            resource.sampleCount != sampleCount ||
            resource.internalFormat != internalFormat ||
            resource.samples != renderbuffer->GetSamples();
        if (!needsCreate) {
            resource.renderbuffer = renderbuffer;
            return &resource;
        }

        resource.Destroy(m_device, m_allocator);
        resource.renderbuffer = renderbuffer;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = static_cast<Uint32>(renderbuffer->GetWidth());
        imageInfo.extent.height = static_cast<Uint32>(renderbuffer->GetHeight());
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.samples = sampleCount;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkImageFormatProperties imageFormatProperties{};
        const VkResult imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
            m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage, imageInfo.flags,
            &imageFormatProperties);
        if (imageFormatResult != VK_SUCCESS || (imageFormatProperties.sampleCounts & sampleCount) == 0) {
            MGLOG_E("GetOrCreateRenderbufferResource: unsupported renderbuffer format=%d samples=%d for renderbuffer %u",
                    static_cast<Int>(format),
                    static_cast<Int>(sampleCount),
                    renderbuffer->GetExternalIndex());
            return nullptr;
        }

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VK_VERIFY(vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &resource.image, &resource.allocation, nullptr),
                  "vmaCreateImage(renderbuffer)");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = resource.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                               VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        viewInfo.subresourceRange.aspectMask = aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        VK_VERIFY(vkCreateImageView(m_device, &viewInfo, nullptr, &resource.view),
                  "vkCreateImageView(renderbuffer)");

        resource.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resource.format = format;
        resource.aspect = aspect;
        resource.extent = {imageInfo.extent.width, imageInfo.extent.height};
        resource.sampleCount = sampleCount;
        resource.internalFormat = internalFormat;
        resource.samples = renderbuffer->GetSamples();
        return &resource;
    }

    Bool VkRenderPassManager::GetPendingRenderbufferClear(
        MG_State::GLState::RenderbufferObject* renderbuffer, ClearAttachmentPayload& outPayload) const {
        if (renderbuffer == nullptr) {
            return false;
        }
        auto it = m_pendingRenderbufferClears.find(renderbuffer);
        if (it == m_pendingRenderbufferClears.end()) {
            return false;
        }
        const auto liveRenderbuffer = it->second.renderbuffer.lock();
        if (!liveRenderbuffer || liveRenderbuffer.get() != renderbuffer) {
            return false;
        }
        outPayload = it->second.payload;
        return outPayload.mask != 0;
    }

    Bool VkRenderPassManager::HasPendingRenderbufferClear(
        const MG_State::GLState::FramebufferAttachmentObject& attachment) const {
        if (!attachment.IsRenderbuffer() || !attachment.GetRenderbuffer()) {
            return false;
        }
        ClearAttachmentPayload payload{};
        return GetPendingRenderbufferClear(attachment.GetRenderbuffer().get(), payload);
    }

    void VkRenderPassManager::QueueRenderbufferClear(
        const ClearAttachmentPayload& clearPayload,
        const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        if (clearPayload.mask == 0 || !attachment.IsRenderbuffer() || !attachment.IsComplete()) {
            return;
        }
        const auto renderbuffer = attachment.GetRenderbuffer();
        if (!renderbuffer) {
            return;
        }
        auto& pending = m_pendingRenderbufferClears[renderbuffer.get()];
        pending.renderbuffer = renderbuffer;
        pending.payload.mask |= clearPayload.mask;
        if ((clearPayload.mask & GL_COLOR_BUFFER_BIT) != 0) {
            pending.payload.color = clearPayload.color;
        }
        if ((clearPayload.mask & GL_DEPTH_BUFFER_BIT) != 0) {
            pending.payload.depth = clearPayload.depth;
        }
        if ((clearPayload.mask & GL_STENCIL_BUFFER_BIT) != 0) {
            pending.payload.stencil = clearPayload.stencil;
        }
    }

    void VkRenderPassManager::QueueRenderbufferClear(
        GLbitfield mask, const ClearFramebufferPayload& clearPayload,
        const MG_State::GLState::FramebufferObject& drawFbo) {
        if ((mask & GL_DEPTH_BUFFER_BIT) != 0) {
            QueueRenderbufferClear(
                ClearAttachmentPayload{.mask = GL_DEPTH_BUFFER_BIT, .depth = clearPayload.depth},
                drawFbo.GetAttachment(FramebufferAttachmentType::Depth));
        }
        if ((mask & GL_STENCIL_BUFFER_BIT) != 0) {
            QueueRenderbufferClear(
                ClearAttachmentPayload{.mask = GL_STENCIL_BUFFER_BIT, .stencil = clearPayload.stencil},
                drawFbo.GetAttachment(FramebufferAttachmentType::Stencil));
        }
    }

    void VkRenderPassManager::PopPendingRenderbufferClear(
        MG_State::GLState::RenderbufferObject* renderbuffer) {
        if (renderbuffer != nullptr) {
            m_pendingRenderbufferClears.erase(renderbuffer);
        }
    }

    VkRenderPassManager::HashType VkRenderPassManager::ComputeHash(
        const MG_State::GLState::FramebufferObject& fbo, Uint32 swapchainImageIndex, Bool includePendingClear) {
        XXHASH_VERIFY(XXH64_reset(m_hashState, m_config.CacheVersion));
        const Bool isDefaultFbo = fbo.IsDefaultFramebuffer();
        if (isDefaultFbo) {
            XXHASH_VERIFY(XXH64_update(m_hashState, &swapchainImageIndex, sizeof(swapchainImageIndex)));
        }
        auto& drawBuffers = fbo.GetDrawBuffers();
        XXHASH_VERIFY(XXH64_update(m_hashState, drawBuffers.data(), drawBuffers.size() * sizeof(drawBuffers[0])));
        auto readBuffer = fbo.GetReadBuffer();
        XXHASH_VERIFY(XXH64_update(m_hashState, &readBuffer, sizeof(FramebufferAttachmentType)));
        Int validDrawBufCount = 0;
        for (Int i = 0; i < drawBuffers.size(); ++i) {
            auto drawbuf = drawBuffers[i];
            if (drawbuf != FramebufferAttachmentType::None)
                validDrawBufCount = std::max(validDrawBufCount, i + 1);
        }
        XXHASH_VERIFY(XXH64_update(m_hashState, &validDrawBufCount, sizeof(validDrawBufCount)));

        auto combineFramebufferAttachmentObjHash = [&](FramebufferAttachmentType attachment) {
            auto& att = fbo.GetAttachment(attachment);

            Int type = 0;
            if (att.IsEmpty()) type = 0;
            else if (att.IsTexture()) type = 1;
            else if (att.IsRenderbuffer()) type = 2;
            XXHASH_VERIFY(XXH64_update(m_hashState, &type, sizeof(type)));
            void* contentPtr = nullptr;
            if (att.IsTexture())
                contentPtr = att.GetTexture().get();
            else if (att.IsRenderbuffer())
                contentPtr = att.GetRenderbuffer().get();
            XXHASH_VERIFY(XXH64_update(m_hashState, &contentPtr, sizeof(contentPtr)));
            if (att.IsTexture()) {
                const Uint64 textureLifetimeId = att.GetTexture()->GetLifetimeId();
                XXHASH_VERIFY(XXH64_update(m_hashState, &textureLifetimeId, sizeof(textureLifetimeId)));
                const Int textureLevel = att.GetTextureLevel();
                XXHASH_VERIFY(XXH64_update(m_hashState, &textureLevel, sizeof(textureLevel)));
                const TextureUploadTarget textureUploadTarget = att.GetTextureUploadTarget();
                XXHASH_VERIFY(XXH64_update(m_hashState, &textureUploadTarget, sizeof(textureUploadTarget)));

                Uint64 imageIdentity = 0;
                auto* texture = att.GetTexture().get();
                auto* resource = m_textureManager.SyncTextureAndGetDescriptor(*texture);
                if (resource != nullptr) {
                    imageIdentity = reinterpret_cast<Uint64>(resource->image);
                    XXHASH_VERIFY(XXH64_update(m_hashState, &resource->sampleCount, sizeof(resource->sampleCount)));
                } else {
                    const VkSampleCountFlagBits fallbackSampleCount = VK_SAMPLE_COUNT_1_BIT;
                    XXHASH_VERIFY(XXH64_update(m_hashState, &fallbackSampleCount, sizeof(fallbackSampleCount)));
                }
                XXHASH_VERIFY(XXH64_update(m_hashState, &imageIdentity, sizeof(imageIdentity)));
            }

            if (includePendingClear && att.IsTexture()) {
                auto* texture = att.GetTexture().get();
                const auto pendingClearKey = VkClearManager::MakePendingClearKey(att);
                auto hasClear = m_clearManager.HasPendingClear(pendingClearKey);
                XXHASH_VERIFY(XXH64_update(m_hashState, &hasClear, sizeof(hasClear)));
                if (hasClear) {
                    ClearAttachmentPayload clearPayload{};
                    Bool hasPayload = m_clearManager.GetPendingClear(pendingClearKey, clearPayload);
                    XXHASH_VERIFY(XXH64_update(m_hashState, &hasPayload, sizeof(hasPayload)));
                    if (hasPayload) {
                        XXHASH_VERIFY(XXH64_update(m_hashState, &clearPayload.mask, sizeof(clearPayload.mask)));
                    }
                }

                VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                if (isDefaultFbo) {
                    const Bool isDefaultColorAttachment =
                        attachment == FramebufferAttachmentType::Color0 ||
                        (attachment >= FramebufferAttachmentType::FrontLeft &&
                         attachment <= FramebufferAttachmentType::BackRight);
                    if (isDefaultColorAttachment) {
                        currentLayout = m_swapchainObject.GetImageLayout(swapchainImageIndex);
                    } else if (attachment == FramebufferAttachmentType::Depth ||
                               attachment == FramebufferAttachmentType::Stencil) {
                        currentLayout = m_swapchainObject.GetDepthStencilImageLayout(swapchainImageIndex);
                    }
                } else {
                    auto* textureResource = m_textureManager.SyncTextureAndGetDescriptor(*texture);
                    if (textureResource != nullptr) {
                        currentLayout = textureResource->layout;
                    }
                }
                XXHASH_VERIFY(XXH64_update(m_hashState, &currentLayout, sizeof(currentLayout)));
            }
            if (att.IsRenderbuffer() && att.GetRenderbuffer()) {
                const auto& renderbuffer = att.GetRenderbuffer();
                const auto internalFormat = renderbuffer->GetInternalFormat();
                const Int width = renderbuffer->GetWidth();
                const Int height = renderbuffer->GetHeight();
                const Int samples = renderbuffer->GetSamples();
                XXHASH_VERIFY(XXH64_update(m_hashState, &internalFormat, sizeof(internalFormat)));
                XXHASH_VERIFY(XXH64_update(m_hashState, &width, sizeof(width)));
                XXHASH_VERIFY(XXH64_update(m_hashState, &height, sizeof(height)));
                XXHASH_VERIFY(XXH64_update(m_hashState, &samples, sizeof(samples)));

                Uint64 imageIdentity = 0;
                VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                auto* resource = GetOrCreateRenderbufferResource(renderbuffer);
                if (resource != nullptr) {
                    imageIdentity = reinterpret_cast<Uint64>(resource->image);
                    currentLayout = resource->layout;
                    XXHASH_VERIFY(XXH64_update(m_hashState, &resource->sampleCount, sizeof(resource->sampleCount)));
                } else {
                    const VkSampleCountFlagBits fallbackSampleCount = VK_SAMPLE_COUNT_1_BIT;
                    XXHASH_VERIFY(XXH64_update(m_hashState, &fallbackSampleCount, sizeof(fallbackSampleCount)));
                }
                XXHASH_VERIFY(XXH64_update(m_hashState, &imageIdentity, sizeof(imageIdentity)));

                if (includePendingClear) {
                    const Bool hasClear = HasPendingRenderbufferClear(att);
                    XXHASH_VERIFY(XXH64_update(m_hashState, &hasClear, sizeof(hasClear)));
                    if (hasClear) {
                        ClearAttachmentPayload clearPayload{};
                        const Bool hasPayload = GetPendingRenderbufferClear(renderbuffer.get(), clearPayload);
                        XXHASH_VERIFY(XXH64_update(m_hashState, &hasPayload, sizeof(hasPayload)));
                        if (hasPayload) {
                            XXHASH_VERIFY(XXH64_update(m_hashState, &clearPayload.mask, sizeof(clearPayload.mask)));
                        }
                    }
                    XXHASH_VERIFY(XXH64_update(m_hashState, &currentLayout, sizeof(currentLayout)));
                }
            }
        };

        for (Int i = 0; i < validDrawBufCount; ++i) {
            auto drawbuf = drawBuffers[i];
            combineFramebufferAttachmentObjHash(drawbuf);
        }

        combineFramebufferAttachmentObjHash(FramebufferAttachmentType::Depth);
        combineFramebufferAttachmentObjHash(FramebufferAttachmentType::Stencil);

        return XXH64_digest(m_hashState);
    }

    RenderPassEntry& VkRenderPassManager::GetOrCreateRenderPass(const MG_State::GLState::FramebufferObject& fbo,
                                                                Uint32 swapchainImageIndex) {
        auto hasPendingClearOnFramebuffer = [&]() -> Bool {
            const auto& drawBuffers = fbo.GetDrawBuffers();
            for (auto attachment : drawBuffers) {
                if (attachment == FramebufferAttachmentType::None) {
                    continue;
                }

                const auto& att = fbo.GetAttachment(attachment);
                if (att.IsTexture() && m_clearManager.HasPendingClear(att)) {
                    return true;
                }
                if (HasPendingRenderbufferClear(att)) {
                    return true;
                }
            }

            const auto& depthAtt = fbo.GetAttachment(FramebufferAttachmentType::Depth);
            if (depthAtt.IsTexture() && m_clearManager.HasPendingClear(depthAtt)) {
                return true;
            }
            if (HasPendingRenderbufferClear(depthAtt)) {
                return true;
            }

            const auto& stencilAtt = fbo.GetAttachment(FramebufferAttachmentType::Stencil);
            if (stencilAtt.IsTexture() && m_clearManager.HasPendingClear(stencilAtt)) {
                return true;
            }
            if (HasPendingRenderbufferClear(stencilAtt)) {
                return true;
            }

            return false;
        };

        // retrieve from cache first
        auto* activeRenderPass = GetActiveRenderPass();
        auto compatibilityHash = ComputeHash(fbo, swapchainImageIndex, false);
        if (activeRenderPass != nullptr &&
            activeRenderPass->CompatibleWith(compatibilityHash) &&
            !hasPendingClearOnFramebuffer()) {
            auto activeIt = m_renderPasses.find(activeRenderPass->hash);
            MOBILEGL_ASSERT(activeIt != m_renderPasses.end(),
                            "GetOrCreateRenderPass: active render pass hash=0x%llx is missing from cache",
                            static_cast<unsigned long long>(activeRenderPass->hash));
            return activeIt->second;
        }
        auto hash = ComputeHash(fbo, swapchainImageIndex, true);
        auto it = m_renderPasses.find(hash);
        if (it != m_renderPasses.end())
            return it->second;

        Bool isDefaultFbo = fbo.IsDefaultFramebuffer();
        // Color attachment
        auto& drawbufs = fbo.GetDrawBuffers();
        const Uint32 colorAttachmentSlotCount = static_cast<Uint32>(drawbufs.size());

        const VkExtent2D swapchainExtent = m_swapchainObject.GetExtent();
        // Default framebuffer attachments are frontend placeholders; Vulkan framebuffer extent must match the swapchain.
        const IntVec2 defaultFramebufferExtent =
            ResolveRenderPassFramebufferExtent(isDefaultFbo, {0, 0, 0}, swapchainExtent);
        Int width = defaultFramebufferExtent.x();
        Int height = defaultFramebufferExtent.y();
        Vector<VkAttachmentDescription> attachmentDescriptions;
        attachmentDescriptions.reserve(colorAttachmentSlotCount + 1);
        // Keep the full GL draw buffer slot span so fragment outputs targeting GL_NONE map to VK_ATTACHMENT_UNUSED.
        Vector<VkAttachmentReference> colorAttachmentRefs(colorAttachmentSlotCount);
        for (auto& attachmentRef : colorAttachmentRefs) {
            attachmentRef.attachment = VK_ATTACHMENT_UNUSED;
            attachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        Vector<PendingClearAttachmentInfo> pendingClearAttachments;
        pendingClearAttachments.reserve(colorAttachmentSlotCount + 1);
        Vector<TrackedAttachmentLayoutInfo> trackedAttachmentLayouts;
        trackedAttachmentLayouts.reserve(colorAttachmentSlotCount + 1);
        auto& textureResources = RenderPassEntry::s_textureResourcesScratch;
        textureResources.clear();
        textureResources.reserve(colorAttachmentSlotCount + 1);
        Vector<VkImageView> attachmentViews;
        attachmentViews.reserve(colorAttachmentSlotCount + 1);
        VkSampleCountFlagBits renderPassSampleCount = VK_SAMPLE_COUNT_1_BIT;
        Bool hasRenderPassSampleCount = false;
        const auto adoptRenderPassSampleCount = [&](VkSampleCountFlagBits sampleCount,
                                                    const char* attachmentKind,
                                                    Int attachmentId) {
            if (!hasRenderPassSampleCount) {
                renderPassSampleCount = sampleCount;
                hasRenderPassSampleCount = true;
                return;
            }
            MOBILEGL_ASSERT(renderPassSampleCount == sampleCount,
                            "GetOrCreateRenderPass: mismatched sample count %d on %s attachment %d (expected %d)",
                            static_cast<Int>(sampleCount), attachmentKind, attachmentId,
                            static_cast<Int>(renderPassSampleCount));
        };
        // This should automatically work on default & offscreen FBO
        // assuming default FBO has the right param
        for (Uint32 i = 0; i < colorAttachmentSlotCount; ++i) {
            auto drawbuf = drawbufs[i];
            auto* texture = ResolveCompleteColorAttachmentTexture(fbo, drawbuf, i);
            if (texture == nullptr)
                continue;

            auto& att = fbo.GetAttachment(drawbuf);
            const Uint32 attachmentMipLevel = static_cast<Uint32>(std::max(att.GetTextureLevel(), 0));
            const auto textureTarget = texture->GetTarget();
            const Uint32 attachmentIndex = static_cast<Uint32>(attachmentDescriptions.size());
            attachmentDescriptions.emplace_back();

            // Color attachment description
            VkAttachmentDescription& desc = attachmentDescriptions.back();
            switch (textureTarget) {
                case TextureTarget::Texture2D:
                case TextureTarget::Texture2DMultisample: {
                    desc.flags = 0;
                    desc.format = isDefaultFbo ?
                        m_swapchainObject.GetSurfaceFormat().format :
                        MG_Util::ConvertTextureInternalFormatToVkEnum(
                            texture->GetFormat());
                    VkSampleCountFlagBits attachmentSampleCount = VK_SAMPLE_COUNT_1_BIT;
                    ClearAttachmentPayload clearPayload{};
                    Bool hasClear = m_clearManager.GetPendingClear(att, clearPayload);
                    VkImageLayout trackedColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    desc.loadOp = hasClear ?
                        VK_ATTACHMENT_LOAD_OP_CLEAR :
                        VK_ATTACHMENT_LOAD_OP_LOAD;
                    desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    desc.finalLayout = isDefaultFbo ?
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR :
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    if (hasClear) {
                        pendingClearAttachments.emplace_back(PendingClearAttachmentInfo {
                            .attachmentIndex = attachmentIndex,
                            .key = VkClearManager::MakePendingClearKey(att)
                        });
                    }
                    const IntVec2 attachmentExtent =
                        ResolveRenderPassFramebufferExtent(isDefaultFbo, att.GetSize(), swapchainExtent);
                    if (width == 0)
                        width = attachmentExtent.x();
                    if (height == 0)
                        height = attachmentExtent.y();

                    if (isDefaultFbo) {
                        const auto& swapchainViews = m_swapchainObject.GetImageViews();
                        MOBILEGL_ASSERT(swapchainImageIndex < swapchainViews.size(),
                                        "GetOrCreateRenderPass: swapchain image index out of range");
                        trackedColorLayout = m_swapchainObject.GetImageLayout(swapchainImageIndex);
                        trackedAttachmentLayouts.emplace_back(TrackedAttachmentLayoutInfo {
                            .target = TrackedAttachmentTarget::SwapchainColor,
                            .swapchainImageIndex = swapchainImageIndex,
                            .finalLayout = desc.finalLayout,
                        });
                        attachmentSampleCount = VK_SAMPLE_COUNT_1_BIT;
                        textureResources.emplace_back(nullptr);
                        attachmentViews.emplace_back(swapchainViews[swapchainImageIndex]);
                    } else {
                        auto* textureResource = m_textureManager.SyncTextureAndGetDescriptor(*texture);
                        MOBILEGL_ASSERT(textureResource,
                                        "GetOrCreateRenderPass: SyncTextureAndGetDescriptor failed at color attachment %d", i);
                        textureResources.emplace_back(textureResource);
                        desc.format = textureResource->format;
                        attachmentSampleCount = textureResource->sampleCount;
                        trackedColorLayout = textureResource->layout;
                        trackedAttachmentLayouts.emplace_back(TrackedAttachmentLayoutInfo {
                            .target = TrackedAttachmentTarget::Texture,
                            .texture = att.GetTexture(),
                            .textureMipLevel = attachmentMipLevel,
                            .finalLayout = desc.finalLayout,
                        });
                        const Uint32 baseArrayLayer = ResolveAttachmentBaseArrayLayer(att);
                        const Uint32 layerCount = ResolveAttachmentLayerCount(att);
                        const VkImageViewType attachmentViewType = ResolveAttachmentViewType(att, *textureResource);
                        attachmentViews.emplace_back(
                            m_textureManager.GetOrCreateAttachmentViewAtMipLevel(
                                *texture, attachmentMipLevel, baseArrayLayer, layerCount, attachmentViewType));
                        MOBILEGL_ASSERT(attachmentViews.back() != VK_NULL_HANDLE,
                                        "GetOrCreateRenderPass: GetOrCreateAttachmentView failed at color attachment %d", i);
                    }
                    desc.samples = attachmentSampleCount;
                    adoptRenderPassSampleCount(attachmentSampleCount, "color", texture->GetExternalIndex());

                    if (!hasClear && trackedColorLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                        MGLOG_W("GetOrCreateRenderPass: color attachment textureId=%d starts with undefined layout and no clear; "
                                "using LOAD_OP_DONT_CARE",
                                texture->GetExternalIndex());
                        desc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    }
                    desc.initialLayout = (hasClear || trackedColorLayout == VK_IMAGE_LAYOUT_UNDEFINED) ?
                        VK_IMAGE_LAYOUT_UNDEFINED :
                        trackedColorLayout;

                    break;
                }
                default:
                    MOBILEGL_ASSERT(false, "Unsupported texture target");
            }

            // Attachment reference
            VkAttachmentReference& attachmentRef = colorAttachmentRefs[i];
            attachmentRef.attachment = attachmentIndex;
            attachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        // Depth/stencil attachment description
        auto& depthAtt = fbo.GetAttachment(FramebufferAttachmentType::Depth);
        auto& stencilAtt = fbo.GetAttachment(FramebufferAttachmentType::Stencil);
        VkAttachmentDescription depthAttachmentDescription;
        VkAttachmentReference depthAttachmentRef;
        depthAttachmentRef.attachment = VK_ATTACHMENT_UNUSED;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkTextureManager::TextureResource* depthTextureResource = nullptr;
        RenderbufferResource* depthRenderbufferResource = nullptr;
        const auto isUsableDepthStencilAttachment = [](const auto& attachment) {
            return attachment.IsComplete() && (attachment.IsTexture() || attachment.IsRenderbuffer());
        };
        const auto sameDepthStencilAttachmentObject = [](const auto& a, const auto& b) {
            if (a.IsTexture() && b.IsTexture()) {
                return a.GetTexture().get() == b.GetTexture().get() &&
                       a.GetTextureUploadTarget() == b.GetTextureUploadTarget() &&
                       a.GetTextureLevel() == b.GetTextureLevel();
            }
            if (a.IsRenderbuffer() && b.IsRenderbuffer()) {
                return a.GetRenderbuffer().get() == b.GetRenderbuffer().get();
            }
            return false;
        };
        const auto* selectedDepthStencilAttachment = isUsableDepthStencilAttachment(depthAtt) ? &depthAtt :
                                                     (isUsableDepthStencilAttachment(stencilAtt) ? &stencilAtt : nullptr);
        const Bool hasDistinctDepthAndStencilAttachments =
            isUsableDepthStencilAttachment(depthAtt) && isUsableDepthStencilAttachment(stencilAtt) &&
            !sameDepthStencilAttachmentObject(depthAtt, stencilAtt);
        if (hasDistinctDepthAndStencilAttachments) {
            MGLOG_E("GetOrCreateRenderPass: separate depth/stencil attachments are not supported yet; using the depth attachment and ignoring the standalone stencil attachment for framebuffer %u",
                    fbo.GetExternalIndex());
        }
        if (selectedDepthStencilAttachment != nullptr) {
            const Uint32 depthAttachmentIndex = static_cast<Uint32>(attachmentDescriptions.size());
            ClearAttachmentPayload clearPayload{};
            Bool hasClear = selectedDepthStencilAttachment->IsTexture()
                ? m_clearManager.GetPendingClear(*selectedDepthStencilAttachment, clearPayload)
                : GetPendingRenderbufferClear(selectedDepthStencilAttachment->GetRenderbuffer().get(), clearPayload);
            Bool clearDepth = hasClear && (clearPayload.mask & GL_DEPTH_BUFFER_BIT) != 0;
            Bool clearStencil = hasClear && (clearPayload.mask & GL_STENCIL_BUFFER_BIT) != 0;
            VkImageLayout trackedDepthLayout = isDefaultFbo ?
                m_swapchainObject.GetDepthStencilImageLayout(swapchainImageIndex) :
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachmentDescription.flags = 0;
            VkSampleCountFlagBits depthAttachmentSampleCount = VK_SAMPLE_COUNT_1_BIT;
            Int depthAttachmentId = 0;
            IntVec2 attachmentExtent = {0, 0};
            if (isDefaultFbo) {
                depthAttachmentDescription.format = m_swapchainObject.GetDepthStencilFormat();
                depthAttachmentId = 0;
            } else if (selectedDepthStencilAttachment->IsTexture()) {
                auto& texture = *selectedDepthStencilAttachment->GetTexture();
                depthTextureResource = m_textureManager.SyncTextureAndGetDescriptor(texture);
                MOBILEGL_ASSERT(depthTextureResource,
                                "GetOrCreateRenderPass: SyncTextureAndGetDescriptor failed at depth attachment");
                trackedDepthLayout = depthTextureResource->layout;
                depthAttachmentDescription.format = depthTextureResource->format;
                depthAttachmentSampleCount = depthTextureResource->sampleCount;
                depthAttachmentId = static_cast<Int>(texture.GetExternalIndex());
                attachmentExtent =
                    ResolveRenderPassFramebufferExtent(isDefaultFbo, selectedDepthStencilAttachment->GetSize(),
                                                       swapchainExtent);
            } else {
                const auto& renderbuffer = selectedDepthStencilAttachment->GetRenderbuffer();
                depthRenderbufferResource = GetOrCreateRenderbufferResource(renderbuffer);
                MOBILEGL_ASSERT(depthRenderbufferResource,
                                "GetOrCreateRenderPass: GetOrCreateRenderbufferResource failed at depth attachment");
                trackedDepthLayout = depthRenderbufferResource->layout;
                depthAttachmentDescription.format = depthRenderbufferResource->format;
                depthAttachmentSampleCount = depthRenderbufferResource->sampleCount;
                depthAttachmentId = static_cast<Int>(renderbuffer->GetExternalIndex());
                attachmentExtent = {static_cast<Int>(depthRenderbufferResource->extent.width),
                                    static_cast<Int>(depthRenderbufferResource->extent.height)};
            }
            depthAttachmentDescription.samples = depthAttachmentSampleCount;
            adoptRenderPassSampleCount(depthAttachmentSampleCount, "depth/stencil", depthAttachmentId);
            const auto loadInfo =
                ResolveDepthStencilAttachmentLoadInfo(trackedDepthLayout, clearDepth, clearStencil);
            depthAttachmentDescription.loadOp = loadInfo.depthLoadOp;
            depthAttachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachmentDescription.stencilLoadOp = loadInfo.stencilLoadOp;
            depthAttachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachmentDescription.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachmentDescription.initialLayout = loadInfo.initialLayout;
            if (trackedDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED && (!clearDepth || !clearStencil)) {
                MGLOG_W("GetOrCreateRenderPass: depth/stencil attachment id=%d starts with undefined layout "
                        "and partial/no clear; using DONT_CARE for uncleared aspects",
                        depthAttachmentId);
            }
            if (hasClear) {
                if (selectedDepthStencilAttachment->IsTexture()) {
                    pendingClearAttachments.emplace_back(PendingClearAttachmentInfo {
                        .attachmentIndex = depthAttachmentIndex,
                        .key = VkClearManager::MakePendingClearKey(*selectedDepthStencilAttachment)
                    });
                } else {
                    pendingClearAttachments.emplace_back(PendingClearAttachmentInfo {
                        .attachmentIndex = depthAttachmentIndex,
                        .renderbuffer = selectedDepthStencilAttachment->GetRenderbuffer().get(),
                        .hasInlinePayload = true,
                        .inlinePayload = clearPayload
                    });
                }
            }
            if (isDefaultFbo) {
                trackedAttachmentLayouts.emplace_back(TrackedAttachmentLayoutInfo {
                    .target = TrackedAttachmentTarget::SwapchainDepthStencil,
                    .swapchainImageIndex = swapchainImageIndex,
                    .finalLayout = depthAttachmentDescription.finalLayout,
                });
                attachmentViews.emplace_back(m_swapchainObject.GetDepthStencilImageView(swapchainImageIndex));
            } else if (selectedDepthStencilAttachment->IsTexture()) {
                auto& texture = *selectedDepthStencilAttachment->GetTexture();
                const Uint32 attachmentMipLevel =
                    static_cast<Uint32>(std::max(selectedDepthStencilAttachment->GetTextureLevel(), 0));
                MOBILEGL_ASSERT(depthTextureResource->layout != VK_IMAGE_LAYOUT_UNDEFINED ||
                                    depthAttachmentDescription.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD,
                                "GetOrCreateRenderPass: depth attachment textureId=%d has undefined tracked layout with LOAD_OP_LOAD",
                                texture.GetExternalIndex());
                MOBILEGL_ASSERT(depthTextureResource->layout != VK_IMAGE_LAYOUT_UNDEFINED ||
                                    depthAttachmentDescription.stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD,
                                "GetOrCreateRenderPass: stencil attachment textureId=%d has undefined tracked layout with LOAD_OP_LOAD",
                                texture.GetExternalIndex());
                trackedAttachmentLayouts.emplace_back(TrackedAttachmentLayoutInfo {
                    .target = TrackedAttachmentTarget::Texture,
                    .texture = selectedDepthStencilAttachment->GetTexture(),
                    .textureMipLevel = attachmentMipLevel,
                    .finalLayout = depthAttachmentDescription.finalLayout,
                });
                textureResources.emplace_back(depthTextureResource);
                const Uint32 baseArrayLayer = ResolveAttachmentBaseArrayLayer(*selectedDepthStencilAttachment);
                const Uint32 layerCount = ResolveAttachmentLayerCount(*selectedDepthStencilAttachment);
                const VkImageViewType attachmentViewType =
                    ResolveAttachmentViewType(*selectedDepthStencilAttachment, *depthTextureResource);
                attachmentViews.emplace_back(
                    m_textureManager.GetOrCreateAttachmentViewAtMipLevel(
                        texture, attachmentMipLevel, baseArrayLayer, layerCount, attachmentViewType));
                MOBILEGL_ASSERT(attachmentViews.back() != VK_NULL_HANDLE,
                                "GetOrCreateRenderPass: GetOrCreateAttachmentView failed at depth attachment");
                if (width == 0 || height == 0) {
                    width = attachmentExtent.x();
                    height = attachmentExtent.y();
                }
            } else {
                const auto& renderbuffer = selectedDepthStencilAttachment->GetRenderbuffer();
                MOBILEGL_ASSERT(depthRenderbufferResource->layout != VK_IMAGE_LAYOUT_UNDEFINED ||
                                    depthAttachmentDescription.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD,
                                "GetOrCreateRenderPass: depth renderbuffer %u has undefined tracked layout with LOAD_OP_LOAD",
                                renderbuffer->GetExternalIndex());
                MOBILEGL_ASSERT(depthRenderbufferResource->layout != VK_IMAGE_LAYOUT_UNDEFINED ||
                                    depthAttachmentDescription.stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD,
                                "GetOrCreateRenderPass: stencil renderbuffer %u has undefined tracked layout with LOAD_OP_LOAD",
                                renderbuffer->GetExternalIndex());
                trackedAttachmentLayouts.emplace_back(TrackedAttachmentLayoutInfo {
                    .target = TrackedAttachmentTarget::Renderbuffer,
                    .renderbuffer = renderbuffer,
                    .finalLayout = depthAttachmentDescription.finalLayout,
                });
                textureResources.emplace_back(nullptr);
                attachmentViews.emplace_back(depthRenderbufferResource->view);
                if (width == 0 || height == 0) {
                    width = attachmentExtent.x();
                    height = attachmentExtent.y();
                }
            }
            attachmentDescriptions.emplace_back(depthAttachmentDescription);
            depthAttachmentRef.attachment = depthAttachmentIndex;
        }
        const Bool hasDepthStencilAttachment = depthAttachmentRef.attachment != VK_ATTACHMENT_UNUSED;

        // Subpass
        VkSubpassDescription subpassDesc;
        subpassDesc.flags = 0;
        subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDesc.inputAttachmentCount = 0;
        subpassDesc.pInputAttachments = nullptr;
        subpassDesc.colorAttachmentCount = colorAttachmentRefs.size();
        subpassDesc.pColorAttachments = colorAttachmentRefs.data();
        subpassDesc.pResolveAttachments = nullptr;
        subpassDesc.pDepthStencilAttachment = hasDepthStencilAttachment ? &depthAttachmentRef : VK_NULL_HANDLE;
        subpassDesc.preserveAttachmentCount = 0;
        subpassDesc.pPreserveAttachments = nullptr;

        // Render Pass
        VkRenderPassCreateInfo renderPassCreateInfo;
        renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.pNext = VK_NULL_HANDLE;
        renderPassCreateInfo.flags = 0;
        renderPassCreateInfo.attachmentCount = attachmentDescriptions.size();
        renderPassCreateInfo.pAttachments = attachmentDescriptions.data();
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpassDesc;
        renderPassCreateInfo.dependencyCount = 0;
        renderPassCreateInfo.pDependencies = nullptr;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateRenderPass(m_device, &renderPassCreateInfo, nullptr, &renderPass));

        // Framebuffer
        VkFramebufferCreateInfo framebufferCreateInfo;
        framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferCreateInfo.pNext = nullptr;
        framebufferCreateInfo.flags = 0;
        framebufferCreateInfo.renderPass = renderPass;
        framebufferCreateInfo.attachmentCount = attachmentViews.size();
        framebufferCreateInfo.pAttachments = attachmentViews.data();
        framebufferCreateInfo.width = width;
        framebufferCreateInfo.height = height;
        framebufferCreateInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateFramebuffer(m_device, &framebufferCreateInfo, nullptr, &framebuffer));
        IntVec2 extent = {width, height};
        RenderPassEntry renderPassEntry {
            hash,
            renderPass,
            framebuffer,
            compatibilityHash,
            Move(pendingClearAttachments),
            Move(trackedAttachmentLayouts),
            static_cast<Uint32>(attachmentViews.size()),
            static_cast<Uint32>(colorAttachmentRefs.size()),
            hasDepthStencilAttachment,
            renderPassSampleCount,
            extent,
            1 };
        MGLOG_D("VkRenderPassManager::GetOrCreateRenderPass: hash=0x%llx compatibilityHash=0x%llx attachmentCount=%u colorAttachmentCount=%u samples=%d extent=%dx%d",
                static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(compatibilityHash),
                renderPassEntry.attachmentCount,
                renderPassEntry.colorAttachmentCount,
                static_cast<Int>(renderPassEntry.sampleCount),
                extent.x(),
                extent.y());
        auto [insertedIt, _] = m_renderPasses.emplace(hash, Move(renderPassEntry));
        return insertedIt->second;
    }

    Bool VkRenderPassManager::BeginRenderPass(VkCommandBuffer commandBuffer, RenderPassEntry& renderPassEntry) {
        // TODO: Transition all the attachments into proper layout before starting the render pass
        VkRenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.pNext = nullptr;
        renderPassBeginInfo.renderPass = renderPassEntry.renderPass;
        renderPassBeginInfo.framebuffer = renderPassEntry.framebuffer;
        renderPassBeginInfo.renderArea.offset = { 0, 0 };
        renderPassBeginInfo.renderArea.extent = {
            (Uint32)renderPassEntry.extent.x(), (Uint32)renderPassEntry.extent.y() };

        Vector<VkClearValue> clearValues(renderPassEntry.attachmentCount);
        for (auto& clearValue: clearValues) {
            clearValue.color = {0.0f, 0.0f, 0.0f, 1.0f};
            clearValue.depthStencil = {1.0f, 0};
        }
        for (const auto& pending: renderPassEntry.pendingClearAttachments) {
            if (pending.attachmentIndex >= clearValues.size()) {
                continue;
            }
            ClearAttachmentPayload clearPayload{};
            SharedPtr<MG_State::GLState::ITextureObject> liveTexture;
            if (pending.hasInlinePayload) {
                clearPayload = pending.inlinePayload;
            } else {
                if (pending.key.texture == nullptr ||
                    !s_clearManager->GetPendingClear(pending.key, clearPayload, liveTexture)) {
                    continue;
                }
            }
            if ((clearPayload.mask & GL_COLOR_BUFFER_BIT) != 0) {
                clearValues[pending.attachmentIndex].color = {
                    clearPayload.color.x(),
                    clearPayload.color.y(),
                    clearPayload.color.z(),
                    ResolveColorClearAlpha(liveTexture.get(), clearPayload.color.w())
                };
            }
            if ((clearPayload.mask & GL_DEPTH_BUFFER_BIT) != 0) {
                clearValues[pending.attachmentIndex].depthStencil.depth = clearPayload.depth;
            }
            if ((clearPayload.mask & GL_STENCIL_BUFFER_BIT) != 0) {
                clearValues[pending.attachmentIndex].depthStencil.stencil = clearPayload.stencil;
            }
        }

        renderPassBeginInfo.clearValueCount = static_cast<Uint32>(clearValues.size());
        renderPassBeginInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        for (const auto& pending: renderPassEntry.pendingClearAttachments) {
            if (pending.hasInlinePayload) {
                if (s_renderPassManager != nullptr) {
                    s_renderPassManager->PopPendingRenderbufferClear(pending.renderbuffer);
                }
            } else {
                s_clearManager->PopPendingClear(pending.key);
            }
        }
        s_activeRenderPass.hash = renderPassEntry.hash;
        s_activeRenderPass.compatibilityHash = renderPassEntry.compatibilityHash;
        s_activeRenderPass.trackedAttachmentLayouts = renderPassEntry.trackedAttachmentLayouts;
        s_activeRenderPass.extent = renderPassEntry.extent;
        s_hasActiveRenderPass = true;

        return true;
    }

    Bool VkRenderPassManager::EndRenderPass(VkCommandBuffer commandBuffer) {
        auto* activeRenderPass = GetActiveRenderPass();
        vkCmdEndRenderPass(commandBuffer);
        if (activeRenderPass != nullptr && !activeRenderPass->trackedAttachmentLayouts.empty()) {
            for (const auto& trackedAttachment : activeRenderPass->trackedAttachmentLayouts) {
                switch (trackedAttachment.target) {
                    case TrackedAttachmentTarget::Texture:
                        MOBILEGL_ASSERT(s_textureManager != nullptr, "EndRenderPass: texture manager is null");
                        if (const auto texture = trackedAttachment.texture.lock()) {
                            s_textureManager->UpdateTrackedImageLayoutAfterAttachmentWrite(
                                commandBuffer,
                                texture.get(),
                                trackedAttachment.textureMipLevel,
                                trackedAttachment.finalLayout);
                        }
                        break;
                    case TrackedAttachmentTarget::Renderbuffer:
                        MOBILEGL_ASSERT(s_renderPassManager != nullptr, "EndRenderPass: render pass manager is null");
                        if (const auto renderbuffer = trackedAttachment.renderbuffer.lock()) {
                            auto resourceIt =
                                s_renderPassManager->m_renderbufferResources.find(renderbuffer.get());
                            if (resourceIt != s_renderPassManager->m_renderbufferResources.end()) {
                                resourceIt->second.layout = trackedAttachment.finalLayout;
                            }
                        }
                        break;
                    case TrackedAttachmentTarget::SwapchainColor:
                        MOBILEGL_ASSERT(s_swapchainObject != nullptr, "EndRenderPass: swapchain object is null");
                        s_swapchainObject->SetImageLayout(trackedAttachment.swapchainImageIndex, trackedAttachment.finalLayout);
                        break;
                    case TrackedAttachmentTarget::SwapchainDepthStencil:
                        MOBILEGL_ASSERT(s_swapchainObject != nullptr, "EndRenderPass: swapchain object is null");
                        s_swapchainObject->SetDepthStencilImageLayout(trackedAttachment.swapchainImageIndex,
                                                                      trackedAttachment.finalLayout);
                        break;
                    default:
                        MOBILEGL_ASSERT(false, "EndRenderPass: unsupported tracked attachment target=%d",
                                        static_cast<Int>(trackedAttachment.target));
                        break;
                }
            }
        }
        s_activeRenderPass = {};
        s_hasActiveRenderPass = false;
        return true;
    }

    ActiveRenderPassInfo* VkRenderPassManager::GetActiveRenderPass() {
        return s_hasActiveRenderPass ? &s_activeRenderPass : nullptr;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
