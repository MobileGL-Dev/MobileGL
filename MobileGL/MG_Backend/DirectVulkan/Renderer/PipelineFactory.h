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
    // Enough of a fingerprint to identify the exact module the driver rejected without keeping the
    // SPIR-V alive for every program in the cache: a driver that answers VK_ERROR_UNKNOWN tells us
    // nothing, so the log has to carry the shader's identity itself. Diagnostic only - never part
    // of any pipeline or program hash.
    struct ShaderStageSpirvDigest {
        Uint32 stage = 0; // VkShaderStageFlagBits
        Uint32 wordCount = 0;
        Uint64 hash = 0;
    };

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
            // GL_PATCH_VERTICES; only read for a PATCH_LIST topology.
            Uint32 patchControlPoints = 3;
            // How many of ARB_viewport_array's viewports this pipeline rasterizes into. 1 for
            // every program that never assigns gl_ViewportIndex, which is all of them outside the
            // conformance suite - the wide shape costs a longer vkCmdSetViewport/Scissor per state
            // change and can cost hardware fast paths, so it is opt-in per program. Baked into the
            // pipeline (viewportCount is not dynamic without VK_EXT_extended_dynamic_state) and
            // therefore hashed; the DYNAMIC viewport/scissor arrays the draw pushes must have
            // exactly this many elements (VUID-vkCmdDraw-viewportCount-03417/-03418).
            Uint32 viewportCount = 1;
            VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
            VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
            VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE;
            // GL's provoking vertex, baked into the pipeline (VK_EXT_provoking_vertex). It selects
            // which vertex a flat varying takes AND the vertex order transform feedback records for
            // strips/fans, so it is part of the pipeline's identity, not dynamic state. Defaults to
            // Vulkan's own convention, which is what a device without the extension gets.
            VkProvokingVertexModeEXT provokingVertexMode = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
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
            // The tessellation control stage this renderer synthesized for a program that has
            // an evaluation stage and none of its own (GL 4.6 core 11.2.2 gives such a program a
            // fixed-function pass-through; Vulkan has no such thing and
            // VUID-VkGraphicsPipelineCreateInfo-pStages-00730 forbids the half-tessellated
            // pipeline outright). Appended to `stages` at creation. A null module means the
            // renderer could not build one, and CreatePipeline refuses the pipeline - the same
            // refusal it applies when `stages` itself is half-tessellated.
            //
            // NOT hashed: it is a pure function of the program and of patchControlPoints, both
            // of which ComputeHash already mixes in.
            VkPipelineShaderStageCreateInfo passthroughTessControlStage{};
            const VkPipelineVertexInputStateCreateInfo* vertexInputState = nullptr;
            // Diagnostic only; may be null. Read solely from the pipeline-creation failure path.
            const Vector<ShaderStageSpirvDigest>* stageSpirvDigests = nullptr;
        };

        explicit PipelineFactory(VkDevice device, const VulkanRendererConfig& config);
        ~PipelineFactory();
        PipelineFactory(const PipelineFactory&) = delete;

        HashType ComputeHash(const PipelineCreatePayload& payload) const;
        VkPipeline GetOrCreatePipeline(const PipelineCreatePayload& payload);
        void DestroyAll();

        // Frame boundary hook: ages the pipeline cache and destroys long-unused entries
        // (their command buffers retired many frames ago), mirroring
        // VkRenderPassManager::OnPresent's sweep. Returns the number of pipelines
        // destroyed so the caller can drop any memoized VkPipeline handle.
        Uint32 OnFrameBoundary();
        // Destroys every cached pipeline hashed on one of `renderPasses`. Only safe
        // when the caller guarantees GPU idleness for them - the render-pass manager
        // calls this (via the renderer) for passes its own >1024-boundary-idle sweep
        // just evicted, and a pipeline hashed on those handles is only ever bound by
        // draws that also hit the render-pass entries. Also closes the handle-recycling
        // hazard: a recycled VkRenderPass value must never serve a stale pipeline.
        // Batched: one cache scan regardless of how many passes died in the sweep.
        // Returns the number destroyed (callers invalidate memos when non-zero).
        Uint32 EvictByRenderPasses(const Vector<VkRenderPass>& renderPasses);
        // Destroys every cached pipeline built from the program with content hash
        // `programHash`. Called from the ProgramFactory eviction path, which proves the
        // same >1024-boundary idleness (the program's pipelines are only bound by draws
        // that stamp its factory entry). Returns the number destroyed.
        Uint32 EvictByProgramHash(HashType programHash);

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
        struct PipelineCacheEntry {
            VkPipeline pipeline = VK_NULL_HANDLE;
            // The hashed inputs the eviction paths key on: programHash ties the entry to
            // its ProgramFactory entry, renderPass records the exact handle the hash
            // folded in (the hash is one-way, so targeted eviction needs them verbatim).
            HashType programHash = 0;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            // Frame-boundary counter value of the last GetOrCreatePipeline hit; drives
            // cache eviction (see OnFrameBoundary).
            Uint64 lastUsedFrame = 0;
        };

        VkPipeline CreatePipeline(const PipelineCreatePayload& payload) const;

        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanRendererConfig& m_config;
        VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
        UnorderedMap<HashType, PipelineCacheEntry> m_cache;
        // Monotonic frame-boundary counter (bumped in OnFrameBoundary) for cache aging.
        Uint64 m_frameCounter = 0;
        static inline XXH64_state_t* m_hashState = XXH64_createState();
        static inline Bool s_suppressBlendedDepthWrite = false;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
