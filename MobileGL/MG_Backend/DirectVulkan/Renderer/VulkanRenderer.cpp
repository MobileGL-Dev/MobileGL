// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderPassGuard.h"
#include "VulkanRenderer.h"
#include "SubmitFencePrefix.h"
#include "MG_Util/X11/DisplayGuard.h"
#include "OffscreenSurfaceRoute.h"

#include "MG_Backend/DirectVulkan/SubgroupSupportPolicy.h"
#include "MG_Backend/DirectGLES/Utils.h"
#include "VertexInputStateFactory.h"
#include "VertexInputStateBuilder.h"

#include "MG_State/GLState/Core.h"
#include <MG_Backend/MGPipe/PipeInputs.h>
// P5c ev: the GPU-write announcement routes through the reverse channel (R2).
#include <MG_Impl/Pipe/ResourceTracker.h>
#include "MG_State/GLState/ProgramState/ProgramObject.h"
#include "MG_State/GLState/ProgramState/ShaderObject.h"
#include "MG_State/GLState/SamplerState/SamplerObject.h"
#include "MG_State/GLState/TextureState/TextureObject.h"
#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_Impl/GLImpl/Texture/GL_Texture.h"
// The generation window GenerateMipmap defines its chain in: one definition, shared with the
// frontend arm and with the wire carrier that publishes it (MGPMipPlan::LevelCount).
#include "MG_Impl/GLImpl/Texture/MipmapGenerationPlan.h"
#include "MG_Util/Converters/GLToMG/TextureEnumConverter.h"
// Only reached from an MGLOG_W, which the shipping INFO log level compiles out - so the
// missing include never broke a default build and did break every WARN/DEBUG-level one.
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToVk/RenderStateEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Math/HalfFloat.h"
#include "MG_Util/Metrics/PipeStats.h"
#include "MG_Util/Metrics/TextureMetrics.h"
#include "MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.h"
#include "MG_Util/Texture/PixelStoreProcessor.h"
#include <Config.h>
// P7 wave 0: the seam WireFramebuffer.inc's MagmaWireFatal and WireDraw.inc's
// WireBufferLegacyFatal die through. Declared by MG_Pipe on purpose - see PipeSessionFail.h.
#include <MG_Pipe/PipeSessionFail.h>
// P5c (T5 / tx): the server's staged-texture shadow GenerateMipmap defines its chain on.
#include <MG_Backend/Record/StagedTextureStore.h>
// P7 wave 2 package B3: rule I's tally for WireDraw.inc's silent draw drops.
#include "WireDeclineTally.h"
#include "WireColorBlitFilter.h"
#include "WireDepthResolveArm.h"
#include "WireDepthResolveProbe.h"
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Remote/Server/ServerLoop.h>
// Shared images: the present side resolves the image it imports (WireSharedImage.inc).
#include <MG_Remote/Server/SharedImageRegistry.h>
#endif
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <vulkan/utility/vk_format_utils.h>
#include <vulkan/vulkan_core.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#if MOBILEGL_BUILD_DISAGGREGATED && !defined(_WIN32)
#include <unistd.h> // close(): shared-image sync_file descriptors (WireSharedImage.inc)
#endif

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

namespace MobileGL::MG_Backend::DirectVulkan {
#if defined(__APPLE__)
    namespace {
        constexpr unsigned long kNSWindowStyleMaskBorderless = 0;
        constexpr unsigned long kNSBackingStoreBuffered = 2;

        template <typename Fn>
        Fn ObjcMsgSend() {
            return reinterpret_cast<Fn>(objc_msgSend);
        }

        id SendId(id receiver, const char* selector) {
            return ObjcMsgSend<id (*)(id, SEL)>()(receiver, sel_registerName(selector));
        }

        void SendVoid(id receiver, const char* selector) {
            ObjcMsgSend<void (*)(id, SEL)>()(receiver, sel_registerName(selector));
        }

        void SendVoidBool(id receiver, const char* selector, bool value) {
            ObjcMsgSend<void (*)(id, SEL, bool)>()(receiver, sel_registerName(selector), value);
        }

        void SendVoidId(id receiver, const char* selector, id value) {
            ObjcMsgSend<void (*)(id, SEL, id)>()(receiver, sel_registerName(selector), value);
        }

        void SendVoidCGRect(id receiver, const char* selector, CGRect value) {
            ObjcMsgSend<void (*)(id, SEL, CGRect)>()(receiver, sel_registerName(selector), value);
        }

        void SendVoidCGSize(id receiver, const char* selector, CGSize value) {
            ObjcMsgSend<void (*)(id, SEL, CGSize)>()(receiver, sel_registerName(selector), value);
        }

        id Retain(id object) {
            return object ? SendId(object, "retain") : nil;
        }

        void Release(id object) {
            if (object) {
                SendVoid(object, "release");
            }
        }

        void* CreateInternalMetalLayer(Uint32 width, Uint32 height, void** outWindow) {
            const auto surfaceWidth = static_cast<CGFloat>(std::max<Uint32>(width, 1));
            const auto surfaceHeight = static_cast<CGFloat>(std::max<Uint32>(height, 1));
            id windowClass = reinterpret_cast<id>(objc_getClass("NSWindow"));
            id metalLayerClass = reinterpret_cast<id>(objc_getClass("CAMetalLayer"));
            MOBILEGL_ASSERT(windowClass && metalLayerClass,
                            "Failed to resolve NSWindow/CAMetalLayer for DirectVulkan pbuffer");

            CGRect frame = {{0.0, 0.0}, {surfaceWidth, surfaceHeight}};
            id window = SendId(windowClass, "alloc");
            window = ObjcMsgSend<id (*)(id, SEL, CGRect, unsigned long, unsigned long, bool)>()(
                window, sel_registerName("initWithContentRect:styleMask:backing:defer:"),
                frame, kNSWindowStyleMaskBorderless, kNSBackingStoreBuffered, true);
            MOBILEGL_ASSERT(window, "Failed to create hidden NSWindow for DirectVulkan pbuffer");

            id contentView = SendId(window, "contentView");
            MOBILEGL_ASSERT(contentView, "Failed to query hidden NSWindow contentView");
            SendVoidBool(contentView, "setWantsLayer:", true);

            id metalLayer = SendId(metalLayerClass, "layer");
            MOBILEGL_ASSERT(metalLayer, "Failed to create hidden CAMetalLayer for DirectVulkan pbuffer");
            Retain(metalLayer);
            SendVoidCGRect(metalLayer, "setFrame:", frame);
            SendVoidCGSize(metalLayer, "setDrawableSize:", frame.size);
            SendVoidId(contentView, "setLayer:", metalLayer);

            *outWindow = window;
            return metalLayer;
        }
    } // namespace
#endif

    static Bool IsPowerVRDevice(const VkPhysicalDeviceProperties& properties) {
        return std::strstr(properties.deviceName, "PowerVR") != nullptr;
    }

    static VkPipelineColorBlendAttachmentState MakeColorBlendAttachmentState(
        Bool blendEnable,
        VkBlendFactor srcColorBlendFactor,
        VkBlendFactor dstColorBlendFactor,
        VkBlendOp colorBlendOp,
        VkBlendFactor srcAlphaBlendFactor,
        VkBlendFactor dstAlphaBlendFactor,
        VkBlendOp alphaBlendOp,
        VkColorComponentFlags colorWriteMask) {
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable = blendEnable ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor = srcColorBlendFactor;
        attachment.dstColorBlendFactor = dstColorBlendFactor;
        attachment.colorBlendOp = colorBlendOp;
        attachment.srcAlphaBlendFactor = srcAlphaBlendFactor;
        attachment.dstAlphaBlendFactor = dstAlphaBlendFactor;
        attachment.alphaBlendOp = alphaBlendOp;
        attachment.colorWriteMask = colorWriteMask;
        return attachment;
    }

    static Bool IsDualSourceBlendFactor(BlendFactor v) {
        switch (v) {
        case BlendFactor::Src1Color:
        case BlendFactor::OneMinusSrc1Color:
        case BlendFactor::Src1Alpha:
        case BlendFactor::OneMinusSrc1Alpha:
            return true;
        default:
            return false;
        }
    }

    static Bool IsQuarterTurnPreTransform(VkSurfaceTransformFlagBitsKHR preTransform) {
        return preTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
               preTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    }

    static IntVec2 ResolveDefaultFramebufferLogicalExtent(VkSurfaceTransformFlagBitsKHR preTransform,
                                                          const IntVec2& rawExtent) {
        if (IsQuarterTurnPreTransform(preTransform)) {
            return {rawExtent.y(), rawExtent.x()};
        }
        return rawExtent;
    }

    // A GL bottom-left-origin rectangle on a quarter-turned default framebuffer, expressed in the
    // stored (rotated) image: the rectangle transposes. This is the mapping the vertex fixup implies
    // (InsertPositionFixup: Y flip, then (x, y) -> (-y, x) for 90 and (y, -x) for 270) and the one
    // MapDefaultFramebufferReadbackRect copies with, so a viewport, a scissor and a readback of the
    // same GL rectangle all land on the same pixels. Unclamped: a viewport may extend past the image.
    static void MapQuarterTurnRect(Int x, Int y, Int width, Int height, const IntVec2& imageExtent,
                                   VkSurfaceTransformFlagBitsKHR preTransform, Int* outX, Int* outY, Int* outWidth,
                                   Int* outHeight) {
        if (preTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
            *outX = y;
            *outY = x;
        } else {
            *outX = imageExtent.x() - y - height;
            *outY = imageExtent.y() - x - width;
        }
        *outWidth = height;
        *outHeight = width;
    }

    // ---------------------------------------------------------------------------------------
    // Default-framebuffer rectangles.
    //
    // GL's window origin is the BOTTOM-left. The default framebuffer's Vulkan image is stored in
    // DISPLAY (top-left) orientation, and the difference is reconciled for VERTICES by negating
    // gl_Position.y - but only for default-FBO draws (GetShaderTransformFlags ->
    // CompileOptionBit::PositionYFlip, applied in ProgramFactory::InsertPositionFixup).
    //
    // Rectangles were never converted. The viewport, the scissor and the ReadPixels copy offset
    // all used the GL bottom-origin Y verbatim as a Vulkan top-origin Y, which is correct only
    // when y == H - y - h (full height, or vertically centred) - and full height is the only case
    // any test ever exercised. In the conformance suite the errors CANCEL in placement (the draw
    // lands in Vulkan rows [y, y+h) and the readback copies the same rows back) and compose into
    // an exact vertical flip: 1,759 of Magma's 1,793 non-passing cases, 861 vertical flips and
    // nothing else across all of gl33.
    //
    // The mapping below is derived from - and at full extent exactly reproduces - the pixel
    // mapping VulkanRenderer::RemapDefaultFramebufferReadback uses:
    //     identity : image(x, H-1-y)      -> flip Y
    //     180      : image(W-1-x, y)      -> mirror X (the rotation already flips the rows)
    // Quarter turns swap the axes and are handled by MapDefaultFramebufferReadbackRect rather than
    // this same-axis helper.
    struct DefaultFramebufferRectMapping {
        Bool flipY = false;
        Bool mirrorX = false;
    };

    static DefaultFramebufferRectMapping GetDefaultFramebufferRectMapping(
            VkSurfaceTransformFlagBitsKHR preTransform) {
        if (preTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) return {false, true};
        if (IsQuarterTurnPreTransform(preTransform)) return {false, false};
        return {true, false};
    }

    // [origin, origin+size) counted from one end is [extent-origin-size, extent-origin) counted
    // from the other. A full-extent rect is a fixed point, which is why this can be introduced
    // without moving anything that works today.
    static Int MapDefaultFramebufferRectAxis(Int origin, Int size, Int extent, Bool invert) {
        return invert ? extent - origin - size : origin;
    }

    // Redundant dynamic-state elimination for the per-draw hot path: within one
    // command-buffer recording, a vkCmdSet* whose values already match what the
    // command buffer holds is skipped. Valid because every PipelineFactory
    // pipeline declares the same eight dynamic states, so the values persist
    // across those pipeline binds; the shadow resets whenever a recording
    // (re)begins, and whenever an auxiliary pipeline with a narrower dynamic
    // set (blit, depth-mipmap) binds - their static state makes the
    // corresponding dynamic values undefined per the spec.
    struct DynamicStateShadow {
        // Last graphics pipeline bound on the frame command buffer. Pipeline
        // binds are command-buffer state (they survive render-pass boundaries),
        // so the same reset points that invalidate dynamic state - recording
        // (re)begin and the aux blit pipelines' raw binds - are exactly the
        // points where this becomes unknown.
        Bool graphicsPipelineValid = false;
        VkPipeline graphicsPipeline = VK_NULL_HANDLE;
        // Index/vertex buffer binds are command-buffer state too. Terrain
        // sections and GUI quads share one sequential index buffer, and GUI
        // batches often reuse a vertex arena buffer, so skipping identical
        // rebinds removes a large share of per-draw driver calls.
        Bool indexBindValid = false;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceSize indexOffset = 0;
        VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM;
        static constexpr Uint32 kMaxShadowedVertexBindings = 8;
        Bool vertexBindValid = false;
        Uint32 vertexBindingCount = 0;
        VkBuffer vertexBuffers[kMaxShadowedVertexBindings] = {};
        VkDeviceSize vertexOffsets[kMaxShadowedVertexBindings] = {};
        Bool viewportValid = false;
        VkViewport viewport{};
        Bool scissorValid = false;
        VkRect2D scissor{};
        Bool blendConstantsValid = false;
        Float blendConstants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Bool depthBiasValid = false;
        Float depthBiasConstantFactor = 0.0f;
        Float depthBiasSlopeFactor = 0.0f;
        Bool lineWidthValid = false;
        Float lineWidth = 0.0f;
        Bool stencilValid = false;
        Uint32 stencilFrontCompareMask = 0;
        Uint32 stencilBackCompareMask = 0;
        Uint32 stencilFrontWriteMask = 0;
        Uint32 stencilBackWriteMask = 0;
        Uint32 stencilFrontReference = 0;
        Uint32 stencilBackReference = 0;
        // Gate over the whole per-draw dynamic-state tail (viewport, scissor, blend
        // constants, depth bias, line width, stencil) - see ApplyDynamicDrawStateTail.
        // Every GL input of that tail lives in RenderState's value-shadowed parameters:
        // each setter early-outs on an equal value and bumps the parameters version
        // otherwise, and capability toggles (scissor test) bump it too. So an unchanged
        // version + unchanged pass geometry means re-running the tail could only
        // re-derive the exact values already applied on this command buffer. The
        // remaining input, the swapchain pre-transform, cannot change mid-recording
        // (a swapchain recreate retires the command buffer, and recording begin resets
        // this whole shadow); the value key below pins it anyway.
        Bool dynamicTailValid = false;
        Uint dynamicTailParamsVersion = 0;
        Int dynamicTailExtentX = 0;
        Int dynamicTailExtentY = 0;
        Bool dynamicTailIsDefaultFbo = false;
        // VALUE key over the tail's inputs, as a second-level gate behind the version.
        // The parameters version is ONE counter for all of RenderState, so anything that
        // is not tail input - a GL_BLEND toggle, a glBlendFuncSeparate, a glColorMask -
        // moves it and forced a full tail re-run. Blaze3D toggles blend around every
        // batch, so that was a per-draw re-derivation of six dynamic states that could
        // not have changed. Equal key => the six Apply* below would each re-derive the
        // value their shadow already holds and emit nothing, so the tail is skippable.
        //
        // Complete input inventory of ApplyDynamicDrawStateTail, one line per reader
        // (each accessor it replaces is a verified plain field read of the same
        // RenderStateParameters field - RenderState.cpp):
        //   ApplyGLViewportState    : Viewports[0], DepthRanges[0], + extent/isDefaultFbo/preTransform
        //   ApplyBlendConstants     : BlendColor
        //   ApplyPolygonOffsetState : PolygonOffsetUnits, PolygonOffsetFactor
        //   ApplyLineWidthState     : LineWidth (see the caveat below)
        //   ApplyStencilState       : StencilStates[0..1].{ValueMask, WriteMask, Ref}
        //   scissor rect            : ScissorTestEnabledMask bit 0, ScissorBoxes[0],
        //                             + extent/isDefaultFbo/preTransform
        // Caveat, unchanged from the version-only gate: ApplyLineWidthState also clamps
        // to the ACTIVE BACKEND OBJECT's aliased line-width range. Those are device
        // limits queried once at backend init and constant for the renderer's lifetime,
        // so they are not part of the key (the version gate never covered them either).
        struct DynamicTailKey {
            Float viewport[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            Float depthRange[2] = {0.0f, 0.0f};
            Float blendColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            Float polygonOffsetFactor = 0.0f;
            Float polygonOffsetUnits = 0.0f;
            Float lineWidth = 0.0f;
            Uint32 stencilValueMask[2] = {0, 0};
            Uint32 stencilWriteMask[2] = {0, 0};
            Int stencilRef[2] = {0, 0};
            Int scissorBox[4] = {0, 0, 0, 0};
            Int extentX = 0;
            Int extentY = 0;
            Uint32 preTransform = 0;
            Bool scissorEnabled = false;
            Bool isDefaultFbo = false;

            Bool operator==(const DynamicTailKey& other) const {
                // NaN in any float input makes this false, which only costs a redundant
                // tail run - never a skipped one.
                for (Uint32 i = 0; i < 4; ++i) {
                    if (viewport[i] != other.viewport[i] || blendColor[i] != other.blendColor[i] ||
                        scissorBox[i] != other.scissorBox[i]) {
                        return false;
                    }
                }
                for (Uint32 i = 0; i < 2; ++i) {
                    if (depthRange[i] != other.depthRange[i] ||
                        stencilValueMask[i] != other.stencilValueMask[i] ||
                        stencilWriteMask[i] != other.stencilWriteMask[i] ||
                        stencilRef[i] != other.stencilRef[i]) {
                        return false;
                    }
                }
                return polygonOffsetFactor == other.polygonOffsetFactor &&
                       polygonOffsetUnits == other.polygonOffsetUnits && lineWidth == other.lineWidth &&
                       extentX == other.extentX && extentY == other.extentY &&
                       preTransform == other.preTransform && scissorEnabled == other.scissorEnabled &&
                       isDefaultFbo == other.isDefaultFbo;
            }
        };
        DynamicTailKey dynamicTailKey{};
    };
    // Per Magma session (MagmaSession.h): it shadows the session's own command buffer.
    static SessionLocal<DynamicStateShadow> g_dynamicStateShadow;

    // ---- D12.3: DynamicTailKey's inputs against the P2 chunk table ----
    //
    // DynamicTailKey's inventory (declared above, one line per reader) is an exact,
    // hand-maintained enumeration of what the six Apply* in the tail read. The P2 chunk table
    // (MG_Pipe/MGPipeRenderStateSpans.h) is an independent, offsetof-derived statement of
    // which bytes of RenderStateParameters are dynamic state. The two were written for
    // different reasons, so making them check each other is free evidence: if a later chunk
    // edit demotes or promotes one of these members, the mismatch is a BUILD BREAK here rather
    // than a tail that silently stops being re-run when its input moves.
    //
    // The brief (P2 D12.3) expects every input to be dynamic; the tree says otherwise for
    // exactly one, and the tree is right - see the ScissorTestEnabledMask note below.
    //
    // These assertions ARE D19's DynamicChunksCoverMagmasDynamicTailKey, in the only file this
    // package owns. D19 names it as a case in MG_Test/Pipe/RenderStateSpansTest.cpp, which
    // belongs to package A (C.5). INTEGRATOR: make sure the outcome is not "neither" - if
    // package A did not land that case, this static_assert block is the whole gate, and if it
    // did, the two are redundant on purpose and both should stay.
    namespace {
        // Is [begin, begin + size) covered entirely by DYNAMIC chunks?
        constexpr Bool MagmaRenderStateRangeIsDynamic(SizeT begin, SizeT size) {
            const SizeT end = begin + size;
            for (SizeT i = 0; i < MG_Pipe::kMGPipeRenderStateChunkCount; ++i) {
                const SizeT chunkBegin = MG_Pipe::kMGPipeRenderStateChunkBoundaries[i];
                const SizeT chunkEnd = MG_Pipe::kMGPipeRenderStateChunkBoundaries[i + 1];
                if (end <= chunkBegin || begin >= chunkEnd) continue; // disjoint
                if (MG_Pipe::MGPipeRenderStateChunkIsPipeline(i)) return false;
            }
            return true;
        }
        using MagmaTailRsp = RenderStateParameters;

#define MAGMA_TAIL_INPUT_IS_DYNAMIC(Member)                                                                            \
    static_assert(MagmaRenderStateRangeIsDynamic(offsetof(MagmaTailRsp, Member),                                       \
                                                 sizeof(MagmaTailRsp::Member)),                                        \
                  "ApplyDynamicDrawStateTail reads " #Member                                                           \
                  ", which the P2 chunk table no longer calls dynamic state: a change to it would "                    \
                  "move the pipeline version, not the parameters version, and the tail would stop "                    \
                  "being re-run for it")

        // Viewports, DepthRanges and ScissorBoxes are asserted over the WHOLE array while the
        // tail reads only element 0. That is deliberately stricter than the reader needs: the
        // chunk table has no per-element granularity today, so an array that is dynamic at all
        // is dynamic entirely, and asserting the whole of it says so. If a later phase ever
        // splits a per-viewport chunk out, this is a build break by design - narrow the assert
        // to element 0 then, and say why in the same commit.
        MAGMA_TAIL_INPUT_IS_DYNAMIC(Viewports);        // ApplyGLViewportState: Viewports[0]
        MAGMA_TAIL_INPUT_IS_DYNAMIC(DepthRanges);      // ApplyGLViewportState: DepthRanges[0]
        MAGMA_TAIL_INPUT_IS_DYNAMIC(BlendColor);       // ApplyBlendConstants
        MAGMA_TAIL_INPUT_IS_DYNAMIC(PolygonOffsetFactor); // ApplyPolygonOffsetState
        MAGMA_TAIL_INPUT_IS_DYNAMIC(PolygonOffsetUnits);  // ApplyPolygonOffsetState
        MAGMA_TAIL_INPUT_IS_DYNAMIC(LineWidth);        // ApplyLineWidthState
        MAGMA_TAIL_INPUT_IS_DYNAMIC(ScissorBoxes);     // the scissor rect: ScissorBoxes[0]
#undef MAGMA_TAIL_INPUT_IS_DYNAMIC

        // ApplyStencilState reads three of the seven members of each face, and D6 splits
        // StencilFaceState at sub-member granularity for exactly this reason: Ref, ValueMask
        // and WriteMask are VK_DYNAMIC_STATE_STENCIL_{REFERENCE,COMPARE_MASK,WRITE_MASK}, while
        // Func and the three ops are baked into the pipeline. Asserted per member, per face,
        // because the split runs THROUGH the struct rather than around it.
        constexpr SizeT kMagmaStencilFace1 = offsetof(MagmaTailRsp, StencilStates) + sizeof(StencilFaceState);
#define MAGMA_TAIL_STENCIL_IS_DYNAMIC(Member)                                                                          \
    static_assert(MagmaRenderStateRangeIsDynamic(offsetof(MagmaTailRsp, StencilStates) +                               \
                                                     offsetof(StencilFaceState, Member),                               \
                                                 sizeof(StencilFaceState::Member)),                                    \
                  "ApplyStencilState reads the FRONT face's " #Member " as dynamic state");                            \
    static_assert(MagmaRenderStateRangeIsDynamic(kMagmaStencilFace1 + offsetof(StencilFaceState, Member),               \
                                                 sizeof(StencilFaceState::Member)),                                    \
                  "ApplyStencilState reads the BACK face's " #Member " as dynamic state")

        MAGMA_TAIL_STENCIL_IS_DYNAMIC(Ref);
        MAGMA_TAIL_STENCIL_IS_DYNAMIC(ValueMask);
        MAGMA_TAIL_STENCIL_IS_DYNAMIC(WriteMask);
#undef MAGMA_TAIL_STENCIL_IS_DYNAMIC

        // THE ONE INPUT THAT IS NOT DYNAMIC, and the brief's D12.3 says it should be.
        // The tree wins, and it is right: the split's only rule is "a byte is pipeline state
        // iff a public setter that calls BumpVersions() writes it", and ScissorTestEnabledMask
        // is written by SetCapability(ScissorTest), which does. It sits in pipeline chunk P6
        // with the other capability bools. The tail reads it only to decide between the
        // scissor box and a full-extent rect, and it is HARMLESS there for a reason worth
        // stating: a pipeline-half write moves the pipeline version, and the pipeline version
        // moves only together with the parameters version (BumpVersions bumps both), so the
        // tail's version gate is invalidated by it just the same. A DYNAMIC member promoted
        // into the pipeline half would break that direction, which is what the asserts above
        // are for; this one is pinned in the opposite direction so that DEMOTING it - which
        // would be a real G7 violation - is also a build break.
        static_assert(!MagmaRenderStateRangeIsDynamic(offsetof(MagmaTailRsp, ScissorTestEnabledMask),
                                                      sizeof(MagmaTailRsp::ScissorTestEnabledMask)),
                      "ScissorTestEnabledMask is written by SetCapability(ScissorTest), which calls "
                      "BumpVersions(), so the chunk table must keep it in the pipeline half");
    } // namespace

    static void ResetDynamicStateShadow() {
        *g_dynamicStateShadow = {};
    }

    static void ShadowedSetScissor(VkCommandBuffer commandBuffer, const VkRect2D& scissor) {
        auto& shadow = *g_dynamicStateShadow;
        if (shadow.scissorValid && shadow.scissor.offset.x == scissor.offset.x &&
            shadow.scissor.offset.y == scissor.offset.y &&
            shadow.scissor.extent.width == scissor.extent.width &&
            shadow.scissor.extent.height == scissor.extent.height) {
            return;
        }
        shadow.scissorValid = true;
        shadow.scissor = scissor;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    // One viewport of the ARB_viewport_array state, mapped into Vulkan's frame. Split out of
    // ApplyGLViewportState so the multi-viewport path derives index i through EXACTLY the same
    // arithmetic as index 0 - the default-framebuffer Y-flip and pre-transform rotation
    // especially, which is the classic way a multi-viewport port comes out upside down for every
    // index but the one that was tested.
    static VkViewport ComputeGLViewport(Uint32 index,
                                        const IntVec2& framebufferExtent,
                                        VkSurfaceTransformFlagBitsKHR preTransform,
                                        Bool isDefaultFramebuffer) {
        // Snapped to integers. The viewport is float STATE (glViewportIndexedf may set a
        // fractional origin, and GetFloati_v hands it back verbatim), but what rasterizes here is
        // the rounded rectangle - a deliberate, documented infidelity rather than a spec claim:
        // MobileGL passes the driver's VIEWPORT_SUBPIXEL_BITS through, so it does advertise
        // subpixel viewport precision it does not deliver. Nothing in KHR-GL43.viewport_array or
        // in Minecraft sets a fractional viewport (the conformance checks are all on the state
        // round trip), which is why the honest-but-lossy path was kept over widening every
        // default-framebuffer Y-flip/pre-transform helper to floats. See the KNOWN INFIDELITY
        // note in MG_IntegrationTest/Scenarios/AdvertisedLimitsScenario.cpp.
        const FloatVec4& stored = MG_Pipe::gPipeInputs.GetViewportIndexed(index);
        const IntVec4 viewportState(static_cast<Int>(std::lround(stored.x())),
                                    static_cast<Int>(std::lround(stored.y())),
                                    static_cast<Int>(std::lround(stored.z())),
                                    static_cast<Int>(std::lround(stored.w())));
        const FloatVec2& depthRange = MG_Pipe::gPipeInputs.GetDepthRangeIndexed(index);
        const IntVec2 logicalExtent = isDefaultFramebuffer
            ? ResolveDefaultFramebufferLogicalExtent(preTransform, framebufferExtent)
            : framebufferExtent;

        Int viewportX = viewportState.x();
        Int viewportY = viewportState.y();
        Int viewportWidth = viewportState.z() > 0 ? viewportState.z() : logicalExtent.x();
        Int viewportHeight = viewportState.w() > 0 ? viewportState.w() : logicalExtent.y();

        if (isDefaultFramebuffer && IsQuarterTurnPreTransform(preTransform)) {
            // Transposed, not scaled: scaling each axis by the other's extent only coincides with
            // the rotation for a viewport covering the whole surface.
            MapQuarterTurnRect(viewportState.x(), viewportState.y(), viewportWidth, viewportHeight, framebufferExtent,
                               preTransform, &viewportX, &viewportY, &viewportWidth, &viewportHeight);
        }

        // The GL viewport rect, expressed against the default framebuffer's stored orientation.
        // A full-height viewport is unchanged by this, which is why every existing scenario keeps
        // its exact behaviour.
        if (isDefaultFramebuffer) {
            const DefaultFramebufferRectMapping mapping = GetDefaultFramebufferRectMapping(preTransform);
            viewportX = MapDefaultFramebufferRectAxis(viewportX, viewportWidth, framebufferExtent.x(),
                                                      mapping.mirrorX);
            viewportY = MapDefaultFramebufferRectAxis(viewportY, viewportHeight, framebufferExtent.y(),
                                                      mapping.flipY);
        }

        VkViewport viewport{};
        viewport.x = static_cast<float>(viewportX);
        viewport.y = static_cast<float>(viewportY);
        viewport.width = static_cast<float>(viewportWidth);
        viewport.height = static_cast<float>(viewportHeight);
        viewport.minDepth = depthRange.x();
        viewport.maxDepth = depthRange.y();
        return viewport;
    }

    static void ApplyGLViewportState(VkCommandBuffer commandBuffer,
                                     const IntVec2& framebufferExtent,
                                     VkSurfaceTransformFlagBitsKHR preTransform,
                                     Bool isDefaultFramebuffer) {
        const VkViewport viewport = ComputeGLViewport(0, framebufferExtent, preTransform, isDefaultFramebuffer);
        auto& shadow = *g_dynamicStateShadow;
        if (shadow.viewportValid && shadow.viewport.x == viewport.x && shadow.viewport.y == viewport.y &&
            shadow.viewport.width == viewport.width && shadow.viewport.height == viewport.height &&
            shadow.viewport.minDepth == viewport.minDepth && shadow.viewport.maxDepth == viewport.maxDepth) {
            return;
        }
        shadow.viewportValid = true;
        shadow.viewport = viewport;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    }

    static void ApplyBlendConstants(VkCommandBuffer commandBuffer) {
        const FloatVec4& blendColor = MG_Pipe::gPipeInputs.GetBlendColor();
        const float blendConstants[4] = {
            blendColor.x(),
            blendColor.y(),
            blendColor.z(),
            blendColor.w(),
        };
        auto& shadow = *g_dynamicStateShadow;
        if (shadow.blendConstantsValid && shadow.blendConstants[0] == blendConstants[0] &&
            shadow.blendConstants[1] == blendConstants[1] && shadow.blendConstants[2] == blendConstants[2] &&
            shadow.blendConstants[3] == blendConstants[3]) {
            return;
        }
        shadow.blendConstantsValid = true;
        shadow.blendConstants[0] = blendConstants[0];
        shadow.blendConstants[1] = blendConstants[1];
        shadow.blendConstants[2] = blendConstants[2];
        shadow.blendConstants[3] = blendConstants[3];
        vkCmdSetBlendConstants(commandBuffer, blendConstants);
    }

    static Bool DrawModeUsesPolygonFill(GLenum mode) {
        switch (mode) {
        case GL_TRIANGLES:
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN:
            return true;
        default:
            return false;
        }
    }

    static void ApplyPolygonOffsetState(VkCommandBuffer commandBuffer) {
        const Float constantFactor = MG_Pipe::gPipeInputs.GetPolygonOffsetUnits();
        const Float slopeFactor = MG_Pipe::gPipeInputs.GetPolygonOffsetFactor();
        auto& shadow = *g_dynamicStateShadow;
        if (shadow.depthBiasValid && shadow.depthBiasConstantFactor == constantFactor &&
            shadow.depthBiasSlopeFactor == slopeFactor) {
            return;
        }
        shadow.depthBiasValid = true;
        shadow.depthBiasConstantFactor = constantFactor;
        shadow.depthBiasSlopeFactor = slopeFactor;
        vkCmdSetDepthBias(commandBuffer, constantFactor, 0.0f, slopeFactor);
    }

    static void ApplyLineWidthState(VkCommandBuffer commandBuffer) {
        Float lineWidth = MG_Pipe::gPipeInputs.GetLineWidth();
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c (hd, CONTRACT-P5C §3.7): with an active transport the dynamic parameters are the
        // SERVER's own backend's - the client caps mirror is client memory (rule E). Monolith
        // reads the mirror as it always did.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            if (MG_Backend::BackendObject* server = MG_Remote::Server::ServerLoopInstance().Backend()) {
                const auto& dynamicParameters = server->GetDynamicParameters();
                const Float minLineWidth = dynamicParameters.AliasedLineWidthRangeMin;
                const Float maxLineWidth = dynamicParameters.AliasedLineWidthRangeMax;
                if (lineWidth < minLineWidth) {
                    lineWidth = minLineWidth;
                } else if (lineWidth > maxLineWidth) {
                    lineWidth = maxLineWidth;
                }
            }
        } else
#endif
        if (MG_Backend::pActiveBackendObject != nullptr) {
            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            const Float minLineWidth = dynamicParameters.AliasedLineWidthRangeMin;
            const Float maxLineWidth = dynamicParameters.AliasedLineWidthRangeMax;
            if (lineWidth < minLineWidth) {
                lineWidth = minLineWidth;
            } else if (lineWidth > maxLineWidth) {
                lineWidth = maxLineWidth;
            }
        }
        auto& shadow = *g_dynamicStateShadow;
        if (shadow.lineWidthValid && shadow.lineWidth == lineWidth) {
            return;
        }
        shadow.lineWidthValid = true;
        shadow.lineWidth = lineWidth;
        vkCmdSetLineWidth(commandBuffer, lineWidth);
    }

    static VkRect2D MakeClampedScissorRect(const IntVec4& scissorBox, const IntVec2& framebufferExtent) {
        const Int x0 = std::max<Int>(0, scissorBox.x());
        const Int y0 = std::max<Int>(0, scissorBox.y());
        const Int x1 = std::min<Int>(framebufferExtent.x(), scissorBox.x() + std::max<Int>(0, scissorBox.z()));
        const Int y1 = std::min<Int>(framebufferExtent.y(), scissorBox.y() + std::max<Int>(0, scissorBox.w()));

        VkRect2D scissor{};
        scissor.offset = {x0, y0};
        scissor.extent = {
            static_cast<Uint32>(std::max<Int>(0, x1 - x0)),
            static_cast<Uint32>(std::max<Int>(0, y1 - y0)),
        };
        return scissor;
    }

    // The clamped rect, re-expressed against the default framebuffer's stored orientation. Same
    // conversion as the viewport - and it must be the same one, or the scissor would cut a band
    // the draw never touched.
    static VkRect2D MapScissorRectToDefaultFramebuffer(VkRect2D scissor, const IntVec2& framebufferExtent,
                                                       VkSurfaceTransformFlagBitsKHR preTransform) {
        const DefaultFramebufferRectMapping mapping = GetDefaultFramebufferRectMapping(preTransform);
        scissor.offset.x = MapDefaultFramebufferRectAxis(scissor.offset.x, static_cast<Int>(scissor.extent.width),
                                                         framebufferExtent.x(), mapping.mirrorX);
        scissor.offset.y = MapDefaultFramebufferRectAxis(scissor.offset.y, static_cast<Int>(scissor.extent.height),
                                                         framebufferExtent.y(), mapping.flipY);
        return scissor;
    }

    static VkRect2D MakeDefaultFramebufferScissorRect(const IntVec4& scissorBox,
                                                      const IntVec2& framebufferExtent,
                                                      VkSurfaceTransformFlagBitsKHR preTransform) {
        if (!IsQuarterTurnPreTransform(preTransform)) {
            return MapScissorRectToDefaultFramebuffer(MakeClampedScissorRect(scissorBox, framebufferExtent),
                                                      framebufferExtent, preTransform);
        }

        const IntVec2 logicalExtent = ResolveDefaultFramebufferLogicalExtent(preTransform, framebufferExtent);
        const Int logicalX0 = std::max<Int>(0, scissorBox.x());
        const Int logicalY0 = std::max<Int>(0, scissorBox.y());
        const Int logicalX1 = std::min<Int>(logicalExtent.x(), scissorBox.x() + std::max<Int>(0, scissorBox.z()));
        const Int logicalY1 = std::min<Int>(logicalExtent.y(), scissorBox.y() + std::max<Int>(0, scissorBox.w()));

        // Transposed into the rotated image (MapQuarterTurnRect). Scaling each axis by the other's
        // extent - what this did before - only agrees with the rotation for a full-surface box, and
        // turned a compositor's per-window clip into an unrelated sub-rectangle.
        Int rawX = 0, rawY = 0, rawWidth = 0, rawHeight = 0;
        MapQuarterTurnRect(logicalX0, logicalY0, std::max<Int>(0, logicalX1 - logicalX0),
                           std::max<Int>(0, logicalY1 - logicalY0), framebufferExtent, preTransform, &rawX, &rawY,
                           &rawWidth, &rawHeight);
        const Int rawX0 = std::max<Int>(0, rawX);
        const Int rawY0 = std::max<Int>(0, rawY);
        const Int rawX1 = std::min<Int>(framebufferExtent.x(), rawX + rawWidth);
        const Int rawY1 = std::min<Int>(framebufferExtent.y(), rawY + rawHeight);

        VkRect2D scissor{};
        scissor.offset = {rawX0, rawY0};
        scissor.extent = {
            static_cast<Uint32>(std::max<Int>(0, rawX1 - rawX0)),
            static_cast<Uint32>(std::max<Int>(0, rawY1 - rawY0)),
        };
        // A quarter turn maps to {false, false}, so this is a no-op today; it is here so the
        // branch cannot drift away from the identity/180 one when quarter turns are modelled.
        return MapScissorRectToDefaultFramebuffer(scissor, framebufferExtent, preTransform);
    }

    static void ApplyStencilState(VkCommandBuffer commandBuffer) {
        const StencilFaceState& frontStencil = MG_Pipe::gPipeInputs.GetStencilState(StencilFace::Front);
        const StencilFaceState& backStencil = MG_Pipe::gPipeInputs.GetStencilState(StencilFace::Back);
        const Uint32 frontReference = static_cast<Uint32>(std::max(frontStencil.Ref, 0));
        const Uint32 backReference = static_cast<Uint32>(std::max(backStencil.Ref, 0));

        auto& shadow = *g_dynamicStateShadow;
        if (shadow.stencilValid && shadow.stencilFrontCompareMask == frontStencil.ValueMask &&
            shadow.stencilBackCompareMask == backStencil.ValueMask &&
            shadow.stencilFrontWriteMask == frontStencil.WriteMask &&
            shadow.stencilBackWriteMask == backStencil.WriteMask &&
            shadow.stencilFrontReference == frontReference && shadow.stencilBackReference == backReference) {
            return;
        }
        shadow.stencilValid = true;
        shadow.stencilFrontCompareMask = frontStencil.ValueMask;
        shadow.stencilBackCompareMask = backStencil.ValueMask;
        shadow.stencilFrontWriteMask = frontStencil.WriteMask;
        shadow.stencilBackWriteMask = backStencil.WriteMask;
        shadow.stencilFrontReference = frontReference;
        shadow.stencilBackReference = backReference;

        vkCmdSetStencilCompareMask(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, frontStencil.ValueMask);
        vkCmdSetStencilCompareMask(commandBuffer, VK_STENCIL_FACE_BACK_BIT, backStencil.ValueMask);
        vkCmdSetStencilWriteMask(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, frontStencil.WriteMask);
        vkCmdSetStencilWriteMask(commandBuffer, VK_STENCIL_FACE_BACK_BIT, backStencil.WriteMask);
        vkCmdSetStencilReference(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, frontReference);
        vkCmdSetStencilReference(commandBuffer, VK_STENCIL_FACE_BACK_BIT, backReference);
    }

    enum class NumericDomain {
        Unknown,
        FloatLike,
        Sint,
        Uint,
    };

    static NumericDomain GetNumericDomainForShaderValueType(GLenum glType) {
        switch (glType) {
        case GL_FLOAT:
        case GL_FLOAT_VEC2:
        case GL_FLOAT_VEC3:
        case GL_FLOAT_VEC4:
            return NumericDomain::FloatLike;
        case GL_INT:
        case GL_INT_VEC2:
        case GL_INT_VEC3:
        case GL_INT_VEC4:
            return NumericDomain::Sint;
        case GL_UNSIGNED_INT:
        case GL_UNSIGNED_INT_VEC2:
        case GL_UNSIGNED_INT_VEC3:
        case GL_UNSIGNED_INT_VEC4:
            return NumericDomain::Uint;
        default:
            return NumericDomain::Unknown;
        }
    }

    static NumericDomain GetNumericDomainForVertexFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16B16_SNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16B16_UNORM:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16B16_SSCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16B16_USCALED:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_R8G8B8A8_USCALED:
            return NumericDomain::FloatLike;
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8_SINT:
        case VK_FORMAT_R8G8B8A8_SINT:
            return NumericDomain::Sint;
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8A8_UINT:
            return NumericDomain::Uint;
        default:
            return NumericDomain::Unknown;
        }
    }

    static Bool TryCoerceVertexFormatNumericDomain(VkFormat sourceFormat,
                                                   NumericDomain targetDomain,
                                                   VkFormat& outFormat) {
        const NumericDomain sourceDomain = GetNumericDomainForVertexFormat(sourceFormat);
        if (sourceDomain == targetDomain || targetDomain == NumericDomain::Unknown) {
            outFormat = sourceFormat;
            return true;
        }
        if (sourceDomain == NumericDomain::FloatLike) {
            return false;
        }

        switch (sourceFormat) {
        case VK_FORMAT_R32_SINT:
            if (targetDomain == NumericDomain::Uint) {
                outFormat = VK_FORMAT_R32_UINT;
                return true;
            }
            return false;
        case VK_FORMAT_R32G32_SINT:
            if (targetDomain == NumericDomain::Uint) {
                outFormat = VK_FORMAT_R32G32_UINT;
                return true;
            }
            return false;
        case VK_FORMAT_R32G32B32_SINT:
            if (targetDomain == NumericDomain::Uint) {
                outFormat = VK_FORMAT_R32G32B32_UINT;
                return true;
            }
            return false;
        case VK_FORMAT_R32G32B32A32_SINT:
            if (targetDomain == NumericDomain::Uint) {
                outFormat = VK_FORMAT_R32G32B32A32_UINT;
                return true;
            }
            return false;
        case VK_FORMAT_R32_UINT:
            if (targetDomain == NumericDomain::Sint) {
                outFormat = VK_FORMAT_R32_SINT;
                return true;
            }
            return false;
        case VK_FORMAT_R32G32_UINT:
            if (targetDomain == NumericDomain::Sint) {
                outFormat = VK_FORMAT_R32G32_SINT;
                return true;
            }
            return false;
        case VK_FORMAT_R32G32B32_UINT:
            if (targetDomain == NumericDomain::Sint) {
                outFormat = VK_FORMAT_R32G32B32_SINT;
                return true;
            }
            return false;
        case VK_FORMAT_R32G32B32A32_UINT:
            if (targetDomain == NumericDomain::Sint) {
                outFormat = VK_FORMAT_R32G32B32A32_SINT;
                return true;
            }
            return false;
        case VK_FORMAT_R16_SINT:
            outFormat = targetDomain == NumericDomain::Uint ? VK_FORMAT_R16_UINT : VK_FORMAT_R16_SSCALED;
            return true;
        case VK_FORMAT_R16G16_SINT:
            outFormat = targetDomain == NumericDomain::Uint ? VK_FORMAT_R16G16_UINT : VK_FORMAT_R16G16_SSCALED;
            return true;
        case VK_FORMAT_R16G16B16_SINT:
            outFormat = targetDomain == NumericDomain::Uint ? VK_FORMAT_R16G16B16_UINT : VK_FORMAT_R16G16B16_SSCALED;
            return true;
        case VK_FORMAT_R16G16B16A16_SINT:
            outFormat = targetDomain == NumericDomain::Uint ? VK_FORMAT_R16G16B16A16_UINT : VK_FORMAT_R16G16B16A16_SSCALED;
            return true;
        case VK_FORMAT_R16_UINT:
            outFormat = targetDomain == NumericDomain::Sint ? VK_FORMAT_R16_SINT : VK_FORMAT_R16_USCALED;
            return true;
        case VK_FORMAT_R16G16_UINT:
            outFormat = targetDomain == NumericDomain::Sint ? VK_FORMAT_R16G16_SINT : VK_FORMAT_R16G16_USCALED;
            return true;
        case VK_FORMAT_R16G16B16_UINT:
            outFormat = targetDomain == NumericDomain::Sint ? VK_FORMAT_R16G16B16_SINT : VK_FORMAT_R16G16B16_USCALED;
            return true;
        case VK_FORMAT_R16G16B16A16_UINT:
            outFormat = targetDomain == NumericDomain::Sint ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_USCALED;
            return true;
        case VK_FORMAT_R8_SINT:
            outFormat = targetDomain == NumericDomain::Uint ? VK_FORMAT_R8_UINT : VK_FORMAT_R8_SSCALED;
            return true;
        case VK_FORMAT_R8G8_SINT:
            outFormat = targetDomain == NumericDomain::Uint ? VK_FORMAT_R8G8_UINT : VK_FORMAT_R8G8_SSCALED;
            return true;
        case VK_FORMAT_R8G8B8_SINT:
            outFormat = targetDomain == NumericDomain::Uint ? VK_FORMAT_R8G8B8_UINT : VK_FORMAT_R8G8B8_SSCALED;
            return true;
        case VK_FORMAT_R8G8B8A8_SINT:
            outFormat = targetDomain == NumericDomain::Uint ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_SSCALED;
            return true;
        case VK_FORMAT_R8_UINT:
            outFormat = targetDomain == NumericDomain::Sint ? VK_FORMAT_R8_SINT : VK_FORMAT_R8_USCALED;
            return true;
        case VK_FORMAT_R8G8_UINT:
            outFormat = targetDomain == NumericDomain::Sint ? VK_FORMAT_R8G8_SINT : VK_FORMAT_R8G8_USCALED;
            return true;
        case VK_FORMAT_R8G8B8_UINT:
            outFormat = targetDomain == NumericDomain::Sint ? VK_FORMAT_R8G8B8_SINT : VK_FORMAT_R8G8B8_USCALED;
            return true;
        case VK_FORMAT_R8G8B8A8_UINT:
            outFormat = targetDomain == NumericDomain::Sint ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_USCALED;
            return true;
        default:
            return false;
        }
    }

    // Vertex attribute locations are tracked in Uint32 bitmasks, so MAX_VERTEX_ATTRIBS is both the
    // state-layer storage bound and the width of every mask below. Keep them in lockstep.
    static constexpr Uint32 kMaxVertexAttribs =
        static_cast<Uint32>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
    static_assert(kMaxVertexAttribs <= 32, "Vertex attribute masks are Uint32");
    // The loops below walk locations [0, kMaxVertexAttribs) and index programObj.vertexInputTypes with
    // each one, so that array must be at least as wide.
    static_assert(kMaxVertexAttribs <= ProgramFactory::VkProgramObject::kMaxVertexInputLocations,
                  "vertexInputTypes is indexed by vertex attribute location");

    static Bool TryGetCurrentVertexAttributeFormat(GLenum glType, VkFormat& outFormat) {
        switch (glType) {
        case GL_FLOAT:
            outFormat = VK_FORMAT_R32_SFLOAT;
            return true;
        case GL_FLOAT_VEC2:
            outFormat = VK_FORMAT_R32G32_SFLOAT;
            return true;
        case GL_FLOAT_VEC3:
            outFormat = VK_FORMAT_R32G32B32_SFLOAT;
            return true;
        case GL_FLOAT_VEC4:
            outFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
            return true;
        case GL_INT:
            outFormat = VK_FORMAT_R32_SINT;
            return true;
        case GL_INT_VEC2:
            outFormat = VK_FORMAT_R32G32_SINT;
            return true;
        case GL_INT_VEC3:
            outFormat = VK_FORMAT_R32G32B32_SINT;
            return true;
        case GL_INT_VEC4:
            outFormat = VK_FORMAT_R32G32B32A32_SINT;
            return true;
        case GL_UNSIGNED_INT:
            outFormat = VK_FORMAT_R32_UINT;
            return true;
        case GL_UNSIGNED_INT_VEC2:
            outFormat = VK_FORMAT_R32G32_UINT;
            return true;
        case GL_UNSIGNED_INT_VEC3:
            outFormat = VK_FORMAT_R32G32B32_UINT;
            return true;
        case GL_UNSIGNED_INT_VEC4:
            outFormat = VK_FORMAT_R32G32B32A32_UINT;
            return true;
        default:
            return false;
        }
    }

    static Bool TryGetCurrentVertexAttributeUploadPayload(
        const MG_State::GLState::CurrentVertexAttributeValue& currentValue,
        GLenum glType,
        VkFormat& outFormat,
        const void*& outData,
        VkDeviceSize& outSize) {
        switch (glType) {
        case GL_FLOAT:
            outFormat = VK_FORMAT_R32_SFLOAT;
            outData = currentValue.floatValue.data();
            outSize = sizeof(Float);
            return true;
        case GL_FLOAT_VEC2:
            outFormat = VK_FORMAT_R32G32_SFLOAT;
            outData = currentValue.floatValue.data();
            outSize = sizeof(Float) * 2;
            return true;
        case GL_FLOAT_VEC3:
            outFormat = VK_FORMAT_R32G32B32_SFLOAT;
            outData = currentValue.floatValue.data();
            outSize = sizeof(Float) * 3;
            return true;
        case GL_FLOAT_VEC4:
            outFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
            outData = currentValue.floatValue.data();
            outSize = sizeof(Float) * 4;
            return true;
        case GL_INT:
            outFormat = VK_FORMAT_R32_SINT;
            outData = currentValue.intValue.data();
            outSize = sizeof(Int32);
            return true;
        case GL_INT_VEC2:
            outFormat = VK_FORMAT_R32G32_SINT;
            outData = currentValue.intValue.data();
            outSize = sizeof(Int32) * 2;
            return true;
        case GL_INT_VEC3:
            outFormat = VK_FORMAT_R32G32B32_SINT;
            outData = currentValue.intValue.data();
            outSize = sizeof(Int32) * 3;
            return true;
        case GL_INT_VEC4:
            outFormat = VK_FORMAT_R32G32B32A32_SINT;
            outData = currentValue.intValue.data();
            outSize = sizeof(Int32) * 4;
            return true;
        case GL_UNSIGNED_INT:
            outFormat = VK_FORMAT_R32_UINT;
            outData = currentValue.uintValue.data();
            outSize = sizeof(Uint32);
            return true;
        case GL_UNSIGNED_INT_VEC2:
            outFormat = VK_FORMAT_R32G32_UINT;
            outData = currentValue.uintValue.data();
            outSize = sizeof(Uint32) * 2;
            return true;
        case GL_UNSIGNED_INT_VEC3:
            outFormat = VK_FORMAT_R32G32B32_UINT;
            outData = currentValue.uintValue.data();
            outSize = sizeof(Uint32) * 3;
            return true;
        case GL_UNSIGNED_INT_VEC4:
            outFormat = VK_FORMAT_R32G32B32A32_UINT;
            outData = currentValue.uintValue.data();
            outSize = sizeof(Uint32) * 4;
            return true;
        default:
            return false;
        }
    }

    static void RecordTextureCopyError(const char* func, ErrorCode code, const char* message) {
        MG_Pipe::gPipeInputs.RecordError(code, MakeUnique<GenericErrorInfo>("DirectVulkan", func, message));
    }

    namespace {
        static constexpr Uint32 kDescriptorSetsPerFrame = 64;


        enum class BlitSurfaceTransform : Uint32 {
            Identity = 0,
            Rotate90 = 1,
            Rotate180 = 2,
            Rotate270 = 3,
        };

        struct BlitImageBinding {
            VkImage image = VK_NULL_HANDLE;
            VkImageLayout* trackedLayout = nullptr;
            VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_NONE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
            IntVec2 extent = {0, 0};
            Uint32 mipLevel = 0;
            Uint32 mipLevelCount = 1;
            Uint32 baseArrayLayer = 0;
            Uint32 layerCount = 1;
            // z slice for a VK_IMAGE_TYPE_3D source; array attachments use baseArrayLayer instead.
            Uint32 depthOffset = 0;
            const char* label = nullptr;
        };

        static Uint32 ComputeMaxProgramBindings(const VkPhysicalDeviceProperties& properties,
                                                const ProgramFactory::UpdateAfterBindLimits& updateAfterBindLimits) {
            const auto& limits = properties.limits;
            static constexpr Uint32 kMinProgramBindings = 16;
            static constexpr Uint32 kMaxProgramBindingsCap = 256;
            const Uint32 maxCombinedImageSamplers =
                std::min(limits.maxPerStageDescriptorSamplers, limits.maxDescriptorSetSamplers);
            const Uint32 maxSampledImages =
                std::min(limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages);
            const Uint32 maxDynamicUniformBuffers =
                std::min(limits.maxPerStageDescriptorUniformBuffers, limits.maxDescriptorSetUniformBuffersDynamic);

            Uint32 maxBindings = limits.maxPerStageResources;
            maxBindings = std::min(maxBindings, maxCombinedImageSamplers);
            maxBindings = std::min(maxBindings, maxSampledImages + maxDynamicUniformBuffers);

            if (updateAfterBindLimits.enabled) {
                const Uint32 updateAfterBindSamplers = std::min(updateAfterBindLimits.maxPerStageSamplers,
                                                                 updateAfterBindLimits.maxSetSamplers);
                const Uint32 updateAfterBindSampledImages = std::min(updateAfterBindLimits.maxPerStageSampledImages,
                                                                      updateAfterBindLimits.maxSetSampledImages);
                const Uint32 updateAfterBindDynamicUniformBuffers =
                    std::min(updateAfterBindLimits.maxPerStageUniformBuffers,
                             updateAfterBindLimits.maxSetUniformBuffersDynamic);
                Uint32 updateAfterBindBindings = updateAfterBindLimits.maxPerStageResources;
                updateAfterBindBindings = std::min(updateAfterBindBindings, updateAfterBindSamplers);
                updateAfterBindBindings =
                    std::min(updateAfterBindBindings, updateAfterBindSampledImages + updateAfterBindDynamicUniformBuffers);
                maxBindings = std::max(maxBindings, updateAfterBindBindings);
            }

            maxBindings = std::max(kMinProgramBindings, maxBindings);
            maxBindings = std::min(kMaxProgramBindingsCap, maxBindings);
            return maxBindings;
        }

        static void GetImageTransitionSourceState(VkImageLayout oldLayout, VkPipelineStageFlags& outSrcStageMask,
                                                  VkAccessFlags& outSrcAccessMask) {
            switch (oldLayout) {
                case VK_IMAGE_LAYOUT_UNDEFINED:
                case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                    outSrcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                    outSrcAccessMask = 0;
                    break;
                case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                    outSrcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                    outSrcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                    outSrcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    outSrcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
                case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
                    outSrcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
                    outSrcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                    break;
                case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                    outSrcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                    outSrcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    break;
                case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                    outSrcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                    outSrcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                    outSrcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
                    outSrcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    break;
                default:
                    outSrcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                    outSrcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    break;
            }
        }

        static void GetImageTransitionDestinationState(VkImageLayout newLayout, VkPipelineStageFlags& outDstStageMask,
                                                       VkAccessFlags& outDstAccessMask) {
            switch (newLayout) {
                case VK_IMAGE_LAYOUT_UNDEFINED:
                    outDstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                    outDstAccessMask = 0;
                    break;
                case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                    outDstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                    outDstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                    outDstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    outDstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
                case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
                case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                    outDstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
                    outDstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    break;
                case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                    outDstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                    outDstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    break;
                case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                    outDstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                    outDstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    break;
                case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                    outDstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
                    outDstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                    break;
                default:
                    outDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                    outDstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    break;
            }
        }

        static BlitSurfaceTransform ToBlitSurfaceTransform(VkSurfaceTransformFlagBitsKHR preTransform) {
            switch (preTransform) {
                case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
                    return BlitSurfaceTransform::Rotate90;
                case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
                    return BlitSurfaceTransform::Rotate180;
                case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
                    return BlitSurfaceTransform::Rotate270;
                default:
                    return BlitSurfaceTransform::Identity;
            }
        }

        static Bool DecodeReadbackPixel(const Uint8* source, VkFormat sourceFormat, Float* rgba) {
            switch (sourceFormat) {
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                    rgba[0] = static_cast<Float>(source[0]) / 255.0f;
                    rgba[1] = static_cast<Float>(source[1]) / 255.0f;
                    rgba[2] = static_cast<Float>(source[2]) / 255.0f;
                    rgba[3] = static_cast<Float>(source[3]) / 255.0f;
                    return true;
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                    rgba[0] = static_cast<Float>(source[2]) / 255.0f;
                    rgba[1] = static_cast<Float>(source[1]) / 255.0f;
                    rgba[2] = static_cast<Float>(source[0]) / 255.0f;
                    rgba[3] = static_cast<Float>(source[3]) / 255.0f;
                    return true;
                case VK_FORMAT_R16G16B16A16_UNORM:
                    for (SizeT component = 0; component < 4; ++component) {
                        Uint16 value = 0;
                        Memcpy(&value, source + component * sizeof(value), sizeof(value));
                        rgba[component] = static_cast<Float>(value) / 65535.0f;
                    }
                    return true;
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                    for (SizeT component = 0; component < 4; ++component) {
                        Uint16 value = 0;
                        Memcpy(&value, source + component * sizeof(value), sizeof(value));
                        rgba[component] = MG_Util::DecodeHalfBitsToFloat(value);
                    }
                    return true;
                case VK_FORMAT_R32G32B32A32_SFLOAT:
                    Memcpy(rgba, source, sizeof(Float) * 4);
                    return true;
                // Single- and dual-channel formats the reinterpretation feature makes common
                // as readback sources (iterationRP custom images are R32F/R32UI-class).
                // Missing channels take GL's defaults: 0 for GB, 1 for alpha.
                case VK_FORMAT_R32_SFLOAT: {
                    Float value = 0.0f;
                    Memcpy(&value, source, sizeof(value));
                    rgba[0] = value;
                    rgba[1] = 0.0f;
                    rgba[2] = 0.0f;
                    rgba[3] = 1.0f;
                    return true;
                }
                case VK_FORMAT_R32G32_SFLOAT: {
                    Float values[2] = {0.0f, 0.0f};
                    Memcpy(values, source, sizeof(values));
                    rgba[0] = values[0];
                    rgba[1] = values[1];
                    rgba[2] = 0.0f;
                    rgba[3] = 1.0f;
                    return true;
                }
                case VK_FORMAT_R32_UINT: {
                    Uint32 value = 0;
                    Memcpy(&value, source, sizeof(value));
                    rgba[0] = static_cast<Float>(value);
                    rgba[1] = 0.0f;
                    rgba[2] = 0.0f;
                    rgba[3] = 1.0f;
                    return true;
                }
                case VK_FORMAT_R32_SINT: {
                    Int32 value = 0;
                    Memcpy(&value, source, sizeof(value));
                    rgba[0] = static_cast<Float>(value);
                    rgba[1] = 0.0f;
                    rgba[2] = 0.0f;
                    rgba[3] = 1.0f;
                    return true;
                }
                case VK_FORMAT_R16_SFLOAT: {
                    Uint16 value = 0;
                    Memcpy(&value, source, sizeof(value));
                    rgba[0] = MG_Util::DecodeHalfBitsToFloat(value);
                    rgba[1] = 0.0f;
                    rgba[2] = 0.0f;
                    rgba[3] = 1.0f;
                    return true;
                }
                case VK_FORMAT_R16G16_SFLOAT:
                    for (SizeT component = 0; component < 2; ++component) {
                        Uint16 value = 0;
                        Memcpy(&value, source + component * sizeof(value), sizeof(value));
                        rgba[component] = MG_Util::DecodeHalfBitsToFloat(value);
                    }
                    rgba[2] = 0.0f;
                    rgba[3] = 1.0f;
                    return true;
                default:
                    return false;
            }
        }

        static Uint8 EncodeReadbackUnorm8(Float value) {
            if (!(value > 0.0f)) {
                return 0;
            }
            if (value >= 1.0f) {
                return 255;
            }
            return static_cast<Uint8>(value * 255.0f + 0.5f);
        }

        static Int GetReadbackChannelCount(GLenum format) {
            switch (format) {
                case GL_RGB:
                case GL_BGR:
                    return 3;
                case GL_RGBA:
                case GL_BGRA:
                    return 4;
                default:
                    return 0;
            }
        }

        static void StoreReadbackPixel(const Float* rgba, GLenum dstFormat, Uint8* dst) {
            const Uint8 r = EncodeReadbackUnorm8(rgba[0]);
            const Uint8 g = EncodeReadbackUnorm8(rgba[1]);
            const Uint8 b = EncodeReadbackUnorm8(rgba[2]);
            const Uint8 a = EncodeReadbackUnorm8(rgba[3]);
            switch (dstFormat) {
                case GL_RGB:
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    break;
                case GL_BGR:
                    dst[0] = b;
                    dst[1] = g;
                    dst[2] = r;
                    break;
                case GL_RGBA:
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    dst[3] = a;
                    break;
                case GL_BGRA:
                    dst[0] = b;
                    dst[1] = g;
                    dst[2] = r;
                    dst[3] = a;
                    break;
                default:
                    break;
            }
        }

        static void StoreReadbackPixelFloat(const Float* rgba, GLenum dstFormat, Float* dst) {
            const Float r = rgba[0];
            const Float g = rgba[1];
            const Float b = rgba[2];
            const Float a = rgba[3];
            switch (dstFormat) {
                case GL_RGB:
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    break;
                case GL_BGR:
                    dst[0] = b;
                    dst[1] = g;
                    dst[2] = r;
                    break;
                case GL_RGBA:
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    dst[3] = a;
                    break;
                case GL_BGRA:
                    dst[0] = b;
                    dst[1] = g;
                    dst[2] = r;
                    dst[3] = a;
                    break;
                default:
                    break;
            }
        }

        // Generic VkFormat texel decode into the wide RGBA row layouts the shared readback
        // store expects: GL_FLOAT rows for normalized/float sources, GL_INT / GL_UNSIGNED_INT
        // rows for integer sources. Missing channels take GL defaults (0,0,0,1).
        enum class ReadbackSourceClass : Uint8 { Unsupported, Float, SignedInt, UnsignedInt };

        struct ReadbackSourceDesc {
            ReadbackSourceClass sourceClass = ReadbackSourceClass::Unsupported;
            Int channels = 0;       // component count stored per texel
            Int componentBits = 0;  // per-component bits for regular formats; 0 for special packed
            Bool isSnorm = false;
            Bool isSrgb = false;
            Bool bgraSwizzle = false;
            VkFormat special = VK_FORMAT_UNDEFINED; // set for packed/special formats
        };

        static Bool GetReadbackSourceDesc(VkFormat format, ReadbackSourceDesc& out) {
            out = ReadbackSourceDesc{};
            switch (format) {
                // --- regular UNORM ---
                case VK_FORMAT_R8_UNORM:              out = {ReadbackSourceClass::Float, 1, 8};  return true;
                case VK_FORMAT_R8G8_UNORM:            out = {ReadbackSourceClass::Float, 2, 8};  return true;
                case VK_FORMAT_R8G8B8A8_UNORM:        out = {ReadbackSourceClass::Float, 4, 8};  return true;
                case VK_FORMAT_B8G8R8A8_UNORM:        out = {ReadbackSourceClass::Float, 4, 8, false, false, true}; return true;
                case VK_FORMAT_R16_UNORM:             out = {ReadbackSourceClass::Float, 1, 16}; return true;
                case VK_FORMAT_R16G16_UNORM:          out = {ReadbackSourceClass::Float, 2, 16}; return true;
                case VK_FORMAT_R16G16B16A16_UNORM:    out = {ReadbackSourceClass::Float, 4, 16}; return true;
                // --- SRGB (decode to linear like GL readback of sRGB textures) ---
                // GL GetTexImage/ReadPixels of sRGB textures return the raw sRGB-encoded
                // bytes (GL 3.3 has no FRAMEBUFFER_SRGB read decode) - do NOT linearize.
                case VK_FORMAT_R8G8B8A8_SRGB:         out = {ReadbackSourceClass::Float, 4, 8}; return true;
                case VK_FORMAT_B8G8R8A8_SRGB:         out = {ReadbackSourceClass::Float, 4, 8, false, false, true}; return true;
                // --- SNORM ---
                case VK_FORMAT_R8_SNORM:              out = {ReadbackSourceClass::Float, 1, 8, true};  return true;
                case VK_FORMAT_R8G8_SNORM:            out = {ReadbackSourceClass::Float, 2, 8, true};  return true;
                case VK_FORMAT_R8G8B8A8_SNORM:        out = {ReadbackSourceClass::Float, 4, 8, true};  return true;
                case VK_FORMAT_R16_SNORM:             out = {ReadbackSourceClass::Float, 1, 16, true}; return true;
                case VK_FORMAT_R16G16_SNORM:          out = {ReadbackSourceClass::Float, 2, 16, true}; return true;
                case VK_FORMAT_R16G16B16A16_SNORM:    out = {ReadbackSourceClass::Float, 4, 16, true}; return true;
                // --- SFLOAT ---
                case VK_FORMAT_R16_SFLOAT:            out = {ReadbackSourceClass::Float, 1, 16}; out.special = format; return true;
                case VK_FORMAT_R16G16_SFLOAT:         out = {ReadbackSourceClass::Float, 2, 16}; out.special = format; return true;
                case VK_FORMAT_R16G16B16A16_SFLOAT:   out = {ReadbackSourceClass::Float, 4, 16}; out.special = format; return true;
                case VK_FORMAT_R32_SFLOAT:            out = {ReadbackSourceClass::Float, 1, 32}; out.special = format; return true;
                case VK_FORMAT_R32G32_SFLOAT:         out = {ReadbackSourceClass::Float, 2, 32}; out.special = format; return true;
                case VK_FORMAT_R32G32B32A32_SFLOAT:   out = {ReadbackSourceClass::Float, 4, 32}; out.special = format; return true;
                // --- UINT ---
                case VK_FORMAT_R8_UINT:               out = {ReadbackSourceClass::UnsignedInt, 1, 8};  return true;
                case VK_FORMAT_R8G8_UINT:             out = {ReadbackSourceClass::UnsignedInt, 2, 8};  return true;
                case VK_FORMAT_R8G8B8A8_UINT:         out = {ReadbackSourceClass::UnsignedInt, 4, 8};  return true;
                case VK_FORMAT_R16_UINT:              out = {ReadbackSourceClass::UnsignedInt, 1, 16}; return true;
                case VK_FORMAT_R16G16_UINT:           out = {ReadbackSourceClass::UnsignedInt, 2, 16}; return true;
                case VK_FORMAT_R16G16B16A16_UINT:     out = {ReadbackSourceClass::UnsignedInt, 4, 16}; return true;
                case VK_FORMAT_R32_UINT:              out = {ReadbackSourceClass::UnsignedInt, 1, 32}; return true;
                case VK_FORMAT_R32G32_UINT:           out = {ReadbackSourceClass::UnsignedInt, 2, 32}; return true;
                case VK_FORMAT_R32G32B32A32_UINT:     out = {ReadbackSourceClass::UnsignedInt, 4, 32}; return true;
                // --- SINT ---
                case VK_FORMAT_R8_SINT:               out = {ReadbackSourceClass::SignedInt, 1, 8};  return true;
                case VK_FORMAT_R8G8_SINT:             out = {ReadbackSourceClass::SignedInt, 2, 8};  return true;
                case VK_FORMAT_R8G8B8A8_SINT:         out = {ReadbackSourceClass::SignedInt, 4, 8};  return true;
                case VK_FORMAT_R16_SINT:              out = {ReadbackSourceClass::SignedInt, 1, 16}; return true;
                case VK_FORMAT_R16G16_SINT:           out = {ReadbackSourceClass::SignedInt, 2, 16}; return true;
                case VK_FORMAT_R16G16B16A16_SINT:     out = {ReadbackSourceClass::SignedInt, 4, 16}; return true;
                case VK_FORMAT_R32_SINT:              out = {ReadbackSourceClass::SignedInt, 1, 32}; return true;
                case VK_FORMAT_R32G32_SINT:           out = {ReadbackSourceClass::SignedInt, 2, 32}; return true;
                case VK_FORMAT_R32G32B32A32_SINT:     out = {ReadbackSourceClass::SignedInt, 4, 32}; return true;
                // --- packed / special ---
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                case VK_FORMAT_A2B10G10R10_UINT_PACK32:
                case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
                case VK_FORMAT_A2R10G10B10_UINT_PACK32:
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
                case VK_FORMAT_R5G6B5_UNORM_PACK16:
                case VK_FORMAT_B5G6R5_UNORM_PACK16:
                case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
                case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
                case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
                case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
                case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
                    out.sourceClass = (format == VK_FORMAT_A2B10G10R10_UINT_PACK32 ||
                                       format == VK_FORMAT_A2R10G10B10_UINT_PACK32) ?
                        ReadbackSourceClass::UnsignedInt : ReadbackSourceClass::Float;
                    out.special = format;
                    return true;
                default:
                    return false;
            }
        }

        static Float SrgbToLinear(Float value) {
            if (value <= 0.04045f) {
                return value / 12.92f;
            }
            return std::pow((value + 0.055f) / 1.055f, 2.4f);
        }

        static Float DecodeUnsignedF11(Uint32 bits) {
            const Uint32 exponent = (bits >> 6) & 0x1F;
            const Uint32 mantissa = bits & 0x3F;
            if (exponent == 0) {
                return static_cast<Float>(mantissa) / 64.0f * std::pow(2.0f, -14.0f);
            }
            if (exponent == 31) {
                return mantissa == 0 ? std::numeric_limits<Float>::infinity()
                                     : std::numeric_limits<Float>::quiet_NaN();
            }
            return (1.0f + static_cast<Float>(mantissa) / 64.0f) *
                   std::pow(2.0f, static_cast<Float>(static_cast<Int>(exponent)) - 15.0f);
        }

        static Float DecodeUnsignedF10(Uint32 bits) {
            const Uint32 exponent = (bits >> 5) & 0x1F;
            const Uint32 mantissa = bits & 0x1F;
            if (exponent == 0) {
                return static_cast<Float>(mantissa) / 32.0f * std::pow(2.0f, -14.0f);
            }
            if (exponent == 31) {
                return mantissa == 0 ? std::numeric_limits<Float>::infinity()
                                     : std::numeric_limits<Float>::quiet_NaN();
            }
            return (1.0f + static_cast<Float>(mantissa) / 32.0f) *
                   std::pow(2.0f, static_cast<Float>(static_cast<Int>(exponent)) - 15.0f);
        }

        static void DecodeReadbackTexelSpecialFloat(const Uint8* source, VkFormat format, Float* rgba) {
            rgba[0] = 0.0f; rgba[1] = 0.0f; rgba[2] = 0.0f; rgba[3] = 1.0f;
            switch (format) {
                case VK_FORMAT_R16_SFLOAT:
                case VK_FORMAT_R16G16_SFLOAT:
                case VK_FORMAT_R16G16B16A16_SFLOAT: {
                    const Int channels = format == VK_FORMAT_R16_SFLOAT ? 1 :
                                         (format == VK_FORMAT_R16G16_SFLOAT ? 2 : 4);
                    for (Int c = 0; c < channels; ++c) {
                        Uint16 bits = 0;
                        Memcpy(&bits, source + static_cast<SizeT>(c) * sizeof(bits), sizeof(bits));
                        rgba[c] = MG_Util::DecodeHalfBitsToFloat(bits);
                    }
                    return;
                }
                case VK_FORMAT_R32_SFLOAT:
                case VK_FORMAT_R32G32_SFLOAT:
                case VK_FORMAT_R32G32B32A32_SFLOAT: {
                    const Int channels = format == VK_FORMAT_R32_SFLOAT ? 1 :
                                         (format == VK_FORMAT_R32G32_SFLOAT ? 2 : 4);
                    Memcpy(rgba, source, static_cast<SizeT>(channels) * sizeof(Float));
                    return;
                }
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                    Uint32 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[0] = static_cast<Float>(word & 0x3FFu) / 1023.0f;
                    rgba[1] = static_cast<Float>((word >> 10) & 0x3FFu) / 1023.0f;
                    rgba[2] = static_cast<Float>((word >> 20) & 0x3FFu) / 1023.0f;
                    rgba[3] = static_cast<Float>((word >> 30) & 0x3u) / 3.0f;
                    return;
                }
                case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
                    Uint32 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[2] = static_cast<Float>(word & 0x3FFu) / 1023.0f;
                    rgba[1] = static_cast<Float>((word >> 10) & 0x3FFu) / 1023.0f;
                    rgba[0] = static_cast<Float>((word >> 20) & 0x3FFu) / 1023.0f;
                    rgba[3] = static_cast<Float>((word >> 30) & 0x3u) / 3.0f;
                    return;
                }
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
                    Uint32 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[0] = DecodeUnsignedF11(word & 0x7FFu);
                    rgba[1] = DecodeUnsignedF11((word >> 11) & 0x7FFu);
                    rgba[2] = DecodeUnsignedF10((word >> 22) & 0x3FFu);
                    return;
                }
                case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: {
                    Uint32 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    const Int exponent = static_cast<Int>((word >> 27) & 0x1Fu) - 15 - 9;
                    const Float scale = std::pow(2.0f, static_cast<Float>(exponent));
                    rgba[0] = static_cast<Float>(word & 0x1FFu) * scale;
                    rgba[1] = static_cast<Float>((word >> 9) & 0x1FFu) * scale;
                    rgba[2] = static_cast<Float>((word >> 18) & 0x1FFu) * scale;
                    return;
                }
                case VK_FORMAT_R5G6B5_UNORM_PACK16:
                case VK_FORMAT_B5G6R5_UNORM_PACK16: {
                    Uint16 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    const Float c0 = static_cast<Float>((word >> 11) & 0x1Fu) / 31.0f;
                    const Float c1 = static_cast<Float>((word >> 5) & 0x3Fu) / 63.0f;
                    const Float c2 = static_cast<Float>(word & 0x1Fu) / 31.0f;
                    const Bool bgr = format == VK_FORMAT_B5G6R5_UNORM_PACK16;
                    rgba[0] = bgr ? c2 : c0;
                    rgba[1] = c1;
                    rgba[2] = bgr ? c0 : c2;
                    return;
                }
                case VK_FORMAT_A1R5G5B5_UNORM_PACK16: {
                    Uint16 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[3] = static_cast<Float>((word >> 15) & 0x1u);
                    rgba[0] = static_cast<Float>((word >> 10) & 0x1Fu) / 31.0f;
                    rgba[1] = static_cast<Float>((word >> 5) & 0x1Fu) / 31.0f;
                    rgba[2] = static_cast<Float>(word & 0x1Fu) / 31.0f;
                    return;
                }
                case VK_FORMAT_R5G5B5A1_UNORM_PACK16: {
                    Uint16 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[0] = static_cast<Float>((word >> 11) & 0x1Fu) / 31.0f;
                    rgba[1] = static_cast<Float>((word >> 6) & 0x1Fu) / 31.0f;
                    rgba[2] = static_cast<Float>((word >> 1) & 0x1Fu) / 31.0f;
                    rgba[3] = static_cast<Float>(word & 0x1u);
                    return;
                }
                case VK_FORMAT_B5G5R5A1_UNORM_PACK16: {
                    Uint16 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[2] = static_cast<Float>((word >> 11) & 0x1Fu) / 31.0f;
                    rgba[1] = static_cast<Float>((word >> 6) & 0x1Fu) / 31.0f;
                    rgba[0] = static_cast<Float>((word >> 1) & 0x1Fu) / 31.0f;
                    rgba[3] = static_cast<Float>(word & 0x1u);
                    return;
                }
                case VK_FORMAT_R4G4B4A4_UNORM_PACK16: {
                    Uint16 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[0] = static_cast<Float>((word >> 12) & 0xFu) / 15.0f;
                    rgba[1] = static_cast<Float>((word >> 8) & 0xFu) / 15.0f;
                    rgba[2] = static_cast<Float>((word >> 4) & 0xFu) / 15.0f;
                    rgba[3] = static_cast<Float>(word & 0xFu) / 15.0f;
                    return;
                }
                case VK_FORMAT_B4G4R4A4_UNORM_PACK16: {
                    Uint16 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[2] = static_cast<Float>((word >> 12) & 0xFu) / 15.0f;
                    rgba[1] = static_cast<Float>((word >> 8) & 0xFu) / 15.0f;
                    rgba[0] = static_cast<Float>((word >> 4) & 0xFu) / 15.0f;
                    rgba[3] = static_cast<Float>(word & 0xFu) / 15.0f;
                    return;
                }
                default:
                    return;
            }
        }

        static Bool DecodeReadbackRowsToWide(const Uint8* srcPixels, VkFormat srcFormat, GLsizei width,
                                             GLsizei height, Vector<Uint8>& outWide, GLenum& outWideType) {
            ReadbackSourceDesc desc{};
            if (!GetReadbackSourceDesc(srcFormat, desc)) {
                return false;
            }
            const SizeT texelSize = VulkanRenderer::GetReadbackTexelSize(srcFormat);
            if (texelSize == 0) {
                return false;
            }
            const SizeT pixelCount = static_cast<SizeT>(width) * static_cast<SizeT>(height);
            outWide.assign(pixelCount * 4 * sizeof(Uint32), 0);

            if (desc.sourceClass == ReadbackSourceClass::Float) {
                outWideType = GL_FLOAT;
                Float* wide = reinterpret_cast<Float*>(outWide.data());
                for (SizeT i = 0; i < pixelCount; ++i) {
                    const Uint8* source = srcPixels + i * texelSize;
                    Float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    if (desc.special != VK_FORMAT_UNDEFINED) {
                        DecodeReadbackTexelSpecialFloat(source, desc.special, rgba);
                    } else {
                        for (Int c = 0; c < desc.channels; ++c) {
                            Float value = 0.0f;
                            if (desc.componentBits == 8) {
                                if (desc.isSnorm) {
                                    Int8 raw = 0;
                                    Memcpy(&raw, source + c, sizeof(raw));
                                    value = std::max(static_cast<Float>(raw) / 127.0f, -1.0f);
                                } else {
                                    value = static_cast<Float>(source[c]) / 255.0f;
                                }
                            } else { // 16
                                if (desc.isSnorm) {
                                    Int16 raw = 0;
                                    Memcpy(&raw, source + static_cast<SizeT>(c) * 2, sizeof(raw));
                                    value = std::max(static_cast<Float>(raw) / 32767.0f, -1.0f);
                                } else {
                                    Uint16 raw = 0;
                                    Memcpy(&raw, source + static_cast<SizeT>(c) * 2, sizeof(raw));
                                    value = static_cast<Float>(raw) / 65535.0f;
                                }
                            }
                            if (desc.isSrgb && c < 3) {
                                value = SrgbToLinear(value);
                            }
                            rgba[c] = value;
                        }
                        if (desc.bgraSwizzle) {
                            std::swap(rgba[0], rgba[2]);
                        }
                    }
                    Memcpy(wide + i * 4, rgba, sizeof(rgba));
                }
                return true;
            }

            // Integer classes: decode to 4 x (U)Int32 per texel; missing alpha reads 1.
            outWideType = desc.sourceClass == ReadbackSourceClass::SignedInt ? GL_INT : GL_UNSIGNED_INT;
            Uint32* wide = reinterpret_cast<Uint32*>(outWide.data());
            for (SizeT i = 0; i < pixelCount; ++i) {
                const Uint8* source = srcPixels + i * texelSize;
                Uint32 rgba[4] = {0, 0, 0, 1};
                if (srcFormat == VK_FORMAT_A2B10G10R10_UINT_PACK32) {
                    Uint32 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[0] = word & 0x3FFu;
                    rgba[1] = (word >> 10) & 0x3FFu;
                    rgba[2] = (word >> 20) & 0x3FFu;
                    rgba[3] = (word >> 30) & 0x3u;
                } else if (srcFormat == VK_FORMAT_A2R10G10B10_UINT_PACK32) {
                    Uint32 word = 0;
                    Memcpy(&word, source, sizeof(word));
                    rgba[2] = word & 0x3FFu;
                    rgba[1] = (word >> 10) & 0x3FFu;
                    rgba[0] = (word >> 20) & 0x3FFu;
                    rgba[3] = (word >> 30) & 0x3u;
                } else {
                    for (Int c = 0; c < desc.channels; ++c) {
                        if (desc.componentBits == 8) {
                            if (desc.sourceClass == ReadbackSourceClass::SignedInt) {
                                Int8 raw = 0;
                                Memcpy(&raw, source + c, sizeof(raw));
                                rgba[c] = static_cast<Uint32>(static_cast<Int32>(raw));
                            } else {
                                rgba[c] = source[c];
                            }
                        } else if (desc.componentBits == 16) {
                            if (desc.sourceClass == ReadbackSourceClass::SignedInt) {
                                Int16 raw = 0;
                                Memcpy(&raw, source + static_cast<SizeT>(c) * 2, sizeof(raw));
                                rgba[c] = static_cast<Uint32>(static_cast<Int32>(raw));
                            } else {
                                Uint16 raw = 0;
                                Memcpy(&raw, source + static_cast<SizeT>(c) * 2, sizeof(raw));
                                rgba[c] = raw;
                            }
                        } else {
                            Memcpy(&rgba[c], source + static_cast<SizeT>(c) * 4, sizeof(Uint32));
                        }
                    }
                }
                Memcpy(wide + i * 4, rgba, sizeof(rgba));
            }
            return true;
        }

        // True floating-point color formats are exempt from GL_FIXED_ONLY read clamping.
        static Bool IsFloatingPointReadbackFormat(VkFormat format) {
            switch (format) {
            case VK_FORMAT_R16_SFLOAT:
            case VK_FORMAT_R16G16_SFLOAT:
            case VK_FORMAT_R16G16B16_SFLOAT:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
            case VK_FORMAT_R32_SFLOAT:
            case VK_FORMAT_R32G32_SFLOAT:
            case VK_FORMAT_R32G32B32_SFLOAT:
            case VK_FORMAT_R32G32B32A32_SFLOAT:
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
                return true;
            default:
                return false;
            }
        }

        // The GL internal format a packed VkFormat stores, for the raw-word readback test below.
        // Only the packed 32-bit layouts MobileGL keeps natively need an entry; anything else takes
        // the wide decode path.
        static TextureInternalFormat GetPackedReadbackInternalFormat(VkFormat format) {
            switch (format) {
            case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
                return TextureInternalFormat::RGB9E5;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                return TextureInternalFormat::R11FG11FB10F;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                return TextureInternalFormat::RGB10A2;
            case VK_FORMAT_A2B10G10R10_UINT_PACK32:
                return TextureInternalFormat::RGB10A2UI;
            default:
                return TextureInternalFormat::Unknown;
            }
        }

        static Bool PackReadbackToClientOrPbo(const Uint8* srcPixels, VkFormat srcFormat, GLsizei width,
                                              GLsizei sliceHeight, GLsizei sliceCount, GLenum format, GLenum type,
                                              void* pixels, Bool applyPackImageParams,
                                              Bool applyReadColorClamp = false) {
            if (width <= 0 || sliceHeight <= 0 || sliceCount <= 0) {
                return true;
            }

            DirectGLES::ReadbackImpl::ReadbackChannelMapping mapping{};
            if (!DirectGLES::ReadbackImpl::GetReadbackChannelMapping(format, mapping) ||
                DirectGLES::ReadbackImpl::GetReadbackDstPixelSize(mapping, type) == 0) {
                MGLOG_E_ONCE("DirectVulkan readback skipped: unsupported format=0x%x type=0x%x", format, type);
                return false;
            }

            // A packed image read with the matching client type hands back its own words: the
            // decode-to-float / re-encode round trip is lossy in the bits (it canonicalizes an
            // RGB9_E5 shared exponent), which glGetTexImage must not do. Left to the wide path when
            // GL_CLAMP_READ_COLOR may still have to act, i.e. for glReadPixels.
            if (!applyReadColorClamp &&
                MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer(
                    GetPackedReadbackInternalFormat(srcFormat), MG_Util::ConvertGLEnumToTextureInputFormat(format),
                    MG_Util::ConvertGLEnumToTexturePixelDataType(type))) {
                return DirectGLES::ReadbackImpl::StorePackedWordsToClient(srcPixels, width, sliceHeight, sliceCount,
                                                                          type, pixels, applyPackImageParams);
            }

            Vector<Uint8> wide;
            GLenum wideType = GL_FLOAT;
            if (!DecodeReadbackRowsToWide(srcPixels, srcFormat, width,
                                          sliceHeight * sliceCount, wide, wideType)) {
                MGLOG_E_ONCE("DirectVulkan readback skipped: unsupported source format=%d",
                        static_cast<Int>(srcFormat));
                return false;
            }

            // glReadPixels final conversion: GL_CLAMP_READ_COLOR defaults to GL_FIXED_ONLY,
            // clamping fixed-point (normalized) buffers to [0,1] - visible for SNORM reads.
            if (applyReadColorClamp && wideType == GL_FLOAT) {
                const GLenum clampMode = MG_Pipe::gPipeInputs.GetClampReadColor();
                const Bool clamp = clampMode == GL_TRUE ||
                    (clampMode == GL_FIXED_ONLY && !IsFloatingPointReadbackFormat(srcFormat));
                if (clamp) {
                    Float* values = reinterpret_cast<Float*>(wide.data());
                    const SizeT count = wide.size() / sizeof(Float);
                    for (SizeT i = 0; i < count; ++i) {
                        values[i] = std::min(std::max(values[i], 0.0f), 1.0f);
                    }
                }
            }

            const Bool sourceIsInteger = wideType == GL_INT || wideType == GL_UNSIGNED_INT;
            if (sourceIsInteger != mapping.isInteger) {
                MGLOG_E_ONCE("DirectVulkan readback skipped: integerness mismatch (format=0x%x source=%d)",
                        format, static_cast<Int>(srcFormat));
                return false;
            }

            return DirectGLES::ReadbackImpl::StoreWideRowsToClient(wide.data(), wideType, width, sliceHeight,
                                                                   sliceCount, mapping, type, pixels,
                                                                   applyPackImageParams);
        }
    } // namespace

    SizeT VulkanRenderer::GetReadbackTexelSize(VkFormat sourceFormat) {
        const VKU_FORMAT_INFO formatInfo = vkuGetFormatInfo(sourceFormat);
        if (formatInfo.texels_per_block != 1) {
            return 0;
        }
        return formatInfo.texel_block_size;
    }

    Bool VulkanRenderer::MapDefaultFramebufferReadbackRect(
            GLint x, GLint y, GLsizei width, GLsizei height, VkExtent2D imageExtent,
            VkSurfaceTransformFlagBitsKHR preTransform, VkOffset2D* imageOffset,
            VkExtent2D* imageCopyExtent) {
        if (width <= 0 || height <= 0 || imageOffset == nullptr || imageCopyExtent == nullptr) {
            return false;
        }

        const Int imageWidth = static_cast<Int>(imageExtent.width);
        const Int imageHeight = static_cast<Int>(imageExtent.height);
        Int mappedX = x;
        Int mappedY = y;
        Uint32 mappedWidth = static_cast<Uint32>(width);
        Uint32 mappedHeight = static_cast<Uint32>(height);

        // InsertPositionFixup first flips GL Y and then applies the surface transform. In pixel
        // coordinates that gives these half-open rectangle mappings into the stored image:
        //   identity: (x, H-y-h), 90: (y, x), 180: (W-x-w, y), 270: (H-y-h, W-x-w).
        // Quarter turns also transpose the copied block's extent.
        switch (preTransform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            mappedX = y;
            mappedY = x;
            mappedWidth = static_cast<Uint32>(height);
            mappedHeight = static_cast<Uint32>(width);
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            mappedX = imageWidth - x - width;
            mappedY = y;
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            mappedX = imageWidth - y - height;
            mappedY = imageHeight - x - width;
            mappedWidth = static_cast<Uint32>(height);
            mappedHeight = static_cast<Uint32>(width);
            break;
        default:
            mappedY = imageHeight - y - height;
            break;
        }

        if (mappedX < 0 || mappedY < 0 || mappedWidth > imageExtent.width ||
            mappedHeight > imageExtent.height ||
            static_cast<Uint64>(mappedX) + mappedWidth > imageExtent.width ||
            static_cast<Uint64>(mappedY) + mappedHeight > imageExtent.height) {
            return false;
        }
        *imageOffset = {mappedX, mappedY};
        *imageCopyExtent = {mappedWidth, mappedHeight};
        return true;
    }

    Bool VulkanRenderer::RemapDefaultFramebufferReadback(
            const Uint8* rawPixels, Uint32 logicalWidth, Uint32 logicalHeight,
            VkSurfaceTransformFlagBitsKHR preTransform, SizeT texelSize, Uint8* outPixels) {
        if (rawPixels == nullptr || outPixels == nullptr || logicalWidth == 0 || logicalHeight == 0 ||
            texelSize == 0) {
            return false;
        }

        const Uint32 rawWidth = IsQuarterTurnPreTransform(preTransform) ? logicalHeight : logicalWidth;
        for (Uint32 outY = 0; outY < logicalHeight; ++outY) {
            for (Uint32 outX = 0; outX < logicalWidth; ++outX) {
                Uint32 srcX = outX;
                Uint32 srcY = outY;
                switch (preTransform) {
                case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
                    srcX = outY;
                    srcY = outX;
                    break;
                case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
                    srcX = logicalWidth - 1 - outX;
                    break;
                case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
                    srcX = logicalHeight - 1 - outY;
                    srcY = logicalWidth - 1 - outX;
                    break;
                default:
                    srcY = logicalHeight - 1 - outY;
                    break;
                }
                Memcpy(outPixels + (static_cast<SizeT>(outY) * logicalWidth + outX) * texelSize,
                       rawPixels + (static_cast<SizeT>(srcY) * rawWidth + srcX) * texelSize,
                       texelSize);
            }
        }
        return true;
    }

    Bool VulkanRenderer::ConvertReadbackPixels(const Uint8* sourcePixels, VkFormat sourceFormat,
                                               GLsizei width, GLsizei height, GLenum destinationFormat,
                                               GLenum destinationType, SizeT destinationRowStride,
                                               Uint8* destinationPixels) {
        if (width <= 0 || height <= 0) {
            return true;
        }
        if (sourcePixels == nullptr || destinationPixels == nullptr) {
            return false;
        }

        const SizeT sourceTexelSize = GetReadbackTexelSize(sourceFormat);
        const Int destinationChannels = GetReadbackChannelCount(destinationFormat);
        if (sourceTexelSize == 0 || destinationChannels == 0 ||
            (destinationType != GL_UNSIGNED_BYTE && destinationType != GL_FLOAT)) {
            return false;
        }
        const SizeT destinationComponentSize = destinationType == GL_FLOAT ? sizeof(Float) : sizeof(Uint8);
        const SizeT destinationPixelSize = static_cast<SizeT>(destinationChannels) * destinationComponentSize;
        if (destinationRowStride < static_cast<SizeT>(width) * destinationPixelSize) {
            return false;
        }

        for (GLsizei row = 0; row < height; ++row) {
            const Uint8* sourceRow = sourcePixels +
                static_cast<SizeT>(row) * static_cast<SizeT>(width) * sourceTexelSize;
            Uint8* destinationRow = destinationPixels + static_cast<SizeT>(row) * destinationRowStride;
            for (GLsizei column = 0; column < width; ++column) {
                const Uint8* source = sourceRow + static_cast<SizeT>(column) * sourceTexelSize;
                Uint8* destination = destinationRow + static_cast<SizeT>(column) * destinationPixelSize;
                Float rgba[4]{};
                if (!DecodeReadbackPixel(source, sourceFormat, rgba)) {
                    return false;
                }
                if (destinationType == GL_FLOAT) {
                    Float converted[4]{};
                    StoreReadbackPixelFloat(rgba, destinationFormat, converted);
                    Memcpy(destination, converted, destinationPixelSize);
                } else {
                    StoreReadbackPixel(rgba, destinationFormat, destination);
                }
            }
        }
        return true;
    }

    VkBool32 VulkanRenderer::DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                           VkDebugUtilsMessageTypeFlagsEXT messageType,
                                           const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
        auto typeToString = [](VkDebugUtilsMessageTypeFlagsEXT messageType) {
            switch (messageType) {
            case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
                return "General";
            case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
                return "Validation";
            case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
                return "Performance";
            case VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT:
                return "DeviceAddressBinding";
            default:
                return "Other";
            }
        };

        switch (messageSeverity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            MGLOG_E_ONCE("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            MGLOG_W_ONCE("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            MGLOG_D("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            MGLOG_D("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        default:
            break;
        }
        return VK_FALSE;
    }

    VulkanRenderer::VulkanRenderer(NativeWindowType window, const VulkanRendererConfig& cfg)
        : m_window(window), m_config(cfg), m_presentsToAppWindow(window != NativeWindowType{}) {
        if (!m_presentsToAppWindow) {
            m_config.SwapInterval.reset();
        }
        // Initialize();
    }

    VulkanRenderer::~VulkanRenderer() {
        Shutdown();
    }

    inline ProgramFactory::CompileOptionFlags GetShaderTransformFlags(VkSurfaceTransformFlagBitsKHR preTransform) {
        ProgramFactory::CompileOptionFlags flags = ProgramFactory::CompileOptionBit::PositionZRemap;
        const auto* wireFbo = MG_Pipe::MGPipeApplier().DrawFramebuffer();
        const Bool isDefault = wireFbo && wireFbo->IsDefault;
        if (isDefault) {
            flags |= ProgramFactory::CompileOptionBit::PositionYFlip;
            // gl_FragCoord follows the same rule the default-framebuffer RECTANGLES follow
            // (GetDefaultFramebufferRectMapping): flipped for identity/180, left alone under a
            // quarter turn, which this renderer converts nothing for. Keeping the two in step
            // is the whole point - a fragment's window Y and the viewport that placed it must
            // agree on which end of the image they count from.
            if (!IsQuarterTurnPreTransform(preTransform)) {
                flags |= ProgramFactory::CompileOptionBit::FragCoordYFlip;
            }
            switch (preTransform) {
            case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
                flags |= ProgramFactory::CompileOptionBit::SurfaceRotate90;
                break;
            case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
                flags |= ProgramFactory::CompileOptionBit::SurfaceRotate180;
                break;
            case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
                flags |= ProgramFactory::CompileOptionBit::SurfaceRotate270;
                break;
            default:
                break;
            }
        }
        return flags;
    }

    void VulkanRenderer::Initialize() {
        // P2 D14, and it belongs HERE rather than on a draw: "a Track-H subsystem whose bit is
        // clear is a STARTUP Fatal{PipeLegacyMemosDisabled}". Checks Magma's own bit only, and
        // only once this backend is the one being brought up, so an Espryt-side bitmask cannot
        // kill a Magma run and vice versa.
        MagmaPipeValidateSubsystemConfiguration();
        CreateInstance();
        if (!CreateSurface()) throw RuntimeError("DirectVulkan: the initial surface could not be created");
        PickPhysicalDevice();
        CreateLogicalDeviceAndQueues();
        CreateAllocator();

        CreateCommandPool();
        m_progressMarkers.Init(m_device, m_physicalDevice.queueFamilies.graphicsFamily);

        // Frames-in-flight is a request, not a guarantee: it also seeds the swapchain image
        // count (SwapchainObject clamps the hint into [minImageCount, maxImageCount]). Not every
        // driver/surface supports >= 3 swapchain images, and keeping more frame slots than the
        // surface can present would leave the surplus slots stalling on vkAcquireNextImageKHR.
        // So clamp to the surface's real limits here, before any per-frame resource is sized off
        // it. (The standalone driver POST is headless and has no surface, so this check lives at
        // renderer init.) Existing logs already report the swapchain's min/actual image count;
        // this one adds the frames-in-flight decision itself.
        {
            // Desired depth comes from MOBILEGL_MAGMA_FRAMESINFLIGHT, parsed once by ConfigLoader
            // with a default of 3 when the variable is unset or invalid.
            Uint32 requestedFramesInFlight = MG_Config::Features.MagmaFramesInFlight;
            MGLOG_I("MaxFramesInFlight: configured request=%u", requestedFramesInFlight);

            VkSurfaceCapabilitiesKHR surfaceCaps{};
            const VkResult capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                m_physicalDevice.handle, m_surface, &surfaceCaps);
            if (capsResult != VK_SUCCESS) {
                MGLOG_W("MaxFramesInFlight: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (VkResult=%d); "
                        "keeping requested %u", static_cast<Int>(capsResult), requestedFramesInFlight);
            } else {
                // Frames-in-flight is the CPU pipeline depth; it only needs to stay <= the number
                // of swapchain images the surface can provide (maxImageCount), so the extra slots
                // never stall on vkAcquireNextImageKHR. It must NOT be forced up to minImageCount:
                // the swapchain independently gets >= minImageCount images (SwapchainObject raises
                // the count), and inflating the CPU depth would only add latency + memory.
                Uint32 chosenFramesInFlight = requestedFramesInFlight;
                if (surfaceCaps.maxImageCount != 0 && chosenFramesInFlight > surfaceCaps.maxImageCount) {
                    chosenFramesInFlight = surfaceCaps.maxImageCount;  // 0 == no upper bound
                }
                if (chosenFramesInFlight < 2) {
                    chosenFramesInFlight = 2;  // never drop below double buffering
                }
                m_config.MaxFramesInFlight = chosenFramesInFlight;
                if (chosenFramesInFlight != requestedFramesInFlight) {
                    MGLOG_W("MaxFramesInFlight: requested %u unsupported by surface (minImageCount=%u, "
                            "maxImageCount=%u); using %u", requestedFramesInFlight, surfaceCaps.minImageCount,
                            surfaceCaps.maxImageCount, chosenFramesInFlight);
                } else {
                    MGLOG_I("MaxFramesInFlight: using %u (surface minImageCount=%u, maxImageCount=%u)",
                            chosenFramesInFlight, surfaceCaps.minImageCount, surfaceCaps.maxImageCount);
                }
            }
        }

        VK_VERIFY(m_frameContext.Initialize(m_device, m_commandPool, m_config.MaxFramesInFlight),
                  "CreateFrameContexts");
        MGLOG_I("CreateFrameContexts completed");
        auto succeeded = false;
        succeeded = m_bufferManager.Initialize({
            .allocator = m_allocator,
            .frameCount = m_frameContext.GetFrameCount(),
            .minUploadBytes = 4 * 1024 * 1024,
            .transientMemoryUsage = VMA_MEMORY_USAGE_AUTO,
            .transientAllocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .transientPersistentMapping = true,
            .transformFeedbackUsageEnabled = m_transformFeedbackFeatureEnabled,
        });
        MOBILEGL_ASSERT(succeeded, "VkBufferManager initialization failed.");
        m_bufferManager.SetCopyCommandProvider(this);
        if (m_timerQuerySupported) {
            m_timerQueryManager = MakeUnique<VkTimerQueryManager>();
            if (m_timerQueryManager->Initialize({.device = m_device,
                                                 .frameCount = m_frameContext.GetFrameCount(),
                                                 .timestampValidBits = m_timestampValidBits,
                                                 .timestampPeriodNs = m_timestampPeriodNs})) {
                m_frameContext.SetRecordingObserver(this);
            } else {
                MGLOG_W("VkTimerQueryManager initialization failed; timer queries disabled");
                m_timerQueryManager.reset();
                m_timerQuerySupported = false;
            }
        }
        m_textureManager = MakeUnique<VkTextureManager>();
        MOBILEGL_ASSERT(m_textureManager != nullptr, "VkTextureManager creation failed.");
        succeeded = m_textureManager->Initialize(
            {m_device, m_physicalDevice.handle, m_allocator, m_commandPool, m_graphicsQueue,
             m_frameContext.GetFrameCount(), m_imageFormatListExtensionEnabled,
             m_sampledReadStageMask,
             static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily)});
        MOBILEGL_ASSERT(succeeded, "VkTextureManager initialization failed.");
        m_clearManager = MakeUnique<VkClearManager>();
        MOBILEGL_ASSERT(m_clearManager != nullptr, "VkClearManager creation failed.");
        succeeded = m_clearManager->Initialize();
        MOBILEGL_ASSERT(succeeded, "VkClearManager initialization failed.");
        m_renderPassManager =
            MakeUnique<VkRenderPassManager>(m_device, m_physicalDevice.handle, m_allocator, m_config, *m_clearManager,
                                            *m_textureManager, m_swapchainObject);
        MOBILEGL_ASSERT(m_renderPassManager != nullptr, "VkRenderPassManager creation failed.");
        succeeded = m_renderPassManager->Initialize();
        MOBILEGL_ASSERT(succeeded, "VkRenderPassManager initialization failed.");

        const Uint32 maxProgramBindings = ComputeMaxProgramBindings(m_physicalDevice.properties, m_updateAfterBindLimits);
        MGLOG_I("DirectVulkan: using %u program descriptor bindings", maxProgramBindings);
        if (IsPowerVRDevice(m_physicalDevice.properties)) {
            m_config.DisablePipelineCache = true;
            MGLOG_W("DirectVulkan: disabling pipeline cache on PowerVR device %s",
                    m_physicalDevice.properties.deviceName);
        }

        RecreateSwapchain();

        m_pipelineFactory = MakeUnique<PipelineFactory>(m_device, m_config);
        MOBILEGL_ASSERT(m_pipelineFactory != nullptr, "PipelineFactory creation failed.");
        {
            // Qualcomm's pipeline compiler does not keep vertex positions invariant across
            // the pipelines of a multi-pass depth-equality chain (even with the SPIR-V
            // Invariant decoration), so a blended depth-writing prepass makes later
            // equality-compare passes drop whole primitives (MC 26.3 improved-transparency
            // clouds flicker black). Suppress depth writes on accumulation-blended pipelines
            // there (see PipelineFactory::ShouldSuppressDepthWrite for the exact scope);
            // MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE forces the quirk on or off on any
            // driver.
            const MG_Config::QuirkOverride quirkOverride =
                MG_Config::Features.MagmaDisableBlendedDepthWriteQuirk;
            const Bool suppressBlendedDepthWrite = PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(
                quirkOverride, m_physicalDevice.properties.vendorID);
            if (suppressBlendedDepthWrite) {
                MGLOG_I("DirectVulkan: suppressing depth writes on accumulation-blended pipelines "
                        "(driver lacks cross-pipeline position invariance)%s",
                        quirkOverride == MG_Config::QuirkOverride::ForceOn ? " (forced on)" : "");
            }
            PipelineFactory::SetSuppressBlendedDepthWrite(suppressBlendedDepthWrite);
        }
        ProgramFactory::SubgroupLoweringPolicy subgroupPolicy{};
        subgroupPolicy.emulateSubgroups = ShouldEmulateSubgroups(m_nativeSubgroupSupported);
        subgroupPolicy.fixIterationRPSubgroupScratch =
            m_nativeSubgroupSupported && ShouldFixIterationRPSubgroupScratch();
        subgroupPolicy.fixIterationRPBarrier = ShouldFixIterationRPBarrier();
        subgroupPolicy.deriveNumSubgroups =
            m_nativeSubgroupSupported && ShouldDeriveNumSubgroups();
        subgroupPolicy.requireFullSubgroups = m_computeFullSubgroupsFeatureEnabled;
        subgroupPolicy.nativeSubgroupSize = m_nativeSubgroupSize;
        subgroupPolicy.maxComputeWorkgroupSubgroups = m_maxComputeWorkgroupSubgroups;
        subgroupPolicy.maxComputeSharedMemoryBytes =
            m_physicalDevice.properties.limits.maxComputeSharedMemorySize;
        m_programFactory = MakeUnique<ProgramFactory>(m_device, m_config, maxProgramBindings,
                                                      m_shaderDrawParametersFeatureEnabled,
                                                      m_unformattedFloatStorageImagesEnabled,
                                                      m_tessellationAndGeometryPointSizeFeatureEnabled,
                                                      MG_Config::Features.EnableSpirvValidation,
                                                      m_updateAfterBindLimits, subgroupPolicy);
        MOBILEGL_ASSERT(m_programFactory != nullptr, "ProgramFactory creation failed.");
        // The swapchain already exists at this point (Initialize creates it first), so seed the
        // height the factory could not be told about from CreateSwapchain.
        m_programFactory->SetDefaultFramebufferHeight(m_swapchainObject.GetExtent().height);
        // Aging evictions (render passes and program entries) must purge the dependent
        // pipeline / compute-pipeline / descriptor-set caches in the same step; both
        // sweeps only run from the frame-boundary seams, long after initialization.
        m_renderPassManager->SetEvictionObserver(this);
        m_programFactory->SetEvictionObserver(this);

        m_samplerManager = MakeUnique<VkSamplerManager>();
        MOBILEGL_ASSERT(m_samplerManager != nullptr, "VkSamplerManager creation failed.");
        succeeded = m_samplerManager->Initialize({m_device, &m_config, m_samplerAnisotropyFeatureEnabled,
                                                  m_physicalDevice.properties.limits.maxSamplerAnisotropy,
                                                  m_customBorderColorFeatureEnabled,
                                                  m_maxCustomBorderColorSamplers});
        MOBILEGL_ASSERT(succeeded, "VkSamplerManager initialization failed.");
        succeeded = InitializeBlitResources();
        MOBILEGL_ASSERT(succeeded, "Blit pipeline resource initialization failed.");
        succeeded = InitializeDepthMipmapResources();
        MOBILEGL_ASSERT(succeeded, "Depth mipmap pipeline resource initialization failed.");

        m_uniformManager = MakeUnique<UniformManager>();
        MOBILEGL_ASSERT(m_uniformManager != nullptr, "UniformDescriptorBinder creation failed.");
        succeeded = m_uniformManager->Initialize(
            m_device, m_physicalDevice.handle, &m_bufferManager, m_programFactory.get(),
            m_physicalDevice.properties.limits.minUniformBufferOffsetAlignment, m_config.MaxFramesInFlight,
            maxProgramBindings, kDescriptorSetsPerFrame, m_textureManager.get(), m_samplerManager.get());
        MOBILEGL_ASSERT(succeeded, "UniformDescriptorBinder initialization failed.");
        m_uniformManager->SetWireInvalidStorageImageArm(m_wireNullDescriptor);
        m_vertexInputStateFactory =
            MakeUnique<VertexInputStateFactory>(m_config, m_physicalDevice.handle);
        MOBILEGL_ASSERT(m_vertexInputStateFactory != nullptr, "VertexInputStateFactory creation failed.");

        // Prime the first frame so Render() always targets an acquired swapchain image.
        // A zero-area window (GLFW's hidden helper window during the WGL bootstrap, or a
        // window that is already minimized) legitimately yields no swapchain here; defer
        // the first acquire to Present in that case instead of acquiring from a null
        // swapchain handle.
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE) {
            VkResult acquireResult =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
            if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
                // Nothing was acquired and no semaphore signal was armed, so
                // rebuilding and re-acquiring on the same semaphore is safe.
                MGLOG_D("Initialize, vkAcquireNextImageKHR got %d, recreating swapchain", acquireResult);
                RecreateSwapchain();
                acquireResult =
                    m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
            } else if (acquireResult == VK_SUBOPTIMAL_KHR) {
                // The image is usable, and its acquire signal is already armed on
                // imageAvailableSemaphore. Re-acquiring here would arm a second signal on a
                // binary semaphore whose first one nobody has waited on yet; keep the image.
                // Only a real surface change schedules a rebuild.
                m_swapchainResizeRequested = m_swapchainResizeRequested || SwapchainIsOutOfDate();
                acquireResult = VK_SUCCESS;
            }
            VK_VERIFY(acquireResult, "Initialize, WaitAndAcquireNextImage");
            // The first acquired image is the one the opening frame renders into, so the
            // default framebuffer addresses it from the start (see
            // m_defaultFramebufferImageIndex).
            m_defaultFramebufferImageIndex = m_imageIndexAcquired;
        } else {
            MGLOG_W("DirectVulkan: no swapchain at initialization (zero-area window); deferring first acquire");
        }
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());

        MGLOG_D("VulkanRenderer initialized");
    }

    void VulkanRenderer::Shutdown() {
        if (m_instance == VK_NULL_HANDLE && m_device == VK_NULL_HANDLE && m_surface == VK_NULL_HANDLE) {
            return;
        }

        if (m_device != VK_NULL_HANDLE) {
            VK_VERIFY(vkDeviceWaitIdle(m_device));
        }
        m_progressMarkers.Shutdown();
        // Parked surface targets (ActivateSurfaceTarget) go before the device and instance.
        for (auto& [_, target] : m_parkedTargets) DestroyParkedTarget(target);
        m_parkedTargets.clear();
        // P7 wave 2 package B3: the session's decline tally. Silent when nothing declined, so a
        // green lane stays quiet and a lane that dropped a draw cannot (rule I's observable).
        WireDeclineTally::Dump("shutdown");
        // The SPIR-V screen's engagement proof: a decline-free session above reads as "screened
        // and valid" only with this count beside it (distinct modules, process-wide).
        MGLOG_I("MGWIRE-SPIRV-SCREEN[shutdown] modulesValidated=%llu",
                static_cast<unsigned long long>(ProgramFactory::DriverModuleValidatorRuns()));
        DestroyWireDrawPass();
        CollectWireObjects(m_submitCounter, true);
        ClearAllWireDrawPassCaches();
        DestroyWireColorBlitResources();
#if MOBILEGL_BUILD_DISAGGREGATED
        DestroyWireYuvResources();
#endif
        DestroyWireDepthMipmapResources();
        DestroyWireMultisampleResolveResources();
#if MOBILEGL_BUILD_DISAGGREGATED
        DestroySharedImagePresentTargets(false);
        DestroySharedImageSync();
#endif
        OnSubmitsCompletedUpTo(m_submitCounter);
        DestroySubmitFencePool();

        DestroyDeferredDepthMipmapCleanup();
        DestroyMultisampleResolveScratchImage();
        DestroyComputePipelines();

        // No sweep runs during teardown, but the observers point at this renderer
        // and the factories die at different times below; disconnect them first.
        if (m_renderPassManager) {
            m_renderPassManager->SetEvictionObserver(nullptr);
        }
        if (m_programFactory) {
            m_programFactory->SetEvictionObserver(nullptr);
        }

        m_pipelineFactory.reset();
        ShutdownBlitResources();
        ShutdownDepthMipmapResources();
        if (m_samplerManager) {
            m_samplerManager->Shutdown();
            m_samplerManager.reset();
        }
        if (m_textureManager) {
            m_textureManager->Shutdown();
            m_textureManager.reset();
        }
        m_vertexInputStateFactory.reset();
        m_xfbCounterBuffer.Destroy();
        m_xfbCounterSlotOwner.fill(0);
        m_xfbCounterSlotLastUse.fill(0);
        m_xfbCounterSlotUseSerial = 0;
        m_xfbCountersValid.fill(false);
        m_xfbLastSeenGeneration.fill(0);
        if (m_occlusionQueryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_occlusionQueryPool, nullptr);
            m_occlusionQueryPool = VK_NULL_HANDLE;
        }
        if (m_xfbQueryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_xfbQueryPool, nullptr);
            m_xfbQueryPool = VK_NULL_HANDLE;
        }
        if (m_primGenReroutePool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_primGenReroutePool, nullptr);
            m_primGenReroutePool = VK_NULL_HANDLE;
        }
        m_primGenRerouteActiveSlots.clear();
        m_primGenRerouteSlotCursor = 0;
        m_primGenRerouteSlotOpen = false;
        // Not sticky across renderers: the next bring-up re-decides both (from the
        // per-process probe memo, so it re-decides without re-probing).
        m_primGenRerouteKind = MG_Util::SelfTest::PrimGenRerouteKind::None;
        m_primGenStreamCountsXfbInactiveDraws = false;
        m_bufferManager.Shutdown();

        // Device is idle (vkDeviceWaitIdle above); query pools can be destroyed.
        m_frameContext.SetRecordingObserver(nullptr);
        if (m_timerQueryManager) {
            m_timerQueryManager->Shutdown();
            m_timerQueryManager.reset();
        }

        if (m_device != VK_NULL_HANDLE) {
            m_frameContext.Destroy(m_device, m_commandPool);
        }

        if (m_uniformManager) {
            m_uniformManager->Shutdown();
            m_uniformManager.reset();
        }
        m_programFactory.reset();

        if (m_renderPassManager) {
            ShutdownSwapchain();
        } else if (m_device != VK_NULL_HANDLE) {
            m_swapchainObject.Shutdown(m_device);
        }
        m_renderPassManager.reset();
        if (m_clearManager) {
            m_clearManager->Shutdown();
            m_clearManager.reset();
        }
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }

        DestroyAllocator();

        if (m_device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        s_vkCmdDrawIndexedIndirectCount = nullptr;
        s_vkCmdWireDrawIndirectCount = nullptr;
        s_vkCmdDrawMultiEXT = nullptr;
        s_vkCmdDrawMultiIndexedEXT = nullptr;

        if (m_instance != VK_NULL_HANDLE && m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }

#if defined(VK_USE_PLATFORM_METAL_EXT)
        if (m_platformLibrary != nullptr) {
            Release(reinterpret_cast<id>(m_platformLibrary));
            m_platformLibrary = nullptr;
        }
        if (m_platformDisplay != nullptr) {
            Release(reinterpret_cast<id>(m_platformDisplay));
            m_platformDisplay = nullptr;
        }
#endif

#if defined(VK_USE_PLATFORM_XLIB_KHR)
        if (m_platformDisplay != nullptr) {
            // No fallback window to destroy any more: the display here is only ever
            // one this renderer opened for a REAL window surface, and that window is
            // the caller's to own. The hidden-window pbuffer fallback that used to be
            // cleaned up here is gone (see CreateSurface).
            using XCloseDisplayFn = int (*)(Display*);
            auto* closeDisplay = reinterpret_cast<XCloseDisplayFn>(m_platformCloseDisplay);
            if (closeDisplay) {
                closeDisplay(static_cast<Display*>(m_platformDisplay));
            }
            m_platformDisplay = nullptr;
        }
        m_platformCloseDisplay = nullptr;
        if (m_platformLibrary != nullptr) {
            dlclose(m_platformLibrary);
            m_platformLibrary = nullptr;
        }
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        // The AImageReader owns the ANativeWindow the pbuffer fallback handed to the
        // WSI, so it outlives the surface and is released only here.
        if (m_fallbackImageReader != nullptr && m_platformLibrary != nullptr) {
            using AImageReaderDeleteFn = void (*)(void*);
            auto* imageReaderDelete =
                reinterpret_cast<AImageReaderDeleteFn>(dlsym(m_platformLibrary, "AImageReader_delete"));
            if (imageReaderDelete) {
                imageReaderDelete(m_fallbackImageReader);
            }
            m_fallbackImageReader = nullptr;
            m_window = 0;
            dlclose(m_platformLibrary);
            m_platformLibrary = nullptr;
        }
#endif

        if (m_debugMessenger != VK_NULL_HANDLE) {
            DestroyDebugMessenger();
            m_debugMessenger = VK_NULL_HANDLE;
        }
        DestroyDebugReportCallback();

        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
        MGLOG_I("VulkanRenderer shut down completed");
    }

    namespace {
    } // namespace

    Bool VulkanRenderer::InitializeBlitResources() {
        // P5f fv gave this an early return for every non-monolith transport, because the hidden
        // programs are frontend objects and creating them on the server would retain a client
        // compiler/allocator dependency. P7 wave 2-B2 (CONTRACT-P7 §5.2, (B')) takes the last
        // step: in a disaggregated build there is NOTHING TO INITIALIZE on any transport,
        // because the monolith arm now blits with WireColorBlit.inc's baked module too.
        //
        // AN `#if` AND NOT A RUNTIME BRANCH, which is the whole point (§4.2): the early return
        // left every ShaderObject / ProgramObject / SamplerObject symbol REFERENCED by this
        // object file, so the link-closure ratchet counted them whether or not the branch ever
        // ran. Compiling the construction out is what makes them fall.
        return true;
    }

    void VulkanRenderer::ShutdownBlitResources() {
        // P7 wave 2-B2: TWO OF P5e RULING 12'S FOUR APPLY-THREAD ALLOCATOR DEBTS ARE GONE, and
        // the scope that named them with them. The debt was real when it was written: the hidden
        // blit program and its samplers are FRONTEND objects, and under an active transport the
        // server backend created them on the apply thread and destroyed them here on it, where
        // their destructors run the client death helper and its lifetime-id probe.
        //
        // What retires it is not a new exemption but the absence of the objects. P5f fv gave
        // InitializeBlitResources an early return for every non-monolith transport, so off the
        // monolith arm `m_blitResources` never holds anything and this assignment destroys
        // nothing - MEASURED, not argued: a temporary probe here and in
        // ShutdownDepthMipmapResources failed the session if either resource was non-null under
        // a non-monolith transport, and integration-magma-{split,spawn,tcp} (93/72/74),
        // integration-magma-full-{split,spawn} (524/524), integration-split (187) and
        // integration-magma-buffers (21) all stayed green with it armed. Dropping its transport
        // test killed EVERY monolith `DirectVulkan.` entry at teardown and left the split
        // entries green, which is what makes the first run evidence rather than an absence.
        //
        // The TYPE stays: MG_Test/Wire/RemoteClientTest's RemoteGuards
        // .BarrieredLegacyScopesCannotExemptAllocator and
        // .BarrieredFrontendRegistryMembersRefuseBothLegacyScopes use it as a NEGATIVE CONTROL -
        // they construct it on the apply thread and assert the allocator still refuses, which is
        // the thing P5f (fr) changed and the only remaining reason for it to exist.
        m_blitResources = {};
    }

    Bool VulkanRenderer::InitializeDepthMipmapResources() {
        // P7 wave 2-B2 (CONTRACT-P7 §5.2, (B')): the depth-mip half of the same step. In a
        // disaggregated build the monolith arm generates its depth chain with
        // WireDepthMipmap.inc's baked pass, so there is no hidden GL program to build on any
        // transport. See InitializeBlitResources above for why this is an `#if`.
        return true;
    }

    void VulkanRenderer::ShutdownDepthMipmapResources() {
        // P7 wave 2-B2: the second of the pair, retired for ShutdownBlitResources's reason and
        // proved by the same probe. InitializeDepthMipmapResources carries the same non-monolith
        // early return, so off the monolith arm this assignment destroys nothing.
        m_depthMipmapResources = {};
    }

    void VulkanRenderer::CollectDeferredDepthMipmapCleanup(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_deferredDepthMipmapCleanup.size(),
                        "CollectDeferredDepthMipmapCleanup: frame index %u out of range (size=%zu)",
                        frameIndex, m_deferredDepthMipmapCleanup.size());
        if (m_device == VK_NULL_HANDLE) {
            return;
        }

        auto& cleanup = m_deferredDepthMipmapCleanup[frameIndex];
        for (auto framebuffer : cleanup.framebuffers) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(m_device, framebuffer, nullptr);
            }
        }
        for (auto pipeline : cleanup.pipelines) {
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, pipeline, nullptr);
            }
        }
        for (auto renderPass : cleanup.renderPasses) {
            if (renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(m_device, renderPass, nullptr);
            }
        }
        for (auto imageView : cleanup.imageViews) {
            if (imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, imageView, nullptr);
            }
        }

        cleanup.framebuffers.clear();
        cleanup.pipelines.clear();
        cleanup.renderPasses.clear();
        cleanup.imageViews.clear();
    }

    void VulkanRenderer::DestroyDeferredDepthMipmapCleanup() {
        for (Uint32 frameIndex = 0; frameIndex < m_deferredDepthMipmapCleanup.size(); ++frameIndex) {
            CollectDeferredDepthMipmapCleanup(frameIndex);
        }
        m_deferredDepthMipmapCleanup.clear();
    }


    // Value hash over every fixed-function GL state the pipeline payload reads that
    // the memo key's other fields (mode, program hash, vertex-input hash, render-pass
    // hash, transform flags) do not already pin down. Enumerated against the payload
    // build in GetOrCreatePipeline - any new GL-state read there must be added here:
    //   - capability bits: CullFace, DepthTest, PolygonOffsetFill (mode gating rides
    //     the memo's mode key), RasterizerDiscard, ColorLogicOp, StencilTest,
    //     PrimitiveRestart(+FixedIndex), SampleShading, SampleMask, plus the depth write mask
    //   - patch vertices, polygon mode, cull face mode, depth func, logic op,
    //     min sample shading, the glSampleMaski word
    //   - front/back stencil ops + compare funcs (ref/mask are dynamic state)
    //   - per draw buffer up to the render pass's colour span: indexed blend enable,
    //     blend factors/equations, indexed colour write mask (broadcast from index 0
    //     when the device lacks independentBlend - the same read the payload does)
    // FBO-derived payload inputs (attachment presence/formats/draw-buffer gating) are
    // pinned by the render-pass hash key, exactly as the version-keyed memo relied on.
    // The fixed-function sample mask this draw actually gets, and the ONE place that decides it.
    //
    // GL 4.6 core 17.3.3 puts SAMPLE_MASK/SAMPLE_MASK_VALUE among the multisample fragment
    // operations and says they make no change "if MULTISAMPLE is disabled, or if the value of
    // SAMPLE_BUFFERS is not one" - so on a single-sample draw framebuffer the mask is a no-op.
    // Vulkan has no such rule: pSampleMask is ANDed with coverage at every rasterizationSamples,
    // and at one sample that coverage is bit 0 alone. Handing the raw GL word straight through
    // therefore turned `glEnable(GL_SAMPLE_MASK); glSampleMaski(0, 0x2);` followed by a draw to
    // the default framebuffer - the ordinary MSAA-render-then-present shape, and what dEQP's
    // multisample cases leave enabled - into a fully discarded, black draw. All-ones restores
    // the null-pSampleMask meaning the pipeline had before the mask was plumbed at all.
    //
    // SAMPLE_BUFFERS is the load-bearing half: MultisampleEnabled defaults to TRUE, so the
    // capability check alone would gate nothing. It is here for spec completeness - GL lets
    // glDisable(GL_MULTISAMPLE) switch the whole step off on a multisample target too.
    //
    // Both callers - the payload and ComputePipelineStateHash's memo word - go through this, so
    // the memo key cannot describe a different mask than the pipeline was built with.
    Uint32 VulkanRenderer::ResolveEffectiveSampleMask(VkSampleCountFlagBits rasterizationSamples) const {
        constexpr Uint32 kFullCoverage = 0xffffffffu;
        if (rasterizationSamples == VK_SAMPLE_COUNT_1_BIT) return kFullCoverage;
        if (!MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::Multisample)) return kFullCoverage;
        if (!MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::SampleMask)) return kFullCoverage;
        return MG_Pipe::gPipeInputs.GetRenderStateParameters().SampleMaskValue;
    }


    Uint64 VulkanRenderer::ComputePipelineSubsetStateHashFallback() const {
        // The client's own hash, over the client's own definition of the pipeline subset - the
        // seven pipeline chunks of the P2 chunk table, which is a strict SUPERSET of what
        // ComputePipelineStateHash enumerated by hand. The render-pass facts it does not carry
        // (colorAttachmentCount, the rasterization sample count, and through them the effective
        // sample mask) are exactly the facts entry.renderPassHash separates, which is why the
        // CSO handle can key this memo in the first place; this fallback inherits that argument
        // unchanged.
        //
        // Only reached with no render-state CSO bound, and only in a build with no pre-handle
        // arm to fall back to instead.
        return MG_Pipe::MGPipeComputePipelineSubsetHash(MG_Pipe::gPipeInputs.GetRenderStateParameters());
    }

    // A program that runs a geometry shader AND captures transform feedback. Both halves are
    // link-time properties, so this is safe to fold into a pipeline keyed on the program hash.
    static Bool ProgramCapturesXfbFromGeometryStage(const MagmaProgramSource& program) {
        if (program.GetTransformFeedbackVaryingCount() == 0) return false;
        // Both halves are link-time properties, so both are asked of the LAST LINK. Reading the
        // live attach list would let a glAttachShader that has not been linked in yet - which GL
        // 4.6 core 7.3 says changes nothing about what the program runs - flip a property this
        // pipeline is cached under, for an executable with no geometry stage in it.
        return program.HasLinkedShaderStage(ShaderStage::Geometry);
    }

    // GL primitive restart is defined on the INDEX STREAM (GL 4.6 core 10.3.6): it splits
    // primitives when a fetched index matches PRIMITIVE_RESTART_INDEX. Two consequences the
    // capability bits alone cannot express, both resolved here because only the caller knows them:
    //
    //  - A non-indexed draw has no index stream, so restart is a no-op for it. Leaving the
    //    pipeline's primitiveRestartEnable on for a glDrawArrays is what made the list-topology
    //    guard below refuse those draws, so an application that enables GL_PRIMITIVE_RESTART once
    //    at init lost every glDrawArrays on a device without the extension.
    //  - The comparison is against the full 32-bit restart index with the fetched index
    //    zero-extended, so a restart index the type cannot hold (0x100FF against UNSIGNED_BYTE
    //    data) matches no index and that draw restarts NOWHERE. UploadAndBindIndexBuffer makes the
    //    same call for the rewrite, and the two must agree or the pipeline says "restart" over
    //    index data nothing rewrote.
    Bool VulkanRenderer::ResolvePrimitiveRestartEnable(Flags<DrawSetupAspect> aspects,
                                                       const IndexBufferView* pIndexBufferView) const {
        if (!(aspects & DrawSetupAspect::IndexBuffer) || pIndexBufferView == nullptr) {
            return false;
        }
        const RenderStateParameters& rsp = MG_Pipe::gPipeInputs.GetRenderStateParameters();
        if (rsp.PrimitiveRestartFixedIndexEnabled) {
            return true;
        }
        if (!rsp.PrimitiveRestartEnabled) {
            return false;
        }
        return rsp.PrimitiveRestartIndex <= MG_Util::FixedRestartIndexForGLType(pIndexBufferView->indexType);
    }

    // MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS - milliseconds to spend in a pipeline-creation MISS
    // on the wire arm. See the call site below for why it exists: the device's divergence needs a
    // COLD driver pipeline cache, and this host's is always warm, so the only way to reproduce
    // what a cold cache costs is to buy the time. Read once; inert unless set; compiled only into
    // the disaggregated build, so the monolith image G1 pins never carries it.
    static Uint32 MagmaTestPipelineCreateDelayMs() {
        static const Uint32 milliseconds = [] {
            const char* value = std::getenv("MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS");
            if (!value || !*value) return 0u;
            return static_cast<Uint32>(std::strtoul(value, nullptr, 10));
        }();
        return milliseconds;
    }

    VkPipeline VulkanRenderer::GetOrCreatePipelineWithInput(GLenum mode, const MagmaProgramSource& program,
            const ProgramFactory::VkProgramObject& programObj, ProgramFactory::CompileOptionFlags transformFlags,
            const VertexInputStateFactory::BackendVertexInputState& vis, const RenderPassEntry& renderPassEntry,
            Bool primitiveRestartEnable, Uint64 wireRenderPassCompatibilityId) {
        Bool invertClockwise = transformFlags & ProgramFactory::CompileOptionBit::PositionYFlip;
        if (programObj.stages.empty()) {
            MGLOG_D("GetOrCreatePipeline skipped: program has no shader stages");
            return VK_NULL_HANDLE;
        }
        // Fast path: skip the full pipeline resolution when the pipeline state is unchanged from the
        // previous draw (the common intra-batch case). The key provably covers every
        // PipelineCreatePayload field: draw mode (topology + polygon-fill depth-bias gate), program
        // content hash (folds program identity + link version + transform flags + shader stages),
        // vertex-input hash (VAO layout), render-pass hash (render targets + the draw-buffer/format
        // driven blend & write-mask gating), and the pipeline-state value hash (all fixed-function state).
        // Reset per-frame and on pipeline destruction so a memoized handle can never dangle.
        // The identity hash mixes each bound buffer's never-reused lifetime id
        // (per-chunk VBOs mint a new one per buffer); the memo and the pipeline
        // payload key on the resolved LAYOUT hash instead, so draws over identical
        // layouts share one pipeline.
        // The one-arg fetch rides the VAO's state-pointer memo (no hash, no map).
        const Uint64 vertexLayoutHash = vis.layoutHash;
        const Uint64 renderPassHash =
            wireRenderPassCompatibilityId != 0 ? 0 :
            renderPassEntry.hash;
        // The pipeline-relevant subset only: glViewport / glScissor / glBlendColor / glStencilMask
        // and friends are dynamic state or not pipeline state at all, and keying the memo on the
        // all-state counter made any of them evict a perfectly good VkPipeline. The memo compares
        // the VALUE hash of that subset, never the version itself: the version is monotonic, so
        // per-draw state flips (GL_BLEND toggles) would otherwise miss entries the memo holds.
        // The version only guards recomputing the hash - unchanged version, unchanged bytes.
        const Uint renderStateVersion = MG_Pipe::gPipeInputs.GetPipelineStateVersion();
        // P2 D12.1. Non-null means the client's render-state CSO handle is this draw's state
        // key and the hash below is not computed at all; null means the pre-handle arm. The
        // two arms' entries can never match each other: the handle arm stores hash 0 and a
        // real handle, the legacy arm a real hash and the null handle, and the probe compares
        // both components.
        const MG_Pipe::MGPipeHandle renderStateCso = ResolveBoundRenderStateCso();
        // Exactly one of the two state keys is live per draw, and the ternary short-circuits,
        // so a draw on the handle arm neither hashes nor touches the fallback cache.
        const Uint64 pipelineStateHash =
            !MG_Pipe::MGPipeHandleIsNull(renderStateCso)
                ? 0
                : ResolveFallbackPipelineStateHash(renderStateVersion,
                                                   renderPassEntry.colorAttachmentCount,
                                                   renderPassEntry.sampleCount);
        for (Uint32 i = 0; i < m_pipelineMemoCount; ++i) {
            const PipelineMemoEntry& entry = m_pipelineMemo[i];
            if (entry.pipeline != VK_NULL_HANDLE && entry.mode == mode &&
                entry.programHash == programObj.hash && entry.vertexInputHash == vertexLayoutHash &&
                entry.renderPassHash == renderPassHash &&
                entry.wireRenderPassCompatibilityId == wireRenderPassCompatibilityId &&
                entry.pipelineStateHash == pipelineStateHash &&
                entry.renderStateCso == renderStateCso &&
                entry.primitiveRestartEnable == primitiveRestartEnable &&
                entry.transformFlags == transformFlags) {
                if (MG_Util::PipeStats::Enabled()) {
                    // Gate 5 of section 2.3.1. On a hit this whole function cost the one
                    // GetPipelineStateVersion read above plus this value compare.
                    MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::MagmaPipelineMemo, /*hit=*/true);
                    MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::AccessorCalls, 1);
                }
                return entry.pipeline;
            }
        }
        if (MG_Util::PipeStats::Enabled()) {
            MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::MagmaPipelineMemo, /*hit=*/false);
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::AccessorCalls, 1);
        }

        // Shape gate. Behind the memo probe deliberately: only a pipeline that was created
        // successfully is ever memoized, so a program refused here can never be sitting in the
        // memo, and the steady-state draw keeps paying nothing for the check.
        //
        // vkCreateGraphicsPipelines is not a validating entry point: a stage set that a
        // conformant implementation would reject with VK_ERROR_* is, on Adreno 830, a SIGSEGV
        // inside the driver - process death instead of a failed draw. The separable-program path
        // is what made these shapes reachable at all (a monolithic glUseProgram program cannot
        // hold a compute stage together with graphics ones, a pipeline object can), so the three
        // it can produce are named and refused here. Same philosophy as the VK_NULL_HANDLE gate
        // in SetupDraw: hostile input degrades to a broken draw, never to a dead process. GL
        // leaves all three undefined for a draw, so nothing legal is being turned away.
        // MGLOG_E, latched: a refused program is never memoized, so the refusal is re-derived
        // on every draw that uses it. Parked at MGLOG_I until the Log.h ordering was fixed.
        {
            Bool hasVertexStage = false;
            for (const auto& stage : programObj.stages) {
                if (stage.module == VK_NULL_HANDLE) {
                    MGLOG_E_ONCE("GetOrCreatePipeline skipped: program=%u has a null shader module for stage 0x%x",
                            program.GetExternalIndex(), static_cast<unsigned>(stage.stage));
                    return VK_NULL_HANDLE;
                }
                if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT) {
                    MGLOG_E_ONCE("GetOrCreatePipeline skipped: program=%u carries a compute stage, which no graphics "
                            "pipeline may contain",
                            program.GetExternalIndex());
                    return VK_NULL_HANDLE;
                }
                if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
                    hasVertexStage = true;
                }
            }
            if (!hasVertexStage) {
                MGLOG_E_ONCE("GetOrCreatePipeline skipped: program=%u has no vertex stage", program.GetExternalIndex());
                return VK_NULL_HANDLE;
            }
        }

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        const auto& limits = m_physicalDevice.properties.limits;
        if (programObj.fragmentInputComponentCount != 0) {
            MOBILEGL_ASSERT(
                programObj.fragmentInputComponentCount <= limits.maxFragmentInputComponents,
                "GetOrCreatePipeline: fragmentInputComponents=%u exceeds device limit=%u program=%u producerStage=%d",
                programObj.fragmentInputComponentCount,
                limits.maxFragmentInputComponents,
                program.GetExternalIndex(),
                static_cast<Int>(programObj.rasterizationProducerStage));
        }
        if (programObj.producerOutputComponentCount != 0) {
            Uint32 producerOutputLimit = 0;
            switch (programObj.rasterizationProducerStage) {
            case ShaderStage::Vertex:
                producerOutputLimit = limits.maxVertexOutputComponents;
                break;
            case ShaderStage::Geometry:
                producerOutputLimit = limits.maxGeometryOutputComponents;
                break;
            case ShaderStage::TessEval:
                producerOutputLimit = limits.maxTessellationEvaluationOutputComponents;
                break;
            default:
                break;
            }
            if (producerOutputLimit != 0) {
                MOBILEGL_ASSERT(
                    programObj.producerOutputComponentCount <= producerOutputLimit,
                    "GetOrCreatePipeline: producerOutputComponents=%u exceeds stage limit=%u program=%u producerStage=%d",
                    programObj.producerOutputComponentCount,
                    producerOutputLimit,
                    program.GetExternalIndex(),
                    static_cast<Int>(programObj.rasterizationProducerStage));
            }
        }
#endif

        const Uint32 vertexInputAttribMask = vis.attributeLocationMask;
        const Uint32 activeAttribMask = programObj.activeVertexInputLocationMask;
        const Uint32 missingAttribMask = activeAttribMask & ~vertexInputAttribMask;
        auto& patchedAttributes = m_patchedAttributesScratch;
        patchedAttributes.assign(vis.attributes.begin(), vis.attributes.end());
        Bool hasPatchedVertexAttributes = false;
        for (auto& attribute : patchedAttributes) {
            if (attribute.location >= kMaxVertexAttribs || (activeAttribMask & (1u << attribute.location)) == 0) {
                continue;
            }

            const GLenum shaderInputType = programObj.vertexInputTypes[attribute.location];
            const NumericDomain shaderInputDomain = GetNumericDomainForShaderValueType(shaderInputType);
            const NumericDomain vertexInputDomain = GetNumericDomainForVertexFormat(attribute.format);
            if (shaderInputDomain == NumericDomain::Unknown || vertexInputDomain == NumericDomain::Unknown ||
                shaderInputDomain == vertexInputDomain) {
                continue;
            }

            VkFormat patchedFormat = VK_FORMAT_UNDEFINED;
            const Bool canPatch = TryCoerceVertexFormatNumericDomain(attribute.format, shaderInputDomain, patchedFormat);
            MOBILEGL_ASSERT(
                canPatch,
                "GetOrCreatePipeline: vertex input location=%u format=%d mismatches shader input type=%u program=%u",
                attribute.location,
                static_cast<Int>(attribute.format),
                static_cast<Uint32>(shaderInputType),
                program.GetExternalIndex());

            MGLOG_W_ONCE("GetOrCreatePipeline: patching vertex input location=%u format=%d -> %d to match shader input type=%u for program=%u",
                    attribute.location,
                    static_cast<Int>(attribute.format),
                    static_cast<Int>(patchedFormat),
                    static_cast<Uint32>(shaderInputType),
                    program.GetExternalIndex());
            attribute.format = patchedFormat;
            hasPatchedVertexAttributes = true;
        }
        VertexInputStateBuilder syntheticVertexInputBuilder;
        VkPipelineVertexInputStateCreateInfo syntheticVertexInputState{};
        const VkPipelineVertexInputStateCreateInfo* pipelineVertexInputState = &vis.state;
        if (missingAttribMask != 0 || hasPatchedVertexAttributes) {
            for (const auto& binding : vis.bindings) {
                syntheticVertexInputBuilder.AddBinding(binding.binding, binding.stride, binding.inputRate);
            }
            for (const auto& attribute : patchedAttributes) {
                syntheticVertexInputBuilder.AddAttribute(attribute.location, attribute.binding, attribute.format,
                                                         attribute.offset);
            }

            Uint32 syntheticBinding = static_cast<Uint32>(vis.bindings.size());
            for (Uint32 location = 0; location < kMaxVertexAttribs; ++location) {
                if ((missingAttribMask & (1u << location)) == 0) {
                    continue;
                }

                VkFormat format = VK_FORMAT_UNDEFINED;
                const Bool supported = TryGetCurrentVertexAttributeFormat(programObj.vertexInputTypes[location], format);
                MOBILEGL_ASSERT(supported,
                                "DirectVulkan does not support current generic vertex attribute type yet: program=%u location=%u type=0x%x activeAttribMask=0x%x vertexInputAttribMask=0x%x",
                                program.GetExternalIndex(), location, programObj.vertexInputTypes[location],
                                activeAttribMask, vertexInputAttribMask);

                syntheticVertexInputBuilder.AddBinding(syntheticBinding, 0, VK_VERTEX_INPUT_RATE_VERTEX);
                syntheticVertexInputBuilder.AddAttribute(location, syntheticBinding, format, 0);
                ++syntheticBinding;
            }
            syntheticVertexInputState = syntheticVertexInputBuilder.Build();
            // Carry the divisor chain over. The synthetic rebuild copies bindings and
            // attributes only, and it keeps every real binding's INDEX, so the divisor
            // descriptions built for them stay valid - but dropping the pNext silently
            // demoted every instanced binding to divisor 1. This path runs whenever the
            // program declares an input the VAO does not feed (which is most capture
            // shaders: KHR-GL43.vertex_attrib_binding declares 16 inputs and enables three),
            // so the loss was near-total rather than a corner case.
            syntheticVertexInputState.pNext = vis.state.pNext;
            pipelineVertexInputState = &syntheticVertexInputState;
        }
        auto cullFaceEnabled = MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::CullFace);
        auto depthTestEnabled = MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::DepthTest);
        auto polygonOffsetFillEnabled =
            MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::PolygonOffsetFill) &&
            DrawModeUsesPolygonFill(mode);
        auto rasterizerDiscardEnabled =
            MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::RasterizerDiscard);
        auto colorLogicOpEnabled =
            MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::ColorLogicOp) && m_logicOpFeatureEnabled;
        auto stencilTestEnabled = MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::StencilTest);
        // A framebuffer without a depth (stencil) attachment behaves as if the depth
        // (stencil) test always passes and nothing is written - even when the bound
        // image is a packed depth-stencil texture attached through only one half.
        {
            const auto* fbo = MG_Pipe::MGPipeApplier().DrawFramebuffer();
            if (fbo && !fbo->IsDefault) {
                if (fbo->Depth.Kind == MG_Pipe::kMGPipeSurfaceKindNone) depthTestEnabled = false;
                if (fbo->Stencil.Kind == MG_Pipe::kMGPipeSurfaceKindNone) stencilTestEnabled = false;
            }
        }
        const StencilFaceState& frontStencil = MG_Pipe::gPipeInputs.GetStencilState(StencilFace::Front);
        const StencilFaceState& backStencil = MG_Pipe::gPipeInputs.GetStencilState(StencilFace::Back);
        const VkPolygonMode requestedPolygonMode =
            MG_Util::ConvertPolygonModeToVkEnum(MG_Pipe::gPipeInputs.GetPolygonModeFront());
        // VK_POLYGON_MODE_LINE/_POINT require the fillModeNonSolid device feature; fall back to
        // VK_POLYGON_MODE_FILL when the device lacks it.
        const VkPolygonMode effectivePolygonMode =
            (requestedPolygonMode == VK_POLYGON_MODE_FILL || m_fillModeNonSolidFeatureEnabled)
                ? requestedPolygonMode
                : VK_POLYGON_MODE_FILL;

        const VkPrimitiveTopology vkTopology = MG_Util::ConvertPrimitiveModeToVkEnum(mode);
        // Resolved by the caller (ResolvePrimitiveRestartEnable), which knows whether the draw is
        // indexed and with what index type; the capability bits alone answer neither.
        Bool primitiveRestartEnabled = primitiveRestartEnable;

        // GL applies restart to PATCHES only when PRIMITIVE_RESTART_FOR_PATCHES_SUPPORTED is true
        // (GL 4.6 core 10.3.6). MobileGL supports no such thing - neither backend has a way to
        // restart a patch stream - and GL_FALSE is a legal answer to that query, so a patch draw
        // simply never restarts here. Doing this BEFORE the feature guard below is what keeps a
        // perfectly ordinary GL_PATCHES draw from being refused on a device that lacks
        // VK_EXT_primitive_topology_list_restart. (When GL_PRIMITIVE_RESTART_FOR_PATCHES_SUPPORTED
        // is eventually added to glGetIntegerv it has to report GL_FALSE to stay consistent with
        // this.)
        if (vkTopology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {
            primitiveRestartEnabled = false;
        }

        // Primitive restart on a *list* topology requires the primitiveTopologyListRestart feature;
        // strip/fan restart works without it. There is no fallback - silently dropping the restarts
        // would weld the primitives on either side of each one together - so the draw is declined
        // here with the reason.
        //
        // Declined, not thrown. This used to THROW_EXCEPTION, which unwinds a C++ exception through
        // the C GL ABI and takes the process down (the hazard GL_Texture.cpp and RenderState.cpp
        // already name); an application that merely enabled a legal desktop feature died instead of
        // getting a draw that rendered nothing. VK_NULL_HANDLE is this function's established
        // "skip this draw" answer, used by the no-stages case above.
        //
        // Reached only when this draw's index stream really does restart. Testing the raw
        // capability bits here instead - which is what it did - refused every NON-INDEXED
        // list-topology draw as well, so an application that enables GL_PRIMITIVE_RESTART once at
        // init and then calls glDrawArrays(GL_TRIANGLES, ...) rendered nothing at all.
        const auto isListTopology = [](VkPrimitiveTopology t) {
            return t == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                   t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                   t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
                   t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        };
        if (primitiveRestartEnabled && !m_primitiveTopologyListRestartFeatureEnabled && isListTopology(vkTopology)) {
            MGLOG_E_ONCE("Draw skipped: primitive restart on a list topology (0x%x) requires the "
                         "primitiveTopologyListRestart device feature (VK_EXT_primitive_topology_list_restart), "
                         "which this device does not support; use a strip/fan topology, or disable primitive "
                         "restart for list-topology draws.",
                         mode);
            return VK_NULL_HANDLE;
        }

        if (MG_Util::PipeStats::Enabled()) {
            // THE payload-builder walk section 2.3.1 says only runs on a pipeline memo miss.
            // Counted as a constant, and counted HERE rather than at the top of the walk:
            // the list-topology primitive-restart refusal above returns VK_NULL_HANDLE after
            // only ten of these reads have run, and a tally that fires before an early return
            // is an OVER-count, which breaks the lower-bound contract every other tally keeps.
            //
            // The 15 are: the six capability reads (cull face, depth test, polygon offset
            // fill, rasterizer discard, colour logic op, stencil test), the draw-FBO slot
            // read that gates depth/stencil, the two stencil face states, the polygon mode,
            // the min sample shading value, the patch vertex count, the depth mask, the depth
            // func, and the second draw-FBO slot read below. The sample-shading CAPABILITY
            // read is the one excluded: it sits behind && on m_sampleRateShadingFeatureEnabled
            // and does not run on a device without the feature. The other conditional reads -
            // the cull-mode ternary, the logic-op fetch, the two tessellation default-level
            // reads - are excluded for the same reason, so this stays a LOWER bound.
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::AccessorCalls, 15);
        }
        PipelineFactory::PipelineCreatePayload payload {
            .programHash = programObj.hash,
            .vertexInputHash = vertexLayoutHash,
            .pipelineLayout = programObj.pipelineLayout,
            .renderPass = renderPassEntry.renderPass,
            .wireRenderPassCompatibilityId = wireRenderPassCompatibilityId,
            .colorAttachmentCount = renderPassEntry.colorAttachmentCount,
            .rasterizationSamples = renderPassEntry.sampleCount,
            // ARB_sample_shading. Dropped on a device without sampleRateShading rather than
            // hard-failing the draw: the rate is a hint, and the pipeline renders correctly at the
            // driver's own rate. Both halves move the render state's PIPELINE version, so a cached
            // pipeline built at the old rate cannot be handed back for the new one.
            .sampleShadingEnable = m_sampleRateShadingFeatureEnabled &&
                                   MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::SampleShading),
            .minSampleShading = MG_Pipe::gPipeInputs.GetMinSampleShadingValue(),
            // Word 1 keeps its all-ones initialiser: GL has no state for samples 32..63.
            .sampleMask = {ResolveEffectiveSampleMask(renderPassEntry.sampleCount), 0xffffffffu},
            .subpass = 0,
            .topology = vkTopology,
            .primitiveRestartEnable = primitiveRestartEnabled,
            .patchControlPoints = static_cast<Uint32>(MG_Pipe::gPipeInputs.GetPatchVertices()),
            .viewportCount = ResolveDrawViewportCount(programObj.writesViewportIndexBuiltin),
            .polygonMode = effectivePolygonMode,
            .cullMode = cullFaceEnabled
                ? MG_Util::ConvertCullFaceModeToVkEnum(MG_Pipe::gPipeInputs.GetCullFaceMode(), invertClockwise)
                : VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            // Read the geometry stage off the program's own shader list rather than
            // programObj.rasterizationProducerStage: that field is filled by the clip-fixup analysis,
            // which does not run for every program, so it reads Unknown for exactly the
            // geometry-plus-capture programs this guard exists to catch. Both inputs are link-time
            // facts folded into programObj.hash, which is what the pipeline memo and the
            // SetupDrawSnapshot fast path key on - so no memo can hand back a pipeline built for the
            // other mode. IsTransformFeedbackActive() would be a live bug here: neither memo key
            // moves on glBeginTransformFeedback.
            .provokingVertexMode = SelectProvokingVertexMode(
                vkTopology, ProgramCapturesXfbFromGeometryStage(program)),
            .depthTestEnable = depthTestEnabled,
            .depthWriteEnable = depthTestEnabled && MG_Pipe::gPipeInputs.GetDepthMask(),
            .depthBiasEnable = polygonOffsetFillEnabled,
            .rasterizerDiscardEnable = rasterizerDiscardEnabled,
            .logicOpEnable = colorLogicOpEnabled,
            .stencilTestEnable = stencilTestEnabled,
            .depthCompareOp = MG_Util::ConvertDepthTestFuncToVkEnum(MG_Pipe::gPipeInputs.GetDepthFunc()),
            .logicOp = MG_Util::ConvertLogicOperationToVkEnum(MG_Pipe::gPipeInputs.GetLogicOp()),
            .frontStencilFailOp = MG_Util::ConvertStencilOperationToVkEnum(frontStencil.FailOp),
            .frontStencilPassOp = MG_Util::ConvertStencilOperationToVkEnum(frontStencil.PassDepthPassOp),
            .frontStencilDepthFailOp = MG_Util::ConvertStencilOperationToVkEnum(frontStencil.PassDepthFailOp),
            .frontStencilCompareOp = MG_Util::ConvertDepthTestFuncToVkEnum(frontStencil.Func),
            .backStencilFailOp = MG_Util::ConvertStencilOperationToVkEnum(backStencil.FailOp),
            .backStencilPassOp = MG_Util::ConvertStencilOperationToVkEnum(backStencil.PassDepthPassOp),
            .backStencilDepthFailOp = MG_Util::ConvertStencilOperationToVkEnum(backStencil.PassDepthFailOp),
            .backStencilCompareOp = MG_Util::ConvertDepthTestFuncToVkEnum(backStencil.Func),
            .fragmentReplacesDepth = programObj.fragmentReplacesDepth,
            .stages = &programObj.stages,
            .vertexInputState = pipelineVertexInputState,
            .stageSpirvDigests = &programObj.stageSpirvDigests
        };
        // A program with a tessellation evaluation stage and no control stage relies on GL's
        // fixed-function pass-through (GL 4.6 core 11.2.2), which Vulkan does not have. Build the
        // stage GL describes for THIS draw's patch size - PATCH_VERTICES is draw state, not link
        // state, so it is only knowable here - and hand it to the pipeline. Where the
        // pass-through cannot stand in for what the evaluation stage actually reads, nothing is
        // attached and CreatePipeline refuses the pipeline, which skips the draw.
        //
        // Gated on the PATCH topology as well, and that gate is load-bearing rather than an
        // optimisation: patchControlPoints is only meaningful for a patch draw, and a pipeline
        // that carries tessellation stages while its topology is anything else violates
        // VUID-VkGraphicsPipelineCreateInfo-topology-00737 - the same class of invalid input as
        // the missing control stage, on the same driver. Such a draw is illegal in GL too (a
        // program with a tessellation stage may only be drawn with GL_PATCHES), so nothing legal
        // loses its pass-through here; what it does lose is the pipeline, because the refusal
        // below then sees an evaluation stage with no control stage and declines.
        //
        // The default tessellation levels (glPatchParameterfv) are draw state for the same reason
        // and are compiled into the same module, so they are read here too and their key is mixed
        // into the pipeline hash - without that a pipeline memoised at one set of levels would be
        // handed back after the application changed them.
        if (programObj.needsPassthroughTessControl && programObj.passthroughTessControlEmulatable &&
            vkTopology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {
            const FloatVec4& defaultOuterLevel = MG_Pipe::gPipeInputs.GetPatchDefaultOuterLevel();
            const FloatVec2& defaultInnerLevel = MG_Pipe::gPipeInputs.GetPatchDefaultInnerLevel();
            payload.passthroughTessControlKey = ProgramFactory::ComputePassthroughTessControlKey(
                payload.patchControlPoints, defaultOuterLevel, defaultInnerLevel,
                programObj.passthroughPerVertexMembers);
            payload.passthroughTessControlStage = m_programFactory->GetOrCreatePassthroughTessControlStage(
                payload.patchControlPoints, defaultOuterLevel, defaultInnerLevel,
                programObj.passthroughPerVertexMembers);
        }
        if (!payload.stencilTestEnable) {
            payload.frontStencilFailOp = VK_STENCIL_OP_KEEP;
            payload.frontStencilPassOp = VK_STENCIL_OP_KEEP;
            payload.frontStencilDepthFailOp = VK_STENCIL_OP_KEEP;
            payload.frontStencilCompareOp = VK_COMPARE_OP_ALWAYS;
            payload.backStencilFailOp = VK_STENCIL_OP_KEEP;
            payload.backStencilPassOp = VK_STENCIL_OP_KEEP;
            payload.backStencilDepthFailOp = VK_STENCIL_OP_KEEP;
            payload.backStencilCompareOp = VK_COMPARE_OP_ALWAYS;
        }
        const Bool hasDepthStencilAttachment = renderPassEntry.hasDepthStencilAttachment;
        if (!hasDepthStencilAttachment &&
            (payload.depthTestEnable || payload.depthWriteEnable || payload.stencilTestEnable)) {
            MGLOG_D("GetOrCreatePipeline: disabling depth/stencil tests for program=%u because render pass has no depth attachment (attachmentCount=%u colorAttachmentCount=%u)",
                    program.GetExternalIndex(),
                    renderPassEntry.attachmentCount,
                    renderPassEntry.colorAttachmentCount);
            payload.depthTestEnable = false;
            payload.depthWriteEnable = false;
            payload.stencilTestEnable = false;
            payload.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            payload.frontStencilFailOp = VK_STENCIL_OP_KEEP;
            payload.frontStencilPassOp = VK_STENCIL_OP_KEEP;
            payload.frontStencilDepthFailOp = VK_STENCIL_OP_KEEP;
            payload.frontStencilCompareOp = VK_COMPARE_OP_ALWAYS;
            payload.backStencilFailOp = VK_STENCIL_OP_KEEP;
            payload.backStencilPassOp = VK_STENCIL_OP_KEEP;
            payload.backStencilDepthFailOp = VK_STENCIL_OP_KEEP;
            payload.backStencilCompareOp = VK_COMPARE_OP_ALWAYS;
        }
        const Uint32 fragmentOutputMask = programObj.activeFragmentOutputLocationMask;
        // Outputs at locations past the render pass's trimmed colour span are
        // simply discarded - GL's semantic for a fragment output whose draw
        // buffer is GL_NONE (the trailing UNUSED slots no longer occupy
        // references, see GetOrCreateRenderPass).
        if ((fragmentOutputMask >> payload.colorAttachmentCount) != 0) {
            MGLOG_D("GetOrCreatePipeline: fragmentOutputMask=0x%x exceeds colorAttachmentCount=%u for program=%u; "
                    "outputs past the span are discarded",
                    fragmentOutputMask, payload.colorAttachmentCount, program.GetExternalIndex());
        }
        MOBILEGL_ASSERT(payload.colorAttachmentCount <= PipelineFactory::PipelineCreatePayload::kMaxColorAttachments,
                        "GetOrCreatePipeline: colorAttachmentCount=%u exceeds payload capacity",
                        payload.colorAttachmentCount);
        // The draw framebuffer is the applier's record on every arm (P13 W6): a draw with none
        // has nothing to build a pipeline for, and is declined by name rather than guessed at.
        const auto* wireFbo = MG_Pipe::MGPipeApplier().DrawFramebuffer();
        if (wireFbo == nullptr) {
            MGLOG_E_ONCE("GetOrCreatePipeline skipped: no draw framebuffer record is bound");
            return VK_NULL_HANDLE;
        }
        const Bool isDefaultDrawFbo = wireFbo->IsDefault;
        Array<FramebufferAttachmentType, MG_Pipe::kMGPipeMaxColorAttachments> drawBuffers{};
        for (SizeT i = 0; i < drawBuffers.size(); ++i)
            drawBuffers[i] = wireFbo->DrawBuffers[i] < 0 ? FramebufferAttachmentType::None :
                static_cast<FramebufferAttachmentType>(static_cast<Int>(FramebufferAttachmentType::Color0) + wireFbo->DrawBuffers[i]);
        for (Uint32 i = 0; i < payload.colorAttachmentCount; ++i) {
            BlendFactor srcRGB = BlendFactor::One;
            BlendFactor dstRGB = BlendFactor::Zero;
            BlendFactor srcAlpha = BlendFactor::One;
            BlendFactor dstAlpha = BlendFactor::Zero;
            BlendEquation colorEquation = BlendEquation::Add;
            BlendEquation alphaEquation = BlendEquation::Add;
            MG_Pipe::gPipeInputs.GetBlendFuncIndexed(i, srcRGB, dstRGB, srcAlpha, dstAlpha);
            MG_Pipe::gPipeInputs.GetBlendEquationIndexed(i, colorEquation, alphaEquation);
            const Bool blendEnabled = MG_Pipe::gPipeInputs.IsCapabilityEnabledIndexed(CapabilityInput::Blend, i);
            // Per-draw-buffer color write mask (glColorMaski). Divergent per-attachment masks require
            // the independentBlend device feature; when it is absent, fall back to draw buffer 0's
            // mask for every attachment (matching the non-indexed glColorMask broadcast).
            const BoolVec4 bufferMask =
                MG_Pipe::gPipeInputs.GetColorMaskIndexed(m_independentBlendFeatureEnabled ? i : 0);
            VkColorComponentFlags attachmentColorWriteMask = static_cast<VkColorComponentFlags>(
                (bufferMask.r() ? VK_COLOR_COMPONENT_R_BIT : 0u) |
                (bufferMask.g() ? VK_COLOR_COMPONENT_G_BIT : 0u) |
                (bufferMask.b() ? VK_COLOR_COMPONENT_B_BIT : 0u) |
                (bufferMask.a() ? VK_COLOR_COMPONENT_A_BIT : 0u));
            Bool effectiveBlendEnabled = blendEnabled;
            if (isDefaultDrawFbo && i < drawBuffers.size() &&
                drawBuffers[i] == FramebufferAttachmentType::None) {
                // The default framebuffer spans the same MAX_DRAW_BUFFERS slots as an FBO
                // (slot 0 is the back buffer, or None after glDrawBuffer(GL_NONE); slots
                // 1+ are always None). Discard writes and blend state for the None slots
                // like the FBO path below does, so stale indexed blend state on phantom
                // slots cannot leak into the pipeline - most notably into the blended
                // depth-write quirk's accumulation scan.
                attachmentColorWriteMask = 0;
                effectiveBlendEnabled = false;
            }
            if (!isDefaultDrawFbo) {
                const Int32 slot = wireFbo->DrawBuffers[i];
                if (slot < 0 || wireFbo->Color[slot].Kind == MG_Pipe::kMGPipeSurfaceKindNone) {
                    attachmentColorWriteMask = 0;
                    effectiveBlendEnabled = false;
                }
            }
            if (effectiveBlendEnabled) {
                MOBILEGL_ASSERT(i < drawBuffers.size(),
                                "GetOrCreatePipeline: color attachment %u is out of draw buffer range %zu",
                                i, drawBuffers.size());

                VkFormat colorAttachmentFormat = VK_FORMAT_UNDEFINED;
                Int textureExternalIndex = -1;
                if (!isDefaultDrawFbo) {
                    const Int32 slot = wireFbo->DrawBuffers[i];
                    colorAttachmentFormat = ResolveWireImage(*wireFbo, wireFbo->Color[slot], VK_IMAGE_ASPECT_COLOR_BIT).format;
                } else {
                    colorAttachmentFormat = m_swapchainObject.GetSurfaceFormat().format;
                }

                // Blending on an attachment whose format lacks
                // VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT is invalid pipeline state
                // (blend support is optional for e.g. 32-bit float formats on some GPUs);
                // force-disable it instead of baking undefined behavior into the pipeline.
                static thread_local UnorderedMap<Int, Bool> formatBlendSupport;
                auto blendSupportIt = formatBlendSupport.find(static_cast<Int>(colorAttachmentFormat));
                if (blendSupportIt == formatBlendSupport.end()) {
                    VkFormatProperties formatProperties{};
                    vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, colorAttachmentFormat,
                                                        &formatProperties);
                    const Bool blendable =
                        (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
                    blendSupportIt =
                        formatBlendSupport.emplace(static_cast<Int>(colorAttachmentFormat), blendable).first;
                    if (!blendable) {
                        MGLOG_E_ONCE("GetOrCreatePipeline: format=%d lacks VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT; "
                                "disabling blending on attachments with this format (first hit: attachment %u textureId=%d program=%u)",
                                static_cast<Int>(colorAttachmentFormat), i, textureExternalIndex,
                                program.GetExternalIndex());
                        if (PipelineFactory::IsSuppressBlendedDepthWriteEnabled()) {
                            // With blending force-disabled the blended depth-write quirk can
                            // never fire for pipelines on this format, so a depth-equality
                            // chain that accumulates into it (MC 26.3 OIT depth_bounds on
                            // RGBA32F) keeps its depth writes and may flicker on this driver.
                            MGLOG_W_ONCE("GetOrCreatePipeline: format=%d is not blendable, so the blended "
                                    "depth-write quirk cannot apply to it; depth-equality chains "
                                    "accumulating into this format may flicker",
                                    static_cast<Int>(colorAttachmentFormat));
                        }
                    }
                }
                if (!blendSupportIt->second) {
                    effectiveBlendEnabled = false;
                }
            }
            // Dual-source blending (GL_SRC1_* factors from glBlendFunc paired with
            // glBindFragDataLocationIndexed) requires the dualSrcBlend device feature. It is detected
            // at device creation and surfaced in the POST; there is no fallback that BLENDS correctly,
            // so a draw that asks for a SRC1 factor on a device without the feature gets the blend
            // DECLINED - this attachment is baked with blending off and neutral One/Zero factors, and
            // the loss is logged once. Both the factors AND the enable have to be neutralised:
            // VUID-VkPipelineColorBlendAttachmentState-srcColorBlendFactor-00608 and its three
            // siblings forbid a VK_BLEND_FACTOR_SRC1_* in the struct without the feature whatever
            // blendEnable says, so clearing only the enable would still be invalid pipeline state.
            // The previous behaviour, throwing, took the whole process down over one unsupported
            // blend factor; this is defined, survivable and visible in the log, and it matches what
            // the non-blendable-format arm above already does.
            if (!m_dualSrcBlendFeatureEnabled &&
                (IsDualSourceBlendFactor(srcRGB) || IsDualSourceBlendFactor(dstRGB) ||
                 IsDualSourceBlendFactor(srcAlpha) || IsDualSourceBlendFactor(dstAlpha))) {
                MGLOG_E_ONCE(
                    "GetOrCreatePipeline: dual-source blending (GL_SRC1_* blend factor) was requested on "
                    "color attachment %u, but the Vulkan device does not support the dualSrcBlend feature "
                    "(see the dualSrcBlend row in the driver POST). Blending is DECLINED on that "
                    "attachment - the fragment's first output is written unblended and the second source "
                    "is dropped (program=%u)",
                    i, program.GetExternalIndex());
                effectiveBlendEnabled = false;
                srcRGB = BlendFactor::One;
                dstRGB = BlendFactor::Zero;
                srcAlpha = BlendFactor::One;
                dstAlpha = BlendFactor::Zero;
            }
            payload.colorBlendAttachments[i] = MakeColorBlendAttachmentState(
                effectiveBlendEnabled,
                MG_Util::ConvertBlendFactorToVkEnum(srcRGB),
                MG_Util::ConvertBlendFactorToVkEnum(dstRGB),
                MG_Util::ConvertBlendEquationToVkEnum(colorEquation),
                MG_Util::ConvertBlendFactorToVkEnum(srcAlpha),
                MG_Util::ConvertBlendFactorToVkEnum(dstAlpha),
                MG_Util::ConvertBlendEquationToVkEnum(alphaEquation),
                attachmentColorWriteMask);
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        // P7 wave 2-B: MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS.
        //
        // The Redmi's OpenRA divergence (exit gate 3) is timing-shaped and reproduces ONLY with a
        // cold driver pipeline cache: three cleared-cache runs give a bit-identical 0.976494 /
        // 14658 mismatched pixels, while a warm cache gives 1.000000 from the second run on. What
        // a cold cache changes is how long vkCreateGraphicsPipelines takes on the apply thread,
        // so this knob buys that time on a host whose cache is always warm. INERT unless set, and
        // compiled only into the disaggregated build - the monolith image, which G1 pins, never
        // sees these lines. The delay goes on the MISS path only: a memo or cache hit is not
        // what is slow on the device either.
        if (const Uint32 delayMs = MagmaTestPipelineCreateDelayMs();
            delayMs != 0 && MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
#endif
        VkPipeline pipeline = m_pipelineFactory->GetOrCreatePipeline(payload);
        if (pipeline != VK_NULL_HANDLE) {
            PipelineMemoEntry& entry = m_pipelineMemo[m_pipelineMemoNext];
            entry.mode = mode;
            entry.programHash = programObj.hash;
            entry.vertexInputHash = vertexLayoutHash;
            entry.renderPassHash = renderPassHash;
            entry.wireRenderPassCompatibilityId = wireRenderPassCompatibilityId;
            entry.pipelineStateHash = pipelineStateHash;
            entry.renderStateCso = renderStateCso;
            entry.primitiveRestartEnable = primitiveRestartEnable;
            entry.transformFlags = transformFlags;
            entry.pipeline = pipeline;
            m_pipelineMemoNext = (m_pipelineMemoNext + 1) % kPipelineMemoSize;
            m_pipelineMemoCount = std::min(m_pipelineMemoCount + 1, kPipelineMemoSize);
        }
        return pipeline;
    }

    // The scissor rectangle Vulkan needs for ARB_viewport_array index `index`. Vulkan has no
    // per-viewport scissor-test TOGGLE - a scissor rectangle always applies - so an index whose
    // GL scissor test is disabled gets the whole framebuffer, which is exactly "the test always
    // passes" (GL 4.6 core 17.3.2).
    VkRect2D VulkanRenderer::ComputeGLScissorRect(Uint32 index, const IntVec2& extent,
                                                  VkSurfaceTransformFlagBitsKHR preTransform,
                                                  Bool isDefaultFbo) const {
        const auto& parameters = MG_Pipe::gPipeInputs.GetRenderStateParameters();
        if ((parameters.ScissorTestEnabledMask & (1u << index)) == 0) {
            VkRect2D full{};
            full.offset = {0, 0};
            full.extent = {static_cast<Uint32>(extent.x()), static_cast<Uint32>(extent.y())};
            return full;
        }
        const IntVec4& scissorBox = parameters.ScissorBoxes[index];
        return isDefaultFbo ? MakeDefaultFramebufferScissorRect(scissorBox, extent, preTransform)
                            : MakeClampedScissorRect(scissorBox, extent);
    }

    // The wide half of ApplyDynamicDrawStateTail: a pipeline built for a gl_ViewportIndex-writing
    // program declares viewportCount > 1, and Vulkan then requires that many viewports AND that
    // many scissors to have been set before the draw
    // (VUID-vkCmdDraw-viewportCount-03417/-03418). Deliberately unmemoized: only conformance
    // shaders reach it, the single-element dynamic-state shadow cannot describe an array, and
    // leaving that shadow invalidated is what makes the next ordinary draw re-push its own
    // single viewport instead of believing the array's element 0 is already bound.
    void VulkanRenderer::ApplyMultiViewportDynamicState(VkCommandBuffer commandBuffer, Uint32 viewportCount,
                                                        const IntVec2& extent,
                                                        VkSurfaceTransformFlagBitsKHR preTransform,
                                                        Bool isDefaultFbo) {
        MOBILEGL_ASSERT(viewportCount <= RenderStateParameters::MAX_VIEWPORTS,
                        "ApplyMultiViewportDynamicState: viewportCount=%u exceeds the indexed state width",
                        viewportCount);
        const Uint32 count = std::min<Uint32>(viewportCount, RenderStateParameters::MAX_VIEWPORTS);

        Array<VkViewport, RenderStateParameters::MAX_VIEWPORTS> viewports{};
        Array<VkRect2D, RenderStateParameters::MAX_VIEWPORTS> scissors{};
        for (Uint32 i = 0; i < count; ++i) {
            viewports[i] = ComputeGLViewport(i, extent, preTransform, isDefaultFbo);
            scissors[i] = ComputeGLScissorRect(i, extent, preTransform, isDefaultFbo);
        }
        vkCmdSetViewport(commandBuffer, 0, count, viewports.data());
        vkCmdSetScissor(commandBuffer, 0, count, scissors.data());

        auto& shadow = *g_dynamicStateShadow;
        shadow.viewportValid = false;
        shadow.scissorValid = false;
        shadow.dynamicTailValid = false;
    }

    void VulkanRenderer::ApplyDynamicDrawStateTail(FrameContext::FrameData& frame, const IntVec2& extent,
                                                   Bool isDefaultFbo, Uint32 viewportCount) {
        auto& shadow = *g_dynamicStateShadow;
        if (viewportCount > 1) {
            // The other five Apply* still run: blend constants, depth bias, line width and the
            // stencil masks are not per-viewport and a multi-viewport draw needs them just as
            // much. Only the viewport/scissor pair takes the array shape.
            ApplyBlendConstants(frame.commandBuffer);
            ApplyPolygonOffsetState(frame.commandBuffer);
            ApplyLineWidthState(frame.commandBuffer);
            ApplyStencilState(frame.commandBuffer);
            ApplyMultiViewportDynamicState(frame.commandBuffer, viewportCount, extent,
                                           m_swapchainObject.GetPreTransform(), isDefaultFbo);
            return;
        }
        // One compare for the whole tail: see the gate's declaration in
        // DynamicStateShadow for why (version, extent, default-FBO flag) pins every
        // input the six Apply* below read.
        //
        // P2 D12.3: this read is RE-SOURCED, not re-shaped. Under MOBILEGL_PIPE_PUSH the
        // accessor no longer walks into GLContext's RenderState - it returns
        // PipeInputs::m_renderStateParametersVersion, which the applier publishes from
        // MGPDynamicState::Version (set_dynamic_state) and MGPBindRenderState::Version
        // (bind_render_state). So the gate now reads what the client PUSHED.
        //
        // What it does NOT do, and the P2 brief expects it to, is stop moving on a
        // pipeline-only change. The tree settles that against the brief: bind_render_state
        // carries m_version too and the applier publishes it, and it has to - Espryt's
        // SyncRenderState uses the very same counter as its all-state change detector and G5
        // forbids touching it, so a bind that rewrote the pipeline half while leaving the
        // counter still would make Espryt skip re-syncing the blend state it just changed.
        // The second-level DynamicTailKey compare below is therefore what actually absorbs a
        // pipeline-only change, exactly as it did before P2: one key build, no vkCmd*.
        const Uint paramsVersion = MG_Pipe::gPipeInputs.GetRenderStateParametersVersion();
        if (shadow.dynamicTailValid && shadow.dynamicTailParamsVersion == paramsVersion &&
            shadow.dynamicTailExtentX == extent.x() && shadow.dynamicTailExtentY == extent.y() &&
            shadow.dynamicTailIsDefaultFbo == isDefaultFbo) {
            // Gate 6 of section 2.3.1: one version read plus a four-integer compare.
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::MagmaDynamicTail, /*hit=*/true);
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::AccessorCalls, 1);
            }
            return;
        }
        if (MG_Util::PipeStats::Enabled()) {
            // The version read above and the bulk parameter fetch that builds the value key.
            MG_Util::PipeStats::CountGate(MG_Util::PipeStats::Gate::MagmaDynamicTail, /*hit=*/false);
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::AccessorCalls, 2);
        }
        const VkSurfaceTransformFlagBitsKHR preTransform = m_swapchainObject.GetPreTransform();
        // Second-level VALUE gate: the version moved, but RenderState's version counts
        // every parameter, most of which this tail never reads. Build the key over
        // exactly the tail's inputs (inventory in DynamicTailKey) out of one bulk
        // parameters fetch and compare; an equal key means every Apply* below would
        // re-derive the value its shadow already holds.
        DynamicStateShadow::DynamicTailKey key;
        {
            const RenderStateParameters& p = MG_Pipe::gPipeInputs.GetRenderStateParameters();
            // Viewport 0 and its depth range: ApplyGLViewportState reads exactly those two
            // (per-index state for indices > 0 is keyed separately, see multiViewportKey below).
            key.viewport[0] = p.Viewports[0].x();
            key.viewport[1] = p.Viewports[0].y();
            key.viewport[2] = p.Viewports[0].z();
            key.viewport[3] = p.Viewports[0].w();
            key.depthRange[0] = p.DepthRanges[0].x();
            key.depthRange[1] = p.DepthRanges[0].y();
            key.blendColor[0] = p.BlendColor.x();
            key.blendColor[1] = p.BlendColor.y();
            key.blendColor[2] = p.BlendColor.z();
            key.blendColor[3] = p.BlendColor.w();
            key.polygonOffsetFactor = p.PolygonOffsetFactor;
            key.polygonOffsetUnits = p.PolygonOffsetUnits;
            key.lineWidth = p.LineWidth;
            // StencilStates[0] is Front, [1] is Back (RenderState::GetStencilFaceIndex),
            // the same order ApplyStencilState reads them in.
            for (Uint32 face = 0; face < 2; ++face) {
                key.stencilValueMask[face] = p.StencilStates[face].ValueMask;
                key.stencilWriteMask[face] = p.StencilStates[face].WriteMask;
                key.stencilRef[face] = p.StencilStates[face].Ref;
            }
            key.scissorEnabled = (p.ScissorTestEnabledMask & 1u) != 0;
            key.scissorBox[0] = p.ScissorBoxes[0].x();
            key.scissorBox[1] = p.ScissorBoxes[0].y();
            key.scissorBox[2] = p.ScissorBoxes[0].z();
            key.scissorBox[3] = p.ScissorBoxes[0].w();
            key.extentX = extent.x();
            key.extentY = extent.y();
            key.preTransform = static_cast<Uint32>(preTransform);
            key.isDefaultFbo = isDefaultFbo;
        }
        if (shadow.dynamicTailValid && shadow.dynamicTailKey == key) {
            // Re-arm the cheap version gate so an unchanged-parameters run of draws after
            // this one costs the four-integer compare again.
            shadow.dynamicTailParamsVersion = paramsVersion;
            return;
        }
        ApplyGLViewportState(frame.commandBuffer, extent, preTransform, isDefaultFbo);
        ApplyBlendConstants(frame.commandBuffer);
        ApplyPolygonOffsetState(frame.commandBuffer);
        ApplyLineWidthState(frame.commandBuffer);
        ApplyStencilState(frame.commandBuffer);
        VkRect2D scissor{};
        if (key.scissorEnabled) {
            const IntVec4 scissorBox(key.scissorBox[0], key.scissorBox[1], key.scissorBox[2], key.scissorBox[3]);
            scissor = isDefaultFbo ? MakeDefaultFramebufferScissorRect(scissorBox, extent, preTransform)
                                   : MakeClampedScissorRect(scissorBox, extent);
        } else {
            scissor.offset = {0, 0};
            scissor.extent = { (Uint)extent.x(), (Uint)extent.y() };
        }
        ShadowedSetScissor(frame.commandBuffer, scissor);
        shadow.dynamicTailValid = true;
        shadow.dynamicTailParamsVersion = paramsVersion;
        shadow.dynamicTailExtentX = extent.x();
        shadow.dynamicTailExtentY = extent.y();
        shadow.dynamicTailIsDefaultFbo = isDefaultFbo;
        shadow.dynamicTailKey = key;
    }

    Uint32 VulkanRenderer::GetBaseTransformFlagsRaw(Bool isDefaultFbo) {
        // GetShaderTransformFlags is a function of the pre-transform AND of whether
        // the bound draw framebuffer is the default one (the Y-flip/rotation bits
        // apply only when presenting). Memo keyed on both; keying on the
        // pre-transform alone served an FBO pass's unflipped flags to the following
        // default-framebuffer pass and flipped the whole frame.
        // isDefaultFbo is supplied by the caller: every draw-path caller has already
        // resolved the bound draw framebuffer (and its default-ness) for its own
        // guards, and re-walking the binding slot + the virtual IsDefaultFramebuffer
        // per draw showed up in the profile. Callers MUST pass the value derived from
        // the SAME draw-framebuffer binding the draw uses - see the assert below.
        MOBILEGL_ASSERT(
            [&] {
                const auto* fbo = MG_Pipe::MGPipeApplier().DrawFramebuffer();
                return isDefaultFbo == (fbo != nullptr && fbo->IsDefault);
            }(),
            "GetBaseTransformFlagsRaw: isDefaultFbo does not match the bound draw framebuffer");
        const VkSurfaceTransformFlagBitsKHR preTransform = m_swapchainObject.GetPreTransform();
        if (!m_baseTransformFlagsKeyValid || preTransform != m_baseTransformFlagsPreTransform ||
            isDefaultFbo != m_baseTransformFlagsIsDefaultFbo) {
            m_baseTransformFlagsCache = GetShaderTransformFlags(preTransform).GetRaw();
            m_baseTransformFlagsPreTransform = preTransform;
            m_baseTransformFlagsIsDefaultFbo = isDefaultFbo;
            m_baseTransformFlagsKeyValid = true;
        }
        return m_baseTransformFlagsCache;
    }

    Bool VulkanRenderer::SetupDraw(FrameContext::FrameData& frame, GLenum mode, Flags<DrawSetupAspect> aspects,
                                   const DrawCmdParam& drawParams,
                                   const IndexBufferView* pIndexBufferView) {
        // First, before anything of this draw is recorded or resolved - on the wire arm too: a
        // split server records every draw its client sends into the same frame command buffer, so
        // the same loading frame grows it the same way there.
        SplitOversizedRecording();
        if (!RewindWireDescriptorSetsIfDue()) return false;
        // The wire route returns before the monolith branch's draw-gated
        // sweep. Dead wire texture/renderbuffer records otherwise live
        // until the next frame boundary, which a long trace may not reach.
        m_textureManager->CollectGarbage();
        return SetupWireDraw(frame, mode, aspects, drawParams, pIndexBufferView);
    }

    void VulkanRenderer::DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
        // Before the wire branch, for SetupDraw's reason.
        SplitOversizedRecording();
        if (!RewindWireDescriptorSetsIfDue()) return;
        m_textureManager->CollectGarbage();
        DispatchWireCompute(numGroupsX, numGroupsY, numGroupsZ);
        return;
    }

    void VulkanRenderer::DispatchComputeIndirect(GLintptr indirect) {
        SplitOversizedRecording();
        // P8-SV: the wire arm's own indirect dispatch, DispatchCompute's shape one call over.
        if (!RewindWireDescriptorSetsIfDue()) return;
        m_textureManager->CollectGarbage();
        DispatchWireComputeIndirect(indirect);
        return;
    }

    VkMemoryBarrier VulkanRenderer::BuildMemoryBarrierForGlBarriers(GLbitfield barriers) {
        VkMemoryBarrier memoryBarrier{};
        memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        memoryBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
            VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        if ((barriers & GL_COMMAND_BARRIER_BIT) != 0) {
            memoryBarrier.dstAccessMask |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        }
        return memoryBarrier;
    }

    void VulkanRenderer::MemoryBarrier(GLbitfield barriers) {
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        VkMemoryBarrier memoryBarrier = BuildMemoryBarrierForGlBarriers(barriers);

        MGLOG_D("DirectVulkan: glMemoryBarrier(0x%x)", static_cast<Uint32>(barriers));
        vkCmdPipelineBarrier(frame.commandBuffer,
                             // ALL_COMMANDS does not include HOST. The barrier's
                             // HOST_WRITE access must have a matching source stage.
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             1, &memoryBarrier, 0, nullptr, 0, nullptr);
    }

    // A tightly packed depth/stencil readback, one aspect or both, in the client's own width.
    // The two arms that read depth or stencil off a Vulkan image - the monolith
    // ReadDepthStencilImageToClient and the wire ReadTextureImageWire - copy their aspect into a
    // host-visible buffer and both owe the application the same bytes, so they share this one
    // encoding rather than each carrying a copy of it. A copy is exactly the kind of pair where
    // a fix landed on one side only: the signed ranges and GL_HALF_FLOAT were added to the wire
    // caller and the monolith caller kept scaling a GL_SHORT depth into 65535, which put a 0.5
    // depth at 0x8000 - a negative index.
    static Bool PackDepthStencilTight(const Uint8* depth, const Uint8* stencil, VkFormat sourceFormat, SizeT count,
                                      GLenum format, GLenum type, Vector<Uint8>& result) {
        const Bool stencilOnly = format == GL_STENCIL_INDEX;
        const auto readDepth = [&](SizeT i) -> Float {
            if (sourceFormat == VK_FORMAT_D16_UNORM) {
                Uint16 word{}; Memcpy(&word, depth + i * 2, sizeof(word));
                return static_cast<Float>(word) / 65535.0f;
            }
            if (sourceFormat == VK_FORMAT_X8_D24_UNORM_PACK32 || sourceFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
                Uint32 word{}; Memcpy(&word, depth + i * 4, sizeof(word));
                return static_cast<Float>(word & 0xffffffu) / 16777215.0f;
            }
            Float value{}; Memcpy(&value, depth + i * sizeof(value), sizeof(value));
            return value;
        };
        SizeT size = 0;
        switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: size = 1; break;
        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: size = 2; break;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: size = 4; break;
        case GL_UNSIGNED_INT_24_8: size = 4; break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: size = 8; break;
        default: return false;
        }
        const Bool packed = type == GL_UNSIGNED_INT_24_8 || type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        if ((format == GL_DEPTH_STENCIL) != packed) return false;
        result.resize(count * size);
        for (SizeT i = 0; i < count; ++i) {
            Uint8* dst = result.data() + i * size;
            const Uint32 s = stencil ? stencil[i] : 0;
            const Float d = stencilOnly ? 0.0f : readDepth(i);
            switch (type) {
            case GL_FLOAT: {
                const Float value = stencilOnly ? static_cast<Float>(s) : d;
                Memcpy(dst, &value, sizeof(value)); break;
            }
            case GL_HALF_FLOAT: {
                const Uint16 value = MG_Util::EncodeFloatToHalfBits(stencilOnly ? static_cast<Float>(s) : d);
                Memcpy(dst, &value, sizeof(value)); break;
            }
            case GL_UNSIGNED_INT_24_8: {
                Uint32 depth24 = 0;
                if (sourceFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
                    Memcpy(&depth24, depth + i * 4, sizeof(depth24));
                    depth24 &= 0xffffffu;
                } else {
                    depth24 = static_cast<Uint32>(std::llround(std::clamp<double>(d, 0.0, 1.0) * 16777215.0));
                }
                const Uint32 value = (depth24 << 8) | s;
                Memcpy(dst, &value, sizeof(value)); break;
            }
            case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
                Memcpy(dst, &d, sizeof(d)); Memcpy(dst + 4, &s, sizeof(s)); break;
            default: {
                const Bool signedType = type == GL_BYTE || type == GL_SHORT || type == GL_INT;
                const Uint64 maximum = signedType ? ((Uint64{1} << (size * 8 - 1)) - 1)
                                                 : ((Uint64{1} << (size * 8)) - 1);
                const Uint32 value = stencilOnly ? s : static_cast<Uint32>(
                    std::llround(std::clamp<double>(d, 0.0, 1.0) * static_cast<double>(maximum)));
                Memcpy(dst, &value, size); break;
            }
            }
        }
        return true;
    }

    #include "WireFramebuffer.inc"
    #include "WireTextureReadback.inc"
    #include "WireColorBlit.inc"
    // AFTER WireColorBlit.inc: the depth mip reuses that file's window-Y convention and is
    // written to be read beside it.
    #include "WireDepthMipmap.inc"
    // AFTER WireDepthMipmap.inc for the same reason: the multisample depth/stencil resolve is
    // that file's pass with the box filter taken out and a sample index put in.
    #include "WireMultisampleResolve.inc"
    #include "WireDraw.inc"
    // AFTER WireFramebuffer.inc: the shared-image present is that file's readback, into an image.
    #include "WireSharedImage.inc"
    // AFTER WireSharedImage.inc: a YUV image is that file's reading side, converted on the way.
    #include "WireYuvImage.inc"

    void VulkanRenderer::Clear(GLbitfield mask) {
        const auto* fbo = MG_Pipe::MGPipeApplier().DrawFramebuffer();
        if (!fbo) MagmaWireFatal("clear-framebuffer-record");
        ClearAttachmentPayload payload{};
        payload.mask = mask;
        payload.color = MG_Pipe::gPipeInputs.GetClearColor();
        payload.depth = MG_Pipe::gPipeInputs.GetClearDepth();
        payload.stencil = MG_Pipe::gPipeInputs.GetClearStencil();
        ClearWireFramebuffer(*fbo, payload);
        return;
    }

    void VulkanRenderer::QueueClearBufferPayload(GLenum buffer, GLint drawbuffer,
                                                 const ClearAttachmentPayload& clearPayload) {
        const auto* fbo = MG_Pipe::MGPipeApplier().DrawFramebuffer();
        if (!fbo) MagmaWireFatal("clear-buffer-framebuffer-record");
        ClearWireFramebuffer(*fbo,clearPayload,buffer == GL_COLOR ? drawbuffer : -1);
        return;
    }

    void VulkanRenderer::ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        ClearAttachmentPayload payload{};
        payload.mask = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        // Vulkan clear values require depth in [0,1] (VUID-VkClearDepthStencilValue-depth-00022).
        payload.depth = std::clamp(depth, 0.0f, 1.0f);
        payload.stencil = static_cast<Uint32>(stencil);
        QueueClearBufferPayload(buffer, drawbuffer, payload);
    }

    void VulkanRenderer::ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        if (value == nullptr) {
            return;
        }
        ClearAttachmentPayload payload{};
        switch (buffer) {
            case GL_COLOR:
                payload.mask = GL_COLOR_BUFFER_BIT;
                payload.color = FloatVec4(value[0], value[1], value[2], value[3]);
                break;
            case GL_DEPTH:
                payload.mask = GL_DEPTH_BUFFER_BIT;
                payload.depth = std::clamp(value[0], 0.0f, 1.0f);
                break;
            default:
                break;
        }
        QueueClearBufferPayload(buffer, drawbuffer, payload);
    }

    void VulkanRenderer::ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        if (value == nullptr) {
            return;
        }
        ClearAttachmentPayload payload{};
        switch (buffer) {
            case GL_COLOR:
                payload.mask = GL_COLOR_BUFFER_BIT;
                payload.colorEncoding = ClearColorEncoding::Uint;
                payload.colorUint = UintVec4(value[0], value[1], value[2], value[3]);
                break;
            case GL_STENCIL:
                payload.mask = GL_STENCIL_BUFFER_BIT;
                payload.stencil = value[0];
                break;
            default:
                break;
        }
        QueueClearBufferPayload(buffer, drawbuffer, payload);
    }

    void VulkanRenderer::ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
        if (value == nullptr) {
            return;
        }
        ClearAttachmentPayload payload{};
        switch (buffer) {
            case GL_COLOR:
                payload.mask = GL_COLOR_BUFFER_BIT;
                payload.colorEncoding = ClearColorEncoding::Int;
                payload.colorInt = IntVec4(value[0], value[1], value[2], value[3]);
                break;
            case GL_STENCIL:
                payload.mask = GL_STENCIL_BUFFER_BIT;
                payload.stencil = static_cast<Uint32>(std::max(value[0], 0));
                break;
            default:
                break;
        }
        QueueClearBufferPayload(buffer, drawbuffer, payload);
    }

    void VulkanRenderer::DestroyMultisampleResolveScratchImage() {
        if (m_msResolveScratch.image != VK_NULL_HANDLE) {
            vmaDestroyImage(m_allocator, m_msResolveScratch.image, m_msResolveScratch.allocation);
        }
        m_msResolveScratch = {};
    }

    void VulkanRenderer::BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                         GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                         GLbitfield mask, GLenum filter) {
        BlitWireFramebuffers(srcX0,srcY0,srcX1,srcY1,dstX0,dstY0,dstX1,dstY1,mask,filter);
        return;
    }

    void VulkanRenderer::CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                           GLint x, GLint y, GLsizei width, GLsizei height) {
        CopyWireFramebufferToTexture(target,level,xoffset,yoffset,x,y,width,height);
        return;
    }

    namespace {
        // GL hands CopyImageSubData ONE z/depth pair and lets the texture target decide what it
        // means. Vulkan splits that meaning across two different fields of VkImageCopy, chosen by
        // the image type:
        //
        //   VK_IMAGE_TYPE_3D  - slices live on the z axis: srcOffset.z/dstOffset.z select them and
        //                       extent.depth counts them. The subresource layer range must stay
        //                       (0, 1): Vulkan reads a 3D image as a single layer whose depth is
        //                       the mip level's depth (VUID-VkImageCopy-apiVersion-07932/-07933).
        //   everything else   - slices live in the array dimension: baseArrayLayer selects them and
        //                       layerCount counts them, while offset.z stays 0 and (when neither
        //                       endpoint is 3D) extent.depth stays 1.
        //
        // A mixed 2D-array <-> 3D pair is legal because maintenance1 - core since Vulkan 1.1 -
        // relaxed the old "layerCounts must match" rule into "the 3D side's extent.depth must
        // equal the array side's layerCount".
        struct CopyImageSliceMapping {
            // True for a VK_IMAGE_TYPE_3D image, i.e. slices ride the z axis, not the layer axis.
            Bool slicesAreDepth = false;
            // The GL z offset, kept in whichever field this endpoint's image type reads it from.
            Uint32 baseSlice = 0;
            // Slices this endpoint can address at the selected mip level; the copy range check
            // needs the level's depth for a 3D image (3D mips shrink in z) and the image's array
            // size for a layered one (array layers do not shrink).
            Uint32 availableSlices = 1;

            Uint32 BaseArrayLayer() const { return slicesAreDepth ? 0u : baseSlice; }
            Int32 OffsetZ() const { return slicesAreDepth ? static_cast<Int32>(baseSlice) : 0; }
        };

        // The Vulkan image one glCopyImageSubData endpoint names, after the two object kinds GL
        // 4.6 core 18.3.2 allows have been collapsed onto the fields this copy reads. A
        // renderbuffer is a single-level, single-layer 2D image, so its shape answers are
        // constants rather than a mip walk. `trackedLayout` points AT the owning resource's own
        // layout field - both resource maps are node-based, so the pointer survives the further
        // lookups the clear materialization below makes.
        struct CopyImageVkImage {
            Bool isRenderbuffer = false;
            VkImage image = VK_NULL_HANDLE;
            VkImageLayout* trackedLayout = nullptr;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_NONE;
            Uint32 mipLevels = 1;
            VkExtent2D extent = {0, 0};
            Uint32 depth = 1;
            Uint32 arrayLayers = 1;
            // Both resources carry a format; this copy used to decline to read it, which is why a
            // four-row drift between the texture and renderbuffer format tables turned into
            // corrupted texels with nothing in the log. vkCmdCopyImage requires size-compatible
            // formats whenever they differ (VUID-vkCmdCopyImage-srcImage-01548) and there is no
            // downstream check - a mismatched pair is a promise the driver takes at face value.
            VkFormat format = VK_FORMAT_UNDEFINED;
        };

        Bool TryResolveCopyImageSliceMapping(TextureTarget target, const CopyImageVkImage& image, Uint32 mipLevel,
                                             GLint glZ, GLsizei glDepth, CopyImageSliceMapping& outMapping) {
            if (glZ < 0 || glDepth <= 0) {
                return false;
            }
            const Uint32 baseSlice = static_cast<Uint32>(glZ);
            if (image.isRenderbuffer) {
                // A renderbuffer holds one 2D image and nothing else; GL still requires the
                // z/depth pair and it can only name that one slice.
                outMapping = {};
                return baseSlice == 0 && glDepth == 1;
            }
            switch (target) {
            case TextureTarget::Texture1D:
            case TextureTarget::Texture2D:
            case TextureTarget::TextureRectangle:
            case TextureTarget::Texture2DMultisample:
                // Not layered at all: GL still requires the z/depth pair, and it can only name the
                // one slice these targets have.
                outMapping = {};
                return baseSlice == 0 && glDepth == 1;
            case TextureTarget::Texture3D:
                outMapping.slicesAreDepth = true;
                outMapping.baseSlice = baseSlice;
                outMapping.availableSlices = std::max(1u, image.depth >> mipLevel);
                return true;
            case TextureTarget::Texture1DArray:
            case TextureTarget::Texture2DArray:
            case TextureTarget::Texture2DMultisampleArray:
            case TextureTarget::TextureCubeMap:
            case TextureTarget::TextureCubeMapArray:
                // A cube map is an array of six faces here (see TryResolveTextureShapeInfo), and GL
                // numbers its faces on the same z axis an array texture numbers its layers, so both
                // arrive as a plain layer range.
                //
                // GL_TEXTURE_1D_ARRAY belongs here too, and needs no remap: this backend STORES it
                // as a VK_IMAGE_TYPE_1D image whose layers live in arrayLayers (ToVulkanLevelExtent
                // moves the count across), and GL 4.6 core 18.3.2 ADDRESSES it as a stack of slices
                // on z with an image height of 1 - so the frontend's y/height are already the 0/1
                // Vulkan requires and the layer lands in baseArrayLayer either way.
                outMapping.slicesAreDepth = false;
                outMapping.baseSlice = baseSlice;
                outMapping.availableSlices = image.arrayLayers;
                return true;
            default:
                // GL_TEXTURE_BUFFER has no image at all. Declined rather than mis-addressed.
                return false;
            }
        }

        Uint CopyImageEndpointName(const CopyImageEndpoint& endpoint) {
            return endpoint.IsRenderbuffer() ? endpoint.RenderbufferHandle.Slot : endpoint.TextureHandle.Slot;
        }
    } // namespace

    void VulkanRenderer::CopyImageSubData(const CopyImageEndpoint& srcEndpoint,
                                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                          const CopyImageEndpoint& dstEndpoint,
                                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        const Bool wire = true; // P13 W6: the record arm is the only arm
        MG_Pipe::MGPipeHandle dstStorageHandle = dstEndpoint.TextureHandle;
        if (HasPendingRecordedWork() && !FlushPendingCommands()) {
            if (LatchWireDeviceLoss("copy-image-flush")) return;
            MagmaWireFatal("copy-image-flush");
        }
        MOBILEGL_ASSERT(srcEndpoint.Exists() && dstEndpoint.Exists(),
                        "CopyImageSubData requires valid source and destination images.");
        // The frontend already declines a zero or negative extent, so anything else here is a
        // caller MobileGL wrote - but it still reaches vkCmdCopyImage in a release build, and a
        // zero extent.depth is as invalid as a zero width.
        if (srcWidth <= 0 || srcHeight <= 0 || srcDepth <= 0) {
            MGLOG_E_ONCE("%s: non-positive copy extent %dx%dx%d; declining the copy", __func__, srcWidth, srcHeight,
                         srcDepth);
            return;
        }

        const auto srcTextureTarget = MG_Util::ConvertGLEnumToTextureTarget(srcTarget);
        const auto dstTextureTarget = MG_Util::ConvertGLEnumToTextureTarget(dstTarget);
        // Both endpoints of a same-image copy would have to share one VkImageLayout, so the
        // TRANSFER_SRC/TRANSFER_DST pair below cannot express it (it needs VK_IMAGE_LAYOUT_GENERAL
        // and an overlap check). Refused outright, and refused for real rather than through an
        // assertion the release build drops: recording the pair anyway is a validation error and,
        // on a tiler, a copy whose source has already been overwritten.
        // Compared by STORAGE, not by GL object: a texture view and the texture it views are two
        // different objects over one VkImage (ARB_texture_view), and GL 4.6 core 8.18 explicitly
        // permits copying between them - so an object-identity test would let exactly the case
        // this guard exists for through.
        const auto* srcStorageTexture =
            srcEndpoint.Texture ? &VkTextureManager::StorageTextureOf(*srcEndpoint.Texture) : nullptr;
        const auto* dstStorageTexture =
            dstEndpoint.Texture ? &VkTextureManager::StorageTextureOf(*dstEndpoint.Texture) : nullptr;
        if (
            !wire &&
            srcStorageTexture == dstStorageTexture && srcEndpoint.Renderbuffer == dstEndpoint.Renderbuffer) {
            MGLOG_E_ONCE("%s: in-place copy on objectId=%u is not supported; declining the copy", __func__,
                         CopyImageEndpointName(srcEndpoint));
            return;
        }

        // One resolver for both object kinds. The texture arm is the same
        // SyncTextureAndGetDescriptor the copy always used; the renderbuffer arm goes through the
        // render-pass manager, which is where a renderbuffer's VkImage lives.
        const auto resolveImage = [this](const CopyImageEndpoint& endpoint, CopyImageVkImage& out) {
            const Bool renderbuffer = endpoint.IsRenderbuffer();
            // The same handle-keyed allocation used by wire FBO clear/draw/read.
            // The legacy render-pass manager owns a different, frontend-keyed map.
            auto* resource = m_textureManager->SyncTextureResourceByHandle(
                renderbuffer ? endpoint.RenderbufferHandle : endpoint.TextureHandle, renderbuffer);
            if (!resource) MagmaWireFatal("copy-image-resource");
            out.isRenderbuffer = renderbuffer;
            out.image = resource->image;
            out.trackedLayout = &resource->layout;
            out.aspect = resource->aspect;
            out.mipLevels = resource->mipLevels;
            out.extent = resource->extent;
            out.depth = resource->depth;
            out.arrayLayers = resource->arrayLayers;
            out.format = resource->format;
            return out.image != VK_NULL_HANDLE;
        };
        CopyImageVkImage srcImage{};
        CopyImageVkImage dstImage{};
        const Bool srcResolved = resolveImage(srcEndpoint, srcImage);
        const Bool dstResolved = resolveImage(dstEndpoint, dstImage);
        // P7 wave 2-B, CONTRACT-P7 §3.2: `copy-image-in-place@P7` RETIRES.
        //
        // What the Fatal here used to say was true of the code below it and of nothing else: the
        // TRANSFER_SRC/TRANSFER_DST pair cannot describe one image, because both endpoints share
        // one VkImageLayout and the second transition undoes the first. VK_IMAGE_LAYOUT_GENERAL
        // can describe it - it is a legal layout for both sides of vkCmdCopyImage - so a
        // same-image copy takes ONE transition of the whole image to GENERAL, GENERAL on both
        // sides of the command, and ONE restore. The monolith arm declines this shape outright
        // (a few lines up, `!wire &&`); the wire arm is now the one that performs it, which is
        // what §3.2 asks for and more than parity would give.
        //
        // `inPlace` is constexpr false in the pull build so that every branch it guards folds
        // away: this function is in the monolith image too and G1 pins that image's .text.
        const Bool inPlace = wire && srcImage.image != VK_NULL_HANDLE &&
                             srcImage.image == dstImage.image;
        if (wire) m_textureManager->FlushPendingUploads();
        // Real checks, not MOBILEGL_ASSERT: the assertions this replaces compile to nothing in
        // a release build, which is where both observed failures happened - a null resource
        // dereferenced right below (lavapipe) and a mip level the VkImage does not have handed
        // to vkCmdCopyImage (Adreno, SIGSEGV inside the driver). Neither is caught downstream:
        // an out-of-range subresource is a promise the driver takes at face value.
        //
        // _ONCE, because the severity is right but the repetition is not: MGLOG_E is the level
        // the project logs failures at and it IS live at the default MOBILEGL_LOG_ACTIVE_LEVEL,
        // so an application that reissues the same rejected copy every frame would otherwise
        // print at ERROR every frame. Once per site says the same thing and says it in a log
        // somebody can still read.
        //
        // The frontend validator (ValidateTextureLevelExists) is what produces the
        // GL_INVALID_VALUE the application is actually owed. This guard exists so the next gap
        // up there declines a copy instead of taking the process down.
        if (!srcResolved || !dstResolved) {
            MGLOG_E_ONCE("%s: source or destination image failed to sync; declining the copy", __func__);
            return;
        }
        // Storage space from here down. srcImage/dstImage are the STORAGE textures' resources
        // (SyncTextureAndGetDescriptor resolves a view to the texture it views), while srcLevel /
        // dstLevel and the z origins below arrived relative to whichever name the application
        // passed - so a view's level 0 has to become the parent level it opened onto before it
        // can index a subresource, exactly as at every other attachment boundary.
        if (wire) {
            const auto mapSubresource = [this](const CopyImageEndpoint& endpoint, GLint& mip, GLint& layer) {
                if (endpoint.IsRenderbuffer()) {
                    if (mip != 0 || layer != 0) MagmaWireFatal("copy-image-renderbuffer-subresource");
                    return endpoint.RenderbufferHandle;
                }
                Uint32 mappedMip = static_cast<Uint32>(mip), mappedLayer = static_cast<Uint32>(layer);
                const auto storage = m_textureManager->ResolveWireTextureStorage(
                    endpoint.TextureHandle, mappedMip, mappedLayer);
                if (MG_Pipe::MGPipeHandleIsNull(storage)) MagmaWireFatal("copy-image-view-record");
                mip = static_cast<GLint>(mappedMip); layer = static_cast<GLint>(mappedLayer);
                return storage;
            };
            (void)mapSubresource(srcEndpoint, srcLevel, srcZ);
            dstStorageHandle = mapSubresource(dstEndpoint, dstLevel, dstZ);
        }
        srcLevel = static_cast<GLint>(ToStorageMipLevel(srcEndpoint.Texture.get(), srcLevel));
        dstLevel = static_cast<GLint>(ToStorageMipLevel(dstEndpoint.Texture.get(), dstLevel));
        srcZ = static_cast<GLint>(ToStorageArrayLayer(srcEndpoint.Texture.get(), srcZ));
        dstZ = static_cast<GLint>(ToStorageArrayLayer(dstEndpoint.Texture.get(), dstZ));
        if (srcLevel < 0 || dstLevel < 0 || static_cast<Uint32>(srcLevel) >= srcImage.mipLevels ||
            static_cast<Uint32>(dstLevel) >= dstImage.mipLevels) {
            MGLOG_E_ONCE("%s: mip level out of range (src %d of %u, dst %d of %u); declining the copy", __func__,
                         srcLevel, srcImage.mipLevels, dstLevel, dstImage.mipLevels);
            return;
        }
        // Size compatibility, the guard whose absence let a table drift two files away reach the
        // driver as a promise. glCopyImageSubData is a raw texel-block move (GL 4.6 core 18.3.2), and
        // Vulkan says as much: when the two formats differ they must be size-compatible - the same
        // texel block size - or vkCmdCopyImage is undefined (VUID-vkCmdCopyImage-srcImage-01548).
        // Nothing else on this path asks: the three checks around it cover the mip range, the region
        // bounds and the slice range, and none of them ever looked at a format.
        //
        // A decline rather than a MOBILEGL_ASSERT, for the reason the neighbouring guards spell out:
        // assertions compile out of the release build that the CTS and shipping both run, which is
        // exactly where the corruption was observed.
        if (srcImage.format != dstImage.format) {
            // Size-compatibility is the COLOUR rule. Vulkan makes each depth/stencil format compatible
            // only with ITSELF, and the texel block sizes cannot tell them apart: X8_D24_UNORM_PACK32,
            // D32_SFLOAT and D24_UNORM_S8_UINT are all 4 bytes and all in different compatibility
            // classes, so a raw block-size test waves through exactly the pairs Vulkan forbids. The
            // frontend cannot filter them either - its own texel-block resolver is byte-size only, so
            // glCopyImageSubData between a GL_DEPTH_COMPONENT24 texture and a GL_DEPTH_COMPONENT32F
            // one reaches here with two different depth formats and 4 == 4.
            const Bool eitherIsDepthStencil =
                ((srcImage.aspect | dstImage.aspect) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
            if (eitherIsDepthStencil) {
                MGLOG_E_ONCE("%s: depth/stencil formats are compatible only with themselves, and source format "
                             "%d differs from destination format %d; declining the copy",
                             __func__, static_cast<Int>(srcImage.format), static_cast<Int>(dstImage.format));
                return;
            }
            const Uint32 srcBlockSize = vkuGetFormatInfo(srcImage.format).texel_block_size;
            const Uint32 dstBlockSize = vkuGetFormatInfo(dstImage.format).texel_block_size;
            if (srcBlockSize == 0 || dstBlockSize == 0 || srcBlockSize != dstBlockSize) {
                MGLOG_E_ONCE("%s: source format %d and destination format %d are not size-compatible "
                             "(%u vs %u bytes per texel block); declining the copy",
                             __func__, static_cast<Int>(srcImage.format), static_cast<Int>(dstImage.format),
                             srcBlockSize, dstBlockSize);
                return;
            }
        }
        const VkImageAspectFlags copyAspectMask =
            srcImage.aspect & dstImage.aspect &
            (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        MOBILEGL_ASSERT(copyAspectMask != 0 &&
                        (srcImage.aspect & copyAspectMask) == srcImage.aspect &&
                        (dstImage.aspect & copyAspectMask) == dstImage.aspect,
                        "CopyImageSubData source and destination aspects are incompatible.");
        const Uint32 srcMipLevel = static_cast<Uint32>(srcLevel);
        const Uint32 dstMipLevel = static_cast<Uint32>(dstLevel);
        const Uint32 srcMipWidth = std::max(1u, srcImage.extent.width >> srcMipLevel);
        const Uint32 srcMipHeight = std::max(1u, srcImage.extent.height >> srcMipLevel);
        const Uint32 dstMipWidth = std::max(1u, dstImage.extent.width >> dstMipLevel);
        const Uint32 dstMipHeight = std::max(1u, dstImage.extent.height >> dstMipLevel);
        // Promoted for the same reason as the level range above, and it is the same bug class:
        // a VkImageCopy whose region runs past the image is an out-of-bounds promise to the
        // driver, and the frontend does not check the region at all (there is a CTS sibling,
        // copy_image.exceeding_boundaries, that asks for exactly this input). Nothing legal is
        // lost by declining - a copy that reads or writes outside the image was never going to
        // produce a correct result, it was going to produce whatever the driver did next.
        if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0 ||
            static_cast<Uint32>(srcX + srcWidth) > srcMipWidth ||
            static_cast<Uint32>(srcY + srcHeight) > srcMipHeight ||
            static_cast<Uint32>(dstX + srcWidth) > dstMipWidth ||
            static_cast<Uint32>(dstY + srcHeight) > dstMipHeight) {
            MGLOG_E_ONCE("%s: region outside image bounds (src %dx%d+%d+%d of %ux%u, dst +%d+%d of %ux%u); "
                         "declining the copy",
                         __func__, srcWidth, srcHeight, srcX, srcY, srcMipWidth, srcMipHeight, dstX, dstY,
                         dstMipWidth, dstMipHeight);
            return;
        }

        // The supported envelope, replacing the "GL_TEXTURE_2D only" assertion that used to stand
        // here: every target whose slices this function can address on one of the two Vulkan axes.
        // A refusal has to be a real decline, not an assertion - the assertion compiled to nothing
        // in a release build and the unsupported shape reached vkCmdCopyImage anyway.
        CopyImageSliceMapping srcSlices;
        CopyImageSliceMapping dstSlices;
        if (!TryResolveCopyImageSliceMapping(srcTextureTarget, srcImage, srcMipLevel, srcZ, srcDepth, srcSlices) ||
            !TryResolveCopyImageSliceMapping(dstTextureTarget, dstImage, dstMipLevel, dstZ, srcDepth, dstSlices)) {
            MGLOG_E_ONCE("%s: unsupported target pair src=%s dst=%s (srcZ=%d dstZ=%d depth=%d); declining the copy",
                         __func__, MG_Util::ConvertTextureTargetToString(srcTextureTarget).c_str(),
                         MG_Util::ConvertTextureTargetToString(dstTextureTarget).c_str(), srcZ, dstZ, srcDepth);
            return;
        }
        // The slice half of the region-bounds guard above. A layered endpoint's bound is NOT the
        // mip-0 2D extent: an array texture is bounded by its layer count (which no mip level
        // shrinks) and a 3D texture by the selected level's depth (which every level halves), so
        // both come from the endpoint that resolved them.
        const Uint32 copySliceCount = static_cast<Uint32>(srcDepth);
        if (srcSlices.baseSlice + copySliceCount > srcSlices.availableSlices ||
            dstSlices.baseSlice + copySliceCount > dstSlices.availableSlices) {
            MGLOG_E_ONCE("%s: slice range outside image bounds (srcZ=%d of %u, dstZ=%d of %u, depth=%d); "
                         "declining the copy",
                         __func__, srcZ, srcSlices.availableSlices, dstZ, dstSlices.availableSlices, srcDepth);
            return;
        }
        // THE ONE IN-PLACE SHAPE THAT STAYS A DECLINE (§3.2). GL 4.6 core 18.3.2: if the source
        // and destination name the same image AND the same level AND the same layers, the result
        // is UNDEFINED where the regions overlap. GENERAL makes the command legal to record, not
        // meaningful to execute - vkCmdCopyImage has no ordering between its own reads and writes
        // within one region, so what a tiler produces is whichever half of each texel it reached
        // first. Declining says that, once, instead of shipping it.
        //
        // The test is ALL THREE dimensions at once: same level, overlapping slice range, and
        // overlapping rectangle. A copy from level 0 to level 1, or between disjoint layers, or
        // between disjoint rectangles of one level, is perfectly defined and is performed.
        if (inPlace && srcMipLevel == dstMipLevel) {
            const Uint32 srcSliceBegin = srcSlices.baseSlice, dstSliceBegin = dstSlices.baseSlice;
            const Bool slicesOverlap = srcSliceBegin < dstSliceBegin + copySliceCount &&
                                       dstSliceBegin < srcSliceBegin + copySliceCount;
            const Bool rectOverlaps = srcX < dstX + srcWidth && dstX < srcX + srcWidth &&
                                      srcY < dstY + srcHeight && dstY < srcY + srcHeight;
            if (slicesOverlap && rectOverlaps) {
                MGLOG_E_ONCE("%s: in-place copy on objectId=%u overlaps itself at level %u "
                             "(src %dx%d+%d+%d slice %u, dst +%d+%d slice %u, %u slice(s)); GL leaves "
                             "the result undefined, so the copy is declined rather than recorded",
                             __func__, CopyImageEndpointName(srcEndpoint), srcMipLevel,
                             srcWidth, srcHeight, srcX, srcY, srcSliceBegin, dstX, dstY,
                             dstSliceBegin, copySliceCount);
                return;
            }
        }

        // Syncing either endpoint can flush and retire the current command buffer for
        // a texture upload. Prepare recording only after both images and uploads are ready.
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const auto materializeClear = [this, &frame](const CopyImageEndpoint& endpoint) {
            return true;
        };
        const Bool clearReady = materializeClear(srcEndpoint);
        MOBILEGL_ASSERT(clearReady, "%s: failed to materialize pending clear for source objectId=%u",
                        __func__, CopyImageEndpointName(srcEndpoint));
        // A clear still parked on the destination would otherwise materialize AFTER this copy and
        // wipe the texels it just wrote.
        const Bool dstClearReady = materializeClear(dstEndpoint);
        MOBILEGL_ASSERT(dstClearReady, "%s: failed to materialize pending clear for destination objectId=%u",
                        __func__, CopyImageEndpointName(dstEndpoint));
        m_textureManager->StampTextureRecordingWrite(dstEndpoint.Texture.get());

        const VkImageLayout srcOriginalLayout = *srcImage.trackedLayout;
        const VkImageLayout dstOriginalLayout = *dstImage.trackedLayout;
        // A layout of UNDEFINED means nothing has ever been written to the image, which on the
        // SOURCE side is glTexStorage without an upload: legal GL, and the texels it copies are
        // undefined by the same spec sentence that lets the application ask. Both sides therefore
        // take the same shape - transition the whole image out of UNDEFINED and settle it on a
        // real layout afterwards, since UNDEFINED is not a layout a barrier may transition BACK to.
        // A renderbuffer settles on its ATTACHMENT layout instead: it is never sampled, and that is
        // the layout MaterializePendingClearForRenderbuffer leaves it in.
        const auto resolveRestoreLayout = [copyAspectMask](VkImageLayout originalLayout, Bool isRenderbuffer) {
            if (originalLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
                return originalLayout;
            }
            const Bool depthStencil =
                (copyAspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
            if (isRenderbuffer) {
                return depthStencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                    : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            return depthStencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        };
        const VkImageLayout srcRestoreLayout = resolveRestoreLayout(srcOriginalLayout, srcImage.isRenderbuffer);
        const VkImageLayout dstRestoreLayout = resolveRestoreLayout(dstOriginalLayout, dstImage.isRenderbuffer);

        // ONE IMAGE, ONE LAYOUT, ONE TRANSITION. For a same-image copy both endpoints are the same
        // VkImage behind the same tracked layout, so the two transitions below would describe one
        // subresource twice and the second would undo the first. GENERAL is the layout both sides
        // of vkCmdCopyImage accept at once, the barrier covers the WHOLE image (both the source
        // level and the destination level are in it), and the access mask carries both directions
        // because this one barrier is the edge for both.
        const VkImageLayout copySourceLayout =
            inPlace ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkImageLayout copyDestinationLayout =
            inPlace ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        const VkAccessFlags copySourceAccess =
            inPlace ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
                    : VK_ACCESS_TRANSFER_READ_BIT;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcOriginalLayout, srcStageMask, srcAccessMask);
        VkImageLayout srcCopyLayout = srcOriginalLayout;
        // The barriers below name a MIP range only. Their layer range is not a parameter:
        // TransitionImageLayout always covers every layer of the image, which is a superset of the
        // [baseSlice, baseSlice + depth) the slice mapping above hands the copy.
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool srcReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcImage.image, *srcImage.trackedLayout, copySourceLayout,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, copySourceAccess,
                srcImage.aspect, 0, srcImage.mipLevels);
            MOBILEGL_ASSERT(srcReady, "%s: failed to transition undefined source image", __func__);
            srcCopyLayout = *srcImage.trackedLayout;
        } else {
            Bool srcReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcImage.image, srcCopyLayout, copySourceLayout,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, copySourceAccess,
                inPlace ? srcImage.aspect : copyAspectMask,
                inPlace ? 0u : srcMipLevel, inPlace ? srcImage.mipLevels : 1u);
            MOBILEGL_ASSERT(srcReady, "%s: failed to transition source image", __func__);
        }

        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionSourceState(dstOriginalLayout, dstStageMask, dstAccessMask);
        VkImageLayout dstCopyLayout = dstOriginalLayout;
        if (inPlace) {
            // Already moved, by the barrier above: trackedLayout is the same field.
            dstCopyLayout = *dstImage.trackedLayout;
        } else if (dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstImage.image, *dstImage.trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                dstImage.aspect, 0, dstImage.mipLevels);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition undefined destination image", __func__);
            dstCopyLayout = *dstImage.trackedLayout;
        } else {
            Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstImage.image, dstCopyLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, copyAspectMask, dstMipLevel, 1);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition destination image", __func__);
        }

        // The GL slice count reaches Vulkan on the layer axis of whichever endpoint is NOT 3D, and
        // on extent.depth as soon as either endpoint IS: a 3D image's subresource is always the
        // single layer (0, 1) and its slices are counted by the depth of the copy extent. With two
        // non-3D endpoints both layer counts carry it and extent.depth stays 1.
        const Bool copyCrossesDepthAxis = srcSlices.slicesAreDepth || dstSlices.slicesAreDepth;
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = copyAspectMask;
        copyRegion.srcSubresource.mipLevel = srcMipLevel;
        copyRegion.srcSubresource.baseArrayLayer = srcSlices.BaseArrayLayer();
        copyRegion.srcSubresource.layerCount = srcSlices.slicesAreDepth ? 1u : copySliceCount;
        copyRegion.srcOffset = {srcX, srcY, srcSlices.OffsetZ()};
        copyRegion.dstSubresource.aspectMask = copyAspectMask;
        copyRegion.dstSubresource.mipLevel = dstMipLevel;
        copyRegion.dstSubresource.baseArrayLayer = dstSlices.BaseArrayLayer();
        copyRegion.dstSubresource.layerCount = dstSlices.slicesAreDepth ? 1u : copySliceCount;
        copyRegion.dstOffset = {dstX, dstY, dstSlices.OffsetZ()};
        copyRegion.extent = {static_cast<Uint32>(srcWidth), static_cast<Uint32>(srcHeight),
                             copyCrossesDepthAxis ? copySliceCount : 1u};
        MGLOG_D("CopyImageSubData: src(target=%s level=%u layer=%u+%u z=%d) -> dst(target=%s level=%u layer=%u+%u "
                "z=%d) extent=[%d x %d x %u]",
                MG_Util::ConvertTextureTargetToString(srcTextureTarget).c_str(), srcMipLevel,
                copyRegion.srcSubresource.baseArrayLayer, copyRegion.srcSubresource.layerCount,
                copyRegion.srcOffset.z, MG_Util::ConvertTextureTargetToString(dstTextureTarget).c_str(), dstMipLevel,
                copyRegion.dstSubresource.baseArrayLayer, copyRegion.dstSubresource.layerCount,
                copyRegion.dstOffset.z, srcWidth, srcHeight, copyRegion.extent.depth);
        vkCmdCopyImage(frame.commandBuffer,
                       srcImage.image, copySourceLayout,
                       dstImage.image, copyDestinationLayout,
                       1, &copyRegion);
        // The copy coordinates above are already storage-relative. Name that
        // storage here too, so a texture view's offsets are not applied twice.
        if (wire && !dstImage.isRenderbuffer) m_textureManager->MarkWireTextureGpuWritten(dstStorageHandle,
            dstMipLevel, dstSlices.BaseArrayLayer(), dstSlices.slicesAreDepth ? 1u : copySliceCount);

        VkPipelineStageFlags srcRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcRestoreAccessMask = 0;
        GetImageTransitionDestinationState(srcRestoreLayout, srcRestoreStageMask, srcRestoreAccessMask);
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool srcRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcImage.image, *srcImage.trackedLayout, srcRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                copySourceAccess, srcRestoreAccessMask,
                srcImage.aspect, 0, srcImage.mipLevels);
            MOBILEGL_ASSERT(srcRestored, "%s: failed to restore undefined source image layout", __func__);
        } else {
            Bool srcRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcImage.image, srcCopyLayout, srcRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                copySourceAccess, srcRestoreAccessMask,
                inPlace ? srcImage.aspect : copyAspectMask,
                inPlace ? 0u : srcMipLevel, inPlace ? srcImage.mipLevels : 1u);
            MOBILEGL_ASSERT(srcRestored, "%s: failed to restore source image layout", __func__);
        }

        VkPipelineStageFlags dstRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstRestoreAccessMask = 0;
        GetImageTransitionDestinationState(dstRestoreLayout, dstRestoreStageMask, dstRestoreAccessMask);
        if (inPlace) {
            // The restore above already moved the whole image off GENERAL, and the destination
            // is that same image: a second restore would barrier a layout the tracker has
            // already left, which is the mirror of the bug the retired Fatal was guarding.
        } else if (dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool dstRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstImage.image, *dstImage.trackedLayout, dstRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask,
                dstImage.aspect, 0, dstImage.mipLevels);
            MOBILEGL_ASSERT(dstRestored, "%s: failed to restore undefined destination image layout", __func__);
        } else {
            Bool dstRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstImage.image, dstCopyLayout, dstRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, copyAspectMask, dstMipLevel, 1);
            MOBILEGL_ASSERT(dstRestored, "%s: failed to restore destination image layout", __func__);
        }

    }

    Bool VulkanRenderer::SubmitReadbackCommandsAndWait(FrameContext::FrameData& frame) {
        if (frame.isCommandRecording) {
            m_frameContext.EndCommandRecording();
            frame.hasCommandBufferRecorded = true;
            InvalidatePipelineMemo(); // command-buffer boundary: drop the pipeline memo
        }
        // The pre-pass stream must never be submitted later than the recording
        // it was paired with (frame commands recorded after a pre-pass move
        // rely on the moved work having executed first).
        m_frameContext.EndPreCommandRecordingIfOpen();
        if (!frame.hasCommandBufferRecorded && !frame.hasPreCommandBufferRecorded) {
            return true;
        }

        if (!SubmitPendingCommandBuffer(frame, frame.imageInFlightFence, /*pooledFence=*/false)) {
            return false;
        }

        // EVERY OUTSTANDING SUBMISSION, NOT ONLY THE ONE JUST MADE (P7 wave 2-B, exit gate 3).
        //
        // This used to wait one fence and then call OnSubmitsCompletedUpTo(frame.lastSubmitIndex),
        // and the comment below said the wait "proved every submission complete". On the monolith
        // arm that is true - nothing flushes before a readback, so the frame's draws and the copy
        // are one submission under one fence. On the WIRE arm it is not: ReadWirePixels calls
        // FlushPendingCommands first, which submits the draws under a POOLED FENCE OF ITS OWN and
        // returns without waiting, so the copy is a later, separately fenced submission. Two
        // submissions on one queue start in order; they do not finish in order unless something
        // makes them, and a fence says nothing about a submission it does not belong to.
        //
        // Measured, on this host, at the copy: submit=72 completed=71. The old code then called
        // OnSubmitsCompletedUpTo(73), which RECORDED 72 as complete and retired its fence - so
        // after the fact nothing could even observe that it had not been waited for.
        //
        // The cost is nothing a readback does not already pay: it is a full CPU stall by
        // construction, and the fences gathered here are exactly the ones already in flight.
        //
        // UNDER #if, although this function serves both arms of the disaggregated build: the PULL
        // build has no wire arm, cannot flush before a readback, and is the image G1 pins to
        // 0xa52203. Its single-fence wait stays the statement it always was.
        Vector<VkFence> readbackFences;
        readbackFences.reserve(m_inFlightSubmits.size() + 1);
        for (const auto& record : m_inFlightSubmits) {
            if (record.fence != VK_NULL_HANDLE && record.submitIndex <= frame.lastSubmitIndex) {
                readbackFences.push_back(record.fence);
            }
        }
        if (readbackFences.empty()) {
            readbackFences.push_back(frame.imageInFlightFence);
        }
        // Measured here on the host (OpenRA, DirectVulkan, inproc, at the frame readback):
        // fences=2 upTo=73 submitCounter=73 completed=71. Two fences, because submission 72 -
        // the frame's draws, flushed by ReadWirePixels under a pooled fence - was still
        // outstanding; the code this replaces waited on one.
        VkResult result = vkWaitForFences(m_device, static_cast<Uint32>(readbackFences.size()),
                                          readbackFences.data(), VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            NoteDeviceLoss(result, "readback vkWaitForFences");
            MGLOG_E_ONCE("DirectVulkan readback: vkWaitForFences returned %d", result);
            return false;
        }
        OnSubmitsCompletedUpTo(frame.lastSubmitIndex);
        result = vkResetFences(m_device, 1, &frame.imageInFlightFence);
        if (result != VK_SUCCESS) {
            MGLOG_E_ONCE("DirectVulkan readback: vkResetFences returned %d", result);
            return false;
        }

        frame.hasCommandBufferRecorded = false;
        frame.isCommandRecording = false;
        // The wait above covered every submission at or below this one, so the full
        // frame-boundary drain applies: descriptor cursors, transient arenas, deferred
        // texture/buffer releases, retired command buffers and the converted
        // vertex-stream cache all rewind here, keeping present-less readback
        // loops bounded (Present is the only other drain point).
        TryDrainFrameTransients();
        return true;
    }

    void VulkanRenderer::ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                    void* pixels) {
        ReadWirePixels(x,y,width,height,format,type,pixels);
        return;
    }

    Bool VulkanRenderer::BlitDepthAcrossFormats(FrameContext::FrameData& frame, VkImage srcImage, VkFormat srcFormat,
                                                VkImageLayout* srcTrackedLayout, Uint32 srcMipLevel,
                                                Uint32 srcBaseArrayLayer, VkImage dstImage, VkFormat dstFormat,
                                                VkImageLayout* dstTrackedLayout, Uint32 dstMipLevel,
                                                Uint32 dstBaseArrayLayer, GLint srcX, GLint srcY, GLint dstX,
                                                GLint dstY, GLint width, GLint height,
                                                VkImageLayout srcRestoreLayout, VkImageLayout dstRestoreLayout,
                                                Bool stencilAspect) {
        const auto aspectMaskForFormat = [](VkFormat format) -> VkImageAspectFlags {
            switch (format) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D32_SFLOAT:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
            }
        };
        const auto depthTexelSize = [](VkFormat format) -> SizeT {
            switch (format) {
            case VK_FORMAT_D16_UNORM:
                return 2;
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return 4;
            default:
                return 0;
            }
        };
        // The stencil aspect of every supported format copies as one byte per texel,
        // so a cross-format stencil "blit" is a raw pass-through.
        const SizeT srcTexel = stencilAspect ? 1 : depthTexelSize(srcFormat);
        const SizeT dstTexel = stencilAspect ? 1 : depthTexelSize(dstFormat);
        if (srcTexel == 0 || dstTexel == 0 || width <= 0 || height <= 0) {
            MGLOG_E_ONCE("BlitDepthAcrossFormats skipped: unsupported formats src=%d dst=%d",
                    static_cast<Int>(srcFormat), static_cast<Int>(dstFormat));
            return false;
        }
        const SizeT pixelCount = static_cast<SizeT>(width) * static_cast<SizeT>(height);

        VkBufferObject readback;
        if (!readback.Create({
                .allocator = m_allocator,
                .size = pixelCount * srcTexel,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            })) {
            return false;
        }

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(*srcTrackedLayout, srcStageMask, srcAccessMask);
        Bool ok = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, srcImage, *srcTrackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcStageMask,
            VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT,
            aspectMaskForFormat(srcFormat), srcMipLevel, 1);
        MOBILEGL_ASSERT(ok, "BlitDepthAcrossFormats: source transition failed");

        VkBufferImageCopy readRegion{};
        readRegion.imageSubresource.aspectMask =
            stencilAspect ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        readRegion.imageSubresource.mipLevel = srcMipLevel;
        readRegion.imageSubresource.baseArrayLayer = srcBaseArrayLayer;
        readRegion.imageSubresource.layerCount = 1;
        readRegion.imageOffset = {srcX, srcY, 0};
        readRegion.imageExtent = {static_cast<Uint32>(width), static_cast<Uint32>(height), 1};
        vkCmdCopyImageToBuffer(frame.commandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.GetHandle(), 1, &readRegion);

        VkPipelineStageFlags srcRestoreStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcRestoreAccess = 0;
        GetImageTransitionDestinationState(srcRestoreLayout, srcRestoreStage, srcRestoreAccess);
        ok = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, srcImage, *srcTrackedLayout, srcRestoreLayout, VK_PIPELINE_STAGE_TRANSFER_BIT,
            srcRestoreStage, VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccess,
            aspectMaskForFormat(srcFormat), srcMipLevel, 1);
        MOBILEGL_ASSERT(ok, "BlitDepthAcrossFormats: source restore failed");

        if (!SubmitReadbackCommandsAndWait(frame)) {
            return false;
        }
        const auto* mapped = static_cast<const Uint8*>(readback.Map());
        if (mapped == nullptr || !readback.Invalidate(pixelCount * srcTexel)) {
            return false;
        }

        // Decode source depths to float, re-encode into the destination texel layout.
        Vector<Uint8> encoded(pixelCount * dstTexel);
        if (stencilAspect) {
            Memcpy(encoded.data(), mapped, pixelCount);
        }
        for (SizeT i = 0; !stencilAspect && i < pixelCount; ++i) {
            Float depthValue = 0.0f;
            switch (srcFormat) {
            case VK_FORMAT_D16_UNORM: {
                Uint16 raw = 0;
                Memcpy(&raw, mapped + i * 2, sizeof(raw));
                depthValue = static_cast<Float>(raw) / 65535.0f;
                break;
            }
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D24_UNORM_S8_UINT: {
                Uint32 raw = 0;
                Memcpy(&raw, mapped + i * 4, sizeof(raw));
                depthValue = static_cast<Float>(raw & 0xFFFFFFu) / static_cast<Float>(0xFFFFFFu);
                break;
            }
            default: {
                Memcpy(&depthValue, mapped + i * 4, sizeof(depthValue));
                break;
            }
            }
            Uint8* dst = encoded.data() + i * dstTexel;
            switch (dstFormat) {
            case VK_FORMAT_D16_UNORM: {
                const Uint16 value =
                    static_cast<Uint16>(std::lround(static_cast<double>(std::clamp(depthValue, 0.0f, 1.0f)) * 65535.0));
                Memcpy(dst, &value, sizeof(value));
                break;
            }
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D24_UNORM_S8_UINT: {
                const Uint32 value = static_cast<Uint32>(
                    std::lround(static_cast<double>(std::clamp(depthValue, 0.0f, 1.0f)) * 16777215.0));
                Memcpy(dst, &value, sizeof(value));
                break;
            }
            default:
                Memcpy(dst, &depthValue, sizeof(depthValue));
                break;
            }
        }

        // Upload the converted region; recording restarted after the readback flush.
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        BufferSlice slice{};
        if (!m_bufferManager.UploadTransient(BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(), encoded.data(),
                                             encoded.size(), 4, slice)) {
            MGLOG_E_ONCE("BlitDepthAcrossFormats: staging upload failed");
            return false;
        }

        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionSourceState(*dstTrackedLayout, dstStageMask, dstAccessMask);
        ok = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, dstImage, *dstTrackedLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstStageMask,
            VK_PIPELINE_STAGE_TRANSFER_BIT, dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
            aspectMaskForFormat(dstFormat), dstMipLevel, 1);
        MOBILEGL_ASSERT(ok, "BlitDepthAcrossFormats: destination transition failed");

        VkBufferImageCopy writeRegion{};
        writeRegion.bufferOffset = slice.offset;
        writeRegion.imageSubresource.aspectMask =
            stencilAspect ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        writeRegion.imageSubresource.mipLevel = dstMipLevel;
        writeRegion.imageSubresource.baseArrayLayer = dstBaseArrayLayer;
        writeRegion.imageSubresource.layerCount = 1;
        writeRegion.imageOffset = {dstX, dstY, 0};
        writeRegion.imageExtent = {static_cast<Uint32>(width), static_cast<Uint32>(height), 1};
        vkCmdCopyBufferToImage(frame.commandBuffer, slice.buffer, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &writeRegion);

        VkPipelineStageFlags dstRestoreStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstRestoreAccess = 0;
        GetImageTransitionDestinationState(dstRestoreLayout, dstRestoreStage, dstRestoreAccess);
        ok = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, dstImage, *dstTrackedLayout, dstRestoreLayout, VK_PIPELINE_STAGE_TRANSFER_BIT,
            dstRestoreStage, VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccess,
            aspectMaskForFormat(dstFormat), dstMipLevel, 1);
        MOBILEGL_ASSERT(ok, "BlitDepthAcrossFormats: destination restore failed");
        return true;
    }

    void VulkanRenderer::ReadDepthStencilImageToClient(VkImage image, VkFormat vkFormat, VkImageLayout* trackedLayout,
                                                       VkImageAspectFlags imageAspect, Uint32 mipLevel,
                                                       Uint32 baseArrayLayer, GLint x, GLint y, GLsizei width,
                                                       GLsizei height, GLenum format, GLenum type, void* pixels,
                                                       Bool defaultFramebufferOrientation, Uint32 sourceLayerCount) {
        const Bool wantDepth = format != GL_STENCIL_INDEX;
        const Bool wantStencil = format != GL_DEPTH_COMPONENT;
        auto& frame = m_frameContext.GetCurrent();

        if (*trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: source layout is undefined");
            return;
        }
        if (wantDepth && (imageAspect & VK_IMAGE_ASPECT_DEPTH_BIT) == 0) {
            MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: attachment has no depth aspect");
            return;
        }
        if (wantStencil && (imageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) == 0) {
            MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: attachment has no stencil aspect");
            return;
        }

        // Per-aspect buffer-copy texel sizes (Vulkan defines the depth aspect of packed
        // formats to copy as its own tightly defined layout).
        SizeT depthCopyBytes = 0;
        switch (vkFormat) {
        case VK_FORMAT_D16_UNORM:
            depthCopyBytes = 2;
            break;
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            depthCopyBytes = 4;
            break;
        case VK_FORMAT_S8_UINT:
            break;
        default:
            MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: unsupported source format=%d",
                    static_cast<Int>(vkFormat));
            return;
        }

        const SizeT pixelCount = static_cast<SizeT>(width) * static_cast<SizeT>(height);
        const VkDeviceSize depthBytes = wantDepth ? pixelCount * depthCopyBytes : 0;
        // Buffer offsets for depth/stencil copies must be 4-byte aligned.
        const VkDeviceSize stencilOffset = (depthBytes + 3) & ~VkDeviceSize{3};
        const VkDeviceSize stencilBytes = wantStencil ? pixelCount : 0;
        VkBufferObject readback;
        if (!readback.Create({
                .allocator = m_allocator,
                .size = stencilOffset + stencilBytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            })) {
            MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: failed to create readback buffer");
            return;
        }

        const VkImageLayout originalLayout = *trackedLayout;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(originalLayout, srcStageMask, srcAccessMask);
        Bool ok = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, image, *trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcStageMask,
            VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, imageAspect, mipLevel, 1);
        MOBILEGL_ASSERT(ok, "%s: failed to transition depth-stencil source image", __func__);

        // The swapchain's depth/stencil image is stored display-side-up like its colour twin, so
        // the GL rect has to be mapped into that space before the copy and the copied rows
        // re-oriented afterwards - the same two halves the colour ReadPixels path applies.
        VkOffset2D copyOffset{x, y};
        VkExtent2D copyExtent{static_cast<Uint32>(width), static_cast<Uint32>(height)};
        if (defaultFramebufferOrientation) {
            const VkExtent2D defaultFboExtent = m_swapchainObject.GetExtent();
            const Bool mapped = MapDefaultFramebufferReadbackRect(
                x, y, width, height, defaultFboExtent, m_swapchainObject.GetPreTransform(), &copyOffset,
                &copyExtent);
            MOBILEGL_ASSERT(mapped, "ReadDepthStencilPixels: default framebuffer read rectangle is out of bounds");
            if (!mapped) return;
        }

        // See the header: a stack of one-row layers and a single multi-row layer copy out to the
        // same tightly-packed bytes, so only the region's shape splits the two cases.
        const Uint32 copyLayerCount = std::max<Uint32>(sourceLayerCount, 1u);
        const Uint32 copyRowCount = copyLayerCount > 1u ? 1u : copyExtent.height;
        VkBufferImageCopy regions[2]{};
        Uint32 regionCount = 0;
        if (wantDepth) {
            auto& region = regions[regionCount++];
            region.bufferOffset = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            region.imageSubresource.mipLevel = mipLevel;
            region.imageSubresource.baseArrayLayer = baseArrayLayer;
            region.imageSubresource.layerCount = copyLayerCount;
            region.imageOffset = {copyOffset.x, copyOffset.y, 0};
            region.imageExtent = {copyExtent.width, copyRowCount, 1};
        }
        if (wantStencil) {
            auto& region = regions[regionCount++];
            region.bufferOffset = stencilOffset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            region.imageSubresource.mipLevel = mipLevel;
            region.imageSubresource.baseArrayLayer = baseArrayLayer;
            region.imageSubresource.layerCount = copyLayerCount;
            region.imageOffset = {copyOffset.x, copyOffset.y, 0};
            region.imageExtent = {copyExtent.width, copyRowCount, 1};
        }
        vkCmdCopyImageToBuffer(frame.commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.GetHandle(),
                               regionCount, regions);

        VkPipelineStageFlags restoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags restoreAccessMask = 0;
        GetImageTransitionDestinationState(originalLayout, restoreStageMask, restoreAccessMask);
        ok = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, image, *trackedLayout, originalLayout, VK_PIPELINE_STAGE_TRANSFER_BIT,
            restoreStageMask, VK_ACCESS_TRANSFER_READ_BIT, restoreAccessMask, imageAspect, mipLevel, 1);
        MOBILEGL_ASSERT(ok, "%s: failed to restore depth-stencil source image layout", __func__);

        if (!SubmitReadbackCommandsAndWait(frame)) {
            return;
        }
        const auto* mapped = static_cast<const Uint8*>(readback.Map());
        if (mapped == nullptr || !readback.Invalidate(stencilOffset + stencilBytes)) {
            MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: failed to map readback buffer");
            return;
        }
        const Uint8* depthSrc = mapped;
        const Uint8* stencilSrc = mapped + stencilOffset;

        // Re-orient the copied band per aspect, before any repacking reads it: the depth and
        // stencil aspects were copied into their own tightly packed sub-buffers, so each is a
        // plain width x height image of its own texel size.
        Vector<Uint8> remappedDepth;
        Vector<Uint8> remappedStencil;
        if (defaultFramebufferOrientation) {
            const VkSurfaceTransformFlagBitsKHR preTransform = m_swapchainObject.GetPreTransform();
            Bool remapped = true;
            if (wantDepth && depthCopyBytes > 0) {
                remappedDepth.resize(pixelCount * depthCopyBytes);
                remapped = RemapDefaultFramebufferReadback(depthSrc, static_cast<Uint32>(width),
                                                           static_cast<Uint32>(height), preTransform,
                                                           depthCopyBytes, remappedDepth.data());
            }
            if (remapped && wantStencil) {
                remappedStencil.resize(pixelCount);
                remapped = RemapDefaultFramebufferReadback(stencilSrc, static_cast<Uint32>(width),
                                                           static_cast<Uint32>(height), preTransform, 1,
                                                           remappedStencil.data());
            }
            if (remapped) {
                if (!remappedDepth.empty()) depthSrc = remappedDepth.data();
                if (!remappedStencil.empty()) stencilSrc = remappedStencil.data();
            } else {
                MGLOG_D("DirectVulkan::ReadDepthStencilPixels: default-FBO remap failed (w=%d h=%d "
                        "preTransform=%d); falling back to raw readback",
                        width, height, static_cast<Int>(preTransform));
            }
        }

        // The packing is the SHARED PackDepthStencilTight, so this arm and the wire arm cannot
        // disagree about the encoding again: one aspect or both, in the client's own width, with
        // the signed integer widths scaled into their own signed range and GL_HALF_FLOAT written
        // as the half word the caller asked for.
        Vector<Uint8> packed;
        if (!PackDepthStencilTight(wantDepth ? depthSrc : nullptr, wantStencil ? stencilSrc : nullptr, vkFormat,
                                   pixelCount, format, type, packed)) {
            MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: unsupported type=0x%x", type);
            return;
        }
        const SizeT dstPixelBytes = packed.size() / pixelCount;

        // Store honoring the client pack state (single slice).
        const SharedPtr<MG_State::GLState::BufferObject> wireReplyHasNoPackBuffer;
        const auto& pixelPackBufferObject =
            wireReplyHasNoPackBuffer;
        const auto packParams = MG_Pipe::gPipeInputs.GetPixelStoreParameters(false);
        const SizeT rowPixels = static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : width);
        const SizeT packAlignment = packParams.Alignment > 0 ? static_cast<SizeT>(packParams.Alignment) : 1;
        const SizeT dstRowStride = ((rowPixels * dstPixelBytes) + packAlignment - 1) / packAlignment * packAlignment;
        const SizeT dstSkipOffset = static_cast<SizeT>(std::max(packParams.SkipRows, 0)) * dstRowStride +
            static_cast<SizeT>(std::max(packParams.SkipPixels, 0)) * dstPixelBytes;
        const SizeT dstRowBytes = static_cast<SizeT>(width) * dstPixelBytes;
        const SizeT pboBaseOffset = reinterpret_cast<SizeT>(pixels);
        if (pixelPackBufferObject != nullptr) {
            const SizeT requiredSize =
                pboBaseOffset + dstSkipOffset + static_cast<SizeT>(height - 1) * dstRowStride + dstRowBytes;
            if (requiredSize > pixelPackBufferObject->GetSize()) {
                MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: pixel pack buffer is too small");
                return;
            }
        }
        for (GLsizei row = 0; row < height; ++row) {
            Uint8* srcRow = packed.data() + static_cast<SizeT>(row) * dstRowBytes;
            const SizeT dstOffset = dstSkipOffset + static_cast<SizeT>(row) * dstRowStride;
            if (pixelPackBufferObject != nullptr) {
                pixelPackBufferObject->WritebackFromBackend({srcRow, dstRowBytes}, pboBaseOffset + dstOffset);
            } else {
                Memcpy(static_cast<Uint8*>(pixels) + dstOffset, srcRow, dstRowBytes);
            }
        }
    }

    void VulkanRenderer::GenerateMipmap(GLenum target) {
        GenerateWireMipmap();
        return;
    }

    // Keyed on the frontend's never-reused lifetime id, NOT on the GL name. The name is
    // recycled the moment glDeleteTransformFeedbacks gives it back, so a name-keyed slot
    // handed a brand-new object the counter group - and the m_xfbCountersValid /
    // m_xfbLastSeenGeneration entries - of the object that died under that name.
    //
    // Slots are never handed back (there is no backend entry telling this renderer that a span
    // closed - registering the EndTransformFeedback one would flip the "captures through its own
    // driver" test FixupGsStripCaptureOrder makes of it), so once all sixteen are owned a new
    // object has to take one over. The victim is chosen among owners with NO OPEN SPAN: an object
    // whose span is closed, or which no longer exists at all, can never resume, so its counter
    // bytes are dead. Least-recently-used ALONE would be exactly the wrong rule - GL only permits
    // another object to capture while this one is PAUSED, so the paused span whose counters the
    // slots exist to protect is by construction the least recently used entry. Taking a group over
    // resets its counter state, because those bytes describe the previous owner's span.
    Uint32 VulkanRenderer::CurrentXfbCounterSlot() {
        constexpr Uint32 kNoSlot = static_cast<Uint32>(kXfbCounterObjectSlots);
        const Uint64 identity = MG_Pipe::gPipeInputs.GetBoundTransformFeedbackLifetimeId();
        MOBILEGL_ASSERT(identity != 0,
                        "transform feedback object reported the free-slot sentinel (0) as its identity - "
                        "every slot would then read as 'mine' without ever being claimed");
        Uint32 freeSlot = kNoSlot;
        for (Uint32 slot = 0; slot < kNoSlot; ++slot) {
            if (m_xfbCounterSlotOwner[slot] == identity) {
                m_xfbCounterSlotLastUse[slot] = ++m_xfbCounterSlotUseSerial;
                return slot;
            }
            if (m_xfbCounterSlotOwner[slot] == 0 && freeSlot == kNoSlot) {
                freeSlot = slot;
            }
        }
        Uint32 slot = freeSlot;
        if (slot == kNoSlot) {
            for (Uint32 candidate = 0; candidate < kNoSlot; ++candidate) {
                if (m_xfbCounterSlotOwner[candidate] != 0 &&
                    MG_Pipe::MGPipeApplier().StreamOutputSpans.count(m_xfbCounterSlotOwner[candidate]) != 0) {
                    continue;
                }
                if (slot == kNoSlot || m_xfbCounterSlotLastUse[candidate] < m_xfbCounterSlotLastUse[slot]) {
                    slot = candidate;
                }
            }
        }
        if (slot == kNoSlot) {
            // Sixteen capture spans open at once. Whatever is taken loses its resume offset and
            // restarts at byte 0 of its capture buffers, which is a wrong picture rather than a
            // slow one - hence a report rather than a silent choice.
            MGLOG_E_ONCE("CurrentXfbCounterSlot: all %zu counter groups belong to transform feedback objects "
                         "with an open capture span; the least recently used one is taken over and that span "
                         "will restart at offset 0 instead of appending",
                         kXfbCounterObjectSlots);
            slot = 0;
            for (Uint32 candidate = 1; candidate < kNoSlot; ++candidate) {
                if (m_xfbCounterSlotLastUse[candidate] < m_xfbCounterSlotLastUse[slot]) {
                    slot = candidate;
                }
            }
        }
        m_xfbCounterSlotOwner[slot] = identity;
        m_xfbCounterSlotLastUse[slot] = ++m_xfbCounterSlotUseSerial;
        m_xfbCountersValid[slot] = false;
        m_xfbLastSeenGeneration[slot] = 0;
        return slot;
    }

    Bool VulkanRenderer::BeginXfbCaptureForDraw(FrameContext::FrameData& frame) {
        if (!m_transformFeedbackFeatureEnabled || !MG_Pipe::gPipeInputs.IsLive() ||
            !MG_Pipe::gPipeInputs.IsTransformFeedbackActive()) {
            return false;
        }
        // A paused span captures nothing, and the counter buffers keep their values, so the
        // next resumed draw appends exactly where the last captured one stopped - which is
        // what pause/resume means (ARB_transform_feedback2).
        if (MG_Pipe::gPipeInputs.IsTransformFeedbackPaused()) {
            return false;
        }
        // The bound pipeline's last pre-rasterization stage has to have been declared with Xfb
        // (VUID-vkCmdBeginTransformFeedbackEXT-None-04128). Everything above this line reads GL
        // state, which cannot answer that: a program can be built as a capture variant and still
        // end up with a module carrying no Xfb mode - the clip/XFB validation backstop rewinding
        // past the decoration, or XfbCaptureDecoratePass resolving none of the requested varyings
        // and changing nothing. Declining the span leaves the capture buffers untouched, which is
        // the same nothing the driver would have written, without the undefined behaviour.
        if (m_currentDrawXfbCaptureDeclined) {
            MGLOG_E_ONCE("BeginXfbCaptureForDraw: declining the capture span - the bound program's last "
                         "pre-rasterization stage carries no Xfb execution mode, so recording one would be "
                         "undefined behaviour rather than a capture");
            return false;
        }
        return BeginWireXfbCaptureForDraw(frame);
    }

    Bool VulkanRenderer::BeginXfbCaptureWithBuffers(FrameContext::FrameData& frame, Uint32 bufferCount,
            const VkBuffer* buffers, const VkDeviceSize* offsets, const VkDeviceSize* sizes) {
        if (!m_xfbCounterBuffer.IsValid()) {
            if (!m_xfbCounterBuffer.Create({
                    .allocator = m_allocator,
                    .size = 16 * kXfbCounterObjectSlots,
                    .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                })) {
                MGLOG_E_ONCE("BeginXfbCaptureForDraw: failed to create the counter buffer");
                return false;
            }
        }
        m_currentDrawXfbBufferCount = bufferCount;
        m_currentDrawXfbBufferMask = 0;
        for (Uint32 i = 0; i < bufferCount; ++i) {
            if (buffers[i] == VK_NULL_HANDLE) continue;
            m_currentDrawXfbBufferMask |= 1u << i;
            s_vkCmdBindTransformFeedbackBuffersEXT(frame.commandBuffer, i, 1, buffers + i,
                                                   offsets + i, sizes + i);
        }

        const Uint32 counterSlot = CurrentXfbCounterSlot();
        const Uint64 generation = MG_Pipe::gPipeInputs.GetTransformFeedbackGeneration();
        const Bool resume = m_xfbCountersValid[counterSlot] && m_xfbLastSeenGeneration[counterSlot] == generation;
        m_xfbLastSeenGeneration[counterSlot] = generation;

        VkBuffer counterBuffers[4] = {};
        VkDeviceSize counterOffsets[4] = {};
        for (SizeT i = 0; i < bufferCount; ++i) {
            if (!(m_currentDrawXfbBufferMask & (1u << i))) continue;
            counterBuffers[i] = m_xfbCounterBuffer.GetHandle();
            counterOffsets[i] = static_cast<VkDeviceSize>(counterSlot) * 16 + static_cast<VkDeviceSize>(i) * 4;
        }
        if (resume) {
            s_vkCmdBeginTransformFeedbackEXT(frame.commandBuffer, 0, static_cast<Uint32>(bufferCount),
                                             counterBuffers, counterOffsets);
        } else {
            s_vkCmdBeginTransformFeedbackEXT(frame.commandBuffer, 0, 0, nullptr, nullptr);
        }
        return true;
    }

    void VulkanRenderer::EndXfbCaptureForDraw(FrameContext::FrameData& frame, Bool began) {
        if (!began) {
            return;
        }
        // Exactly the targets used by this draw's Begin, independent of frontend
        // objects and of whether the span came from the wire or monolith state.
        const Uint32 bufferCount = m_currentDrawXfbBufferCount;
        const Uint32 counterSlot = CurrentXfbCounterSlot();
        VkBuffer counterBuffers[4] = {};
        VkDeviceSize counterOffsets[4] = {};
        for (SizeT i = 0; i < bufferCount; ++i) {
            if (!(m_currentDrawXfbBufferMask & (1u << i))) continue;
            counterBuffers[i] = m_xfbCounterBuffer.GetHandle();
            counterOffsets[i] = static_cast<VkDeviceSize>(counterSlot) * 16 + static_cast<VkDeviceSize>(i) * 4;
        }
        s_vkCmdEndTransformFeedbackEXT(frame.commandBuffer, 0, static_cast<Uint32>(bufferCount), counterBuffers,
                                       counterOffsets);
        m_xfbCountersValid[counterSlot] = true;
        m_xfbWritesPendingVisibility = true;
    }

    // GL makes transform feedback results visible to every later command on their own, with no
    // glMemoryBarrier in between - unlike shader storage writes, which is why the barrier the
    // Vulkan memory model requires has to be supplied here rather than by the application. It
    // cannot be recorded where the write happens (inside the capturing draw's render pass, which
    // declares no self-dependency), so it is emitted at the next point that could read the
    // captured buffer: the following draw, or a readback.
    void VulkanRenderer::MakeXfbWritesVisible() {
        if (!m_xfbWritesPendingVisibility) {
            return;
        }
        m_xfbWritesPendingVisibility = false;
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }
        VkMemoryBarrier memoryBarrier{};
        memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask =
                VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
        // Every way a captured buffer can be read back: replayed as vertex attributes or indices
        // by glDrawTransformFeedback, sampled through a uniform or storage binding, sourced as an
        // indirect command, copied out, or mapped.
        memoryBarrier.dstAccessMask =
                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT |
                VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
    }

    void VulkanRenderer::DrawArrays(const DrawCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        if (!SetupDraw(frame, payload.mode, 0, payload.params)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        VkCommandBuffer& commandBuffer = frame.commandBuffer;

        const Bool xfbActive = BeginXfbCaptureForDraw(frame);
        BeginXfbQueryForDraw(commandBuffer, xfbActive);
        const Bool occlusionActive = BeginOcclusionForDraw(commandBuffer);
        vkCmdDraw(commandBuffer,
            payload.params.vertexCount,
            payload.params.instanceCount,
            payload.params.firstVertex,
            payload.params.firstInstance);
        EndOcclusionForDraw(commandBuffer, occlusionActive);
        EndXfbCaptureForDraw(frame, xfbActive);
        EndXfbQueryForDraw(commandBuffer);
    }

    Bool VulkanRenderer::StartOcclusionQueryCapture() {
        if (!m_hostQueryResetEnabled || s_vkResetQueryPool == nullptr) {
            return false;
        }
        if (m_occlusionQueryPool == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
            poolInfo.queryCount = kOcclusionQuerySlots;
            if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_occlusionQueryPool) != VK_SUCCESS) {
                MGLOG_E_ONCE("StartOcclusionQueryCapture: vkCreateQueryPool failed");
                m_occlusionQueryPool = VK_NULL_HANDLE;
                return false;
            }
            s_vkResetQueryPool(m_device, m_occlusionQueryPool, 0, kOcclusionQuerySlots);
        }
        m_occlusionActiveSlots.clear();
        m_occlusionCaptureActive = true;
        return true;
    }

    void VulkanRenderer::StopOcclusionQueryCapture(Vector<Uint32>& outSlots) {
        outSlots = Move(m_occlusionActiveSlots);
        m_occlusionActiveSlots.clear();
        m_occlusionCaptureActive = false;
    }

    Bool VulkanRenderer::ResolveOcclusionQueryResult(const Vector<Uint32>& slots, Uint64& outSamples) {
        outSamples = 0;
        if (slots.empty()) {
            return true;
        }
        if (m_occlusionQueryPool == VK_NULL_HANDLE) {
            return true;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (frame.isCommandRecording) {
            if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
                VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            }
            if (!SubmitReadbackCommandsAndWait(frame)) {
                return false;
            }
        }
        for (const Uint32 slot : slots) {
            Uint64 value = 0;
            const VkResult result =
                vkGetQueryPoolResults(m_device, m_occlusionQueryPool, slot, 1, sizeof(value), &value, sizeof(value),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (result == VK_SUCCESS) {
                outSamples += value;
            }
            s_vkResetQueryPool(m_device, m_occlusionQueryPool, slot, 1);
        }
        return true;
    }

    Bool VulkanRenderer::StartXfbQueryCapture(Uint32 kind) {
        if (!m_xfbQueriesSupported || !m_hostQueryResetEnabled || s_vkResetQueryPool == nullptr ||
            s_vkCmdBeginQueryIndexedEXT == nullptr || kind > 1) {
            return false;
        }
        if (m_xfbQueryPool == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            poolInfo.queryType = VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT;
            poolInfo.queryCount = kXfbQuerySlots;
            if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_xfbQueryPool) != VK_SUCCESS) {
                MGLOG_E_ONCE("StartXfbQueryCapture: vkCreateQueryPool failed");
                m_xfbQueryPool = VK_NULL_HANDLE;
                return false;
            }
            s_vkResetQueryPool(m_device, m_xfbQueryPool, 0, kXfbQuerySlots);
        }
        // The reroute pool, on the first GENERATED span that needs it. A creation
        // failure disarms rather than failing the capture: the stream path still
        // answers (with the driver's defect), which beats answering nothing.
        if (kind == 1 && m_primGenRerouteKind != MG_Util::SelfTest::PrimGenRerouteKind::None &&
            m_primGenReroutePool == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            poolInfo.queryCount = kXfbQuerySlots;
            if (m_primGenRerouteKind == MG_Util::SelfTest::PrimGenRerouteKind::PrimitivesGeneratedExt) {
                // The query Vulkan defines for this GL target; counts vertex stream 0
                // when begun with plain vkCmdBeginQuery.
                poolInfo.queryType = VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT;
            } else {
                poolInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                // The clipping-stage INVOCATION counter: one per primitive reaching
                // primitive clipping (GL's CLIPPING_INPUT_PRIMITIVES) - post-tess/GS,
                // pre-clip, and per spec still counted under rasterizer discard, which
                // is exactly the set GL_PRIMITIVES_GENERATED is defined over. The
                // stage's OUTPUT count (CLIPPING_PRIMITIVES_BIT) would be wrong:
                // clipping may drop or split primitives.
                poolInfo.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT;
            }
            if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_primGenReroutePool) != VK_SUCCESS) {
                MGLOG_E_ONCE("StartXfbQueryCapture: reroute pool creation failed; the "
                             "PRIMITIVES_GENERATED reroute is disarmed and XFB-inactive draws keep "
                             "the stream query");
                m_primGenReroutePool = VK_NULL_HANDLE;
                m_primGenRerouteKind = MG_Util::SelfTest::PrimGenRerouteKind::None;
            } else {
                s_vkResetQueryPool(m_device, m_primGenReroutePool, 0, kXfbQuerySlots);
            }
        }
        m_xfbQueryActiveSlots[kind].clear();
        m_xfbQueryCaptureActive[kind] = true;
        if (kind == 1) {
            m_primGenRerouteActiveSlots.clear();
        }
        return true;
    }

    Bool VulkanRenderer::ArePausedDrawsGpuCounted() const {
        // Exactly the gate BeginXfbQueryForDraw applies per draw, so a span told "armed"
        // really does get a reroute slot for every draw with no open capture - a paused
        // span's draws included.
        const Bool rerouteArmed = m_primGenRerouteKind != MG_Util::SelfTest::PrimGenRerouteKind::None &&
                                  m_primGenReroutePool != VK_NULL_HANDLE;
        // Otherwise the paused draw takes a stream slot, which is an exact count of it
        // on a driver the probe measured as counting capture-less draws.
        return rerouteArmed || m_primGenStreamCountsXfbInactiveDraws;
    }

    void VulkanRenderer::StopXfbQueryCapture(Uint32 kind, Vector<Uint32>& outSlots,
                                             Vector<Uint32>& outRerouteSlots) {
        if (kind > 1) {
            return;
        }
        outSlots = Move(m_xfbQueryActiveSlots[kind]);
        m_xfbQueryActiveSlots[kind].clear();
        m_xfbQueryCaptureActive[kind] = false;
        outRerouteSlots.clear();
        if (kind == 1) {
            outRerouteSlots = Move(m_primGenRerouteActiveSlots);
            m_primGenRerouteActiveSlots.clear();
        }
    }

    Bool VulkanRenderer::ResolveXfbQueryResult(const Vector<Uint32>& slots, const Vector<Uint32>& rerouteSlots,
                                               Bool wantGenerated, Uint64& outPrimitives) {
        outPrimitives = 0;
        const Bool haveStreamSlots = !slots.empty() && m_xfbQueryPool != VK_NULL_HANDLE;
        // Reroute slots only ever accumulate the GENERATED target (see
        // BeginXfbQueryForDraw); WRITTEN never opens one.
        const Bool haveRerouteSlots =
            wantGenerated && !rerouteSlots.empty() && m_primGenReroutePool != VK_NULL_HANDLE;
        if (!haveStreamSlots && !haveRerouteSlots) {
            return true;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (frame.isCommandRecording) {
            if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
                VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            }
            if (!SubmitReadbackCommandsAndWait(frame)) {
                return false;
            }
        }
        if (haveStreamSlots) {
            for (const Uint32 slot : slots) {
                Uint64 pair[2] = {0, 0}; // {primitivesWritten, primitivesNeeded}
                const VkResult result =
                    vkGetQueryPoolResults(m_device, m_xfbQueryPool, slot, 1, sizeof(pair), pair, sizeof(pair),
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                if (result == VK_SUCCESS) {
                    outPrimitives += pair[wantGenerated ? 1 : 0];
                }
            }
        }
        if (haveRerouteSlots) {
            for (const Uint32 slot : rerouteSlots) {
                // Both reroute pool kinds answer one 64-bit primitive count per slot.
                Uint64 generated = 0;
                const VkResult result = vkGetQueryPoolResults(
                    m_device, m_primGenReroutePool, slot, 1, sizeof(generated), &generated,
                    sizeof(generated), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                if (result == VK_SUCCESS) {
                    outPrimitives += generated;
                }
            }
        }
        return true;
    }

    void VulkanRenderer::BeginXfbQueryForDraw(VkCommandBuffer commandBuffer, Bool xfbActive) {
        m_xfbQuerySlotOpen = false;
        m_primGenRerouteSlotOpen = false;
        if ((!m_xfbQueryCaptureActive[0] && !m_xfbQueryCaptureActive[1]) || m_xfbQueryPool == VK_NULL_HANDLE) {
            return;
        }
        // Every draw with no OPEN capture is the stream query's silent case, and that
        // includes a draw made while the GL span is merely PAUSED (the pause closes the
        // capture, so BeginXfbCaptureForDraw already answered false for it). Paused
        // draws are rerouted like any other: the frontend's CPU paused-primitive
        // counter cannot stand in for them - it is written by only 3 of the ~15 draw
        // entry points (never the instanced, indirect or multi-draw ones) and answers 0
        // for GL_PATCHES by design, since the tessellator's amplification is not
        // knowable on the CPU - which is exactly the CTS's shape. Double counting is
        // prevented on the other side instead: a GENERATED span opened while this
        // reroute is armed ignores that CPU counter entirely (see
        // ArePausedDrawsGpuCounted and DirectVulkan.cpp's XfbGenerated resolve), so
        // every XFB-inactive draw in the span is priced exactly once, by this pool.
        const Bool rerouteGenerated = m_xfbQueryCaptureActive[1] &&
                                      m_primGenRerouteKind != MG_Util::SelfTest::PrimGenRerouteKind::None &&
                                      m_primGenReroutePool != VK_NULL_HANDLE && !xfbActive;
        // The stream slot stays for WRITTEN whatever the reroute does (with capture
        // inactive its primitivesWritten is 0, which is the correct WRITTEN answer),
        // and for GENERATED wherever this draw is not rerouted - so one GL query span
        // may accumulate stream slots (XFB-active draws) and reroute slots
        // (XFB-inactive draws) side by side.
        const Bool wantStreamSlot =
            m_xfbQueryCaptureActive[0] || (m_xfbQueryCaptureActive[1] && !rerouteGenerated);
        if (wantStreamSlot) {
            const Uint32 slot = m_xfbQuerySlotCursor;
            m_xfbQuerySlotCursor = (m_xfbQuerySlotCursor + 1) % kXfbQuerySlots;
            // Slots are never host-reset at read time (both GL targets may reference one
            // slot); recycle them here instead.
            s_vkResetQueryPool(m_device, m_xfbQueryPool, slot, 1);
            s_vkCmdBeginQueryIndexedEXT(commandBuffer, m_xfbQueryPool, slot, 0, 0);
            if (m_xfbQueryCaptureActive[0]) {
                m_xfbQueryActiveSlots[0].push_back(slot);
            }
            if (m_xfbQueryCaptureActive[1] && !rerouteGenerated) {
                m_xfbQueryActiveSlots[1].push_back(slot);
            }
            m_xfbQuerySlotOpen = true;
            m_xfbQueryOpenSlot = slot;
        }
        if (rerouteGenerated) {
            // Latched at INFO on purpose: it is the pinned integration lane's arming
            // observable (the shape UnlocatedIoBlockScenario asserts), and the builds
            // CI runs compile INFO in.
            MGLOG_I_ONCE("PRIMITIVES_GENERATED reroute engaged: an XFB-inactive draw accumulates "
                         "through the %s pool",
                         m_primGenRerouteKind ==
                                 MG_Util::SelfTest::PrimGenRerouteKind::PrimitivesGeneratedExt
                             ? "VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT"
                             : "clipping-invocations statistics");
            const Uint32 slot = m_primGenRerouteSlotCursor;
            m_primGenRerouteSlotCursor = (m_primGenRerouteSlotCursor + 1) % kXfbQuerySlots;
            // Same recycle-at-begin discipline as the stream pool. Both pool kinds
            // are begun with plain vkCmdBeginQuery (a PRIMITIVES_GENERATED_EXT
            // query begun this way counts vertex stream 0).
            s_vkResetQueryPool(m_device, m_primGenReroutePool, slot, 1);
            vkCmdBeginQuery(commandBuffer, m_primGenReroutePool, slot, 0);
            m_primGenRerouteActiveSlots.push_back(slot);
            m_primGenRerouteSlotOpen = true;
            m_primGenRerouteOpenSlot = slot;
        }
    }

    void VulkanRenderer::EndXfbQueryForDraw(VkCommandBuffer commandBuffer) {
        if (m_xfbQuerySlotOpen) {
            s_vkCmdEndQueryIndexedEXT(commandBuffer, m_xfbQueryPool, m_xfbQueryOpenSlot, 0);
            m_xfbQuerySlotOpen = false;
        }
        if (m_primGenRerouteSlotOpen) {
            vkCmdEndQuery(commandBuffer, m_primGenReroutePool, m_primGenRerouteOpenSlot);
            m_primGenRerouteSlotOpen = false;
        }
    }

    Bool VulkanRenderer::BeginOcclusionForDraw(VkCommandBuffer commandBuffer) {
        if (!m_occlusionCaptureActive || m_occlusionQueryPool == VK_NULL_HANDLE) {
            return false;
        }
        const Uint32 slot = m_occlusionSlotCursor;
        m_occlusionSlotCursor = (m_occlusionSlotCursor + 1) % kOcclusionQuerySlots;
        // Slots recycle after their read; a wrapped-past unread slot is stale, so
        // reset it here (host reset - the slot's prior GPU use has long retired).
        s_vkResetQueryPool(m_device, m_occlusionQueryPool, slot, 1);
        vkCmdBeginQuery(commandBuffer, m_occlusionQueryPool, slot,
                        m_occlusionQueryPreciseEnabled ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        m_occlusionActiveSlots.push_back(slot);
        return true;
    }

    void VulkanRenderer::EndOcclusionForDraw(VkCommandBuffer commandBuffer, Bool began) {
        if (!began) {
            return;
        }
        vkCmdEndQuery(commandBuffer, m_occlusionQueryPool, m_occlusionActiveSlots.back());
    }

    void VulkanRenderer::DrawElements(const DrawIndexedCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        DrawCmdParam vertexRange{};
        vertexRange.vertexCount = payload.params.indexCount + (payload.params.vertexOffset > 0
                                                                   ? static_cast<Uint32>(payload.params.vertexOffset)
                                                                   : 0);
        vertexRange.instanceCount = payload.params.instanceCount;
        vertexRange.firstVertex = 0;
        vertexRange.firstInstance = static_cast<Uint32>(payload.params.firstInstance);
        vertexRange.baseVertex = payload.params.vertexOffset;
        // Direct DrawElements fetches exactly the indices in its view, so vertex-stream
        // conversion may bound its work by scanning them.
        vertexRange.indexRangeIsExactView = true;

        if (!SetupDraw(frame, payload.mode, DrawSetupAspect::IndexBuffer, vertexRange,
                       &payload.indexBufferView)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        VkCommandBuffer& commandBuffer = frame.commandBuffer;

        const Bool xfbActive = BeginXfbCaptureForDraw(frame);
        BeginXfbQueryForDraw(commandBuffer, xfbActive);
        const Bool occlusionActive = BeginOcclusionForDraw(commandBuffer);
        vkCmdDrawIndexed(commandBuffer,
            payload.params.indexCount,
            payload.params.instanceCount,
            payload.params.firstIndex,
            payload.params.vertexOffset,
            payload.params.firstInstance);
        EndOcclusionForDraw(commandBuffer, occlusionActive);
        EndXfbCaptureForDraw(frame, xfbActive);
        EndXfbQueryForDraw(commandBuffer);
    }

    void VulkanRenderer::MultiDrawArrays(const MultiDrawCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        // One state/pipeline setup covering the union of all sub-draw vertex ranges, then a vkCmdDraw
        // per range -- mirrors MultiDrawElements.
        DrawCmdParam vertexRange{};
        for (Uint32 idraw = 0; idraw < payload.drawCount; ++idraw) {
            vertexRange.vertexCount = std::max(vertexRange.vertexCount,
                                               payload.pParams[idraw].firstVertex + payload.pParams[idraw].vertexCount);
            vertexRange.instanceCount = std::max(vertexRange.instanceCount, payload.pParams[idraw].instanceCount);
            vertexRange.firstInstance = std::max(vertexRange.firstInstance, payload.pParams[idraw].firstInstance);
        }

        if (!SetupDraw(frame, payload.mode, 0, vertexRange)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        EmitMultiDraw(frame.commandBuffer, payload.pParams, payload.drawCount);
    }

    // The tier-2 indirect batch uploads the param arrays as-is: the leading members of the
    // renderer's draw-parameter structs are exactly Vulkan's indirect command layouts, and
    // vkCmdDraw(Indexed)Indirect accepts any 4-aligned stride >= the command size, so the
    // trailing CPU-side metadata rides along unread instead of forcing a repack.
    static_assert(sizeof(DrawIndexedCmdParam) == sizeof(VkDrawIndexedIndirectCommand) &&
                      offsetof(DrawIndexedCmdParam, indexCount) == offsetof(VkDrawIndexedIndirectCommand, indexCount) &&
                      offsetof(DrawIndexedCmdParam, instanceCount) ==
                          offsetof(VkDrawIndexedIndirectCommand, instanceCount) &&
                      offsetof(DrawIndexedCmdParam, firstIndex) == offsetof(VkDrawIndexedIndirectCommand, firstIndex) &&
                      offsetof(DrawIndexedCmdParam, vertexOffset) ==
                          offsetof(VkDrawIndexedIndirectCommand, vertexOffset) &&
                      offsetof(DrawIndexedCmdParam, firstInstance) ==
                          offsetof(VkDrawIndexedIndirectCommand, firstInstance),
                  "DrawIndexedCmdParam must alias VkDrawIndexedIndirectCommand for the tier-2 multi-draw upload");
    static_assert(sizeof(DrawCmdParam) % 4 == 0 && sizeof(DrawCmdParam) >= sizeof(VkDrawIndirectCommand) &&
                      offsetof(DrawCmdParam, vertexCount) == offsetof(VkDrawIndirectCommand, vertexCount) &&
                      offsetof(DrawCmdParam, instanceCount) == offsetof(VkDrawIndirectCommand, instanceCount) &&
                      offsetof(DrawCmdParam, firstVertex) == offsetof(VkDrawIndirectCommand, firstVertex) &&
                      offsetof(DrawCmdParam, firstInstance) == offsetof(VkDrawIndirectCommand, firstInstance),
                  "DrawCmdParam must lead with VkDrawIndirectCommand for the tier-2 multi-draw upload");

    void VulkanRenderer::EmitMultiDraw(VkCommandBuffer commandBuffer, const DrawCmdParam* pParams, Uint32 drawCount) {
        if (drawCount == 0) {
            return;
        }
        if (drawCount == 1) {
            vkCmdDraw(commandBuffer, pParams[0].vertexCount, pParams[0].instanceCount, pParams[0].firstVertex,
                      pParams[0].firstInstance);
            return;
        }

        // Tier 1: VK_EXT_multi_draw. vkCmdDrawMultiEXT shares one instanceCount/firstInstance
        // across the whole batch, so the batch must be uniform in both (GL's glMultiDrawArrays
        // always is: 1/0).
        if (m_multiDrawAllowExt) {
            Bool uniformInstances = true;
            for (Uint32 idraw = 1; idraw < drawCount; ++idraw) {
                if (pParams[idraw].instanceCount != pParams[0].instanceCount ||
                    pParams[idraw].firstInstance != pParams[0].firstInstance) {
                    uniformInstances = false;
                    break;
                }
            }
            if (uniformInstances) {
                static thread_local Vector<VkMultiDrawInfoEXT> infos;
                infos.resize(drawCount);
                for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
                    infos[idraw].firstVertex = pParams[idraw].firstVertex;
                    infos[idraw].vertexCount = pParams[idraw].vertexCount;
                }
                for (Uint32 base = 0; base < drawCount; base += m_maxMultiDrawCount) {
                    const Uint32 chunk = std::min(drawCount - base, m_maxMultiDrawCount);
                    s_vkCmdDrawMultiEXT(commandBuffer, chunk, infos.data() + base, pParams[0].instanceCount,
                                        pParams[0].firstInstance, sizeof(VkMultiDrawInfoEXT));
                }
                return;
            }
        }

        // Tier 2: multiDrawIndirect - one vkCmdDrawIndirect over a transient command array.
        // A sub-draw with firstInstance != 0 is illegal in an indirect command without the
        // drawIndirectFirstInstance feature; such a batch falls to the unrolled tier.
        if (m_multiDrawAllowIndirect) {
            Bool firstInstanceLegal = m_drawIndirectFirstInstanceFeatureEnabled;
            if (!firstInstanceLegal) {
                firstInstanceLegal = true;
                for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
                    if (pParams[idraw].firstInstance != 0) {
                        firstInstanceLegal = false;
                        break;
                    }
                }
            }
            const Uint32 maxIndirectCount = m_physicalDevice.properties.limits.maxDrawIndirectCount;
            if (firstInstanceLegal && maxIndirectCount > 0) {
                BufferSlice commandSlice{};
                if (m_bufferManager.UploadTransient(BufferKind::Indirect, m_frameContext.GetCurrentFrameIndex(),
                                                    pParams,
                                                    static_cast<VkDeviceSize>(drawCount) * sizeof(DrawCmdParam),
                                                    sizeof(Uint32), commandSlice)) {
                    for (Uint32 base = 0; base < drawCount; base += maxIndirectCount) {
                        const Uint32 chunk = std::min(drawCount - base, maxIndirectCount);
                        vkCmdDrawIndirect(commandBuffer, commandSlice.buffer,
                                          commandSlice.offset +
                                              static_cast<VkDeviceSize>(base) * sizeof(DrawCmdParam),
                                          chunk, sizeof(DrawCmdParam));
                    }
                    return;
                }
                // Transient arena refused the upload: fall through to the unrolled tier.
            }
        }

        // Tier 3: unrolled loop, byte-identical fallback (and the only tier where a SPIR-V
        // DrawIndex consumer sees 0 for every sub-draw instead of the sub-draw index).
        for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
            vkCmdDraw(commandBuffer, pParams[idraw].vertexCount, pParams[idraw].instanceCount,
                      pParams[idraw].firstVertex, pParams[idraw].firstInstance);
        }
    }

    void VulkanRenderer::EmitMultiDrawIndexed(VkCommandBuffer commandBuffer, const DrawIndexedCmdParam* pParams,
                                              Uint32 drawCount) {
        if (drawCount == 0) {
            return;
        }
        if (drawCount == 1) {
            vkCmdDrawIndexed(commandBuffer, pParams[0].indexCount, pParams[0].instanceCount, pParams[0].firstIndex,
                             pParams[0].vertexOffset, pParams[0].firstInstance);
            return;
        }

        // Tier 1: VK_EXT_multi_draw. VkMultiDrawIndexedInfoEXT carries per-draw
        // firstIndex/indexCount/vertexOffset (pVertexOffset = nullptr keeps the per-draw
        // offsets), but instanceCount/firstInstance are batch-wide, so the batch must be
        // uniform in both (GL's glMultiDrawElements* always is: 1/0).
        if (m_multiDrawAllowExt) {
            Bool uniformInstances = true;
            for (Uint32 idraw = 1; idraw < drawCount; ++idraw) {
                if (pParams[idraw].instanceCount != pParams[0].instanceCount ||
                    pParams[idraw].firstInstance != pParams[0].firstInstance) {
                    uniformInstances = false;
                    break;
                }
            }
            if (uniformInstances) {
                static thread_local Vector<VkMultiDrawIndexedInfoEXT> infos;
                infos.resize(drawCount);
                for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
                    infos[idraw].firstIndex = pParams[idraw].firstIndex;
                    infos[idraw].indexCount = pParams[idraw].indexCount;
                    infos[idraw].vertexOffset = pParams[idraw].vertexOffset;
                }
                for (Uint32 base = 0; base < drawCount; base += m_maxMultiDrawCount) {
                    const Uint32 chunk = std::min(drawCount - base, m_maxMultiDrawCount);
                    s_vkCmdDrawMultiIndexedEXT(commandBuffer, chunk, infos.data() + base,
                                               pParams[0].instanceCount,
                                               static_cast<Uint32>(pParams[0].firstInstance),
                                               sizeof(VkMultiDrawIndexedInfoEXT), nullptr);
                }
                return;
            }
        }

        // Tier 2: multiDrawIndirect - one vkCmdDrawIndexedIndirect over a transient command
        // array (DrawIndexedCmdParam aliases VkDrawIndexedIndirectCommand, see static_assert).
        if (m_multiDrawAllowIndirect) {
            Bool firstInstanceLegal = m_drawIndirectFirstInstanceFeatureEnabled;
            if (!firstInstanceLegal) {
                firstInstanceLegal = true;
                for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
                    if (pParams[idraw].firstInstance != 0) {
                        firstInstanceLegal = false;
                        break;
                    }
                }
            }
            const Uint32 maxIndirectCount = m_physicalDevice.properties.limits.maxDrawIndirectCount;
            if (firstInstanceLegal && maxIndirectCount > 0) {
                BufferSlice commandSlice{};
                if (m_bufferManager.UploadTransient(BufferKind::Indirect, m_frameContext.GetCurrentFrameIndex(),
                                                    pParams,
                                                    static_cast<VkDeviceSize>(drawCount) *
                                                        sizeof(DrawIndexedCmdParam),
                                                    sizeof(Uint32), commandSlice)) {
                    for (Uint32 base = 0; base < drawCount; base += maxIndirectCount) {
                        const Uint32 chunk = std::min(drawCount - base, maxIndirectCount);
                        vkCmdDrawIndexedIndirect(commandBuffer, commandSlice.buffer,
                                                 commandSlice.offset +
                                                     static_cast<VkDeviceSize>(base) * sizeof(DrawIndexedCmdParam),
                                                 chunk, sizeof(DrawIndexedCmdParam));
                    }
                    return;
                }
            }
        }

        // Tier 3: unrolled loop, byte-identical fallback (and the only tier where a SPIR-V
        // DrawIndex consumer sees 0 for every sub-draw instead of the sub-draw index).
        for (Uint32 idraw = 0; idraw < drawCount; ++idraw) {
            vkCmdDrawIndexed(commandBuffer, pParams[idraw].indexCount, pParams[idraw].instanceCount,
                             pParams[idraw].firstIndex, pParams[idraw].vertexOffset, pParams[idraw].firstInstance);
        }
    }

    void VulkanRenderer::MultiDrawElements(const MultiDrawIndexedCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        DrawCmdParam vertexRange{};
        for (Uint32 idraw = 0; idraw < payload.drawCount; ++idraw) {
            vertexRange.vertexCount = std::max(vertexRange.vertexCount, payload.pParams[idraw].indexCount);
            vertexRange.instanceCount = std::max(vertexRange.instanceCount, payload.pParams[idraw].instanceCount);
            vertexRange.firstInstance = std::max(vertexRange.firstInstance,
                                                 static_cast<Uint32>(payload.pParams[idraw].firstInstance));
        }

        if (!SetupDraw(frame, payload.mode, DrawSetupAspect::IndexBuffer, vertexRange,
                  &payload.indexBufferView)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        // Collapse contiguous sub-draw runs BEFORE tier dispatch: merging shrinks the
        // param span every tier consumes (fewer VkMultiDrawIndexedInfoEXT entries, a
        // smaller transient command array, fewer unrolled vkCmdDrawIndexed). Per-sub-draw
        // command emission in the driver dominates a Sodium-shaped multi-draw
        // (steady-state profile: >60% of the case inside the Vulkan driver's
        // vkCmdDrawIndexed encoding for 132x32 sub-draws/frame), and a chunk
        // renderer's sub-draws are runs of adjacent index ranges over one buffer.
        // Two draws are one iff they concatenate to an identical index stream:
        //  - a LIST topology (points/lines/triangles). Strips/fans/loops would
        //    weld primitives across the seam.
        //  - the accumulated count ends on a primitive boundary, otherwise GL
        //    discards the dangling indices at the sub-draw's end but the merged
        //    stream would assemble them with the next sub-draw's indices.
        //  - primitive restart is off: with restart on, a sentinel mid-stream
        //    resets assembly, so a partial primitive before the seam would
        //    otherwise be discarded per sub-draw (same dangling-index argument).
        //  - identical baseVertex/instancing and firstIndex adjacency, so the
        //    merged range fetches exactly the two sub-draws' indices in order.
        Uint32 mergeGranularity = 0;
        switch (payload.mode) {
            case GL_POINTS:    mergeGranularity = 1; break;
            case GL_LINES:     mergeGranularity = 2; break;
            case GL_TRIANGLES: mergeGranularity = 3; break;
            default: break;
        }
        if (mergeGranularity != 0) {
            const RenderStateParameters& rsp = MG_Pipe::gPipeInputs.GetRenderStateParameters();
            if (rsp.PrimitiveRestartEnabled || rsp.PrimitiveRestartFixedIndexEnabled) {
                mergeGranularity = 0;
            }
        }
        const DrawIndexedCmdParam* pParams = payload.pParams;
        Uint32 drawCount = payload.drawCount;
        static thread_local Vector<DrawIndexedCmdParam> mergedParams;
        if (mergeGranularity != 0) {
            mergedParams.clear();
            mergedParams.reserve(drawCount);
            Uint32 idraw = 0;
            while (idraw < drawCount) {
                DrawIndexedCmdParam head = pParams[idraw];
                ++idraw;
                if (head.indexCount == 0) {
                    continue; // draws nothing, contributes nothing to a run
                }
                if (head.instanceCount == 1) {
                    while (idraw < drawCount) {
                        const DrawIndexedCmdParam& next = pParams[idraw];
                        if (next.indexCount == 0) {
                            ++idraw;
                            continue;
                        }
                        if (head.indexCount % mergeGranularity != 0 ||
                            next.instanceCount != 1 ||
                            next.vertexOffset != head.vertexOffset ||
                            next.firstInstance != head.firstInstance ||
                            next.firstIndex != head.firstIndex + head.indexCount ||
                            head.indexCount + next.indexCount < head.indexCount) {
                            break;
                        }
                        head.indexCount += next.indexCount;
                        ++idraw;
                    }
                }
                mergedParams.push_back(head);
            }
            pParams = mergedParams.data();
            drawCount = static_cast<Uint32>(mergedParams.size());
        }

        EmitMultiDrawIndexed(frame.commandBuffer, pParams, drawCount);
    }

    VkCommandBuffer VulkanRenderer::AcquireBufferCopyCommandBuffer() {
        if (m_device == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0) {
            return VK_NULL_HANDLE;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        // vkCmdCopyBuffer must be recorded outside a render pass; draws re-begin
        // their render pass lazily, matching the existing blit/clear pattern.
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }
        return frame.commandBuffer;
    }

    Bool VulkanRenderer::IsFrameSerialComplete(Uint64 serial) const {
        return serial <= m_bufferManager.GetCompletedSerial();
    }

    Bool VulkanRenderer::WaitForFrameSerial(Uint64 serial, Uint64 timeoutNs) {
        (void)timeoutNs;
        if (IsFrameSerialComplete(serial)) {
            return true;
        }
        // Work recorded under the current serial has not been submitted yet
        // (submission happens in Present, on this same thread), so blocking
        // can never make progress; the caller reports a timeout instead.
        if (serial >= m_bufferManager.GetFrameSerial()) {
            return false;
        }
        if (m_device == VK_NULL_HANDLE || m_graphicsQueue == VK_NULL_HANDLE) {
            return IsFrameSerialComplete(serial);
        }
        // More than one submit can carry the same frame serial (mid-frame flush,
        // then Present). The completed floor is clamped below any serial still
        // held by an in-flight record; the first matching fence is not proof.
        // OnSubmitsCompletedUpTo erases records, so copy fields and restart the
        // search after every wait instead of continuing an invalidated iterator.
        for (;;) {
            const auto record = std::find_if(m_inFlightSubmits.begin(), m_inFlightSubmits.end(),
                [serial](const auto& candidate) {
                    return candidate.frameSerial >= serial && candidate.fence != VK_NULL_HANDLE;
                });
            if (record == m_inFlightSubmits.end()) break;
            const Uint64 submitIndex = record->submitIndex;
            if (!WaitForSubmitsUpTo(submitIndex, UINT64_MAX))
                break; // fall through to the queue drain below
            TryDrainFrameTransients();
            if (IsFrameSerialComplete(serial)) return true;
        }

        // No usable record - fall back to draining the graphics queue. This over-waits (bounded by
        // the in-flight frame count) but never deadlocks.
        const VkResult result = vkQueueWaitIdle(m_graphicsQueue);
        if (result != VK_SUCCESS) {
            NoteDeviceLoss(result, "WaitForFrameSerial vkQueueWaitIdle");
            MGLOG_E_ONCE("WaitForFrameSerial: vkQueueWaitIdle returned %d", result);
            return false;
        }
        m_bufferManager.NotifyDeviceIdle();
        OnSubmitsCompletedUpTo(m_submitCounter);
        // The queue was just drained; take the free frame-boundary drain when
        // nothing is recorded (present-less timer-query loops). No-op otherwise.
        TryDrainFrameTransients();
        return IsFrameSerialComplete(serial);
    }

    Uint64 VulkanRenderer::GetSyncPointSubmitIndex() const {
        // Commands recorded (or still recording) since the last submission are
        // carried by the NEXT submission; a fence created now must wait for it.
        return m_submitCounter + (HasPendingRecordedWork() ? 1 : 0);
    }

    Bool VulkanRenderer::HasPendingRecordedWork() const {
        if (m_frameContext.GetFrameCount() == 0) {
            return false;
        }
        const auto& frame = m_frameContext.GetCurrent();
        return frame.isCommandRecording || frame.hasCommandBufferRecorded ||
               frame.isPreCommandRecording || frame.hasPreCommandBufferRecorded;
    }

    Bool VulkanRenderer::RewindWireDescriptorSetsIfDue() {
        if (!m_uniformManager || m_frameContext.GetFrameCount() == 0) return true;
        const Uint32 frameIndex = m_frameContext.GetCurrentFrameIndex();
        if (!m_uniformManager->WireDescriptorSetBudgetReached(frameIndex)) return true;

        // A set cannot be rewritten while a recorded or submitted command
        // buffer might still read it. This wire-only boundary first submits
        // the current recording, then idles the graphics queue before reuse.
        if (HasPendingRecordedWork() && !FlushPendingCommands()) {
            if (LatchWireDeviceLoss("descriptor-rewind-flush")) return false;
            MagmaWireFatal("descriptor-rewind-flush");
        }
        const Uint64 through = m_submitCounter;
        if (through > m_completedSubmitCounter) {
            // B4 has not yet made every old retirement site aggregate-safe.
            // Queue idle proves even a submission a previous single-fence path
            // prematurely removed from m_inFlightSubmits has finished; only
            // then may any layout cursor be rewound and its sets rewritten.
            if (const VkResult idle = vkQueueWaitIdle(m_graphicsQueue); idle != VK_SUCCESS) {
                NoteDeviceLoss(idle, "descriptor rewind vkQueueWaitIdle");
                if (LatchWireDeviceLoss("descriptor-rewind-wait")) return false;
                MagmaWireFatal("descriptor-rewind-wait");
            }
            OnSubmitsCompletedUpTo(through);
        }
        if (HasPendingRecordedWork() || m_completedSubmitCounter < through)
            MagmaWireFatal("descriptor-rewind-proof");
        const SizeT cachedSets = m_uniformManager->RewindWireDescriptorSets(frameIndex);
        MGLOG_I("Magma descriptor rewind: frame=%u retiredSubmit=%llu budget=%u cached=%zu",
                frameIndex, static_cast<unsigned long long>(through),
                UniformManager::kWireDescriptorSetBudget, cachedSets);
        return true;
    }

    Bool VulkanRenderer::IsSubmitIndexComplete(Uint64 submitIndex) {
        if (submitIndex <= m_completedSubmitCounter) {
            return true;
        }
        if (submitIndex > m_submitCounter) {
            return false; // not even submitted; no point polling fences
        }
        RefreshCompletedSubmits();
        return submitIndex <= m_completedSubmitCounter;
    }

    void VulkanRenderer::RegisterSubmit(VkFence fence, Bool pooledFence) {
        ++m_submitCounter;
        m_inFlightSubmits.push_back({m_submitCounter, m_bufferManager.GetFrameSerial(), fence, pooledFence});
    }

    void VulkanRenderer::RefreshCompletedSubmits() {
        if (m_device == VK_NULL_HANDLE) {
            return;
        }
        // Prefix-only scan: submissions to a single queue complete in order,
        // and stopping at the first unsignaled fence stays conservative even
        // if they did not.
        while (!m_inFlightSubmits.empty()) {
            // Copy before OnSubmitsCompletedUpTo erases the front record.
            const Uint64 frontIndex = m_inFlightSubmits.front().submitIndex;
            if (vkGetFenceStatus(m_device, m_inFlightSubmits.front().fence) != VK_SUCCESS) {
                break;
            }
            OnSubmitsCompletedUpTo(frontIndex);
        }
    }

    Bool VulkanRenderer::WaitForSubmitsUpTo(Uint64 submitIndex, Uint64 timeoutNs) {
        if (submitIndex <= m_completedSubmitCounter) return true;
        if (submitIndex > m_submitCounter || m_device == VK_NULL_HANDLE) return false;
        Vector<VkFence> fences;
        if (!CollectSubmitFencePrefix(m_inFlightSubmits, submitIndex, VkFence{}, fences))
            return false;
        const VkResult result = vkWaitForFences(m_device, static_cast<Uint32>(fences.size()),
                                                fences.data(), VK_TRUE, timeoutNs);
        if (result == VK_SUCCESS) {
            OnSubmitsCompletedUpTo(submitIndex);
            return true;
        }
        NoteDeviceLoss(result, "WaitForSubmitsUpTo vkWaitForFences");
        if (result != VK_TIMEOUT)
            MGLOG_E_ONCE("WaitForSubmitsUpTo: vkWaitForFences returned %d", result);
        return false;
    }

    void VulkanRenderer::OnSubmitsCompletedUpTo(Uint64 submitIndex) {
        m_completedSubmitCounter = std::max(m_completedSubmitCounter, submitIndex);
        // ---- P7 wave 2 package B3: THE COMPLETED-FRAME-SERIAL FLOOR MUST BE PROVABLE --------
        //
        // "Frame-serial completion piggybacks on submission completion" holds only while a
        // frame serial has ONE submission. On the wire arm it has at least two: the per-frame
        // palette glTexImage2D reaches UploadPendingWireLevels ->
        // FlushWirePendingCommandsForTextureUpdate -> FlushPendingCommands at the first
        // palette-sampling draw and submits S1 under a POOLED fence, and the frame's draws
        // (and the barriered vkCmdCopyBuffer that StagedWireRangeCopy records for every
        // streamed glBufferSubData) then go out in the Present submission S2.
        //
        // S1 is tiny and signals almost immediately. Retiring it used to advance the floor to
        // ITS frameSerial - which is also S2's - so VkBufferManager was told "frame N-1 is
        // complete" while S2(N-1), the submission actually carrying frame N-1's buffer copies,
        // was still executing. The next frame's first write to that buffer then failed
        // WriteWireBuffer's busy predicate and took the unordered host memcpy, and S2(N-1)'s
        // queued copies landed on top of it afterwards: a PREFIX TEAR of one draw's vertex
        // range, no log line, no GL error, no Fatal.
        //
        // The floor now advances only to a serial NO remaining in-flight submission still
        // carries. NotifyFrameSerialComplete is monotone and already refuses the still-
        // recording serial, so this can only ever advance the floor later, never further.
        // On the disaggregated build's MONOLITH arm this is behaviour-identical (one
        // submission per serial). The #else branch below is the pull build's statement,
        // unchanged, which is what keeps G1's .text byte-identical.
        Bool retiredAny = false;
        Uint64 advanceTo = 0;
        while (!m_inFlightSubmits.empty() && m_inFlightSubmits.front().submitIndex <= submitIndex) {
            SubmitRecord record = m_inFlightSubmits.front();
            m_inFlightSubmits.erase(m_inFlightSubmits.begin());
            retiredAny = true;
            advanceTo = std::max(advanceTo, record.frameSerial);
            if (!record.pooledFence || m_device == VK_NULL_HANDLE) {
                continue; // frame-slot fences are reset/destroyed by FrameContext
            }
            if (vkResetFences(m_device, 1, &record.fence) == VK_SUCCESS) {
                m_freeSubmitFences.push_back(record.fence);
            } else {
                vkDestroyFence(m_device, record.fence, nullptr);
            }
        }
        if (retiredAny) {
            // Clamp to one below the lowest serial still in flight.
            for (const auto& remaining : m_inFlightSubmits) {
                advanceTo = std::min(advanceTo, remaining.frameSerial > 0 ? remaining.frameSerial - 1 : Uint64{0});
            }
            // Lens C's probe, and the red-once's reading: an advance that names a serial some
            // remaining submission still carries. Unreachable with the clamp above; the R-16
            // revert is to delete the clamp loop, and then this counts.
            for (const auto& remaining : m_inFlightSubmits) {
                if (remaining.frameSerial <= advanceTo) {
                    WireDeclineTally::CountUnsoundSerialComplete();
                    // Logged per event, not once: this is the red-once's reading and the log is
                    // the only place it can be read from: the renderer this trace builds is
                    // never Shutdown() (the process exits), so a teardown dump reports the
                    // start-up context's zero instead of the replay's count.
                    MGLOG_W("MGWIRE-FLOOR unsound-serial-complete #%llu: advancing the completed "
                            "frame-serial floor to %llu, which submission %llu (serial %llu) still carries",
                            static_cast<unsigned long long>(WireDeclineTally::UnsoundSerialCompleteEvents()),
                            static_cast<unsigned long long>(advanceTo),
                            static_cast<unsigned long long>(remaining.submitIndex),
                            static_cast<unsigned long long>(remaining.frameSerial));
                    break;
                }
            }
            {
                static const Bool probe = [] {
                    const char* value = std::getenv("MOBILEGL_MAGMA_WIREBUF_PROBE");
                    return value && value[0] == '1';
                }();
                if (probe) {
                    Uint64 minRemaining = ~Uint64{0};
                    for (const auto& r : m_inFlightSubmits) minRemaining = std::min(minRemaining, r.frameSerial);
                    MGLOG_I("FLOOR retire upTo=%llu advanceTo=%llu remaining=%zu minRemainingSerial=%lld "
                            "frameSerial=%llu",
                            static_cast<unsigned long long>(submitIndex),
                            static_cast<unsigned long long>(advanceTo), m_inFlightSubmits.size(),
                            m_inFlightSubmits.empty() ? -1LL : static_cast<long long>(minRemaining),
                            static_cast<unsigned long long>(m_bufferManager.GetFrameSerial()));
                }
            }
            m_bufferManager.NotifyFrameSerialComplete(advanceTo);
        }
        // Mid-frame-flushed command buffers whose submission just completed can
        // be freed now; present-less flush loops have no other reclaim point.
        m_frameContext.FreeRetiredCommandBuffersCompletedUpTo(m_completedSubmitCounter);
        CollectWireObjects(m_completedSubmitCounter);
#if MOBILEGL_BUILD_DISAGGREGATED
        RecycleSharedImageSemaphores(m_completedSubmitCounter);
#endif
    }

    Bool VulkanRenderer::TryDrainFrameTransients() {
        if (m_wirePreparationDepth != 0) return false;
        if (m_device == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0) {
            return false;
        }
        if (m_completedSubmitCounter != m_submitCounter) {
            RefreshCompletedSubmits();
            if (m_completedSubmitCounter != m_submitCounter) {
                return false;
            }
        }
        if (HasPendingRecordedWork()) {
            return false;
        }

        // Texture uploads submit on this queue with their own fences. The
        // renderer watermark alone cannot prove images/views are idle.
        if (m_textureManager && !m_textureManager->WireUploadsAreIdle()) return false;
        // No preparation, recording or GPU work remains. A minimized Present
        // may have abandoned a recording tagged for a submission that will
        // never occur, so reclaim those future-tagged objects as well.
        CollectWireObjects(m_completedSubmitCounter, true);
        ClearAllWireDrawPassCaches();

        // Every submission is complete and nothing recorded references the
        // per-frame transients. Pure-reclaim work runs on every drain: it only
        // releases memory that is provably dead, never invalidates anything a
        // later draw would have to rebuild. Raise the buffer manager's
        // completed floor first so busy-tracking reflects the proven idleness.
        m_bufferManager.NotifyDeviceIdle();

        const Uint32 frameIndex = m_frameContext.GetCurrentFrameIndex();
        m_frameContext.FreeAllRetiredCommandBuffers();
        for (Uint32 slot = 0; slot < m_deferredDepthMipmapCleanup.size(); ++slot) {
            CollectDeferredDepthMipmapCleanup(slot);
        }
        if (m_textureManager) {
            m_textureManager->CollectAllDeferredReleases();
        }
        m_bufferManager.CollectAllDeferredReleases();
        // Descriptor cursors rewind on every drain (the pre-drain readback path
        // already did exactly this), keeping fence/readback loops' set usage bounded.
        if (m_uniformManager) {
            m_uniformManager->BeginFrame(frameIndex);
        }

        // Frame-boundary-equivalent work - transient arena rewind (which invalidates
        // the conversion cache) and the cache-aging clocks - is gated to every 8th
        // drain since the last Present: a presenting app's mid-frame readbacks/waits
        // must neither force re-conversion/re-upload churn for the rest of the frame
        // nor multiply the aging rate (which would shrink the 1024-boundary retire
        // window and thrash periodically-used pipelines/programs), while present-less
        // loops still rewind the arena and age their caches every 8 iterations -
        // bounded by 8 iterations' transient usage.
        ++m_drainsSinceLastPresent;
        // B3 probe: a drain that reaches here found the GPU caught up and nothing recording.
        // Every 8th one is treated as a FRAME BOUNDARY - mid-frame. This probe is what REFUTED
        // that as the OpenRA mechanism (one successful drain, zero boundary works across the
        // replay; magma-b3.md §2.4): the cause was the floor in OnSubmitsCompletedUpTo.
        {
            static const Bool probe = [] {
                const char* value = std::getenv("MOBILEGL_MAGMA_WIREBUF_PROBE");
                return value && value[0] == '1';
            }();
            if (probe) {
                MGLOG_I("WBUF drain#%llu%s submit=%llu completed=%llu",
                        static_cast<unsigned long long>(m_drainsSinceLastPresent),
                        (m_drainsSinceLastPresent % 8) == 0 ? " ***FRAME-BOUNDARY-WORK***" : "",
                        static_cast<unsigned long long>(m_submitCounter),
                        static_cast<unsigned long long>(m_completedSubmitCounter));
            }
        }
        if ((m_drainsSinceLastPresent % 8) != 0) {
            return true;
        }
        if (m_textureManager) {
            m_textureManager->BeginFrame(frameIndex);
        }
        m_bufferManager.BeginFrame(frameIndex);
        if (m_renderPassManager) {
            m_renderPassManager->OnPresent();
        }
        // The pipeline memo can survive across these boundaries (no per-frame reset
        // on this path), so it must drop whenever the sweep destroys anything.
        if (m_programFactory) {
            m_programFactory->OnFrameBoundary();
        }
        if (m_pipelineFactory && m_pipelineFactory->OnFrameBoundary() > 0) {
            InvalidatePipelineMemo();
        }
        if (m_samplerManager) {
            m_samplerManager->OnFrameBoundary();
        }
        return true;
    }

    VkFence VulkanRenderer::AcquirePooledSubmitFence() {
        if (!m_freeSubmitFences.empty()) {
            VkFence fence = m_freeSubmitFences.back();
            m_freeSubmitFences.pop_back();
            return fence;
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        const VkResult result = vkCreateFence(m_device, &fenceInfo, nullptr, &fence);
        if (result != VK_SUCCESS) {
            MGLOG_E_ONCE("AcquirePooledSubmitFence: vkCreateFence returned %d", result);
            return VK_NULL_HANDLE;
        }
        return fence;
    }

    void VulkanRenderer::DestroySubmitFencePool() {
        // Callers guarantee device idle, so in-flight fences are inert.
        for (const auto& record : m_inFlightSubmits) {
            if (record.pooledFence && m_device != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, record.fence, nullptr);
            }
        }
        m_inFlightSubmits.clear();
        for (auto fence : m_freeSubmitFences) {
            if (m_device != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, fence, nullptr);
            }
        }
        m_freeSubmitFences.clear();
        m_completedSubmitCounter = m_submitCounter;
    }

    Bool VulkanRenderer::SubmitPendingCommandBuffer(FrameContext::FrameData& frame, VkFence fence, Bool pooledFence) {
        RetireWireDrawPass();
        // Batched texture uploads must reach the queue before the frame's
        // commands: the recording being submitted may sample images whose
        // texels only exist in the texture manager's open upload batch.
        if (m_textureManager) {
            m_textureManager->FlushPendingUploads();
        }
        VkPipelineStageFlags waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSemaphore waitSemaphore = frame.imageAvailableSemaphore;
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (!frame.imageAvailableSemaphoreConsumed) {
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &waitSemaphore;
            submitInfo.pWaitDstStageMask = &waitDstStageMask;
        }
        // The pre-pass stream, when recorded, executes strictly before the
        // frame's commands within the same submission.
        VkCommandBuffer commandBuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        Uint32 commandBufferCount = 0;
        if (frame.hasPreCommandBufferRecorded) {
            commandBuffers[commandBufferCount++] = frame.preCommandBuffer;
        }
        if (frame.hasCommandBufferRecorded) {
            commandBuffers[commandBufferCount++] = frame.commandBuffer;
        }
        submitInfo.commandBufferCount = commandBufferCount;
        submitInfo.pCommandBuffers = commandBuffers;
#if MOBILEGL_BUILD_DISAGGREGATED
        // Shared-image fences staged for the work recorded since the last submission.
        if (LatchIfGpuHung("flush-submit")) return false;
        SharedImageSubmitSync sharedImageSync;
        AttachSharedImageSync(submitInfo, sharedImageSync);
        const GpuProgressMarkers::Bracket bracket = m_progressMarkers.Acquire();
        const VkResult result = GpuProgressMarkers::SubmitBracketed(m_graphicsQueue, submitInfo, fence, bracket);
        if (result == VK_SUCCESS) m_progressMarkers.Submitted(bracket);
        else m_progressMarkers.Abandon(bracket);
#else
        const VkResult result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, fence);
#endif
        if (result != VK_SUCCESS) {
            NoteDeviceLoss(result, "SubmitPendingCommandBuffer vkQueueSubmit");
            MGLOG_E_ONCE("SubmitPendingCommandBuffer: vkQueueSubmit returned %d", result);
            return false;
        }
        frame.imageAvailableSemaphoreConsumed = true;
        frame.hasCommandBufferRecorded = false;
        frame.hasPreCommandBufferRecorded = false;
        RegisterSubmit(fence, pooledFence);
        frame.lastSubmitIndex = m_submitCounter;
#if MOBILEGL_BUILD_DISAGGREGATED
        CommitSharedImageSync(sharedImageSync);
#endif
        return true;
    }

    Bool VulkanRenderer::FlushPendingCommands() {
        if (m_device == VK_NULL_HANDLE || m_graphicsQueue == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0) {
            return false;
        }
        // Non-blocking completion poll: gives flush-only workloads (no sync
        // objects, no present) a point where finished submissions retire their
        // pooled fences and mid-frame command buffers.
        RefreshCompletedSubmits();
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording && !frame.hasCommandBufferRecorded &&
            !frame.isPreCommandRecording && !frame.hasPreCommandBufferRecorded) {
            // GL flush semantics still demand batched texture uploads start
            // executing in finite time even when no draw was recorded.
            if (m_textureManager) {
                m_textureManager->FlushPendingUploads();
            }
            return false;
        }
        // Acquire the fence while recording is still open: failing here must
        // not end recording, or the next draw's BeginCommandRecording would
        // reset the command buffer and silently drop the frame's commands.
        VkFence fence = AcquirePooledSubmitFence();
        if (fence == VK_NULL_HANDLE) {
            return false;
        }
        if (frame.isCommandRecording) {
            if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
                VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            }
            m_frameContext.EndCommandRecording();
        }
        m_frameContext.EndPreCommandRecordingIfOpen();
        const Bool submittingPreCommandBuffer = frame.hasPreCommandBufferRecorded;
        if (!SubmitPendingCommandBuffer(frame, fence, /*pooledFence=*/true)) {
            // Submit failure (device loss regime): the ended command buffer
            // stays marked recorded so Present can still try to submit it.
            m_freeSubmitFences.push_back(fence); // still unsignaled, reusable
            return false;
        }

        // Command-buffer boundary: the pipeline memo must not survive it, or a
        // pipeline bound only through memo hits is never re-stamped in the factory
        // cache and the aging sweep could destroy it while the flushed submission
        // still references it. Mirrors the drops at the readback and Present
        // boundaries; costs one full pipeline lookup on the next draw.
        InvalidatePipelineMemo();

        // The submitted command buffer may still be executing; recording must
        // restart on a fresh one. If none can be allocated, fall back to
        // draining this submission so reusing the buffer stays legal.
        const VkResult retireResult = m_frameContext.RetireCurrentCommandBuffer(submittingPreCommandBuffer);
        if (retireResult != VK_SUCCESS) {
            MGLOG_E_ONCE("FlushPendingCommands: RetireCurrentCommandBuffer returned %d; draining submission", retireResult);
            if (WaitForSubmitsUpTo(m_submitCounter, UINT64_MAX)) {
                // Every registered fence through this submission was waited.
            } else if (vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS) {
                m_bufferManager.NotifyDeviceIdle();
                OnSubmitsCompletedUpTo(m_submitCounter);
            } else {
                // Device is effectively lost; the command buffer may still be
                // pending, but no recovery can make reuse legal.
                MGLOG_E_ONCE("FlushPendingCommands: drain failed; command buffer reuse is unsafe");
            }
        }
        return true;
    }

    void VulkanRenderer::NoteDeviceLoss(VkResult result, const char* where) {
        if (result != VK_ERROR_DEVICE_LOST || m_deviceLost) return;
        m_deviceLost = true;
        MGLOG_E("DirectVulkan: the VkDevice is LOST - %s returned VK_ERROR_DEVICE_LOST (the driver reset a GPU "
                "fault or hang); nothing this renderer recorded or submits from here on will execute",
                where != nullptr ? where : "a Vulkan call");
#if MOBILEGL_BUILD_DISAGGREGATED
        // Latch NOW where a session latch exists, so the session stops at its next record however
        // the verb in hand unwinds - including through a caller that only logs a failed submit.
        if (MG_Pipe::MGPipeSessionLatchArmed()) (void)LatchWireDeviceLoss(where);
#endif
    }

    Bool VulkanRenderer::IsDeviceLost() {
        // Only failure paths ask, so the probe's wait never lands on a healthy frame: a device
        // that is merely slow answers VK_SUCCESS once its work drains, a lost one answers at once.
        if (!m_deviceLost && m_device != VK_NULL_HANDLE) {
            NoteDeviceLoss(vkDeviceWaitIdle(m_device), "the device-loss probe's vkDeviceWaitIdle");
        }
        return m_deviceLost;
    }

    Bool VulkanRenderer::LatchWireDeviceLoss(const char* site) {
        if (!IsDeviceLost()) return false;
        if (!m_deviceLossLatched) {
            m_deviceLossLatched = true;
            // Returns (latched) on a served session; dies, as the site's own Fatal would have,
            // where no session latch is armed.
            (void)MG_Pipe::MGPipeSessionLatch(MG_Pipe::MGPipeFatalFamily::DeviceLost,
                                              "MGPipe: Fatal{BackendDeviceLost, \"Magma:%s\"} - this session's "
                                              "VkDevice was lost to a GPU fault or hang; the session ends and its "
                                              "client reads a lost context, every other session keeps running",
                                              site != nullptr ? site : "?");
        }
        return true;
    }

    Bool VulkanRenderer::LatchIfGpuHung(const char* site) {
        if (!m_progressMarkers.Hung()) return false;
        if (!m_deviceLost) {
            // NOT LOST TO THE DRIVER: a submission that a failed preemption leaves running is reset
            // without a device loss, and the next one would stop the GPU again. Treated as lost from
            // here on - nothing more is submitted - and the session ends as one whose device was.
            m_deviceLost = true;
            MGLOG_E("DirectVulkan: the GPU hang watch named this session's VkDevice (a submission ran %llu ms "
                    "without finishing, at %s); the device is treated as LOST and nothing more is submitted",
                    static_cast<unsigned long long>(m_progressMarkers.HungRunningMs()),
                    site != nullptr ? site : "?");
        }
        // Where no session latch is armed (no served session) the device only stays "lost".
        if (!m_deviceLossLatched && MG_Pipe::MGPipeSessionLatchArmed()) {
            m_deviceLossLatched = true;
            (void)MG_Pipe::MGPipeSessionLatch(MG_Pipe::MGPipeFatalFamily::DeviceLost,
                                              "MGPipe: Fatal{BackendDeviceLost, \"Magma:gpu-hang:%s\"} - this "
                                              "session's GPU work ran past the hang budget and stopped the GPU for "
                                              "every process; the session ends and its client reads a lost context, "
                                              "every other session keeps running",
                                              site != nullptr ? site : "?");
        }
        return true;
    }

    Bool VulkanRenderer::FlushForSyncPoint(Uint64 submitIndex) {
        // A flush only helps a sync point whose commands are not submitted
        // yet; for an already-submitted index it would just split the frame's
        // render pass (a full tile load/store on TBDR GPUs) without advancing
        // the fence.
        if (submitIndex <= m_submitCounter) {
            return false;
        }
        return FlushPendingCommands();
    }

    void VulkanRenderer::SplitOversizedRecording() {
        const Uint32 maxDraws = MG_Config::Features.MagmaMaxDrawsPerCommandBuffer;
        if (maxDraws == 0) {
            return;
        }
        // Counted before the check, so a buffer carries at most maxDraws calls before the one
        // that splits it. OnFrameCommandRecordingBegan resets the count on every new recording.
        if (++m_drawsInRecording <= maxDraws || !HasPendingRecordedWork()) {
            return;
        }
        // Nothing of the current call is recorded yet, so the split falls between two GL calls,
        // exactly where a glFlush would put it: the render pass ends with STORE, the next draw
        // re-begins it with LOAD, and queries and transform feedback are bracketed per draw.
        if (!FlushPendingCommands()) {
            return;
        }
        // Fresh command buffer: the same reset every other mid-frame flush site applies.
        ++m_oversizedRecordingSplits;
        MGLOG_I_ONCE("DirectVulkan: a frame recorded more than %u draws into one command buffer; submitting it and "
                     "continuing on a fresh one (MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER, 0 = unbounded)",
                     maxDraws);
        MGLOG_D("DirectVulkan: split an oversized recording (%llu so far, submit index %llu)",
                static_cast<unsigned long long>(m_oversizedRecordingSplits),
                static_cast<unsigned long long>(m_submitCounter));

        // Splitting only bounds the driver's command memory if the submitted buffers are freed
        // as fast as they are produced. A GPU that falls behind would park them all on the
        // retired list, which is the same growth spread over more buffers - so hold the queue
        // to frames-in-flight + 1 outstanding submissions, the steady state of a pipelined
        // frame plus this split. Wait on the fence directly rather than through
        // WaitForSubmitIndex: its opportunistic TryDrainFrameTransients may rewind the transient
        // arena, and this runs at the start of a draw whose caller may already hold slices of it.
        const SizeT maxOutstanding = static_cast<SizeT>(m_frameContext.GetFrameCount()) + 1;
        while (m_inFlightSubmits.size() > maxOutstanding) {
            const SubmitRecord oldest = m_inFlightSubmits.front();
            if (oldest.fence == VK_NULL_HANDLE) {
                break;
            }
            const VkResult waitResult = vkWaitForFences(m_device, 1, &oldest.fence, VK_TRUE, UINT64_MAX);
            if (waitResult != VK_SUCCESS) {
                NoteDeviceLoss(waitResult, "SplitOversizedRecording vkWaitForFences");
                MGLOG_E_ONCE("SplitOversizedRecording: vkWaitForFences returned %d", waitResult);
                break;
            }
            OnSubmitsCompletedUpTo(oldest.submitIndex);
        }
    }

    Bool VulkanRenderer::WaitForSubmitIndex(Uint64 submitIndex, Uint64 timeoutNs, Bool flushIfPending) {
        if (IsSubmitIndexComplete(submitIndex)) {
            return true;
        }
        if (submitIndex > m_submitCounter) {
            if (!flushIfPending) {
                return false;
            }
            FlushPendingCommands();
            if (submitIndex > m_submitCounter) {
                // Nothing could be submitted (empty batch or submit failure);
                // the index cannot complete yet.
                return false;
            }
        }
        if (!WaitForSubmitsUpTo(submitIndex, timeoutNs)) return false;
        // A blocking wait can drain a present-less loop's frame transients.
        TryDrainFrameTransients();
        return true;
    }

    void VulkanRenderer::OnFrameCommandRecordingBegan(VkCommandBuffer commandBuffer) {
        m_drawsInRecording = 0;
        // Dynamic state does not survive a command-buffer boundary.
        ResetDynamicStateShadow();
        if (m_uniformManager) {
            m_uniformManager->OnCommandBufferBoundary();
        }
        // Pre-pass stream bookkeeping: a fresh frame recording references no
        // textures yet.
        if (m_textureManager) {
            m_textureManager->AdvanceRecordingGeneration();
        }
        if (m_timerQueryManager) {
            m_timerQueryManager->OnFrameCommandRecordingBegan(commandBuffer, m_frameContext.GetCurrentFrameIndex(),
                                                              m_bufferManager.GetFrameSerial());
        }
    }

    VkProvokingVertexModeEXT VulkanRenderer::SelectProvokingVertexMode(VkPrimitiveTopology topology,
                                                                       Bool capturesXfbFromGeometryStage) const {
        if (!m_provokingVertexLastEnabled) {
            return VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
        }
        // Measured, and identical on lavapipe and on the NVIDIA Vulkan driver: a geometry shader's
        // emitted triangle strip is already recorded in GL's provoking-last vertex order, so asking
        // for LAST rotates it a second time. The input-assembler path has the opposite problem, and
        // the mode is a single pipeline bit, so the two cannot be satisfied at once: a program that
        // both runs a geometry shader and captures transform feedback keeps Vulkan's own convention,
        // and pays for it with a GL-wrong flat vertex in that one case. Deliberately a link-time
        // program property, not IsTransformFeedbackActive() - see the memo note in the header.
        if (capturesXfbFromGeometryStage) {
            return VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
        }
        // VUID-VkGraphicsPipelineCreateInfo-topology-04884 only bites when
        // transformFeedbackPreservesProvokingVertex is enabled; when it is not, a fan may take LAST.
        if (m_provokingVertexXfbPreserveEnabled && topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN &&
            !m_provokingVertexFanPreserved) {
            return VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
        }
        // Only provokingVertexModePerPipeline lets modes differ inside one render pass instance;
        // elsewhere every pipeline takes GL's default so the render pass stays self-consistent, and
        // glProvokingVertex(GL_FIRST_VERTEX_CONVENTION) goes unhonoured. Honouring it there would
        // mean ending the render pass on every glProvokingVertex change; not worth it until a target
        // device actually lacks the property.
        if (!m_provokingVertexModePerPipeline) {
            return VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
        }
        return (MG_Pipe::gPipeInputs.IsLive() &&
                MG_Pipe::gPipeInputs.GetProvokingVertexMode() == ProvokingVertexMode::FirstVertex)
                   ? VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT
                   : VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    }

    Bool VulkanRenderer::IsTimerQuerySupported() const {
        return m_timerQuerySupported && m_timerQueryManager != nullptr;
    }

    SharedPtr<VkTimerQueryManager::TimestampRecord> VulkanRenderer::WriteTimerQueryTimestamp() {
        if (!IsTimerQuerySupported() || m_device == VK_NULL_HANDLE || m_frameContext.GetFrameCount() == 0) {
            return nullptr;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        // vkCmdWriteTimestamp is valid both inside and outside a render pass,
        // so any active render pass is left untouched.
        return m_timerQueryManager->WriteTimestamp(frame.commandBuffer, m_frameContext.GetCurrentFrameIndex(),
                                                   m_bufferManager.GetFrameSerial());
    }

    Bool VulkanRenderer::IsTimerQueryResultReady(VkTimerQueryManager::TimestampRecord& record) {
        if (record.harvested) {
            return true;
        }
        if (!m_timerQueryManager) {
            return false;
        }
        // Ask the pool first. It polls with VK_QUERY_RESULT_WITH_AVAILABILITY_BIT and is the
        // authority on whether the timestamp has landed; the frame serial is not, because it only
        // advances at Present and neither completion notifier will mark the CURRENT serial done - so
        // a timestamp written and fence-waited inside one GL frame could never be read back in it.
        if (m_timerQueryManager->TryHarvest(record)) {
            return true;
        }
        return IsFrameSerialComplete(record.frameSerial) && m_timerQueryManager->TryHarvest(record);
    }

    Bool VulkanRenderer::WaitForTimerQueryResult(VkTimerQueryManager::TimestampRecord& record) {
        if (IsTimerQueryResultReady(record)) {
            return true;
        }
        // WaitForFrameSerial refuses serials that cannot complete without
        // further submissions (a timestamp written this frame only executes
        // once Present submits the command buffer), so this returns false
        // instead of deadlocking; the record resolves after a later Present.
        if (!WaitForFrameSerial(record.frameSerial, UINT64_MAX)) {
            return false;
        }
        return IsTimerQueryResultReady(record);
    }

    Uint64 VulkanRenderer::GetTimerQueryElapsedNs(const VkTimerQueryManager::TimestampRecord& begin,
                                                  const VkTimerQueryManager::TimestampRecord& end) const {
        return m_timerQueryManager ? m_timerQueryManager->ElapsedNs(begin, end) : 0;
    }

    Uint64 VulkanRenderer::GetTimerQueryTimestampNs(const VkTimerQueryManager::TimestampRecord& record) const {
        return m_timerQueryManager ? m_timerQueryManager->TimestampNs(record) : 0;
    }

    void VulkanRenderer::Present() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // A lost device presents nothing (its session is latched and closing).
        if (m_deviceLost && LatchWireDeviceLoss("present")) return;
        // Present has its own submit path. Tag its last wire pass before that
        // submission, rather than retaining it until a draw in the next frame.
        RetireWireDrawPass();
#endif
        if (m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended) {
            // No usable swapchain: the window was zero-area at initialization, or
            // presentation was suspended when the window minimized. Try to bring a
            // swapchain up now that the window may have a real size; until then, drop
            // this frame's recording instead of submitting - a submit would wait on a
            // never-signaled acquire semaphore and reuse a still-signaled fence.
            if (!RecreateSwapchain()) {
                auto& suspendedFrame = m_frameContext.GetCurrent();
                if (VkRenderPassManager::GetActiveRenderPass()) {
                    VkRenderPassManager::EndRenderPass(suspendedFrame.commandBuffer);
                }
                if (suspendedFrame.isCommandRecording) {
                    m_frameContext.EndCommandRecording();
                }
                m_frameContext.AbandonPreCommandRecording();
                suspendedFrame.isCommandRecording = false;
                suspendedFrame.hasCommandBufferRecorded = false;
                InvalidatePipelineMemo();
#if MOBILEGL_BUILD_DISAGGREGATED
                AbandonSharedImageReads();
#endif
                // The dropped recording is never submitted, so once the fence
                // poll shows the pre-suspension submissions complete the frame
                // transients (descriptor sets, transient arenas, deferred
                // releases, conversion caches) can rewind; without this a
                // minimized-window app accumulates them for the whole
                // suspension.
                TryDrainFrameTransients();
                // The application counted this swap; no image did.
                m_swapchainObject.OnFrameDropped();
                MGLOG_D("Present skipped: no usable swapchain (zero-area window)");
                return;
            }
            m_presentSuspended = false;
            // A slot's Present fence alone does not prove its older pooled
            // flushes retired. Wait the full prefix before FrameContext may
            // free its retired command buffers and reset the slot fence.
            const Uint64 slotSubmit = m_frameContext.GetCurrent().lastSubmitIndex;
            if (slotSubmit > m_completedSubmitCounter &&
                !WaitForSubmitsUpTo(slotSubmit, UINT64_MAX)) {
                if (LatchWireDeviceLoss("deferred-acquire-submit-wait")) return;
                MagmaWireFatal("deferred-acquire-submit-wait");
            }
            const VkResult acquireResult =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
            if (acquireResult == VK_SUBOPTIMAL_KHR) {
                // Usable image with its acquire signal already armed; a rebuild is scheduled
                // only if the surface genuinely no longer matches (see step 4 of Present).
                m_swapchainResizeRequested = m_swapchainResizeRequested || SwapchainIsOutOfDate();
            } else {
                VK_VERIFY(acquireResult, "Present, deferred first WaitAndAcquireNextImage");
            }
        }
        MOBILEGL_ASSERT(m_imageIndexAcquired < m_swapchainObject.GetImageCount(),
                        "Present, acquired image index out of range");
        m_renderPassManager->OnPresent();
        // A real presented frame is the canonical aging cadence; mid-frame drains
        // count against this and only age when presents stop coming.
        m_drainsSinceLastPresent = 0;
        // Age the content-addressed caches on the same frame-boundary cadence. Each
        // keeps its own internal 256-sweep gate, so the per-frame cost is one counter
        // increment and compare per cache; entries used by this frame's still-
        // unsubmitted recording were stamped this boundary (every command-buffer
        // boundary drops the pipeline memo, so the first draw of each recording
        // performs a real, stamping lookup) and can never age out.
        m_programFactory->OnFrameBoundary();
        if (m_pipelineFactory->OnFrameBoundary() > 0) {
            InvalidatePipelineMemo(); // an aged-out pipeline may still be memoized
            // A recreated pipeline could reuse a freed handle value and alias
            // the bind-dedup shadow; force the next draw to re-bind.
            g_dynamicStateShadow->graphicsPipelineValid = false;
        }
        m_samplerManager->OnFrameBoundary();
        auto& frame = m_frameContext.GetCurrent();
        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        if (activeRenderPass)
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
#if MOBILEGL_BUILD_DISAGGREGATED
        // The frame boundary of the shared images this frame read (WireSharedImage.inc): handed
        // back to the foreign family in this recording, and this submission signals the read
        // fence their next writer waits for.
        ReleaseHeldSharedImages();
        if (m_sharedImageSyncFd && m_sharedImageReads.Pending() > 0 && m_sharedImageSignal == VK_NULL_HANDLE)
            m_sharedImageSignal = TakeSharedImageSemaphore(/*exportable=*/true);
#endif

        // Transition while this frame's recording is still open. A frame that
        // rendered only into FBOs has no default-framebuffer render pass, and that
        // pass's finalLayout is the only other thing that carries the swapchain
        // image to PRESENT_SRC_KHR - so closing the buffer first, which made
        // TransitionToPresent refuse to record, handed the image to
        // vkQueuePresentKHR in the layout it was acquired in (UNDEFINED on a fresh
        // swapchain). The SetImageLayout below then made the tracker's
        // disagreement with reality permanent for that image index.
        const auto acquiredImageLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
        m_frameContext.TransitionToPresent(m_swapchainObject.GetImage(m_imageIndexAcquired), acquiredImageLayout);

        if (frame.isCommandRecording) {
            m_frameContext.EndCommandRecording();
            frame.hasCommandBufferRecorded = true;
            InvalidatePipelineMemo(); // command-buffer boundary: drop the pipeline memo
        }
        m_frameContext.EndPreCommandRecordingIfOpen();

        const Bool shouldSubmitCommandBuffer = frame.hasCommandBufferRecorded;

        // 1) Submit current frame work (the pre-pass stream, when recorded,
        //    rides the same submission strictly ahead of the frame commands).
        //    Batched texture uploads go first: the frame's commands may sample
        //    images whose texels only exist in the open upload batch, and
        //    flushing here also bounds upload latency to one frame.
        if (m_textureManager) {
            m_textureManager->FlushPendingUploads();
        }
        auto submitPacket = m_frameContext.GetSubmitInfo(shouldSubmitCommandBuffer, m_imageIndexAcquired);
#if MOBILEGL_BUILD_DISAGGREGATED
        if (LatchIfGpuHung("present-submit")) return;
        SharedImageSubmitSync sharedImageSync;
        AttachSharedImageSync(submitPacket.submitInfo, sharedImageSync);
        const GpuProgressMarkers::Bracket bracket = m_progressMarkers.Acquire();
        const VkResult presentSubmit = GpuProgressMarkers::SubmitBracketed(m_graphicsQueue, submitPacket.submitInfo,
                                                                           frame.imageInFlightFence, bracket);
        if (presentSubmit == VK_SUCCESS) m_progressMarkers.Submitted(bracket);
        else m_progressMarkers.Abandon(bracket);
#else
        const VkResult presentSubmit = vkQueueSubmit(m_graphicsQueue, 1, &submitPacket.submitInfo, frame.imageInFlightFence);
#endif
        NoteDeviceLoss(presentSubmit, "Present vkQueueSubmit");
        if (presentSubmit != VK_SUCCESS && LatchWireDeviceLoss("present-submit")) return;
        VK_VERIFY(presentSubmit);
        RegisterSubmit(frame.imageInFlightFence, /*pooledFence=*/false);
        frame.lastSubmitIndex = m_submitCounter;
#if MOBILEGL_BUILD_DISAGGREGATED
        CommitSharedImageSync(sharedImageSync);
        PublishSharedImageReads();
#endif
        frame.isCommandRecording = false;
        frame.hasCommandBufferRecorded = false;
        frame.hasPreCommandBufferRecorded = false;
        m_swapchainObject.SetImageLayout(m_imageIndexAcquired, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // 2) Present current frame.
        auto presentPacket = m_frameContext.GetPresentInfo(m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        auto result = vkQueuePresentKHR(m_presentQueue, &presentPacket.presentInfo);
        if (result == VK_SUBOPTIMAL_KHR) {
            // Suboptimal is not a reason to rebuild on its own: a driver may report it for a
            // surface whose size and orientation still match what we built from (Android does
            // this routinely), and rebuilding on it alone destroys every pipeline and
            // reallocates the default framebuffer once per frame - flicker, then garbage.
            // Defer to the surface-capabilities comparison below.
            result = VK_SUCCESS;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            MGLOG_D("Present, vkQueuePresentKHR got %d, recreating swapchain", result);
            if (!RecreateSwapchain()) {
                // Window went zero-area (minimize) with the swapchain out of date:
                // stop submitting/acquiring until it has a size again.
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window with out-of-date swapchain; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
            result = VK_SUCCESS;
        }
        NoteDeviceLoss(result, "vkQueuePresentKHR");
        if (result != VK_SUCCESS && LatchWireDeviceLoss("present-queue")) return;
        VK_VERIFY(result, "Present, vkQueuePresentKHR");
        // EGL swap semantics: the presented color buffer's content is undefined the
        // next time this image is acquired (EGL_BUFFER_DESTROYED, the default swap
        // behaviour), and EVERY ancillary depth/stencil buffer's content is
        // undefined after any swap. The render-pass manager turns the undefined
        // attachments' next tile loads into LOAD_OP_DONT_CARE.
        //
        // EGL_EXT_buffer_age changes the first half: once the client has asked for an age it
        // repaints only what changed since the image it is given was last shown, so a presented
        // image keeps its content (the wire passes load it; Vulkan preserves an image's content
        // through a present and a layout transition out of PRESENT_SRC) and its age is counted.
        m_swapchainObject.OnImagePresented(m_imageIndexAcquired);
        if (!m_swapchainObject.BufferAgeAsked()) m_swapchainObject.SetImageContentDefined(m_imageIndexAcquired, false);
        m_swapchainObject.SetAllDepthStencilContentUndefined();
        // The authoritative check, done here - after the frame is presented, before the next
        // acquire. This is what makes a launcher-side resolution change take effect: shrinking
        // the window's buffer (SurfaceHolder.setFixedSize) moves currentExtent, the swapchain
        // follows, and the compositor scales the smaller image up to the view for free.
        if (!m_swapchainResizeRequested && SwapchainIsOutOfDate()) {
            m_swapchainResizeRequested = true;
        }
        // A new eglSwapInterval lands here too, between frames: this one is on screen and the
        // next has recorded nothing. Only a different present mode is worth the rebuild (it
        // drops every pipeline); otherwise the live swapchain already paces as asked.
        if (m_config.SwapInterval != m_swapchainSwapInterval) {
            if (SwapchainObject::ChooseSwapchainPresentMode(m_swapchainObject.GetSupportedPresentModes(),
                                                            m_config.SwapInterval) !=
                m_swapchainObject.GetPresentMode()) {
                m_swapchainResizeRequested = true;
            } else {
                MGLOG_D("DirectVulkan: swap interval %d -> %s", *m_config.SwapInterval,
                        SwapchainObject::GetPresentModeName(m_swapchainObject.GetPresentMode()));
                m_swapchainSwapInterval = m_config.SwapInterval;
            }
        }
        if (m_swapchainResizeRequested) {
            MGLOG_D("Present, processing requested swapchain rebuild (resize or swap interval)");
            if (!RecreateSwapchain()) {
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window on requested resize; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
        }

        // 3) Advance frame slot.
        m_frameContext.AdvanceToNext();

        // FrameContext waits then resets the slot fence and frees retired
        // command buffers. A later Present fence is not aggregate proof for
        // earlier pooled flushes, so wait every registered submission in this
        // slot's prefix BEFORE the reset/free happens.
        const Uint64 slotSubmit = m_frameContext.GetCurrent().lastSubmitIndex;
        if (slotSubmit > m_completedSubmitCounter &&
            !WaitForSubmitsUpTo(slotSubmit, UINT64_MAX)) {
            if (LatchWireDeviceLoss("present-slot-submit-wait")) return;
            MagmaWireFatal("present-slot-submit-wait");
        }
        // 4) Wait/reset/acquire for next frame.
        result = m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        if (result == VK_SUBOPTIMAL_KHR) {
            // An image WAS acquired and its signal is armed on this slot's
            // imageAvailableSemaphore, so the frame proceeds normally. Whether a rebuild is
            // actually needed is decided by the surface-capabilities comparison at the next
            // Present - suboptimal alone must not schedule one, or a driver that reports it
            // every frame would rebuild every frame.
            m_swapchainResizeRequested = m_swapchainResizeRequested || SwapchainIsOutOfDate();
            result = VK_SUCCESS;
        } else if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Nothing acquired, nothing signaled: safe to rebuild and re-acquire.
            MGLOG_D("Present, vkAcquireNextImageKHR got %d, recreating swapchain", result);
            if (!RecreateSwapchain()) {
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window on next-frame acquire; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
            result =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        }
        NoteDeviceLoss(result, "vkAcquireNextImageKHR");
        if (result != VK_SUCCESS && LatchWireDeviceLoss("present-acquire")) return;
        VK_VERIFY(result, "Present, vkAcquireNextImageKHR");
        // Pull keeps its historical slot-fence inference. In a disaggregated
        // build the aggregate wait above already retired the registered prefix
        // before FrameContext reset the slot fence; this repeat is idempotent.
        OnSubmitsCompletedUpTo(m_frameContext.GetCurrent().lastSubmitIndex);
        CollectDeferredDepthMipmapCleanup(m_frameContext.GetCurrentFrameIndex());
        // Its previous recording has completed. Drop cached attachment views
        // before the texture manager can release their retired images.
        ClearWireDrawPassCache(m_frameContext.GetCurrentFrameIndex());
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        // Descriptor-set reuse cursors rewind exactly once per frame, here,
        // after the slot's fence wait proved its previous sets GPU-idle. (The
        // per-draw-path lazy rewind missed frames whose recording was opened
        // by a staged buffer copy or timer-query timestamp, leaking a fresh
        // descriptor set per draw for the whole frame; it would also be unsafe
        // after a mid-frame FlushPendingCommands, which does not wait.)
        m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
    }

    Int32 VulkanRenderer::CurrentDrawBufferAge() {
        if (m_surface == VK_NULL_HANDLE || m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended)
            return 0;
        m_swapchainObject.NoteBufferAgeAsked();
        if (m_imageIndexAcquired >= m_swapchainObject.GetImageCount()) return 0;
        // The image the next frame's first draw makes the default framebuffer's (it was acquired at
        // the end of the last present). Its content is a frame only if nothing undefined it since.
        if (!m_swapchainObject.IsImageContentDefined(m_imageIndexAcquired)) return 0;
        return m_swapchainObject.BufferAgeOf(m_imageIndexAcquired);
    }

    void VulkanRenderer::CreateInstance() {
#if defined(VK_USE_PLATFORM_METAL_EXT)
        // MoltenVK snapshots its configuration when the loader first discovers the ICD. Set
        // this before instance-extension enumeration, while preserving an explicit user value.
        if (std::getenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS") == nullptr) {
            if (::setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 0) == 0) {
                MGLOG_I("MoltenVK: enabling Metal argument buffers");
            } else {
                MGLOG_W("MoltenVK: could not enable Metal argument buffers before ICD discovery");
            }
        }
#endif
        m_extensions = EnumerateInstanceExtensions();
        MGLOG_I("Got %d Vulkan instance extensions: ", m_extensions.size());
        for (auto& extension : m_extensions) {
            MGLOG_I("    %s (r.%u)", extension.extensionName, extension.specVersion);
        }

        Bool validationLayerAvailable = CheckValidationLayerSupport();
        MGLOG_I("Validation layers %s.", validationLayerAvailable ? "available" : "not available");
        MGLOG_I("Validation layers %s.", m_config.EnableValidationLayers ? "requested" : "not requested");

        if (m_config.EnableValidationLayers && !validationLayerAvailable) {
            MGLOG_I("Validation layers not available! Disabling validation layers.");
        }

        m_validationLayersEnabled = m_config.EnableValidationLayers && validationLayerAvailable;

        // The debug messenger is a VK_EXT_debug_utils object, but a driver can ship
        // the validation layers while exposing only the older VK_EXT_debug_report
        // (Adreno 650 / Vulkan 1.1.128 does exactly that). Requesting the extension
        // unconditionally tripped the required-extension assert below, aborting every
        // validation-enabled build in CreateInstance. Keep the layers - they still
        // validate, and on Android they report to logcat on their own - and drop only
        // the messenger.
        const Bool debugUtilsAvailable =
            m_validationLayersEnabled && IsExtensionSupported(m_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        // Without a reporting channel the layers validate but say nothing, so fall
        // back to VK_EXT_debug_report when debug_utils is missing.
        const Bool debugReportAvailable = m_validationLayersEnabled && !debugUtilsAvailable &&
                                          IsExtensionSupported(m_extensions, VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        if (m_validationLayersEnabled && !debugUtilsAvailable) {
            MGLOG_I("%s not available; validation reports via %s instead.", VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                    debugReportAvailable ? VK_EXT_DEBUG_REPORT_EXTENSION_NAME : "(no channel)");
        }

        // ---------------- App info -------------------
        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = m_config.AppName.c_str();
        appInfo.applicationVersion = VK_MAKE_VERSION(m_config.CacheVersion, 0, 0);
        appInfo.pEngineName = "MobileGL";
        appInfo.engineVersion = VK_MAKE_VERSION(m_config.Version.Major, m_config.Version.Minor, m_config.Version.Patch);
#ifdef VK_USE_PLATFORM_WIN32_KHR
        appInfo.apiVersion = VK_API_VERSION_1_3;
#else
        appInfo.apiVersion = VK_API_VERSION_1_1;
#endif

        // ---------------- Instance info -------------------
        VkInstanceCreateInfo instanceInfo = {};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

        // Extensions
        Vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME};
        if (!m_window) {
#ifdef VK_USE_PLATFORM_METAL_EXT
            exts.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined VK_USE_PLATFORM_ANDROID_KHR
            m_headlessSurfaceSupported = IsExtensionSupported(m_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
            if (m_headlessSurfaceSupported) {
                exts.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
            } else {
                // No mobile ICD seen so far implements VK_EXT_headless_surface
                // (Mali r32p1 does not), and this used to abort the process the
                // moment an application asked for a pbuffer context. CreateSurface()
                // gives the WSI an AImageReader window instead, so request the
                // Android surface extension for it.
                MGLOG_I("%s not available; falling back to an AImageReader %s surface for the pbuffer context.",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
                exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
                m_androidSurfaceEnabled = true;
            }
#elif defined VK_USE_PLATFORM_XLIB_KHR
            // An offscreen surface has ZERO window-system dependence, by design and on
            // every machine - including ones that do have a display. There used to be a
            // fallback here that requested VK_KHR_xlib_surface and had CreateSurface()
            // open a hidden, never-mapped X window; it is gone. A pbuffer that quietly
            // needs an X server is a pbuffer that works on a workstation and dies on a
            // headless runner, which is exactly what it did: with no DISPLAY, XOpenDisplay
            // returned null and the next Xlib call segfaulted. If the loader genuinely has
            // no VK_EXT_headless_surface, that is an honest bring-up failure and is
            // reported as one below - never papered over with a window.
            m_headlessSurfaceSupported = IsExtensionSupported(m_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
            if (!m_headlessSurfaceSupported) {
                MGLOG_F("%s is not available from this Vulkan loader, so an offscreen (pbuffer) DirectVulkan "
                        "surface cannot be created. Refusing to substitute a window: offscreen surfaces must not "
                        "depend on a window system. Install an ICD that implements it (lavapipe does).",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
                throw RuntimeError("VK_EXT_headless_surface is unavailable for an offscreen DirectVulkan surface");
            }
            exts.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
#else
            exts.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
#endif
        } else {
#ifdef VK_USE_PLATFORM_ANDROID_KHR
            exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
            m_androidSurfaceEnabled = true;
#elif defined VK_USE_PLATFORM_WIN32_KHR
            exts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined VK_USE_PLATFORM_METAL_EXT
            exts.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined VK_USE_PLATFORM_XLIB_KHR
            exts.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#else
#warning "VulkanContext::CreateInstance: VK_KHR_*_surface extension not defined on this platform"
#endif
        } // TODO: support more platforms

#if defined(VK_USE_PLATFORM_METAL_EXT)
        if (IsExtensionSupported(m_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        } else {
            MGLOG_I("Optional Vulkan instance extension not supported: %s",
                    VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        }
#endif

        if (debugUtilsAvailable) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else if (debugReportAvailable) {
            exts.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        }

        MGLOG_I("Enabling %d Vulkan instance extensions:", exts.size());
        for (const char* ext : exts) {
            MGLOG_I("    %s", ext);
        }

        for (const char* ext : exts) {
            if (!IsExtensionSupported(m_extensions, ext)) {
                MGLOG_E("Required Vulkan instance extension not found: %s", ext);
            }
            MOBILEGL_ASSERT(IsExtensionSupported(m_extensions, ext), "Required Vulkan instance extension not found: %s",
                            ext);
        }

        instanceInfo.enabledExtensionCount = exts.size();
        instanceInfo.ppEnabledExtensionNames = exts.data();

        auto debugMessengerCreateInfo = PopulateDebugMessengerCreateInfo();
        // Layers
        const void* instanceCreatePNext = nullptr;
        if (m_validationLayersEnabled) {
            MGLOG_I("Enabling validation layer...");
            instanceInfo.enabledLayerCount = static_cast<uint32_t>(std::size(s_validationLayerNames));
            instanceInfo.ppEnabledLayerNames = s_validationLayerNames;
            // Chaining the messenger create-info is only legal with the extension on.
            instanceCreatePNext = debugUtilsAvailable ? &debugMessengerCreateInfo : nullptr;
        } else {
            instanceInfo.enabledLayerCount = 0;
        }

        instanceInfo.pNext = instanceCreatePNext;

        VK_VERIFY(vkCreateInstance(&instanceInfo, nullptr, &m_instance), "vkCreateInstance failed");

        if (debugUtilsAvailable) {
            VK_VERIFY(SetupDebugMessenger());
        } else if (debugReportAvailable) {
            VK_VERIFY(SetupDebugReportCallback());
        }
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT,
                                                              Uint64, size_t, Int32 messageCode, const char* pLayerPrefix,
                                                              const char* pMessage, void*) {
        if ((flags & (VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
                      VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT)) != 0) {
            // MGLOG_F, unlatched, on purpose: a validation-layer report means MobileGL fed
            // Vulkan something illegal, which is a broken invariant rather than an expected
            // failure mode. It stays loud and keeps repeating - the quietness rules that latch
            // W/E are for expected failures, not for this. The callback is only installed when
            // a build arms the debug report extension, so it costs shipping builds nothing.
            MGLOG_F("[Vulkan %s %d] %s", pLayerPrefix ? pLayerPrefix : "?", messageCode, pMessage ? pMessage : "");
        }
        return VK_FALSE;
    }

    VkResult VulkanRenderer::SetupDebugReportCallback() {
        auto vkCreateDebugReportCallbackEXT =
            (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugReportCallbackEXT");
        if (!vkCreateDebugReportCallbackEXT) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkDebugReportCallbackCreateInfoEXT createInfo{VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
        createInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
                           VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        createInfo.pfnCallback = &DebugReportCallback;
        return vkCreateDebugReportCallbackEXT(m_instance, &createInfo, nullptr, &m_debugReportCallback);
    }

    void VulkanRenderer::DestroyDebugReportCallback() {
        if (m_debugReportCallback == VK_NULL_HANDLE) return;
        auto func = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(m_instance,
                                                                               "vkDestroyDebugReportCallbackEXT");
        if (func != nullptr) func(m_instance, m_debugReportCallback, nullptr);
        m_debugReportCallback = VK_NULL_HANDLE;
    }

    VkResult VulkanRenderer::SetupDebugMessenger() {
        auto createInfo = PopulateDebugMessengerCreateInfo();
        auto vkCreateDebugUtilsMessengerEXT =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (!vkCreateDebugUtilsMessengerEXT) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VK_VERIFY(vkCreateDebugUtilsMessengerEXT(m_instance, &createInfo, nullptr, &m_debugMessenger));
        return VK_SUCCESS;
    }

    VkResult VulkanRenderer::DestroyDebugMessenger() {
        if (m_debugMessenger != VK_NULL_HANDLE) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance,
                                                                                   "vkDestroyDebugUtilsMessengerEXT");
            if (func != nullptr) {
                func(m_instance, m_debugMessenger, nullptr);
            } else {
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }
        return VK_SUCCESS;
    }

    VkDebugUtilsMessengerCreateInfoEXT VulkanRenderer::PopulateDebugMessengerCreateInfo() {
        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = DebugCallback;
        createInfo.pUserData = this;
        return createInfo;
    }

    void VulkanRenderer::PickPhysicalDevice() {
        Uint32 deviceCount = 0;
        VK_VERIFY(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr));
        if (deviceCount == 0) {
            // A real, reachable configuration, not a broken invariant: an instance can be
            // created from ICDs that load perfectly and then expose no device at all - a
            // GPU-less machine with the vendor ICDs installed (RADV/ANV/NVK on a CI runner)
            // is exactly that. It has to be a bring-up failure the caller can report.
            //
            // It used to be MGLOG_E + MOBILEGL_ASSERT, and back then BOTH were compiled out at
            // the INFO log level every shipping and CI build uses - the ordering bug that made
            // MGLOG_E dead at INFO was only fixed in 2026-08. The count-zero case therefore fell
            // through in silence to `devices[0]` on an EMPTY vector below and segfaulted in
            // vkGetPhysicalDeviceProperties. MGLOG_F stays: E is live now, but MOBILEGL_ASSERT
            // is still DEBUG-only and this is a genuine bring-up abort, not a recoverable error.
            MGLOG_F("No Vulkan physical devices found: the instance loaded ICDs but none of them exposes a "
                    "device. Cannot bring up DirectVulkan. (A software ICD such as lavapipe provides one; "
                    "pin it with VK_ICD_FILENAMES if the machine has no GPU.)");
            throw RuntimeError("No Vulkan physical devices available for DirectVulkan");
        }
        MGLOG_I("Found %d physical device(s).", deviceCount);

        Vector<VkPhysicalDevice> devices(deviceCount);
        // Same truncation hazard as the instance-extension enumeration: a VK_INCOMPLETE here
        // leaves the tail of `devices` default-constructed (VK_NULL_HANDLE), and every one of
        // those is a null handle waiting to be passed to the driver. Take only what was
        // actually written.
        const VkResult enumerateResult = vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());
        if (enumerateResult != VK_SUCCESS && enumerateResult != VK_INCOMPLETE) {
            VK_VERIFY(enumerateResult, "vkEnumeratePhysicalDevices failed");
        }
        devices.resize(deviceCount);
        if (devices.empty()) {
            MGLOG_F("vkEnumeratePhysicalDevices reported devices and then wrote none");
            throw RuntimeError("No Vulkan physical devices available for DirectVulkan");
        }
        for (Int i = 0; i < deviceCount; i++) {
            if (GetMoreCapablePhysicalDevice(devices[i], m_surface, m_physicalDevice, m_physicalDevice))
                MGLOG_I("Picked physical device %d.", i);
        }

        if (m_physicalDevice.handle == VK_NULL_HANDLE) {
            m_physicalDevice.handle = devices[0];
            vkGetPhysicalDeviceProperties(devices[0], &m_physicalDevice.properties);
            MGLOG_I("No suitable physical device picked yet, defaulting to device 0.");
            MGLOG_W("No graphics queue found on physical device. Picking a device that doesn't do graphics?");
        }
    }

    Bool VulkanRenderer::GetMoreCapablePhysicalDevice(VkPhysicalDevice newVkDevice, VkSurfaceKHR surface,
                                                      const PhysicalDevice& otherDevice,
                                                      PhysicalDevice& outBetterDevice) {
        const auto deviceTypeToStr = [](VkPhysicalDeviceType type) {
            switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                return "INTEGRATED_GPU";
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                return "DISCRETE_GPU";
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                return "CPU";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                return "VIRTUAL_GPU";
            case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                return "OTHER";
            default:
                return "UNKNOWN";
            }
        };

        PhysicalDevice newDevice;
        newDevice.handle = newVkDevice;

        vkGetPhysicalDeviceProperties(newVkDevice, &newDevice.properties);
        const auto& deviceProperties = newDevice.properties;
        auto apiVersion = deviceProperties.apiVersion;
        MGLOG_I("    %s (Vulkan %d.%d.%d, %s)", deviceProperties.deviceName, VK_VERSION_MAJOR(apiVersion),
                VK_VERSION_MINOR(apiVersion), VK_VERSION_PATCH(apiVersion),
                deviceTypeToStr(deviceProperties.deviceType));

        // Check device extensions (including swapchain extension)
        Bool deviceExtSupported = IsNecessaryDeviceExtensionSupported(newVkDevice);
        if (!deviceExtSupported) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: Some of the required device extension not supported on this "
                    "device)");
            return false;
        }

        // Check swapchain capabilities
        auto swapchainCapabilities = SwapchainObject::GetSwapchainCapabilities(newVkDevice, surface);
        if (!swapchainCapabilities.IsComplete()) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: Swapchain capabilities not met)");
            return false;
        }

        // Check queue families
        Vector<VkQueueFamilyProperties> queueFamilies = GetQueueFamilyFromPhysicalDevice(newVkDevice);
        newDevice.queueFamilies.graphicsFamily = GetQueueFamilyIndex(queueFamilies, VK_QUEUE_GRAPHICS_BIT);
        if (newDevice.queueFamilies.graphicsFamily == -1) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: No graphics queue family)");
            return false;
        }

        newDevice.queueFamilies.presentFamily =
            GetPresentQueueFamilyIndex(newDevice, surface, queueFamilies, newDevice.queueFamilies.graphicsFamily);
        if (newDevice.queueFamilies.presentFamily == -1) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: No present queue family)");
            return false;
        }

        // Accept software/virtual/other devices when no discrete or integrated GPU
        // has been selected yet. This is important for Linux headless CI using lavapipe.
        if (!otherDevice.IsComplete()) {
            outBetterDevice = newDevice;
            MGLOG_I("    Picked physical device. (Reason: First suitable device)");
            return true;
        }

        // Pick discrete GPU
        if (newDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            otherDevice.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            outBetterDevice = newDevice;
            MGLOG_I("    Picked physical device. (Reason: Discrete GPU)");
            return true;
        }

        // Pick integrated GPU if no discrete GPU
        if (newDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            otherDevice.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            outBetterDevice = newDevice;
            MGLOG_I("    Picked physical device. (Reason: Integrated GPU and no discrete one found yet)");
            return true;
        }

        // Ignore other GPU when discrete GPU found
        if (newDevice.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            otherDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            outBetterDevice = otherDevice;
            MGLOG_I("    Ignored physical device. (Reason: Already picked discrete GPU)");
            return false;
        }

        return false;
    }

    Bool VulkanRenderer::IsNecessaryDeviceExtensionSupported(VkPhysicalDevice device) {
        const Vector<VkExtensionProperties> availableExtensions = EnumerateDeviceExtensions(device);

        MGLOG_I("Got %u Vulkan device extensions: ", static_cast<Uint32>(availableExtensions.size()));
        for (auto& extension : availableExtensions) {
            MGLOG_I("    %s (r.%u)", extension.extensionName, extension.specVersion);
        }

        for (SizeT i = 0; i < std::size(s_deviceExtensionNames); ++i) {
            if (!IsExtensionSupported(availableExtensions, s_deviceExtensionNames[i])) {
                MGLOG_I("Required extension not found: %s", s_deviceExtensionNames[i]);
                return false;
            }
            MGLOG_I("Required extension found: %s", s_deviceExtensionNames[i]);
        }

        return true;
    }

    void VulkanRenderer::CreateLogicalDeviceAndQueues() {
        Float queuePriority = 1.0f;

        Vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        MOBILEGL_ASSERT(m_physicalDevice.queueFamilies.graphicsFamily != -1, "Graphics queue family not found.");
        VkDeviceQueueCreateInfo& gfxQueueCreateInfo = queueCreateInfos.emplace_back();
        gfxQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        gfxQueueCreateInfo.queueFamilyIndex = m_physicalDevice.queueFamilies.graphicsFamily;
        gfxQueueCreateInfo.queueCount = 1;
        gfxQueueCreateInfo.pQueuePriorities = &queuePriority;

        if (m_physicalDevice.queueFamilies.graphicsFamily != m_physicalDevice.queueFamilies.presentFamily) {
            MOBILEGL_ASSERT(m_physicalDevice.queueFamilies.presentFamily != -1, "Present queue family not found.");
            VkDeviceQueueCreateInfo& presentQueueCreateInfo = queueCreateInfos.emplace_back();
            presentQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            presentQueueCreateInfo.queueFamilyIndex = m_physicalDevice.queueFamilies.presentFamily;
            presentQueueCreateInfo.queueCount = 1;
            presentQueueCreateInfo.pQueuePriorities = &queuePriority;
        }

        VkPhysicalDeviceFeatures supportedDeviceFeatures{};
        vkGetPhysicalDeviceFeatures(m_physicalDevice.handle, &supportedDeviceFeatures);

        VkPhysicalDeviceFeatures deviceFeatures{};
        // Match GL's robust buffer-fetch behavior where the Vulkan device supports it. This covers
        // out-of-range fetches; arbitrary GL vertex strides/offsets still need the explicit tight
        // repack in VertexInputStateFactory when they violate Vulkan's address-alignment rules.
        // MOBILEGL_MAGMA_DISABLE_ROBUST_BUFFER_ACCESS leaves it off to measure or dodge its GPU cost.
        deviceFeatures.robustBufferAccess = MG_Config::Features.MagmaDisableRobustBufferAccess
                                                ? VK_FALSE
                                                : supportedDeviceFeatures.robustBufferAccess;
        deviceFeatures.geometryShader = supportedDeviceFeatures.geometryShader;
        deviceFeatures.tessellationShader = supportedDeviceFeatures.tessellationShader;
        // gl_PointSize is an ORDINARY per-vertex output in desktop GL - a tessellation
        // evaluation or geometry shader may write it, and a program may capture it by name -
        // but in Vulkan the PointSize built-in is only usable from those two stages when this
        // feature is on (VUID-RuntimeSpirv-PointSize-06439; SPIR-V spells the requirement as
        // the TessellationPointSize / GeometryPointSize capabilities, which glslang emits from
        // any such write). Left off, every one of those programs is invalid usage that a lenient
        // driver silently gives an undefined point size and a strict one faults on. Nothing here
        // asks for it speculatively: the feature is taken only where the device advertises it.
        deviceFeatures.shaderTessellationAndGeometryPointSize =
            supportedDeviceFeatures.shaderTessellationAndGeometryPointSize;
        m_tessellationAndGeometryPointSizeFeatureEnabled =
            deviceFeatures.shaderTessellationAndGeometryPointSize == VK_TRUE;
        // Sampled-read barriers may only name the shader stages whose device feature is
        // actually enabled (VUID-vkCmdPipelineBarrier-srcStageMask-04090/-04091), so the
        // mask is assembled here, next to the feature decision, and handed to consumers.
        m_sampledReadStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        if (deviceFeatures.geometryShader == VK_TRUE) {
            m_sampledReadStageMask |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
        }
        if (deviceFeatures.tessellationShader == VK_TRUE) {
            m_sampledReadStageMask |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                      VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
        }
        deviceFeatures.independentBlend = supportedDeviceFeatures.independentBlend;
        m_independentBlendFeatureEnabled = deviceFeatures.independentBlend == VK_TRUE;
        deviceFeatures.fillModeNonSolid = supportedDeviceFeatures.fillModeNonSolid;
        m_fillModeNonSolidFeatureEnabled = deviceFeatures.fillModeNonSolid == VK_TRUE;
        deviceFeatures.dualSrcBlend = supportedDeviceFeatures.dualSrcBlend;
        m_dualSrcBlendFeatureEnabled = deviceFeatures.dualSrcBlend == VK_TRUE;
        // ARB_sample_shading. Without this feature a pipeline may not set sampleShadingEnable
        // (VUID-VkPipelineMultisampleStateCreateInfo-sampleShadingEnable-00784), so the GL enable
        // has to be dropped rather than forwarded - which is what the flag below records.
        deviceFeatures.sampleRateShading = supportedDeviceFeatures.sampleRateShading;
        m_sampleRateShadingFeatureEnabled = deviceFeatures.sampleRateShading == VK_TRUE;
        // ARB_viewport_array rasterization. Without multiViewport a pipeline may declare exactly
        // one viewport (VUID-VkPipelineViewportStateCreateInfo-viewportCount-01216), so a shader's
        // gl_ViewportIndex can only ever select viewport 0 and the other fifteen rectangles are
        // state with nowhere to go. The GL state stays 16 wide either way - GL 4.3 core requires
        // MAX_VIEWPORTS >= 16 and that is a frontend promise, not a device one; this gate decides
        // only whether a DRAW can rasterize into more than one of them.
        deviceFeatures.multiViewport = supportedDeviceFeatures.multiViewport;
        m_multiViewportFeatureEnabled = deviceFeatures.multiViewport == VK_TRUE;
        m_maxRasterizableViewports =
            m_multiViewportFeatureEnabled
                ? std::min<Uint32>(RenderStateParameters::MAX_VIEWPORTS,
                                   std::max<Uint32>(m_physicalDevice.properties.limits.maxViewports, 1u))
                : 1u;
        MGLOG_I("Vulkan: multiViewport %s; rasterizable viewports=%u (device limit %u, GL state width %u)",
                m_multiViewportFeatureEnabled ? "enabled" : "UNAVAILABLE", m_maxRasterizableViewports,
                m_physicalDevice.properties.limits.maxViewports,
                static_cast<Uint32>(RenderStateParameters::MAX_VIEWPORTS));
        if (!m_multiViewportFeatureEnabled) {
            MGLOG_W("Vulkan: the device does not support the multiViewport feature; gl_ViewportIndex will always "
                    "select viewport 0 and per-viewport scissor/depth-range state past index 0 cannot be "
                    "rasterized (the state itself is still stored and queryable)");
        }
        deviceFeatures.logicOp = supportedDeviceFeatures.logicOp;
        deviceFeatures.shaderClipDistance = supportedDeviceFeatures.shaderClipDistance;
        deviceFeatures.shaderCullDistance = supportedDeviceFeatures.shaderCullDistance;
        deviceFeatures.wideLines = supportedDeviceFeatures.wideLines;
        m_logicOpFeatureEnabled = deviceFeatures.logicOp == VK_TRUE;
        deviceFeatures.shaderInt64 = supportedDeviceFeatures.shaderInt64;
        // Required for any module that declares OpCapability Float64 - which is every shader with a
        // double in it, including the 64-bit vertex attribute path (the attribute itself arrives as
        // uint32 words, but the bitcast result and everything computed from it is Float64). Without
        // it vkCreateShaderModule is invalid usage (VUID-VkShaderModuleCreateInfo-pCode-08740),
        // which is why SupportsFloat64VertexAttributes gates the entry point on the same feature.
        deviceFeatures.shaderFloat64 = supportedDeviceFeatures.shaderFloat64;
        // Required before a VK_IMAGE_VIEW_TYPE_CUBE_ARRAY view may be created
        // (VUID-VkImageViewCreateInfo-viewType-01004). Without it a cube map array texture cannot
        // get its sampled or full view, so SyncTextureResource fails and the texture stays unbacked.
        deviceFeatures.imageCubeArray = supportedDeviceFeatures.imageCubeArray;
        // Required for desktop GL image load/store semantics. iterationRP writes storage
        // images from vertex and fragment stages and uses formats outside Vulkan's small
        // mandatory storage-image set.
        deviceFeatures.vertexPipelineStoresAndAtomics =
            supportedDeviceFeatures.vertexPipelineStoresAndAtomics;
        deviceFeatures.fragmentStoresAndAtomics = supportedDeviceFeatures.fragmentStoresAndAtomics;
        deviceFeatures.shaderStorageImageExtendedFormats =
            supportedDeviceFeatures.shaderStorageImageExtendedFormats;
        // The formatless float-storage compatibility path must be all-or-nothing: transformed
        // modules declare both capabilities and image bindings may be read, written, or both.
        m_unformattedFloatStorageImagesEnabled =
            supportedDeviceFeatures.shaderStorageImageReadWithoutFormat == VK_TRUE &&
            supportedDeviceFeatures.shaderStorageImageWriteWithoutFormat == VK_TRUE;
        if (m_unformattedFloatStorageImagesEnabled) {
            deviceFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
            deviceFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        } else {
            // Surface the degradation instead of failing silently: shader packs that bind a
            // float storage image with a format different from its declaration (e.g.
            // iterationRP) will render incorrectly on this device.
            MGLOG_W("CreateLogicalDeviceAndQueues: shaderStorageImage*WithoutFormat unavailable "
                    "(read=%d write=%d); float storage-image format reinterpretation is disabled "
                    "and packs relying on it may misrender",
                    supportedDeviceFeatures.shaderStorageImageReadWithoutFormat,
                    supportedDeviceFeatures.shaderStorageImageWriteWithoutFormat);
        }
        deviceFeatures.drawIndirectFirstInstance = supportedDeviceFeatures.drawIndirectFirstInstance;
        m_drawIndirectFirstInstanceFeatureEnabled = deviceFeatures.drawIndirectFirstInstance == VK_TRUE;
        deviceFeatures.multiDrawIndirect = supportedDeviceFeatures.multiDrawIndirect;
        m_multiDrawIndirectFeatureEnabled = deviceFeatures.multiDrawIndirect == VK_TRUE;
        m_logicOpFeatureEnabled = deviceFeatures.logicOp == VK_TRUE;
        // Backs GL_TEXTURE_MAX_ANISOTROPY_EXT; optional in Vulkan, so the sampler manager falls back
        // to isotropic filtering (and the extension goes unadvertised) when the device lacks it.
        deviceFeatures.samplerAnisotropy = supportedDeviceFeatures.samplerAnisotropy;
        m_samplerAnisotropyFeatureEnabled = deviceFeatures.samplerAnisotropy == VK_TRUE;
        // GL_SAMPLES_PASSED needs exact sample counts; without the feature the boolean
        // occlusion result still satisfies any-samples-style consumers.
        deviceFeatures.occlusionQueryPrecise = supportedDeviceFeatures.occlusionQueryPrecise;
        m_occlusionQueryPreciseEnabled = deviceFeatures.occlusionQueryPrecise == VK_TRUE;
        m_tessellationShaderFeatureEnabled = deviceFeatures.tessellationShader == VK_TRUE;
        // Backs the GL_PRIMITIVES_GENERATED reroute's statistics tier (see the
        // m_primGenReroute* members): a VK_QUERY_TYPE_PIPELINE_STATISTICS pool may only
        // be created with this feature enabled. Enabled wherever the device has it - the
        // feature alone costs nothing; pools exist only where the reroute is armed.
        deviceFeatures.pipelineStatisticsQuery = supportedDeviceFeatures.pipelineStatisticsQuery;
        m_pipelineStatisticsQueryFeatureEnabled = deviceFeatures.pipelineStatisticsQuery == VK_TRUE;

        VkDeviceCreateInfo deviceCreateInfo{};
        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
        deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
        if (m_validationLayersEnabled) {
            deviceCreateInfo.enabledLayerCount = static_cast<uint32_t>(std::size(s_validationLayerNames));
            deviceCreateInfo.ppEnabledLayerNames = s_validationLayerNames;
        } else {
            deviceCreateInfo.enabledLayerCount = 0;
        }

        Vector<const char*> enabledDeviceExtensions;
        enabledDeviceExtensions.reserve(std::size(s_deviceExtensionNames) + 2);
        for (const char* extensionName : s_deviceExtensionNames) {
            enabledDeviceExtensions.push_back(extensionName);
        }

        const Vector<VkExtensionProperties> availableExtensions = EnumerateDeviceExtensions(m_physicalDevice.handle);
        ResolveOptionalDeviceExtensions(availableExtensions, enabledDeviceExtensions);

        // VK_KHR_image_format_list lets a MUTABLE_FORMAT image declare exactly which formats it
        // may be viewed as. Adreno drops UBWC bandwidth compression on a blindly-mutable image
        // (measured: 65 -> 80 fps in MC 26.2 once mutability is not requested); an explicit,
        // compression-compatible format list is the portable way to keep both.
        m_imageFormatListExtensionEnabled =
            IsExtensionSupported(availableExtensions, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        if (m_imageFormatListExtensionEnabled) {
            enabledDeviceExtensions.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        }
        MGLOG_I("VK_KHR_image_format_list enabled: %s",
                m_imageFormatListExtensionEnabled ? "true" : "false");
        MGLOG_I("VK_KHR_draw_indirect_count enabled: %s", m_drawIndirectCountExtensionEnabled ? "true" : "false");

        m_indexTypeUint8ExtensionEnabled = false;
        const char* indexTypeUint8ExtensionName = nullptr;
        if (IsExtensionSupported(availableExtensions, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME)) {
            indexTypeUint8ExtensionName = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME;
        } else if (IsExtensionSupported(availableExtensions, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME)) {
            indexTypeUint8ExtensionName = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME;
        }

        auto getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFeatures2"));
        if (getPhysicalDeviceFeatures2 == nullptr) {
            getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFeatures2KHR"));
        }

        m_updateAfterBindLimits = {};
        VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
        descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        VkPhysicalDeviceDescriptorIndexingProperties descriptorIndexingProperties{};
        descriptorIndexingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
        const Bool descriptorIndexingCore =
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
            // CreateInstance requests Vulkan 1.1 on these platforms. A 1.2+ GPU
            // does not promote descriptor indexing into that application's core
            // API, so the record arm enables VK_EXT_descriptor_indexing instead (P13 W6:
            // every library, not only the transport build - the old monolith-only `true`
            // here treated the 1.2 device as core under a 1.1 instance).
            false &&
#endif
            m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_2;
        const Bool descriptorIndexingExtension =
            IsExtensionSupported(availableExtensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
        if (getPhysicalDeviceProperties2 == nullptr) {
            getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
        }
        // Linux/Android request Vulkan 1.1: a 1.2 physical device alone does
        // not expose the promoted renderpass2/depth-resolve API to this app.
#ifdef VK_USE_PLATFORM_WIN32_KHR
        const Bool wireDepthResolveCore = m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_2;
#else
        const Bool wireDepthResolveCore = false;
#endif
        const Bool wireDepthResolveExtensions =
            m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_1 &&
            IsExtensionSupported(availableExtensions, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) &&
            IsExtensionSupported(availableExtensions, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        const Bool wireDepthResolveEnabled = getPhysicalDeviceProperties2 && (wireDepthResolveCore || wireDepthResolveExtensions);
        m_wireCreateRenderPass2 = nullptr;
        m_wireDepthResolveModes = m_wireStencilResolveModes = 0;
        if (wireDepthResolveEnabled) {
            if (!wireDepthResolveCore) {
                EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions,
                                              VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
                EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions,
                                              VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
            }
            VkPhysicalDeviceDepthStencilResolveProperties resolveProperties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &resolveProperties;
            getPhysicalDeviceProperties2(m_physicalDevice.handle, &properties);
            m_wireDepthResolveModes = resolveProperties.supportedDepthResolveModes;
            m_wireStencilResolveModes = resolveProperties.supportedStencilResolveModes;
        }
        // P7 wave 2-B2, CONTRACT-P7 §3.2 (`multisample-blit-aspect`): the shader resolve's
        // stencil half needs SPV_EXT_shader_stencil_export, because a fragment shader cannot
        // write the stencil aspect without it. Enabled on the same terms as the resolve
        // extensions above - wire arms only - and the arm declines stencil where it is absent
        // rather than producing an undefined aspect.
        m_wireShaderStencilExport = IsExtensionSupported(availableExtensions, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
        if (m_wireShaderStencilExport)
            EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions,
                                          VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
        // P7 gate 5 (g5-msprobe): which multisample depth/stencil resolve arm goes first is measured
        // once the device exists (ArmWireDepthResolveOrder, below), not keyed on the vendor.
        m_wirePreferShaderDepthResolve = false;
        // P11 B2 (T0): a client's AHardwareBuffer imported as a wire buffer store. Wire arms, on
        // Android, whenever the device advertises the extension - not per session: the device is
        // created once per server and a later session asking T0 must find it able. Enabling it
        // changes no T2 behaviour; only an import uses it. The 1.0-era dependencies are core at the
        // 1.1 this device is created at, and are enabled too when advertised (the spike's set).
        m_wireAhbImport = false;
        m_wireGetAhbProperties = nullptr;
        m_sharedImageSyncFd = false;
#if defined(__ANDROID__)
        if (m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_1 &&
            IsExtensionSupported(availableExtensions, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) &&
            IsExtensionSupported(availableExtensions, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
            m_wireAhbImport = true;
            for (const char* name : {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                                     VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                                     VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
                                     VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                     VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                                     VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_1_EXTENSION_NAME}) {
                if (IsExtensionSupported(availableExtensions, name))
                    EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions, name);
            }
            // Shared-image fences (WireSharedImage.inc) cross sessions as sync_files, which this
            // device waits on and signals through SYNC_FD binary semaphores - imported for a wait,
            // exported for a signal. Both directions, or the CPU fallback.
            if (IsExtensionSupported(availableExtensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
                auto getExternalSemaphoreProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                    vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
                if (getExternalSemaphoreProperties == nullptr) {
                    getExternalSemaphoreProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                        vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"));
                }
                VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
                if (getExternalSemaphoreProperties != nullptr) {
                    VkPhysicalDeviceExternalSemaphoreInfo query{
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
                    query.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                    getExternalSemaphoreProperties(m_physicalDevice.handle, &query, &properties);
                }
                constexpr VkExternalSemaphoreFeatureFlags both =
                    VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
                if ((properties.externalSemaphoreFeatures & both) == both) {
                    m_sharedImageSyncFd = true;
                    for (const char* name : {VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                                             VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME}) {
                        if (IsExtensionSupported(availableExtensions, name))
                            EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions, name);
                    }
                }
            }
            MGLOG_I("DirectVulkan: shared-image fences %s",
                    m_sharedImageSyncFd ? "wait and signal on the GPU (SYNC_FD semaphores)"
                                        : "are waited for on the CPU (no SYNC_FD semaphore import and export)");
        }
#endif
        if ((descriptorIndexingCore || descriptorIndexingExtension) && getPhysicalDeviceFeatures2 != nullptr &&
            getPhysicalDeviceProperties2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &descriptorIndexingFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            VkPhysicalDeviceProperties2 propertyQuery{};
            propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            propertyQuery.pNext = &descriptorIndexingProperties;
            getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);

            // This renderer emits every descriptor category listed below, including
            // dynamic UBOs and combined image samplers. Do not enable a partial
            // descriptor-indexing contract: it would make a later reflected program
            // fail in the driver instead of choosing its ordinary descriptor layout.
            const Bool allUpdateAfterBindFeatures =
                descriptorIndexingFeatures.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_TRUE &&
                descriptorIndexingFeatures.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_TRUE &&
                (!deviceFeatures.robustBufferAccess || descriptorIndexingProperties.robustBufferAccessUpdateAfterBind);
            if (allUpdateAfterBindFeatures) {
                if (!descriptorIndexingCore && !IsExtensionAlreadyEnabled(
                                                   enabledDeviceExtensions,
                                                   VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
                }
                descriptorIndexingFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &descriptorIndexingFeatures;
                m_updateAfterBindLimits = {
                    true,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindSamplers,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindUniformBuffers,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages,
                    descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindStorageImages,
                    descriptorIndexingProperties.maxPerStageUpdateAfterBindResources,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSamplers,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindUniformBuffers,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageBuffers,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSampledImages,
                    descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageImages};
                MGLOG_I("Vulkan: update-after-bind descriptor layouts enabled");
            } else {
                MGLOG_I("Vulkan: descriptor indexing is present but lacks the complete update-after-bind feature set; "
                        "using ordinary descriptor layouts");
            }
        } else {
            MGLOG_I("Vulkan: descriptor indexing unavailable; using ordinary descriptor layouts");
        }

        VkPhysicalDeviceIndexTypeUint8Features indexTypeUint8Features{};
        indexTypeUint8Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES;
        if (indexTypeUint8ExtensionName != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &indexTypeUint8Features;
            MOBILEGL_ASSERT(getPhysicalDeviceFeatures2 != nullptr,
                            "CreateLogicalDeviceAndQueues: vkGetPhysicalDeviceFeatures2 is unavailable");
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (indexTypeUint8Features.indexTypeUint8 == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, indexTypeUint8ExtensionName)) {
                    enabledDeviceExtensions.push_back(indexTypeUint8ExtensionName);
                }
                m_indexTypeUint8ExtensionEnabled = true;
                indexTypeUint8Features.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &indexTypeUint8Features;
                MGLOG_I("Enabled optional device extension: %s", indexTypeUint8ExtensionName);
            } else {
                MGLOG_W("%s is advertised, but indexTypeUint8 feature is unavailable; uint8 index buffers will stay disabled",
                        indexTypeUint8ExtensionName);
            }
        } else {
            MGLOG_W("VK_KHR_index_type_uint8 / VK_EXT_index_type_uint8 not supported; uint8 index buffers will stay disabled");
        }

        m_shaderDrawParametersFeatureEnabled = false;
        VkPhysicalDeviceShaderDrawParametersFeatures shaderDrawParametersFeatures{};
        shaderDrawParametersFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
        if (m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_1 && getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &shaderDrawParametersFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (shaderDrawParametersFeatures.shaderDrawParameters == VK_TRUE) {
                shaderDrawParametersFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &shaderDrawParametersFeatures;
                m_shaderDrawParametersFeatureEnabled = true;
            }
        } else if (m_shaderDrawParametersExtensionEnabled) {
            // Vulkan 1.0 device: enabling VK_KHR_shader_draw_parameters alone exposes the SPIR-V
            // DrawParameters capability; the shaderDrawParameters feature struct only exists from 1.1.
            m_shaderDrawParametersFeatureEnabled = true;
        }
        if (!m_shaderDrawParametersFeatureEnabled) {
            MGLOG_W("shaderDrawParameters is unavailable; shaders using gl_DrawID/gl_BaseInstance will not work");
        }

        // primitiveTopologyListRestart lets primitive restart work on *list* topologies (strip/fan
        // restart needs no feature). Optional; enabled via VK_EXT_primitive_topology_list_restart.
        m_primitiveTopologyListRestartFeatureEnabled = false;
        VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT listRestartFeatures{};
        listRestartFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &listRestartFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (listRestartFeatures.primitiveTopologyListRestart == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions,
                                               VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
                }
                listRestartFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &listRestartFeatures;
                m_primitiveTopologyListRestartFeatureEnabled = true;
                MGLOG_I("Enabled optional device extension: %s",
                        VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
            }
        }

        // VK_EXT_custom_border_color: an arbitrary GL_TEXTURE_BORDER_COLOR, in float or integer form,
        // instead of the four predefined VkBorderColor values. Without it a border outside
        // transparent black / opaque black / opaque white has to be snapped, which is what made every
        // border texel of a GL_RGBA8 texture with border (255,255,255,255) sample as 0 and what made
        // an integer border of -1 come back as 0.
        //
        // customBorderColorWithoutFormat is required alongside customBorderColors, not merely
        // preferred: a GL sampler object carries a border colour with no idea which texture it will
        // be paired with, so the VkSamplerCustomBorderColorCreateInfoEXT this backend builds has to
        // leave `format` VK_FORMAT_UNDEFINED.
        m_customBorderColorFeatureEnabled = false;
        m_maxCustomBorderColorSamplers = 0;
        VkPhysicalDeviceCustomBorderColorFeaturesEXT customBorderColorFeatures{};
        customBorderColorFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &customBorderColorFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (customBorderColorFeatures.customBorderColors == VK_TRUE &&
                customBorderColorFeatures.customBorderColorWithoutFormat == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions,
                                               VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
                }
                customBorderColorFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &customBorderColorFeatures;
                m_customBorderColorFeatureEnabled = true;

                if (getPhysicalDeviceProperties2 != nullptr) {
                    VkPhysicalDeviceCustomBorderColorPropertiesEXT customBorderColorProperties{};
                    customBorderColorProperties.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT;
                    VkPhysicalDeviceProperties2 propertyQuery{};
                    propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    propertyQuery.pNext = &customBorderColorProperties;
                    getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);
                    m_maxCustomBorderColorSamplers = customBorderColorProperties.maxCustomBorderColorSamplers;
                }
                MGLOG_I("Enabled optional device extension: %s (maxCustomBorderColorSamplers=%u)",
                        VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, m_maxCustomBorderColorSamplers);
            }
        }
        if (!m_customBorderColorFeatureEnabled) {
            MGLOG_I("%s unavailable; GL_TEXTURE_BORDER_COLOR snaps to the nearest predefined VkBorderColor",
                    VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        }

        // Native subgroup topology, and VK_EXT_subgroup_size_control's
        // computeFullSubgroups feature. REQUIRE_FULL_SUBGROUPS on a compute stage is what
        // turns the derived gl_NumSubgroups (DeriveNumSubgroupsPass) from
        // encouraged-but-unspecified driver behaviour into a spec guarantee: with the bit
        // set and local_size_x a multiple of the subgroup size, every subgroup launches
        // full, so the subgroup count is exactly invocations / size ("Full Subgroups",
        // VUID-VkPipelineShaderStageCreateInfo-flags-02759/-02785).
        m_nativeSubgroupSize = 0;
        m_nativeSubgroupSupported = false;
        m_computeFullSubgroupsFeatureEnabled = false;
        if (getPhysicalDeviceProperties2 != nullptr) {
            VkPhysicalDeviceSubgroupProperties subgroupProperties{};
            subgroupProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
            VkPhysicalDeviceProperties2 subgroupPropertyQuery{};
            subgroupPropertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            subgroupPropertyQuery.pNext = &subgroupProperties;
            getPhysicalDeviceProperties2(m_physicalDevice.handle, &subgroupPropertyQuery);
            // Mirrors the loader's HasUsableShaderSubgroupSupport gate, including the
            // MOBILEGL_MAGMA_DISABLE_SUBGROUP escape hatch, so the module lowerings can never
            // disagree with the advertised capabilities.
            const Bool usableSubgroups =
                subgroupProperties.subgroupSize > 0 &&
                (subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
                (subgroupProperties.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
            if (usableSubgroups && !MG_Config::Features.MagmaDisableSubgroup) {
                m_nativeSubgroupSize = subgroupProperties.subgroupSize;
                m_nativeSubgroupSupported = true;
            }
        }
        VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroupSizeControlFeatures{};
        subgroupSizeControlFeatures.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT;
        m_maxComputeWorkgroupSubgroups = 0;
        if (m_nativeSubgroupSupported &&
            IsExtensionSupported(availableExtensions, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &subgroupSizeControlFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (getPhysicalDeviceProperties2 != nullptr) {
                VkPhysicalDeviceSubgroupSizeControlPropertiesEXT subgroupSizeControlProperties{};
                subgroupSizeControlProperties.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT;
                VkPhysicalDeviceProperties2 propertyQuery{};
                propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                propertyQuery.pNext = &subgroupSizeControlProperties;
                getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);
                m_maxComputeWorkgroupSubgroups =
                    subgroupSizeControlProperties.maxComputeWorkgroupSubgroups;
            }
            if (subgroupSizeControlFeatures.computeFullSubgroups == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions,
                                               VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
                }
                // Only the full-subgroups guarantee is wanted; required/varying subgroup
                // sizes stay unrequested.
                subgroupSizeControlFeatures.subgroupSizeControl = VK_FALSE;
                subgroupSizeControlFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &subgroupSizeControlFeatures;
                m_computeFullSubgroupsFeatureEnabled = true;
                MGLOG_I("Enabled optional device extension: %s (computeFullSubgroups)",
                        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
            }
        }

        // VK_EXT_transform_feedback backs GL transform feedback capture.
        m_transformFeedbackFeatureEnabled = false;
        VkPhysicalDeviceTransformFeedbackFeaturesEXT transformFeedbackFeatures{};
        transformFeedbackFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &transformFeedbackFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (transformFeedbackFeatures.transformFeedback == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
                }
                transformFeedbackFeatures.geometryStreams = VK_FALSE;
                transformFeedbackFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &transformFeedbackFeatures;
                m_transformFeedbackFeatureEnabled = true;
                MGLOG_I("Enabled optional device extension: %s", VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
            }
        }
        // VK_EXT_primitives_generated_query - the query Vulkan defines for GL's
        // GL_PRIMITIVES_GENERATED precisely because the stream query above needs no
        // capture by spec but drivers disagree. Taken with BOTH the base feature and the
        // rasterizer-discard feature or not at all: without the latter, a discarding draw
        // inside the query is invalid usage, and GL applications toggle discard freely.
        // Only the PRIMITIVES_GENERATED reroute consumes it (see ArmPrimGenReroute).
        m_primitivesGeneratedQueryFeatureEnabled = false;
        m_primitivesGeneratedQueryDiscardFeatureEnabled = false;
        VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT primitivesGeneratedQueryFeatures{};
        primitivesGeneratedQueryFeatures.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &primitivesGeneratedQueryFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (primitivesGeneratedQueryFeatures.primitivesGeneratedQuery == VK_TRUE &&
                primitivesGeneratedQueryFeatures.primitivesGeneratedQueryWithRasterizerDiscard == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions,
                                               VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME);
                }
                primitivesGeneratedQueryFeatures.primitivesGeneratedQueryWithNonZeroStreams = VK_FALSE;
                primitivesGeneratedQueryFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &primitivesGeneratedQueryFeatures;
                m_primitivesGeneratedQueryFeatureEnabled = true;
                m_primitivesGeneratedQueryDiscardFeatureEnabled = true;
                MGLOG_I("Enabled optional device extension: %s",
                        VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME);
            }
        }
        // VK_EXT_provoking_vertex. Two independent features live behind one extension:
        //   provokingVertexLast                       -> flat varyings, gl_Layer/gl_ViewportIndex and
        //                                                the input-assembler capture order.
        //   transformFeedbackPreservesProvokingVertex -> spec-level guarantee for the capture order;
        //                                                only legal when the transformFeedback
        //                                                feature is also enabled, which is why this
        //                                                block sits after the one above.
        // They are enabled independently on purpose: gating the first on the second would leave flat
        // shading GL-wrong on any device without VK_EXT_transform_feedback, for no legality reason.
        m_provokingVertexLastEnabled = false;
        m_provokingVertexXfbPreserveEnabled = false;
        m_provokingVertexModePerPipeline = false;
        m_provokingVertexFanPreserved = false;
        VkPhysicalDeviceProvokingVertexFeaturesEXT provokingVertexFeatures{};
        provokingVertexFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &provokingVertexFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);

            VkPhysicalDeviceProvokingVertexPropertiesEXT provokingVertexProperties{};
            provokingVertexProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT;
            auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
            if (getPhysicalDeviceProperties2 == nullptr) {
                getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
            }
            if (getPhysicalDeviceProperties2 != nullptr) {
                VkPhysicalDeviceProperties2 propertyQuery{};
                propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                propertyQuery.pNext = &provokingVertexProperties;
                getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);
            }
            m_provokingVertexModePerPipeline = provokingVertexProperties.provokingVertexModePerPipeline == VK_TRUE;
            m_provokingVertexFanPreserved =
                provokingVertexProperties.transformFeedbackPreservesTriangleFanProvokingVertex == VK_TRUE;

            if (provokingVertexFeatures.provokingVertexLast == VK_TRUE) {
                // transformFeedbackPreservesProvokingVertex is deliberately NOT requested. Measured:
                // asking for it regresses transform_feedback.geometry on GL33 through GL45. A
                // geometry shader emits its triangles already in GL's vertex order, and the pipeline
                // that captures them runs on FIRST (see SelectProvokingVertexMode); without the
                // guarantee the driver leaves that stream alone, but with it the capture is forced to
                // follow the pipeline's FIRST convention and comes back rotated. The guarantee buys
                // nothing here either - the input-assembler capture order that
                // direct_state_access.queries_functional needs comes from provokingVertexLast alone,
                // which was confirmed by measurement. Leaving it off also keeps VU 04884 disarmed, so
                // a TRIANGLE_FAN pipeline may take LAST on any device.
                const Bool wantXfbPreserve = false;

                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
                }
                provokingVertexFeatures.provokingVertexLast = VK_TRUE;
                provokingVertexFeatures.transformFeedbackPreservesProvokingVertex =
                    wantXfbPreserve ? VK_TRUE : VK_FALSE;
                provokingVertexFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &provokingVertexFeatures;
                m_provokingVertexLastEnabled = true;
                m_provokingVertexXfbPreserveEnabled = wantXfbPreserve;
                MGLOG_I("Enabled optional device extension: %s (transformFeedbackPreservesProvokingVertex=%s)",
                        VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, wantXfbPreserve ? "true" : "false");
            }
        }
        if (!m_provokingVertexLastEnabled) {
            MGLOG_W("VK_EXT_provoking_vertex is unavailable; flat-shaded varyings take a primitive's first "
                    "vertex instead of GL's last, and transform feedback records TRIANGLE_STRIP/TRIANGLE_FAN "
                    "triangles rotated (0,1,2 / 1,3,2 instead of 0,1,2 / 2,1,3)");
        }

        if (!m_transformFeedbackFeatureEnabled) {
            MGLOG_W("VK_EXT_transform_feedback is unavailable; transform feedback capture will not work");
        }

        // VK_EXT_vertex_attribute_divisor. Vulkan's instance input rate advances an attribute
        // once per instance and nothing else, so without this every glVertexAttribDivisor value
        // collapses to 1 and an attribute meant to change every N instances changes every one.
        m_vertexAttributeDivisorEnabled = false;
        VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vertexAttributeDivisorFeatures{};
        vertexAttributeDivisorFeatures.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &vertexAttributeDivisorFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (vertexAttributeDivisorFeatures.vertexAttributeInstanceRateDivisor == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions,
                                               VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
                }
                vertexAttributeDivisorFeatures.vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
                vertexAttributeDivisorFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &vertexAttributeDivisorFeatures;
                m_vertexAttributeDivisorEnabled = true;
                MGLOG_I("Enabled optional device extension: %s", VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
            }
        }
        if (!m_vertexAttributeDivisorEnabled) {
            MGLOG_W("VK_EXT_vertex_attribute_divisor is unavailable; a glVertexAttribDivisor other "
                    "than 1 will advance its attribute once per instance");
        }

        // Host query reset lets the occlusion-query ring recycle slots without a
        // command-buffer round trip.
        m_hostQueryResetEnabled = false;
        VkPhysicalDeviceHostQueryResetFeatures hostQueryResetFeatures{};
        hostQueryResetFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
        if (IsExtensionSupported(availableExtensions, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &hostQueryResetFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (hostQueryResetFeatures.hostQueryReset == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
                }
                hostQueryResetFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &hostQueryResetFeatures;
                m_hostQueryResetEnabled = true;
            }
        }

        // YUV shared images (WireYuvImage.inc) are sampled through a VkSamplerYcbcrConversion, a
        // feature of its own; asked only where AHardwareBuffers are imported at all.
        m_samplerYcbcrConversion = false;
        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures{};
        ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        if (m_wireAhbImport && getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &ycbcrFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (ycbcrFeatures.samplerYcbcrConversion == VK_TRUE) {
                ycbcrFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &ycbcrFeatures;
                m_samplerYcbcrConversion = true;
            }
        }

        // VK_EXT_multi_draw: tier 1 of the multi-draw dispatch - one vkCmdDrawMulti(Indexed)EXT
        // for a whole glMultiDraw* batch (VkMultiDrawIndexedInfoEXT carries per-draw
        // firstIndex/indexCount/vertexOffset, so glMultiDrawElementsBaseVertex fits natively).
        // Requested only when both the extension and its multiDraw feature are present;
        // absent it, the dispatch falls to the multiDrawIndirect tier or the unrolled loop.
        m_multiDrawExtensionEnabled = false;
        m_maxMultiDrawCount = 0;
        VkPhysicalDeviceMultiDrawFeaturesEXT multiDrawFeatures{};
        multiDrawFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT;
        if (IsExtensionSupported(availableExtensions, VK_EXT_MULTI_DRAW_EXTENSION_NAME) &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &multiDrawFeatures;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (multiDrawFeatures.multiDraw == VK_TRUE) {
                if (!IsExtensionAlreadyEnabled(enabledDeviceExtensions, VK_EXT_MULTI_DRAW_EXTENSION_NAME)) {
                    enabledDeviceExtensions.push_back(VK_EXT_MULTI_DRAW_EXTENSION_NAME);
                }
                multiDrawFeatures.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &multiDrawFeatures;
                m_multiDrawExtensionEnabled = true;

                VkPhysicalDeviceMultiDrawPropertiesEXT multiDrawProperties{};
                multiDrawProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT;
                auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
                if (getPhysicalDeviceProperties2 == nullptr) {
                    getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                        vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
                }
                if (getPhysicalDeviceProperties2 != nullptr) {
                    VkPhysicalDeviceProperties2 propertyQuery{};
                    propertyQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    propertyQuery.pNext = &multiDrawProperties;
                    getPhysicalDeviceProperties2(m_physicalDevice.handle, &propertyQuery);
                }
                // Spec minimum is 1024; a driver reporting 0 through a failed query must not
                // zero out every batch, so fall back to the spec minimum.
                m_maxMultiDrawCount = multiDrawProperties.maxMultiDrawCount != 0
                                          ? multiDrawProperties.maxMultiDrawCount
                                          : 1024;
                MGLOG_I("Enabled optional device extension: %s (maxMultiDrawCount=%u)",
                        VK_EXT_MULTI_DRAW_EXTENSION_NAME, m_maxMultiDrawCount);
            } else {
                MGLOG_I("VK_EXT_multi_draw is advertised but its multiDraw feature is unavailable; "
                        "multi-draw batches use the indirect or unrolled tier");
            }
        }

        // Codex closeout finding 2 (cf-magma): GL 4.6 core 8.26 makes an access through an
        // invalid image unit load zero and DISCARD stores and atomics. A null storage-image
        // descriptor is that rule exactly, so the wire arm binds one for such a unit
        // (UniformManager::SetWireInvalidStorageImageArm). ONLY nullDescriptor is requested:
        // robustBufferAccess2 / robustImageAccess2 would put a bounds check on every access of
        // every draw. Wire arms only, on the same terms as the depth-resolve extensions above -
        // the monolith arm resolves image units elsewhere and keeps its device as it was.
        m_wireNullDescriptor = false;
        VkPhysicalDeviceRobustness2FeaturesEXT robustness2Features{};
        robustness2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        const char* robustness2Name =
            IsExtensionSupported(availableExtensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ? VK_EXT_ROBUSTNESS_2_EXTENSION_NAME
            : IsExtensionSupported(availableExtensions, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME) ? VK_KHR_ROBUSTNESS_2_EXTENSION_NAME
                                                                                            : nullptr;
        if (robustness2Name != nullptr &&
            getPhysicalDeviceFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            featureQuery.pNext = &robustness2Features;
            getPhysicalDeviceFeatures2(m_physicalDevice.handle, &featureQuery);
            if (robustness2Features.nullDescriptor == VK_TRUE) {
                EnableOptionalDeviceExtension(availableExtensions, enabledDeviceExtensions, robustness2Name);
                robustness2Features.robustBufferAccess2 = VK_FALSE;
                robustness2Features.robustImageAccess2 = VK_FALSE;
                robustness2Features.pNext = const_cast<void*>(deviceCreateInfo.pNext);
                deviceCreateInfo.pNext = &robustness2Features;
                m_wireNullDescriptor = true;
            }
        }
        MGLOG_I("Magma wire: robustness2 nullDescriptor %s (%s)", m_wireNullDescriptor ? "enabled" : "unavailable",
                robustness2Name != nullptr ? robustness2Name : "no robustness2 extension");

        deviceCreateInfo.enabledExtensionCount = static_cast<Uint32>(enabledDeviceExtensions.size());
        deviceCreateInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
        MGLOG_I("Device feature support: robustBufferAccess=%s geometryShader=%s independentBlend=%s logicOp=%s shaderClipDistance=%s "
                "shaderCullDistance=%s wideLines=%s shaderInt64=%s vertexStoresAtomics=%s "
                "fragmentStoresAtomics=%s storageImageExtendedFormats=%s storageImageReadWithoutFormat=%s "
                "storageImageWriteWithoutFormat=%s drawIndirectFirstInstance=%s "
                "multiDrawIndirect=%s",
            supportedDeviceFeatures.robustBufferAccess ? "true" : "false",
            supportedDeviceFeatures.geometryShader ? "true" : "false",
            supportedDeviceFeatures.independentBlend ? "true" : "false",
            supportedDeviceFeatures.logicOp ? "true" : "false",
            supportedDeviceFeatures.shaderClipDistance ? "true" : "false",
            supportedDeviceFeatures.shaderCullDistance ? "true" : "false",
            supportedDeviceFeatures.wideLines ? "true" : "false",
            supportedDeviceFeatures.shaderInt64 ? "true" : "false",
            supportedDeviceFeatures.vertexPipelineStoresAndAtomics ? "true" : "false",
            supportedDeviceFeatures.fragmentStoresAndAtomics ? "true" : "false",
            supportedDeviceFeatures.shaderStorageImageExtendedFormats ? "true" : "false",
            supportedDeviceFeatures.shaderStorageImageReadWithoutFormat ? "true" : "false",
            supportedDeviceFeatures.shaderStorageImageWriteWithoutFormat ? "true" : "false",
            supportedDeviceFeatures.drawIndirectFirstInstance ? "true" : "false",
            supportedDeviceFeatures.multiDrawIndirect ? "true" : "false");
        MGLOG_I("Device feature enabled: robustBufferAccess=%s geometryShader=%s independentBlend=%s logicOp=%s shaderClipDistance=%s "
                "shaderCullDistance=%s wideLines=%s shaderInt64=%s vertexStoresAtomics=%s "
                "fragmentStoresAtomics=%s storageImageExtendedFormats=%s storageImageReadWithoutFormat=%s "
                "storageImageWriteWithoutFormat=%s drawIndirectFirstInstance=%s "
                "multiDrawIndirect=%s shaderDrawParameters=%s",
            deviceFeatures.robustBufferAccess ? "true" : "false",
            deviceFeatures.geometryShader ? "true" : "false",
            deviceFeatures.independentBlend ? "true" : "false",
            deviceFeatures.logicOp ? "true" : "false",
            deviceFeatures.shaderClipDistance ? "true" : "false",
            deviceFeatures.shaderCullDistance ? "true" : "false",
            deviceFeatures.wideLines ? "true" : "false",
            deviceFeatures.shaderInt64 ? "true" : "false",
            deviceFeatures.vertexPipelineStoresAndAtomics ? "true" : "false",
            deviceFeatures.fragmentStoresAndAtomics ? "true" : "false",
            deviceFeatures.shaderStorageImageExtendedFormats ? "true" : "false",
            deviceFeatures.shaderStorageImageReadWithoutFormat ? "true" : "false",
            deviceFeatures.shaderStorageImageWriteWithoutFormat ? "true" : "false",
            deviceFeatures.drawIndirectFirstInstance ? "true" : "false",
            deviceFeatures.multiDrawIndirect ? "true" : "false",
            m_shaderDrawParametersFeatureEnabled ? "true" : "false");
        VK_VERIFY(vkCreateDevice(m_physicalDevice.handle, &deviceCreateInfo, nullptr, &m_device), "vkCreateDevice");

        if (wireDepthResolveEnabled) {
            m_wireCreateRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(
                vkGetDeviceProcAddr(m_device, wireDepthResolveCore ? "vkCreateRenderPass2" : "vkCreateRenderPass2KHR"));
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        if (m_wireAhbImport) {
            m_wireGetAhbProperties =
                reinterpret_cast<void*>(vkGetDeviceProcAddr(m_device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
            if (m_wireGetAhbProperties == nullptr) m_wireAhbImport = false;
        }
        if (m_sharedImageSyncFd) {
            m_importSemaphoreFd =
                reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(m_device, "vkImportSemaphoreFdKHR"));
            m_getSemaphoreFd =
                reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(m_device, "vkGetSemaphoreFdKHR"));
            if (m_importSemaphoreFd == nullptr || m_getSemaphoreFd == nullptr) {
                MGLOG_W("DirectVulkan: VK_KHR_external_semaphore_fd is missing an entry point; shared-image fences "
                        "are waited for on the CPU");
                m_sharedImageSyncFd = false;
            }
        }
#endif

        s_vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFNDrawIndexedIndirectCountFunc>(
            vkGetDeviceProcAddr(m_device, "vkCmdDrawIndexedIndirectCountKHR"));
        if (s_vkCmdDrawIndexedIndirectCount == nullptr) {
            s_vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFNDrawIndexedIndirectCountFunc>(
                vkGetDeviceProcAddr(m_device, "vkCmdDrawIndexedIndirectCount"));
        }
        if (m_drawIndirectCountExtensionEnabled && s_vkCmdDrawIndexedIndirectCount == nullptr) {
            MGLOG_W("VK_KHR_draw_indirect_count enabled but vkCmdDrawIndexedIndirectCount entry point is missing, will continue as if VK_KHR_draw_indirect_count is not supported!");
            m_drawIndirectCountExtensionEnabled = false;
        }
        // P8-D: the wire arm's glMultiDrawArraysIndirectCount. Null = that verb reads its count on
        // the CPU (WireDraw.inc's DrawWireIndirectNative answers false for it).
        s_vkCmdWireDrawIndirectCount = reinterpret_cast<PFNDrawIndexedIndirectCountFunc>(
            vkGetDeviceProcAddr(m_device, "vkCmdDrawIndirectCountKHR"));
        if (s_vkCmdWireDrawIndirectCount == nullptr) {
            s_vkCmdWireDrawIndirectCount = reinterpret_cast<PFNDrawIndexedIndirectCountFunc>(
                vkGetDeviceProcAddr(m_device, "vkCmdDrawIndirectCount"));
        }

        s_vkCmdDrawMultiEXT = nullptr;
        s_vkCmdDrawMultiIndexedEXT = nullptr;
        if (m_multiDrawExtensionEnabled) {
            s_vkCmdDrawMultiEXT =
                reinterpret_cast<PFN_vkCmdDrawMultiEXT>(vkGetDeviceProcAddr(m_device, "vkCmdDrawMultiEXT"));
            s_vkCmdDrawMultiIndexedEXT = reinterpret_cast<PFN_vkCmdDrawMultiIndexedEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdDrawMultiIndexedEXT"));
            if (s_vkCmdDrawMultiEXT == nullptr || s_vkCmdDrawMultiIndexedEXT == nullptr) {
                MGLOG_W("VK_EXT_multi_draw enabled but its entry points are missing, will continue as if "
                        "VK_EXT_multi_draw is not supported!");
                s_vkCmdDrawMultiEXT = nullptr;
                s_vkCmdDrawMultiIndexedEXT = nullptr;
                m_multiDrawExtensionEnabled = false;
            }
        }

        // Resolve the multi-draw dispatch tiers once: device support clamped by the
        // MOBILEGL_MAGMA_MULTIDRAW_MODE preference. Requesting an unavailable tier is
        // never an error - the dispatch falls down the chain ext -> indirect -> unroll.
        {
            using MG_Config::MultiDrawMode;
            const MultiDrawMode mode = MG_Config::Features.MagmaMultiDrawMode;
            m_multiDrawAllowExt =
                m_multiDrawExtensionEnabled && (mode == MultiDrawMode::Auto || mode == MultiDrawMode::Ext);
            m_multiDrawAllowIndirect = m_multiDrawIndirectFeatureEnabled && mode != MultiDrawMode::Unroll;
            m_multiDrawForceUnrollIndirect = mode == MultiDrawMode::Unroll;
            if (mode == MultiDrawMode::Ext && !m_multiDrawExtensionEnabled) {
                MGLOG_I("MOBILEGL_MAGMA_MULTIDRAW_MODE=ext requested but VK_EXT_multi_draw is unavailable; "
                        "falling back to the %s tier",
                        m_multiDrawAllowIndirect ? "indirect" : "unroll");
            }
            if (mode == MultiDrawMode::Indirect && !m_multiDrawIndirectFeatureEnabled) {
                MGLOG_I("MOBILEGL_MAGMA_MULTIDRAW_MODE=indirect requested but the multiDrawIndirect device "
                        "feature is unavailable; falling back to the unroll tier");
            }
            MGLOG_I("Multi-draw dispatch tier: %s (VK_EXT_multi_draw=%s, multiDrawIndirect=%s, mode=%s)",
                    m_multiDrawAllowExt ? "ext" : (m_multiDrawAllowIndirect ? "indirect" : "unroll"),
                    m_multiDrawExtensionEnabled ? "true" : "false",
                    m_multiDrawIndirectFeatureEnabled ? "true" : "false",
                    mode == MultiDrawMode::Auto       ? "auto"
                    : mode == MultiDrawMode::Ext      ? "ext"
                    : mode == MultiDrawMode::Indirect ? "indirect"
                                                      : "unroll");
        }

        if (m_transformFeedbackFeatureEnabled) {
            s_vkCmdBindTransformFeedbackBuffersEXT = reinterpret_cast<PFN_vkCmdBindTransformFeedbackBuffersEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdBindTransformFeedbackBuffersEXT"));
            s_vkCmdBeginTransformFeedbackEXT = reinterpret_cast<PFN_vkCmdBeginTransformFeedbackEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdBeginTransformFeedbackEXT"));
            s_vkCmdEndTransformFeedbackEXT = reinterpret_cast<PFN_vkCmdEndTransformFeedbackEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdEndTransformFeedbackEXT"));
            if (s_vkCmdBindTransformFeedbackBuffersEXT == nullptr || s_vkCmdBeginTransformFeedbackEXT == nullptr ||
                s_vkCmdEndTransformFeedbackEXT == nullptr) {
                MGLOG_W("VK_EXT_transform_feedback entry points missing; transform feedback capture disabled");
                m_transformFeedbackFeatureEnabled = false;
            }
        }
        if (m_hostQueryResetEnabled) {
            s_vkResetQueryPool =
                reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(m_device, "vkResetQueryPool"));
            if (s_vkResetQueryPool == nullptr) {
                s_vkResetQueryPool =
                    reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(m_device, "vkResetQueryPoolEXT"));
            }
            if (s_vkResetQueryPool == nullptr) {
                m_hostQueryResetEnabled = false;
            }
        }
        if (m_transformFeedbackFeatureEnabled) {
            s_vkCmdBeginQueryIndexedEXT = reinterpret_cast<PFN_vkCmdBeginQueryIndexedEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdBeginQueryIndexedEXT"));
            s_vkCmdEndQueryIndexedEXT = reinterpret_cast<PFN_vkCmdEndQueryIndexedEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdEndQueryIndexedEXT"));
            VkPhysicalDeviceTransformFeedbackPropertiesEXT xfbProperties{};
            xfbProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &xfbProperties;
            // Resolved via proc addr: vkGetPhysicalDeviceProperties2 is Vulkan 1.1, and Android's
            // libvulkan.so only exports it from API 28 while minSdk is 26.
            auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
            if (getPhysicalDeviceProperties2 == nullptr) {
                getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
            }
            if (getPhysicalDeviceProperties2 != nullptr) {
                getPhysicalDeviceProperties2(m_physicalDevice.handle, &properties2);
            }
            m_xfbQueriesSupported = xfbProperties.transformFeedbackQueries == VK_TRUE &&
                s_vkCmdBeginQueryIndexedEXT != nullptr && s_vkCmdEndQueryIndexedEXT != nullptr;
        }
        MGLOG_I("index type uint8 enabled: %s", m_indexTypeUint8ExtensionEnabled ? "true" : "false");
        MGLOG_I("Logical device created.");

        // Queues
        vkGetDeviceQueue(m_device, m_physicalDevice.queueFamilies.graphicsFamily, 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_physicalDevice.queueFamilies.presentFamily, 0, &m_presentQueue);
        MGLOG_I("Queues got successfully.");

        // Timestamp (timer query) support: re-enumerate the graphics queue
        // family's properties for its timestampValidBits (0 means the queue
        // cannot write timestamps) and take timestampPeriod (ns per tick) from
        // the device limits.
        const auto timestampQueueFamilies = GetQueueFamilyFromPhysicalDevice(m_physicalDevice.handle);
        m_timestampValidBits = 0;
        const Int32 graphicsFamilyIndex = m_physicalDevice.queueFamilies.graphicsFamily;
        if (graphicsFamilyIndex >= 0 && static_cast<SizeT>(graphicsFamilyIndex) < timestampQueueFamilies.size()) {
            m_timestampValidBits = timestampQueueFamilies[graphicsFamilyIndex].timestampValidBits;
        }
        m_timestampPeriodNs = m_physicalDevice.properties.limits.timestampPeriod;
        m_timerQuerySupported = m_timestampValidBits > 0 && m_timestampPeriodNs > 0.0f;
        MGLOG_I("Timer queries %s (timestampValidBits=%u, timestampPeriod=%f ns/tick)",
                m_timerQuerySupported ? "supported" : "not supported", m_timestampValidBits, m_timestampPeriodNs);

        // Last, because it records on m_graphicsQueue: decide the PRIMITIVES_GENERATED
        // reroute for XFB-inactive draws. Nothing else has touched the queue yet.
        ArmPrimGenReroute();
        // Same terms: it records on m_graphicsQueue, which nothing but the probe above has used.
        ArmWireDepthResolveOrder();
    }

    // P7 gate 5 (g5-msprobe): THE ARM ORDER OF THE MULTISAMPLE DEPTH/STENCIL RESOLVE IS A MEASURED
    // PROPERTY OF THIS DEVICE. WireDepthResolveArm.h says why (the Adreno 830's no-draw
    // VK_KHR_depth_stencil_resolve pass writes nothing), WireDepthResolveProbe.h what is run. The
    // answer is a device property, so it is memoized per DEVICE IDENTITY (WireDepthResolveArmCache:
    // vendor, device, driver version, pipeline-cache UUID - codex closeout finding 7), and logged once
    // per identity with every reading - the line the device evidence quotes.
    //
    // THE MEASUREMENT CAN BE REPLACED, AND ONLY HERE: MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=bug|clean
    // hands ChooseWireDepthResolveArm a canned measurement instead of running the probe - no host
    // lane runs on a device with the defect - and the DirectVulkan.{Split,Spawn}.MsResolveBug./
    // MsFlipBug. entries force `bug` and read ResolveWireDepthStencil's arm line back from the
    // server log. The canned measurement is evaluated like a real one, so those entries go red
    // when the evaluation stops detecting the defect. MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=elide-subject
    // runs the REAL probe with its render-pass resolve left unrecorded (the sentinel survives, the
    // shader control still resolves), so the DirectVulkan.{Split,Spawn}.MsResolveElide. entries go
    // red when the real recording, readback or tally stops detecting it too.
    void VulkanRenderer::ArmWireDepthResolveOrder() {
        m_wirePreferShaderDepthResolve = false;
        // ResolveWireDepthStencil's own test for the render-pass arm, less its per-format half
        // (the probe asks that per format): without it the shader pass is the only arm.
        const Bool renderPassArmAvailable = !MagmaWireForcedShaderDepthResolve() && m_wireCreateRenderPass2 &&
            (m_wireDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0;
        const char* knobValue = std::getenv("MGITEST_MAGMA_DEPTH_RESOLVE_PROBE");
        const WireDepthResolveProbeKnob knob = ParseWireDepthResolveProbeKnob(knobValue);
        // Memoized per DEVICE IDENTITY, not per process (codex closeout finding 7; see
        // WireDepthResolveArmCache): a second renderer on another device or driver probes for itself.
        const WireDepthResolveDeviceIdentity identity =
            MakeWireDepthResolveDeviceIdentity(m_physicalDevice.properties, renderPassArmAvailable, knob);
        static WireDepthResolveArmCache s_choices;
        const WireDepthResolveArmChoice resolved = s_choices.Resolve(identity, [&]() {
            if (knob == WireDepthResolveProbeKnob::Unrecognised)
                MGLOG_W("DirectVulkan: MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=%s is not `bug`, `clean` or "
                        "`elide-subject`; the resolve probe runs as if it were unset", knobValue);
            WireDepthResolveArmChoice choice = ChooseWireDepthResolveArm(knob, renderPassArmAvailable, [&]() {
                WireDepthResolveProbeContext context;
                context.physicalDevice = m_physicalDevice.handle;
                context.device = m_device;
                context.queue = m_graphicsQueue;
                context.queueFamilyIndex = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
                context.createRenderPass2 = m_wireCreateRenderPass2;
                context.depthResolveModes = m_wireDepthResolveModes;
                context.stencilResolveModes = m_wireStencilResolveModes;
                context.shaderStencilExport = m_wireShaderStencilExport;
                context.elideSubject = knob == WireDepthResolveProbeKnob::ElideSubject;
                return RunWireDepthResolveProbe(context);
            });
            if (choice.measurement.fenceWaitTimedOut) {
                // Its objects are leaked on purpose (see the PRIMITIVES_GENERATED probe above for
                // why this device must not be idle-waited on their account).
                MGLOG_W("DirectVulkan: the depth/stencil resolve probe timed out waiting on its own "
                        "submission; its Vulkan objects are deliberately leaked and the render-pass arm "
                        "keeps its place");
            } else if (choice.verdict == WireDepthResolveProbeVerdict::Inconclusive) {
                MGLOG_W("DirectVulkan: depth/stencil resolve probe verdict=inconclusive - the shader "
                        "control did not resolve the probe's inputs either, so nothing is concluded about "
                        "the render pass and the default arm order stands");
            }
            MGLOG_I("DirectVulkan: depth/stencil resolve probe (%s) verdict=%s: %s%s%s",
                    choice.measurement.fromKnob ? "forced by MGITEST_MAGMA_DEPTH_RESOLVE_PROBE"
                    : choice.measurement.subjectElided
                        ? "measured on this device with its render-pass resolve elided by "
                          "MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=elide-subject"
                    : renderPassArmAvailable ? "measured on this device"
                                             : "not run: no render-pass arm",
                    WireDepthResolveProbeVerdictName(choice.verdict),
                    choice.preferShader ? "the shader pass resolves first, the no-draw render pass is the fallback"
                    : (renderPassArmAvailable || choice.measurement.fromKnob)
                        ? "the VK_KHR_depth_stencil_resolve render pass resolves first"
                        : "the shader pass is the only arm",
                    choice.measurement.failureReason.empty() ? "" : "; ",
                    choice.measurement.failureReason.c_str());
            for (const WireDepthResolveFormatReading& reading : choice.measurement.formats)
                MGLOG_I("DirectVulkan: depth/stencil resolve probe %s",
                        DescribeWireDepthResolveFormat(reading).c_str());
            return choice;
        });
        m_wirePreferShaderDepthResolve = resolved.preferShader;
    }

    void VulkanRenderer::ArmPrimGenReroute() {
        using namespace MG_Util::SelfTest;
        m_primGenRerouteKind = PrimGenRerouteKind::None;
        const MG_Config::QuirkOverride overrideSetting = MG_Config::Features.MagmaPrimGenQueryReroute;
        // Without stream queries the GENERATED path never opens a slot at all, so
        // there is nothing to reroute - whatever the override says.
        if (!m_xfbQueriesSupported || !m_hostQueryResetEnabled) {
            return;
        }
        const Bool primitivesGeneratedQueryUsable =
            m_primitivesGeneratedQueryFeatureEnabled && m_primitivesGeneratedQueryDiscardFeatureEnabled;
        PrimitivesGeneratedNoXfbVerdict verdict = PrimitivesGeneratedNoXfbVerdict::Inconclusive;
        // The probe only matters under Auto (ForceOn bypasses the verdict, ForceOff
        // never asks), and the answer is a device property - so it is memoized per
        // process rather than re-paid on every renderer recreation.
        if (overrideSetting == MG_Config::QuirkOverride::Auto) {
            static const PrimitivesGeneratedNoXfbMeasurement s_measurement = [&]() {
                PrimitivesGeneratedNoXfbProbeContext probeContext;
                probeContext.device = m_device;
                probeContext.queue = m_graphicsQueue;
                probeContext.queueFamilyIndex =
                    static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
                probeContext.transformFeedbackQueriesUsable = m_xfbQueriesSupported;
                probeContext.primitivesGeneratedQueryUsable = primitivesGeneratedQueryUsable;
                probeContext.pipelineStatisticsEnabled = m_pipelineStatisticsQueryFeatureEnabled;
                probeContext.tessellationEnabled = m_tessellationShaderFeatureEnabled;
                auto& fns = probeContext.fns;
                fns.vkCreateCommandPool = vkCreateCommandPool;
                fns.vkDestroyCommandPool = vkDestroyCommandPool;
                fns.vkAllocateCommandBuffers = vkAllocateCommandBuffers;
                fns.vkBeginCommandBuffer = vkBeginCommandBuffer;
                fns.vkEndCommandBuffer = vkEndCommandBuffer;
                fns.vkCreateQueryPool = vkCreateQueryPool;
                fns.vkDestroyQueryPool = vkDestroyQueryPool;
                fns.vkCmdResetQueryPool = vkCmdResetQueryPool;
                fns.vkCmdBeginQuery = vkCmdBeginQuery;
                fns.vkCmdEndQuery = vkCmdEndQuery;
                fns.vkCmdBeginQueryIndexedEXT = s_vkCmdBeginQueryIndexedEXT;
                fns.vkCmdEndQueryIndexedEXT = s_vkCmdEndQueryIndexedEXT;
                fns.vkCreateRenderPass = vkCreateRenderPass;
                fns.vkDestroyRenderPass = vkDestroyRenderPass;
                fns.vkCreateFramebuffer = vkCreateFramebuffer;
                fns.vkDestroyFramebuffer = vkDestroyFramebuffer;
                fns.vkCmdBeginRenderPass = vkCmdBeginRenderPass;
                fns.vkCmdEndRenderPass = vkCmdEndRenderPass;
                fns.vkCreateShaderModule = vkCreateShaderModule;
                fns.vkDestroyShaderModule = vkDestroyShaderModule;
                fns.vkCreatePipelineLayout = vkCreatePipelineLayout;
                fns.vkDestroyPipelineLayout = vkDestroyPipelineLayout;
                fns.vkCreateGraphicsPipelines = vkCreateGraphicsPipelines;
                fns.vkDestroyPipeline = vkDestroyPipeline;
                fns.vkCmdBindPipeline = vkCmdBindPipeline;
                fns.vkCmdDraw = vkCmdDraw;
                fns.vkCreateFence = vkCreateFence;
                fns.vkDestroyFence = vkDestroyFence;
                fns.vkQueueSubmit = vkQueueSubmit;
                fns.vkWaitForFences = vkWaitForFences;
                fns.vkGetQueryPoolResults = vkGetQueryPoolResults;
                fns.vkDeviceWaitIdle = vkDeviceWaitIdle;
                return RunPrimitivesGeneratedNoXfbProbe(probeContext);
            }();
            verdict = EvaluatePrimitivesGeneratedNoXfbVerdict(s_measurement);
            if (s_measurement.fenceWaitTimedOut) {
                // The probe's submission never signaled within its bound, so it left its
                // command pool, query pools, render pass, framebuffer, shader modules,
                // pipeline layout, pipelines and fence alive on purpose. This device is the
                // renderer's own and outlives them, so nothing here may destroy them or
                // wait the device idle - the queue may still be executing that submission,
                // and an idle wait is the hang the bound exists to prevent. They leak for
                // the process's life; a device this sick has bigger problems.
                MGLOG_W("PRIMITIVES_GENERATED probe timed out waiting on its own submission (%s); its "
                        "Vulkan objects are deliberately leaked and XFB-inactive draws keep the stream "
                        "query", s_measurement.failureReason.c_str());
            } else if (!s_measurement.ran) {
                MGLOG_W("PRIMITIVES_GENERATED probe did not run (%s); XFB-inactive draws keep the "
                        "stream query", s_measurement.failureReason.c_str());
            } else {
                const auto logShape = [](const char* name,
                                         const MG_Util::SelfTest::PrimitivesGeneratedNoXfbShapeMeasurement&
                                             shape) {
                    MGLOG_I("PRIMITIVES_GENERATED probe %s: drawn=%d stream=%llu/%llu pgq=%llu(%d) "
                            "stat=%llu(%d)",
                            name, shape.drawn ? 1 : 0,
                            static_cast<unsigned long long>(shape.streamGenerated),
                            static_cast<unsigned long long>(shape.expectedPrimitives),
                            static_cast<unsigned long long>(shape.primitivesGeneratedExt),
                            shape.primitivesGeneratedExtMeasured ? 1 : 0,
                            static_cast<unsigned long long>(shape.statisticsClippingInput),
                            shape.statisticsMeasured ? 1 : 0);
                };
                logShape("triangles", s_measurement.trianglesPlain);
                logShape("triangles+discard", s_measurement.trianglesDiscard);
                logShape("patches+discard", s_measurement.patchesDiscard);
            }
        }
        // A driver whose stream query counts capture-less draws counts a PAUSED span's
        // draws through the stream slot they take, so that span's result must not have
        // the frontend's CPU paused counter added on top of it either (the pre-reroute
        // accounting did exactly that, double counting every paused draw the CPU could
        // price). Measured, not assumed: the forced arms never ask the probe and leave
        // this false.
        m_primGenStreamCountsXfbInactiveDraws = verdict == PrimitivesGeneratedNoXfbVerdict::StreamCounts;
        m_primGenRerouteKind = ChoosePrimitivesGeneratedReroute(
            overrideSetting, verdict, primitivesGeneratedQueryUsable, m_pipelineStatisticsQueryFeatureEnabled);
        if (m_primGenRerouteKind != PrimGenRerouteKind::None) {
            MGLOG_I("PRIMITIVES_GENERATED for XFB-inactive draws will accumulate through a %s pool%s",
                    m_primGenRerouteKind == PrimGenRerouteKind::PrimitivesGeneratedExt
                        ? "VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT"
                        : "clipping-invocations pipeline-statistics",
                    overrideSetting == MG_Config::QuirkOverride::ForceOn ? " (forced on)" : "");
        }
    }

    void VulkanRenderer::CreateAllocator() {
        MOBILEGL_ASSERT(m_instance != VK_NULL_HANDLE, "CreateAllocator requires valid VkInstance");
        MOBILEGL_ASSERT(m_physicalDevice.handle != VK_NULL_HANDLE, "CreateAllocator requires valid physical device");
        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE, "CreateAllocator requires valid VkDevice");

        if (m_allocator != nullptr) {
            return;
        }

        VmaAllocatorCreateInfo allocatorInfo{};
        VmaVulkanFunctions vulkanFunctions{};
        vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        allocatorInfo.instance = m_instance;
        allocatorInfo.physicalDevice = m_physicalDevice.handle;
        allocatorInfo.device = m_device;
        allocatorInfo.pVulkanFunctions = &vulkanFunctions;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_0;

        VK_VERIFY(vmaCreateAllocator(&allocatorInfo, &m_allocator), "vmaCreateAllocator");
    }

    void VulkanRenderer::DestroyAllocator() {
        if (m_allocator != nullptr) {
            vmaDestroyAllocator(m_allocator);
            m_allocator = nullptr;
        }
    }

    void VulkanRenderer::CreateSwapchain() {
        const VkExtent2D desiredExtent = {
            std::max<Uint32>(m_config.SurfaceWidth, 1),
            std::max<Uint32>(m_config.SurfaceHeight, 1),
        };
        m_swapchainObject.Create(m_device, m_physicalDevice.handle, m_surface,
                                 static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily),
                                 static_cast<Uint32>(m_physicalDevice.queueFamilies.presentFamily),
                                 m_config.MaxFramesInFlight, desiredExtent, m_config.SwapInterval);
        if (m_config.SwapInterval && m_config.SwapInterval != m_swapchainSwapInterval) {
            MGLOG_D("DirectVulkan: swap interval %d -> %s", *m_config.SwapInterval,
                    SwapchainObject::GetPresentModeName(m_swapchainObject.GetPresentMode()));
        }
        m_swapchainSwapInterval = m_config.SwapInterval;
        // The FragCoordYFlip variants bake this height in; it is the only input to a shader
        // module that lives outside the GL program, so the factory has to learn it here (and on
        // every recreation, which is the only way it can change).
        if (m_programFactory) {
            m_programFactory->SetDefaultFramebufferHeight(m_swapchainObject.GetExtent().height);
        }
    }

    void VulkanRenderer::CreateCommandPool() {
        VkCommandPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        createInfo.queueFamilyIndex = m_physicalDevice.queueFamilies.graphicsFamily;
        VK_VERIFY(vkCreateCommandPool(m_device, &createInfo, nullptr, &m_commandPool));
        MGLOG_I("Command pool created");
    }

    Bool VulkanRenderer::CreateSurface() {
        if (!m_window) {
#if defined VK_USE_PLATFORM_METAL_EXT
            m_window = reinterpret_cast<NativeWindowType>(
                CreateInternalMetalLayer(m_config.SurfaceWidth, m_config.SurfaceHeight, &m_platformDisplay));
            m_platformLibrary = reinterpret_cast<void*>(m_window);
#elif defined VK_USE_PLATFORM_ANDROID_KHR
            // EVERY ENTRY POINT HERE IS OPTIONAL, and each refusal is a `return false`, not
            // MOBILEGL_ASSERT: that macro is compiled out at the INFO level every shipping build uses,
            // and a renderer created for a WINDOW never enabled the headless extension - asking its
            // instance for vkCreateHeadlessSurfaceEXT answers null, and calling that crashed the server
            // the first time such a renderer needed an offscreen target.
            auto* createHeadlessSurface =
                m_headlessSurfaceSupported ? reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                                                 vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"))
                                           : nullptr;
            const OffscreenSurfaceRoute route = ChooseOffscreenSurfaceRoute(
                m_headlessSurfaceSupported, createHeadlessSurface != nullptr, m_androidSurfaceEnabled);
            if (route == OffscreenSurfaceRoute::Headless) {
                VkHeadlessSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
                const VkResult result = createHeadlessSurface(m_instance, &sci, nullptr, &m_surface);
                if (result != VK_SUCCESS) {
                    MGLOG_E("DirectVulkan: vkCreateHeadlessSurfaceEXT failed (%d) for an offscreen target",
                            static_cast<int>(result));
                    m_surface = VK_NULL_HANDLE;
                    return false;
                }
                return true;
            }
            if (route == OffscreenSurfaceRoute::Unavailable) {
                MGLOG_E("DirectVulkan: no offscreen surface route on this instance (neither %s nor %s enabled)",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
                return false;
            }
            // Windowless context on a driver without VK_EXT_headless_surface: give
            // the WSI an AImageReader's ANativeWindow. It is a real, valid producer
            // surface that is attached to no display and whose images this code never
            // acquires, which is exactly the "drawable nobody sees" the Xlib fallback
            // below builds out of an unmapped window. libmediandk is dlopen'd rather
            // than linked so a device without it degrades to a refusal instead of
            // failing to load the library at all.
            {
                void* mediaLib = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
                if (mediaLib == nullptr) {
                    MGLOG_E("DirectVulkan: libmediandk.so could not be loaded for an offscreen target");
                    return false;
                }
                using AImageReaderNewFn = int (*)(int32_t, int32_t, int32_t, int32_t, void**);
                using AImageReaderGetWindowFn = int (*)(void*, void**);
                using AImageReaderDeleteFn = void (*)(void*);
                auto* imageReaderNew = reinterpret_cast<AImageReaderNewFn>(dlsym(mediaLib, "AImageReader_new"));
                auto* imageReaderGetWindow =
                    reinterpret_cast<AImageReaderGetWindowFn>(dlsym(mediaLib, "AImageReader_getWindow"));
                auto* imageReaderDelete =
                    reinterpret_cast<AImageReaderDeleteFn>(dlsym(mediaLib, "AImageReader_delete"));
                if (imageReaderNew == nullptr || imageReaderGetWindow == nullptr || imageReaderDelete == nullptr) {
                    MGLOG_E("DirectVulkan: libmediandk.so is missing AImageReader_new/getWindow/delete");
                    dlclose(mediaLib);
                    return false;
                }

                constexpr int32_t kAndroidFormatRgba8888 = 0x1; // AIMAGE_FORMAT_RGBA_8888
                const int32_t width = static_cast<int32_t>(std::max<Uint32>(m_config.SurfaceWidth, 1));
                const int32_t height = static_cast<int32_t>(std::max<Uint32>(m_config.SurfaceHeight, 1));
                void* reader = nullptr;
                // maxImages must cover the swapchain's images; the reader never
                // acquires any, so this only sizes its buffer queue.
                const int status = imageReaderNew(width, height, kAndroidFormatRgba8888, 8, &reader);
                void* nativeWindow = nullptr;
                const int windowStatus =
                    (status == 0 && reader != nullptr) ? imageReaderGetWindow(reader, &nativeWindow) : -1;
                if (windowStatus != 0 || nativeWindow == nullptr) {
                    MGLOG_E("DirectVulkan: no AImageReader window for an offscreen target (new %d, getWindow %d)",
                            status, windowStatus);
                    if (reader != nullptr) imageReaderDelete(reader);
                    dlclose(mediaLib);
                    return false;
                }
                m_fallbackImageReader = reader;
                m_platformLibrary = mediaLib;
                m_window = reinterpret_cast<NativeWindowType>(nativeWindow);
            }
#elif defined VK_USE_PLATFORM_XLIB_KHR
            // No fall-through to Xlib: an offscreen surface never touches a window
            // system. CreateInstance() has already refused the bring-up if the loader
            // lacks the extension, so reaching here without it is a broken invariant
            // rather than a platform limitation - report it and fail, do not continue.
            auto* createHeadlessSurface =
                reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                    vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"));
            if (!m_headlessSurfaceSupported || createHeadlessSurface == nullptr) {
                MGLOG_F("vkCreateHeadlessSurfaceEXT is unavailable (%s reported as %s) while creating an "
                        "offscreen DirectVulkan surface",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
                        m_headlessSurfaceSupported ? "supported" : "unsupported");
                throw RuntimeError("vkCreateHeadlessSurfaceEXT is unavailable for an offscreen DirectVulkan surface");
            }
            VkHeadlessSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
            VK_VERIFY(createHeadlessSurface(m_instance, &sci, nullptr, &m_surface),
                      "vkCreateHeadlessSurfaceEXT failed");
            return true;
#else
            auto* createHeadlessSurface =
                reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                    vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"));
            if (createHeadlessSurface == nullptr) {
                // Same class as the Xlib branch above: a null entry point behind
                // MOBILEGL_ASSERT is a segv on the next line in every INFO-level build.
                MGLOG_F("vkCreateHeadlessSurfaceEXT is unavailable while creating an offscreen DirectVulkan "
                        "surface (%s missing from this loader)",
                        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
                throw RuntimeError("vkCreateHeadlessSurfaceEXT is unavailable for an offscreen DirectVulkan surface");
            }
            VkHeadlessSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
            VK_VERIFY(createHeadlessSurface(m_instance, &sci, nullptr, &m_surface),
                      "vkCreateHeadlessSurfaceEXT failed");
            return true;
#endif
        }
#if defined VK_USE_PLATFORM_ANDROID_KHR
        auto* nativeWindow = static_cast<ANativeWindow*>(m_window);
        if (!nativeWindow) throw RuntimeError("ANativeWindowType is null");

        VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        sci.window = nativeWindow;
        const VkResult androidSurfaceResult = vkCreateAndroidSurfaceKHR(m_instance, &sci, nullptr, &m_surface);
        if (androidSurfaceResult != VK_SUCCESS) {
            MGLOG_E("DirectVulkan: vkCreateAndroidSurfaceKHR failed (%d)", static_cast<int>(androidSurfaceResult));
            m_surface = VK_NULL_HANDLE;
            return false;
        }
#elif defined VK_USE_PLATFORM_WIN32_KHR
        auto hwnd = static_cast<HWND>(m_window);
        MOBILEGL_ASSERT(hwnd, "HWND is null");

        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = hwnd;
        VK_VERIFY(vkCreateWin32SurfaceKHR(m_instance, &sci, nullptr, &m_surface), "vkCreateWin32SurfaceKHR failed");
#elif defined VK_USE_PLATFORM_METAL_EXT
        MOBILEGL_ASSERT(m_window, "CAMetalLayer is null");

        VkMetalSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
        sci.pLayer = reinterpret_cast<const void*>(m_window);
        VK_VERIFY(vkCreateMetalSurfaceEXT(m_instance, &sci, nullptr, &m_surface), "vkCreateMetalSurfaceEXT failed");
#elif defined VK_USE_PLATFORM_XLIB_KHR
        // Reached only for a REAL on-screen window surface (a windowed desktop app,
        // retrace in window mode). Presentation to a window legitimately needs a
        // window system; offscreen requests returned above and never come here, so
        // there is no longer any path that opens a display on a caller's behalf.
        //
        // Every failure below is a real error return, not MOBILEGL_ASSERT: that macro
        // is compiled out at the INFO log level every shipping and CI build uses, so
        // asserting here meant a null Display sailed straight into the next Xlib call
        // and segfaulted - which is exactly how this presented in CI.
        if (!m_window) {
            MGLOG_F("CreateSurface: a window surface was requested with no native window");
            throw RuntimeError("CreateSurface: no native window for the Vulkan Xlib surface");
        }

        void* x11Lib = dlopen("libX11.so.6", RTLD_LOCAL | RTLD_NOW);
        if (!x11Lib) {
            x11Lib = dlopen("libX11.so", RTLD_LOCAL | RTLD_NOW);
        }
        if (x11Lib == nullptr) {
            MGLOG_F("Failed to open libX11 (.so.6 and .so) while creating a Vulkan Xlib window surface: %s",
                    dlerror());
            throw RuntimeError("libX11 is unavailable for the Vulkan Xlib window surface");
        }
        using XOpenDisplayFn = Display* (*)(const char*);
        using XCloseDisplayFn = int (*)(Display*);
        auto* xOpenDisplay = reinterpret_cast<XOpenDisplayFn>(dlsym(x11Lib, "XOpenDisplay"));
        auto* xCloseDisplay = reinterpret_cast<XCloseDisplayFn>(dlsym(x11Lib, "XCloseDisplay"));
        if (xOpenDisplay == nullptr || xCloseDisplay == nullptr) {
            MGLOG_F("Failed to resolve XOpenDisplay/XCloseDisplay while creating a Vulkan Xlib window surface");
            dlclose(x11Lib);
            throw RuntimeError("libX11 is missing XOpenDisplay/XCloseDisplay");
        }

        const char* displayName = std::getenv("DISPLAY");
        // Never the display this process serves (MG_Util/X11/DisplayGuard.h).
        auto* display = displayName != nullptr && MG_Util::X11::MayDialX11Display(displayName)
                            ? xOpenDisplay(displayName)
                            : nullptr;
        if (display == nullptr) {
            MGLOG_F("XOpenDisplay(%s) failed while creating a Vulkan Xlib window surface; there is no usable X "
                    "display for the requested window surface",
                    displayName != nullptr ? displayName : "<DISPLAY unset>");
            dlclose(x11Lib);
            throw RuntimeError("XOpenDisplay failed for the Vulkan Xlib window surface");
        }
        m_platformDisplay = display;
        m_platformLibrary = x11Lib;
        m_platformCloseDisplay = reinterpret_cast<void*>(xCloseDisplay);

        VkXlibSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
        sci.dpy = display;
        sci.window = static_cast<Window>(m_window);
        VK_VERIFY(vkCreateXlibSurfaceKHR(m_instance, &sci, nullptr, &m_surface), "vkCreateXlibSurfaceKHR failed");
#else
        // #warning "VulkanRenderer::Initialize called on a platform which is not supported yet"
        MGLOG_W("VulkanRenderer::Initialize called on a platform which is not supported yet"); // TODO: support more
                                                                                               // platforms
#endif
        return true;
    }

    Vector<VkQueueFamilyProperties> VulkanRenderer::GetQueueFamilyFromPhysicalDevice(VkPhysicalDevice device) {
        Uint32 queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        Vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
        return queueFamilies;
    }

    Int VulkanRenderer::GetQueueFamilyIndex(const Vector<VkQueueFamilyProperties>& queueFamilies,
                                            VkQueueFlagBits flag) {
        for (Uint32 i = 0; i < queueFamilies.size(); i++) {
            if (queueFamilies[i].queueFlags & flag) {
                return i;
            }
        }
        return -1;
    }

    Int VulkanRenderer::GetPresentQueueFamilyIndex(const PhysicalDevice& physicalDevice, VkSurfaceKHR surface,
                                                   const Vector<VkQueueFamilyProperties>& queueFamilies,
                                                   Int preferredFamilyIndex) {
        if (preferredFamilyIndex != -1) {
            VkBool32 supportsPresent = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice.handle, preferredFamilyIndex, surface,
                                                 &supportsPresent);
            if (supportsPresent) return preferredFamilyIndex;
        }

        for (Uint32 i = 0; i < queueFamilies.size(); i++) {
            VkBool32 supportsPresent = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice.handle, i, surface, &supportsPresent);
            if (supportsPresent) return i;
        }
        return -1;
    }

    Vector<VkExtensionProperties> VulkanRenderer::EnumerateInstanceExtensions() {
        // The two-call idiom has a race the spec explicitly allows for: the loader
        // re-scans ICDs, so the property count can GROW between the sizing call and
        // the fill call, and the fill then returns VK_INCOMPLETE having written only
        // as many entries as the caller asked for. The result is a silently TRUNCATED
        // extension list - and which extensions fall off the end is exactly as stable
        // as the loader's scan order, i.e. not at all. That is how a headless CI
        // runner could decide VK_EXT_headless_surface did not exist on one run and
        // did on the next, sending the pbuffer path into the Xlib fallback with no
        // X server to open. The sibling EnumerateDeviceExtensions below already
        // checked its second call; this one dropped the result on the floor.
        // Loop until a fill call agrees with its own sizing call.
        Vector<VkExtensionProperties> extensions;
        for (Uint32 attempt = 0; attempt < 8; ++attempt) {
            Uint32 extensionCount = 0;
            VK_VERIFY(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
            extensions.resize(extensionCount);
            if (extensionCount == 0) {
                return extensions;
            }
            const VkResult result =
                vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
            if (result == VK_SUCCESS) {
                extensions.resize(extensionCount);
                return extensions;
            }
            if (result != VK_INCOMPLETE) {
                VK_VERIFY(result, "vkEnumerateInstanceExtensionProperties failed");
                return extensions;
            }
            MGLOG_I("vkEnumerateInstanceExtensionProperties returned VK_INCOMPLETE (the loader's list grew "
                    "mid-enumeration); re-enumerating");
        }
        MGLOG_F("vkEnumerateInstanceExtensionProperties never settled; the instance extension list may be "
                "truncated and surface-extension selection is about to be made on incomplete information");
        return extensions;
    }

    Vector<VkExtensionProperties> VulkanRenderer::EnumerateDeviceExtensions(VkPhysicalDevice device) {
        Uint32 extensionCount = 0;
        VK_VERIFY(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr));
        Vector<VkExtensionProperties> extensions(extensionCount);
        VK_VERIFY(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()));
        return extensions;
    }

    Bool VulkanRenderer::IsExtensionSupported(const Vector<VkExtensionProperties>& availableExtensions,
                                              const char* extensionName) {
        for (const auto& extension : availableExtensions) {
            if (strcmp(extension.extensionName, extensionName) == 0) {
                return true;
            }
        }
        return false;
    }

    Bool VulkanRenderer::IsExtensionAlreadyEnabled(const Vector<const char*>& enabledExtensions,
                                                   const char* extensionName) {
        return std::any_of(enabledExtensions.begin(), enabledExtensions.end(),
                           [&extensionName](const String& name) { return name == extensionName; });
    }

    Bool VulkanRenderer::EnableOptionalDeviceExtension(const Vector<VkExtensionProperties>& availableExtensions,
                                                       Vector<const char*>& inOutEnabledExtensions,
                                                       const char* extensionName) {
        if (!IsExtensionSupported(availableExtensions, extensionName)) {
            MGLOG_I("Optional device extension not supported: %s", extensionName);
            return false;
        }

        if (!IsExtensionAlreadyEnabled(inOutEnabledExtensions, extensionName)) {
            inOutEnabledExtensions.push_back(extensionName);
        }
        MGLOG_I("Enabled optional device extension: %s", extensionName);
        return true;
    }

    void VulkanRenderer::ResolveOptionalDeviceExtensions(const Vector<VkExtensionProperties>& availableExtensions,
                                                         Vector<const char*>& inOutEnabledExtensions) {
        m_drawIndirectCountExtensionEnabled = EnableOptionalDeviceExtension(availableExtensions, inOutEnabledExtensions,
                                                                            VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
        m_shaderDrawParametersExtensionEnabled =
            EnableOptionalDeviceExtension(availableExtensions, inOutEnabledExtensions,
                                          VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        EnableOptionalDeviceExtension(availableExtensions, inOutEnabledExtensions,
                                      VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
    }

    Bool VulkanRenderer::CheckValidationLayerSupport() {
        Uint32 layerCount = 0;
        VK_VERIFY(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));

        Vector<VkLayerProperties> layers(layerCount);
        VK_VERIFY(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()));

        for (const char* layerName : s_validationLayerNames) {
            for (const auto& layerProperties : layers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    return true;
                }
            }
        }
        return false;
    }

    void VulkanRenderer::ShutdownSwapchain() {
        MOBILEGL_ASSERT(m_renderPassManager != nullptr, "ShutdownSwapchain: render pass manager is null");
        m_renderPassManager->Shutdown();

        m_swapchainObject.Shutdown(m_device);
    }

    Bool VulkanRenderer::RecreateSwapchain() {
        // No surface target is active (its surface was released): nothing to build on.
        if (m_surface == VK_NULL_HANDLE) return false;
        // Handle cases like minimize on Windows, where swapchain could return a 0x0 extent
        const auto swapchainCapabilities =
            SwapchainObject::GetSwapchainCapabilities(m_physicalDevice.handle, m_surface);
        if (swapchainCapabilities.capabilities.currentExtent.width == 0 ||
            swapchainCapabilities.capabilities.currentExtent.height == 0) {
            return false;
        }

        vkDeviceWaitIdle(m_device);
        OnSubmitsCompletedUpTo(m_submitCounter);
        // The old swapchain will be destroyed below, including recordings that
        // are being abandoned instead of submitted. Release all their views now.
        DestroyWireDrawPass();
        CollectWireObjects(m_submitCounter, true);
        ClearAllWireDrawPassCaches();

        if (m_timerQueryManager) {
            // The in-progress command buffer is abandoned below (its recording
            // flags are force-cleared), so timestamp writes recorded into it
            // will never execute; resolve or invalidate all pending records now
            // to keep later waits from hanging on never-available queries.
            m_timerQueryManager->InvalidatePendingRecords();
        }

        DestroyDeferredDepthMipmapCleanup();
        m_deferredDepthMipmapCleanup.assign(m_frameContext.GetFrameCount(), {});

        const VkExtent2D previousExtent = m_swapchainObject.GetSurfaceExtent();
        ShutdownSwapchain();

        // The new swapchain publishes its extent (SwapchainObject::Create), which is how a client
        // sizing its viewport from eglQuerySurface learns that the window moved.
        CreateSwapchain();
        const VkExtent2D extent = m_swapchainObject.GetSurfaceExtent();
        if (extent.width != previousExtent.width || extent.height != previousExtent.height) {
            MGLOG_I("DirectVulkan: swapchain rebuilt at %ux%u (was %ux%u); the new extent is published", extent.width,
                    extent.height, previousExtent.width, previousExtent.height);
        }
        // Every image of the fresh swapchain holds garbage, so the default framebuffer restarts
        // at index 0: the next write into it re-points the GL-visible index at whatever the next
        // acquire returns (see m_defaultFramebufferImageIndex). Without this the index could name
        // an image of the swapchain just destroyed.
        m_defaultFramebufferImageIndex = 0;
        VK_VERIFY(m_frameContext.InitializeSwapchainSemaphores(m_device,
                                                               static_cast<Uint32>(m_swapchainObject.GetImageCount())),
                  "RecreateSwapchain, InitializeSwapchainSemaphores");
        MOBILEGL_ASSERT(m_renderPassManager != nullptr, "RecreateSwapchain: render pass manager is null");
        Bool ok = m_renderPassManager->Initialize();
        MOBILEGL_ASSERT(ok, "RecreateSwapchain: render pass manager initialization failed");
        if (m_pipelineFactory) {
            m_pipelineFactory->DestroyAll();
        }
        InvalidatePipelineMemo(); // pipelines freed -> the memoized handle would dangle
        g_dynamicStateShadow->graphicsPipelineValid = false;
        DestroyComputePipelines();
        if (m_frameContext.GetFrameCount() > 0) {
            m_frameContext.GetCurrent().isCommandRecording = false;
            m_frameContext.GetCurrent().hasCommandBufferRecorded = false;
            // The pre-pass stream paired with the abandoned recording is
            // dropped with it (its next Begin resets the buffer).
            m_frameContext.GetCurrent().isPreCommandRecording = false;
            m_frameContext.GetCurrent().hasPreCommandBufferRecorded = false;
        }
        const Bool okArena = m_bufferManager.RecreateTransientArenas(m_frameContext.GetFrameCount());
        MOBILEGL_ASSERT(okArena, "RecreateSwapchain: buffer manager transient arena initialization failed");
        if (m_frameContext.GetFrameCount() > 0) {
            if (m_textureManager) {
                m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
            }
            m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }
        return true;
    }

    const PhysicalDevice& VulkanRenderer::GetPhysicalDevice() const {
        return m_physicalDevice;
    }

    Bool VulkanRenderer::SwapchainIsOutOfDate() {
        if (m_surface == VK_NULL_HANDLE || m_swapchainObject.GetHandle() == VK_NULL_HANDLE) {
            return false;
        }
        VkSurfaceCapabilitiesKHR surfaceCaps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice.handle, m_surface, &surfaceCaps) !=
            VK_SUCCESS) {
            return false;
        }
        // A driver-defined currentExtent (UINT32_MAX) means the surface takes its size from the
        // swapchain, so there is nothing to compare against - the app's requested size wins and
        // only an explicit RequestSwapchainResize can change it.
        if (surfaceCaps.currentExtent.width == UINT32_MAX || surfaceCaps.currentExtent.height == UINT32_MAX) {
            return false;
        }
        // Compare in SURFACE space against the extent the live swapchain was created from. Using
        // the swapchain's own (quarter-turn swapped) extent here would report a difference on
        // every rotated frame and rebuild forever.
        const VkExtent2D builtFrom = m_swapchainObject.GetSurfaceExtent();
        const Bool extentChanged = surfaceCaps.currentExtent.width != builtFrom.width ||
                                   surfaceCaps.currentExtent.height != builtFrom.height;
        const Bool transformChanged = surfaceCaps.currentTransform != m_swapchainObject.GetPreTransform();
        if (!extentChanged && !transformChanged) {
            return false;
        }
        MGLOG_D("Swapchain out of date: surface %ux%u transform %u -> %ux%u transform %u",
                builtFrom.width, builtFrom.height, static_cast<Uint32>(m_swapchainObject.GetPreTransform()),
                surfaceCaps.currentExtent.width, surfaceCaps.currentExtent.height,
                static_cast<Uint32>(surfaceCaps.currentTransform));
        return true;
    }

    void VulkanRenderer::RequestSwapchainResize(Uint32 width, Uint32 height) {
        width = std::max<Uint32>(width, 1);
        height = std::max<Uint32>(height, 1);
        if (m_config.SurfaceWidth == width && m_config.SurfaceHeight == height) {
            return;
        }
        m_config.SurfaceWidth = width;
        m_config.SurfaceHeight = height;
        m_swapchainResizeRequested = true;
    }

    void VulkanRenderer::SetSwapInterval(Int interval) {
        if (m_presentsToAppWindow) {
            m_config.SwapInterval = interval;
        }
    }

    void VulkanRenderer::ExchangeActiveTarget(SurfaceTarget& target) {
        std::swap(m_activeTargetSerial, target.serial);
        std::swap(m_window, target.window);
        std::swap(m_platformDisplay, target.platformDisplay);
        std::swap(m_platformLibrary, target.platformLibrary);
        std::swap(m_platformCloseDisplay, target.platformCloseDisplay);
        std::swap(m_fallbackImageReader, target.fallbackImageReader);
        std::swap(m_config.SurfaceWidth, target.surfaceWidth);
        std::swap(m_config.SurfaceHeight, target.surfaceHeight);
        std::swap(m_config.SwapInterval, target.swapInterval);
        std::swap(m_presentsToAppWindow, target.presentsToAppWindow);
        std::swap(m_swapchainSwapInterval, target.swapchainSwapInterval);
        std::swap(m_swapchainResizeRequested, target.swapchainResizeRequested);
        std::swap(m_presentSuspended, target.presentSuspended);
        std::swap(m_surface, target.surface);
        std::swap(m_swapchainObject, target.swapchain);
        m_frameContext.ExchangeSwapchainSemaphores(target.renderFinishedSemaphores);
        std::swap(m_imageIndexAcquired, target.imageIndexAcquired);
        std::swap(m_defaultFramebufferImageIndex, target.defaultFramebufferImageIndex);
    }

    void VulkanRenderer::ParkActiveTarget() {
        if (m_surface == VK_NULL_HANDLE) return;
        // The active surface's recorded work goes to the queue now, while its images are still
        // the default framebuffer's.
        FlushPendingCommands();
        auto& frame = m_frameContext.GetCurrent();
        // Its acquired image keeps its acquire for when the surface comes back, but the signal
        // lives on THIS slot's semaphore, which the next surface acquires with. Wait it out with
        // an empty submission; the image is then simply held, and its next submit waits nothing.
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE && !m_presentSuspended &&
            !frame.imageAvailableSemaphoreConsumed) {
            const VkFence fence = AcquirePooledSubmitFence();
            if (fence == VK_NULL_HANDLE || !SubmitPendingCommandBuffer(frame, fence, /*pooledFence=*/true)) {
                MGLOG_E("DirectVulkan: a parked surface's pending acquire could not be waited out");
            }
        }
        SurfaceTarget parked;
        ExchangeActiveTarget(parked);
        m_parkedTargets[m_activeTargetKey] = std::move(parked);
        m_activeTargetKey = 0;
    }

    void VulkanRenderer::OnActiveTargetChanged() {
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE) {
            m_swapchainObject.PublishDefaultFramebufferInfo();
            if (m_programFactory) m_programFactory->SetDefaultFramebufferHeight(m_swapchainObject.GetExtent().height);
        }
        if (m_renderPassManager) m_renderPassManager->SetDefaultFramebufferTarget(m_activeTargetSerial);
        InvalidatePipelineMemo();
        ResetDynamicStateShadow();
    }

    Bool VulkanRenderer::ActivateSurfaceTarget(Uint64 key, NativeWindowType window,
                                               const VulkanRendererConfig& surfaceConfig) {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) return true;
        ParkActiveTarget();
        auto parked = m_parkedTargets.find(key);
        if (parked != m_parkedTargets.end()) {
            ExchangeActiveTarget(parked->second);
            m_parkedTargets.erase(parked);
            // Its acquire was waited when it was parked (ParkActiveTarget).
            m_frameContext.GetCurrent().imageAvailableSemaphoreConsumed = true;
        } else {
            m_activeTargetSerial = m_nextTargetSerial++;
            m_window = window;
            m_presentsToAppWindow = window != NativeWindowType{};
            m_config.SurfaceWidth = surfaceConfig.SurfaceWidth;
            m_config.SurfaceHeight = surfaceConfig.SurfaceHeight;
            m_config.SwapInterval = m_presentsToAppWindow ? surfaceConfig.SwapInterval : Nullopt;
            m_swapchainSwapInterval.reset();
            m_swapchainResizeRequested = false;
            m_presentSuspended = false;
            if (!CreateSurface()) {
                // Nothing of the half-built target stays active: its image reader and library go
                // with it, and the renderer has no target until the caller activates another.
                SurfaceTarget failed;
                ExchangeActiveTarget(failed);
                DestroyParkedTarget(failed);
                m_activeTargetKey = 0;
                MGLOG_E("DirectVulkan: surface target for key %llu could not be built (%s)",
                        static_cast<unsigned long long>(key), window != NativeWindowType{} ? "window" : "pbuffer");
                return false;
            }
            CreateSwapchain();
            m_defaultFramebufferImageIndex = 0;
            VK_VERIFY(m_frameContext.InitializeSwapchainSemaphores(
                          m_device, static_cast<Uint32>(m_swapchainObject.GetImageCount())),
                      "ActivateSurfaceTarget, InitializeSwapchainSemaphores");
            if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE) {
                VkResult result = m_frameContext.AcquireNextImageInCurrentSlot(
                    m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
                if (result == VK_SUBOPTIMAL_KHR) result = VK_SUCCESS;
                VK_VERIFY(result, "ActivateSurfaceTarget, AcquireNextImageInCurrentSlot");
                m_defaultFramebufferImageIndex = m_imageIndexAcquired;
            } else {
                m_presentSuspended = true;
            }
            MGLOG_I("DirectVulkan: surface target %llu built (%ux%u, %s)",
                    static_cast<unsigned long long>(m_activeTargetSerial), m_swapchainObject.GetExtent().width,
                    m_swapchainObject.GetExtent().height, m_presentsToAppWindow ? "window" : "pbuffer");
        }
        m_activeTargetKey = key;
        OnActiveTargetChanged();
        return true;
    }

    void VulkanRenderer::DestroyParkedTarget(SurfaceTarget& target) {
        target.swapchain.Shutdown(m_device);
        for (VkSemaphore semaphore : target.renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_device, semaphore, nullptr);
        }
        target.renderFinishedSemaphores.clear();
        if (target.surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, target.surface, nullptr);
            target.surface = VK_NULL_HANDLE;
        }
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (target.fallbackImageReader != nullptr && target.platformLibrary != nullptr) {
            using AImageReaderDeleteFn = void (*)(void*);
            auto* imageReaderDelete =
                reinterpret_cast<AImageReaderDeleteFn>(dlsym(target.platformLibrary, "AImageReader_delete"));
            if (imageReaderDelete) imageReaderDelete(target.fallbackImageReader);
            target.fallbackImageReader = nullptr;
            dlclose(target.platformLibrary);
            target.platformLibrary = nullptr;
        }
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
        if (target.platformDisplay != nullptr) {
            using XCloseDisplayFn = int (*)(Display*);
            auto* closeDisplay = reinterpret_cast<XCloseDisplayFn>(target.platformCloseDisplay);
            if (closeDisplay) closeDisplay(static_cast<Display*>(target.platformDisplay));
            target.platformDisplay = nullptr;
        }
        if (target.platformLibrary != nullptr) {
            dlclose(target.platformLibrary);
            target.platformLibrary = nullptr;
        }
#endif
    }

    void VulkanRenderer::DestroySurfaceTarget(Uint64 key) {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) ParkActiveTarget();
        auto parked = m_parkedTargets.find(key);
        if (parked == m_parkedTargets.end()) return;
        // Whatever is recorded may name the target's images (a blit out of it, say): submit it,
        // then let the GPU finish with everything before the views go.
        FlushPendingCommands();
        VK_VERIFY(vkDeviceWaitIdle(m_device));
        OnSubmitsCompletedUpTo(m_submitCounter);
        DestroyWireDrawPass();
        CollectWireObjects(m_submitCounter, true);
        ClearAllWireDrawPassCaches();
        if (m_renderPassManager) m_renderPassManager->PurgeRenderPasses();
        InvalidatePipelineMemo();
        ResetDynamicStateShadow();
        DestroyParkedTarget(parked->second);
        m_parkedTargets.erase(parked);
    }

    Bool VulkanRenderer::FollowActiveWindowResize() {
        if (m_surface == VK_NULL_HANDLE || !m_presentsToAppWindow) return false;
        const auto& frame = m_frameContext.GetCurrent();
        if (frame.isCommandRecording || frame.hasCommandBufferRecorded || frame.isPreCommandRecording ||
            frame.hasPreCommandBufferRecorded) {
            m_swapchainResizeRequested = true;
            MGLOG_I("DirectVulkan: the window resized mid-frame; its swapchain is rebuilt at this frame's present");
            return true;
        }
        // The surface already reports the swapchain's extent: nothing to rebuild.
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE && !SwapchainIsOutOfDate()) return true;
        // The target's own rebuild - the path a surface's re-activation takes: its pending acquire waited
        // out, a new VkSurfaceKHR and swapchain at the surface's current extent and pre-transform, a new
        // target serial for the render passes, and the extent published.
        const Uint64 key = m_activeTargetKey;
        const NativeWindowType window = m_window;
        VulkanRendererConfig config;
        config.SurfaceWidth = m_config.SurfaceWidth;
        config.SurfaceHeight = m_config.SurfaceHeight;
        config.SwapInterval = m_config.SwapInterval;
        DestroySurfaceTarget(key);
        return ActivateSurfaceTarget(key, window, config);
    }

    VkExtent2D VulkanRenderer::SurfaceTargetExtent(Uint64 key) const {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) {
            return m_swapchainObject.GetHandle() != VK_NULL_HANDLE ? m_swapchainObject.GetSurfaceExtent()
                                                                   : VkExtent2D{0, 0};
        }
        const auto parked = m_parkedTargets.find(key);
        if (parked == m_parkedTargets.end() || parked->second.swapchain.GetHandle() == VK_NULL_HANDLE) {
            return VkExtent2D{0, 0};
        }
        return parked->second.swapchain.GetSurfaceExtent();
    }

    VkInstance VulkanRenderer::GetInstance() const {
        return m_instance;
    }

    Bool VulkanRenderer::GetWireAhbImport(WireAhbImport& out) const {
        out = WireAhbImport{};
        if (!m_wireAhbImport || m_device == VK_NULL_HANDLE || m_wireGetAhbProperties == nullptr) return false;
        out.device = m_device;
        out.queue = m_graphicsQueue;
        out.queueFamily = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice.handle, &out.memory);
        out.getAhbProperties = m_wireGetAhbProperties;
        return true;
    }

    void VulkanRenderer::DestroyComputePipelines() {
        if (m_device != VK_NULL_HANDLE) {
            for (const auto& [hash, pipeline] : m_computePipelines) {
                (void)hash;
                if (pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(m_device, pipeline, nullptr);
                }
            }
        }
        m_computePipelines.clear();
    }

    void VulkanRenderer::OnRenderPassesDestroyed(const Vector<VkRenderPass>& renderPasses) {
        if (m_pipelineFactory == nullptr) {
            return;
        }
        // The render-pass sweep's >1024-boundary idle guarantee covers these pipelines
        // too (they are only bound by draws that hit the dying entries), so the factory
        // destroys them immediately. The memo must drop as well: it can hand out a
        // cached handle without touching the factory.
        if (m_pipelineFactory->EvictByRenderPasses(renderPasses) > 0) {
            InvalidatePipelineMemo();
        }
    }

    void VulkanRenderer::OnProgramEvicted(ProgramFactory::HashType programHash,
                                          VkDescriptorSetLayout descriptorSetLayout) {
        // Same >1024-boundary idleness as the program entry: its compute pipeline is
        // only dispatched, and its graphics pipelines only bound, through paths that
        // stamp the entry, so immediate destruction is GPU-safe. (The graphics memo
        // never holds compute pipelines; it only needs invalidating for the factory
        // eviction below.)
        const auto computeIt = m_computePipelines.find(programHash);
        if (computeIt != m_computePipelines.end()) {
            if (computeIt->second != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, computeIt->second, nullptr);
            }
            m_computePipelines.erase(computeIt);
        }
        if (m_pipelineFactory != nullptr && m_pipelineFactory->EvictByProgramHash(programHash) > 0) {
            InvalidatePipelineMemo();
        }
        if (m_uniformManager != nullptr) {
            m_uniformManager->OnDescriptorSetLayoutDestroyed(descriptorSetLayout);
        }
    }

    VkPipeline VulkanRenderer::GetOrCreateComputePipeline(const ProgramFactory::VkProgramObject& programObj) {
        const auto it = m_computePipelines.find(programObj.hash);
        if (it != m_computePipelines.end()) {
            return it->second;
        }

        const auto stageIt = std::find_if(programObj.stages.begin(), programObj.stages.end(),
            [](const VkPipelineShaderStageCreateInfo& stage) {
                return stage.stage == VK_SHADER_STAGE_COMPUTE_BIT;
            });
        MOBILEGL_ASSERT(stageIt != programObj.stages.end(),
                        "GetOrCreateComputePipeline: program has no compute stage");
        if (stageIt == programObj.stages.end()) {
            return VK_NULL_HANDLE;
        }

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = *stageIt;
        pipelineInfo.layout = programObj.pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
                  "GetOrCreateComputePipeline, vkCreateComputePipelines");
        // A failed creation must never be memoized - same contract as
        // PipelineFactory::GetOrCreatePipeline: caching the null would serve it back
        // for the rest of the process and every dispatch of this program would be
        // silently skipped. Retrying costs one failed vkCreateComputePipelines per
        // dispatch, which is the correct price.
        if (pipeline == VK_NULL_HANDLE) {
            MGLOG_E("GetOrCreateComputePipeline: vkCreateComputePipelines failed; not caching the failure");
            return VK_NULL_HANDLE;
        }
        m_computePipelines.emplace(programObj.hash, pipeline);
        return pipeline;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
