// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "SwapchainObject.h"
#include "VkClearManager.h"
#include "VkTextureManager.h"
#include "../VkIncludes.h"
#include "../VulkanRendererConfig.h"
#include "MG_State/GLState/FramebufferState/FramebufferObject.h"

#include <Includes.h>
#include <vk_mem_alloc.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    enum class TrackedAttachmentTarget : Uint8 {
        Texture,
        Renderbuffer,
        SwapchainColor,
        SwapchainDepthStencil
    };

    struct PendingClearAttachmentInfo {
        Uint32 attachmentIndex = 0;
        PendingClearKey key{};
        MG_State::GLState::RenderbufferObject* renderbuffer = nullptr;
        Bool hasInlinePayload = false;
        ClearAttachmentPayload inlinePayload{};
    };

    struct TrackedAttachmentLayoutInfo {
        TrackedAttachmentTarget target = TrackedAttachmentTarget::Texture;
        WeakPtr<MG_State::GLState::ITextureObject> texture;
        WeakPtr<MG_State::GLState::RenderbufferObject> renderbuffer;
        Uint32 textureMipLevel = 0;
        Uint32 swapchainImageIndex = 0;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct DepthStencilAttachmentLoadInfo {
        VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkAttachmentLoadOp stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    DepthStencilAttachmentLoadInfo ResolveDepthStencilAttachmentLoadInfo(
        VkImageLayout trackedLayout, Bool clearDepth, Bool clearStencil);
    IntVec2 ResolveRenderPassFramebufferExtent(Bool isDefaultFbo, const TextureSize& attachmentExtent,
                                               VkExtent2D swapchainExtent);

    struct RenderPassEntry {
        static inline VkDevice s_device;
        static inline Vector<VkTextureManager::TextureResource*> s_textureResourcesScratch;
        Uint64 hash = 0;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        Uint64 compatibilityHash = 0;
        Vector<PendingClearAttachmentInfo> pendingClearAttachments;
        Vector<TrackedAttachmentLayoutInfo> trackedAttachmentLayouts;
        Uint32 attachmentCount = 0;
        Uint32 colorAttachmentCount = 0;
        Bool hasDepthStencilAttachment = false;
        VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
        IntVec2 extent = {0, 0};
        Uint32 subpass = 0;

        RenderPassEntry() = default;
        RenderPassEntry(const RenderPassEntry&) = delete;
        RenderPassEntry(RenderPassEntry&& that) noexcept {
            std::swap(hash, that.hash);
            std::swap(renderPass, that.renderPass);
            std::swap(framebuffer, that.framebuffer);
            std::swap(compatibilityHash, that.compatibilityHash);
            std::swap(pendingClearAttachments, that.pendingClearAttachments);
            std::swap(trackedAttachmentLayouts, that.trackedAttachmentLayouts);
            std::swap(attachmentCount, that.attachmentCount);
            std::swap(colorAttachmentCount, that.colorAttachmentCount);
            std::swap(hasDepthStencilAttachment, that.hasDepthStencilAttachment);
            std::swap(sampleCount, that.sampleCount);
            std::swap(extent, that.extent);
            std::swap(subpass, that.subpass);
        }
        RenderPassEntry(
            Uint64 hash,
            VkRenderPass renderpass,
            VkFramebuffer framebuffer,
            Uint64 compatibilityHash,
            const Vector<PendingClearAttachmentInfo>& pendingClearAttachments,
            const Vector<TrackedAttachmentLayoutInfo>& trackedAttachmentLayouts,
            Uint32 attachmentCount,
            Uint32 colorAttachmentCount,
            Bool hasDepthStencilAttachment,
            VkSampleCountFlagBits sampleCount,
            IntVec2 extent, int subpass):
            hash(hash),
            renderPass(renderpass),
            framebuffer(framebuffer),
            compatibilityHash(compatibilityHash),
            pendingClearAttachments(Move(pendingClearAttachments)),
            trackedAttachmentLayouts(Move(trackedAttachmentLayouts)),
            attachmentCount(attachmentCount),
            colorAttachmentCount(colorAttachmentCount),
            hasDepthStencilAttachment(hasDepthStencilAttachment),
            sampleCount(sampleCount),
            extent(extent),
            subpass(subpass)
        {}

        ~RenderPassEntry() {
            if (renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(s_device, renderPass, nullptr);
            }
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(s_device, framebuffer, nullptr);
            }
        }

        Bool CompatibleWith(const RenderPassEntry& that) const {
            return this->compatibilityHash == that.compatibilityHash;
        }

        Bool CompatibleWith(Uint64 compatibilityHash) const {
            return this->compatibilityHash == compatibilityHash;
        }
    };

    struct ActiveRenderPassInfo {
        Uint64 hash = 0;
        Uint64 compatibilityHash = 0;
        Vector<TrackedAttachmentLayoutInfo> trackedAttachmentLayouts;
        IntVec2 extent = {0, 0};

        Bool CompatibleWith(const RenderPassEntry& that) const {
            return compatibilityHash == that.compatibilityHash;
        }

        Bool CompatibleWith(Uint64 thatCompatibilityHash) const {
            return compatibilityHash == thatCompatibilityHash;
        }
    };

    class VkRenderPassManager {
    public:
        using HashType = Uint64;
        VkRenderPassManager(VkDevice device,
            VkPhysicalDevice physicalDevice, VmaAllocator allocator, const VulkanRendererConfig& config,
            VkClearManager& clearManager, VkTextureManager& textureManager, SwapchainObject& swapchainObject);
        ~VkRenderPassManager();

        Bool Initialize();
        void Shutdown();

        HashType ComputeHash(
            const MG_State::GLState::FramebufferObject& fbo,
            Uint32 swapchainImageIndex,
            Bool includePendingClear = true);
        RenderPassEntry& GetOrCreateRenderPass(const MG_State::GLState::FramebufferObject& fbo, Uint32 swapchainImageIndex);
        void QueueRenderbufferClear(GLbitfield mask, const ClearFramebufferPayload& clearPayload,
                                    const MG_State::GLState::FramebufferObject& drawFbo);
        void QueueRenderbufferClear(const ClearAttachmentPayload& clearPayload,
                                    const MG_State::GLState::FramebufferAttachmentObject& attachment);
        void PopPendingRenderbufferClear(MG_State::GLState::RenderbufferObject* renderbuffer);
        static Bool BeginRenderPass(VkCommandBuffer commandBuffer, RenderPassEntry& renderPassEntry);
        static Bool EndRenderPass(VkCommandBuffer commandBuffer);
        static ActiveRenderPassInfo* GetActiveRenderPass();
    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VmaAllocator m_allocator = nullptr;
        const VulkanRendererConfig& m_config;
        VkClearManager& m_clearManager;
        VkTextureManager& m_textureManager;
        SwapchainObject& m_swapchainObject;
        UnorderedMap<Uint64, RenderPassEntry> m_renderPasses;

        struct RenderbufferResource {
            WeakPtr<MG_State::GLState::RenderbufferObject> renderbuffer;
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_NONE;
            VkExtent2D extent = {0, 0};
            VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
            TextureInternalFormat internalFormat = TextureInternalFormat::Unknown;
            Int samples = 0;

            void Destroy(VkDevice device, VmaAllocator allocator);
        };

        struct PendingRenderbufferClear {
            WeakPtr<MG_State::GLState::RenderbufferObject> renderbuffer;
            ClearAttachmentPayload payload{};
        };

        UnorderedMap<MG_State::GLState::RenderbufferObject*, RenderbufferResource> m_renderbufferResources;
        UnorderedMap<MG_State::GLState::RenderbufferObject*, PendingRenderbufferClear> m_pendingRenderbufferClears;

        RenderbufferResource* GetOrCreateRenderbufferResource(
            const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbuffer);
        Bool GetPendingRenderbufferClear(MG_State::GLState::RenderbufferObject* renderbuffer,
                                         ClearAttachmentPayload& outPayload) const;
        Bool HasPendingRenderbufferClear(
            const MG_State::GLState::FramebufferAttachmentObject& attachment) const;
        void CollectRenderbufferGarbage();

        static inline XXH64_state_t* m_hashState = XXH64_createState();
        static inline ActiveRenderPassInfo s_activeRenderPass{};
        static inline Bool s_hasActiveRenderPass = false;
        static inline VkClearManager* s_clearManager = nullptr;
        static inline VkTextureManager* s_textureManager = nullptr;
        static inline SwapchainObject* s_swapchainObject = nullptr;
        static inline VkRenderPassManager* s_renderPassManager = nullptr;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
