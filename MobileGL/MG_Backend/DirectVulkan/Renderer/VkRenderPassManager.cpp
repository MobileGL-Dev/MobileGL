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
        // GL promises "at least the requested samples", so a non-power-of-two
        // request (legal in GL, e.g. 3) rounds up to the next Vulkan bit.
        if (requestedSamples <= 1) {
            outSampleCount = VK_SAMPLE_COUNT_1_BIT;
            return true;
        }
        if (requestedSamples > 64) {
            return false;
        }
        Uint32 bit = 1;
        while (bit < static_cast<Uint32>(requestedSamples)) {
            bit <<= 1;
        }
        outSampleCount = static_cast<VkSampleCountFlagBits>(bit);
        return true;
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

    static Bool ColorFormatLacksAlpha(const MG_State::GLState::ITextureObject* texture) {
        return texture != nullptr && MG_Util::GetBaseInternalFormatComponentCount(texture->GetFormat()) == 3;
    }

    [[maybe_unused]] static Float ResolveColorClearAlpha(const MG_State::GLState::ITextureObject* texture, Float requestedAlpha) {
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
        if (attachment.IsLayered()) {
            return 0;
        }
        const TextureUploadTarget uploadTarget = attachment.GetTextureUploadTarget();
        if (!IsCubeMapFaceUploadTarget(uploadTarget)) {
            return static_cast<Uint32>(std::max(attachment.GetTextureLayer(), 0));
        }
        return static_cast<Uint32>(uploadTarget) - static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
    }

    static Uint32 ResolveAttachmentLayerCount(const MG_State::GLState::FramebufferAttachmentObject& attachment) {
        if (attachment.IsLayered()) {
            return static_cast<Uint32>(std::max(attachment.GetSize().z(), 1));
        }
        return 1u;
    }

    static VkImageViewType ResolveAttachmentViewType(
        const MG_State::GLState::FramebufferAttachmentObject& attachment,
        const VkTextureManager::TextureResource& resource) {
        if (attachment.IsLayered()) {
            return resource.viewType;
        }
        // A non-layered attachment names ONE layer, so the view over it is a plain 2D view whatever
        // the image's own view type is. The cube-face upload targets always meant this; a cube map
        // array attached through glFramebufferTextureLayer means it too, and a CUBE_ARRAY view over
        // a single layer is not a legal attachment. The CUBE arm is inert today - no frontend path
        // produces a non-layered cube attachment without a face upload target - and is kept for
        // symmetry with CUBE_ARRAY.
        if (IsCubeMapFaceUploadTarget(attachment.GetTextureUploadTarget()) ||
            resource.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY || resource.viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
            return VK_IMAGE_VIEW_TYPE_2D;
        }
        return resource.viewType;
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
            MGLOG_W_ONCE("GetOrCreateRenderPass: draw buffer slot %u (%s) on FBO %u has an incomplete texture attachment; using VK_ATTACHMENT_UNUSED",
                    drawBufferIndex,
                    MG_Util::ConvertFramebufferAttachmentTypeToString(attachmentType).c_str(),
                    fbo.GetExternalIndex());
            return nullptr;
        }

        auto* texture = attachment.GetTexture().get();
        if (texture == nullptr) {
            MGLOG_W_ONCE("GetOrCreateRenderPass: draw buffer slot %u (%s) on FBO %u resolved to a null texture; using VK_ATTACHMENT_UNUSED",
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
        if (unormTwinView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, unormTwinView, nullptr);
        }
        if (image != VK_NULL_HANDLE && allocation != nullptr) {
            vmaDestroyImage(allocator, image, allocation);
        }
        renderbuffer.reset();
        image = VK_NULL_HANDLE;
        allocation = nullptr;
        view = VK_NULL_HANDLE;
        unormTwinView = VK_NULL_HANDLE;
        layout = VK_IMAGE_LAYOUT_UNDEFINED;
        format = VK_FORMAT_UNDEFINED;
        aspect = VK_IMAGE_ASPECT_NONE;
        extent = {0, 0};
        sampleCount = VK_SAMPLE_COUNT_1_BIT;
        internalFormat = TextureInternalFormat::Unknown;
        samples = 0;
        deadSinceFrame = kNeverObservedDead;
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
        CollectDeferredRenderbufferReleases(/*destroyAll=*/true); // caller guarantees device idle
        m_pendingRenderbufferClears.clear();
        RenderPassEntry::s_textureResourcesScratch.clear();
        s_activeRenderPass = {};
        s_hasActiveRenderPass = false;
        m_rpFastValid = false;
    }

    Uint64 VkRenderPassManager::RetireAgeFrames() const {
        // MaxFramesInFlight + 2 covers the frame ring plus one boundary for the
        // recording-to-submit gap and one because OnPresent runs ahead of Present's
        // fence wait; the floor of 8 keeps a margin over the default ring of 3 while
        // still releasing multi-MB attachment memory promptly (the render-pass cache's
        // 1024-frame retirement would pin it for no additional safety).
        return std::max<Uint64>(8, static_cast<Uint64>(m_config.MaxFramesInFlight) + 2);
    }

    void VkRenderPassManager::DeferRenderbufferBackingRelease(RenderbufferResource& resource) {
        // The superseded backing may still be referenced by in-flight command buffers
        // (glRenderbufferStorage can respecify a renderbuffer drawn this very frame),
        // so it is parked and destroyed only after RetireAgeFrames() boundaries.
        if (resource.image == VK_NULL_HANDLE && resource.view == VK_NULL_HANDLE) {
            return;
        }
        m_deferredRenderbufferReleases.push_back(
            {resource.image, resource.allocation, resource.view, resource.unormTwinView, m_frameCounter});
        resource.image = VK_NULL_HANDLE;
        resource.allocation = nullptr;
        resource.view = VK_NULL_HANDLE;
        resource.unormTwinView = VK_NULL_HANDLE;
    }

    void VkRenderPassManager::CollectDeferredRenderbufferReleases(Bool destroyAll) {
        if (m_deferredRenderbufferReleases.empty()) {
            return;
        }
        const Uint64 retireAgeFrames = RetireAgeFrames();
        std::erase_if(m_deferredRenderbufferReleases, [&](DeferredRenderbufferRelease& release) {
            if (!destroyAll && m_frameCounter - release.deferredAtFrame < retireAgeFrames) {
                return false;
            }
            if (release.view != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, release.view, nullptr);
            }
            if (release.unormTwinView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, release.unormTwinView, nullptr);
            }
            if (release.image != VK_NULL_HANDLE) {
                vmaDestroyImage(m_allocator, release.image, release.allocation);
            }
            return true;
        });
    }

    void VkRenderPassManager::CollectRenderbufferGarbage() {
        // Two-phase reclamation: a dead renderbuffer's VkImage may still be referenced by
        // command buffers submitted up to frames-in-flight frames ago (it was legally
        // attached and drawn right up to its deletion), so the first observation of an
        // expired weak reference only stamps the current frame counter; Destroy runs once
        // enough frame boundaries have passed that the stamping frame's submission fence
        // has provably been waited (see RetireAgeFrames).
        const Uint64 retireAgeFrames = RetireAgeFrames();
        for (auto it = m_renderbufferResources.begin(); it != m_renderbufferResources.end();) {
            auto& resource = it->second;
            const auto liveRenderbuffer = resource.renderbuffer.lock();
            if (liveRenderbuffer && liveRenderbuffer.get() == it->first) {
                resource.deadSinceFrame = RenderbufferResource::kNeverObservedDead;
                ++it;
                continue;
            }
            if (resource.deadSinceFrame == RenderbufferResource::kNeverObservedDead) {
                resource.deadSinceFrame = m_frameCounter;
                ++it;
                continue;
            }
            if (m_frameCounter - resource.deadSinceFrame < retireAgeFrames) {
                ++it;
                continue;
            }
            m_pendingRenderbufferClears.erase(it->first);
            resource.Destroy(m_device, m_allocator);
            it = m_renderbufferResources.erase(it);
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
            MGLOG_E_ONCE("GetOrCreateRenderbufferResource: unsupported renderbuffer sample count %d for renderbuffer %u",
                    renderbuffer->GetSamples(),
                    renderbuffer->GetExternalIndex());
            return nullptr;
        }

        const auto internalFormat = renderbuffer->GetInternalFormat();
        // Three-channel color formats widen to their RGBA twin exactly like textures do
        // (VkTextureManager::ResolveTextureFormatInfo): blits/resolves between a
        // renderbuffer and a texture of the same GL format then see one VkFormat.
        const VkFormat format = [&]() -> VkFormat {
            switch (internalFormat) {
            case TextureInternalFormat::RGB:
            case TextureInternalFormat::RGB8:
            case TextureInternalFormat::R3G3B2:
            case TextureInternalFormat::RGB4:
            case TextureInternalFormat::RGB5:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureInternalFormat::SRGB8:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case TextureInternalFormat::RGB8Snorm:
                return VK_FORMAT_R8G8B8A8_SNORM;
            case TextureInternalFormat::RGB10:
            case TextureInternalFormat::RGB12:
            case TextureInternalFormat::RGB16:
                return VK_FORMAT_R16G16B16A16_UNORM;
            case TextureInternalFormat::RGB16Snorm:
                return VK_FORMAT_R16G16B16A16_SNORM;
            case TextureInternalFormat::RGB16F:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case TextureInternalFormat::RGB32F:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            case TextureInternalFormat::RGB8I:
                return VK_FORMAT_R8G8B8A8_SINT;
            case TextureInternalFormat::RGB8UI:
                return VK_FORMAT_R8G8B8A8_UINT;
            case TextureInternalFormat::RGB16I:
                return VK_FORMAT_R16G16B16A16_SINT;
            case TextureInternalFormat::RGB16UI:
                return VK_FORMAT_R16G16B16A16_UINT;
            case TextureInternalFormat::RGB32I:
                return VK_FORMAT_R32G32B32A32_SINT;
            case TextureInternalFormat::RGB32UI:
                return VK_FORMAT_R32G32B32A32_UINT;
            default:
                return MG_Util::ConvertTextureInternalFormatToVkEnum(internalFormat);
            }
        }();
        const VkImageAspectFlags aspect = ResolveImageAspectMaskForFormat(format);
        // Renderbuffers are never sampled (GL has no way to bind one to a sampler), so the
        // usage set is attachment + transfer: transfer covers readback (vkCmdCopyImageToBuffer),
        // BlitFramebuffer, CopyTexImage sources, and out-of-render-pass clear materialization.
        const VkImageUsageFlags imageUsage =
            ((aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                       : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        // GL allows the implementation to allocate more samples than requested
        // (glRenderbufferStorageMultisample only promises "at least"), and devices
        // like llvmpipe expose 1x/4x but not 2x. Round the request up to the
        // nearest supported count for this format.
        if (renderbuffer->GetSamples() > 0) {
            auto supportedIt = m_attachmentSampleCountsByFormat.find(format);
            if (supportedIt == m_attachmentSampleCountsByFormat.end()) {
                VkImageFormatProperties formatProperties{};
                VkSampleCountFlags supported = VK_SAMPLE_COUNT_1_BIT;
                if (vkGetPhysicalDeviceImageFormatProperties(m_physicalDevice, format, VK_IMAGE_TYPE_2D,
                                                             VK_IMAGE_TILING_OPTIMAL, imageUsage, 0,
                                                             &formatProperties) == VK_SUCCESS) {
                    supported = formatProperties.sampleCounts;
                }
                supportedIt = m_attachmentSampleCountsByFormat.emplace(format, supported).first;
            }
            const VkSampleCountFlags supported = supportedIt->second;
            if ((supported & sampleCount) == 0) {
                // Smallest supported count above the request, else the largest below it.
                Uint32 rounded = 0;
                for (Uint32 bit = static_cast<Uint32>(sampleCount) << 1; bit <= VK_SAMPLE_COUNT_64_BIT; bit <<= 1) {
                    if ((supported & bit) != 0) {
                        rounded = bit;
                        break;
                    }
                }
                if (rounded == 0) {
                    for (Uint32 bit = static_cast<Uint32>(sampleCount) >> 1; bit != 0; bit >>= 1) {
                        if ((supported & bit) != 0) {
                            rounded = bit;
                            break;
                        }
                    }
                }
                if (rounded != 0) {
                    sampleCount = static_cast<VkSampleCountFlagBits>(rounded);
                }
            }
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
            // A new renderbuffer at a recycled address may adopt a compatible entry that
            // was already stamped dead; it is alive again, so cancel the aging.
            resource.deadSinceFrame = RenderbufferResource::kNeverObservedDead;
            return &resource;
        }

        // Respecify: park the old backing for aged destruction instead of destroying
        // inline - it may still be referenced by in-flight command buffers.
        DeferRenderbufferBackingRelease(resource);
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
        imageInfo.usage = imageUsage;
        imageInfo.samples = sampleCount;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // sRGB renderbuffers attach through their UNORM twin while GL_FRAMEBUFFER_SRGB
        // is disabled, which needs a format-reinterpreting second view.
        const Bool hasUnormTwin = ResolveSrgbAttachmentWriteFormat(format, false) != format;
        if (hasUnormTwin) {
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }

        VkImageFormatProperties imageFormatProperties{};
        const VkResult imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
            m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage, imageInfo.flags,
            &imageFormatProperties);
        if (imageFormatResult != VK_SUCCESS || (imageFormatProperties.sampleCounts & sampleCount) == 0) {
            MGLOG_E_ONCE("GetOrCreateRenderbufferResource: unsupported renderbuffer format=%d samples=%d for renderbuffer %u",
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
        ++m_renderbufferImageEpoch; // a new attachment image invalidates cached render passes

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
        if (hasUnormTwin) {
            viewInfo.format = ResolveSrgbAttachmentWriteFormat(format, false);
            VK_VERIFY(vkCreateImageView(m_device, &viewInfo, nullptr, &resource.unormTwinView),
                      "vkCreateImageView(renderbuffer unorm twin)");
        }

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
            // The whole colour description, not just the float vector: an integer clear keeps its
            // value in colorInt/colorUint, and dropping the encoding here would leave the pending
            // clear reading as an all-zero float one.
            pending.payload.color = clearPayload.color;
            pending.payload.colorEncoding = clearPayload.colorEncoding;
            pending.payload.colorInt = clearPayload.colorInt;
            pending.payload.colorUint = clearPayload.colorUint;
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
        if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
            // Color renderbuffer draw buffers take the framebuffer-level clear too; texture
            // attachments are skipped by the per-attachment overload's IsRenderbuffer guard.
            for (const auto attachmentType : drawFbo.GetDrawBuffers()) {
                if (attachmentType == FramebufferAttachmentType::None) {
                    continue;
                }
                QueueRenderbufferClear(
                    ClearAttachmentPayload{.mask = GL_COLOR_BUFFER_BIT, .color = clearPayload.color},
                    drawFbo.GetAttachment(attachmentType));
            }
        }
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
        const MG_State::GLState::FramebufferObject& fbo, Uint32 swapchainImageIndex, Bool includePendingClear,
        Bool includeDefaultFboDepthStencil) {
        XXHASH_VERIFY(XXH64_reset(m_hashState, m_config.CacheVersion));
        const Bool isDefaultFbo = fbo.IsDefaultFramebuffer();
        if (isDefaultFbo) {
            XXHASH_VERIFY(XXH64_update(m_hashState, &swapchainImageIndex, sizeof(swapchainImageIndex)));
        }
        // sRGB attachments switch between their sRGB and UNORM-twin views with this
        // capability (ResolveSrgbAttachmentWriteFormat), changing the render pass formats.
        const Bool framebufferSrgbEnabled =
            MG_State::pGLContext->IsCapabilityEnabled(MobileGL::CapabilityInput::FramebufferSrgb);
        XXHASH_VERIFY(XXH64_update(m_hashState, &framebufferSrgbEnabled, sizeof(framebufferSrgbEnabled)));
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
                const Int textureLayer = att.GetTextureLayer();
                XXHASH_VERIFY(XXH64_update(m_hashState, &textureLayer, sizeof(textureLayer)));
                const Bool textureLayered = att.IsLayered();
                XXHASH_VERIFY(XXH64_update(m_hashState, &textureLayered, sizeof(textureLayered)));

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
                        // Content validity feeds the attachment's loadOp (see the
                        // creation path), so it must key the cache as well.
                        if (!m_swapchainObject.IsImageContentDefined(swapchainImageIndex)) {
                            currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        }
                    } else if (attachment == FramebufferAttachmentType::Depth ||
                               attachment == FramebufferAttachmentType::Stencil) {
                        currentLayout = m_swapchainObject.GetDepthStencilImageLayout(swapchainImageIndex);
                        if (!m_swapchainObject.IsDepthStencilContentDefined(swapchainImageIndex)) {
                            currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        }
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

        // The depth-less default-FBO flavor omits the depth/stencil attachment
        // entirely, so it must hash differently from the depth-full flavor.
        const Bool depthStencilIncluded = !isDefaultFbo || includeDefaultFboDepthStencil;
        XXHASH_VERIFY(XXH64_update(m_hashState, &depthStencilIncluded, sizeof(depthStencilIncluded)));
        if (depthStencilIncluded) {
            combineFramebufferAttachmentObjHash(FramebufferAttachmentType::Depth);
            combineFramebufferAttachmentObjHash(FramebufferAttachmentType::Stencil);
        }

        return XXH64_digest(m_hashState);
    }

    RenderPassEntry& VkRenderPassManager::GetOrCreateRenderPass(const MG_State::GLState::FramebufferObject& fbo,
                                                                Uint32 swapchainImageIndex,
                                                                Bool drawUsesDepthStencil) {
        // Resolve the default-FBO depth flavor (see the header comment): keep the
        // depth attachment when the caller needs it, when a depth/stencil clear is
        // pending, or when the active pass already carries it (escalate-only, so
        // alternating depth-less draws never split an established depth pass).
        Bool includeDefaultFboDepthStencil = true;
        if (fbo.IsDefaultFramebuffer()) {
            Bool activeDefaultHasDepthStencil = false;
            if (const auto* active = GetActiveRenderPass()) {
                Bool activeIsSwapchainPass = false;
                Bool activeHasSwapchainDepthStencil = false;
                for (const auto& tracked : active->trackedAttachmentLayouts) {
                    activeIsSwapchainPass |= tracked.target == TrackedAttachmentTarget::SwapchainColor;
                    activeHasSwapchainDepthStencil |=
                        tracked.target == TrackedAttachmentTarget::SwapchainDepthStencil;
                }
                activeDefaultHasDepthStencil = activeIsSwapchainPass && activeHasSwapchainDepthStencil;
            }
            const auto& defaultDepthAtt = fbo.GetAttachment(FramebufferAttachmentType::Depth);
            const auto& defaultStencilAtt = fbo.GetAttachment(FramebufferAttachmentType::Stencil);
            const Bool pendingDepthStencilClear =
                (defaultDepthAtt.IsTexture() && m_clearManager.HasPendingClear(defaultDepthAtt)) ||
                HasPendingRenderbufferClear(defaultDepthAtt) ||
                (defaultStencilAtt.IsTexture() && m_clearManager.HasPendingClear(defaultStencilAtt)) ||
                HasPendingRenderbufferClear(defaultStencilAtt);
            includeDefaultFboDepthStencil =
                drawUsesDepthStencil || activeDefaultHasDepthStencil || pendingDepthStencilClear;
        }

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

        // Dirty-flag state tracking: when the framebuffer state is provably unchanged since the
        // render pass was last resolved, the active render pass is still valid -> skip the
        // expensive per-draw ComputeHash (XXH64 over every attachment + a SyncTexture per
        // attachment). Correctness signals: same FBO object + GetObjectVersion (attachment /
        // draw-buffer / read-buffer changes bump it), same swapchain image, no attachment VkImage
        // recreated since (texture + renderbuffer image epochs), and no pending clear (which alters
        // load ops). Any of these differing forces the full recompute below. Portable to VK 1.1.
        if (activeRenderPass != nullptr && m_rpFastValid && m_rpFastFbo == &fbo &&
            m_rpFastFboLifetimeId == fbo.GetLifetimeId() &&
            m_rpFastFboVersion == fbo.GetObjectVersion() && m_rpFastSwapchainIndex == swapchainImageIndex &&
            m_rpFastTexEpoch == m_textureManager.GetTextureImageEpoch() &&
            m_rpFastRbEpoch == m_renderbufferImageEpoch &&
            (!fbo.IsDefaultFramebuffer() || m_rpFastHadDepthStencil == includeDefaultFboDepthStencil) &&
            m_rpFastRenderPassHash == activeRenderPass->hash && !hasPendingClearOnFramebuffer()) {
            auto activeIt = m_renderPasses.find(activeRenderPass->hash);
            if (activeIt != m_renderPasses.end()) {
                activeIt->second.lastUsedFrame = m_frameCounter;
                return activeIt->second;
            }
        }

        auto compatibilityHash = ComputeHash(fbo, swapchainImageIndex, false, includeDefaultFboDepthStencil);
        if (activeRenderPass != nullptr &&
            activeRenderPass->CompatibleWith(compatibilityHash) &&
            !hasPendingClearOnFramebuffer()) {
            auto activeIt = m_renderPasses.find(activeRenderPass->hash);
            MOBILEGL_ASSERT(activeIt != m_renderPasses.end(),
                            "GetOrCreateRenderPass: active render pass hash=0x%llx is missing from cache",
                            static_cast<unsigned long long>(activeRenderPass->hash));
            // Populate the fast-path memo so subsequent unchanged draws skip ComputeHash. Read the
            // epochs AFTER ComputeHash: its attachment SyncTexture can create an image (bump the epoch).
            m_rpFastValid = true;
            m_rpFastFbo = &fbo;
            m_rpFastFboLifetimeId = fbo.GetLifetimeId();
            m_rpFastFboVersion = fbo.GetObjectVersion();
            m_rpFastSwapchainIndex = swapchainImageIndex;
            m_rpFastTexEpoch = m_textureManager.GetTextureImageEpoch();
            m_rpFastRbEpoch = m_renderbufferImageEpoch;
            m_rpFastRenderPassHash = activeRenderPass->hash;
            m_rpFastHadDepthStencil = activeIt->second.hasDepthStencilAttachment;
            activeIt->second.lastUsedFrame = m_frameCounter;
            return activeIt->second;
        }
        auto hash = ComputeHash(fbo, swapchainImageIndex, true, includeDefaultFboDepthStencil);
        auto it = m_renderPasses.find(hash);
        if (it != m_renderPasses.end()) {
            it->second.lastUsedFrame = m_frameCounter;
            return it->second;
        }

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
        Uint32 framebufferLayers = 1;
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

            // Renderbuffer color attachments mirror the texture path below, with the
            // resource (image/view/format/layout) coming from the render-pass manager's
            // renderbuffer store instead of the texture manager.
            if (drawbuf != FramebufferAttachmentType::None && !isDefaultFbo) {
                const auto& rbAtt = fbo.GetAttachment(drawbuf);
                if (rbAtt.IsRenderbuffer() && rbAtt.IsComplete()) {
                    const auto& renderbuffer = rbAtt.GetRenderbuffer();
                    auto* rbResource = GetOrCreateRenderbufferResource(renderbuffer);
                    if (rbResource == nullptr || (rbResource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
                        MGLOG_E_ONCE("GetOrCreateRenderPass: draw buffer slot %u on FBO %u has an unsupported color "
                                "renderbuffer %u; using VK_ATTACHMENT_UNUSED",
                                i, fbo.GetExternalIndex(), renderbuffer->GetExternalIndex());
                        continue;
                    }

                    const Uint32 rbAttachmentIndex = static_cast<Uint32>(attachmentDescriptions.size());
                    attachmentDescriptions.emplace_back();
                    VkAttachmentDescription& rbDesc = attachmentDescriptions.back();

                    ClearAttachmentPayload rbClearPayload{};
                    Bool rbHasClear = GetPendingRenderbufferClear(renderbuffer.get(), rbClearPayload) &&
                                      (rbClearPayload.mask & GL_COLOR_BUFFER_BIT) != 0;
                    if (rbHasClear &&
                        MG_Util::GetBaseInternalFormatComponentCount(renderbuffer->GetInternalFormat()) == 3) {
                        // RGB renderbuffers are backed by an RGBA image; the missing alpha reads as 1.
                        ForceOpaqueClearAlpha(rbClearPayload);
                    }

                    const VkImageLayout trackedRbLayout = rbResource->layout;
                    const Bool rbFramebufferSrgb =
                        MG_State::pGLContext->IsCapabilityEnabled(MobileGL::CapabilityInput::FramebufferSrgb);
                    const VkFormat rbAttachmentFormat =
                        ResolveSrgbAttachmentWriteFormat(rbResource->format, rbFramebufferSrgb);
                    rbDesc.flags = 0;
                    rbDesc.format = rbAttachmentFormat;
                    rbDesc.samples = rbResource->sampleCount;
                    rbDesc.loadOp = rbHasClear ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                    (trackedRbLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                                                  : VK_ATTACHMENT_LOAD_OP_LOAD);
                    rbDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    rbDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    rbDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    rbDesc.initialLayout = (rbHasClear || trackedRbLayout == VK_IMAGE_LAYOUT_UNDEFINED) ?
                        VK_IMAGE_LAYOUT_UNDEFINED : trackedRbLayout;
                    rbDesc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    adoptRenderPassSampleCount(rbResource->sampleCount, "color",
                                               static_cast<Int>(renderbuffer->GetExternalIndex()));

                    if (rbHasClear) {
                        pendingClearAttachments.emplace_back(PendingClearAttachmentInfo {
                            .attachmentIndex = rbAttachmentIndex,
                            .colorAttachmentSlot = i,
                            .renderbuffer = renderbuffer.get(),
                            .hasInlinePayload = true,
                            .inlinePayload = rbClearPayload,
                        });
                    }

                    if (width == 0)
                        width = static_cast<Int>(rbResource->extent.width);
                    if (height == 0)
                        height = static_cast<Int>(rbResource->extent.height);

                    trackedAttachmentLayouts.emplace_back(TrackedAttachmentLayoutInfo {
                        .target = TrackedAttachmentTarget::Renderbuffer,
                        .renderbuffer = renderbuffer,
                        .finalLayout = rbDesc.finalLayout,
                    });
                    textureResources.emplace_back(nullptr);
                    attachmentViews.emplace_back(rbAttachmentFormat != rbResource->format ? rbResource->unormTwinView
                                                                                          : rbResource->view);
                    MOBILEGL_ASSERT(attachmentViews.back() != VK_NULL_HANDLE,
                                    "GetOrCreateRenderPass: renderbuffer view missing at color attachment %d", i);

                    colorAttachmentRefs[i].attachment = rbAttachmentIndex;
                    continue;
                }
            }

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
                case TextureTarget::Texture1D:
                case TextureTarget::Texture1DArray:
                case TextureTarget::Texture2D:
                case TextureTarget::Texture2DArray:
                case TextureTarget::Texture2DMultisample:
                case TextureTarget::Texture2DMultisampleArray:
                case TextureTarget::Texture3D:
                case TextureTarget::TextureCubeMap:
                case TextureTarget::TextureCubeMapArray:
                case TextureTarget::TextureRectangle: {
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
                            .colorAttachmentSlot = i,
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
                        // EGL: a presented color buffer's content is undefined when its
                        // image comes back around (EGL_BUFFER_DESTROYED, the default
                        // swap behaviour) - skip the tile load instead of reloading
                        // stale pixels nobody may rely on.
                        if (!hasClear && !m_swapchainObject.IsImageContentDefined(swapchainImageIndex)) {
                            trackedColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        }
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
                        desc.format = ResolveSrgbAttachmentWriteFormat(
                            textureResource->format,
                            MG_State::pGLContext->IsCapabilityEnabled(MobileGL::CapabilityInput::FramebufferSrgb));
                        attachmentSampleCount = textureResource->sampleCount;
                        trackedColorLayout = textureResource->layout;
                        trackedAttachmentLayouts.emplace_back(TrackedAttachmentLayoutInfo {
                            .target = TrackedAttachmentTarget::Texture,
                            .texture = att.GetTexture(),
                            .textureRaw = att.GetTexture().get(),
                            .textureMipLevel = attachmentMipLevel,
                            .finalLayout = desc.finalLayout,
                        });
                        const Uint32 baseArrayLayer = ResolveAttachmentBaseArrayLayer(att);
                        const Uint32 layerCount = ResolveAttachmentLayerCount(att);
                        framebufferLayers = std::max(framebufferLayers, layerCount);
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
                        MGLOG_W_ONCE("GetOrCreateRenderPass: color attachment textureId=%d starts with undefined layout and no clear; "
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
        // Depth-less default-FBO flavor: nothing in this pass touches depth/stencil
        // and their content is undefined anyway (EGL swap), so drop the attachment
        // and its whole tile load + store.
        if (isDefaultFbo && !includeDefaultFboDepthStencil) {
            selectedDepthStencilAttachment = nullptr;
        }
        const Bool hasDistinctDepthAndStencilAttachments =
            isUsableDepthStencilAttachment(depthAtt) && isUsableDepthStencilAttachment(stencilAtt) &&
            !sameDepthStencilAttachmentObject(depthAtt, stencilAtt);
        if (hasDistinctDepthAndStencilAttachments) {
            MGLOG_E_ONCE("GetOrCreateRenderPass: separate depth/stencil attachments are not supported yet; using the depth attachment and ignoring the standalone stencil attachment for framebuffer %u",
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
            // EGL 1.5 §3.10.1: every ancillary (depth/stencil) buffer's content is
            // undefined after a swap, so the first default-FBO pass of a frame can
            // skip the depth/stencil tile load outright.
            if (isDefaultFbo && !m_swapchainObject.IsDepthStencilContentDefined(swapchainImageIndex)) {
                trackedDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
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
                MGLOG_W_ONCE("GetOrCreateRenderPass: depth/stencil attachment id=%d starts with undefined layout "
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
                    .textureRaw = selectedDepthStencilAttachment->GetTexture().get(),
                    .textureMipLevel = attachmentMipLevel,
                    .finalLayout = depthAttachmentDescription.finalLayout,
                });
                textureResources.emplace_back(depthTextureResource);
                const Uint32 baseArrayLayer = ResolveAttachmentBaseArrayLayer(*selectedDepthStencilAttachment);
                const Uint32 layerCount = ResolveAttachmentLayerCount(*selectedDepthStencilAttachment);
                framebufferLayers = std::max(framebufferLayers, layerCount);
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

        // Declare only the used colour-reference span. The GL draw-buffer array
        // always spans 8 slots, so passes used to declare colorAttachmentCount=8
        // with trailing VK_ATTACHMENT_UNUSED holes - and Adreno configures its
        // per-pixel render-backend/export path from the DECLARED count, so every
        // fragment of every pass paid the 8-target export cost (measured on
        // Adreno 650 / MC 26.2: 11.9 -> 7.5 ms of GPU time per frame, with the
        // single-quad swapchain blit pass alone dropping 1.26 -> 0.40 ms).
        // Interior GL_NONE holes keep their slots so fragment-output locations
        // still line up; a fragment output at a location past the trimmed count
        // is discarded, which is exactly GL's semantic for writing to a draw
        // buffer set to GL_NONE.
        while (!colorAttachmentRefs.empty() &&
               colorAttachmentRefs.back().attachment == VK_ATTACHMENT_UNUSED) {
            colorAttachmentRefs.pop_back();
        }

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

        // External subpass dependencies. Without them there is NO execution/memory
        // dependency between consecutive render passes (or a pass and a transfer)
        // touching the same attachments when the image layout does not change — the
        // layout-transition helper no-ops on identical layouts and no other barrier
        // exists. Tile-based GPUs then race tile loads against the previous pass's
        // stores (multi-pass FBO chains like MC 26.3's OIT flicker on Adreno).
        // Conservative both-ways dependencies: prior writes (attachment output,
        // depth/stencil, transfer, shader) are made visible to this pass's loads,
        // and this pass's attachment writes to subsequent sampling/transfer/loads.
        VkSubpassDependency subpassDependencies[2];
        subpassDependencies[0] = {};
        subpassDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        subpassDependencies[0].dstSubpass = 0;
        subpassDependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        subpassDependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                               VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        subpassDependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[0].dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_READ_BIT;
        subpassDependencies[0].dependencyFlags = 0;
        subpassDependencies[1] = {};
        subpassDependencies[1].srcSubpass = 0;
        subpassDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        subpassDependencies[1].srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[1].srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        subpassDependencies[1].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                              VK_PIPELINE_STAGE_TRANSFER_BIT;
        subpassDependencies[1].dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        subpassDependencies[1].dependencyFlags = 0;

        // Render Pass
        VkRenderPassCreateInfo renderPassCreateInfo;
        renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.pNext = VK_NULL_HANDLE;
        renderPassCreateInfo.flags = 0;
        renderPassCreateInfo.attachmentCount = attachmentDescriptions.size();
        renderPassCreateInfo.pAttachments = attachmentDescriptions.data();
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpassDesc;
        renderPassCreateInfo.dependencyCount = 2;
        renderPassCreateInfo.pDependencies = subpassDependencies;

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
        framebufferCreateInfo.layers = framebufferLayers;
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
            framebufferLayers };
        MGLOG_D("VkRenderPassManager::GetOrCreateRenderPass: hash=0x%llx compatibilityHash=0x%llx attachmentCount=%u colorAttachmentCount=%u samples=%d extent=%dx%d",
                static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(compatibilityHash),
                renderPassEntry.attachmentCount,
                renderPassEntry.colorAttachmentCount,
                static_cast<Int>(renderPassEntry.sampleCount),
                extent.x(),
                extent.y());
        auto [insertedIt, _] = m_renderPasses.emplace(hash, Move(renderPassEntry));
        insertedIt->second.lastUsedFrame = m_frameCounter;
        return insertedIt->second;
    }

    void VkRenderPassManager::OnPresent() {
        ++m_frameCounter;

        // Runs every frame boundary, ahead of the render-pass sweep gate below: the walk
        // is O(#renderbuffer resources) — single digits in practice — and per-frame
        // invocation keeps dead-resource reclaim latency at the aging bound instead of
        // coupling it to renderbuffer *use* (the GetOrCreateRenderbufferResource call
        // site never runs again once an app stops using renderbuffers).
        CollectRenderbufferGarbage();
        CollectDeferredRenderbufferReleases(/*destroyAll=*/false);

        // Sweep occasionally; evict entries whose last use is far past every
        // in-flight frame so their VkRenderPass/VkFramebuffer can be destroyed
        // safely (RenderPassEntry's destructor releases the handles).
        constexpr Uint64 kSweepInterval = 256;
        constexpr Uint64 kRetireAgeFrames = 1024;
        if ((m_frameCounter % kSweepInterval) != 0) {
            return;
        }

        // Collect the dying handles and notify once after the loop: pipelines hashed
        // on them share the entries' >kRetireAgeFrames idleness (they are only bound
        // by draws that hit those entries), so the observer may destroy them
        // immediately - and a single batched notification costs one pipeline-cache
        // scan instead of one per evicted pass.
        Vector<VkRenderPass> destroyedRenderPasses;
        const Uint64 activeHash = s_hasActiveRenderPass ? s_activeRenderPass.hash : 0;
        for (auto it = m_renderPasses.begin(); it != m_renderPasses.end();) {
            const Bool isActive = s_hasActiveRenderPass && it->first == activeHash;
            if (!isActive && m_frameCounter - it->second.lastUsedFrame > kRetireAgeFrames) {
                if (m_rpFastValid && m_rpFastRenderPassHash == it->first) {
                    m_rpFastValid = false;
                }
                destroyedRenderPasses.push_back(it->second.renderPass);
                it = m_renderPasses.erase(it);
            } else {
                ++it;
            }
        }
        if (!destroyedRenderPasses.empty() && m_evictionObserver != nullptr) {
            m_evictionObserver->OnRenderPassesDestroyed(destroyedRenderPasses);
        }
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
                // The inline payload was snapshotted when the entry was CREATED, but the
                // clear VALUE is not part of the entry's hash - a cache hit with a newer
                // glClear would replay the creation-time value and drop the new one (the
                // texture path below is immune because it re-reads the live payload).
                // Same defense as ClearAttachmentsOnActiveRenderPass: prefer the live
                // pending clear, fall back to the snapshot only when none is queued.
                if (s_renderPassManager != nullptr &&
                    s_renderPassManager->GetPendingRenderbufferClear(pending.renderbuffer, clearPayload)) {
                    if ((clearPayload.mask & GL_COLOR_BUFFER_BIT) != 0 && pending.renderbuffer != nullptr &&
                        MG_Util::GetBaseInternalFormatComponentCount(pending.renderbuffer->GetInternalFormat()) ==
                            3) {
                        // RGB renderbuffers are backed by an RGBA image; the missing alpha reads as 1.
                        ForceOpaqueClearAlpha(clearPayload);
                    }
                } else {
                    clearPayload = pending.inlinePayload;
                }
            } else {
                if (pending.key.texture == nullptr ||
                    !s_clearManager->GetPendingClear(pending.key, clearPayload, liveTexture)) {
                    continue;
                }
            }
            if ((clearPayload.mask & GL_COLOR_BUFFER_BIT) != 0) {
                clearValues[pending.attachmentIndex].color =
                    MakeVkClearColorValue(clearPayload, ColorFormatLacksAlpha(liveTexture.get()));
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
        // Pre-pass stream bookkeeping: this pass's attachment images are now
        // referenced by the open frame recording.
        if (s_textureManager != nullptr) {
            for (const auto& tracked : renderPassEntry.trackedAttachmentLayouts) {
                if (tracked.target == TrackedAttachmentTarget::Texture) {
                    if (const auto texture = tracked.texture.lock()) {
                        s_textureManager->StampTextureRecordingUse(texture.get());
                    }
                }
            }
        }
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
        // The fast-path memo reuses the ACTIVE render pass; once the pass ends it must not carry
        // over (the next span may be a different FBO resolved before its render pass is begun).
        if (s_renderPassManager != nullptr) {
            s_renderPassManager->m_rpFastValid = false;
        }
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
                        // The pass stored into the attachment: its content is defined
                        // until the image is next presented.
                        s_swapchainObject->SetImageContentDefined(trackedAttachment.swapchainImageIndex, true);
                        break;
                    case TrackedAttachmentTarget::SwapchainDepthStencil:
                        MOBILEGL_ASSERT(s_swapchainObject != nullptr, "EndRenderPass: swapchain object is null");
                        s_swapchainObject->SetDepthStencilImageLayout(trackedAttachment.swapchainImageIndex,
                                                                      trackedAttachment.finalLayout);
                        s_swapchainObject->SetDepthStencilContentDefined(trackedAttachment.swapchainImageIndex, true);
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
