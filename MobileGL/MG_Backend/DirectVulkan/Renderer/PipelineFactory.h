// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/PipelineFactory.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "Config.h"
#include "../VkIncludes.h"
#include "MG_State/GLState/FramebufferState/FramebufferObject.h"
#include <Includes.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    class PipelineFactory {
    public:
        using HashType = Uint64;

        struct PipelineCreatePayload {
            static constexpr Uint32 kMaxColorAttachments = MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS;

            HashType programHash = 0;
            HashType vertexInputHash = 0;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            Uint32 colorAttachmentCount = 1;
            VkSampleCountFlagBits rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            Uint32 subpass = 0;
            VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
            VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
            VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE;
            Bool depthTestEnable = false;
            Bool depthWriteEnable = false;
            Bool depthBiasEnable = false;
            Bool rasterizerDiscardEnable = false;
            Bool logicOpEnable = false;
            Bool stencilTestEnable = false;
            VkCompareOp depthCompareOp = VK_COMPARE_OP_ALWAYS;
            VkLogicOp logicOp = VK_LOGIC_OP_COPY;
            VkStencilOp frontStencilFailOp = VK_STENCIL_OP_KEEP;
            VkStencilOp frontStencilPassOp = VK_STENCIL_OP_KEEP;
            VkStencilOp frontStencilDepthFailOp = VK_STENCIL_OP_KEEP;
            VkCompareOp frontStencilCompareOp = VK_COMPARE_OP_ALWAYS;
            VkStencilOp backStencilFailOp = VK_STENCIL_OP_KEEP;
            VkStencilOp backStencilPassOp = VK_STENCIL_OP_KEEP;
            VkStencilOp backStencilDepthFailOp = VK_STENCIL_OP_KEEP;
            VkCompareOp backStencilCompareOp = VK_COMPARE_OP_ALWAYS;
            Array<VkPipelineColorBlendAttachmentState, kMaxColorAttachments> colorBlendAttachments{};
            const Vector<VkPipelineShaderStageCreateInfo>* stages = nullptr;
            const VkPipelineVertexInputStateCreateInfo* vertexInputState = nullptr;
        };

        explicit PipelineFactory(VkDevice device, const VulkanRendererConfig& config);
        ~PipelineFactory();
        PipelineFactory(const PipelineFactory&) = delete;

        HashType ComputeHash(const PipelineCreatePayload& payload) const;
        VkPipeline GetOrCreatePipeline(const PipelineCreatePayload& payload);
        void DestroyAll();

    private:
        VkPipeline CreatePipeline(const PipelineCreatePayload& payload) const;

        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanRendererConfig& m_config;
        VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
        UnorderedMap<HashType, VkPipeline> m_cache;
        static inline XXH64_state_t* m_hashState = XXH64_createState();
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
