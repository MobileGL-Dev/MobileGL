// MobileGL - MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// See the header for what is being measured and why. The plumbing here is shaped
// like the POST timestamp probe (DriverPost.cpp, ProbeVulkanTimerQuery): one
// throwaway command buffer, a bounded fence wait that deliberately leaks the
// device objects rather than idle-wait a hung GPU, and teardown on every path.

#include "PrimitivesGeneratedNoXfbProbe.h"
#include "PrimitivesGeneratedNoXfbProbeSpv.h"

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        template <typename Callable>
        struct ProbeScopeGuard {
            explicit ProbeScopeGuard(Callable callable) : onExit(Move(callable)) {}
            ProbeScopeGuard(const ProbeScopeGuard&) = delete;
            ProbeScopeGuard& operator=(const ProbeScopeGuard&) = delete;
            ~ProbeScopeGuard() { onExit(); }

        private:
            Callable onExit;
        };

        Bool AllRequiredFnsPresent(const PrimitivesGeneratedNoXfbProbeFns& fns) {
            return fns.vkCreateCommandPool != nullptr && fns.vkDestroyCommandPool != nullptr &&
                   fns.vkAllocateCommandBuffers != nullptr && fns.vkBeginCommandBuffer != nullptr &&
                   fns.vkEndCommandBuffer != nullptr && fns.vkCreateQueryPool != nullptr &&
                   fns.vkDestroyQueryPool != nullptr && fns.vkCmdResetQueryPool != nullptr &&
                   fns.vkCmdBeginQuery != nullptr && fns.vkCmdEndQuery != nullptr &&
                   fns.vkCmdBeginQueryIndexedEXT != nullptr && fns.vkCmdEndQueryIndexedEXT != nullptr &&
                   fns.vkCreateRenderPass != nullptr && fns.vkDestroyRenderPass != nullptr &&
                   fns.vkCreateFramebuffer != nullptr && fns.vkDestroyFramebuffer != nullptr &&
                   fns.vkCmdBeginRenderPass != nullptr && fns.vkCmdEndRenderPass != nullptr &&
                   fns.vkCreateShaderModule != nullptr && fns.vkDestroyShaderModule != nullptr &&
                   fns.vkCreatePipelineLayout != nullptr && fns.vkDestroyPipelineLayout != nullptr &&
                   fns.vkCreateGraphicsPipelines != nullptr && fns.vkDestroyPipeline != nullptr &&
                   fns.vkCmdBindPipeline != nullptr && fns.vkCmdDraw != nullptr &&
                   fns.vkCreateFence != nullptr && fns.vkDestroyFence != nullptr &&
                   fns.vkQueueSubmit != nullptr && fns.vkWaitForFences != nullptr &&
                   fns.vkGetQueryPoolResults != nullptr && fns.vkDeviceWaitIdle != nullptr;
        }
    } // namespace

    PrimitivesGeneratedNoXfbMeasurement RunPrimitivesGeneratedNoXfbProbe(
        const PrimitivesGeneratedNoXfbProbeContext& context) {
        PrimitivesGeneratedNoXfbMeasurement measurement;
        const auto fail = [&](String reason) {
            measurement.ran = false;
            measurement.failureReason = Move(reason);
            return measurement;
        };

        if (!context.transformFeedbackQueriesUsable) {
            return fail("transform feedback stream queries are not usable on this device, so the "
                        "probe has no subject");
        }
        if (context.device == VK_NULL_HANDLE || context.queue == VK_NULL_HANDLE) {
            return fail("no device/queue was supplied");
        }
        const PrimitivesGeneratedNoXfbProbeFns& fns = context.fns;
        if (!AllRequiredFnsPresent(fns)) {
            return fail("a required Vulkan entry point was not resolved");
        }

        VkDevice device = context.device;

        // Slot i of each pool belongs to shape i (0 = triangles plain, 1 = triangles
        // under discard, 2 = patches under discard). Unused slots are reset either
        // way; reset needs no feature and an unqueried reset slot is never read.
        constexpr Uint32 kShapeSlots = 3;
        const Bool drawPatches = context.tessellationEnabled;
        const Bool measureStatistics = context.pipelineStatisticsEnabled;
        // Only with BOTH feature bits: without ...WithRasterizerDiscard, a
        // discarding draw inside the query is invalid usage
        // (VUID-vkCmdDraw-primitivesGeneratedQueryWithRasterizerDiscard-06708),
        // and two of the three shapes discard.
        const Bool measurePrimitivesGeneratedExt = context.primitivesGeneratedQueryUsable;

        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkQueryPool streamQueryPool = VK_NULL_HANDLE;
        VkQueryPool primitivesGeneratedQueryPool = VK_NULL_HANDLE;
        VkQueryPool statisticsQueryPool = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkShaderModule vertModule = VK_NULL_HANDLE;
        VkShaderModule tescModule = VK_NULL_HANDLE;
        VkShaderModule teseModule = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline trianglePlainPipeline = VK_NULL_HANDLE;
        VkPipeline triangleDiscardPipeline = VK_NULL_HANDLE;
        VkPipeline patchDiscardPipeline = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;

        // Teardown on every path. When the fence wait timed out the submission may
        // still be executing on a hung GPU: vkDeviceWaitIdle could block forever
        // and destroying in-flight objects is undefined, so everything is
        // deliberately leaked - a hung GPU must not hang the caller. The same flag
        // is returned in the measurement, because a caller that OWNS the device must
        // make the same choice for it (see the header): destroying a device whose
        // children are alive and whose queue may still be executing is the very hang
        // this bound exists to prevent.
        const ProbeScopeGuard teardown([&]() {
            if (measurement.fenceWaitTimedOut) {
                return;
            }
            fns.vkDeviceWaitIdle(device);
            if (fence != VK_NULL_HANDLE) fns.vkDestroyFence(device, fence, nullptr);
            if (trianglePlainPipeline != VK_NULL_HANDLE)
                fns.vkDestroyPipeline(device, trianglePlainPipeline, nullptr);
            if (triangleDiscardPipeline != VK_NULL_HANDLE)
                fns.vkDestroyPipeline(device, triangleDiscardPipeline, nullptr);
            if (patchDiscardPipeline != VK_NULL_HANDLE)
                fns.vkDestroyPipeline(device, patchDiscardPipeline, nullptr);
            if (pipelineLayout != VK_NULL_HANDLE) fns.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (vertModule != VK_NULL_HANDLE) fns.vkDestroyShaderModule(device, vertModule, nullptr);
            if (tescModule != VK_NULL_HANDLE) fns.vkDestroyShaderModule(device, tescModule, nullptr);
            if (teseModule != VK_NULL_HANDLE) fns.vkDestroyShaderModule(device, teseModule, nullptr);
            if (framebuffer != VK_NULL_HANDLE) fns.vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (renderPass != VK_NULL_HANDLE) fns.vkDestroyRenderPass(device, renderPass, nullptr);
            if (statisticsQueryPool != VK_NULL_HANDLE) fns.vkDestroyQueryPool(device, statisticsQueryPool, nullptr);
            if (primitivesGeneratedQueryPool != VK_NULL_HANDLE)
                fns.vkDestroyQueryPool(device, primitivesGeneratedQueryPool, nullptr);
            if (streamQueryPool != VK_NULL_HANDLE) fns.vkDestroyQueryPool(device, streamQueryPool, nullptr);
            if (commandPool != VK_NULL_HANDLE) fns.vkDestroyCommandPool(device, commandPool, nullptr);
        });

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = context.queueFamilyIndex;
        if (fns.vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            return fail("vkCreateCommandPool failed");
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (fns.vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            return fail("vkAllocateCommandBuffers failed");
        }

        VkQueryPoolCreateInfo streamPoolInfo{};
        streamPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        streamPoolInfo.queryType = VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT;
        streamPoolInfo.queryCount = kShapeSlots;
        if (fns.vkCreateQueryPool(device, &streamPoolInfo, nullptr, &streamQueryPool) != VK_SUCCESS) {
            return fail("vkCreateQueryPool(TRANSFORM_FEEDBACK_STREAM) failed");
        }
        if (measurePrimitivesGeneratedExt) {
            VkQueryPoolCreateInfo pgqPoolInfo{};
            pgqPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            pgqPoolInfo.queryType = VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT;
            pgqPoolInfo.queryCount = kShapeSlots;
            if (fns.vkCreateQueryPool(device, &pgqPoolInfo, nullptr, &primitivesGeneratedQueryPool) !=
                VK_SUCCESS) {
                return fail("vkCreateQueryPool(PRIMITIVES_GENERATED_EXT) failed");
            }
        }
        if (measureStatistics) {
            VkQueryPoolCreateInfo statPoolInfo{};
            statPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            statPoolInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            statPoolInfo.queryCount = kShapeSlots;
            // CLIPPING_INVOCATIONS counts the primitives PROCESSED BY (i.e. reaching)
            // primitive clipping - GL's CLIPPING_INPUT_PRIMITIVES - which is the
            // pre-clip, post-vertex-processing set PRIMITIVES_GENERATED is defined
            // over. CLIPPING_PRIMITIVES (the stage's OUTPUT count) would be wrong:
            // clipping may drop or split primitives.
            statPoolInfo.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT;
            if (fns.vkCreateQueryPool(device, &statPoolInfo, nullptr, &statisticsQueryPool) != VK_SUCCESS) {
                return fail("vkCreateQueryPool(PIPELINE_STATISTICS) failed");
            }
        }

        // Zero-attachment render pass + 1x1 framebuffer: the draw is discarded
        // before rasterization, nothing is ever written, but vkCmdDraw needs a
        // render pass instance to live in.
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        if (fns.vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
            return fail("vkCreateRenderPass failed");
        }
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.width = 1;
        framebufferInfo.height = 1;
        framebufferInfo.layers = 1;
        if (fns.vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
            return fail("vkCreateFramebuffer failed");
        }

        const auto makeModule = [&](const std::uint32_t* words, std::size_t wordCount, VkShaderModule& out) {
            VkShaderModuleCreateInfo moduleInfo{};
            moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            moduleInfo.codeSize = wordCount * sizeof(std::uint32_t);
            moduleInfo.pCode = words;
            return fns.vkCreateShaderModule(device, &moduleInfo, nullptr, &out) == VK_SUCCESS;
        };
        if (!makeModule(kPrimitivesGeneratedNoXfbProbeVertSpv, kPrimitivesGeneratedNoXfbProbeVertSpvWordCount,
                        vertModule)) {
            return fail("vkCreateShaderModule(vert) failed");
        }
        if (drawPatches) {
            if (!makeModule(kPrimitivesGeneratedNoXfbProbeTescSpv, kPrimitivesGeneratedNoXfbProbeTescSpvWordCount,
                            tescModule) ||
                !makeModule(kPrimitivesGeneratedNoXfbProbeTeseSpv, kPrimitivesGeneratedNoXfbProbeTeseSpvWordCount,
                            teseModule)) {
                return fail("vkCreateShaderModule(tesc/tese) failed");
            }
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (fns.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            return fail("vkCreatePipelineLayout failed");
        }

        // With rasterizerDiscardEnable the viewport and multisample state are
        // ignored by the spec, but well-formed ones are supplied anyway: the probe
        // must never be the thing that trips a picky driver. The discard-off
        // variant rasterizes into the zero-attachment subpass, which writes
        // nothing anywhere.
        const auto makePipeline = [&](Bool tessellated, Bool rasterizerDiscard, VkPipeline& out) {
            VkPipelineShaderStageCreateInfo stages[3] = {};
            Uint32 stageCount = 0;
            stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[stageCount].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[stageCount].module = vertModule;
            stages[stageCount].pName = "main";
            ++stageCount;
            if (tessellated) {
                stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[stageCount].stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
                stages[stageCount].module = tescModule;
                stages[stageCount].pName = "main";
                ++stageCount;
                stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[stageCount].stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
                stages[stageCount].module = teseModule;
                stages[stageCount].pName = "main";
                ++stageCount;
            }

            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology =
                tessellated ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineTessellationStateCreateInfo tessellation{};
            tessellation.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
            tessellation.patchControlPoints = 1;

            VkViewport viewport{};
            viewport.width = 1.0f;
            viewport.height = 1.0f;
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{};
            scissor.extent.width = 1;
            scissor.extent.height = 1;
            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.pViewports = &viewport;
            viewportState.scissorCount = 1;
            viewportState.pScissors = &scissor;

            VkPipelineRasterizationStateCreateInfo rasterization{};
            rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterization.rasterizerDiscardEnable = rasterizerDiscard ? VK_TRUE : VK_FALSE;
            rasterization.polygonMode = VK_POLYGON_MODE_FILL;
            rasterization.cullMode = VK_CULL_MODE_NONE;
            rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterization.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisample{};
            multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = stageCount;
            pipelineInfo.pStages = stages;
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pTessellationState = tessellated ? &tessellation : nullptr;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterization;
            pipelineInfo.pMultisampleState = &multisample;
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.renderPass = renderPass;
            pipelineInfo.subpass = 0;
            return fns.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &out) ==
                   VK_SUCCESS;
        };
        if (!makePipeline(false, false, trianglePlainPipeline)) {
            return fail("vkCreateGraphicsPipelines(triangles) failed");
        }
        if (!makePipeline(false, true, triangleDiscardPipeline)) {
            return fail("vkCreateGraphicsPipelines(triangles, discard) failed");
        }
        if (drawPatches && !makePipeline(true, true, patchDiscardPipeline)) {
            return fail("vkCreateGraphicsPipelines(patches, discard) failed");
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fns.vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            return fail("vkBeginCommandBuffer failed");
        }
        fns.vkCmdResetQueryPool(commandBuffer, streamQueryPool, 0, kShapeSlots);
        if (measurePrimitivesGeneratedExt) {
            fns.vkCmdResetQueryPool(commandBuffer, primitivesGeneratedQueryPool, 0, kShapeSlots);
        }
        if (measureStatistics) {
            fns.vkCmdResetQueryPool(commandBuffer, statisticsQueryPool, 0, kShapeSlots);
        }

        VkRenderPassBeginInfo renderPassBegin{};
        renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBegin.renderPass = renderPass;
        renderPassBegin.framebuffer = framebuffer;
        renderPassBegin.renderArea.extent.width = 1;
        renderPassBegin.renderArea.extent.height = 1;
        fns.vkCmdBeginRenderPass(commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);

        // Each query kind wraps ITS OWN replay of the shape's draw, never a shared
        // one. Not pedantry - a co-active control CONTAMINATES the subject:
        // measured on lavapipe, a dedicated primitives-generated query active
        // around the same draw switches llvmpipe's primitive collection on, and
        // the stream query on that draw then answers the exact count it answers 0
        // for when it is alone - which is how the renderer actually runs it. A
        // probe that measured them together certified this driver healthy and
        // repaired nothing. The replays are identical recordings of a
        // deterministic draw, so the per-shape comparison loses nothing.
        const auto recordShape = [&](Uint32 slot, VkPipeline pipeline, Uint32 vertexCount) {
            fns.vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            // THE SUBJECT, alone: no vkCmdBeginTransformFeedbackEXT anywhere in
            // this command buffer - the stream query wraps a draw with transform
            // feedback inactive, exactly the CTS's tessellator-measuring shape.
            fns.vkCmdBeginQueryIndexedEXT(commandBuffer, streamQueryPool, slot, 0, 0);
            fns.vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
            fns.vkCmdEndQueryIndexedEXT(commandBuffer, streamQueryPool, slot, 0);
            if (measurePrimitivesGeneratedExt) {
                // Plain vkCmdBeginQuery: a PRIMITIVES_GENERATED_EXT query begun
                // this way counts vertex stream 0, which is where every non-GS
                // (and default-stream GS) primitive goes.
                fns.vkCmdBeginQuery(commandBuffer, primitivesGeneratedQueryPool, slot, 0);
                fns.vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
                fns.vkCmdEndQuery(commandBuffer, primitivesGeneratedQueryPool, slot);
            }
            if (measureStatistics) {
                fns.vkCmdBeginQuery(commandBuffer, statisticsQueryPool, slot, 0);
                fns.vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
                fns.vkCmdEndQuery(commandBuffer, statisticsQueryPool, slot);
            }
        };
        recordShape(0, trianglePlainPipeline, 3);   // one rasterized triangle
        recordShape(1, triangleDiscardPipeline, 3); // one discarded triangle
        if (drawPatches) {
            // one 1-vertex patch -> one tessellated, discarded triangle
            recordShape(2, patchDiscardPipeline, 1);
        }
        fns.vkCmdEndRenderPass(commandBuffer);
        if (fns.vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            return fail("vkEndCommandBuffer failed");
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (fns.vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            return fail("vkCreateFence failed");
        }
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        if (fns.vkQueueSubmit(context.queue, 1, &submitInfo, fence) != VK_SUCCESS) {
            return fail("vkQueueSubmit failed");
        }
        constexpr Uint64 kFenceTimeoutNs = 5'000'000'000ull; // a probe must never hang its caller
        if (fns.vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
            // Set BEFORE failing: the scope guard reads it to skip every destroy, and
            // the caller reads it out of the measurement to skip destroying the device.
            measurement.fenceWaitTimedOut = true;
            return fail("the probe submission did not complete within 5 s");
        }

        const auto readShape = [&](Uint32 slot, Uint64 expected, PrimitivesGeneratedNoXfbShapeMeasurement& out) {
            Uint64 streamPair[2] = {0, 0}; // {primitivesWritten, primitivesNeeded}
            if (fns.vkGetQueryPoolResults(device, streamQueryPool, slot, 1, sizeof(streamPair), streamPair,
                                          sizeof(streamPair),
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
                return false;
            }
            out.drawn = true;
            out.expectedPrimitives = expected;
            out.streamGenerated = streamPair[1];
            if (measurePrimitivesGeneratedExt) {
                Uint64 generated = 0;
                if (fns.vkGetQueryPoolResults(device, primitivesGeneratedQueryPool, slot, 1, sizeof(generated),
                                              &generated, sizeof(generated),
                                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                    out.primitivesGeneratedExtMeasured = true;
                    out.primitivesGeneratedExt = generated;
                }
            }
            if (measureStatistics) {
                Uint64 clippingInput = 0;
                if (fns.vkGetQueryPoolResults(device, statisticsQueryPool, slot, 1, sizeof(clippingInput),
                                              &clippingInput, sizeof(clippingInput),
                                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                    out.statisticsMeasured = true;
                    out.statisticsClippingInput = clippingInput;
                }
            }
            return true;
        };
        if (!readShape(0, 1, measurement.trianglesPlain)) {
            return fail("vkGetQueryPoolResults(triangles) failed");
        }
        if (!readShape(1, 1, measurement.trianglesDiscard)) {
            return fail("vkGetQueryPoolResults(triangles, discard) failed");
        }
        if (drawPatches && !readShape(2, 1, measurement.patchesDiscard)) {
            return fail("vkGetQueryPoolResults(patches, discard) failed");
        }

        measurement.ran = true;
        return measurement;
    }

    PrimitivesGeneratedNoXfbVerdict EvaluatePrimitivesGeneratedNoXfbVerdict(
        const PrimitivesGeneratedNoXfbMeasurement& measurement) {
        if (!measurement.ran || !measurement.trianglesPlain.drawn || !measurement.trianglesDiscard.drawn) {
            return PrimitivesGeneratedNoXfbVerdict::Inconclusive;
        }
        const PrimitivesGeneratedNoXfbShapeMeasurement* shapes[3] = {&measurement.trianglesPlain,
                                                                     &measurement.trianglesDiscard,
                                                                     &measurement.patchesDiscard};
        Bool anyStreamSilent = false;
        Bool allStreamExact = true;
        Bool allPrimitivesGeneratedExtExact = true;
        Bool allStatisticsExact = true;
        // Whether the statistics substitute DOMINATES the stream query shape by shape:
        // every shape the statistics do not answer exactly must be one the stream query
        // answered 0 for anyway. Without this, a plain-shape-only substitute could be
        // armed on a device whose stream query was RIGHT on a shape the statistics get
        // wrong - and the renderer reroutes every XFB-inactive draw, so that shape would
        // be downgraded from correct to wrong. "Never worse per draw" is what makes
        // arming on an uncharacterised driver defensible; it has to be measured, not
        // assumed.
        Bool statisticsDominateStream = true;
        for (const auto* shape : shapes) {
            if (!shape->drawn) {
                continue;
            }
            if (shape->streamGenerated == 0) {
                anyStreamSilent = true;
            }
            if (shape->streamGenerated != shape->expectedPrimitives) {
                allStreamExact = false;
                // A nonzero wrong answer is neither the defect nor health: refuse
                // a verdict rather than repair a driver the probe does not
                // understand.
                if (shape->streamGenerated != 0) {
                    return PrimitivesGeneratedNoXfbVerdict::Inconclusive;
                }
            }
            if (!shape->primitivesGeneratedExtMeasured ||
                shape->primitivesGeneratedExt != shape->expectedPrimitives) {
                allPrimitivesGeneratedExtExact = false;
            }
            if (!shape->statisticsMeasured ||
                shape->statisticsClippingInput != shape->expectedPrimitives) {
                allStatisticsExact = false;
                // Only a shape the stream query was silent on may be left behind by
                // the substitute; a shape it answered exactly must not be traded away.
                if (shape->streamGenerated == shape->expectedPrimitives) {
                    statisticsDominateStream = false;
                }
            }
        }
        if (allStreamExact) {
            return PrimitivesGeneratedNoXfbVerdict::StreamCounts;
        }
        // At this point at least one drawn shape answered exactly 0.
        MOBILEGL_ASSERT(anyStreamSilent, "verdict fell through with no silent shape");
        if (allPrimitivesGeneratedExtExact) {
            return PrimitivesGeneratedNoXfbVerdict::PrimitivesGeneratedExtSubstitute;
        }
        if (allStatisticsExact) {
            return PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitute;
        }
        const auto& plain = measurement.trianglesPlain;
        const Bool plainStatisticsExact =
            plain.statisticsMeasured && plain.statisticsClippingInput == plain.expectedPrimitives;
        // Both halves are required: the substitute must repair the plain shape, AND it
        // must not cost any shape an answer the stream query already had right.
        return (plainStatisticsExact && statisticsDominateStream)
                   ? PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitutePlainOnly
                   : PrimitivesGeneratedNoXfbVerdict::Unfixable;
    }

    PrimGenRerouteKind ChoosePrimitivesGeneratedReroute(MG_Config::QuirkOverride overrideSetting,
                                                        PrimitivesGeneratedNoXfbVerdict verdict,
                                                        Bool primitivesGeneratedQueryUsable,
                                                        Bool pipelineStatisticsEnabled) {
        switch (overrideSetting) {
        case MG_Config::QuirkOverride::ForceOff:
            return PrimGenRerouteKind::None;
        case MG_Config::QuirkOverride::ForceOn:
            // ForceOn bypasses the device verdict, never the structural checks:
            // without a hostable pool there is nothing to route through. The
            // dedicated query wins where both exist - its semantics are the GL
            // target's by definition.
            if (primitivesGeneratedQueryUsable) {
                return PrimGenRerouteKind::PrimitivesGeneratedExt;
            }
            return pipelineStatisticsEnabled ? PrimGenRerouteKind::ClippingStatistics
                                             : PrimGenRerouteKind::None;
        case MG_Config::QuirkOverride::Auto:
            break;
        }
        switch (verdict) {
        case PrimitivesGeneratedNoXfbVerdict::PrimitivesGeneratedExtSubstitute:
            return primitivesGeneratedQueryUsable ? PrimGenRerouteKind::PrimitivesGeneratedExt
                                                  : PrimGenRerouteKind::None;
        case PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitute:
        case PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitutePlainOnly:
            return pipelineStatisticsEnabled ? PrimGenRerouteKind::ClippingStatistics
                                             : PrimGenRerouteKind::None;
        case PrimitivesGeneratedNoXfbVerdict::Inconclusive:
        case PrimitivesGeneratedNoXfbVerdict::StreamCounts:
        case PrimitivesGeneratedNoXfbVerdict::Unfixable:
            break;
        }
        return PrimGenRerouteKind::None;
    }
} // namespace MobileGL::MG_Util::SelfTest
