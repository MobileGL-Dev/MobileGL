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
#include <unordered_map>
#include <vk_mem_alloc.h>

namespace MobileGL::MG_Backend::DirectVulkan {
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
        // Per Magma session (MagmaSession.h): each session's renderer is its own device.
        static inline SessionLocal<VkDevice> s_device;
        Uint64 hash = 0;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        Uint64 compatibilityHash = 0;
        Uint32 attachmentCount = 0;
        Uint32 colorAttachmentCount = 0;
        Bool hasDepthStencilAttachment = false;
        VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
        IntVec2 extent = {0, 0};
        // VkFramebufferCreateInfo::layers of the entry's framebuffer (>1 for layered GL attachments).
        Uint32 layers = 1;
        // Frame counter value of the last GetOrCreateRenderPass hit; drives cache eviction.
        Uint64 lastUsedFrame = 0;
        // P15 S0 (wire passes only): what the render pass was created from, and the render
        // passes that differ from it only in loading some attachments with LOAD_OP_CLEAR. Load ops
        // are not part of render-pass compatibility, so a variant begins with this entry's own
        // framebuffer and runs every pipeline created against `renderPass`.
        Vector<VkAttachmentDescription> wireAttachments;
        Vector<VkAttachmentReference> wireColors;
        VkAttachmentReference wireDepth{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
        Vector<std::pair<Uint64, VkRenderPass>> clearVariants;

        RenderPassEntry() = default;
        RenderPassEntry(const RenderPassEntry&) = delete;
        RenderPassEntry(RenderPassEntry&& that) noexcept {
            std::swap(hash, that.hash);
            std::swap(renderPass, that.renderPass);
            std::swap(framebuffer, that.framebuffer);
            std::swap(compatibilityHash, that.compatibilityHash);
            std::swap(attachmentCount, that.attachmentCount);
            std::swap(colorAttachmentCount, that.colorAttachmentCount);
            std::swap(hasDepthStencilAttachment, that.hasDepthStencilAttachment);
            std::swap(sampleCount, that.sampleCount);
            std::swap(extent, that.extent);
            std::swap(layers, that.layers);
            std::swap(lastUsedFrame, that.lastUsedFrame);
            std::swap(wireAttachments, that.wireAttachments);
            std::swap(wireColors, that.wireColors);
            std::swap(wireDepth, that.wireDepth);
            std::swap(clearVariants, that.clearVariants);
        }
        // Move ASSIGNMENT, not just construction. The move constructor above and the
        // destructor below each independently suppress the implicit one, which left the
        // type move-constructible but not move-assignable - and therefore not swappable,
        // which std::swap(pair&, pair&) requires. That was invisible while UnorderedMap
        // only ever move-CONSTRUCTED an element into a fresh slot. ska::flat_hash_map
        // probes robin-hood: inserting swaps the entry being placed against the one
        // already sitting in the slot whenever it has travelled further from its desired
        // position, so the mapped type has to be swappable or the table fails to
        // instantiate at all.
        //
        // SWAP SEMANTICS, exactly like the move constructor: this does not release the
        // destination's handles, it parks them in `that`, which destroys them when it
        // dies. That is correct for the only caller - std::swap, whose temporary expires
        // immediately - and it is what keeps the three-move sequence from destroying a
        // live render pass. It is NOT correct for a hand-written `a = std::move(b)` where
        // `a` held live handles and `b` outlives the statement: those handles would then
        // survive until `b` dies. There is no such caller; add a destroy-then-steal
        // assignment before writing one.
        RenderPassEntry& operator=(RenderPassEntry&& that) noexcept {
            if (this != &that) {
                std::swap(hash, that.hash);
                std::swap(renderPass, that.renderPass);
                std::swap(framebuffer, that.framebuffer);
                std::swap(compatibilityHash, that.compatibilityHash);
                std::swap(attachmentCount, that.attachmentCount);
                std::swap(colorAttachmentCount, that.colorAttachmentCount);
                std::swap(hasDepthStencilAttachment, that.hasDepthStencilAttachment);
                std::swap(sampleCount, that.sampleCount);
                std::swap(extent, that.extent);
                std::swap(layers, that.layers);
                std::swap(lastUsedFrame, that.lastUsedFrame);
                std::swap(wireAttachments, that.wireAttachments);
                std::swap(wireColors, that.wireColors);
                std::swap(wireDepth, that.wireDepth);
                std::swap(clearVariants, that.clearVariants);
            }
            return *this;
        }
        ~RenderPassEntry() {
            for (const auto& variant : clearVariants)
                if (variant.second != VK_NULL_HANDLE) vkDestroyRenderPass(s_device.Get(), variant.second, nullptr);
            if (renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(s_device.Get(), renderPass, nullptr);
            }
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(s_device.Get(), framebuffer, nullptr);
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
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
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
            VkTextureManager& textureManager, SwapchainObject& swapchainObject);
        ~VkRenderPassManager();

        Bool Initialize();
        void Shutdown();
        // The surface target the default framebuffer currently names (VulkanRenderer's surface
        // targets): a default-framebuffer pass of one surface is not another's.
        void SetDefaultFramebufferTarget(Uint64 serial) {
            m_defaultFramebufferTarget = serial;
        }
        static Bool BeginRenderPass(VkCommandBuffer commandBuffer, RenderPassEntry& renderPassEntry);
        // P15 S0: begins `renderPassEntry` through `renderPass` (one of its clear-load variants)
        // with these clear values; it is active as the entry itself, so a draw continues it.
        static Bool BeginRenderPass(VkCommandBuffer commandBuffer, RenderPassEntry& renderPassEntry,
                                    VkRenderPass renderPass, const VkClearValue* clearValues, Uint32 clearValueCount);
        static Bool EndRenderPass(VkCommandBuffer commandBuffer);
        static ActiveRenderPassInfo* GetActiveRenderPass();
    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VmaAllocator m_allocator = nullptr;
        const VulkanRendererConfig& m_config;
        VkTextureManager& m_textureManager;
        SwapchainObject& m_swapchainObject;

        // Bumped whenever a renderbuffer VkImage is (re)created; together with the texture
        // manager's image epoch this invalidates the render-pass fast path on any attachment
        // image recreation.
        Uint64 m_renderbufferImageEpoch = 1;

    public:
        // Bumped whenever a renderbuffer backing is (re)created; consecutive-draw
        // snapshots include it so an attachment respecify forces a re-resolve.
        Uint64 GetRenderbufferImageEpoch() const { return m_renderbufferImageEpoch; }

    private:

        Uint64 m_defaultFramebufferTarget = 0;

        // Supported sample counts per attachment format, so per-draw resource lookups
        // do not repeat vkGetPhysicalDeviceImageFormatProperties.
        UnorderedMap<VkFormat, VkSampleCountFlags> m_attachmentSampleCountsByFormat;


        static inline thread_local XXH64_state_t* m_hashState = XXH64_createState();
        // Per Magma session (MagmaSession.h): one session's open render pass and managers are not
        // another's.
        static inline SessionLocal<ActiveRenderPassInfo> s_activeRenderPass;
        static inline SessionLocal<Bool> s_hasActiveRenderPass;
        static inline SessionLocal<VkTextureManager*> s_textureManager;
        static inline SessionLocal<SwapchainObject*> s_swapchainObject;
        static inline SessionLocal<VkRenderPassManager*> s_renderPassManager;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
