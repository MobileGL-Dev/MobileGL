// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "VulkanContext.h"
#include "SwapchainManager.h"
#include "FrameContext.h"
#include "VkCommon.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    class VulkanRenderer {
    public:
        using RenderCallback = std::function<void(VkCommandBuffer, Uint32, VkExtent2D)>;

        explicit VulkanRenderer(ANativeWindow* window);
        ~VulkanRenderer();

        VulkanRenderer(const VulkanRenderer&) = delete;
        VulkanRenderer& operator=(const VulkanRenderer&) = delete;

        void Initialize();
        void RenderFrame();
        void Present();
        void SetSwapchainViewportSize(const UintVec2& size);

        VkPipeline CreateGraphicsPipelineFromSpv(const String& name, const Vector<Uint>& vertexSpv,
                                                 const Vector<Uint>& fragmentSpv);
        void RegisterRenderCallback(const String& name, RenderCallback callback);

    private:
        void EnsureInitialized();
        void CreateCommandPool();
        void CreateRenderPass();
        void DestroyRenderPass();
        void CreateFramebuffers();
        void CreateFrameResources();
        void DestroyFrameResources();
        void RecreateSwapchain();
        void RecordAndSubmit(Uint32 imageIndex);
        void DrawAndPresent();
        void DestroyPipelines();

        struct PipelineInfo {
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
        };

        ANativeWindow* m_window = nullptr;
        VkManager::VulkanContext m_ctx;
        UniquePtr<VkManager::SwapchainManager> m_swapchain;
        VkRenderPass m_renderPass = VK_NULL_HANDLE;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        Vector<UniquePtr<VkManager::FrameContext>> m_frames;
        Uint32 m_currentFrame = 0;
        Bool m_initialized = false;

        UnorderedMap<String, RenderCallback> m_callbacks;
        UnorderedMap<String, PipelineInfo> m_pipelines;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
