// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkRenderPassManager.h"
#include "RenderPassGuard.h"
#include <atomic>

#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_State/GLState/TextureState/TextureObject2D.h"
#include "MG_Util/Converters/MGToStr/FramebufferEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Metrics/TextureMetrics.h"
#include <MG_Backend/MGPipe/PipeInputs.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    [[maybe_unused]] static Float ResolveColorClearAlpha(const MG_State::GLState::ITextureObject* texture, Float requestedAlpha) {
        if (texture != nullptr && MG_Util::GetBaseInternalFormatComponentCount(texture->GetFormat()) == 3) {
            return 1.0f;
        }
        return requestedAlpha;
    }

    // ResolveAttachmentLayerCount lives in VkTextureManager.h, beside ToVulkanLevelExtent, because
    // VkClearManager needs the SAME answer: its pending-clear key's layerCount becomes a real
    // VkImageSubresourceRange when a clear is materialised outside a render pass. See the header.

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
        RenderPassEntry::s_device.Get() = m_device;
        s_clearManager.Get() = &m_clearManager;
        s_textureManager.Get() = &m_textureManager;
        s_swapchainObject.Get() = &m_swapchainObject;
        s_renderPassManager.Get() = this;
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
        s_activeRenderPass.Get() = {};
        s_hasActiveRenderPass.Get() = false;
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

    void VkRenderPassManager::PurgeRenderPasses() {
        Vector<VkRenderPass> destroyedRenderPasses;
        destroyedRenderPasses.reserve(m_renderPasses.size());
        for (auto& [_, entry] : m_renderPasses) destroyedRenderPasses.push_back(entry.renderPass);
        m_renderPasses.clear();
        if (!destroyedRenderPasses.empty() && m_evictionObserver != nullptr) {
            m_evictionObserver->OnRenderPassesDestroyed(destroyedRenderPasses);
        }
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
        const Uint64 activeHash = s_hasActiveRenderPass.Get() ? s_activeRenderPass.Get().hash : 0;
        for (auto it = m_renderPasses.begin(); it != m_renderPasses.end();) {
            const Bool isActive = s_hasActiveRenderPass.Get() && it->first == activeHash;
            if (!isActive && m_frameCounter - it->second.lastUsedFrame > kRetireAgeFrames) {
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
        renderPassBeginInfo.clearValueCount = static_cast<Uint32>(clearValues.size());
        renderPassBeginInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        // Pre-pass stream bookkeeping: this pass's attachment images are now
        // referenced by the open frame recording.
        if (s_textureManager.Get() != nullptr) {
            for (const auto& tracked : renderPassEntry.trackedAttachmentLayouts) {
                if (tracked.target == TrackedAttachmentTarget::Texture) {
                    if (const auto texture = tracked.texture.lock()) {
                        s_textureManager.Get()->StampTextureRecordingUse(texture.get());
                    }
                }
            }
        }
        s_activeRenderPass.Get().commandBuffer = commandBuffer;
        s_activeRenderPass.Get().hash = renderPassEntry.hash;
        s_activeRenderPass.Get().compatibilityHash = renderPassEntry.compatibilityHash;
        s_activeRenderPass.Get().trackedAttachmentLayouts = renderPassEntry.trackedAttachmentLayouts;
        s_activeRenderPass.Get().extent = renderPassEntry.extent;
        s_hasActiveRenderPass.Get() = true;

        return true;
    }

    Bool VkRenderPassManager::EndRenderPass(VkCommandBuffer commandBuffer) {
        BumpWireImageWriteEpoch();
        auto* activeRenderPass = GetActiveRenderPass();
        Vector<TrackedAttachmentLayoutInfo> trackedAttachmentLayouts;
        if (activeRenderPass != nullptr) {
            trackedAttachmentLayouts = Move(activeRenderPass->trackedAttachmentLayouts);
        }
        vkCmdEndRenderPass(commandBuffer);
        // Layout reconciliation below may record barriers, whose guard closes an active
        // pass. Detach the ended pass first so that guard cannot recursively end it again.
        s_activeRenderPass.Get() = {};
        s_hasActiveRenderPass.Get() = false;
        if (!trackedAttachmentLayouts.empty()) {
            for (const auto& trackedAttachment : trackedAttachmentLayouts) {
                switch (trackedAttachment.target) {
                    case TrackedAttachmentTarget::Texture:
                        MOBILEGL_ASSERT(s_textureManager.Get() != nullptr, "EndRenderPass: texture manager is null");
                        if (const auto texture = trackedAttachment.texture.lock()) {
                            s_textureManager.Get()->UpdateTrackedImageLayoutAfterAttachmentWrite(
                                commandBuffer,
                                texture.get(),
                                trackedAttachment.textureMipLevel,
                                trackedAttachment.finalLayout);
                        }
                        break;
                    case TrackedAttachmentTarget::Renderbuffer:
                        MOBILEGL_ASSERT(s_renderPassManager.Get() != nullptr, "EndRenderPass: render pass manager is null");
                        if (const auto renderbuffer = trackedAttachment.renderbuffer.lock()) {
                            auto resourceIt =
                                s_renderPassManager.Get()->m_renderbufferResources.find(renderbuffer.get());
                            if (resourceIt != s_renderPassManager.Get()->m_renderbufferResources.end()) {
                                resourceIt->second.layout = trackedAttachment.finalLayout;
                            }
                        }
                        break;
                    case TrackedAttachmentTarget::SwapchainColor:
                        MOBILEGL_ASSERT(s_swapchainObject.Get() != nullptr, "EndRenderPass: swapchain object is null");
                        s_swapchainObject.Get()->SetImageLayout(trackedAttachment.swapchainImageIndex, trackedAttachment.finalLayout);
                        // The pass stored into the attachment: its content is defined
                        // until the image is next presented.
                        s_swapchainObject.Get()->SetImageContentDefined(trackedAttachment.swapchainImageIndex, true);
                        break;
                    case TrackedAttachmentTarget::SwapchainDepthStencil:
                        MOBILEGL_ASSERT(s_swapchainObject.Get() != nullptr, "EndRenderPass: swapchain object is null");
                        s_swapchainObject.Get()->SetDepthStencilImageLayout(trackedAttachment.swapchainImageIndex,
                                                                      trackedAttachment.finalLayout);
                        s_swapchainObject.Get()->SetDepthStencilContentDefined(trackedAttachment.swapchainImageIndex, true);
                        break;
                    default:
                        MOBILEGL_ASSERT(false, "EndRenderPass: unsupported tracked attachment target=%d",
                                        static_cast<Int>(trackedAttachment.target));
                        break;
                }
            }
        }
        return true;
    }

    ActiveRenderPassInfo* VkRenderPassManager::GetActiveRenderPass() {
        return s_hasActiveRenderPass.Get() ? &s_activeRenderPass.Get() : nullptr;
    }
    namespace {
        std::atomic<Uint64> g_wireImageWriteEpoch{1};
    }
    Uint64 WireImageWriteEpoch() { return g_wireImageWriteEpoch.load(std::memory_order_relaxed); }
    void BumpWireImageWriteEpoch() { g_wireImageWriteEpoch.fetch_add(1, std::memory_order_relaxed); }

    void EndActiveRenderPassOn(VkCommandBuffer commandBuffer) {
        BumpWireImageWriteEpoch();
        const auto* active = VkRenderPassManager::GetActiveRenderPass();
        if (active != nullptr && active->commandBuffer == commandBuffer && commandBuffer != VK_NULL_HANDLE)
            VkRenderPassManager::EndRenderPass(commandBuffer);
    }

} // namespace MobileGL::MG_Backend::DirectVulkan
