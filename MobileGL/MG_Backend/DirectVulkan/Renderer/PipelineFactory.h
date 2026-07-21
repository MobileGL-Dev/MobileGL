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
            Bool primitiveRestartEnable = false;
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
            // The fragment module writes gl_FragDepth (SPIR-V DepthReplacing); exempts the
            // pipeline from the blended depth-write quirk (see ShouldSuppressDepthWrite).
            Bool fragmentReplacesDepth = false;
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

        // Driver quirk: suppress depth writes on accumulation-blended pipelines. Multi-pass
        // depth-equality rendering (a blended prepass writes depth that later passes re-test
        // with an equality-inclusive compare on the re-rasterized geometry) requires
        // cross-pipeline position invariance that some mobile compilers do not provide, even
        // with the SPIR-V Invariant decoration; whole primitives then drop out of the later
        // passes. Only MIN/MAX extremum blends are stripped - the signature of such a
        // chain's depth-bounds pass (MC 26.3 OIT), and per a fixture-wide trace sweep the
        // only depth-writing shape the chain actually uses - so every other blend
        // (sorted-transparency "over" like vanilla MC water, additive glows, ...) keeps
        // its depth writes. Set at renderer initialization based on the active driver.
        static void SetSuppressBlendedDepthWrite(Bool enabled);
        static Bool IsSuppressBlendedDepthWriteEnabled() { return s_suppressBlendedDepthWrite; }
        // Device gate for the quirk: ForceOn/ForceOff bypass detection, Auto enables it on
        // the known-affected vendor (Qualcomm).
        static Bool ShouldSuppressBlendedDepthWriteForDevice(MG_Config::QuirkOverride quirkOverride,
                                                             Uint32 vendorId);
        // Pure per-pipeline strip decision (exempts gl_FragDepth writers, masked-out and
        // non-accumulation blends); combined with the device flag in CreatePipeline. Static
        // and payload-only so tests can pin the contract without a VkDevice.
        static Bool ShouldSuppressDepthWrite(const PipelineCreatePayload& payload);

    private:
        VkPipeline CreatePipeline(const PipelineCreatePayload& payload) const;

        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanRendererConfig& m_config;
        VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
        UnorderedMap<HashType, VkPipeline> m_cache;
        static inline XXH64_state_t* m_hashState = XXH64_createState();
        static inline Bool s_suppressBlendedDepthWrite = false;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
