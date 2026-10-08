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

    VkRenderPassManager::VkRenderPassManager(VkDevice device,
        VkPhysicalDevice physicalDevice, VmaAllocator allocator, const VulkanRendererConfig& config,
        VkTextureManager& textureManager, SwapchainObject& swapchainObject):
        m_device(device), m_physicalDevice(physicalDevice), m_allocator(allocator), m_config(config),
        m_textureManager(textureManager), m_swapchainObject(swapchainObject) {
        RenderPassEntry::s_device.Get() = m_device;
        s_textureManager.Get() = &m_textureManager;
        s_swapchainObject.Get() = &m_swapchainObject;
        s_renderPassManager.Get() = this;
    }

    VkRenderPassManager::~VkRenderPassManager() {}

    Bool VkRenderPassManager::Initialize() {
        return true;
    }

    void VkRenderPassManager::Shutdown() {
        s_activeRenderPass.Get() = {};
        s_hasActiveRenderPass.Get() = false;
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
        s_activeRenderPass.Get().commandBuffer = commandBuffer;
        s_activeRenderPass.Get().hash = renderPassEntry.hash;
        s_activeRenderPass.Get().compatibilityHash = renderPassEntry.compatibilityHash;
        s_activeRenderPass.Get().extent = renderPassEntry.extent;
        s_hasActiveRenderPass.Get() = true;

        return true;
    }

    Bool VkRenderPassManager::BeginRenderPass(VkCommandBuffer commandBuffer, RenderPassEntry& renderPassEntry,
                                              VkRenderPass renderPass, const VkClearValue* clearValues,
                                              Uint32 clearValueCount) {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = renderPass;
        begin.framebuffer = renderPassEntry.framebuffer;
        begin.renderArea = {{0, 0}, {static_cast<Uint32>(renderPassEntry.extent.x()),
                                     static_cast<Uint32>(renderPassEntry.extent.y())}};
        begin.clearValueCount = clearValueCount;
        begin.pClearValues = clearValues;
        vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        s_activeRenderPass.Get().commandBuffer = commandBuffer;
        s_activeRenderPass.Get().hash = renderPassEntry.hash;
        s_activeRenderPass.Get().compatibilityHash = renderPassEntry.compatibilityHash;
        s_activeRenderPass.Get().extent = renderPassEntry.extent;
        s_hasActiveRenderPass.Get() = true;
        return true;
    }

    Bool VkRenderPassManager::EndRenderPass(VkCommandBuffer commandBuffer) {
        BumpWireImageWriteEpoch();
        vkCmdEndRenderPass(commandBuffer);
        s_activeRenderPass.Get() = {};
        s_hasActiveRenderPass.Get() = false;
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
