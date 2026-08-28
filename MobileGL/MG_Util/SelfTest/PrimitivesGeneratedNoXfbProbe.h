// MobileGL - MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Config.h>
#include <Includes.h>

namespace MobileGL::MG_Util::SelfTest {
    // ============ PRIMITIVES GENERATED WITHOUT TRANSFORM FEEDBACK ============
    //
    // GL_PRIMITIVES_GENERATED counts what the last vertex processing stage emits
    // whether or not a transform feedback capture is active (GL 4.6 core 13.4), and
    // the DirectVulkan backend serves it from the second result
    // (primitivesNeeded) of a VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT pool
    // slot wrapped around each draw. VK_EXT_transform_feedback defines that value
    // as the primitives the vertex stream produced, capture or no capture - but a
    // Mali driver (G1-Ultra, observed against the gl44/gl45/gl46 CTS) answers 0
    // for every draw made while no vkCmdBeginTransformFeedbackEXT span is open,
    // while answering exactly right as soon as one is. The tessellation suites
    // measure the tessellator by exactly that shape (rasterizer discard on,
    // transform feedback INACTIVE, a PATCHES draw inside a GENERATED query;
    // esextcTessellationShaderUtils.cpp, captureTessellationData), size their
    // capture buffers from the answer, and die on the zero-byte buffer the 0
    // produces - about 29 tessellation tests per tree plus all 13
    // tessellation_shader.vertex bodies.
    //
    // THE PROBE draws three shapes through pipelines with no Xfb execution mode
    // and no transform feedback begun, each inside its own stream-query slot:
    //   - one triangle, plainly (no rasterizer discard);
    //   - one triangle with rasterizer discard baked into the pipeline;
    //   - one PATCHES draw with discard, through a passthrough tessellation
    //     pipeline whose all-1 levels emit exactly one triangle (when the device
    //     has tessellationShader) - the CTS shape verbatim.
    // Alongside each stream slot it measures the TWO candidate substitutes, each
    // around ITS OWN identical replay of the shape's draw - never co-active with
    // the subject, because a co-active control contaminates it: on lavapipe a
    // dedicated primitives-generated query active around the same draw switches
    // the driver's primitive collection on and the stream query then counts a
    // draw it answers 0 for when alone, which is how the renderer actually runs
    // it. The substitutes:
    //   - a VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT slot, where the device has
    //     VK_EXT_primitives_generated_query with BOTH primitivesGeneratedQuery and
    //     primitivesGeneratedQueryWithRasterizerDiscard (without the discard
    //     feature the spec forbids the query around a discarding draw at all -
    //     VUID-vkCmdDraw-...-06708 - and GL applications toggle discard freely, so
    //     a base-feature-only device cannot use this tier). The extension exists
    //     precisely because GL needs PRIMITIVES_GENERATED without a capture, so
    //     its semantics are exact by definition - what remains to prove is that
    //     the DRIVER's implementation is not silent in the same way its stream
    //     query is;
    //   - a VK_QUERY_TYPE_PIPELINE_STATISTICS slot counting CLIPPING_INVOCATIONS
    //     (when the device has pipelineStatisticsQuery): one invocation of the
    //     primitive clipping stage per primitive reaching it - GL's
    //     CLIPPING_INPUT_PRIMITIVES - which sits AFTER every vertex processing
    //     stage (post-tess, post-GS) and, per spec, BEFORE rasterizer discard, so
    //     for an XFB-inactive draw it is definitionally the number
    //     PRIMITIVES_GENERATED must answer. (A geometry stage's non-zero vertex
    //     streams never reach clipping, but non-indexed GL_PRIMITIVES_GENERATED
    //     counts stream 0 alone, so the sets still agree. The stage's OUTPUT
    //     count - CLIPPING_PRIMITIVES - would not: clipping drops and splits.)
    //
    // THE CONTROL DISCIPLINE (DriverBugProbes.h): the substitute slots are the
    // probe's controls, and the DISCARD dimension is measured separately because
    // it is a real fault line, not paranoia: Mesa llvmpipe short-circuits its
    // clipping statistics under rasterizer discard (reading 0 there while counting
    // the identical undiscarded draw exactly) while its dedicated
    // primitives-generated query counts both - measured 2026-08, and the reason
    // the verdict ranks the dedicated query first. A substitute qualifies only by
    // answering the exact expected count on every shape it is required to cover;
    // a device where no substitute qualifies even for the plain shape gets none
    // (the honest verdict is the current behaviour); anything that fits neither
    // the defect nor health is INCONCLUSIVE and must never arm anything. The
    // expected counts are exact (1 triangle per shape), not merely nonzero, so a
    // driver that half-counts cannot arm a half-right repair.

    // What one drawn shape of the probe measured.
    struct PrimitivesGeneratedNoXfbShapeMeasurement {
        // The shape's draw was recorded and its query slots were read back.
        Bool drawn = false;
        // Primitives the draw is defined to emit (1 for every shape).
        Uint64 expectedPrimitives = 0;
        // The stream-query slot's primitivesNeeded answer - what the renderer's
        // GL_PRIMITIVES_GENERATED path would have returned.
        Uint64 streamGenerated = 0;
        // Whether the dedicated primitives-generated slot ran (it needs the
        // extension with both feature bits, see above).
        Bool primitivesGeneratedExtMeasured = false;
        // Its answer for the same draw.
        Uint64 primitivesGeneratedExt = 0;
        // Whether the statistics slot ran (it needs pipelineStatisticsQuery).
        Bool statisticsMeasured = false;
        // The clipping-stage invocation count for the same draw.
        Uint64 statisticsClippingInput = 0;
    };

    struct PrimitivesGeneratedNoXfbMeasurement {
        // The probe submitted and read back at least the two triangle shapes.
        // False when any setup step failed; failureReason then names the step.
        Bool ran = false;
        // The probe's bounded fence wait expired with the submission possibly
        // still executing. The probe then deliberately LEAKED every child object
        // it created (no vkDeviceWaitIdle, no destroys - a hung GPU must not hang
        // the caller), so a caller that owns the device MUST NOT destroy or
        // idle-wait it either: vkDestroyDevice with live children and in-flight
        // work is the exact hang the bound exists to prevent. The POST leaks its
        // throwaway device on this flag, mirroring its sibling probes.
        Bool fenceWaitTimedOut = false;
        String failureReason;
        PrimitivesGeneratedNoXfbShapeMeasurement trianglesPlain;
        PrimitivesGeneratedNoXfbShapeMeasurement trianglesDiscard;
        // drawn = false when the device has no tessellationShader.
        PrimitivesGeneratedNoXfbShapeMeasurement patchesDiscard;
    };

    // The device-level entry points the probe records with. Supplied by the caller
    // because the two callers resolve them differently: the renderer passes its
    // statically linked symbols (and its vkGetDeviceProcAddr-resolved EXT
    // pointers), the driver POST passes vkGetInstanceProcAddr trampolines.
    struct PrimitivesGeneratedNoXfbProbeFns {
        PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
        PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
        PFN_vkCreateQueryPool vkCreateQueryPool = nullptr;
        PFN_vkDestroyQueryPool vkDestroyQueryPool = nullptr;
        PFN_vkCmdResetQueryPool vkCmdResetQueryPool = nullptr;
        PFN_vkCmdBeginQuery vkCmdBeginQuery = nullptr;
        PFN_vkCmdEndQuery vkCmdEndQuery = nullptr;
        PFN_vkCmdBeginQueryIndexedEXT vkCmdBeginQueryIndexedEXT = nullptr;
        PFN_vkCmdEndQueryIndexedEXT vkCmdEndQueryIndexedEXT = nullptr;
        PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
        PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
        PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
        PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
        PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
        PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
        PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
        PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
        PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
        PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
        PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
        PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
        PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
        PFN_vkCmdDraw vkCmdDraw = nullptr;
        PFN_vkCreateFence vkCreateFence = nullptr;
        PFN_vkDestroyFence vkDestroyFence = nullptr;
        PFN_vkQueueSubmit vkQueueSubmit = nullptr;
        PFN_vkWaitForFences vkWaitForFences = nullptr;
        PFN_vkGetQueryPoolResults vkGetQueryPoolResults = nullptr;
        PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    };

    struct PrimitivesGeneratedNoXfbProbeContext {
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        Uint32 queueFamilyIndex = 0;
        // The device was created with VK_EXT_transform_feedback, its
        // transformFeedback feature, and advertises transformFeedbackQueries.
        // Without this the probe has no subject and reports "did not run".
        Bool transformFeedbackQueriesUsable = false;
        // The device was created with VK_EXT_primitives_generated_query and BOTH
        // its primitivesGeneratedQuery and ...WithRasterizerDiscard features;
        // gates the dedicated-query control slots.
        Bool primitivesGeneratedQueryUsable = false;
        // The device was created with the pipelineStatisticsQuery feature; gates
        // the statistics control slots. A probe with no control at all can still
        // DETECT, but never qualifies a substitute.
        Bool pipelineStatisticsEnabled = false;
        // The device was created with the tessellationShader feature; gates the
        // PATCHES shape.
        Bool tessellationEnabled = false;
        PrimitivesGeneratedNoXfbProbeFns fns;
    };

    // Records, submits and reads back the probe. Synchronous: waits on its own
    // fence (bounded; on timeout it deliberately leaks its device objects rather
    // than idle-wait a possibly hung GPU, mirroring the POST timestamp probe) and
    // destroys everything it created. Never touches MG_State or renderer state -
    // the caller only lends it a device and an otherwise idle queue.
    PrimitivesGeneratedNoXfbMeasurement RunPrimitivesGeneratedNoXfbProbe(
        const PrimitivesGeneratedNoXfbProbeContext& context);

    // The verdict vocabulary. Pure function of the measurement, split from the
    // Vulkan plumbing so a unit test can pin every mapping with synthetic numbers.
    enum class PrimitivesGeneratedNoXfbVerdict : Uint8 {
        // The probe did not run, or answered something that is neither healthy nor
        // the defect (a half-count, a nonzero-but-wrong stream answer). Must never
        // arm the reroute and must never be reported as the bug.
        Inconclusive,
        // Every drawn shape's stream query answered its exact expected count: the
        // driver counts XFB-inactive draws and the existing path is correct.
        StreamCounts,
        // The defect is present (a drawn shape's stream query answered exactly 0)
        // and the dedicated primitives-generated query answered every drawn shape
        // exactly, the rasterizer-discard shapes included: the substitution is
        // proven whole through the query Vulkan defines for exactly this GL
        // target.
        PrimitivesGeneratedExtSubstitute,
        // The defect is present, the dedicated query did not qualify (absent, or
        // silent like the stream query), and the statistics control answered EVERY
        // drawn shape exactly - discard shapes included: the substitution is
        // proven whole through clipping statistics.
        StatisticsSubstitute,
        // The defect is present and the statistics control is exact on the PLAIN
        // shape but not on every drawn shape (llvmpipe's discard short-circuit
        // does this to its statistics - its dedicated query is what rescues it to
        // the verdict above). This verdict additionally GUARANTEES domination:
        // every shape the statistics missed measured exactly 0 through the stream
        // query too, so rerouting is never worse per draw - it repairs every
        // shape the statistics answer exactly and leaves the rest at the 0 they
        // already read. A measurement where the stream was EXACT on a shape the
        // statistics missed does not qualify (rerouting would downgrade that
        // shape) and falls to Unfixable instead. The shapes the substitute
        // misses - the CTS's discarded shapes among them wherever they are the
        // missed ones - stay broken, and the report must say which.
        StatisticsSubstitutePlainOnly,
        // The defect is present and no substitute qualifies: none is exact
        // everywhere, and the plain-only fallback either misses the plain shape
        // or fails the domination rule above. The honest verdict is the current
        // behaviour.
        Unfixable,
    };

    PrimitivesGeneratedNoXfbVerdict EvaluatePrimitivesGeneratedNoXfbVerdict(
        const PrimitivesGeneratedNoXfbMeasurement& measurement);

    // Which query pool the renderer routes GL_PRIMITIVES_GENERATED accumulation
    // for XFB-inactive draws through.
    enum class PrimGenRerouteKind : Uint8 {
        None,
        // VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT (needs the extension with both
        // feature bits - see the context flag).
        PrimitivesGeneratedExt,
        // VK_QUERY_TYPE_PIPELINE_STATISTICS over clipping invocations (needs
        // pipelineStatisticsQuery).
        ClippingStatistics,
    };

    // The arming decision. Pure, so the override mapping is unit-pinnable:
    //   - ForceOff never reroutes;
    //   - ForceOn bypasses the verdict but never the structural checks: it takes
    //     the dedicated query where the device can host it, the statistics pool
    //     where only that exists, and nothing where neither does;
    //   - Auto follows the verdict: the dedicated query on
    //     PrimitivesGeneratedExtSubstitute, the statistics pool on
    //     StatisticsSubstitute and StatisticsSubstitutePlainOnly (each already
    //     implies its feature-backed control), and nothing otherwise.
    PrimGenRerouteKind ChoosePrimitivesGeneratedReroute(MG_Config::QuirkOverride overrideSetting,
                                                        PrimitivesGeneratedNoXfbVerdict verdict,
                                                        Bool primitivesGeneratedQueryUsable,
                                                        Bool pipelineStatisticsEnabled);
} // namespace MobileGL::MG_Util::SelfTest
