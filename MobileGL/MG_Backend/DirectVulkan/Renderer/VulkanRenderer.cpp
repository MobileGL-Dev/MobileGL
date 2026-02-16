// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRenderer.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        VkShaderModule CreateShaderModule(VkDevice device, const Vector<Uint>& spv) {
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = spv.size() * sizeof(Uint);
            smci.pCode = spv.data();
            VkShaderModule module = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateShaderModule(device, &smci, nullptr, &module), "vkCreateShaderModule");
            return module;
        }
    } // namespace

    VulkanRenderer::VulkanRenderer(ANativeWindow* window) : m_window(window) {}

    VulkanRenderer::~VulkanRenderer() {
        if (!m_initialized) return;
        vkDeviceWaitIdle(m_ctx.GetDevice());
        DestroyPipelines();
        DestroyFrameResources();
        DestroyRenderPass();
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_ctx.GetDevice(), m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }
        if (m_swapchain) {
            m_swapchain.reset();
        }
        m_ctx.Cleanup();
        m_initialized = false;
    }

    void VulkanRenderer::Initialize() {
        if (m_initialized) return;
        m_ctx.Initialize(m_window, "MobileGL-VulkanRenderer");
        m_swapchain = MakeUnique<VkManager::SwapchainManager>(m_ctx);
        m_swapchain->Initialize();
        CreateCommandPool();
        CreateRenderPass();
        CreateFramebuffers();
        CreateFrameResources();
        m_initialized = true;
    }

    void VulkanRenderer::EnsureInitialized() {
        if (!m_initialized) {
            throw RuntimeError("VulkanRenderer not initialized");
        }
    }

    void VulkanRenderer::CreateCommandPool() {
        if (m_commandPool != VK_NULL_HANDLE) return;
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = m_ctx.GetGraphicsQueueFamily();
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_VERIFY(vkCreateCommandPool(m_ctx.GetDevice(), &cpci, nullptr, &m_commandPool), "vkCreateCommandPool");
    }

    void VulkanRenderer::CreateRenderPass() {
        DestroyRenderPass();
        VkAttachmentDescription color{};
        color.format = m_swapchain->GetFormat();
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments = &color;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;

        VK_VERIFY(vkCreateRenderPass(m_ctx.GetDevice(), &rpci, nullptr, &m_renderPass), "vkCreateRenderPass");
    }

    void VulkanRenderer::DestroyRenderPass() {
        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_ctx.GetDevice(), m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
        }
    }

    void VulkanRenderer::CreateFramebuffers() {
        const auto& views = m_swapchain->GetImageViews();
        Vector<VkFramebuffer> fbs;
        fbs.reserve(views.size());
        for (auto view : views) {
            VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbci.renderPass = m_renderPass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &view;
            fbci.width = m_swapchain->GetExtent().width;
            fbci.height = m_swapchain->GetExtent().height;
            fbci.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateFramebuffer(m_ctx.GetDevice(), &fbci, nullptr, &fb), "vkCreateFramebuffer");
            fbs.push_back(fb);
        }
        m_swapchain->SetFramebuffers(Move(fbs));
    }

    void VulkanRenderer::SetSwapchainViewportSize(const UintVec2& size) {
        if (m_swapchain) m_swapchain->SetViewportSize(size);
    }

    void VulkanRenderer::CreateFrameResources() {
        DestroyFrameResources();
        Uint32 imageCount = static_cast<Uint32>(m_swapchain->GetImageViews().size());
        if (imageCount == 0) throw RuntimeError("Swapchain has zero images");
        Uint32 frames = std::min<Uint32>(2, imageCount);
        for (Uint32 i = 0; i < frames; ++i) {
            auto frame = MakeUnique<VkManager::FrameContext>();
            frame->Initialize(m_ctx, m_commandPool);
            m_frames.push_back(Move(frame));
        }
        m_currentFrame = 0;
    }

    void VulkanRenderer::DestroyFrameResources() {
        for (auto& frame : m_frames) {
            if (frame) frame->Cleanup(m_ctx);
        }
        m_frames.clear();
    }

    void VulkanRenderer::DestroyPipelines() {
        auto device = m_ctx.GetDevice();
        for (auto& [_, info] : m_pipelines) {
            if (info.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, info.pipeline, nullptr);
            if (info.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, info.layout, nullptr);
        }
        m_pipelines.clear();
    }

    VkPipeline VulkanRenderer::CreateGraphicsPipelineFromSpv(const String& name, const Vector<Uint>& vertexSpv,
                                                             const Vector<Uint>& fragmentSpv) {
        EnsureInitialized();
        auto it = m_pipelines.find(name);
        if (it != m_pipelines.end()) return it->second.pipeline;

        VkShaderModule vs = CreateShaderModule(m_ctx.GetDevice(), vertexSpv);
        VkShaderModule fs = CreateShaderModule(m_ctx.GetDevice(), fragmentSpv);

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        ia.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo vpci{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vpci.viewportCount = 1;
        vpci.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorAttach{};
        colorAttach.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorAttach.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &colorAttach;

        VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamics;

        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VK_VERIFY(vkCreatePipelineLayout(m_ctx.GetDevice(), &plci, nullptr, &layout), "vkCreatePipelineLayout");

        VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vertexInput;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vpci;
        gpi.pRasterizationState = &raster;
        gpi.pMultisampleState = &ms;
        gpi.pColorBlendState = &blend;
        gpi.pDynamicState = &dyn;
        gpi.layout = layout;
        gpi.renderPass = m_renderPass;
        gpi.subpass = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateGraphicsPipelines(m_ctx.GetDevice(), VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline),
                  "vkCreateGraphicsPipelines");

        vkDestroyShaderModule(m_ctx.GetDevice(), vs, nullptr);
        vkDestroyShaderModule(m_ctx.GetDevice(), fs, nullptr);

        m_pipelines[name] = {pipeline, layout};
        return pipeline;
    }

    void VulkanRenderer::RegisterRenderCallback(const String& name, RenderCallback callback) {
        m_callbacks[name] = Move(callback);
    }

    void VulkanRenderer::RecordAndSubmit(Uint32 imageIndex) {
        auto& frame = *m_frames[m_currentFrame];
        VK_VERIFY(vkWaitForFences(m_ctx.GetDevice(), 1, &frame.InFlightFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        VK_VERIFY(vkResetFences(m_ctx.GetDevice(), 1, &frame.InFlightFence), "vkResetFences");
        VK_VERIFY(vkResetCommandBuffer(frame.CommandBuffer, 0), "vkResetCommandBuffer");

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_VERIFY(vkBeginCommandBuffer(frame.CommandBuffer, &bi), "vkBeginCommandBuffer");

        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = m_renderPass;
        rpbi.framebuffer = m_swapchain->GetFramebuffers()[imageIndex];
        rpbi.renderArea.offset = {0, 0};
        rpbi.renderArea.extent = m_swapchain->GetExtent();
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;

        vkCmdBeginRenderPass(frame.CommandBuffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(m_swapchain->GetExtent().width);
        vp.height = static_cast<float>(m_swapchain->GetExtent().height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(frame.CommandBuffer, 0, 1, &vp);

        VkRect2D scissor{{0, 0}, m_swapchain->GetExtent()};
        vkCmdSetScissor(frame.CommandBuffer, 0, 1, &scissor);

        for (auto& [_, cb] : m_callbacks) {
            cb(frame.CommandBuffer, imageIndex, m_swapchain->GetExtent());
        }

        vkCmdEndRenderPass(frame.CommandBuffer);
        VK_VERIFY(vkEndCommandBuffer(frame.CommandBuffer), "vkEndCommandBuffer");

        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphore waitSemaphores[] = {frame.ImageAvailable};
        VkSemaphore signalSemaphores[] = {frame.RenderFinished};

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = waitSemaphores;
        si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &frame.CommandBuffer;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = signalSemaphores;

        VK_VERIFY(vkQueueSubmit(m_ctx.GetGraphicsQueue(), 1, &si, frame.InFlightFence), "vkQueueSubmit");
    }

    void VulkanRenderer::DrawAndPresent() {
        EnsureInitialized();
        auto& frame = *m_frames[m_currentFrame];
        auto& imagesInFlight = m_swapchain->GetImagesInFlight();

        Uint32 imageIndex = 0;
        VkResult res = vkAcquireNextImageKHR(m_ctx.GetDevice(), m_swapchain->GetSwapchain(), UINT64_MAX,
                                             frame.ImageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            return;
        }
        VK_VERIFY(res, "vkAcquireNextImageKHR");

        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(m_ctx.GetDevice(), 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[imageIndex] = frame.InFlightFence;

        RecordAndSubmit(imageIndex);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        VkSemaphore waitSemaphores[] = {frame.RenderFinished};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = waitSemaphores;
        VkSwapchainKHR swapchains[] = {m_swapchain->GetSwapchain()};
        pi.swapchainCount = 1;
        pi.pSwapchains = swapchains;
        pi.pImageIndices = &imageIndex;

        VkResult pres = vkQueuePresentKHR(m_ctx.GetGraphicsQueue(), &pi);
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
            RecreateSwapchain();
        } else {
            VK_VERIFY(pres, "vkQueuePresentKHR");
        }

        m_currentFrame = (m_currentFrame + 1) % m_frames.size();
    }

    void VulkanRenderer::RecreateSwapchain() {
        vkDeviceWaitIdle(m_ctx.GetDevice());
        DestroyFrameResources();
        DestroyRenderPass();

        m_swapchain->Recreate();
        CreateRenderPass();
        CreateFramebuffers();
        CreateFrameResources();

        DestroyPipelines();
    }

    void VulkanRenderer::RenderFrame() {
        DrawAndPresent();
    }

    void VulkanRenderer::Present() {
        DrawAndPresent();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
