// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/PipelineFactory.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PipelineFactory.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    PipelineFactory::PipelineFactory(VkDevice device, const VulkanRendererConfig& config):
        m_device(device), m_config(config) {
        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE, "PipelineFactory: device is null");

        if (m_config.DisablePipelineCache) {
            MGLOG_I("DirectVulkan: pipeline cache disabled");
            return;
        }

        VkPipelineCacheCreateInfo pipelineCacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VK_VERIFY(vkCreatePipelineCache(m_device, &pipelineCacheInfo, nullptr, &m_pipelineCache),
                  "vkCreatePipelineCache");
    }

    PipelineFactory::~PipelineFactory() {
        DestroyAll();
        if (m_pipelineCache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
            m_pipelineCache = VK_NULL_HANDLE;
        }
    }

    PipelineFactory::HashType PipelineFactory::ComputeHash(const PipelineCreatePayload& payload) const {
        XXHASH_VERIFY(XXH64_reset(m_hashState, m_config.CacheVersion));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.programHash, sizeof(payload.programHash)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.vertexInputHash, sizeof(payload.vertexInputHash)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.pipelineLayout, sizeof(payload.pipelineLayout)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.renderPass, sizeof(payload.renderPass)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.colorAttachmentCount, sizeof(payload.colorAttachmentCount)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.rasterizationSamples, sizeof(payload.rasterizationSamples)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.subpass, sizeof(payload.subpass)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.topology, sizeof(payload.topology)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.cullMode, sizeof(payload.cullMode)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.frontFace, sizeof(payload.frontFace)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.depthTestEnable, sizeof(payload.depthTestEnable)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.depthWriteEnable, sizeof(payload.depthWriteEnable)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.depthBiasEnable, sizeof(payload.depthBiasEnable)));
        XXHASH_VERIFY(
            XXH64_update(m_hashState, &payload.rasterizerDiscardEnable, sizeof(payload.rasterizerDiscardEnable)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.logicOpEnable, sizeof(payload.logicOpEnable)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.stencilTestEnable, sizeof(payload.stencilTestEnable)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.depthCompareOp, sizeof(payload.depthCompareOp)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.logicOp, sizeof(payload.logicOp)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.frontStencilFailOp, sizeof(payload.frontStencilFailOp)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.frontStencilPassOp, sizeof(payload.frontStencilPassOp)));
        XXHASH_VERIFY(
            XXH64_update(m_hashState, &payload.frontStencilDepthFailOp, sizeof(payload.frontStencilDepthFailOp)));
        XXHASH_VERIFY(
            XXH64_update(m_hashState, &payload.frontStencilCompareOp, sizeof(payload.frontStencilCompareOp)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.backStencilFailOp, sizeof(payload.backStencilFailOp)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.backStencilPassOp, sizeof(payload.backStencilPassOp)));
        XXHASH_VERIFY(
            XXH64_update(m_hashState, &payload.backStencilDepthFailOp, sizeof(payload.backStencilDepthFailOp)));
        XXHASH_VERIFY(
            XXH64_update(m_hashState, &payload.backStencilCompareOp, sizeof(payload.backStencilCompareOp)));
        if (payload.colorAttachmentCount > 0) {
            XXHASH_VERIFY(XXH64_update(
                m_hashState,
                payload.colorBlendAttachments.data(),
                sizeof(payload.colorBlendAttachments[0]) * payload.colorAttachmentCount));
        }
        return XXH64_digest(m_hashState);
    }

    VkPipeline PipelineFactory::GetOrCreatePipeline(const PipelineCreatePayload& payload) {
        const HashType hash = ComputeHash(payload);
        auto it = m_cache.find(hash);
        if (it != m_cache.end()) {
            return it->second;
        }

        VkPipeline pipeline = CreatePipeline(payload);
        m_cache.emplace(hash, pipeline);
        return pipeline;
    }

    void PipelineFactory::DestroyAll() {
        for (auto& pair : m_cache) {
            if (pair.second != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, pair.second, nullptr);
            }
        }
        m_cache.clear();
    }

    VkPipeline PipelineFactory::CreatePipeline(const PipelineCreatePayload& payload) const {
        MOBILEGL_ASSERT(payload.stages != nullptr && !payload.stages->empty(), "PipelineFactory: stages are empty");
        MOBILEGL_ASSERT(payload.vertexInputState != nullptr, "PipelineFactory: vertexInputState is null");
        MOBILEGL_ASSERT(payload.pipelineLayout != VK_NULL_HANDLE, "PipelineFactory: pipelineLayout is null");
        MOBILEGL_ASSERT(payload.renderPass != VK_NULL_HANDLE, "PipelineFactory: renderPass is null");
        MOBILEGL_ASSERT(payload.colorAttachmentCount <= PipelineCreatePayload::kMaxColorAttachments,
                "PipelineFactory: colorAttachmentCount=%u is unexpectedly large",
                payload.colorAttachmentCount);
        MGLOG_D("PipelineFactory::CreatePipeline: programHash=0x%llx vertexInputHash=0x%llx colorAttachmentCount=%u subpass=%u",
            static_cast<unsigned long long>(payload.programHash),
            static_cast<unsigned long long>(payload.vertexInputHash),
            payload.colorAttachmentCount,
            payload.subpass);

        static constexpr VkDynamicState kDynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_LINE_WIDTH,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE
        };

        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(std::size(kDynamicStates));
        dynamicState.pDynamicStates = kDynamicStates;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = payload.topology;

        VkPipelineViewportStateCreateInfo vpci{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vpci.viewportCount = 1;
        vpci.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = payload.cullMode;
        raster.frontFace = payload.frontFace;
        raster.depthBiasEnable = payload.depthBiasEnable ? VK_TRUE : VK_FALSE;
        raster.rasterizerDiscardEnable = payload.rasterizerDiscardEnable ? VK_TRUE : VK_FALSE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = payload.rasterizationSamples;

        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = payload.depthTestEnable ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = payload.depthWriteEnable ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = payload.depthCompareOp;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = payload.stencilTestEnable ? VK_TRUE : VK_FALSE;
        if (payload.stencilTestEnable) {
            depthStencil.front.failOp = payload.frontStencilFailOp;
            depthStencil.front.passOp = payload.frontStencilPassOp;
            depthStencil.front.depthFailOp = payload.frontStencilDepthFailOp;
            depthStencil.front.compareOp = payload.frontStencilCompareOp;
            depthStencil.front.compareMask = 0xffffffffu;
            depthStencil.front.writeMask = 0xffffffffu;
            depthStencil.front.reference = 0;
            depthStencil.back.failOp = payload.backStencilFailOp;
            depthStencil.back.passOp = payload.backStencilPassOp;
            depthStencil.back.depthFailOp = payload.backStencilDepthFailOp;
            depthStencil.back.compareOp = payload.backStencilCompareOp;
            depthStencil.back.compareMask = 0xffffffffu;
            depthStencil.back.writeMask = 0xffffffffu;
            depthStencil.back.reference = 0;
        }

        Vector<VkPipelineColorBlendAttachmentState> colorAttachments(payload.colorAttachmentCount);
        for (Uint32 i = 0; i < payload.colorAttachmentCount; ++i) {
            colorAttachments[i] = payload.colorBlendAttachments[i];
        }
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.logicOpEnable = payload.logicOpEnable ? VK_TRUE : VK_FALSE;
        blend.logicOp = payload.logicOp;
        blend.attachmentCount = payload.colorAttachmentCount;
        blend.pAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();

        VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi.stageCount = static_cast<Uint32>(payload.stages->size());
        gpi.pStages = payload.stages->data();
        gpi.pVertexInputState = payload.vertexInputState;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vpci;
        gpi.pRasterizationState = &raster;
        gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &depthStencil;
        gpi.pColorBlendState = &blend;
        gpi.pDynamicState = &dynamicState;
        gpi.layout = payload.pipelineLayout;
        gpi.renderPass = payload.renderPass;
        gpi.subpass = payload.subpass;

        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &gpi, nullptr, &pipeline),
                  "vkCreateGraphicsPipelines");
        return pipeline;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
