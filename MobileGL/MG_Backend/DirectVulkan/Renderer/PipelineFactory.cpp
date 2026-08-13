// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/PipelineFactory.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PipelineFactory.h"

#include <algorithm>

namespace MobileGL::MG_Backend::DirectVulkan {
    static const char* PrimitiveTopologyToString(VkPrimitiveTopology topology) {
        switch (topology) {
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY)
        ENUM_STR_CASE(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
        default:
            return "VK_PRIMITIVE_TOPOLOGY_UNKNOWN";
        }
    }

    static const char* SampleCountToString(VkSampleCountFlagBits sampleCount) {
        switch (sampleCount) {
        ENUM_STR_CASE(VK_SAMPLE_COUNT_1_BIT)
        ENUM_STR_CASE(VK_SAMPLE_COUNT_2_BIT)
        ENUM_STR_CASE(VK_SAMPLE_COUNT_4_BIT)
        ENUM_STR_CASE(VK_SAMPLE_COUNT_8_BIT)
        ENUM_STR_CASE(VK_SAMPLE_COUNT_16_BIT)
        ENUM_STR_CASE(VK_SAMPLE_COUNT_32_BIT)
        ENUM_STR_CASE(VK_SAMPLE_COUNT_64_BIT)
        default:
            return "VK_SAMPLE_COUNT_UNKNOWN";
        }
    }

    static const char* CullModeToString(VkCullModeFlags cullMode) {
        switch (cullMode) {
        case VK_CULL_MODE_NONE:
            return "VK_CULL_MODE_NONE";
        case VK_CULL_MODE_FRONT_BIT:
            return "VK_CULL_MODE_FRONT_BIT";
        case VK_CULL_MODE_BACK_BIT:
            return "VK_CULL_MODE_BACK_BIT";
        case VK_CULL_MODE_FRONT_AND_BACK:
            return "VK_CULL_MODE_FRONT_AND_BACK";
        default:
            return "VK_CULL_MODE_UNKNOWN";
        }
    }

    static const char* CompareOpToString(VkCompareOp compareOp) {
        switch (compareOp) {
        ENUM_STR_CASE(VK_COMPARE_OP_NEVER)
        ENUM_STR_CASE(VK_COMPARE_OP_LESS)
        ENUM_STR_CASE(VK_COMPARE_OP_EQUAL)
        ENUM_STR_CASE(VK_COMPARE_OP_LESS_OR_EQUAL)
        ENUM_STR_CASE(VK_COMPARE_OP_GREATER)
        ENUM_STR_CASE(VK_COMPARE_OP_NOT_EQUAL)
        ENUM_STR_CASE(VK_COMPARE_OP_GREATER_OR_EQUAL)
        ENUM_STR_CASE(VK_COMPARE_OP_ALWAYS)
        default:
            return "VK_COMPARE_OP_UNKNOWN";
        }
    }

    static const char* LogicOpToString(VkLogicOp logicOp) {
        switch (logicOp) {
        ENUM_STR_CASE(VK_LOGIC_OP_CLEAR)
        ENUM_STR_CASE(VK_LOGIC_OP_AND)
        ENUM_STR_CASE(VK_LOGIC_OP_AND_REVERSE)
        ENUM_STR_CASE(VK_LOGIC_OP_COPY)
        ENUM_STR_CASE(VK_LOGIC_OP_AND_INVERTED)
        ENUM_STR_CASE(VK_LOGIC_OP_NO_OP)
        ENUM_STR_CASE(VK_LOGIC_OP_XOR)
        ENUM_STR_CASE(VK_LOGIC_OP_OR)
        ENUM_STR_CASE(VK_LOGIC_OP_NOR)
        ENUM_STR_CASE(VK_LOGIC_OP_EQUIVALENT)
        ENUM_STR_CASE(VK_LOGIC_OP_INVERT)
        ENUM_STR_CASE(VK_LOGIC_OP_OR_REVERSE)
        ENUM_STR_CASE(VK_LOGIC_OP_COPY_INVERTED)
        ENUM_STR_CASE(VK_LOGIC_OP_OR_INVERTED)
        ENUM_STR_CASE(VK_LOGIC_OP_NAND)
        ENUM_STR_CASE(VK_LOGIC_OP_SET)
        default:
            return "VK_LOGIC_OP_UNKNOWN";
        }
    }

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

    // Must be called once, before any pipeline is created: the flag is not part of the
    // pipeline hash, so flipping it mid-life would serve cached pipelines built under the
    // old value.
    void PipelineFactory::SetSuppressBlendedDepthWrite(Bool enabled) {
        s_suppressBlendedDepthWrite = enabled;
    }

    Bool PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(MG_Config::QuirkOverride quirkOverride,
                                                                   Uint32 vendorId) {
        static constexpr Uint32 kVendorIdQualcomm = 0x5143;
        switch (quirkOverride) {
        case MG_Config::QuirkOverride::ForceOn:
            return true;
        case MG_Config::QuirkOverride::ForceOff:
            return false;
        case MG_Config::QuirkOverride::Auto:
        default:
            return vendorId == kVendorIdQualcomm;
        }
    }

    namespace {
        // MIN/MAX extremum blending: the signature of a depth-bounds accumulation pass
        // (MC 26.3 OIT writes vec4(-linD, linD, deviceZ, 0) under GL_MAX while writing
        // depth for its equality chain). MIN/MAX ignore blend factors per the Vulkan spec.
        //
        // Deliberately the ONLY shape stripped. A quirk should touch as little unrelated
        // content as possible, and a trace sweep of every fixture showed the wider
        // alternatives all cost more than they fix:
        //   - additive ONE+ONE with a depth write matched zero draws of the 26.3 chain
        //     (its transmittance/accumulate passes disable depth writes themselves) - the
        //     only real content it caught was harmless additive glow effects (Create);
        //   - sorted-transparency "over" blends (SRC_ALPHA-style) are order-dependent,
        //     drawn once per surface, and rely on their depth writes for occlusion;
        //   - separate-alpha accumulation over an over-blending color channel has no
        //     known pairing with a depth-equality chain (color channel only, see tests).
        // If a future workload pairs another blend shape with an equality chain, widen
        // this with that evidence in hand rather than pre-emptively.
        Bool IsAccumulationBlend(const VkPipelineColorBlendAttachmentState& attachment) {
            return attachment.colorBlendOp == VK_BLEND_OP_MIN ||
                   attachment.colorBlendOp == VK_BLEND_OP_MAX;
        }
    } // namespace

    Bool PipelineFactory::ShouldSuppressDepthWrite(const PipelineCreatePayload& payload) {
        if (!payload.depthWriteEnable) {
            return false;
        }
        // A shader that assigns gl_FragDepth supplies depth itself rather than taking the
        // pipeline's interpolated Z, so a driver that varies the vertex position math
        // between pipelines cannot desynchronize it. (A gl_FragDepth = gl_FragCoord.z
        // passthrough is the exception that stays exposed; no known content pairs one with
        // an equality chain, and 26.3's composite is a genuine computed-depth writer.)
        if (payload.fragmentReplacesDepth) {
            return false;
        }
        for (Uint32 i = 0; i < payload.colorAttachmentCount; ++i) {
            const VkPipelineColorBlendAttachmentState& attachment = payload.colorBlendAttachments[i];
            if (attachment.blendEnable != VK_TRUE) {
                continue;
            }
            // All color writes masked: blending is moot (depth-prepass pattern that left
            // GL_BLEND enabled); stripping the depth write would delete the whole prepass.
            if (attachment.colorWriteMask == 0) {
                continue;
            }
            // Any attachment qualifies, not just attachment 0: the 26.3 transmittance pass
            // accumulates into a 2-target MRT and must stay stripped.
            if (IsAccumulationBlend(attachment)) {
                return true;
            }
        }
        return false;
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
        XXHASH_VERIFY(
            XXH64_update(m_hashState, &payload.primitiveRestartEnable, sizeof(payload.primitiveRestartEnable)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.patchControlPoints, sizeof(payload.patchControlPoints)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.viewportCount, sizeof(payload.viewportCount)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.polygonMode, sizeof(payload.polygonMode)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.cullMode, sizeof(payload.cullMode)));
        XXHASH_VERIFY(XXH64_update(m_hashState, &payload.frontFace, sizeof(payload.frontFace)));
        XXHASH_VERIFY(
            XXH64_update(m_hashState, &payload.provokingVertexMode, sizeof(payload.provokingVertexMode)));
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
        XXHASH_VERIFY(
            XXH64_update(m_hashState, &payload.fragmentReplacesDepth, sizeof(payload.fragmentReplacesDepth)));
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
            it->second.lastUsedFrame = m_frameCounter;
            return it->second.pipeline;
        }

        VkPipeline pipeline = CreatePipeline(payload);
        // A failed creation must never be memoized. Caching VK_NULL_HANDLE served the null back for
        // the rest of the process, so one transient driver rejection turned every later draw with
        // the same state into a vkCmdBindPipeline(VK_NULL_HANDLE) - the SIGSEGV behind 9 of the 15
        // CTS process deaths. Retrying costs one failed vkCreateGraphicsPipelines per draw, which
        // is the correct price for a broken pipeline and is bounded by the draw itself being
        // skipped.
        if (pipeline == VK_NULL_HANDLE) {
            // Unlatched, like the CreatePipeline report it accompanies: a pipeline MobileGL
            // assembled and the driver refused is a broken invariant, not an expected failure,
            // so it stays loud for as long as it is reachable. Raised from MGLOG_I once the
            // Log.h ordering fix made MGLOG_E live in INFO builds.
            MGLOG_E("PipelineFactory::GetOrCreatePipeline: creation failed for hash=0x%llx "
                    "programHash=0x%llx; not caching the failure",
                    static_cast<unsigned long long>(hash),
                    static_cast<unsigned long long>(payload.programHash));
            return VK_NULL_HANDLE;
        }
        m_cache.emplace(hash, PipelineCacheEntry{pipeline, payload.programHash, payload.renderPass,
                                                 m_frameCounter});
        return pipeline;
    }

    void PipelineFactory::DestroyAll() {
        for (auto& pair : m_cache) {
            if (pair.second.pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, pair.second.pipeline, nullptr);
            }
        }
        m_cache.clear();
    }

    Uint32 PipelineFactory::OnFrameBoundary() {
        ++m_frameCounter;

        // Sweep cadence and retire age mirror VkRenderPassManager::OnPresent: an entry
        // idle for more than kRetireAgeFrames frame boundaries cannot be referenced by
        // any in-flight command buffer (frames-in-flight <= MOBILEGL_MAGMA_FRAMESINFLIGHT),
        // so immediate vkDestroyPipeline is safe. The caller must drop its "last
        // pipeline" memo when this returns non-zero: the memo can return a cached
        // handle without touching this cache, so an evicted pipeline may still be
        // memoized (present-less flush loops never reset the memo per frame).
        constexpr Uint64 kSweepInterval = 256;
        constexpr Uint64 kRetireAgeFrames = 1024;
        if ((m_frameCounter % kSweepInterval) != 0) {
            return 0;
        }

        Uint32 evicted = 0;
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (m_frameCounter - it->second.lastUsedFrame > kRetireAgeFrames) {
                if (it->second.pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(m_device, it->second.pipeline, nullptr);
                }
                it = m_cache.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        if (evicted > 0) {
            MGLOG_D("PipelineFactory::OnFrameBoundary: evicted %u idle pipelines (%zu remain)", evicted,
                    m_cache.size());
        }
        return evicted;
    }

    Uint32 PipelineFactory::EvictByRenderPasses(const Vector<VkRenderPass>& renderPasses) {
        if (renderPasses.empty() || m_cache.empty()) {
            return 0;
        }
        // Sorted-batch membership test keeps a mass eviction (shader-pack switch,
        // dimension exit) at one O(cache * log batch) scan instead of one full scan
        // per dying pass.
        Vector<VkRenderPass> sortedPasses = renderPasses;
        std::sort(sortedPasses.begin(), sortedPasses.end());
        Uint32 evicted = 0;
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (std::binary_search(sortedPasses.begin(), sortedPasses.end(), it->second.renderPass)) {
                if (it->second.pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(m_device, it->second.pipeline, nullptr);
                }
                it = m_cache.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        if (evicted > 0) {
            MGLOG_D("PipelineFactory::EvictByRenderPasses: evicted %u pipelines for %zu destroyed render passes",
                    evicted, sortedPasses.size());
        }
        return evicted;
    }

    Uint32 PipelineFactory::EvictByProgramHash(HashType programHash) {
        Uint32 evicted = 0;
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (it->second.programHash == programHash) {
                if (it->second.pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(m_device, it->second.pipeline, nullptr);
                }
                it = m_cache.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        if (evicted > 0) {
            MGLOG_D("PipelineFactory::EvictByProgramHash: evicted %u pipelines for program hash 0x%llx",
                    evicted, static_cast<unsigned long long>(programHash));
        }
        return evicted;
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
        ia.primitiveRestartEnable = payload.primitiveRestartEnable ? VK_TRUE : VK_FALSE;

        // Only a patch topology has a tessellation stage to configure; leaving the pointer null
        // otherwise is what the spec expects.
        VkPipelineTessellationStateCreateInfo tessellation{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        tessellation.patchControlPoints = payload.patchControlPoints;

        VkPipelineViewportStateCreateInfo vpci{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        // Both counts move together: GL has one scissor rectangle per viewport, and Vulkan
        // requires viewportCount == scissorCount whenever both are dynamic
        // (VUID-VkPipelineViewportStateCreateInfo-scissorCount-04136). The caller has already
        // clamped this to the device's multiViewport capability.
        vpci.viewportCount = std::max<Uint32>(payload.viewportCount, 1u);
        vpci.scissorCount = vpci.viewportCount;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = payload.polygonMode;
        raster.cullMode = payload.cullMode;
        raster.frontFace = payload.frontFace;
        raster.depthBiasEnable = payload.depthBiasEnable ? VK_TRUE : VK_FALSE;
        raster.rasterizerDiscardEnable = payload.rasterizerDiscardEnable ? VK_TRUE : VK_FALSE;
        raster.lineWidth = 1.0f;
        // Only chain the struct when the mode is not Vulkan's implicit default: a device without
        // VK_EXT_provoking_vertex enabled must never see this pNext entry, and the renderer's
        // selector already collapses to FIRST in exactly that case - so a device without the
        // extension produces a byte-identical VkGraphicsPipelineCreateInfo to before.
        VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provokingVertexState{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
        if (payload.provokingVertexMode != VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT) {
            provokingVertexState.provokingVertexMode = payload.provokingVertexMode;
            provokingVertexState.pNext = raster.pNext;
            raster.pNext = &provokingVertexState;
        }

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
        // Suppress depth writes on accumulation-blended pipelines when the active driver
        // cannot keep vertex positions invariant across the pipelines of a multi-pass
        // depth-equality chain (see SetSuppressBlendedDepthWrite). The decision is narrowed
        // in ShouldSuppressDepthWrite: sorted-transparency "over" blends (vanilla MC water),
        // gl_FragDepth writers, and masked-out attachments keep their depth writes.
        // This bakes the decision into the pipeline, which only works because depth write is
        // static state here - adding VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE to kDynamicStates
        // would let the record-time value override it and silently disable the quirk.
        if (s_suppressBlendedDepthWrite && ShouldSuppressDepthWrite(payload)) {
            depthStencil.depthWriteEnable = VK_FALSE;
        }
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.logicOpEnable = payload.logicOpEnable ? VK_TRUE : VK_FALSE;
        blend.logicOp = payload.logicOp;
        blend.attachmentCount = payload.colorAttachmentCount;
        blend.pAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();

        // A GL program may have a tessellation EVALUATION stage and no CONTROL stage: GL 4.6 core
        // 11.2.2 gives it a fixed-function pass-through instead. Vulkan has no such stage, and
        // VUID-VkGraphicsPipelineCreateInfo-pStages-00730 requires both tessellation stages or
        // neither - so the renderer synthesizes the pass-through GL describes and hands it in
        // here (see ProgramFactory::GetOrCreatePassthroughTessControlStage).
        //
        // The refusal below is what keeps the half-tessellated shape away from the driver when
        // there is no synthesized stage to add - because Mali does not reject it, it dereferences
        // null INSIDE vkCreateGraphicsPipelines and takes the process down (SIGSEGV, fault addr
        // 0x34, on Mali-G715/r54p2 and Mali-G925/r49p1 alike; Adreno and lavapipe merely render
        // wrong). Returning VK_NULL_HANDLE routes this through the same path a driver rejection
        // takes: the draw is skipped, nothing is memoised, and the process survives.
        const Vector<VkPipelineShaderStageCreateInfo>* effectiveStages = payload.stages;
        Vector<VkPipelineShaderStageCreateInfo> stagesWithPassthrough;
        if (payload.passthroughTessControlStage.module != VK_NULL_HANDLE) {
            stagesWithPassthrough = *payload.stages;
            stagesWithPassthrough.push_back(payload.passthroughTessControlStage);
            effectiveStages = &stagesWithPassthrough;
        }
        {
            VkShaderStageFlags stagesPresent = 0;
            for (const auto& stageInfo : *effectiveStages) {
                stagesPresent |= stageInfo.stage;
            }
            const Bool hasTessControl = (stagesPresent & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) != 0;
            const Bool hasTessEval = (stagesPresent & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) != 0;
            if (hasTessControl != hasTessEval) {
                // Latched, and the latch is the point: a failed creation is deliberately never
                // memoised (see GetOrCreatePipeline), so a program in this state re-enters here
                // once per draw, every frame - and a refusal diagnostic that repeats per draw is
                // noise, not a diagnostic. One line names the program; the draws it explains are
                // all the same draw.
                static Bool s_warnedHalfTessellatedPipeline = false;
                if (!s_warnedHalfTessellatedPipeline) {
                    s_warnedHalfTessellatedPipeline = true;
                    MGLOG_E_ONCE("PipelineFactory::CreatePipeline: refusing a pipeline with %s tessellation stage and "
                            "no %s stage (VUID-VkGraphicsPipelineCreateInfo-pStages-00730). programHash=0x%llx "
                            "patchControlPoints=%u. Its draws are skipped; logged once.",
                            hasTessEval ? "an evaluation" : "a control",
                            hasTessEval ? "control" : "evaluation",
                            static_cast<unsigned long long>(payload.programHash),
                            payload.patchControlPoints);
                }
                return VK_NULL_HANDLE;
            }
        }

        VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi.stageCount = static_cast<Uint32>(effectiveStages->size());
        gpi.pStages = effectiveStages->data();
        gpi.pVertexInputState = payload.vertexInputState;
        gpi.pInputAssemblyState = &ia;
        gpi.pTessellationState =
            payload.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? &tessellation : nullptr;
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
        const VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &gpi, nullptr, &pipeline);
        // Loud, at MGLOG_F, and deliberately NOT latched. vkCreateGraphicsPipelines refusing a
        // pipeline MobileGL assembled is a should-never-happen state, and the driver's own
        // answer is VK_ERROR_UNKNOWN - no information at all - so this dump is the entire
        // diagnosis. It is not an expected failure mode, so the one-shot rule that quiets W/E
        // does not apply: while this is reachable it should keep saying so on every draw.
        // GetOrCreatePipeline deliberately does not cache the failure, which is what makes that
        // repetition happen; if the repetition ever needs to stop, fix the pipeline, not the log.
        if (result != VK_SUCCESS) {
            MGLOG_F("PipelineFactory::CreatePipeline failed: result=%s (%d) programHash=0x%llx vertexInputHash=0x%llx stageCount=%u topology=%s(%d) colorAttachmentCount=%u samples=%s(%d) subpass=%u",
                    VkResultToString(result),
                    result,
                    static_cast<unsigned long long>(payload.programHash),
                    static_cast<unsigned long long>(payload.vertexInputHash),
                    gpi.stageCount,
                    PrimitiveTopologyToString(payload.topology),
                    payload.topology,
                    payload.colorAttachmentCount,
                    SampleCountToString(payload.rasterizationSamples),
                    payload.rasterizationSamples,
                    payload.subpass);
            MGLOG_F("PipelineFactory::CreatePipeline state: cullMode=%s(0x%x) frontFace=%d depthTest=%d depthWrite=%d depthCompare=%s(%d) depthBias=%d rasterizerDiscard=%d stencilTest=%d logicOpEnable=%d logicOp=%s(%d)",
                    CullModeToString(payload.cullMode),
                    static_cast<Uint32>(payload.cullMode),
                    payload.frontFace,
                    payload.depthTestEnable ? 1 : 0,
                    payload.depthWriteEnable ? 1 : 0,
                    CompareOpToString(payload.depthCompareOp),
                    payload.depthCompareOp,
                    payload.depthBiasEnable ? 1 : 0,
                    payload.rasterizerDiscardEnable ? 1 : 0,
                    payload.stencilTestEnable ? 1 : 0,
                    payload.logicOpEnable ? 1 : 0,
                    LogicOpToString(payload.logicOp),
                    payload.logicOp);
            MGLOG_F("PipelineFactory::CreatePipeline vertex input: bindingCount=%u attributeCount=%u",
                    payload.vertexInputState->vertexBindingDescriptionCount,
                    payload.vertexInputState->vertexAttributeDescriptionCount);
            // The driver's own answer is VK_ERROR_UNKNOWN, i.e. no information at all, so the only
            // way to work out WHICH shader it choked on (the open sampler-array-in-struct
            // investigation) is to name the modules. MGLOG_I, not _D: this is part of a
            // should-never-happen report and must survive in the INFO-level builds that CTS
            // actually runs against, alongside the MGLOG_F lines above.
            if (payload.stageSpirvDigests) {
                for (SizeT i = 0; i < payload.stageSpirvDigests->size(); ++i) {
                    const auto& digest = (*payload.stageSpirvDigests)[i];
                    MGLOG_I("PipelineFactory::CreatePipeline spirv[%zu]: stage=0x%x words=%u bytes=%zu "
                            "hash=0x%llx",
                            i, digest.stage, digest.wordCount,
                            static_cast<SizeT>(digest.wordCount) * sizeof(Uint32),
                            static_cast<unsigned long long>(digest.hash));
                }
            } else {
                MGLOG_I("PipelineFactory::CreatePipeline: no SPIR-V digests attached to the payload");
            }
            if (payload.stages) {
                for (SizeT i = 0; i < payload.stages->size(); ++i) {
                    const auto& stage = (*payload.stages)[i];
                    // VkShaderModule is a non-dispatchable handle: a pointer on 64-bit but a
                    // plain uint64_t on 32-bit ABIs, where a cast to const void* is ill-formed
                    // (broke the armeabi-v7a build). Print it as the 64-bit value it is.
                    MGLOG_I("PipelineFactory::CreatePipeline stage[%zu]: stage=0x%x module=0x%llx entry=%s "
                            "specialization=%d",
                            i, static_cast<Uint32>(stage.stage),
                            static_cast<unsigned long long>(reinterpret_cast<Uint64>(stage.module)),
                            stage.pName ? stage.pName : "(null)", stage.pSpecializationInfo ? 1 : 0);
                }
            }
            for (Uint32 i = 0; i < payload.colorAttachmentCount; ++i) {
                const auto& attachment = payload.colorBlendAttachments[i];
                MGLOG_F("PipelineFactory::CreatePipeline colorAttachment[%u]: blend=%d colorWriteMask=0x%x srcColor=%d dstColor=%d colorOp=%d srcAlpha=%d dstAlpha=%d alphaOp=%d",
                        i,
                        attachment.blendEnable == VK_TRUE ? 1 : 0,
                        static_cast<Uint32>(attachment.colorWriteMask),
                        attachment.srcColorBlendFactor,
                        attachment.dstColorBlendFactor,
                        attachment.colorBlendOp,
                        attachment.srcAlphaBlendFactor,
                        attachment.dstAlphaBlendFactor,
                        attachment.alphaBlendOp);
            }
        }
        VK_VERIFY(result, "vkCreateGraphicsPipelines");
        return pipeline;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
