// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRenderer.h"

#include "MG_Backend/DirectVulkan/SubgroupSupportPolicy.h"
#include "MG_Backend/DirectGLES/Utils.h"
#include "VertexInputStateFactory.h"
#include "VertexInputStateBuilder.h"

#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/ProgramState/ProgramObject.h"
#include "MG_State/GLState/ProgramState/ShaderObject.h"
#include "MG_State/GLState/SamplerState/SamplerObject.h"
#include "MG_State/GLState/TextureState/TextureObject.h"
#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_Util/Converters/GLToMG/TextureEnumConverter.h"
// Only reached from an MGLOG_W, which the shipping INFO log level compiles out - so the
// missing include never broke a default build and did break every WARN/DEBUG-level one.
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToVk/RenderStateEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Math/HalfFloat.h"
#include "MG_Util/Metrics/TextureMetrics.h"
#include "MG_Util/Texture/PixelStoreProcessor.h"
#include <Config.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vulkan/utility/vk_format_utils.h>
#include <vulkan/vulkan_core.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
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

    static Bool ShouldUseTransientVertexIndexBuffer(const MG_State::GLState::BufferObject& bufferObject) {
        switch (bufferObject.GetUsage()) {
        case BufferUsage::StreamDraw:
        case BufferUsage::StreamRead:
        case BufferUsage::StreamCopy:
        case BufferUsage::DynamicDraw:
        case BufferUsage::DynamicRead:
        case BufferUsage::DynamicCopy:
            return true;
        case BufferUsage::StaticDraw:
        case BufferUsage::StaticRead:
        case BufferUsage::StaticCopy:
        default:
            return false;
        }
    }

    static VkColorComponentFlags GetSupportedColorWriteMaskForComponentCount(SizeT componentCount) {
        switch (componentCount) {
        case 1:
            return VK_COLOR_COMPONENT_R_BIT;
        case 2:
            return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
        case 3:
            return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        case 4:
            return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        default:
            MOBILEGL_ASSERT(false,
                            "GetSupportedColorWriteMaskForComponentCount: unsupported componentCount=%zu",
                            componentCount);
            return 0;
        }
    }

    // GL 4.6 core 15.2.3: a colour format with no alpha channel reads as if alpha were one.
    // The substitution has to happen in the clear value's own type, so this reports the condition
    // and MakeVkClearColorValue applies it to whichever union member the encoding selects.
    static Bool ColorFormatLacksAlpha(const MG_State::GLState::ITextureObject* texture) {
        return texture != nullptr && MG_Util::GetBaseInternalFormatComponentCount(texture->GetFormat()) == 3;
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

    static Int ScaleFramebufferCoordinate(Int value, Int fromExtent, Int toExtent) {
        if (fromExtent <= 0 || toExtent <= 0) {
            return value;
        }
        return static_cast<Int>((static_cast<Int64>(value) * toExtent + fromExtent / 2) / fromExtent);
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
    static DynamicStateShadow g_dynamicStateShadow;

    static void ResetDynamicStateShadow() {
        g_dynamicStateShadow = {};
    }

    // vkCmdBindVertexBuffers, skipped when this command buffer already holds these
    // buffers and offsets at binding 0.
    static void ShadowedBindVertexBuffers(VkCommandBuffer commandBuffer, const VkBuffer* buffers,
                                          const VkDeviceSize* offsets, Uint32 count) {
        auto& shadow = g_dynamicStateShadow;
        Bool identical = shadow.vertexBindValid && shadow.vertexBindingCount == count &&
                         count <= DynamicStateShadow::kMaxShadowedVertexBindings;
        if (identical) {
            for (Uint32 i = 0; i < count; ++i) {
                if (shadow.vertexBuffers[i] != buffers[i] || shadow.vertexOffsets[i] != offsets[i]) {
                    identical = false;
                    break;
                }
            }
        }
        if (identical) {
            return;
        }
        vkCmdBindVertexBuffers(commandBuffer, 0, count, buffers, offsets);
        if (count <= DynamicStateShadow::kMaxShadowedVertexBindings) {
            shadow.vertexBindValid = true;
            shadow.vertexBindingCount = count;
            std::copy_n(buffers, count, shadow.vertexBuffers);
            std::copy_n(offsets, count, shadow.vertexOffsets);
        } else {
            shadow.vertexBindValid = false;
        }
    }

    static void ShadowedSetScissor(VkCommandBuffer commandBuffer, const VkRect2D& scissor) {
        auto& shadow = g_dynamicStateShadow;
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
        const FloatVec4& stored = MG_State::pGLContext->GetViewportIndexed(index);
        const IntVec4 viewportState(static_cast<Int>(std::lround(stored.x())),
                                    static_cast<Int>(std::lround(stored.y())),
                                    static_cast<Int>(std::lround(stored.z())),
                                    static_cast<Int>(std::lround(stored.w())));
        const FloatVec2& depthRange = MG_State::pGLContext->GetDepthRangeIndexed(index);
        const IntVec2 logicalExtent = isDefaultFramebuffer
            ? ResolveDefaultFramebufferLogicalExtent(preTransform, framebufferExtent)
            : framebufferExtent;

        Int viewportX = viewportState.x();
        Int viewportY = viewportState.y();
        Int viewportWidth = viewportState.z() > 0 ? viewportState.z() : logicalExtent.x();
        Int viewportHeight = viewportState.w() > 0 ? viewportState.w() : logicalExtent.y();

        if (isDefaultFramebuffer && IsQuarterTurnPreTransform(preTransform)) {
            viewportX = ScaleFramebufferCoordinate(viewportX, logicalExtent.x(), framebufferExtent.x());
            viewportY = ScaleFramebufferCoordinate(viewportY, logicalExtent.y(), framebufferExtent.y());
            viewportWidth = ScaleFramebufferCoordinate(viewportWidth, logicalExtent.x(), framebufferExtent.x());
            viewportHeight = ScaleFramebufferCoordinate(viewportHeight, logicalExtent.y(), framebufferExtent.y());
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
        auto& shadow = g_dynamicStateShadow;
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
        const FloatVec4& blendColor = MG_State::pGLContext->GetBlendColor();
        const float blendConstants[4] = {
            blendColor.x(),
            blendColor.y(),
            blendColor.z(),
            blendColor.w(),
        };
        auto& shadow = g_dynamicStateShadow;
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
        const Float constantFactor = MG_State::pGLContext->GetPolygonOffsetUnits();
        const Float slopeFactor = MG_State::pGLContext->GetPolygonOffsetFactor();
        auto& shadow = g_dynamicStateShadow;
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
        Float lineWidth = MG_State::pGLContext->GetLineWidth();
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
        auto& shadow = g_dynamicStateShadow;
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

        const Int rawX0 = ScaleFramebufferCoordinate(logicalX0, logicalExtent.x(), framebufferExtent.x());
        const Int rawY0 = ScaleFramebufferCoordinate(logicalY0, logicalExtent.y(), framebufferExtent.y());
        const Int rawX1 = ScaleFramebufferCoordinate(logicalX1, logicalExtent.x(), framebufferExtent.x());
        const Int rawY1 = ScaleFramebufferCoordinate(logicalY1, logicalExtent.y(), framebufferExtent.y());

        VkRect2D scissor{};
        scissor.offset = {std::max<Int>(0, rawX0), std::max<Int>(0, rawY0)};
        scissor.extent = {
            static_cast<Uint32>(std::max<Int>(0, rawX1 - rawX0)),
            static_cast<Uint32>(std::max<Int>(0, rawY1 - rawY0)),
        };
        // A quarter turn maps to {false, false}, so this is a no-op today; it is here so the
        // branch cannot drift away from the identity/180 one when quarter turns are modelled.
        return MapScissorRectToDefaultFramebuffer(scissor, framebufferExtent, preTransform);
    }

    static void ApplyStencilState(VkCommandBuffer commandBuffer) {
        const StencilFaceState& frontStencil = MG_State::pGLContext->GetStencilState(StencilFace::Front);
        const StencilFaceState& backStencil = MG_State::pGLContext->GetStencilState(StencilFace::Back);
        const Uint32 frontReference = static_cast<Uint32>(std::max(frontStencil.Ref, 0));
        const Uint32 backReference = static_cast<Uint32>(std::max(backStencil.Ref, 0));

        auto& shadow = g_dynamicStateShadow;
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

    static SizeT GetComponentCountForShaderValueType(GLenum glType) {
        switch (glType) {
        case GL_FLOAT:
        case GL_INT:
        case GL_UNSIGNED_INT:
            return 1;
        case GL_FLOAT_VEC2:
        case GL_INT_VEC2:
        case GL_UNSIGNED_INT_VEC2:
            return 2;
        case GL_FLOAT_VEC3:
        case GL_INT_VEC3:
        case GL_UNSIGNED_INT_VEC3:
            return 3;
        case GL_FLOAT_VEC4:
        case GL_INT_VEC4:
        case GL_UNSIGNED_INT_VEC4:
            return 4;
        default:
            return 0;
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

    template <typename ComponentT>
    static Float ConvertIntegerVertexComponentToFloat(ComponentT value, Bool normalized) {
        if (!normalized) {
            return static_cast<Float>(value);
        }
        if constexpr (std::is_signed_v<ComponentT>) {
            const Float scaled = static_cast<Float>(value) /
                                 static_cast<Float>(std::numeric_limits<ComponentT>::max());
            return std::max<Float>(-1.0f, scaled);
        } else {
            return static_cast<Float>(value) /
                   static_cast<Float>(std::numeric_limits<ComponentT>::max());
        }
    }

    template <typename ComponentT>
    static Bool ConvertIntegerVertexStreamToFloat32(
        const MG_State::GLState::VertexAttribute& attribute,
        const Uint8* sourceData,
        SizeT sourceStride,
        SizeT elementCount,
        Vector<Float>& outData) {
        if (sourceData == nullptr || attribute.Size < 1 || attribute.Size > 4 || sourceStride == 0) {
            return false;
        }

        const SizeT componentCount = static_cast<SizeT>(attribute.Size);
        outData.resize(elementCount * componentCount);
        for (SizeT element = 0; element < elementCount; ++element) {
            const Uint8* sourceElement = sourceData + element * sourceStride;
            Float* destinationElement = outData.data() + element * componentCount;
            for (SizeT component = 0; component < componentCount; ++component) {
                ComponentT value{};
                Memcpy(&value, sourceElement + component * sizeof(ComponentT), sizeof(ComponentT));
                destinationElement[component] =
                    ConvertIntegerVertexComponentToFloat(value, attribute.Normalized);
            }
        }
        return true;
    }

    static Bool ConvertScaledIntegerVertexStreamToFloat32(
        const MG_State::GLState::VertexAttribute& attribute,
        const Uint8* sourceData,
        SizeT sourceStride,
        SizeT elementCount,
        Vector<Float>& outData) {
        switch (attribute.Type) {
        case DataType::Int8:
            return ConvertIntegerVertexStreamToFloat32<Int8>(
                attribute, sourceData, sourceStride, elementCount, outData);
        case DataType::Uint8:
            return ConvertIntegerVertexStreamToFloat32<Uint8>(
                attribute, sourceData, sourceStride, elementCount, outData);
        case DataType::Int16:
            return ConvertIntegerVertexStreamToFloat32<Int16>(
                attribute, sourceData, sourceStride, elementCount, outData);
        case DataType::Uint16:
            return ConvertIntegerVertexStreamToFloat32<Uint16>(
                attribute, sourceData, sourceStride, elementCount, outData);
        default:
            return false;
        }
    }

    static Bool RepackVertexStream(const Uint8* sourceData,
                                   SizeT sourceStride,
                                   SizeT elementSize,
                                   SizeT elementCount,
                                   Vector<Uint8>& outData) {
        if (sourceData == nullptr || sourceStride == 0 || elementSize == 0) {
            return false;
        }
        outData.resize(elementCount * elementSize);
        for (SizeT element = 0; element < elementCount; ++element) {
            Memcpy(outData.data() + element * elementSize,
                   sourceData + element * sourceStride,
                   elementSize);
        }
        return true;
    }

    static NumericDomain GetNumericDomainForTextureInternalFormat(TextureInternalFormat format) {
        switch (format) {
        case TextureInternalFormat::R8I:
        case TextureInternalFormat::R16I:
        case TextureInternalFormat::R32I:
        case TextureInternalFormat::RG8I:
        case TextureInternalFormat::RG16I:
        case TextureInternalFormat::RG32I:
        case TextureInternalFormat::RGB8I:
        case TextureInternalFormat::RGB16I:
        case TextureInternalFormat::RGB32I:
        case TextureInternalFormat::RGBA8I:
        case TextureInternalFormat::RGBA16I:
        case TextureInternalFormat::RGBA32I:
            return NumericDomain::Sint;
        case TextureInternalFormat::R8UI:
        case TextureInternalFormat::R16UI:
        case TextureInternalFormat::R32UI:
        case TextureInternalFormat::RG8UI:
        case TextureInternalFormat::RG16UI:
        case TextureInternalFormat::RG32UI:
        case TextureInternalFormat::RGB8UI:
        case TextureInternalFormat::RGB16UI:
        case TextureInternalFormat::RGB32UI:
        case TextureInternalFormat::RGBA8UI:
        case TextureInternalFormat::RGBA16UI:
        case TextureInternalFormat::RGBA32UI:
        case TextureInternalFormat::RGB10A2UI:
            return NumericDomain::Uint;
        case TextureInternalFormat::DepthComponent:
        case TextureInternalFormat::DepthComponent16:
        case TextureInternalFormat::DepthComponent24:
        case TextureInternalFormat::DepthComponent32:
        case TextureInternalFormat::DepthComponent32F:
        case TextureInternalFormat::Depth24Stencil8:
        case TextureInternalFormat::Depth32FStencil8:
        case TextureInternalFormat::DepthStencil:
            return NumericDomain::Unknown;
        default:
            return NumericDomain::FloatLike;
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

    static const char* VkImageLayoutToString(VkImageLayout layout) {
        switch (layout) {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                return "VK_IMAGE_LAYOUT_UNDEFINED";
            case VK_IMAGE_LAYOUT_GENERAL:
                return "VK_IMAGE_LAYOUT_GENERAL";
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL";
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return "VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL";
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL";
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return "VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL";
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return "VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL";
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                return "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR";
            case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
                return "VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL";
            case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
                return "VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL";
            default:
                return "VK_IMAGE_LAYOUT_OTHER";
        }
    }

    static Bool ActiveRenderPassUsesTexture(const ActiveRenderPassInfo& activeRenderPass,
                                            const MG_State::GLState::ITextureObject& texture) {
        for (const auto& trackedAttachment : activeRenderPass.trackedAttachmentLayouts) {
            if (trackedAttachment.target != TrackedAttachmentTarget::Texture) {
                continue;
            }
            // Raw identity compare (see textureRaw): the caller's texture is
            // live, so a dangling tracked pointer can never equal its address
            // unless the allocator reused it - and that false positive merely
            // ends the render pass early, never misses a genuine use.
            if (trackedAttachment.textureRaw == &texture) {
                return true;
            }
        }
        return false;
    }

    static void RecordClearBufferError(const char* func, ErrorCode code, const char* message) {
        MG_State::pGLContext->RecordError(code, MakeUnique<GenericErrorInfo>("DirectVulkan", func, message));
    }

    static void RecordTextureCopyError(const char* func, ErrorCode code, const char* message) {
        MG_State::pGLContext->RecordError(code, MakeUnique<GenericErrorInfo>("DirectVulkan", func, message));
    }

    static Bool HasDistinctCompleteDepthStencilTextureAttachments(
        const MG_State::GLState::FramebufferObject& framebufferObject) {
        if (framebufferObject.GetExternalIndex() == 0) {
            return false;
        }

        const auto& depthAttachment = framebufferObject.GetAttachment(FramebufferAttachmentType::Depth);
        const auto& stencilAttachment = framebufferObject.GetAttachment(FramebufferAttachmentType::Stencil);
        if (!depthAttachment.IsComplete() || !stencilAttachment.IsComplete() ||
            !depthAttachment.IsTexture() || !stencilAttachment.IsTexture()) {
            return false;
        }

        return depthAttachment.GetTexture().get() != stencilAttachment.GetTexture().get() ||
               depthAttachment.GetTextureUploadTarget() != stencilAttachment.GetTextureUploadTarget() ||
               depthAttachment.GetTextureLevel() != stencilAttachment.GetTextureLevel();
    }

    static Bool IsColorAttachment(FramebufferAttachmentType attachmentType) {
        return attachmentType >= FramebufferAttachmentType::Color0 &&
               attachmentType <= FramebufferAttachmentType::Color31;
    }

    static Bool HasUnsupportedCompleteRenderbufferAttachment(
        const MG_State::GLState::FramebufferObject& framebufferObject) {
        if (framebufferObject.GetExternalIndex() == 0) {
            return false;
        }

        const auto& depthAttachment = framebufferObject.GetAttachment(FramebufferAttachmentType::Depth);
        const auto& stencilAttachment = framebufferObject.GetAttachment(FramebufferAttachmentType::Stencil);
        if (!depthAttachment.IsComplete() || !stencilAttachment.IsComplete()) {
            return false;
        }
        if (depthAttachment.IsRenderbuffer() && stencilAttachment.IsRenderbuffer()) {
            return depthAttachment.GetRenderbuffer().get() != stencilAttachment.GetRenderbuffer().get();
        }
        if ((depthAttachment.IsRenderbuffer() || stencilAttachment.IsRenderbuffer()) &&
            (depthAttachment.IsTexture() || stencilAttachment.IsTexture())) {
            return true;
        }
        return false;
    }

    static Bool IsUnsupportedFramebufferForDirectVulkan(
        const MG_State::GLState::FramebufferObject& framebufferObject) {
        // TODO: Revisit this gate when DirectVulkan has full color renderbuffer render/blit/readback support.
        return HasDistinctCompleteDepthStencilTextureAttachments(framebufferObject) ||
               HasUnsupportedCompleteRenderbufferAttachment(framebufferObject);
    }

    static void RecordUnsupportedFramebufferError(const char* func) {
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidFramebufferOperation,
            MakeUnique<GenericErrorInfo>(
                "DirectVulkan", func,
                "DirectVulkan does not support this non-default framebuffer configuration."));
    }

    static Bool IsValidSampledImageLayout(VkImageLayout layout) {
        switch (layout) {
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            case VK_IMAGE_LAYOUT_GENERAL:
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
            case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
                return true;
            default:
                return false;
        }
    }

    namespace {
        static constexpr Uint32 kDescriptorSetsPerFrame = 64;
        static constexpr Uint kHiddenBlitProgramId = 0xFFFFFFF0u;
        static constexpr Uint kHiddenBlitVertexShaderId = 0xFFFFFFF1u;
        static constexpr Uint kHiddenBlitFragmentShaderId = 0xFFFFFFF2u;
        static constexpr Uint kHiddenBlitNearestSamplerId = 0xFFFFFFF3u;
        static constexpr Uint kHiddenBlitLinearSamplerId = 0xFFFFFFF4u;
        static constexpr Uint kHiddenDepthMipmapProgramId = 0xFFFFFFF5u;
        static constexpr Uint kHiddenDepthMipmapVertexShaderId = 0xFFFFFFF6u;
        static constexpr Uint kHiddenDepthMipmapFragmentShaderId = 0xFFFFFFF7u;
        static constexpr const char* kFullscreenTriangleVertexShaderSource = R"(#version 460 core
uniform vec4 uSrcRect;
uniform vec4 uDstRect;
uniform int uSurfaceTransform;
layout(location = 0) out vec2 vTexCoord;

vec2 ApplySurfaceTransform(vec2 position, int transform) {
    vec2 p = position;
    p.y = -p.y;
    if (transform == 1) {
        p = vec2(-p.y, p.x);
    } else if (transform == 2) {
        p = -p;
    } else if (transform == 3) {
        p = vec2(p.y, -p.x);
    }
    return p;
}

void main() {
    const vec2 uvTri[3] = vec2[](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0)
    );
    vec2 uv = uvTri[gl_VertexID];
    vec2 dst = uDstRect.xy + uv * uDstRect.zw;
    vec2 clip = dst * 2.0 - 1.0;
    clip = ApplySurfaceTransform(clip, uSurfaceTransform);
    gl_Position = vec4(clip, 0.0, 1.0);
    vTexCoord = uSrcRect.xy + uv * uSrcRect.zw;
}
)";

        static constexpr const char* kBlitFragmentShaderSource = R"(#version 460 core
layout(binding = 0) uniform sampler2D uSource;
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // Explicit LOD, not texture(): a blit reads exactly the selected level, so
    // derivative-based mip selection has no business here. It is also load-bearing:
    // on Adreno 650 (driver 512.502) an implicit-LOD sample of this single-mip
    // UBWC render target through the pre-rotation (ROTATE_90) mapping reads past
    // the image's allocation - despite the sampler's maxLod=0 and a nominal 1:1
    // texel mapping whose LOD is 0, so the driver's implicit-LOD path itself is at
    // fault - and page-faults the GPU once the neighbouring memory is returned to
    // the kernel (frame 2 of Minecraft 26.2's resource reload; the kernel then
    // invalidates the context and the next submit dies with EDEADLK ->
    // VK_ERROR_DEVICE_LOST at Present). Verified on device: texture() faults on
    // the second frame every run, textureLod survives with identical state.
    outColor = textureLod(uSource, vTexCoord, 0.0);
}
)";

        static constexpr const char* kDepthMipmapFragmentShaderSource = R"(#version 460 core
layout(binding = 0) uniform sampler2D uSource;
layout(location = 0) in vec2 vTexCoord;
uniform ivec2 uSrcTexelSize;

void main() {
    ivec2 srcBase = ivec2(vTexCoord * vec2(uSrcTexelSize));
    ivec2 srcMax = uSrcTexelSize - ivec2(1);
    float depth0 = texelFetch(uSource, clamp(srcBase, ivec2(0), srcMax), 0).r;
    float depth1 = texelFetch(uSource, clamp(srcBase + ivec2(1, 0), ivec2(0), srcMax), 0).r;
    float depth2 = texelFetch(uSource, clamp(srcBase + ivec2(0, 1), ivec2(0), srcMax), 0).r;
    float depth3 = texelFetch(uSource, clamp(srcBase + ivec2(1, 1), ivec2(0), srcMax), 0).r;
    gl_FragDepth = 0.25 * (depth0 + depth1 + depth2 + depth3);
}
)";


        static Uint32 ComputeFullMipLevelCount(const IntVec3& baseTexelSize) {
            Int maxDimension = std::max<Int>(
                baseTexelSize.x(),
                std::max<Int>(baseTexelSize.y(), std::max<Int>(baseTexelSize.z(), 1)));
            Uint32 mipLevelCount = 1;
            while (maxDimension > 1) {
                maxDimension = std::max<Int>(maxDimension / 2, 1);
                ++mipLevelCount;
            }
            return mipLevelCount;
        }

        static IntVec3 ComputeMipTexelSize(const IntVec3& baseTexelSize, Uint32 relativeMipLevel) {
            const Int width = std::max<Int>(baseTexelSize.x() >> static_cast<Int>(relativeMipLevel), 1);
            const Int height = std::max<Int>(baseTexelSize.y() >> static_cast<Int>(relativeMipLevel), 1);
            const Int depth = std::max<Int>(baseTexelSize.z() >> static_cast<Int>(relativeMipLevel), 1);
            return {width, height, depth};
        }

        static Bool EnsureGenerateMipmapStorageAllocated(::MobileGL::MG_State::GLState::TextureObjectMipmap& texture,
                                                         Uint32 baseMipLevel) {
            const Uint32 existingMipLevelCount = static_cast<Uint32>(texture.GetMipmapLevelCount());
            if (existingMipLevelCount <= baseMipLevel) {
                return false;
            }

            const auto& uploadTargets = texture.GetUploadTargets();
            if (uploadTargets.empty()) {
                return false;
            }

            for (const auto uploadTarget : uploadTargets) {
                const IntVec3 baseTexelSize = texture.GetMipmapTexelSize(uploadTarget, baseMipLevel);
                const SizeT baseByteSize = texture.GetMipmapByteSize(uploadTarget, baseMipLevel);
                if (baseTexelSize.x() <= 0 || baseTexelSize.y() <= 0 || baseTexelSize.z() <= 0 ||
                    baseByteSize == 0) {
                    return false;
                }

                const SizeT baseTexelCount = static_cast<SizeT>(baseTexelSize.x()) *
                                             static_cast<SizeT>(baseTexelSize.y()) *
                                             static_cast<SizeT>(baseTexelSize.z());
                if (baseTexelCount == 0 || (baseByteSize % baseTexelCount) != 0) {
                    return false;
                }

                const SizeT bytesPerTexel = baseByteSize / baseTexelCount;
                const Uint32 requiredMipLevelCount = baseMipLevel + ComputeFullMipLevelCount(baseTexelSize);
                if (existingMipLevelCount >= requiredMipLevelCount) {
                    continue;
                }

                for (Uint32 level = existingMipLevelCount; level < requiredMipLevelCount; ++level) {
                    const IntVec3 levelTexelSize = ComputeMipTexelSize(baseTexelSize, level - baseMipLevel);
                    const SizeT levelByteSize = bytesPerTexel * static_cast<SizeT>(levelTexelSize.x()) *
                                                static_cast<SizeT>(levelTexelSize.y()) *
                                                static_cast<SizeT>(levelTexelSize.z());
                    texture.AllocateStorage(uploadTarget, level, {levelTexelSize, levelByteSize});
                    texture.MarkStorageDirty(uploadTarget, level, false);
                }
            }
            return true;
        }

        static VkImageLayout ResolveGenerateMipmapFinalLayout(VkImageAspectFlags aspectMask) {
            return (aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        static Bool IsCubeMapFaceUploadTarget(TextureUploadTarget target) {
            return target >= TextureUploadTarget::CubeMapPositiveX &&
                   target <= TextureUploadTarget::CubeMapNegativeZ;
        }

        static Uint32 ResolveAttachmentBaseArrayLayer(const MG_State::GLState::FramebufferAttachmentObject& attachment) {
            const TextureUploadTarget uploadTarget = attachment.GetTextureUploadTarget();
            if (IsCubeMapFaceUploadTarget(uploadTarget)) {
                return static_cast<Uint32>(uploadTarget) - static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
            }
            // Every other layered attachment names its layer directly. Returning 0 regardless made
            // every blit, copy and ReadPixels against such an attachment read layer zero.
            return static_cast<Uint32>(std::max(attachment.GetTextureLayer(), 0));
        }

        // A 3D image has arrayLayers == 1: its "layer" is a z slice, which has to travel as an
        // image offset rather than a base array layer (VkBufferImageCopy requires baseArrayLayer 0
        // for VK_IMAGE_TYPE_3D).
        static Bool AttachmentIsDepthSlice(const MG_State::GLState::FramebufferAttachmentObject& attachment) {
            return attachment.IsTexture() && attachment.GetTexture() &&
                   attachment.GetTexture()->GetTarget() == TextureTarget::Texture3D;
        }

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

        static VkImageAspectFlags GetSwapchainDepthStencilAspectMask(const SwapchainObject& swapchainObject) {
            VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            switch (swapchainObject.GetDepthStencilFormat()) {
                case VK_FORMAT_D24_UNORM_S8_UINT:
                case VK_FORMAT_D32_SFLOAT_S8_UINT:
                    aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                    break;
                default:
                    break;
            }
            return aspectMask;
        }

        static FramebufferAttachmentType ResolveFramebufferCopyAttachmentType(
            const MG_State::GLState::FramebufferObject& fbo, Bool isReadFramebuffer,
            VkImageAspectFlags aspectMask) {
            if ((aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
                return isReadFramebuffer ? fbo.GetReadBuffer() : fbo.GetDrawBuffers()[0];
            }
            if ((aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
                return FramebufferAttachmentType::Depth;
            }
            if ((aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
                return FramebufferAttachmentType::Stencil;
            }
            return FramebufferAttachmentType::None;
        }

        static Bool ResolveColorBlitBinding(MG_State::GLState::FramebufferObject& fbo, Bool isReadFramebuffer,
                                            Uint32 swapchainImageIndex, SwapchainObject& swapchainObject,
                                            VkTextureManager& textureManager,
                                            VkRenderPassManager& renderPassManager, BlitImageBinding& outBinding) {
            const Bool isDefaultFbo = fbo.IsDefaultFramebuffer();
            const FramebufferAttachmentType attachmentType =
                isReadFramebuffer ? fbo.GetReadBuffer() : fbo.GetDrawBuffers()[0];
            outBinding.label = isReadFramebuffer ? "read" : "draw";

            if (isDefaultFbo) {
                const Bool defaultColorAttachment =
                    attachmentType == FramebufferAttachmentType::Color0 ||
                    (attachmentType >= FramebufferAttachmentType::FrontLeft &&
                     attachmentType <= FramebufferAttachmentType::BackRight);
                if (!defaultColorAttachment) {
                    MGLOG_E_ONCE("BlitFramebuffer skipped: default framebuffer color attachment %d is not supported",
                            static_cast<Int>(attachmentType));
                    return false;
                }
                outBinding.image = swapchainObject.GetImage(swapchainImageIndex);
                outBinding.trackedLayout = nullptr;
                outBinding.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                outBinding.format = swapchainObject.GetSurfaceFormat().format;
                const auto extent = swapchainObject.GetExtent();
                outBinding.extent = {static_cast<Int>(extent.width), static_cast<Int>(extent.height)};
                outBinding.mipLevel = 0;
                outBinding.mipLevelCount = 1;
                outBinding.baseArrayLayer = 0;
                outBinding.layerCount = 1;
                return true;
            }

            if (attachmentType < FramebufferAttachmentType::Color0 || attachmentType > FramebufferAttachmentType::Color31) {
                MGLOG_E_ONCE("BlitFramebuffer only supports color attachments right now (attachment=%d)",
                        static_cast<Int>(attachmentType));
                return false;
            }

            const auto& attachment = fbo.GetAttachment(attachmentType);
            if (!attachment.IsComplete()) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: %s framebuffer color attachment is incomplete",
                        isReadFramebuffer ? "read" : "draw");
                return false;
            }
            if (attachment.IsRenderbuffer()) {
                const auto& renderbuffer = attachment.GetRenderbuffer();
                auto* rbResource = renderPassManager.GetOrCreateRenderbufferResource(renderbuffer);
                if (rbResource == nullptr || (rbResource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
                    MGLOG_E_ONCE("BlitFramebuffer skipped: %s framebuffer color renderbuffer %u is unsupported",
                            outBinding.label, renderbuffer->GetExternalIndex());
                    return false;
                }
                outBinding.image = rbResource->image;
                outBinding.trackedLayout = &rbResource->layout;
                outBinding.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                outBinding.format = rbResource->format;
                outBinding.sampleCount = rbResource->sampleCount;
                outBinding.extent = {static_cast<Int>(rbResource->extent.width),
                                     static_cast<Int>(rbResource->extent.height)};
                outBinding.mipLevel = 0;
                outBinding.mipLevelCount = 1;
                outBinding.baseArrayLayer = 0;
                outBinding.layerCount = 1;
                return true;
            }
            if (!attachment.IsTexture()) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: unsupported framebuffer attachment type");
                return false;
            }

            auto* texture = attachment.GetTexture().get();
            MOBILEGL_ASSERT(texture != nullptr, "ResolveColorBlitBinding: texture attachment is null");

            auto* resource = textureManager.SyncTextureAndGetDescriptor(*texture);
            if (resource == nullptr) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: failed to sync %s framebuffer textureId=%d",
                        outBinding.label, texture->GetExternalIndex());
                return false;
            }
            if ((resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: %s framebuffer attachment textureId=%d is not a color image",
                        outBinding.label, texture->GetExternalIndex());
                return false;
            }

            outBinding.image = resource->image;
            outBinding.trackedLayout = &resource->layout;
            outBinding.aspectMask = resource->aspect;
            outBinding.format = resource->format;
            outBinding.sampleCount = resource->sampleCount;
            const auto attachmentExtent = attachment.GetSize();
            outBinding.extent = {attachmentExtent.x(), attachmentExtent.y()};
            outBinding.mipLevel = static_cast<Uint32>(std::max(attachment.GetTextureLevel(), 0));
            outBinding.mipLevelCount = resource->mipLevels;
            if (AttachmentIsDepthSlice(attachment)) {
                outBinding.depthOffset = static_cast<Uint32>(std::max(attachment.GetTextureLayer(), 0));
                outBinding.baseArrayLayer = 0;
            } else {
                outBinding.baseArrayLayer = ResolveAttachmentBaseArrayLayer(attachment);
            }
            outBinding.layerCount = 1;
            return true;
        }

        static Bool ResolveFramebufferBlitBinding(MG_State::GLState::FramebufferObject& fbo, Bool isReadFramebuffer,
                                                  Uint32 swapchainImageIndex, SwapchainObject& swapchainObject,
                                                  VkTextureManager& textureManager,
                                                  VkRenderPassManager& renderPassManager,
                                                  VkImageAspectFlags requiredAspectMask,
                                                  BlitImageBinding& outBinding) {
            const Bool isDefaultFbo = fbo.IsDefaultFramebuffer();
            const auto attachmentType = ResolveFramebufferCopyAttachmentType(fbo, isReadFramebuffer, requiredAspectMask);
            if (attachmentType == FramebufferAttachmentType::None) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: unsupported aspect mask=0x%x",
                        static_cast<Uint32>(requiredAspectMask));
                return false;
            }

            outBinding.label = isReadFramebuffer ? "read" : "draw";
            if (isDefaultFbo) {
                const auto extent = swapchainObject.GetExtent();
                outBinding.extent = {static_cast<Int>(extent.width), static_cast<Int>(extent.height)};
                outBinding.mipLevel = 0;
                outBinding.mipLevelCount = 1;
                outBinding.baseArrayLayer = 0;
                outBinding.layerCount = 1;
                outBinding.trackedLayout = nullptr;
                if ((requiredAspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
                    outBinding.image = swapchainObject.GetImage(swapchainImageIndex);
                    outBinding.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    return true;
                }

                const VkImageAspectFlags swapchainAspectMask = GetSwapchainDepthStencilAspectMask(swapchainObject);
                if ((swapchainAspectMask & requiredAspectMask) != requiredAspectMask) {
                    MGLOG_E_ONCE("BlitFramebuffer skipped: swapchain depth image missing required aspect mask=0x%x",
                            static_cast<Uint32>(requiredAspectMask));
                    return false;
                }

                outBinding.image = swapchainObject.GetDepthStencilImage(swapchainImageIndex);
                outBinding.format = swapchainObject.GetDepthStencilFormat();
                outBinding.aspectMask = requiredAspectMask;
                return true;
            }

            const auto& attachment = fbo.GetAttachment(attachmentType);
            if (!attachment.IsComplete()) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: %s framebuffer attachment is incomplete (fbo=%u attachmentType=%d "
                        "isTexture=%d isRenderbuffer=%d texId=%d)",
                        outBinding.label, fbo.GetExternalIndex(), static_cast<Int>(attachmentType),
                        attachment.IsTexture() ? 1 : 0, attachment.IsRenderbuffer() ? 1 : 0,
                        attachment.IsTexture() && attachment.GetTexture() ? static_cast<Int>(attachment.GetTexture()->GetExternalIndex()) : -1);
                return false;
            }
            if (attachment.IsRenderbuffer()) {
                const auto& renderbuffer = attachment.GetRenderbuffer();
                auto* rbResource = renderPassManager.GetOrCreateRenderbufferResource(renderbuffer);
                if (rbResource == nullptr) {
                    MGLOG_E_ONCE("BlitFramebuffer skipped: %s framebuffer renderbuffer %u is unsupported",
                            outBinding.label, renderbuffer->GetExternalIndex());
                    return false;
                }
                if ((rbResource->aspect & requiredAspectMask) != requiredAspectMask) {
                    MGLOG_E_ONCE("BlitFramebuffer skipped: %s framebuffer renderbuffer %u is missing aspect mask=0x%x",
                            outBinding.label, renderbuffer->GetExternalIndex(),
                            static_cast<Uint32>(requiredAspectMask));
                    return false;
                }
                outBinding.image = rbResource->image;
                outBinding.trackedLayout = &rbResource->layout;
                outBinding.aspectMask = requiredAspectMask;
                outBinding.format = rbResource->format;
                outBinding.sampleCount = rbResource->sampleCount;
                outBinding.extent = {static_cast<Int>(rbResource->extent.width),
                                     static_cast<Int>(rbResource->extent.height)};
                outBinding.mipLevel = 0;
                outBinding.mipLevelCount = 1;
                outBinding.baseArrayLayer = 0;
                outBinding.layerCount = 1;
                return true;
            }
            if (!attachment.IsTexture()) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: unsupported framebuffer attachment type");
                return false;
            }

            auto* texture = attachment.GetTexture().get();
            MOBILEGL_ASSERT(texture != nullptr, "ResolveFramebufferBlitBinding: texture attachment is null");
            auto* resource = textureManager.SyncTextureAndGetDescriptor(*texture);
            if (resource == nullptr) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: failed to sync %s framebuffer textureId=%d",
                        outBinding.label, texture->GetExternalIndex());
                return false;
            }
            if ((resource->aspect & requiredAspectMask) != requiredAspectMask) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: %s framebuffer attachment textureId=%d is missing aspect mask=0x%x",
                        outBinding.label, texture->GetExternalIndex(), static_cast<Uint32>(requiredAspectMask));
                return false;
            }

            outBinding.image = resource->image;
            outBinding.trackedLayout = &resource->layout;
            outBinding.aspectMask = requiredAspectMask;
            outBinding.format = resource->format;
            outBinding.sampleCount = resource->sampleCount;
            const auto attachmentExtent = attachment.GetSize();
            outBinding.extent = {attachmentExtent.x(), attachmentExtent.y()};
            outBinding.mipLevel = static_cast<Uint32>(std::max(attachment.GetTextureLevel(), 0));
            outBinding.mipLevelCount = resource->mipLevels;
            if (AttachmentIsDepthSlice(attachment)) {
                outBinding.depthOffset = static_cast<Uint32>(std::max(attachment.GetTextureLayer(), 0));
                outBinding.baseArrayLayer = 0;
            } else {
                outBinding.baseArrayLayer = ResolveAttachmentBaseArrayLayer(attachment);
            }
            outBinding.layerCount = 1;
            return true;
        }

        static Bool ResolveTextureCopyDestinationBinding(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel,
                                                         VkTextureManager& textureManager, BlitImageBinding& outBinding) {
            auto* resource = textureManager.SyncTextureAndGetDescriptor(texture);
            if (resource == nullptr) {
                MGLOG_E_ONCE("CopyTexSubImage2D skipped: failed to sync destination textureId=%d",
                        texture.GetExternalIndex());
                return false;
            }
            const VkImageAspectFlags copyAspectMask =
                resource->aspect & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            if (copyAspectMask == 0) {
                MGLOG_E_ONCE("CopyTexSubImage2D skipped: destination textureId=%d uses unsupported aspect mask=0x%x",
                        texture.GetExternalIndex());
                return false;
            }
            if (mipLevel >= resource->mipLevels) {
                MGLOG_E_ONCE("CopyTexSubImage2D skipped: destination textureId=%d mip=%u out of range (mips=%u)",
                        texture.GetExternalIndex(), mipLevel, resource->mipLevels);
                return false;
            }

            outBinding.image = resource->image;
            outBinding.trackedLayout = &resource->layout;
            outBinding.aspectMask = copyAspectMask;
            outBinding.extent = {
                static_cast<Int>(std::max(1u, resource->extent.width >> mipLevel)),
                static_cast<Int>(std::max(1u, resource->extent.height >> mipLevel))};
            outBinding.mipLevel = mipLevel;
            outBinding.mipLevelCount = 1;
            outBinding.baseArrayLayer = 0;
            outBinding.layerCount = 1;
            outBinding.label = "destination texture";
            return true;
        }

        static Bool ResolveTextureCopySourceBinding(MG_State::GLState::FramebufferObject& fbo, Uint32 swapchainImageIndex,
                                                    SwapchainObject& swapchainObject,
                                                    VkTextureManager& textureManager,
                                                    VkRenderPassManager& renderPassManager,
                                                    VkImageAspectFlags requiredAspectMask,
                                                    BlitImageBinding& outBinding) {
            const Bool isDefaultFbo = fbo.IsDefaultFramebuffer();
            const auto attachmentType = ResolveFramebufferCopyAttachmentType(fbo, true, requiredAspectMask);
            if (attachmentType == FramebufferAttachmentType::None) {
                MGLOG_E_ONCE("CopyTexSubImage2D skipped: unsupported source aspect mask=0x%x",
                        static_cast<Uint32>(requiredAspectMask));
                return false;
            }

            outBinding.label = "read";
            if (isDefaultFbo) {
                const auto extent = swapchainObject.GetExtent();
                outBinding.extent = {static_cast<Int>(extent.width), static_cast<Int>(extent.height)};
                outBinding.mipLevel = 0;
                outBinding.mipLevelCount = 1;
                outBinding.baseArrayLayer = 0;
                outBinding.layerCount = 1;
                outBinding.trackedLayout = nullptr;
                if ((requiredAspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
                    outBinding.image = swapchainObject.GetImage(swapchainImageIndex);
                    outBinding.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    return true;
                }

                const VkImageAspectFlags swapchainAspectMask = GetSwapchainDepthStencilAspectMask(swapchainObject);
                if ((swapchainAspectMask & requiredAspectMask) != requiredAspectMask) {
                    MGLOG_E_ONCE("CopyTexSubImage2D skipped: swapchain depth image missing required aspect mask=0x%x",
                            static_cast<Uint32>(requiredAspectMask));
                    return false;
                }

                outBinding.image = swapchainObject.GetDepthStencilImage(swapchainImageIndex);
                outBinding.aspectMask = requiredAspectMask;
                return true;
            }

            const auto& attachment = fbo.GetAttachment(attachmentType);
            if (!attachment.IsComplete()) {
                MGLOG_E_ONCE("CopyTexSubImage2D skipped: read framebuffer attachment %d is incomplete",
                        static_cast<Int>(attachmentType));
                return false;
            }
            if (attachment.IsRenderbuffer()) {
                const auto& renderbuffer = attachment.GetRenderbuffer();
                auto* rbResource = renderPassManager.GetOrCreateRenderbufferResource(renderbuffer);
                if (rbResource == nullptr) {
                    MGLOG_E_ONCE("CopyTexSubImage2D skipped: read framebuffer renderbuffer %u is unsupported",
                            renderbuffer->GetExternalIndex());
                    return false;
                }
                if ((rbResource->aspect & requiredAspectMask) != requiredAspectMask) {
                    MGLOG_E_ONCE("CopyTexSubImage2D skipped: read framebuffer renderbuffer %u aspect mask=0x%x "
                            "does not satisfy requested mask=0x%x",
                            renderbuffer->GetExternalIndex(), static_cast<Uint32>(rbResource->aspect),
                            static_cast<Uint32>(requiredAspectMask));
                    return false;
                }
                outBinding.image = rbResource->image;
                outBinding.trackedLayout = &rbResource->layout;
                outBinding.aspectMask = requiredAspectMask;
                outBinding.format = rbResource->format;
                outBinding.sampleCount = rbResource->sampleCount;
                outBinding.extent = {static_cast<Int>(rbResource->extent.width),
                                     static_cast<Int>(rbResource->extent.height)};
                outBinding.mipLevel = 0;
                outBinding.mipLevelCount = 1;
                outBinding.baseArrayLayer = 0;
                outBinding.layerCount = 1;
                return true;
            }
            if (!attachment.IsTexture()) {
                MGLOG_E_ONCE("CopyTexSubImage2D skipped: unsupported read framebuffer attachment type");
                return false;
            }

            auto* texture = attachment.GetTexture().get();
            MOBILEGL_ASSERT(texture != nullptr, "ResolveTextureCopySourceBinding: source texture attachment is null");
            auto* resource = textureManager.SyncTextureAndGetDescriptor(*texture);
            if (resource == nullptr) {
                MGLOG_E_ONCE("CopyTexSubImage2D skipped: failed to sync read framebuffer textureId=%d",
                        texture->GetExternalIndex());
                return false;
            }
            if ((resource->aspect & requiredAspectMask) != requiredAspectMask) {
                MGLOG_E_ONCE("CopyTexSubImage2D skipped: read framebuffer textureId=%d aspect mask=0x%x does not satisfy requested mask=0x%x",
                        texture->GetExternalIndex(), static_cast<Uint32>(resource->aspect),
                        static_cast<Uint32>(requiredAspectMask));
                return false;
            }

            outBinding.image = resource->image;
            outBinding.trackedLayout = &resource->layout;
            outBinding.aspectMask = requiredAspectMask;
            outBinding.format = resource->format;
            outBinding.sampleCount = resource->sampleCount;
            const auto attachmentExtent = attachment.GetSize();
            outBinding.extent = {attachmentExtent.x(), attachmentExtent.y()};
            outBinding.mipLevel = static_cast<Uint32>(std::max(attachment.GetTextureLevel(), 0));
            outBinding.mipLevelCount = 1;
            if (AttachmentIsDepthSlice(attachment)) {
                outBinding.depthOffset = static_cast<Uint32>(std::max(attachment.GetTextureLayer(), 0));
                outBinding.baseArrayLayer = 0;
            } else {
                outBinding.baseArrayLayer = ResolveAttachmentBaseArrayLayer(attachment);
            }
            outBinding.layerCount = 1;
            return true;
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

        static Bool RequiresShaderBlitToDefaultFramebuffer(VkSurfaceTransformFlagBitsKHR preTransform) {
            switch (preTransform) {
                case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
                case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
                    return true;
                default:
                    return false;
            }
        }

        static void ApplyNativeBlitDefaultFramebufferTransform(VkSurfaceTransformFlagBitsKHR preTransform,
                                                               const BlitImageBinding& dstBinding,
                                                               VkImageBlit& blitRegion) {
            switch (preTransform) {
                case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
                    blitRegion.dstOffsets[0].y = dstBinding.extent.y() - blitRegion.dstOffsets[0].y;
                    blitRegion.dstOffsets[1].y = dstBinding.extent.y() - blitRegion.dstOffsets[1].y;
                    break;
                case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
                    blitRegion.dstOffsets[0].x = dstBinding.extent.x() - blitRegion.dstOffsets[0].x;
                    blitRegion.dstOffsets[1].x = dstBinding.extent.x() - blitRegion.dstOffsets[1].x;
                    break;
                default:
                    break;
            }
        }

        // The same conversion on the READ side, which never had one: a blit whose source is the
        // default framebuffer used raw GL offsets against a display-oriented image, so it sampled
        // the mirrored band and wrote it upside down. Mapping BOTH endpoints inverts the offset
        // pair, and an inverted pair is exactly how VkImageBlit spells "flip this axis" - so the
        // band and the row order are corrected in one step. A full-extent blit is unchanged in
        // band and gains the row flip it always needed.
        static void ApplyNativeBlitDefaultFramebufferSourceTransform(VkSurfaceTransformFlagBitsKHR preTransform,
                                                                     const BlitImageBinding& srcBinding,
                                                                     VkImageBlit& blitRegion) {
            switch (preTransform) {
                case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
                    blitRegion.srcOffsets[0].y = srcBinding.extent.y() - blitRegion.srcOffsets[0].y;
                    blitRegion.srcOffsets[1].y = srcBinding.extent.y() - blitRegion.srcOffsets[1].y;
                    break;
                case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
                    blitRegion.srcOffsets[0].x = srcBinding.extent.x() - blitRegion.srcOffsets[0].x;
                    blitRegion.srcOffsets[1].x = srcBinding.extent.x() - blitRegion.srcOffsets[1].x;
                    break;
                default:
                    break;
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

        static SizeT AlignPixelRow(SizeT rowBytes, Int alignment) {
            const SizeT resolvedAlignment = static_cast<SizeT>(std::max(alignment, 1));
            return (rowBytes + resolvedAlignment - 1) & ~(resolvedAlignment - 1);
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
                const GLenum clampMode = MG_State::pGLContext->GetClampReadColor();
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
        : m_window(window), m_config(cfg) {
        // Initialize();
    }

    VulkanRenderer::~VulkanRenderer() {
        Shutdown();
    }

    inline ProgramFactory::CompileOptionFlags GetShaderTransformFlags(VkSurfaceTransformFlagBitsKHR preTransform) {
        ProgramFactory::CompileOptionFlags flags = ProgramFactory::CompileOptionBit::PositionZRemap;
        const auto& currentDrawFBO =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        if (currentDrawFBO != nullptr && currentDrawFBO->IsDefaultFramebuffer()) {
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
        CreateInstance();
        CreateSurface();
        PickPhysicalDevice();
        CreateLogicalDeviceAndQueues();
        CreateAllocator();

        CreateCommandPool();

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
                                                  m_physicalDevice.properties.limits.maxSamplerAnisotropy});
        MOBILEGL_ASSERT(succeeded, "VkSamplerManager initialization failed.");
        succeeded = InitializeBlitResources();
        MOBILEGL_ASSERT(succeeded, "Blit pipeline resource initialization failed.");
        succeeded = InitializeDepthMipmapResources();
        MOBILEGL_ASSERT(succeeded, "Depth mipmap pipeline resource initialization failed.");

        m_uniformManager = MakeUnique<UniformManager>();
        MOBILEGL_ASSERT(m_uniformManager != nullptr, "UniformDescriptorBinder creation failed.");
        succeeded = m_uniformManager->Initialize(
            m_device, &m_bufferManager, m_programFactory.get(),
            m_physicalDevice.properties.limits.minUniformBufferOffsetAlignment, m_config.MaxFramesInFlight,
            maxProgramBindings, kDescriptorSetsPerFrame, m_textureManager.get(), m_samplerManager.get());
        MOBILEGL_ASSERT(succeeded, "UniformDescriptorBinder initialization failed.");
        m_vertexInputStateFactory = MakeUnique<VertexInputStateFactory>(m_config, m_physicalDevice.handle);
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
        } else {
            MGLOG_W("DirectVulkan: no swapchain at initialization (zero-area window); deferring first acquire");
        }
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_convertedVertexStreams.clear();

        MGLOG_D("VulkanRenderer initialized");
    }

    void VulkanRenderer::Shutdown() {
        if (m_instance == VK_NULL_HANDLE && m_device == VK_NULL_HANDLE && m_surface == VK_NULL_HANDLE) {
            return;
        }

        if (m_device != VK_NULL_HANDLE) {
            VK_VERIFY(vkDeviceWaitIdle(m_device));
        }
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
        m_xfbCounterSlotByObject.clear();
        m_xfbNextCounterSlot = 0;
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

    // Scans the draw's index range from host-visible index bytes and returns the largest
    // fetchable vertex index. Usable only when the draw's range is exactly its
    // IndexBufferView (drawParams.indexRangeIsExactView). The view's byte offset is either
    // an offset into the bound element-array buffer or, with no bound buffer, a raw client
    // pointer. Primitive-restart sentinels are skipped so they cannot inflate the bound.
    static Bool TryComputeMaxIndexFromHostBytes(const MG_State::GLState::VertexArrayObject& vao,
                                                const IndexBufferView& indexView, Uint32& outMaxIndex) {
        SizeT indexSize = 0;
        switch (indexView.indexType) {
        case GL_UNSIGNED_BYTE: indexSize = 1; break;
        case GL_UNSIGNED_SHORT: indexSize = 2; break;
        case GL_UNSIGNED_INT: indexSize = 4; break;
        default: return false;
        }

        const Uint8* indexBytes = nullptr;
        const auto& indexBufferShared = indexView.forceClientMemory
            ? SharedPtr<MG_State::GLState::BufferObject>{}
            : vao.GetIndexBufferBindingSlot().GetBoundObject();
        if (indexBufferShared != nullptr) {
            const SizeT bufferSize = indexBufferShared->GetSize();
            if (indexBufferShared->MappedData() == nullptr || indexView.indexByteOffset > bufferSize ||
                indexView.indexByteSize > bufferSize - indexView.indexByteOffset) {
                return false;
            }
            // Recorded-but-unexecuted GPU writes (XFB capture, SSBO, storage texel
            // buffer) land in the coherent mapping this scan is about to read;
            // submit-and-wait first, exactly like the restart-index rewrite does.
            // A no-op unless the gpu-write flag is set.
            indexBufferShared->SyncGpuWrites();
            indexBufferShared->SyncPersistentMappedRange();
            indexBytes = indexBufferShared->MappedData() + indexView.indexByteOffset;
        } else {
            indexBytes = reinterpret_cast<const Uint8*>(indexView.indexByteOffset);
            if (indexBytes == nullptr) {
                return false;
            }
        }

        const SizeT indexCount = indexView.indexByteSize / indexSize;
        // The all-ones sentinel is only a restart marker when primitive restart is enabled;
        // with restart off it is a legitimate index and excluding it would truncate the
        // converted stream by exactly that vertex.
        const Bool primitiveRestartActive =
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestart) ||
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex);
        const Uint32 restartSentinel = indexSize == 1 ? 0xFFu : indexSize == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        Uint32 maxIndex = 0;
        Bool sawIndex = false;
        for (SizeT i = 0; i < indexCount; ++i) {
            Uint32 index = 0;
            switch (indexSize) {
            case 1: index = indexBytes[i]; break;
            case 2: index = reinterpret_cast<const Uint16*>(indexBytes)[i]; break;
            default: index = reinterpret_cast<const Uint32*>(indexBytes)[i]; break;
            }
            if (primitiveRestartActive && index == restartSentinel) {
                continue;
            }
            maxIndex = std::max(maxIndex, index);
            sawIndex = true;
        }
        if (!sawIndex) {
            return false;
        }
        outMaxIndex = maxIndex;
        return true;
    }

    Bool VulkanRenderer::TryBindResolvedVertexBindings(
        VkCommandBuffer commandBuffer, const MG_State::GLState::VertexArrayObject& vao,
        ResolvedVertexBindings& entry, Uint64 vaoContentHash, Uint32 activeAttribMask,
        Uint64 frameSerial) {
        // Layout + buffer identity in two loads from data the caller already has: the
        // VAO's content hash mixes every enabled attribute's format AND its bound
        // buffer's address (any change bumps the config version, invalidating the hash
        // memo the caller read), and the program's active-location mask fixes the
        // synthetic-binding set. Together they pin bindings.size(), every base offset
        // and which buffer each binding reads, so the hit path never has to resolve the
        // vertex-input factory entry at all - that chase was the dominant cost of a
        // VAO-cycling frame's memo hit.
        if (entry.frameSerial == 0 || entry.vertexInputHash != vaoContentHash ||
            entry.activeAttribMask != activeAttribMask) {
            return false;
        }

        if (entry.frameSerial == frameSerial) {
            // What is left to establish is that the buffers still hand back the slices
            // recorded here, and that none of them is a host map whose shadow needs
            // pushing down. An unmoved manager-wide epoch counter says both.
            if (!entry.anyBufferMapped && entry.sliceEpochCounter == m_bufferManager.GetSliceEpochCounter()) {
                ShadowedBindVertexBuffers(commandBuffer, entry.vkBuffers, entry.vkOffsets, entry.bindingCount);
                return true;
            }

            // Something moved somewhere; ask the buffers themselves.
            const auto& attributes = vao.GetAllAttributes();
            const MG_State::GLState::BufferObject* synced = nullptr;
            for (Uint32 binding = 0; binding < entry.bindingCount; ++binding) {
                auto* bufferObject = attributes[entry.attributeLocations[binding]].Buffer.get();
                if (bufferObject != entry.buffers[binding]) {
                    return false;
                }
                // What the resolving path does before every acquire: a persistent map the
                // backend could not adopt into coherent GPU storage mutates its shadow with
                // no API call, so the write range has to be pushed down here too. It is a
                // no-op for every buffer that is not such a map; when it is not, it dispatches
                // a SubData that retires the epoch below, and this draw resolves in full.
                if (bufferObject != synced) {
                    bufferObject->SyncPersistentMappedRange();
                    synced = bufferObject;
                }
                const auto* resource =
                    static_cast<const VkBufferResource*>(bufferObject->GetBackendResource().get());
                if (resource == nullptr || resource->sliceEpoch != entry.sliceEpochs[binding]) {
                    return false;
                }
            }

            ShadowedBindVertexBuffers(commandBuffer, entry.vkBuffers, entry.vkOffsets, entry.bindingCount);
            return true;
        }

        // NO cross-frame trust: a memo recorded in an earlier frame declines here and
        // the draw re-resolves through the full acquire path. The epoch-compare
        // revalidation that used to sit here shipped visible corruption (journeymap /
        // common-mods retraces, vertex anomalies on Adreno): the acquire path is the
        // frame's content-sync point, and skipping it across frames trusted the
        // BumpSliceEpoch inventory to cover every way a buffer's GPU copy can go stale.
        // At least one path escapes it. Until that inventory is proven complete the
        // hot layout memo above (same-frame) keeps the factory-chase win, and the
        // first draw of each (VAO, frame) pays one full resolve.
        return false;
    }

    VulkanRenderer::VaoDrawMemo* VulkanRenderer::LookupVaoDrawMemo(
        const MG_State::GLState::VertexArrayObject* vao) {
        if (m_vaoDrawMemoTable.empty()) {
            m_vaoDrawMemoTable.resize(kVaoDrawMemoSlotCount);
        }
        // Multiplicative mix of the (16-byte-aligned) address; take high bits, they
        // carry the most entropy of a multiply.
        const Uint64 mixed = static_cast<Uint64>(reinterpret_cast<SizeT>(vao) >> 4) * 0x9E3779B97F4A7C15ull;
        const Uint32 index = static_cast<Uint32>(mixed >> 32) & (kVaoDrawMemoSlotCount - 1);
        // The address still picks the slot (it is what the caller has in hand), but it is
        // the lifetime id that decides whether the slot is THIS object's: an address on
        // its own is recycled, and a slot matched on a recycled address hands the new VAO
        // the dead one's resolved bindings.
        const Uint64 lifetimeId = vao->GetLifetimeId();
        VaoDrawMemo& first = m_vaoDrawMemoTable[index];
        if (first.vaoKey == vao && first.vaoLifetimeId == lifetimeId) {
            return &first;
        }
        VaoDrawMemo& second = m_vaoDrawMemoTable[index ^ 1u];
        if (second.vaoKey == vao && second.vaoLifetimeId == lifetimeId) {
            return &second;
        }
        // Miss: recycle a slot. Prefer an empty one; otherwise evict the entry whose
        // bindings memo is older (its VAO is the one drawn less recently).
        VaoDrawMemo* victim = &first;
        if (first.vaoKey != nullptr &&
            (second.vaoKey == nullptr || second.bindings.frameSerial < first.bindings.frameSerial)) {
            victim = &second;
        }
        victim->vaoKey = vao;
        victim->vaoLifetimeId = lifetimeId;
        victim->contentHash = 0;
        victim->layoutFactsValid = false;
        // Unmatchable until a resolve completes (same rule as before: a bailed-out
        // resolve must never leave stale contents matchable).
        victim->bindings.frameSerial = 0;
        victim->bindings.indexFrameSerial = 0;
        victim->bindings.indexBuffer = nullptr;
        return victim;
    }

    Bool VulkanRenderer::UploadAndBindVertexBuffers(
        VkCommandBuffer commandBuffer, const MG_State::GLState::VertexArrayObject& vao,
        const ProgramFactory::VkProgramObject& programObj, const DrawCmdParam& drawParams,
        const IndexBufferView* pIndexBufferView) {
        static_assert(ResolvedVertexBindings::kMaxBindings == DynamicStateShadow::kMaxShadowedVertexBindings,
                      "the resolved-binding memo is sized to what the bind shadow can compare");
        const Bool indexedDraw = pIndexBufferView != nullptr;
        // Exclusive upper bound on the vertex-stream elements this draw can fetch through
        // vertex-rate bindings, or 0 when unbounded (indirect/multi draws). Computed lazily
        // because the index scan is only worth doing when a conversion actually needs it.
        SizeT drawElementBound = 0;
        Bool drawElementBoundComputed = false;
        auto resolveDrawElementBound = [&]() -> SizeT {
            if (drawElementBoundComputed) {
                return drawElementBound;
            }
            drawElementBoundComputed = true;
            if (!indexedDraw) {
                drawElementBound = static_cast<SizeT>(drawParams.firstVertex) + drawParams.vertexCount;
            } else if (drawParams.indexRangeIsExactView) {
                Uint32 maxIndex = 0;
                if (TryComputeMaxIndexFromHostBytes(vao, *pIndexBufferView, maxIndex)) {
                    drawElementBound = static_cast<SizeT>(maxIndex) + 1 +
                                       static_cast<SizeT>(std::max(drawParams.baseVertex, 0));
                }
            }
            return drawElementBound;
        };
        const Uint32 activeAttribMask = programObj.activeVertexInputLocationMask;

        const Uint64 frameSerial = m_bufferManager.GetFrameSerial();
        // Probe the memo BEFORE resolving the vertex-input entry: a hit needs nothing
        // from it (the VAO's own hash memo pins layout and buffers - see
        // TryBindResolvedVertexBindings), and skipping the resolve also skips its
        // per-draw cold chase into the factory's heap entry. The direct-mapped slot
        // lookup replaces the old pointer-keyed hash-map find, whose metadata and
        // key-storage probing was the dominant per-draw cost of a VAO-cycling frame.
        m_currentDrawResolvedEntry = nullptr;
        VaoDrawMemo* slot = nullptr;
        ResolvedVertexBindings* memo = nullptr;
        Uint64 vaoContentHash = 0;
        const Bool vaoHashKnown = vao.GetBackendHashMemo(vaoContentHash);
        if (vaoHashKnown) {
            slot = LookupVaoDrawMemo(&vao);
            memo = &slot->bindings;
            if (TryBindResolvedVertexBindings(commandBuffer, vao, *memo, vaoContentHash,
                                              activeAttribMask, frameSerial)) {
                m_currentDrawResolvedEntry = memo;
                return true;
            }
            // Whatever it described is stale; a resolve that bails out below must not
            // leave the old contents matchable either.
            memo->frameSerial = 0;
        }
        auto& vertexInputState = m_vertexInputStateFactory->GetOrCreateVertexInputState(vao);
        if (slot == nullptr) {
            // First sight since a config change: the factory resolve just stamped the
            // VAO's hash memo, so the slot can be claimed (and the facts below stored)
            // for every later draw of this configuration.
            slot = LookupVaoDrawMemo(&vao);
            memo = &slot->bindings;
            memo->frameSerial = 0;
        }
        // Refresh the layout facts served to TrySetupDrawFastPath. Pure values derived
        // from the content hash, so this is correct even for layouts whose BINDINGS are
        // not memoisable (client arrays, conversions).
        slot->contentHash = vertexInputState.hash;
        slot->layoutHash = vertexInputState.layoutHash;
        slot->layoutAuxMasks = VertexInputStateFactory::PackVertexInputAuxMasks(
            vertexInputState.unsupportedAttribMask, vertexInputState.attributeLocationMask);
        slot->layoutFactsValid = true;
        const Uint32 vertexInputAttribMask = vertexInputState.attributeLocationMask;
        const Uint32 missingAttribMask = activeAttribMask & ~vertexInputAttribMask;

        const auto bindingCount = vertexInputState.bindings.size() + static_cast<SizeT>(std::popcount(missingAttribMask));
        // Anything the memo cannot key on (see ResolvedVertexBindings) clears this as
        // the resolve below discovers it.
        Bool memoisable = missingAttribMask == 0 && bindingCount > 0 &&
                          bindingCount <= ResolvedVertexBindings::kMaxBindings;
        Bool anyBufferMapped = false;

        auto& vkBuffers = m_vertexBuffersScratch;
        auto& vkOffsets = m_vertexOffsetsScratch;
        vkBuffers.assign(bindingCount, VK_NULL_HANDLE);
        vkOffsets.assign(bindingCount, 0);

        auto uploadConvertedStream = [&](VertexInputStateFactory::VertexStreamConversion conversion,
                                         const MG_State::GLState::VertexAttribute& attribute,
                                         const Uint8* sourceData, SizeT sourceStride,
                                         SizeT elementSize, SizeT elementCount,
                                         BufferSlice& outSlice) -> Bool {
            // A resolved stride of 0 is the binding model's "never advance" (see the
            // factory's layout notes): exactly one element is converted and every vertex
            // reads it. That single element is read at offset 0, so the stride is never
            // actually used - but both converters reject 0 as a degenerate input, which
            // made the documented single-element conversion unreachable and silently
            // dropped every draw using such a binding. Substitute the element's own
            // size; the caller's cache key still carries the distinct stride 0.
            if (sourceStride == 0 && elementCount == 1) {
                sourceStride = elementSize;
            }
            const void* uploadData = nullptr;
            VkDeviceSize uploadSize = 0;
            switch (conversion) {
            case VertexInputStateFactory::VertexStreamConversion::Repack:
                if (!RepackVertexStream(sourceData, sourceStride, elementSize, elementCount,
                                        m_vertexRepackScratch)) {
                    return false;
                }
                uploadData = m_vertexRepackScratch.data();
                uploadSize = static_cast<VkDeviceSize>(m_vertexRepackScratch.size());
                break;
            case VertexInputStateFactory::VertexStreamConversion::ScaledIntegerToFloat32:
                if (!ConvertScaledIntegerVertexStreamToFloat32(attribute, sourceData, sourceStride,
                                                               elementCount, m_vertexConversionScratch)) {
                    return false;
                }
                uploadData = m_vertexConversionScratch.data();
                uploadSize = static_cast<VkDeviceSize>(m_vertexConversionScratch.size() * sizeof(Float));
                break;
            case VertexInputStateFactory::VertexStreamConversion::None:
                return false;
            }
            return uploadSize > 0 &&
                   m_bufferManager.UploadTransient(BufferKind::Vertex,
                                                   m_frameContext.GetCurrentFrameIndex(),
                                                   uploadData, uploadSize, 16, outSlice);
        };

        for (SizeT binding = 0; binding < bindingCount; ++binding) {
            if (binding >= vertexInputState.bindings.size()) {
                break;
            }
            const Uint32 bindingLocation = binding < vertexInputState.bindingAttributeLocations.size()
                                               ? vertexInputState.bindingAttributeLocations[binding]
                                               : static_cast<Uint32>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
            const Bool usesClientMemory = binding < vertexInputState.bindingUsesClientMemory.size() &&
                                          vertexInputState.bindingUsesClientMemory[binding];
            const auto conversion = binding < vertexInputState.bindingConversions.size()
                                        ? vertexInputState.bindingConversions[binding]
                                        : VertexInputStateFactory::VertexStreamConversion::None;
            if (usesClientMemory) {
                memoisable = false;
                const Uint32 location = bindingLocation;
                MOBILEGL_ASSERT(location < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS,
                                "UploadAndBindVertexStreams failed to resolve client attribute location");

                const auto& attr = vao.GetAttribute(location);
                const SizeT elementSize =
                    VertexInputStateFactory::GetAttributeByteSize(attr.Type, attr.Size, attr.IsBgra);
                const SizeT stride = attr.Stride > 0 ? static_cast<SizeT>(attr.Stride) : elementSize;
                const auto* clientData = reinterpret_cast<const Uint8*>(attr.Offset);
                if (!clientData || elementSize == 0 || stride == 0) {
                    MGLOG_E_ONCE("UploadAndBindVertexStreams skipped: invalid client vertex attribute at location %u", location);
                    return false;
                }

                // Client arrays have no queryable size, so bound the upload by the draw's
                // real fetch range. For indexed draws that means scanning the index bytes:
                // the guessed vertexCount (indexCount + baseVertex) can both truncate draws
                // whose max index exceeds their index count and over-read below it.
                const SizeT clientElementBound = resolveDrawElementBound();
                BufferSlice slice{};
                Bool uploaded = false;
                if (conversion == VertexInputStateFactory::VertexStreamConversion::None) {
                    const SizeT lastVertex =
                        clientElementBound > 0
                            ? clientElementBound - 1
                            : (drawParams.vertexCount > 0
                                   ? static_cast<SizeT>(drawParams.firstVertex) + drawParams.vertexCount - 1
                                   : static_cast<SizeT>(drawParams.firstVertex));
                    const SizeT uploadSize = lastVertex * stride + elementSize;
                    uploaded = m_bufferManager.UploadTransient(
                        BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(), clientData,
                        static_cast<VkDeviceSize>(uploadSize), 16, slice);
                } else {
                    if (clientElementBound == 0) {
                        // Indirect/multi indexed draws have no CPU-visible index range and a
                        // client array has no size to fall back to; a guessed range could
                        // truncate the converted stream, so skip the draw loudly.
                        MGLOG_E_ONCE("UploadAndBindVertexStreams skipped: converted client-memory attribute "
                                "location=%u has no computable vertex range", location);
                        return false;
                    }
                    uploaded = uploadConvertedStream(conversion, attr, clientData, stride, elementSize,
                                                     clientElementBound, slice);
                }
                if (!uploaded) {
                    MOBILEGL_ASSERT(false,
                                    "UploadAndBindVertexStreams skipped: failed to upload client attribute binding %zu",
                                    binding);
                    return false;
                }

                vkBuffers[binding] = slice.buffer;
                vkOffsets[binding] = slice.offset;
                continue;
            }

            // VertexInputStateFactory fills bindingBufferKeys[b] and bindingAttributeLocations[b]
            // from the SAME loop iteration, one binding per enabled attribute with no merging, so
            // this attribute's Buffer IS the SharedPtr by construction - no need to search the VAO's
            // 32 slots for it. The client-memory branch above has already returned, so the location
            // is in range here.
            const auto& sourceBufferShared = vao.GetAttribute(bindingLocation).Buffer;
            MOBILEGL_ASSERT(sourceBufferShared != nullptr,
                            "UploadAndBindVertexStreams failed to resolve source buffer");
            BufferSlice slice{};
            const SizeT sourceSize = sourceBufferShared->GetSize();
            const SizeT baseOffset =
                binding < vertexInputState.bindingBaseOffsets.size() ? vertexInputState.bindingBaseOffsets[binding] : 0;
            MOBILEGL_ASSERT(baseOffset <= sourceSize,
                            "UploadAndBindVertexStreams skipped: binding %zu base offset %zu exceeds buffer size %zu",
                            binding, baseOffset, sourceSize);

            if (conversion != VertexInputStateFactory::VertexStreamConversion::None) {
                memoisable = false;
                MOBILEGL_ASSERT(bindingLocation < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS,
                                "UploadAndBindVertexStreams failed to resolve converted attribute location");
                const auto& attr = vao.GetAttribute(bindingLocation);
                const SizeT elementSize =
                    VertexInputStateFactory::GetAttributeByteSize(attr.Type, attr.Size, attr.IsBgra);
                // Zero is a legal binding stride and means "never advance" (see
                // VertexAttribute::Stride), so it is NOT folded into the element size here -
                // it selects the single-element conversion below instead.
                const SizeT sourceStride = static_cast<SizeT>(attr.Stride);
                if (sourceBufferShared->MappedData() == nullptr || elementSize == 0 ||
                    baseOffset > sourceSize || elementSize > sourceSize - baseOffset) {
                    MGLOG_E_ONCE("UploadAndBindVertexStreams skipped: invalid converted source binding=%zu "
                            "location=%u base=%zu size=%zu element=%zu stride=%zu",
                            binding, bindingLocation, baseOffset, sourceSize, elementSize, sourceStride);
                    return false;
                }

                // A GPU-written source (XFB capture, SSBO, storage texel buffer) has its
                // bytes produced by commands that are merely RECORDED at this point, and
                // MappedData() aliases the coherent GPU memory they will write into -
                // converting now would read pre-write garbage. Submit-and-wait first,
                // mirroring the restart-index rewrite; a flag-test no-op otherwise.
                sourceBufferShared->SyncGpuWrites();
                sourceBufferShared->SyncPersistentMappedRange();
                const SizeT availableElementCount =
                    sourceStride == 0 ? 1 : 1 + (sourceSize - baseOffset - elementSize) / sourceStride;
                const Bool cacheable = !sourceBufferShared->IsBackendPersistentMapped();
                // Convert only what this draw can fetch instead of the whole buffer tail.
                // Instance-rate bindings index by instance, not the vertex range, so they
                // keep the tail. Indexed draws from cacheable buffers also keep the tail: a
                // single cached whole-range conversion per frame is cheaper than a per-draw
                // index scan. Persistent-mapped buffers are uncacheable and reconvert every
                // draw, so for them the scan plus bounded conversion is the cheaper trade.
                const Bool vertexRateBinding =
                    vertexInputState.bindings[binding].inputRate == VK_VERTEX_INPUT_RATE_VERTEX;
                SizeT elementCount = availableElementCount;
                if (vertexRateBinding && (!indexedDraw || !cacheable)) {
                    const SizeT elementBound = resolveDrawElementBound();
                    if (elementBound > 0) {
                        elementCount = std::min(elementCount, elementBound);
                    }
                }
                const ConvertedVertexStreamKey cacheKey{
                    .buffer = sourceBufferShared.get(),
                    .changeSerial = sourceBufferShared->GetChangeSerial(),
                    .baseOffset = baseOffset,
                    .sourceStride = static_cast<Uint32>(sourceStride),
                    .type = attr.Type,
                    .size = attr.Size,
                    .normalized = attr.Normalized,
                    .isInteger = attr.IsInteger,
                    .conversion = conversion,
                };

                Bool reusedCachedStream = false;
                if (cacheable) {
                    const auto cached = m_convertedVertexStreams.find(cacheKey);
                    // A cached conversion covering at least this draw's range is a strict
                    // prefix match: converted streams are tightly packed from element 0.
                    if (cached != m_convertedVertexStreams.end() &&
                        cached->second.elementCount >= elementCount) {
                        slice = cached->second.slice;
                        reusedCachedStream = true;
                    }
                }
                if (!reusedCachedStream) {
                    const Uint8* sourceData = sourceBufferShared->MappedData() + baseOffset;
                    if (!uploadConvertedStream(conversion, attr, sourceData, sourceStride,
                                               elementSize, elementCount, slice)) {
                        MGLOG_E_ONCE("UploadAndBindVertexStreams skipped: failed to convert binding=%zu location=%u",
                                binding, bindingLocation);
                        return false;
                    }
                    if (cacheable) {
                        m_convertedVertexStreams[cacheKey] =
                            ConvertedVertexStream{slice, elementCount, sourceBufferShared};
                    }
                }
                vkBuffers[binding] = slice.buffer;
                vkOffsets[binding] = slice.offset;
                continue;
            }

            if (ShouldUseTransientVertexIndexBuffer(*sourceBufferShared)) {
                if (!m_bufferManager.AcquireStreamedSlice(BufferKind::Vertex, sourceBufferShared, slice)) {
                    MOBILEGL_ASSERT(false, "UploadAndBindVertexStreams skipped: failed to upload transient binding %zu", binding);
                    return false;
                }
            } else {
                if (!m_bufferManager.AcquireResidentSlice(BufferKind::Vertex, sourceBufferShared, slice)) {
                    MGLOG_E_ONCE("UploadAndBindVertexStreams skipped: failed to sync resident binding %zu", binding);
                    return false;
                }
            }
            vkBuffers[binding] = slice.buffer;
            vkOffsets[binding] = slice.offset + static_cast<VkDeviceSize>(baseOffset);
            // bindingAttributeLocations carries MAX_VERTEX_ATTRIBS as its "no location"
            // sentinel; GetAttribute() folds that to an empty attribute but the memo
            // indexes the raw array, so such a binding is not memoisable.
            memoisable = memoisable && bindingLocation < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS;
            if (memoisable) {
                // memo is always non-null here: the slot was claimed (and its serial
                // zeroed) before the resolve started.
                memo->attributeLocations[binding] = static_cast<Uint8>(bindingLocation);
                memo->buffers[binding] = sourceBufferShared.get();
                // Read after the acquire: it is the acquire that creates the resource
                // and mints the epoch this slice belongs to.
                const auto* resource =
                    static_cast<const VkBufferResource*>(sourceBufferShared->GetBackendResource().get());
                memo->sliceEpochs[binding] = resource != nullptr ? resource->sliceEpoch : 0;
                anyBufferMapped = anyBufferMapped || sourceBufferShared->IsMapped();
            }
        }

        SizeT syntheticBinding = vertexInputState.bindings.size();
        for (Uint32 location = 0; location < kMaxVertexAttribs; ++location) {
            if ((missingAttribMask & (1u << location)) == 0) {
                continue;
            }

            const auto glType = programObj.vertexInputTypes[location];
            const auto& currentValue = MG_State::pGLContext->GetCurrentVertexAttribute(location);
            VkFormat format = VK_FORMAT_UNDEFINED;
            const void* sourceData = nullptr;
            VkDeviceSize sourceSize = 0;
            const Bool supported = TryGetCurrentVertexAttributeUploadPayload(currentValue, glType, format,
                                                                            sourceData, sourceSize);
            if (!supported) {
                // SetupDraw's pre-flight should have rejected this already; never upload a null payload.
                MGLOG_E_ONCE("UploadAndBindVertexStreams skipped: unsupported current generic vertex attribute type: "
                        "programHash=%llu location=%u type=0x%x",
                        static_cast<unsigned long long>(programObj.hash), location, glType);
                return false;
            }

            BufferSlice slice{};
            if (!m_bufferManager.UploadTransient(BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(),
                                                 sourceData, sourceSize, 16, slice)) {
                MOBILEGL_ASSERT(false,
                                "UploadAndBindVertexStreams skipped: failed to upload current attribute binding for location %u",
                                location);
                return false;
            }

            vkBuffers[syntheticBinding] = slice.buffer;
            vkOffsets[syntheticBinding] = slice.offset;
            ++syntheticBinding;
        }

        if (bindingCount > 0) {
            const Uint32 count = static_cast<Uint32>(bindingCount);
            if (memoisable && memo != nullptr) {
                std::copy_n(vkBuffers.data(), count, memo->vkBuffers);
                std::copy_n(vkOffsets.data(), count, memo->vkOffsets);
                // The factory keys entries on the VAO content hash, so this is the same
                // value the hit path reads back from the VAO's own hash memo.
                memo->vertexInputHash = vertexInputState.hash;
                memo->activeAttribMask = activeAttribMask;
                memo->bindingCount = count;
                memo->anyBufferMapped = anyBufferMapped;
                // Read after every acquire above, so it covers the epochs they minted.
                memo->sliceEpochCounter = m_bufferManager.GetSliceEpochCounter();
                // Published last: the entry is only matchable once every field above is
                // the one this completed resolve produced.
                memo->frameSerial = frameSerial;
                // The same draw's UploadAndBindIndexBuffer may extend this entry with the
                // EBO slice memo; the pointer dies at the map's next insert (next draw).
                m_currentDrawResolvedEntry = memo;
            }
            ShadowedBindVertexBuffers(commandBuffer, vkBuffers.data(), vkOffsets.data(), count);
        }
        return true;
    }

    namespace {
        // Copies index data, replacing every occurrence of the application's arbitrary restart
        // index with the fixed all-ones value of the index type - the only one Vulkan restarts
        // on. An index that already equals the fixed value would then be indistinguishable from
        // a restart, so it is nudged to the next-lowest value: it can only be a real index (the
        // application's restart index is a different number), and the vertex it selects is
        // outside any well-defined draw anyway, whereas leaving it alone would tear the
        // primitive in two.
        void RewriteRestartIndices(const void* source, SizeT sizeBytes, VkIndexType indexType,
                                   Uint32 applicationRestartIndex, Vector<Uint8>& output) {
            output.resize(sizeBytes);
            if (sizeBytes == 0 || source == nullptr) {
                return;
            }
            Memcpy(output.data(), source, sizeBytes);
            const auto rewrite = [&](auto* indices, auto fixedMax) {
                const SizeT count = sizeBytes / sizeof(*indices);
                for (SizeT i = 0; i < count; ++i) {
                    if (indices[i] == static_cast<decltype(fixedMax)>(applicationRestartIndex)) {
                        indices[i] = fixedMax;
                    } else if (indices[i] == fixedMax) {
                        indices[i] = fixedMax - 1;
                    }
                }
            };
            switch (indexType) {
            case VK_INDEX_TYPE_UINT8:
                rewrite(reinterpret_cast<Uint8*>(output.data()), static_cast<Uint8>(0xFFu));
                break;
            case VK_INDEX_TYPE_UINT16:
                rewrite(reinterpret_cast<Uint16*>(output.data()), static_cast<Uint16>(0xFFFFu));
                break;
            case VK_INDEX_TYPE_UINT32:
                rewrite(reinterpret_cast<Uint32*>(output.data()), static_cast<Uint32>(0xFFFFFFFFu));
                break;
            default:
                break;
            }
        }
    } // namespace

    Bool VulkanRenderer::UploadAndBindIndexBuffer(FrameContext::FrameData& frame,
                                                  const MG_State::GLState::VertexArrayObject& vao,
                                                  const IndexBufferView* pIndexBufferView) {
        VkIndexType vkIndexType = VK_INDEX_TYPE_MAX_ENUM;
        switch (pIndexBufferView->indexType) {
        case GL_UNSIGNED_BYTE:
            MOBILEGL_ASSERT(m_indexTypeUint8ExtensionEnabled,
                            "DrawElements with GL_UNSIGNED_BYTE requires VK_KHR_index_type_uint8 or VK_EXT_index_type_uint8");
            vkIndexType = VK_INDEX_TYPE_UINT8;
            break;
        case GL_UNSIGNED_SHORT:
            vkIndexType = VK_INDEX_TYPE_UINT16;
            break;
        case GL_UNSIGNED_INT:
            vkIndexType = VK_INDEX_TYPE_UINT32;
            break;
        default:
            MGLOG_D("DrawElements skipped: index type %u is not supported yet", pIndexBufferView->indexType);
            return false;
        }

        // GL_PRIMITIVE_RESTART uses an arbitrary restart index (glPrimitiveRestartIndex), but Vulkan
        // only restarts on the fixed all-ones value of the index type. GL_PRIMITIVE_RESTART_FIXED_INDEX
        // already matches that, so only the arbitrary form needs handling: rewrite the indices into a
        // transient copy where the application's restart index becomes the fixed one.
        Uint32 substituteRestartIndex = 0;
        Bool substituteRestart = false;
        // One bulk parameters fetch instead of up to three accessor calls per indexed
        // draw; all three inputs are pure reads of these fields.
        const RenderStateParameters& rsp = MG_State::pGLContext->GetRenderStateParameters();
        if (rsp.PrimitiveRestartEnabled && !rsp.PrimitiveRestartFixedIndexEnabled) {
            const Uint32 restartIndex = rsp.PrimitiveRestartIndex;
            Uint32 fixedMax = 0;
            switch (vkIndexType) {
            case VK_INDEX_TYPE_UINT8: fixedMax = 0xFFu; break;
            case VK_INDEX_TYPE_UINT16: fixedMax = 0xFFFFu; break;
            case VK_INDEX_TYPE_UINT32: fixedMax = 0xFFFFFFFFu; break;
            default: break;
            }
            substituteRestart = restartIndex != fixedMax;
            substituteRestartIndex = restartIndex;
        }

        // Bound by reference so the SharedPtr below is the one already in hand rather than a fresh
        // GL-name map lookup plus an atomic refcount pair on every indexed draw - the vertex path
        // above documents the same cost.
        const SharedPtr<MG_State::GLState::BufferObject>& indexBufferShared =
            vao.GetIndexBufferBindingSlot().GetBoundObject();
        const auto* indexBuffer = pIndexBufferView->forceClientMemory ? nullptr : indexBufferShared.get();
        if (indexBuffer == nullptr) {
            // No element-array buffer: the view's byte offset is a raw client pointer
            // (desktop drivers accept client-memory indices and the GL CTS relies on
            // this even in core contexts). Snapshot the data into a transient slice.
            const auto* clientIndices = reinterpret_cast<const void*>(pIndexBufferView->indexByteOffset);
            if (clientIndices == nullptr || pIndexBufferView->indexByteSize == 0) {
                MGLOG_E_ONCE("DrawElements skipped: no element array buffer bound and no client index data");
                return false;
            }
            Vector<Uint8> rewrittenIndices;
            const void* uploadSource = clientIndices;
            if (substituteRestart) {
                RewriteRestartIndices(clientIndices, pIndexBufferView->indexByteSize, vkIndexType,
                                      substituteRestartIndex, rewrittenIndices);
                uploadSource = rewrittenIndices.data();
            }
            BufferSlice slice{};
            if (!m_bufferManager.UploadTransient(BufferKind::Index, m_frameContext.GetCurrentFrameIndex(),
                                                 uploadSource, pIndexBufferView->indexByteSize, 4, slice)) {
                MGLOG_E_ONCE("DrawElements skipped: failed to upload client index data");
                return false;
            }
            auto& shadow = g_dynamicStateShadow;
            if (!shadow.indexBindValid || shadow.indexBuffer != slice.buffer ||
                shadow.indexOffset != slice.offset || shadow.indexType != vkIndexType) {
                vkCmdBindIndexBuffer(frame.commandBuffer, slice.buffer, slice.offset, vkIndexType);
                shadow.indexBindValid = true;
                shadow.indexBuffer = slice.buffer;
                shadow.indexOffset = slice.offset;
                shadow.indexType = vkIndexType;
            }
            return true;
        }
        const SizeT indexDataSizeBytes = pIndexBufferView->indexByteSize;
        MOBILEGL_ASSERT(pIndexBufferView->indexByteOffset + indexDataSizeBytes <= indexBuffer->GetSize(),
                        "DrawElements index range out of bounds");

        // EBO slice memo (see ResolvedVertexBindings): skips the per-draw
        // AcquireResidentSlice when the live bound EBO and its resource epoch still
        // match what the recording draw resolved. Restart substitution re-uploads per
        // draw and never stores a memo, so a hit requires it off. The index TYPE is not
        // memo state: it flows from the draw's view into the shadowed bind below.
        ResolvedVertexBindings* indexMemo = m_currentDrawResolvedEntry;
        if (indexMemo != nullptr && !substituteRestart && indexMemo->indexFrameSerial != 0 &&
            indexMemo->indexBuffer == indexBuffer) {
            // One-compare rescue first (mirrors TryBindResolvedVertexBindings): the
            // use-serial was stamped this frame and the manager-wide slice-epoch
            // counter has not moved, so no buffer anywhere - this EBO included -
            // changed its slice or gained a host map since the epoch was verified.
            // Skips the per-draw GetBackendResource chase into a cold resource object.
            Bool sliceStillValid = false;
            const Uint64 frameSerial = m_bufferManager.GetFrameSerial();
            if (indexMemo->indexFrameSerial == frameSerial && !indexMemo->indexBufferMapped &&
                indexMemo->indexSliceEpochCounter == m_bufferManager.GetSliceEpochCounter()) {
                sliceStillValid = true;
            }
            // NO cross-frame trust for the EBO either (same corruption class as the
            // vertex half, see TryBindResolvedVertexBindings): a memo from an earlier
            // frame declines and the draw re-runs the acquire, which is the sync point.
            if (sliceStillValid) {
                const VkDeviceSize memoBindOffset = indexMemo->indexSliceOffset +
                    static_cast<VkDeviceSize>(pIndexBufferView->indexByteOffset);
                auto& shadow = g_dynamicStateShadow;
                if (!shadow.indexBindValid || shadow.indexBuffer != indexMemo->indexVkBuffer ||
                    shadow.indexOffset != memoBindOffset || shadow.indexType != vkIndexType) {
                    vkCmdBindIndexBuffer(frame.commandBuffer, indexMemo->indexVkBuffer, memoBindOffset,
                                         vkIndexType);
                    shadow.indexBindValid = true;
                    shadow.indexBuffer = indexMemo->indexVkBuffer;
                    shadow.indexOffset = memoBindOffset;
                    shadow.indexType = vkIndexType;
                }
                return true;
            }
        }

        BufferSlice slice{};
        MOBILEGL_ASSERT(indexBufferShared != nullptr, "UploadAndBindIndexBuffer failed to resolve shared EBO");
        if (substituteRestart) {
            // The whole buffer is rewritten, not just this draw's range, so that every element
            // index keeps its position: an indirect draw's firstIndex lives in GPU memory and
            // cannot be adjusted from here.
            indexBufferShared->SyncGpuWrites();
            Vector<Uint8> rewrittenIndices;
            RewriteRestartIndices(indexBufferShared->MappedData(), indexBufferShared->GetSize(), vkIndexType,
                                  substituteRestartIndex, rewrittenIndices);
            if (!m_bufferManager.UploadTransient(BufferKind::Index, m_frameContext.GetCurrentFrameIndex(),
                                                 rewrittenIndices.data(), rewrittenIndices.size(), 4, slice)) {
                MGLOG_E_ONCE("DrawElements skipped: failed to upload restart-substituted index data");
                return false;
            }
        } else if (ShouldUseTransientVertexIndexBuffer(*indexBufferShared)) {
            MOBILEGL_ASSERT(indexBufferShared->GetSize() != 0, "DrawElements requires non-empty EBO data");
            if (!m_bufferManager.AcquireStreamedSlice(BufferKind::Index, indexBufferShared, slice)) {
                MOBILEGL_ASSERT(false, "DrawElements skipped: failed to prepare transient index buffer");
                return false;
            }
        } else if (!m_bufferManager.AcquireResidentSlice(BufferKind::Index, indexBufferShared, slice)) {
            MGLOG_E_ONCE("DrawElements skipped: failed to sync resident index buffer");
            return false;
        } else if (indexMemo != nullptr && !substituteRestart) {
            // Resident acquire succeeded: record the slice for the next draw of this VAO.
            // Read the epoch AFTER the acquire - it is the acquire that mints the epoch
            // this slice belongs to.
            const auto* resource = static_cast<const VkBufferResource*>(
                indexBufferShared->GetBackendResource().get());
            if (resource != nullptr) {
                indexMemo->indexBuffer = indexBuffer;
                indexMemo->indexSliceEpoch = resource->sliceEpoch;
                // Read after the acquire for the same reason as the epoch: the acquire
                // may have bumped the manager-wide counter minting this very epoch.
                indexMemo->indexSliceEpochCounter = m_bufferManager.GetSliceEpochCounter();
                indexMemo->indexVkBuffer = slice.buffer;
                indexMemo->indexSliceOffset = slice.offset;
                indexMemo->indexFrameSerial = m_bufferManager.GetFrameSerial();
                // A host-mapped EBO can mutate its shadow with no epoch bump; the hit
                // path declines on this flag (mirror of anyBufferMapped).
                indexMemo->indexBufferMapped = indexBufferShared->IsMapped();
            }
        }
        const VkDeviceSize indexBindOffset =
            slice.offset + static_cast<VkDeviceSize>(pIndexBufferView->indexByteOffset);
        auto& shadow = g_dynamicStateShadow;
        if (!shadow.indexBindValid || shadow.indexBuffer != slice.buffer ||
            shadow.indexOffset != indexBindOffset || shadow.indexType != vkIndexType) {
            vkCmdBindIndexBuffer(frame.commandBuffer, slice.buffer, indexBindOffset, vkIndexType);
            shadow.indexBindValid = true;
            shadow.indexBuffer = slice.buffer;
            shadow.indexOffset = indexBindOffset;
            shadow.indexType = vkIndexType;
        }
        return true;
    }

    Bool VulkanRenderer::InitializeBlitResources() {
        ShutdownBlitResources();

        auto vertexShader = MakeShared<MG_State::GLState::ShaderObject>(ShaderStage::Vertex, kHiddenBlitVertexShaderId);
        vertexShader->SetShaderSource(kFullscreenTriangleVertexShaderSource);
        vertexShader->Compile();
        if (!vertexShader->GetCompileStatus()) {
            MGLOG_E("InitializeBlitResources failed: vertex shader compile error: %s", vertexShader->GetInfoLog().c_str());
            return false;
        }

        auto fragmentShader = MakeShared<MG_State::GLState::ShaderObject>(ShaderStage::Fragment, kHiddenBlitFragmentShaderId);
        fragmentShader->SetShaderSource(kBlitFragmentShaderSource);
        fragmentShader->Compile();
        if (!fragmentShader->GetCompileStatus()) {
            MGLOG_E("InitializeBlitResources failed: fragment shader compile error: %s", fragmentShader->GetInfoLog().c_str());
            return false;
        }

        m_blitResources.program = MakeShared<MG_State::GLState::ProgramObject>(kHiddenBlitProgramId);
        m_blitResources.program->AttachShader(vertexShader);
        m_blitResources.program->AttachShader(fragmentShader);
        m_blitResources.program->Link(false);
        if (!m_blitResources.program->GetLinkStatus()) {
            MGLOG_E("InitializeBlitResources failed: program link error: %s", m_blitResources.program->GetInfoLog().c_str());
            return false;
        }

        m_blitResources.srcRectLocation = m_blitResources.program->GetUniformLocation("uSrcRect");
        m_blitResources.dstRectLocation = m_blitResources.program->GetUniformLocation("uDstRect");
        m_blitResources.surfaceTransformLocation = m_blitResources.program->GetUniformLocation("uSurfaceTransform");
        MOBILEGL_ASSERT(m_blitResources.srcRectLocation >= 0, "InitializeBlitResources: missing uSrcRect");
        MOBILEGL_ASSERT(m_blitResources.dstRectLocation >= 0, "InitializeBlitResources: missing uDstRect");
        MOBILEGL_ASSERT(m_blitResources.surfaceTransformLocation >= 0,
                        "InitializeBlitResources: missing uSurfaceTransform");
        MOBILEGL_ASSERT(m_blitResources.program->GetUBOSize() > 0,
                        "InitializeBlitResources: blit program global UBO is empty");
        MOBILEGL_ASSERT(m_programFactory != nullptr, "InitializeBlitResources: program factory is null");

        ProgramFactory::CompileOptionFlags blitTransformFlags = 0;
        const auto& blitProgramObj = m_programFactory->GetOrCreateProgram(*m_blitResources.program, blitTransformFlags);
        Bool foundBlitSamplerBinding = false;
        for (Uint32 binding = 0; binding < blitProgramObj.samplerNameByBinding.size(); ++binding) {
            if (blitProgramObj.bindingKinds[binding] != ProgramFactory::DescriptorBindingKind::CombinedImageSampler) {
                continue;
            }
            if (blitProgramObj.samplerNameByBinding[binding] == "uSource") {
                m_blitResources.samplerBinding = binding;
                foundBlitSamplerBinding = true;
                break;
            }
        }
        MOBILEGL_ASSERT(foundBlitSamplerBinding,
                        "InitializeBlitResources: failed to resolve reflected binding for uSource");

        auto createSampler = [](Uint externalIndex, SamplerFilterMode filter) {
            auto sampler = MakeShared<MG_State::GLState::SamplerObject>(externalIndex);
            sampler->SetWrapS(SamplerWrapMode::ClampToEdge);
            sampler->SetWrapT(SamplerWrapMode::ClampToEdge);
            sampler->SetWrapR(SamplerWrapMode::ClampToEdge);
            sampler->SetMinFilter(filter);
            sampler->SetMagFilter(filter);
            sampler->SetMipmapMode(SamplerMipmapMode::None);
            sampler->SetLodRange(0.0f, 0.0f);
            return sampler;
        };

        m_blitResources.nearestSampler = createSampler(kHiddenBlitNearestSamplerId, SamplerFilterMode::Nearest);
        m_blitResources.linearSampler = createSampler(kHiddenBlitLinearSamplerId, SamplerFilterMode::Linear);
        return true;
    }

    void VulkanRenderer::ShutdownBlitResources() {
        m_blitResources = {};
    }

    Bool VulkanRenderer::InitializeDepthMipmapResources() {
        ShutdownDepthMipmapResources();

        auto vertexShader = MakeShared<MG_State::GLState::ShaderObject>(ShaderStage::Vertex,
                                                                         kHiddenDepthMipmapVertexShaderId);
        vertexShader->SetShaderSource(kFullscreenTriangleVertexShaderSource);
        vertexShader->Compile();
        if (!vertexShader->GetCompileStatus()) {
            MGLOG_E("InitializeDepthMipmapResources failed: vertex shader compile error: %s",
                    vertexShader->GetInfoLog().c_str());
            return false;
        }

        auto fragmentShader = MakeShared<MG_State::GLState::ShaderObject>(ShaderStage::Fragment,
                                                                           kHiddenDepthMipmapFragmentShaderId);
        fragmentShader->SetShaderSource(kDepthMipmapFragmentShaderSource);
        fragmentShader->Compile();
        if (!fragmentShader->GetCompileStatus()) {
            MGLOG_E("InitializeDepthMipmapResources failed: fragment shader compile error: %s",
                    fragmentShader->GetInfoLog().c_str());
            return false;
        }

        m_depthMipmapResources.program = MakeShared<MG_State::GLState::ProgramObject>(kHiddenDepthMipmapProgramId);
        m_depthMipmapResources.program->AttachShader(vertexShader);
        m_depthMipmapResources.program->AttachShader(fragmentShader);
        m_depthMipmapResources.program->Link(false);
        if (!m_depthMipmapResources.program->GetLinkStatus()) {
            MGLOG_E("InitializeDepthMipmapResources failed: program link error: %s",
                    m_depthMipmapResources.program->GetInfoLog().c_str());
            return false;
        }

        m_depthMipmapResources.srcRectLocation = m_depthMipmapResources.program->GetUniformLocation("uSrcRect");
        m_depthMipmapResources.dstRectLocation = m_depthMipmapResources.program->GetUniformLocation("uDstRect");
        m_depthMipmapResources.surfaceTransformLocation =
            m_depthMipmapResources.program->GetUniformLocation("uSurfaceTransform");
        m_depthMipmapResources.srcTexelSizeLocation =
            m_depthMipmapResources.program->GetUniformLocation("uSrcTexelSize");
        MOBILEGL_ASSERT(m_depthMipmapResources.srcRectLocation >= 0,
                        "InitializeDepthMipmapResources: missing uSrcRect");
        MOBILEGL_ASSERT(m_depthMipmapResources.dstRectLocation >= 0,
                        "InitializeDepthMipmapResources: missing uDstRect");
        MOBILEGL_ASSERT(m_depthMipmapResources.surfaceTransformLocation >= 0,
                        "InitializeDepthMipmapResources: missing uSurfaceTransform");
        MOBILEGL_ASSERT(m_depthMipmapResources.srcTexelSizeLocation >= 0,
                        "InitializeDepthMipmapResources: missing uSrcTexelSize");
        MOBILEGL_ASSERT(m_depthMipmapResources.program->GetUBOSize() > 0,
                        "InitializeDepthMipmapResources: depth mipmap program global UBO is empty");
        MOBILEGL_ASSERT(m_programFactory != nullptr, "InitializeDepthMipmapResources: program factory is null");

        ProgramFactory::CompileOptionFlags transformFlags = 0;
        const auto& programObj =
            m_programFactory->GetOrCreateProgram(*m_depthMipmapResources.program, transformFlags);
        Bool foundSamplerBinding = false;
        for (Uint32 binding = 0; binding < programObj.samplerNameByBinding.size(); ++binding) {
            if (programObj.bindingKinds[binding] != ProgramFactory::DescriptorBindingKind::CombinedImageSampler) {
                continue;
            }
            if (programObj.samplerNameByBinding[binding] == "uSource") {
                m_depthMipmapResources.samplerBinding = binding;
                foundSamplerBinding = true;
                break;
            }
        }
        MOBILEGL_ASSERT(foundSamplerBinding,
                        "InitializeDepthMipmapResources: failed to resolve reflected binding for uSource");
        return true;
    }

    void VulkanRenderer::ShutdownDepthMipmapResources() {
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

    VkPipeline VulkanRenderer::GetOrCreateBlitPipeline(const RenderPassEntry& renderPassEntry) {
        MOBILEGL_ASSERT(m_blitResources.program != nullptr, "GetOrCreateBlitPipeline: blit program is null");
        MOBILEGL_ASSERT(m_programFactory != nullptr, "GetOrCreateBlitPipeline: program factory is null");
        MOBILEGL_ASSERT(m_uniformManager != nullptr, "GetOrCreateBlitPipeline: descriptor binder is null");

        static const VkPipelineVertexInputStateCreateInfo kEmptyVertexInputState {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        };
        ProgramFactory::CompileOptionFlags transformFlags = 0;
        const auto& programObj = m_programFactory->GetOrCreateProgram(*m_blitResources.program, transformFlags);
        PipelineFactory::PipelineCreatePayload payload{
            .programHash = programObj.hash,
            .vertexInputHash = 0,
            .pipelineLayout = programObj.pipelineLayout,
            .renderPass = renderPassEntry.renderPass,
            .colorAttachmentCount = renderPassEntry.colorAttachmentCount,
            .rasterizationSamples = renderPassEntry.sampleCount,
            .subpass = 0,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            // Functionally irrelevant to the blit (no flat varying, no capture), but on a device with
            // provokingVertexModePerPipeline == VK_FALSE a blit pipeline left on FIRST inside a render
            // pass whose draw pipelines are LAST is an illegal mix. Note this does NOT cover
            // GenerateDepthMipmapWithShader, which builds its pipeline directly and keeps Vulkan's
            // FIRST - legal only because it creates and begins its own render pass. Anything that ever
            // records that pipeline inside an outer render pass must route through this selector too.
            .provokingVertexMode = SelectProvokingVertexMode(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false),
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .stages = &programObj.stages,
            .vertexInputState = &kEmptyVertexInputState,
            .stageSpirvDigests = &programObj.stageSpirvDigests
        };
        static constexpr VkColorComponentFlags kColorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        MOBILEGL_ASSERT(payload.colorAttachmentCount <= PipelineFactory::PipelineCreatePayload::kMaxColorAttachments,
                        "GetOrCreateBlitPipeline: colorAttachmentCount=%u exceeds payload capacity",
                        payload.colorAttachmentCount);
        for (Uint32 i = 0; i < payload.colorAttachmentCount; ++i) {
            payload.colorBlendAttachments[i] = MakeColorBlendAttachmentState(
                false,
                VK_BLEND_FACTOR_ONE,
                VK_BLEND_FACTOR_ZERO,
                VK_BLEND_OP_ADD,
                VK_BLEND_FACTOR_ONE,
                VK_BLEND_FACTOR_ZERO,
                VK_BLEND_OP_ADD,
                kColorWriteMask);
        }
        return m_pipelineFactory->GetOrCreatePipeline(payload);
    }

    Bool VulkanRenderer::GenerateDepthMipmapWithShader(FrameContext::FrameData& frame,
                                                       MG_State::GLState::ITextureObject& texture,
                                                       VkTextureManager::TextureResource& resource,
                                                       Uint32 baseMipLevel,
                                                       Uint32 generateMipLevelCount,
                                                       const IntVec3& storageBaseTexelSize,
                                                       VkImageLayout originalLayout,
                                                       VkImageLayout finalLayout) {
        MOBILEGL_ASSERT(m_depthMipmapResources.program != nullptr,
                        "GenerateDepthMipmapWithShader: depth mipmap program is null");
        MOBILEGL_ASSERT(m_blitResources.nearestSampler != nullptr,
                        "GenerateDepthMipmapWithShader: helper sampler is null");
        MOBILEGL_ASSERT(m_programFactory != nullptr, "GenerateDepthMipmapWithShader: program factory is null");
        MOBILEGL_ASSERT(m_uniformManager != nullptr, "GenerateDepthMipmapWithShader: uniform manager is null");
        MOBILEGL_ASSERT(texture.GetTarget() == TextureTarget::Texture2D,
                        "GenerateDepthMipmapWithShader only supports GL_TEXTURE_2D depth textures");
        MOBILEGL_ASSERT(resource.aspect == VK_IMAGE_ASPECT_DEPTH_BIT,
                        "GenerateDepthMipmapWithShader requires a depth-only aspect");
        MOBILEGL_ASSERT(resource.depth == 1 && resource.arrayLayers == 1,
                        "GenerateDepthMipmapWithShader only supports single-layer depth textures");
        MOBILEGL_ASSERT(m_frameContext.GetCurrentFrameIndex() < m_deferredDepthMipmapCleanup.size(),
                        "GenerateDepthMipmapWithShader: frame index %u out of range (cleanup slots=%zu)",
                        m_frameContext.GetCurrentFrameIndex(), m_deferredDepthMipmapCleanup.size());

        auto& deferredCleanup = m_deferredDepthMipmapCleanup[m_frameContext.GetCurrentFrameIndex()];

        ProgramFactory::CompileOptionFlags transformFlags = 0;
        const auto& programObj =
            m_programFactory->GetOrCreateProgram(*m_depthMipmapResources.program, transformFlags);

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = resource.format;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 0;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpassDesc{};
        subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDesc.pDepthStencilAttachment = &depthAttachmentRef;

        VkRenderPassCreateInfo renderPassCreateInfo{};
        renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.attachmentCount = 1;
        renderPassCreateInfo.pAttachments = &depthAttachment;
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpassDesc;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateRenderPass(m_device, &renderPassCreateInfo, nullptr, &renderPass),
                  "GenerateDepthMipmapWithShader: vkCreateRenderPass");

        static const VkPipelineVertexInputStateCreateInfo kEmptyVertexInputState {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        };

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizationState{};
        rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizationState.cullMode = VK_CULL_MODE_NONE;
        rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizationState.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampleState{};
        multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencilState{};
        depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencilState.depthTestEnable = VK_TRUE;
        depthStencilState.depthWriteEnable = VK_TRUE;
        depthStencilState.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        depthStencilState.minDepthBounds = 0.0f;
        depthStencilState.maxDepthBounds = 1.0f;

        VkPipelineColorBlendStateCreateInfo colorBlendState{};
        colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<Uint32>(sizeof(dynamicStates) / sizeof(dynamicStates[0]));
        dynamicState.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineCreateInfo{};
        pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineCreateInfo.stageCount = static_cast<Uint32>(programObj.stages.size());
        pipelineCreateInfo.pStages = programObj.stages.data();
        pipelineCreateInfo.pVertexInputState = &kEmptyVertexInputState;
        pipelineCreateInfo.pInputAssemblyState = &inputAssembly;
        pipelineCreateInfo.pViewportState = &viewportState;
        pipelineCreateInfo.pRasterizationState = &rasterizationState;
        pipelineCreateInfo.pMultisampleState = &multisampleState;
        pipelineCreateInfo.pDepthStencilState = &depthStencilState;
        pipelineCreateInfo.pColorBlendState = &colorBlendState;
        pipelineCreateInfo.pDynamicState = &dynamicState;
        pipelineCreateInfo.layout = programObj.pipelineLayout;
        pipelineCreateInfo.renderPass = renderPass;
        pipelineCreateInfo.subpass = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &pipeline),
                  "GenerateDepthMipmapWithShader: vkCreateGraphicsPipelines");
        deferredCleanup.renderPasses.push_back(renderPass);
        deferredCleanup.pipelines.push_back(pipeline);

        auto createMipView = [&](Uint32 mipLevel, VkImageAspectFlags aspectMask) {
            VkImageViewCreateInfo viewCreateInfo{};
            viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewCreateInfo.image = resource.image;
            viewCreateInfo.viewType = resource.viewType;
            viewCreateInfo.format = resource.format;
            viewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewCreateInfo.subresourceRange.aspectMask = aspectMask;
            viewCreateInfo.subresourceRange.baseMipLevel = mipLevel;
            viewCreateInfo.subresourceRange.levelCount = 1;
            viewCreateInfo.subresourceRange.baseArrayLayer = 0;
            viewCreateInfo.subresourceRange.layerCount = 1;

            VkImageView view = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateImageView(m_device, &viewCreateInfo, nullptr, &view),
                      "GenerateDepthMipmapWithShader: vkCreateImageView");
            return view;
        };

        VkPipelineStageFlags originalSrcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags originalSrcAccessMask = 0;
        GetImageTransitionSourceState(originalLayout, originalSrcStageMask, originalSrcAccessMask);

        VkPipelineStageFlags finalDstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags finalDstAccessMask = 0;
        GetImageTransitionDestinationState(finalLayout, finalDstStageMask, finalDstAccessMask);

        VkPipelineStageFlags attachmentStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags attachmentAccessMask = 0;
        GetImageTransitionDestinationState(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                           attachmentStageMask, attachmentAccessMask);

        if (originalLayout != finalLayout) {
            if (baseMipLevel > 0) {
                VkImageLayout lowerMipLayout = originalLayout;
                const Bool lowerReady = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, resource.image, lowerMipLayout, finalLayout,
                    originalSrcStageMask, finalDstStageMask,
                    originalSrcAccessMask, finalDstAccessMask,
                    resource.aspect, 0, baseMipLevel);
                MOBILEGL_ASSERT(lowerReady, "%s: failed to transition lower untouched mip levels", __func__);
            }

            if (generateMipLevelCount < resource.mipLevels) {
                VkImageLayout upperMipLayout = originalLayout;
                const Bool upperReady = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, resource.image, upperMipLayout, finalLayout,
                    originalSrcStageMask, finalDstStageMask,
                    originalSrcAccessMask, finalDstAccessMask,
                    resource.aspect, generateMipLevelCount, resource.mipLevels - generateMipLevelCount);
                MOBILEGL_ASSERT(upperReady, "%s: failed to transition upper untouched mip levels", __func__);
            }

            VkImageLayout baseMipLayout = originalLayout;
            const Bool baseReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, resource.image, baseMipLayout, finalLayout,
                originalSrcStageMask, finalDstStageMask,
                originalSrcAccessMask, finalDstAccessMask,
                resource.aspect, baseMipLevel, 1);
            MOBILEGL_ASSERT(baseReady, "%s: failed to transition base mip level to sampled layout", __func__);
        }

        resource.layout = finalLayout;

        auto* depthProgramData = static_cast<Uint8*>(m_depthMipmapResources.program->MapUBO());
        MOBILEGL_ASSERT(depthProgramData != nullptr, "GenerateDepthMipmapWithShader: depth mipmap UBO is null");
        auto writeUniform = [&](Int location, const void* data, SizeT size) {
            MOBILEGL_ASSERT(location >= 0, "GenerateDepthMipmapWithShader: invalid uniform location");
            const Uint offset = m_depthMipmapResources.program->GetUniformOffset(static_cast<Uint>(location));
            // A RETURN, not only an assert: the assert compiles out in release, and a program
            // whose SPIR-V job settled cancelled reports kInvalidUniformOffset (~0u) with a
            // zero-sized shadow - which would make the memcpy below a wild write at
            // depthProgramData + 4 GiB rather than a dropped uniform.
            if (offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
                offset + size > m_depthMipmapResources.program->GetUBOSize()) {
                MOBILEGL_ASSERT(false, "GenerateDepthMipmapWithShader: uniform write out of bounds");
                return;
            }
            memcpy(depthProgramData + offset, data, size);
            m_depthMipmapResources.program->MarkUBOContentDirty();
        };

        for (Uint32 level = baseMipLevel + 1; level < generateMipLevelCount; ++level) {
            VkImageLayout dstMipLayout = originalLayout;
            const Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, resource.image, dstMipLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                originalSrcStageMask, attachmentStageMask,
                originalSrcAccessMask, attachmentAccessMask,
                resource.aspect, level, 1);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition mip level %u to depth attachment layout", __func__, level);

            const IntVec3 srcTexelSize = ComputeMipTexelSize(storageBaseTexelSize, level - 1);
            const IntVec3 dstTexelSize = ComputeMipTexelSize(storageBaseTexelSize, level);
            const Int srcTexelSizeUniform[2] = {srcTexelSize.x(), srcTexelSize.y()};

            const VkImageView sourceImageView = createMipView(level - 1, VK_IMAGE_ASPECT_DEPTH_BIT);
            const VkImageView depthAttachmentView = createMipView(level, resource.aspect);
            deferredCleanup.imageViews.push_back(sourceImageView);
            deferredCleanup.imageViews.push_back(depthAttachmentView);

            VkFramebufferCreateInfo framebufferCreateInfo{};
            framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferCreateInfo.renderPass = renderPass;
            framebufferCreateInfo.attachmentCount = 1;
            framebufferCreateInfo.pAttachments = &depthAttachmentView;
            framebufferCreateInfo.width = static_cast<Uint32>(dstTexelSize.x());
            framebufferCreateInfo.height = static_cast<Uint32>(dstTexelSize.y());
            framebufferCreateInfo.layers = 1;

            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateFramebuffer(m_device, &framebufferCreateInfo, nullptr, &framebuffer),
                      "GenerateDepthMipmapWithShader: vkCreateFramebuffer");
            deferredCleanup.framebuffers.push_back(framebuffer);

            VkRenderPassBeginInfo renderPassBeginInfo{};
            renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassBeginInfo.renderPass = renderPass;
            renderPassBeginInfo.framebuffer = framebuffer;
            renderPassBeginInfo.renderArea.offset = {0, 0};
            renderPassBeginInfo.renderArea.extent = {
                static_cast<Uint32>(dstTexelSize.x()), static_cast<Uint32>(dstTexelSize.y())
            };

            vkCmdBeginRenderPass(frame.commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(dstTexelSize.x());
            viewport.height = static_cast<float>(dstTexelSize.y());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = {static_cast<Uint32>(dstTexelSize.x()), static_cast<Uint32>(dstTexelSize.y())};
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            // The depth-mipmap pipeline's narrower dynamic set (viewport/scissor
            // only) leaves the other dynamic states undefined; its raw scissor
            // and viewport writes also bypass the shadow.
            ResetDynamicStateShadow();

            std::fill(depthProgramData,
                      depthProgramData + m_depthMipmapResources.program->GetUBOSize(),
                      Uint8{0});
            BlitUniformData blitUniformData{};
            writeUniform(m_depthMipmapResources.srcRectLocation,
                         blitUniformData.srcRect,
                         sizeof(blitUniformData.srcRect));
            writeUniform(m_depthMipmapResources.dstRectLocation,
                         blitUniformData.dstRect,
                         sizeof(blitUniformData.dstRect));
            writeUniform(m_depthMipmapResources.surfaceTransformLocation,
                         &blitUniformData.surfaceTransform,
                         sizeof(blitUniformData.surfaceTransform));
            writeUniform(m_depthMipmapResources.srcTexelSizeLocation,
                         srcTexelSizeUniform,
                         sizeof(srcTexelSizeUniform));

            const auto samplerBindingOverride = UniformManager::SamplerBindingOverride{
                .binding = m_depthMipmapResources.samplerBinding,
                .texture = &texture,
                .sampler = m_blitResources.nearestSampler.get(),
                .imageView = sourceImageView,
            };
            const Bool bound = m_uniformManager->BindProgramUniformBuffers(
                frame.commandBuffer, *m_depthMipmapResources.program, programObj,
                m_frameContext.GetCurrentFrameIndex(), VK_PIPELINE_BIND_POINT_GRAPHICS, &samplerBindingOverride);
            MOBILEGL_ASSERT(bound, "GenerateDepthMipmapWithShader: BindProgramUniformBuffers failed");
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            vkCmdEndRenderPass(frame.commandBuffer);

            VkImageLayout finishedMipLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            const Bool finishedReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, resource.image, finishedMipLayout, finalLayout,
                attachmentStageMask, finalDstStageMask,
                attachmentAccessMask, finalDstAccessMask,
                resource.aspect, level, 1);
            MOBILEGL_ASSERT(finishedReady, "%s: failed to transition mip level %u to sampled layout", __func__, level);
        }
        return true;
    }

    // Boost-style hash combine. The inputs are tiny enum ordinals and bit masks, so
    // full avalanche is unnecessary; the combine only has to keep distinct state
    // vectors apart under the memo's otherwise-exact key.
    static inline Uint64 CombinePipelineStateWord(Uint64 hash, Uint64 word) {
        return hash ^ (word + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2));
    }

    // Value hash over every fixed-function GL state the pipeline payload reads that
    // the memo key's other fields (mode, program hash, vertex-input hash, render-pass
    // hash, transform flags) do not already pin down. Enumerated against the payload
    // build in GetOrCreatePipeline - any new GL-state read there must be added here:
    //   - capability bits: CullFace, DepthTest, PolygonOffsetFill (mode gating rides
    //     the memo's mode key), RasterizerDiscard, ColorLogicOp, StencilTest,
    //     PrimitiveRestart(+FixedIndex), plus the depth write mask
    //   - patch vertices, polygon mode, cull face mode, depth func, logic op
    //   - front/back stencil ops + compare funcs (ref/mask are dynamic state)
    //   - per draw buffer up to the render pass's colour span: indexed blend enable,
    //     blend factors/equations, indexed colour write mask (broadcast from index 0
    //     when the device lacks independentBlend - the same read the payload does)
    // FBO-derived payload inputs (attachment presence/formats/draw-buffer gating) are
    // pinned by the render-pass hash key, exactly as the version-keyed memo relied on.
    Uint64 VulkanRenderer::ComputePipelineStateHash(Uint32 colorAttachmentCount) const {
        // One bulk fetch instead of ~17 per-field accessor calls into MG_State: every
        // input below is a plain field of RenderStateParameters, and each accessor this
        // replaces (IsCapabilityEnabled / Get*) is a verified pure read of that same
        // field (RenderState.cpp), so the hashed values are bit-identical. This runs on
        // every draw whose pipeline-state version moved (a per-draw GL_BLEND toggle),
        // where the accessor-call overhead dominated the hash itself.
        const RenderStateParameters& p = MG_State::pGLContext->GetRenderStateParameters();
        Uint64 capabilityBits = 0;
        capabilityBits |= p.CullFaceEnabled ? 1ull << 0 : 0;
        capabilityBits |= p.DepthTestEnabled ? 1ull << 1 : 0;
        capabilityBits |= p.PolygonOffsetFillEnabled ? 1ull << 2 : 0;
        capabilityBits |= p.RasterizerDiscardEnabled ? 1ull << 3 : 0;
        capabilityBits |= p.ColorLogicOpEnabled ? 1ull << 4 : 0;
        capabilityBits |= p.StencilTestEnabled ? 1ull << 5 : 0;
        capabilityBits |= p.PrimitiveRestartEnabled ? 1ull << 6 : 0;
        capabilityBits |= p.PrimitiveRestartFixedIndexEnabled ? 1ull << 7 : 0;
        capabilityBits |= p.DepthMask ? 1ull << 8 : 0;
        Uint64 hash = CombinePipelineStateWord(0x243F6A8885A308D3ull, capabilityBits);
        hash = CombinePipelineStateWord(hash, static_cast<Uint64>(p.PatchVertices));
        hash = CombinePipelineStateWord(hash, static_cast<Uint64>(p.PolygonModeFront));
        hash = CombinePipelineStateWord(hash, static_cast<Uint64>(p.CullFaceModeSetting));
        hash = CombinePipelineStateWord(hash, static_cast<Uint64>(p.DepthFunc));
        hash = CombinePipelineStateWord(hash, static_cast<Uint64>(p.LogicOp));
        // StencilStates[0] is Front, [1] is Back (RenderState::GetStencilFaceIndex) -
        // the same order the two GetStencilState(face) calls used to hash in.
        for (const StencilFaceState& stencil : p.StencilStates) {
            hash = CombinePipelineStateWord(hash,
                static_cast<Uint64>(stencil.FailOp) |
                (static_cast<Uint64>(stencil.PassDepthPassOp) << 16) |
                (static_cast<Uint64>(stencil.PassDepthFailOp) << 32) |
                (static_cast<Uint64>(stencil.Func) << 48));
        }
        MOBILEGL_ASSERT(colorAttachmentCount <= p.BlendStates.size(),
                        "ComputePipelineStateHash: colorAttachmentCount %u exceeds MAX_DRAW_BUFFERS",
                        colorAttachmentCount);
        for (Uint32 i = 0; i < colorAttachmentCount; ++i) {
            const PerBufferBlendState& blend = p.BlendStates[i];
            const BoolVec4 mask = p.ColorMasks[m_independentBlendFeatureEnabled ? i : 0];
            Uint64 attachmentWord = blend.Enabled ? 1ull : 0;
            attachmentWord |= (mask.r() ? 1ull << 1 : 0) | (mask.g() ? 1ull << 2 : 0) |
                              (mask.b() ? 1ull << 3 : 0) | (mask.a() ? 1ull << 4 : 0);
            attachmentWord |= static_cast<Uint64>(blend.SrcFactorRGB) << 8;
            attachmentWord |= static_cast<Uint64>(blend.DstFactorRGB) << 16;
            attachmentWord |= static_cast<Uint64>(blend.SrcFactorAlpha) << 24;
            attachmentWord |= static_cast<Uint64>(blend.DstFactorAlpha) << 32;
            attachmentWord |= static_cast<Uint64>(blend.ColorEquation) << 40;
            attachmentWord |= static_cast<Uint64>(blend.AlphaEquation) << 48;
            hash = CombinePipelineStateWord(hash, attachmentWord);
        }
        return hash;
    }

    // A program that runs a geometry shader AND captures transform feedback. Both halves are
    // link-time properties, so this is safe to fold into a pipeline keyed on the program hash.
    static Bool ProgramCapturesXfbFromGeometryStage(const MG_State::GLState::ProgramObject& program) {
        if (program.GetTransformFeedbackVaryingCount() == 0) return false;
        for (const auto& shader : program.GetAttachedShaders()) {
            if (shader && shader->GetShaderStage() == ShaderStage::Geometry) return true;
        }
        return false;
    }

    VkPipeline VulkanRenderer::GetOrCreatePipeline(
            GLenum mode,
            const MG_State::GLState::ProgramObject& program,
            const ProgramFactory::VkProgramObject& programObj,
            ProgramFactory::CompileOptionFlags transformFlags,
            const MG_State::GLState::VertexArrayObject& vao,
            const RenderPassEntry& renderPassEntry) {
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
        auto& vis = m_vertexInputStateFactory->GetOrCreateVertexInputState(vao);
        const Uint64 vertexLayoutHash = vis.layoutHash;
        const Uint64 renderPassHash = renderPassEntry.hash;
        // The pipeline-relevant subset only: glViewport / glScissor / glBlendColor / glStencilMask
        // and friends are dynamic state or not pipeline state at all, and keying the memo on the
        // all-state counter made any of them evict a perfectly good VkPipeline. The memo compares
        // the VALUE hash of that subset, never the version itself: the version is monotonic, so
        // per-draw state flips (GL_BLEND toggles) would otherwise miss entries the memo holds.
        // The version only guards recomputing the hash - unchanged version, unchanged bytes.
        const Uint renderStateVersion = MG_State::pGLContext->GetPipelineStateVersion();
        if (!m_pipelineStateHashValid || m_pipelineStateHashVersion != renderStateVersion ||
            m_pipelineStateHashColorCount != renderPassEntry.colorAttachmentCount) {
            m_pipelineStateHash = ComputePipelineStateHash(renderPassEntry.colorAttachmentCount);
            m_pipelineStateHashVersion = renderStateVersion;
            m_pipelineStateHashColorCount = renderPassEntry.colorAttachmentCount;
            m_pipelineStateHashValid = true;
        }
        const Uint64 pipelineStateHash = m_pipelineStateHash;
        for (Uint32 i = 0; i < m_pipelineMemoCount; ++i) {
            const PipelineMemoEntry& entry = m_pipelineMemo[i];
            if (entry.pipeline != VK_NULL_HANDLE && entry.mode == mode &&
                entry.programHash == programObj.hash && entry.vertexInputHash == vertexLayoutHash &&
                entry.renderPassHash == renderPassHash &&
                entry.pipelineStateHash == pipelineStateHash &&
                entry.transformFlags == transformFlags) {
                return entry.pipeline;
            }
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
        auto cullFaceEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::CullFace);
        auto depthTestEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::DepthTest);
        auto polygonOffsetFillEnabled =
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PolygonOffsetFill) &&
            DrawModeUsesPolygonFill(mode);
        auto rasterizerDiscardEnabled =
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::RasterizerDiscard);
        auto colorLogicOpEnabled =
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ColorLogicOp) && m_logicOpFeatureEnabled;
        auto stencilTestEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::StencilTest);
        // A framebuffer without a depth (stencil) attachment behaves as if the depth
        // (stencil) test always passes and nothing is written - even when the bound
        // image is a packed depth-stencil texture attached through only one half.
        {
            const auto& gatingFbo =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
            if (gatingFbo != nullptr && !gatingFbo->IsDefaultFramebuffer()) {
                const auto& depthAtt = gatingFbo->GetAttachment(MobileGL::FramebufferAttachmentType::Depth);
                const auto& stencilAtt = gatingFbo->GetAttachment(MobileGL::FramebufferAttachmentType::Stencil);
                if (!depthAtt.IsValid() || depthAtt.IsEmpty()) {
                    depthTestEnabled = false;
                }
                if (!stencilAtt.IsValid() || stencilAtt.IsEmpty()) {
                    stencilTestEnabled = false;
                }
            }
        }
        const StencilFaceState& frontStencil = MG_State::pGLContext->GetStencilState(StencilFace::Front);
        const StencilFaceState& backStencil = MG_State::pGLContext->GetStencilState(StencilFace::Back);
        const VkPolygonMode requestedPolygonMode =
            MG_Util::ConvertPolygonModeToVkEnum(MG_State::pGLContext->GetPolygonModeFront());
        // VK_POLYGON_MODE_LINE/_POINT require the fillModeNonSolid device feature; fall back to
        // VK_POLYGON_MODE_FILL when the device lacks it.
        const VkPolygonMode effectivePolygonMode =
            (requestedPolygonMode == VK_POLYGON_MODE_FILL || m_fillModeNonSolidFeatureEnabled)
                ? requestedPolygonMode
                : VK_POLYGON_MODE_FILL;

        const VkPrimitiveTopology vkTopology = MG_Util::ConvertPrimitiveModeToVkEnum(mode);
        const Bool primitiveRestartEnabled =
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestart) ||
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex);
        // Primitive restart on a *list* topology requires the primitiveTopologyListRestart feature;
        // strip/fan restart works without it. Silently dropping restarts would corrupt geometry, so
        // hard-fail here (at the draw) with the reason when the device lacks the feature.
        const auto isListTopology = [](VkPrimitiveTopology t) {
            return t == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                   t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                   t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
                   t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY || t == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        };
        if (primitiveRestartEnabled && !m_primitiveTopologyListRestartFeatureEnabled && isListTopology(vkTopology)) {
            THROW_EXCEPTION("Primitive restart on a list topology requires the primitiveTopologyListRestart device "
                            "feature (VK_EXT_primitive_topology_list_restart), which this device does not support; use "
                            "a strip/fan topology or a device that supports it.");
        }

        PipelineFactory::PipelineCreatePayload payload {
            .programHash = programObj.hash,
            .vertexInputHash = vertexLayoutHash,
            .pipelineLayout = programObj.pipelineLayout,
            .renderPass = renderPassEntry.renderPass,
            .colorAttachmentCount = renderPassEntry.colorAttachmentCount,
            .rasterizationSamples = renderPassEntry.sampleCount,
            .subpass = 0,
            .topology = vkTopology,
            .primitiveRestartEnable = primitiveRestartEnabled,
            .patchControlPoints = static_cast<Uint32>(MG_State::pGLContext->GetPatchVertices()),
            .viewportCount = ResolveDrawViewportCount(programObj.writesViewportIndexBuiltin),
            .polygonMode = effectivePolygonMode,
            .cullMode = cullFaceEnabled
                ? MG_Util::ConvertCullFaceModeToVkEnum(MG_State::pGLContext->GetCullFaceMode(), invertClockwise)
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
            .depthWriteEnable = depthTestEnabled && MG_State::pGLContext->GetDepthMask(),
            .depthBiasEnable = polygonOffsetFillEnabled,
            .rasterizerDiscardEnable = rasterizerDiscardEnabled,
            .logicOpEnable = colorLogicOpEnabled,
            .stencilTestEnable = stencilTestEnabled,
            .depthCompareOp = MG_Util::ConvertDepthTestFuncToVkEnum(MG_State::pGLContext->GetDepthFunc()),
            .logicOp = MG_Util::ConvertLogicOperationToVkEnum(MG_State::pGLContext->GetLogicOp()),
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
        if (programObj.needsPassthroughTessControl && programObj.passthroughTessControlEmulatable &&
            vkTopology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {
            payload.passthroughTessControlStage =
                m_programFactory->GetOrCreatePassthroughTessControlStage(payload.patchControlPoints);
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
        const auto& drawFboBinding =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        MOBILEGL_ASSERT(drawFboBinding != nullptr, "GetOrCreatePipeline: draw framebuffer is null");
        const Bool isDefaultDrawFbo = drawFboBinding->IsDefaultFramebuffer();
        const auto& drawBuffers = drawFboBinding->GetDrawBuffers();
        auto resolveCompleteColorAttachmentTexture = [&](Uint32 drawBufferIndex) -> MG_State::GLState::ITextureObject* {
            if (isDefaultDrawFbo || drawBufferIndex >= drawBuffers.size()) {
                return nullptr;
            }

            const auto drawBuffer = drawBuffers[drawBufferIndex];
            if (drawBuffer == FramebufferAttachmentType::None) {
                return nullptr;
            }

            const auto& attachment = drawFboBinding->GetAttachment(drawBuffer);
            if (!attachment.IsTexture() || !attachment.IsComplete()) {
                return nullptr;
            }

            return attachment.GetTexture().get();
        };
        for (Uint32 i = 0; i < payload.colorAttachmentCount; ++i) {
            BlendFactor srcRGB = BlendFactor::One;
            BlendFactor dstRGB = BlendFactor::Zero;
            BlendFactor srcAlpha = BlendFactor::One;
            BlendFactor dstAlpha = BlendFactor::Zero;
            BlendEquation colorEquation = BlendEquation::Add;
            BlendEquation alphaEquation = BlendEquation::Add;
            MG_State::pGLContext->GetBlendFuncIndexed(i, srcRGB, dstRGB, srcAlpha, dstAlpha);
            MG_State::pGLContext->GetBlendEquationIndexed(i, colorEquation, alphaEquation);
            const Bool blendEnabled = MG_State::pGLContext->IsCapabilityEnabledIndexed(CapabilityInput::Blend, i);
            // Per-draw-buffer color write mask (glColorMaski). Divergent per-attachment masks require
            // the independentBlend device feature; when it is absent, fall back to draw buffer 0's
            // mask for every attachment (matching the non-indexed glColorMask broadcast).
            const BoolVec4 bufferMask =
                MG_State::pGLContext->GetColorMaskIndexed(m_independentBlendFeatureEnabled ? i : 0);
            VkColorComponentFlags attachmentColorWriteMask = static_cast<VkColorComponentFlags>(
                (bufferMask.r() ? VK_COLOR_COMPONENT_R_BIT : 0u) |
                (bufferMask.g() ? VK_COLOR_COMPONENT_G_BIT : 0u) |
                (bufferMask.b() ? VK_COLOR_COMPONENT_B_BIT : 0u) |
                (bufferMask.a() ? VK_COLOR_COMPONENT_A_BIT : 0u));
            Bool effectiveBlendEnabled = blendEnabled;
            MG_State::GLState::ITextureObject* colorAttachmentTexture = nullptr;
            MG_State::GLState::RenderbufferObject* colorAttachmentRenderbuffer = nullptr;
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
            if (!isDefaultDrawFbo && i < drawBuffers.size()) {
                const auto drawBuffer = drawBuffers[i];
                colorAttachmentTexture = resolveCompleteColorAttachmentTexture(i);
                if (colorAttachmentTexture == nullptr && drawBuffer != FramebufferAttachmentType::None) {
                    const auto& attachment = drawFboBinding->GetAttachment(drawBuffer);
                    if (attachment.IsRenderbuffer() && attachment.IsComplete()) {
                        colorAttachmentRenderbuffer = attachment.GetRenderbuffer().get();
                    }
                }
                if (drawBuffer == FramebufferAttachmentType::None ||
                    (colorAttachmentTexture == nullptr && colorAttachmentRenderbuffer == nullptr)) {
                    // GL ignores writes and per-target blend state for GL_NONE draw buffer slots.
                    // Depth-only or otherwise unattached draw buffers should also discard color writes.
                    attachmentColorWriteMask = 0;
                    effectiveBlendEnabled = false;
                }
                if (colorAttachmentRenderbuffer != nullptr) {
                    const SizeT componentCount = MG_Util::GetBaseInternalFormatComponentCount(
                        colorAttachmentRenderbuffer->GetInternalFormat());
                    attachmentColorWriteMask &= GetSupportedColorWriteMaskForComponentCount(componentCount);
                }
                if (colorAttachmentTexture != nullptr) {
                    auto* texture = colorAttachmentTexture;
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
                    const auto* textureResource = m_textureManager->SyncTextureAndGetDescriptor(*texture);
                    MOBILEGL_ASSERT(textureResource != nullptr,
                                    "GetOrCreatePipeline: failed to sync color attachment textureId=%d",
                                    texture->GetExternalIndex());
                    VkFormatProperties attachmentFormatProperties{};
                    vkGetPhysicalDeviceFormatProperties(
                        m_physicalDevice.handle,
                        textureResource->format,
                        &attachmentFormatProperties);
                    MOBILEGL_ASSERT(
                        (attachmentFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0,
                        "GetOrCreatePipeline: color attachment %u format=%d textureId=%d lacks VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT (program=%u)",
                        i,
                        static_cast<Int>(textureResource->format),
                        texture->GetExternalIndex(),
                        program.GetExternalIndex());
#endif
                    const SizeT componentCount = MG_Util::GetBaseInternalFormatComponentCount(texture->GetFormat());
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
                    const NumericDomain attachmentNumericDomain =
                        GetNumericDomainForTextureInternalFormat(texture->GetFormat());
                    for (Uint32 outputLocation = 0;
                         outputLocation < ProgramFactory::VkProgramObject::kMaxVertexInputLocations;
                         ++outputLocation) {
                        if ((programObj.activeFragmentOutputLocationMask & (1u << outputLocation)) == 0 ||
                            outputLocation != i) {
                            continue;
                        }

                        const GLenum fragmentOutputType = programObj.fragmentOutputTypes[outputLocation];
                        const NumericDomain fragmentOutputDomain =
                            GetNumericDomainForShaderValueType(fragmentOutputType);
                        // GL allows fragment outputs with more components than the bound color attachment;
                        // excess components are discarded during conversion to the attachment format.
                        MOBILEGL_ASSERT(
                            attachmentNumericDomain == NumericDomain::Unknown ||
                                fragmentOutputDomain == NumericDomain::Unknown ||
                                attachmentNumericDomain == fragmentOutputDomain,
                            "GetOrCreatePipeline: fragment output location=%d type=%u mismatches color attachment %u internalFormat=%d textureId=%d program=%u",
                            static_cast<Int>(outputLocation),
                            static_cast<Uint32>(fragmentOutputType),
                            i,
                            static_cast<Int>(texture->GetFormat()),
                            texture->GetExternalIndex(),
                            program.GetExternalIndex());
                    }
#endif
                    const VkColorComponentFlags supportedColorWriteMask =
                        GetSupportedColorWriteMaskForComponentCount(componentCount);
                    if ((attachmentColorWriteMask & ~supportedColorWriteMask) != 0) {
                        MGLOG_W_ONCE(
                            "GetOrCreatePipeline: clamping colorWriteMask=0x%x to 0x%x on color attachment %u (componentCount=%zu textureId=%d internalFormat=%d program=%u blendEnabled=%d)",
                            static_cast<Uint32>(attachmentColorWriteMask),
                            static_cast<Uint32>(attachmentColorWriteMask & supportedColorWriteMask),
                            i,
                            componentCount,
                            texture->GetExternalIndex(),
                            static_cast<Int>(texture->GetFormat()),
                            program.GetExternalIndex(),
                            effectiveBlendEnabled ? 1 : 0);
                        attachmentColorWriteMask &= supportedColorWriteMask;
                    }
                }
            }
            if (effectiveBlendEnabled) {
                MOBILEGL_ASSERT(i < drawBuffers.size(),
                                "GetOrCreatePipeline: color attachment %u is out of draw buffer range %zu",
                                i, drawBuffers.size());

                VkFormat colorAttachmentFormat = VK_FORMAT_UNDEFINED;
                Int textureExternalIndex = -1;
                if (isDefaultDrawFbo) {
                    colorAttachmentFormat = m_swapchainObject.GetSurfaceFormat().format;
                } else if (colorAttachmentRenderbuffer != nullptr) {
                    textureExternalIndex = static_cast<Int>(colorAttachmentRenderbuffer->GetExternalIndex());
                    colorAttachmentFormat = MG_Util::ConvertTextureInternalFormatToVkEnum(
                        colorAttachmentRenderbuffer->GetInternalFormat());
                } else {
                    auto* texture = colorAttachmentTexture;
                    MOBILEGL_ASSERT(texture != nullptr,
                                    "GetOrCreatePipeline: blend is enabled on draw buffer %u but no complete color attachment is bound",
                                    i);
                    textureExternalIndex = texture->GetExternalIndex();
                    auto* textureResource = m_textureManager->SyncTextureAndGetDescriptor(*texture);
                    MOBILEGL_ASSERT(textureResource != nullptr,
                                    "GetOrCreatePipeline: failed to sync blend color attachment textureId=%d",
                                    texture->GetExternalIndex());
                    colorAttachmentFormat = textureResource->format;
                }

                // Blending on an attachment whose format lacks
                // VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT is invalid pipeline state
                // (blend support is optional for e.g. 32-bit float formats on some GPUs);
                // force-disable it instead of baking undefined behavior into the pipeline.
                static UnorderedMap<Int, Bool> formatBlendSupport;
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
            // glBindFragDataLocationIndexed) requires the dualSrcBlend device feature. It is detected at
            // device creation and surfaced in the POST; if a shader actually issues a draw with a SRC1
            // factor on a device that lacks it, there is no fallback, so hard-fail here at use time
            // rather than silently mistranslating the blend equation.
            if (effectiveBlendEnabled && !m_dualSrcBlendFeatureEnabled &&
                (IsDualSourceBlendFactor(srcRGB) || IsDualSourceBlendFactor(dstRGB) ||
                 IsDualSourceBlendFactor(srcAlpha) || IsDualSourceBlendFactor(dstAlpha))) {
                THROW_EXCEPTION(
                    "Dual-source blending (GL_SRC1_* blend factor) was used on color attachment " +
                    std::to_string(i) +
                    ", but the Vulkan device does not support the dualSrcBlend feature (see the "
                    "dualSrcBlend row in the driver POST). No fallback exists; the draw cannot proceed.");
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
        VkPipeline pipeline = m_pipelineFactory->GetOrCreatePipeline(payload);
        if (pipeline != VK_NULL_HANDLE) {
            PipelineMemoEntry& entry = m_pipelineMemo[m_pipelineMemoNext];
            entry.mode = mode;
            entry.programHash = programObj.hash;
            entry.vertexInputHash = vertexLayoutHash;
            entry.renderPassHash = renderPassHash;
            entry.pipelineStateHash = pipelineStateHash;
            entry.transformFlags = transformFlags;
            entry.pipeline = pipeline;
            m_pipelineMemoNext = (m_pipelineMemoNext + 1) % kPipelineMemoSize;
            m_pipelineMemoCount = std::min(m_pipelineMemoCount + 1, kPipelineMemoSize);
        }
        return pipeline;
    }

    Bool VulkanRenderer::PrepareStorageImageTextures(
        FrameContext::FrameData& frame,
        const MG_State::GLState::ProgramObject& program,
        const ProgramFactory::VkProgramObject& programObj) {
        if (!programObj.hasStorageImages) {
            return true;
        }
        auto& storageTextures = m_storageImageTexturesScratch;
        if (!m_uniformManager->CollectStorageImageTextures(program, programObj, storageTextures)) {
            MGLOG_E_ONCE("%s: failed to collect storage images for program=%u",
                    __func__, program.GetExternalIndex());
            return false;
        }
        if (storageTextures.empty()) {
            return true;
        }

        // Steady-state fast path: when every collected texture is already resident in GENERAL
        // with no pending clear and no dirty content, the loop below has nothing to record, so
        // keep the render pass alive instead of splitting it on every storage-image draw (on
        // tiled GPUs each split is a full tile load/store). GL makes cross-draw image-store
        // coherence the app's job (glMemoryBarrier), so no implicit barrier is owed here.
        // Record every image-unit binding before probing anything: a texture whose image was
        // created without STORAGE usage (the default - it costs UBWC compression on Adreno)
        // needs a recreate, and the probe below is what ends the render pass so that recreate
        // lands here rather than mid-pass. This cannot be folded into the probe loop, which
        // stops at the first texture that needs work and would leave the rest unmarked.
        for (auto* texture : storageTextures) {
            MOBILEGL_ASSERT(texture != nullptr, "%s: collected a null storage texture", __func__);
            m_textureManager->MarkStorageImageTexture(*texture);
        }

        Bool anyNeedsPreparation = false;
        for (auto* texture : storageTextures) {
            if (m_textureManager->NeedsStorageImagePreparation(*texture) ||
                m_clearManager->HasPendingClear(texture)) {
                anyNeedsPreparation = true;
                break;
            }
        }
        if (!anyNeedsPreparation) {
            return true;
        }

        // A first-time storage-usage upgrade recreates the image and carries the old contents
        // forward with an out-of-band, immediately-submitted copy (PreserveTextureContentsOnRecreate).
        // Whatever this frame already recorded into the old image is still sitting unsubmitted in
        // this command buffer, so that copy would read pre-frame content and this frame's rendering
        // into the texture would be lost - precisely the render-target-then-image-unit case this
        // whole path exists for. Submit what is recorded first; the copy then queues behind it.
        Bool anyNeedsStorageUpgrade = false;
        for (auto* texture : storageTextures) {
            if (m_textureManager->NeedsStorageUsageUpgrade(*texture)) {
                anyNeedsStorageUpgrade = true;
                break;
            }
        }
        if (anyNeedsStorageUpgrade && HasPendingRecordedWork()) {
            if (FlushPendingCommands()) {
                // Fresh command buffer: the sampled-descriptor-set memo describes bindings that
                // only existed in the retired one. FlushPendingCommands drops the pipeline memo
                // itself; this is the other command-buffer-scoped cache.
                m_lastSampledSetValid = false;
            } else {
                // Best effort: the upgrade still produces a correct image, only its preserved
                // contents may predate this frame's writes. Dropping the draw would be worse.
                MGLOG_E_ONCE("%s: flush before a storage-usage image upgrade failed; preserved contents "
                        "may be stale for one frame", __func__);
            }
        }
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }

        // Image uploads, deferred-clear materialization, and layout barriers are illegal inside
        // a classic render pass. Do this before sampler preparation as well: a texture used by
        // both a sampler and an image must stay in GENERAL, and both descriptors must name that
        // same layout independent of SPIR-V reflection/binding order.
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        for (auto* texture : storageTextures) {
            if (!MaterializePendingClearForTexture(frame.commandBuffer, *texture)) {
                MGLOG_E_ONCE("%s: failed to materialize pending clear for storage textureId=%d",
                        __func__, texture->GetExternalIndex());
                return false;
            }
            if (!m_textureManager->TransitionTextureForStorageImage(frame.commandBuffer, *texture)) {
                MGLOG_E_ONCE("%s: failed to prepare storage textureId=%d",
                        __func__, texture->GetExternalIndex());
                return false;
            }
        }
        return true;
    }
    Bool VulkanRenderer::PrepareSamplerImageFeedbackSnapshots(
        FrameContext::FrameData& frame,
        const MG_State::GLState::ProgramObject& program,
        const ProgramFactory::VkProgramObject& programObj,
        VkPipelineStageFlags consumerShaderStageMask) {
        auto& feedbackBindings = m_samplerImageFeedbackScratch;
        auto& overrides = m_samplerImageBindingOverridesScratch;
        overrides.clear();
        if (!programObj.hasStorageImages) {
            feedbackBindings.clear();
            return true;
        }
        if (!m_uniformManager->CollectSamplerImageFeedback(program, programObj, feedbackBindings)) {
            MGLOG_E_ONCE("%s: failed to collect sampler/image feedback for program=%u", __func__,
                         program.GetExternalIndex());
            return false;
        }

        if (feedbackBindings.empty()) {
            return true;
        }
        // Copy and layout barriers cannot be recorded inside a render pass. A graphics draw only
        // gets here after an actual sampled/writable-image mip overlap was found, so ordinary
        // graphics draws retain the active pass.
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        struct SnapshotCacheEntry {
            MG_State::GLState::ITextureObject* texture = nullptr;
            SamplerNumericDomain numericDomain = SamplerNumericDomain::Unknown;
            VkTextureManager::SampledTextureSnapshot snapshot{};
        };
        Vector<SnapshotCacheEntry> snapshotCache;
        snapshotCache.reserve(feedbackBindings.size());
        overrides.reserve(feedbackBindings.size());
        for (const auto& feedback : feedbackBindings) {
            VkTextureManager::SampledTextureSnapshot snapshot{};
            const auto existing = std::find_if(
                snapshotCache.begin(), snapshotCache.end(), [&feedback](const SnapshotCacheEntry& candidate) {
                    return candidate.texture == feedback.texture && candidate.numericDomain == feedback.numericDomain;
                });
            if (existing != snapshotCache.end()) {
                snapshot = existing->snapshot;
            } else {
                if (!m_textureManager->SnapshotTextureForSampling(frame.commandBuffer, *feedback.texture,
                                                                  feedback.numericDomain, consumerShaderStageMask,
                                                                  snapshot) ||
                    snapshot.imageView == VK_NULL_HANDLE) {
                    MGLOG_E_ONCE("%s: failed to snapshot textureId=%d for sampler binding=%u element=%u", __func__,
                                 feedback.texture != nullptr ? feedback.texture->GetExternalIndex() : 0,
                                 feedback.samplerBinding, feedback.samplerElement);
                    return false;
                }
                snapshotCache.push_back({.texture = feedback.texture,
                                         .numericDomain = feedback.numericDomain,
                                         .snapshot = snapshot});
            }
            overrides.push_back({
                .binding = feedback.samplerBinding,
                .element = feedback.samplerElement,
                .texture = feedback.texture,
                .sampler = feedback.sampler,
                .imageView = snapshot.imageView,
                .imageLayout = snapshot.layout,
                .forceNearestFiltering = feedback.numericDomain == SamplerNumericDomain::SignedInteger ||
                                         feedback.numericDomain == SamplerNumericDomain::UnsignedInteger,
            });
            if (program.GetExternalIndex() == 194 && feedback.texture->GetExternalIndex() == 75) {
                MGLOG_D_ONCE("sampler/image feedback snapshot: program=194 texture=75 binding=%u element=%u view=%p",
                             feedback.samplerBinding, feedback.samplerElement, snapshot.imageView);
            }
        }
        return true;
    }

    // The scissor rectangle Vulkan needs for ARB_viewport_array index `index`. Vulkan has no
    // per-viewport scissor-test TOGGLE - a scissor rectangle always applies - so an index whose
    // GL scissor test is disabled gets the whole framebuffer, which is exactly "the test always
    // passes" (GL 4.6 core 17.3.2).
    VkRect2D VulkanRenderer::ComputeGLScissorRect(Uint32 index, const IntVec2& extent,
                                                  VkSurfaceTransformFlagBitsKHR preTransform,
                                                  Bool isDefaultFbo) const {
        const auto& parameters = MG_State::pGLContext->GetRenderStateParameters();
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

        auto& shadow = g_dynamicStateShadow;
        shadow.viewportValid = false;
        shadow.scissorValid = false;
        shadow.dynamicTailValid = false;
    }

    void VulkanRenderer::ApplyDynamicDrawStateTail(FrameContext::FrameData& frame, const IntVec2& extent,
                                                   Bool isDefaultFbo, Uint32 viewportCount) {
        auto& shadow = g_dynamicStateShadow;
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
        const Uint paramsVersion = MG_State::pGLContext->GetRenderStateParametersVersion();
        if (shadow.dynamicTailValid && shadow.dynamicTailParamsVersion == paramsVersion &&
            shadow.dynamicTailExtentX == extent.x() && shadow.dynamicTailExtentY == extent.y() &&
            shadow.dynamicTailIsDefaultFbo == isDefaultFbo) {
            return;
        }
        const VkSurfaceTransformFlagBitsKHR preTransform = m_swapchainObject.GetPreTransform();
        // Second-level VALUE gate: the version moved, but RenderState's version counts
        // every parameter, most of which this tail never reads. Build the key over
        // exactly the tail's inputs (inventory in DynamicTailKey) out of one bulk
        // parameters fetch and compare; an equal key means every Apply* below would
        // re-derive the value its shadow already holds.
        DynamicStateShadow::DynamicTailKey key;
        {
            const RenderStateParameters& p = MG_State::pGLContext->GetRenderStateParameters();
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
                const auto& fbo =
                    MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
                return isDefaultFbo == (fbo != nullptr && fbo->IsDefaultFramebuffer());
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

    Bool VulkanRenderer::TrySetupDrawFastPath(FrameContext::FrameData& frame, GLenum mode,
                                              Flags<DrawSetupAspect> aspects, const DrawCmdParam& drawParams,
                                              const IndexBufferView* pIndexBufferView) {
        if (!frame.isCommandRecording) {
            return false;
        }
        // Entry select: by the draw program's lifetime id, MRU first (the id pins
        // the entry; every other fact is re-guarded below, so probing a stale
        // entry can only decline, never serve stale state).
        const auto& program = *MG_State::pGLContext->GetProgramForDraw();
        const Uint64 programLifetimeId = program.GetLifetimeId();
        SetupDrawSnapshot* snapPtr = nullptr;
        {
            SetupDrawSnapshot& mru = m_setupDrawSnapshots[m_setupDrawSnapshotMru];
            if (mru.valid && mru.programLifetimeId == programLifetimeId) {
                snapPtr = &mru;
            } else {
                for (Uint32 i = 0; i < kSetupDrawSnapshotCount; ++i) {
                    SetupDrawSnapshot& candidate = m_setupDrawSnapshots[i];
                    if (candidate.valid && candidate.programLifetimeId == programLifetimeId) {
                        snapPtr = &candidate;
                        m_setupDrawSnapshotMru = i;
                        break;
                    }
                }
            }
        }
        if (snapPtr == nullptr) {
            return false;
        }
        SetupDrawSnapshot& snap = *snapPtr;
        if (snap.aspects != aspects.GetRaw() || snap.mode != mode) {
            return false;
        }
        if (m_clearManager->HasAnyPendingClears()) {
            return false;
        }
        const auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        if (activeRenderPass == nullptr || activeRenderPass->hash != snap.renderPassHash ||
            snap.imageIndex != m_imageIndexAcquired) {
            return false;
        }
        if (program.GetBackendStateVersion() != snap.programVersion) {
            return false;
        }
        // glBegin/EndTransformFeedback moves no key this fast path otherwise observes
        // (the design makes capture a compile-option FLAG precisely because no version
        // bumps, VulkanRenderer.h's pipeline-memo note) - but the snapshot bakes that
        // flag into resolvedTransformFlags and the pipeline. Recompute the one dynamic
        // bit (the full path's exact predicate) and decline on a mismatch, or the first
        // captured draw after glBeginTransformFeedback would bind the undecorated
        // variant and silently capture nothing while the CPU bookkeeping advances.
        const Bool wantsXfbCapture = m_transformFeedbackFeatureEnabled &&
                                     MG_State::pGLContext->IsTransformFeedbackActive() &&
                                     program.GetTransformFeedbackVaryingCount() > 0;
        const Bool snapHasXfbCapture =
            static_cast<Bool>(ProgramFactory::CompileOptionFlags(snap.resolvedTransformFlags) &
                              ProgramFactory::CompileOptionBit::XfbCapture);
        if (wantsXfbCapture != snapHasXfbCapture) {
            return false;
        }
        // A changed VAO does NOT decline: the VAO only feeds the pipeline's vertex
        // input state (re-resolved below through the layout-keyed memo, so N VAOs
        // sharing one attribute layout share one pipeline) and the vertex/index
        // buffer binds (re-run every draw anyway). Declining here would send every
        // draw of a VAO-cycling stream (Minecraft chunk rendering) through the full
        // path, re-resolving descriptors and texture layouts nothing invalidated.
        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const Bool vaoMoved =
            static_cast<const void*>(&vao) != snap.vao || vao.GetLifetimeId() != snap.vaoLifetimeId ||
            vao.GetConfigVersion() != snap.vaoConfigVersion;
        const auto& drawFbo =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        if (static_cast<const void*>(drawFbo.get()) != snap.drawFbo ||
            drawFbo->GetLifetimeId() != snap.drawFboLifetimeId ||
            drawFbo->GetObjectVersion() != snap.fboVersion) {
            return false;
        }
        // The two monotonic counters get a shadow-compare rescue instead of an
        // unconditional decline: both bump on state writes whose VALUE often lands
        // back on what the snapshot already describes (a GL_BLEND toggle between
        // two draws, a redundant glBindSampler), and declining here sends every
        // such draw through the full SetupDraw.
        const Uint renderStateVersion = MG_State::pGLContext->GetPipelineStateVersion();
        const Uint64 bindGeneration = MG_State::pGLContext->GetTextureBindGeneration();
        const Bool renderStateMoved = renderStateVersion != snap.renderStateVersion;
        const Bool bindsMoved = bindGeneration != snap.bindGeneration;
        if (renderStateMoved) {
            // Only the pipeline depends on the moved state - except the render-pass
            // flavor input (depth/stencil participation); a flip of that must take
            // the full path's pass selection. One bulk parameters fetch instead of
            // two capability-accessor calls; both are pure reads of the same fields.
            const RenderStateParameters& rsp = MG_State::pGLContext->GetRenderStateParameters();
            const Bool drawUsesDepthStencil = rsp.DepthTestEnabled || rsp.StencilTestEnabled;
            if (drawUsesDepthStencil != snap.drawUsesDepthStencil) {
                return false;
            }
        }
        // The FBO identity+version compare above proved this draw's framebuffer is the
        // snapshotting draw's, so its default-ness is the snapshot's too - no second walk
        // of the binding slot and no virtual IsDefaultFramebuffer call.
        if (GetBaseTransformFlagsRaw(snap.drawFboIsDefault) != snap.baseTransformFlags) {
            return false;
        }
        if (m_textureManager->GetResourceEraseEpoch() != snap.textureEraseEpoch ||
            m_textureManager->GetTextureImageEpoch() != snap.textureImageEpoch ||
            m_renderPassManager->GetRenderbufferImageEpoch() != snap.renderbufferImageEpoch) {
            return false;
        }
        // Program entry: (lifetimeId, backend-state version, resolved flags) were proven
        // equal above, and those pin the factory hash - so the snapshot's memoised entry
        // pointer IS this draw's entry while the factory's open-addressing cache has not
        // moved entries (structure epoch). Bypassing GetOrCreateProgram skips its use
        // stamp, so re-stamp here or the idle sweep could evict a live entry.
        const ProgramFactory::VkProgramObject* programObjPtr = snap.programObj;
        if (programObjPtr != nullptr &&
            snap.programFactoryEpoch == m_programFactory->GetCacheStructureEpoch()) {
            m_programFactory->StampProgramUse(*programObjPtr);
        } else {
            programObjPtr = &m_programFactory->GetOrCreateProgram(
                program, ProgramFactory::CompileOptionFlags(snap.resolvedTransformFlags));
            snap.programObj = programObjPtr;
            snap.programFactoryEpoch = m_programFactory->GetCacheStructureEpoch();
        }
        const auto& programObj = *programObjPtr;

        // The pipeline and the vertex-input pre-flight depend on the VAO only through
        // its resolved LAYOUT (layoutHash folds the attribute formats, bindings and the
        // unsupported mask; the masks below are functions of the same configuration),
        // never its identity. A VAO-cycling stream (Minecraft chunk rendering) swaps
        // hundreds of VAOs sharing one layout per frame: answer "same layout?" from the
        // VAO's aux memo - it sits next to the config-version word this compare chain
        // already loaded - instead of chasing the vertex-input factory's cold heap entry.
        Uint64 vaoLayoutHash = snap.vaoLayoutHash;
        Bool vaoLayoutMoved = false;
        if (vaoMoved) {
            // Read the layout facts through the flat per-VAO memo table, keyed by the
            // VAO's content-hash memo. The hash memo shares the cache line this compare
            // chain already loaded (the config version), and the table slot is compact
            // and hot - unlike the VAO's aux-memo words, which start a second cold line
            // of every object in a VAO-cycling frame. The slot only ever answers for
            // THIS object: LookupVaoDrawMemo matches (address, lifetime id), so a slot
            // a destroyed VAO left behind at a recycled address misses and the facts
            // are re-resolved. The contentHash compare is the second gate on top of
            // that identity check, catching a reconfiguration of the same live object.
            Uint64 auxMasks = 0;
            Bool factsKnown = false;
            Uint64 contentHash = 0;
            if (vao.GetBackendHashMemo(contentHash)) {
                const VaoDrawMemo* vaoMemo = LookupVaoDrawMemo(&vao);
                if (vaoMemo->layoutFactsValid && vaoMemo->contentHash == contentHash) {
                    vaoLayoutHash = vaoMemo->layoutHash;
                    auxMasks = vaoMemo->layoutAuxMasks;
                    factsKnown = true;
                }
            }
            if (!factsKnown) {
                // First sight of this VAO configuration: resolve (which stamps the
                // VAO's hash memo) and read the same facts from the entry, then stamp
                // the table slot for every later draw.
                const auto& vertexInputState = m_vertexInputStateFactory->GetOrCreateVertexInputState(vao);
                vaoLayoutHash = vertexInputState.layoutHash;
                auxMasks = VertexInputStateFactory::PackVertexInputAuxMasks(
                    vertexInputState.unsupportedAttribMask, vertexInputState.attributeLocationMask);
                Uint64 stampedHash = 0;
                if (vao.GetBackendHashMemo(stampedHash)) {
                    VaoDrawMemo* vaoMemo = LookupVaoDrawMemo(&vao);
                    vaoMemo->contentHash = stampedHash;
                    vaoMemo->layoutHash = vaoLayoutHash;
                    vaoMemo->layoutAuxMasks = auxMasks;
                    vaoMemo->layoutFactsValid = true;
                }
            }
            vaoLayoutMoved = vaoLayoutHash != snap.vaoLayoutHash;
            if (vaoLayoutMoved) {
                // Vertex-input pre-flight for the changed layout, mirroring the full
                // path: a bad attribute must never be baked into a cached VkPipeline,
                // and the current-value synthesis in UploadAndBindVertexBuffers must
                // never see an unsupported generic-attribute type. Declining routes the
                // draw through the full path's loud failure reporting. An UNMOVED layout
                // needs no pre-flight: the snapshotting draw passed it with identical
                // inputs (same program; masks pinned by the layout hash).
                const Uint32 unsupportedAttribMask = static_cast<Uint32>(auxMasks >> 32);
                const Uint32 attributeLocationMask = static_cast<Uint32>(auxMasks);
                const Uint32 activeAttribMask = programObj.activeVertexInputLocationMask;
                if ((unsupportedAttribMask & activeAttribMask) != 0) {
                    return false;
                }
                const Uint32 missingAttribMask = activeAttribMask & ~attributeLocationMask;
                if (missingAttribMask != 0) {
                    for (Uint32 location = 0; location < kMaxVertexAttribs; ++location) {
                        if ((missingAttribMask & (1u << location)) == 0) {
                            continue;
                        }
                        if (MG_State::GLState::ClassifyVertexAttribType(programObj.vertexInputTypes[location])
                                .baseType == MG_State::GLState::VertexAttribBaseType::Unsupported) {
                            return false;
                        }
                    }
                }
            }
        }
        if (bindsMoved &&
            !m_uniformManager->SampledBindingsUnchanged(program, programObj, snap.sampledBindingRecords)) {
            return false;
        }

        // Same sampled set as the snapshotting draw (program/bind keys above);
        // verify content and params are untouched and every layout is still
        // sampleable, then stamp recording use exactly as the full path would.
        // A feedback case (sampled texture written by the active pass) fails the
        // layout check and falls back to the full path's end-pass handling.
        // The ENTRY's copies, not the scratch vectors: with more than one entry
        // the scratch holds only the last full-path draw's set, which may belong
        // to a different program.
        const auto& sampledTextures = snap.sampledTextures;
        const auto& sampledResources = snap.sampledResources;
        if (sampledResources.size() != sampledTextures.size()) {
            return false;
        }
        Uint64 contentSum = 0;
        Uint64 paramsSum = 0;
        // The descriptor-reuse hint (see BindProgramUniformBuffers) additionally needs
        // every sampled resource still in the exact layout the cached descriptors hold.
        // A layout that moved to a different-but-sampleable one only clears the hint
        // (this draw re-resolves and re-caches) - the fast path itself stays valid.
        // bindsMoved does not clear the hint: reaching this point with a moved bind
        // generation means SampledBindingsUnchanged proved the per-binding (texture,
        // sampler) pairs identical, and the sums/generation checks below cover every
        // remaining descriptor input.
        const Bool layoutSnapshotUsable = snap.sampledLayouts.size() == sampledTextures.size();
        Bool samplerDescriptorsUnchanged = layoutSnapshotUsable;
        for (SizeT i = 0; i < sampledTextures.size(); ++i) {
            const auto* sampledTexture = sampledTextures[i];
            if (sampledTexture == nullptr) {
                continue;
            }
            auto* resource = sampledResources[i];
            if (resource == nullptr || !IsValidSampledImageLayout(resource->layout)) {
                return false;
            }
            if (layoutSnapshotUsable && snap.sampledLayouts[i] != resource->layout) {
                snap.sampledLayouts[i] = resource->layout;
                samplerDescriptorsUnchanged = false;
            }
            contentSum += sampledTexture->GetContentVersion();
            paramsSum += sampledTexture->GetTextureParamsVersion();
            // Folded into this walk (was a second loop): the stamp is a plain recency
            // store. Stamping ahead of the sum compare below is benign - a declined
            // draw re-runs the full path, which stamps the same resources, and an
            // over-stamp only delays garbage collection by one generation.
            m_textureManager->StampResourceRecordingUse(*resource);
        }
        if (contentSum != snap.sampledContentSum || paramsSum != snap.sampledParamsSum) {
            return false;
        }
        const Uint64 samplingResolutionGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
        if (samplingResolutionGeneration != snap.samplingResolutionGeneration) {
            // Decline, not re-arm: snap.resolvedTransformFlags bakes the
            // ExplicitLod0Sampling verdict, which reads the effective sampler's
            // filters/aniso/LOD range - exactly the state this counter tracks.
            // Re-arming the stamp here would rebuild the descriptors but keep the
            // stale SPIR-V variant forever (every later draw compares equal again).
            // Same shape as the erase-epoch declines above; costs one full-path draw
            // per sampler/shape change, and the full path's LOD memo re-probes.
            return false;
        }

        // Everything the full path would re-resolve is provably unchanged - or, for
        // a moved pipeline-state version or a changed vertex-input LAYOUT, reduces to
        // re-resolving just the pipeline through the value-keyed memo against the
        // still-active render pass. A changed VAO with the SAME layout keeps the
        // snapshot's pipeline outright (the layout is the pipeline's only VAO input).
        // Run only the per-draw tail.
        VkPipeline pipeline = snap.pipeline;
        if (renderStateMoved || vaoLayoutMoved) {
            pipeline = VK_NULL_HANDLE;
            // The render pass is provably the snapshot's (hash match above), so probe
            // the value-keyed pipeline memo directly - no render-pass-entry re-fetch
            // (whose pending-clear probes cost more than the whole probe below). For a
            // moved state version, first refresh the pipeline-state VALUE hash exactly
            // as GetOrCreatePipeline would (same inputs: the snapshot pins the pass, so
            // its color attachment count is the right hash input); the value hash is
            // what lets a per-draw GL_BLEND toggle alternate between two memo entries
            // instead of missing forever on a monotonic version. A miss falls through
            // to the full lookup.
            if (!m_pipelineStateHashValid || m_pipelineStateHashVersion != renderStateVersion ||
                m_pipelineStateHashColorCount != snap.renderPassColorCount) {
                m_pipelineStateHash = ComputePipelineStateHash(snap.renderPassColorCount);
                m_pipelineStateHashVersion = renderStateVersion;
                m_pipelineStateHashColorCount = snap.renderPassColorCount;
                m_pipelineStateHashValid = true;
            }
            const auto memoTransformFlags =
                ProgramFactory::CompileOptionFlags(snap.resolvedTransformFlags);
            for (Uint32 i = 0; i < m_pipelineMemoCount; ++i) {
                const PipelineMemoEntry& entry = m_pipelineMemo[i];
                if (entry.pipeline != VK_NULL_HANDLE && entry.mode == mode &&
                    entry.programHash == programObj.hash && entry.vertexInputHash == vaoLayoutHash &&
                    entry.renderPassHash == snap.renderPassHash &&
                    entry.pipelineStateHash == m_pipelineStateHash &&
                    entry.transformFlags == memoTransformFlags) {
                    pipeline = entry.pipeline;
                    break;
                }
            }
            if (pipeline == VK_NULL_HANDLE) {
                // Same lookup the full path would do; every input (FBO + version, image
                // index, depth/stencil participation, image epochs, no pending clears)
                // was verified unchanged above, so this is a pure cache hit on the same
                // entry the snapshot's pipeline was built against.
                const RenderPassEntry& renderPassEntry = m_renderPassManager->GetOrCreateRenderPass(
                    *drawFbo, m_imageIndexAcquired, snap.drawUsesDepthStencil);
                if (!activeRenderPass->CompatibleWith(renderPassEntry)) {
                    return false;
                }
                pipeline = GetOrCreatePipeline(mode, program, programObj,
                                               ProgramFactory::CompileOptionFlags(snap.resolvedTransformFlags),
                                               vao, renderPassEntry);
                if (pipeline == VK_NULL_HANDLE) {
                    return false;
                }
            }
        }
        // Every decline is behind us: the snapshot again describes the current
        // counters, so the next draw's compare is two integer loads.
        snap.renderStateVersion = renderStateVersion;
        snap.bindGeneration = bindGeneration;
        snap.vao = static_cast<const void*>(&vao);
        snap.vaoLifetimeId = vao.GetLifetimeId();
        snap.vaoConfigVersion = vao.GetConfigVersion();
        snap.vaoLayoutHash = vaoLayoutHash;
        snap.pipeline = pipeline;
        if (!g_dynamicStateShadow.graphicsPipelineValid ||
            g_dynamicStateShadow.graphicsPipeline != pipeline) {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            g_dynamicStateShadow.graphicsPipelineValid = true;
            g_dynamicStateShadow.graphicsPipeline = pipeline;
        }
        if (!m_uniformManager->BindProgramUniformBuffers(frame.commandBuffer, program, programObj,
                                                         m_frameContext.GetCurrentFrameIndex(),
                                                         VK_PIPELINE_BIND_POINT_GRAPHICS, nullptr,
                                                         samplerDescriptorsUnchanged)) {
            return false;
        }
        if (!UploadAndBindVertexBuffers(frame.commandBuffer, vao, programObj, drawParams, pIndexBufferView)) {
            return false;
        }
        if (aspects & DrawSetupAspect::IndexBuffer) {
            const Bool idxUploadOk = UploadAndBindIndexBuffer(frame, vao, pIndexBufferView);
            MOBILEGL_ASSERT(idxUploadOk, "SetupDraw fast path: failed to upload index buffer");
        }
        ApplyDynamicDrawStateTail(frame, snap.renderPassExtent, snap.drawFboIsDefault, snap.viewportCount);
        return true;
    }

    Bool VulkanRenderer::SetupDraw(FrameContext::FrameData& frame, GLenum mode, Flags<DrawSetupAspect> aspects,
                                   const DrawCmdParam& drawParams,
                                   const IndexBufferView* pIndexBufferView) {
        // Sync each sampled texture at most once across this whole draw: the layout
        // probe loop, the post-transition loop, and ResolveSamplerDescriptor would
        // otherwise each re-run the full SyncTexture path on the same textures.
        MakeXfbWritesVisible();
        VkTextureManager::DrawSyncScope drawSyncScope(*m_textureManager);
        m_textureManager->CollectGarbage();
        {
            // Mirror DirectGLES's SyncToBackend gate: a program whose phase-B job failed or
            // was cancelled has no usable optimized module - and on an in-place
            // SanitizeAndOptimizeBinary failure GetGeneratedSpirv() still holds the RAW
            // glslang words, which must never reach vkCreateShaderModule. Drop the draw.
            const auto& drawProgram = *MG_State::pGLContext->GetProgramForDraw();
            if (!drawProgram.GetLinkStatus() || !drawProgram.GetSpirvStatus()) {
                MGLOG_D("SetupDraw skipped: program=%u is linked=%d spirv=%d",
                        drawProgram.GetExternalIndex(), static_cast<int>(drawProgram.GetLinkStatus()),
                        static_cast<int>(drawProgram.GetSpirvStatus()));
                return false;
            }
        }
        if (TrySetupDrawFastPath(frame, mode, aspects, drawParams, pIndexBufferView)) {
            return true;
        }
        const auto& drawFbo =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        if (drawFbo != nullptr && IsUnsupportedFramebufferForDirectVulkan(*drawFbo)) {
            // Nothing was mutated: other entries' per-probe guards (FBO identity +
            // version among them) stay authoritative, so none need invalidating.
            RecordUnsupportedFramebufferError(__func__);
            return false;
        }
        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto& program = *MG_State::pGLContext->GetProgramForDraw();
        // The fast path declined (or had no entry for this program): whatever THIS
        // program's entry saw may be stale, and the full path below mutates state as
        // it goes, so the entry must not stay matchable if that path fails mid-way.
        // Select it now - the program's own entry when one exists, else an invalid
        // slot, else a round-robin victim - and invalidate it until the successful
        // refill at the end. Other programs' entries keep their validity: every fact
        // they carry is re-guarded per probe (live pass hash, epochs, versions,
        // sums), so a full path run in between can only make them decline.
        SetupDrawSnapshot* fillSnap = nullptr;
        {
            Uint32 fillIndex = kSetupDrawSnapshotCount;
            const Uint64 fillProgramLifetimeId = program.GetLifetimeId();
            for (Uint32 i = 0; i < kSetupDrawSnapshotCount; ++i) {
                if (m_setupDrawSnapshots[i].valid &&
                    m_setupDrawSnapshots[i].programLifetimeId == fillProgramLifetimeId) {
                    fillIndex = i;
                    break;
                }
            }
            if (fillIndex == kSetupDrawSnapshotCount) {
                for (Uint32 i = 0; i < kSetupDrawSnapshotCount; ++i) {
                    if (!m_setupDrawSnapshots[i].valid) {
                        fillIndex = i;
                        break;
                    }
                }
            }
            if (fillIndex == kSetupDrawSnapshotCount) {
                fillIndex = m_setupDrawSnapshotVictim;
                m_setupDrawSnapshotVictim = (m_setupDrawSnapshotVictim + 1) % kSetupDrawSnapshotCount;
            }
            fillSnap = &m_setupDrawSnapshots[fillIndex];
            fillSnap->valid = false;
            m_setupDrawSnapshotMru = fillIndex;
        }
        const Bool drawFboIsDefault = drawFbo != nullptr && drawFbo->IsDefaultFramebuffer();
        ProgramFactory::CompileOptionFlags transformFlags =
            ProgramFactory::CompileOptionFlags(GetBaseTransformFlagsRaw(drawFboIsDefault));
        // Captured draws take the xfb-decorated program variant.
        if (m_transformFeedbackFeatureEnabled && MG_State::pGLContext->IsTransformFeedbackActive() &&
            program.GetTransformFeedbackVaryingCount() > 0) {
            transformFlags |= ProgramFactory::CompileOptionBit::XfbCapture;
        }
        // Sampling a colour render target through the driver's implicit-LOD path faults the GPU on
        // Adreno 650 (see ForceExplicitLod0SamplePass); ask for the explicit-LOD variant when doing
        // so cannot change a texel, i.e. when every sampler this program reads is pinned to a
        // single mip level. The probe walks every sampler binding, so its verdict is memoized
        // under the sampled-set memo's key plus the sampled textures' params-version sum (level
        // range and filter changes live there); the previous draw's texture list is valid for the
        // sum exactly when that key matches (same program, same binds).
        {
            const Uint64 lodProgramLifetimeId = program.GetLifetimeId();
            const Uint32 lodProgramVersion = program.GetBackendStateVersion();
            const Uint64 lodBindGeneration = MG_State::pGLContext->GetTextureBindGeneration();
            // The probe also reads the EFFECTIVE sampler's filters/aniso/LOD range
            // (ProgramSamplesOnlySingleLevelTextures), and those setters bump ONLY the
            // sampling-resolution generation - not the texture params version the sum
            // below covers. Without this key a filter/aniso change would keep serving
            // the stale verdict.
            const Uint64 lodSamplingGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
            Bool lodMemoHit = false;
            if (m_lastLodDecisionValid && m_lastSampledSetValid &&
                m_lastLodProgramLifetimeId == lodProgramLifetimeId &&
                m_lastLodProgramVersion == lodProgramVersion &&
                m_lastLodBindGeneration == lodBindGeneration &&
                m_lastLodSamplingGeneration == lodSamplingGeneration && m_lastLodBaseFlags == transformFlags &&
                m_lastSampledSetProgramLifetimeId == lodProgramLifetimeId &&
                m_lastSampledSetProgramVersion == lodProgramVersion &&
                m_lastSampledSetBindGeneration == lodBindGeneration) {
                Uint64 paramsSum = 0;
                for (const auto* sampledTexture : m_sampledTexturesScratch) {
                    if (sampledTexture != nullptr) {
                        paramsSum += sampledTexture->GetTextureParamsVersion();
                    }
                }
                if (paramsSum == m_lastLodParamsSum) {
                    transformFlags = m_lastLodResultFlags;
                    lodMemoHit = true;
                }
            }
            if (!lodMemoHit) {
                const ProgramFactory::CompileOptionFlags baseFlags = transformFlags;
                const auto& baseProgramObj = m_programFactory->GetOrCreateProgram(program, transformFlags);
                if (UniformManager::ProgramSamplesOnlySingleLevelTextures(program, baseProgramObj)) {
                    transformFlags |= ProgramFactory::CompileOptionBit::ExplicitLod0Sampling;
                }
                m_lastLodDecisionValid = true;
                m_lastLodProgramLifetimeId = lodProgramLifetimeId;
                m_lastLodProgramVersion = lodProgramVersion;
                m_lastLodBindGeneration = lodBindGeneration;
                m_lastLodSamplingGeneration = lodSamplingGeneration;
                m_lastLodBaseFlags = baseFlags;
                m_lastLodResultFlags = transformFlags;
                m_lastLodParamsSum = 0;  // filled below once the sampled set is known
            }
        }
        // GL's gl_BaseVertex is zero for every command without a baseVertex parameter, while
        // Vulkan's builtin reports the draw's firstVertex; a non-indexed draw therefore takes
        // the zeroed program variant. The question is about the program's SPIR-V, not about
        // this draw, so it is memoized on (program lifetime, backend-state version): only the
        // very first draw of a program pays the extra lookup, and a program used exclusively
        // with non-indexed draws never resolves - never compiles, never re-stamps - the
        // variant no draw of it would use.
        const Bool nonIndexedDraw = !(aspects & DrawSetupAspect::IndexBuffer);
        const Uint64 baseVertexProgramLifetimeId = program.GetLifetimeId();
        const Uint32 baseVertexProgramVersion = program.GetBackendStateVersion();
        const Bool baseVertexQueryKnown = m_lastBaseVertexQueryValid &&
                                          m_lastBaseVertexProgramLifetimeId == baseVertexProgramLifetimeId &&
                                          m_lastBaseVertexProgramVersion == baseVertexProgramVersion;
        if (baseVertexQueryKnown && nonIndexedDraw && m_lastBaseVertexReads) {
            transformFlags |= ProgramFactory::CompileOptionBit::ZeroBaseVertex;
        }
        const ProgramFactory::VkProgramObject* resolvedProgramObj =
            &m_programFactory->GetOrCreateProgram(program, transformFlags);
        if (!baseVertexQueryKnown) {
            // Read the answer out of the entry BEFORE any second lookup: that lookup may
            // insert and move every entry of the open-addressing cache, dangling the
            // reference. The zeroing pass leaves the variable declared, so the variant just
            // resolved answers the same as the base one either way.
            const Bool readsBaseVertex = resolvedProgramObj->readsBaseVertexBuiltin;
            m_lastBaseVertexQueryValid = true;
            m_lastBaseVertexProgramLifetimeId = baseVertexProgramLifetimeId;
            m_lastBaseVertexProgramVersion = baseVertexProgramVersion;
            m_lastBaseVertexReads = readsBaseVertex;
            if (nonIndexedDraw && readsBaseVertex) {
                transformFlags |= ProgramFactory::CompileOptionBit::ZeroBaseVertex;
                resolvedProgramObj = &m_programFactory->GetOrCreateProgram(program, transformFlags);
            }
        }
        const auto& programObj = *resolvedProgramObj;
        // For the snapshot's memoised entry pointer: if anything below inserts into the
        // program cache (blit/aux program compiles), the epoch moves and the snapshot
        // stores no pointer for this draw - the fast path then re-looks-up once.
        const Uint64 programFactoryEpochAtResolve = m_programFactory->GetCacheStructureEpoch();

        // Begin command recording if not yet
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
            // New command buffer: a program/FBO address from a previous frame may have been
            // recycled, so start the sampled-set skip cache fresh this frame.
            m_lastSampledSetValid = false;
        }

        if (!PrepareStorageImageTextures(frame, program, programObj)) {
            MGLOG_E_ONCE("SetupDraw skipped: storage image preparation failed");
            return false;
        }
        if (!PrepareSamplerImageFeedbackSnapshots(frame, program, programObj,
                                                  VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)) {
            MGLOG_E_ONCE("SetupDraw skipped: sampler/image feedback snapshot failed");
            return false;
        }

        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();

        // Check if any of the textures to sample have pending clears,
        // which probably indicates it's been gone through codepath like `fbo attach` -> `clear` -> `fbo detach`, and
        // without draws in between to give it a chance to materialize such clear.
        // Deal with this situation here.
        // Reuse the previous draw's sampled-texture list when the set is provably unchanged (same
        // program+state+transform and no bind/unbind/delete since), skipping the per-draw GL walk.
        // The layout/feedback/transition loops below still run on the list every draw, so this only
        // elides re-resolving *which* textures are sampled, never their layout handling.
        auto& sampledTextures = m_sampledTexturesScratch;
        {
            const Uint64 programLifetimeId = program.GetLifetimeId();
            const Uint32 programVersion = program.GetBackendStateVersion();
            const Uint64 bindGeneration = MG_State::pGLContext->GetTextureBindGeneration();
            const Bool sampledSetUnchanged =
                m_lastSampledSetValid && m_lastSampledSetProgramLifetimeId == programLifetimeId &&
                m_lastSampledSetProgramVersion == programVersion &&
                m_lastSampledSetTransformFlags == transformFlags &&
                m_lastSampledSetBindGeneration == bindGeneration;
            if (!sampledSetUnchanged) {
                const Bool hasSampledTextures = m_uniformManager->CollectSampledTextures(
                    program, programObj, sampledTextures, &m_sampledBindingRecordsScratch);
                MOBILEGL_ASSERT(hasSampledTextures, "%s: CollectSampledTextures failed", __func__);
                m_lastSampledSetValid = true;
                m_lastSampledSetProgramLifetimeId = programLifetimeId;
                m_lastSampledSetProgramVersion = programVersion;
                m_lastSampledSetTransformFlags = transformFlags;
                m_lastSampledSetBindGeneration = bindGeneration;
            }
            // Complete a freshly-made LOD decision (see above): its params sum
            // can only be taken once the sampled set is known. A genuine
            // all-zero sum merely re-probes next draw.
            if (m_lastLodDecisionValid && m_lastLodParamsSum == 0) {
                Uint64 paramsSum = 0;
                for (const auto* sampledTexture : sampledTextures) {
                    if (sampledTexture != nullptr) {
                        paramsSum += sampledTexture->GetTextureParamsVersion();
                    }
                }
                m_lastLodParamsSum = paramsSum;
            }
        }
        MGLOG_D("SetupDraw: program=%u drawFbo=%u sampledTextureCount=%zu activeRenderPass=%s",
                program.GetExternalIndex(), drawFbo ? drawFbo->GetExternalIndex() : 0u, sampledTextures.size(),
                activeRenderPass ? "true" : "false");
        Bool activeRenderPassUsesSampledTexture = false;
        if (activeRenderPass != nullptr) {
            for (auto* sampledTexture : sampledTextures) {
                if (sampledTexture == nullptr) {
                    continue;
                }
                if (ActiveRenderPassUsesTexture(*activeRenderPass, *sampledTexture)) {
                    MGLOG_D("SetupDraw: active render pass is still using sampled textureId=%d; ending render pass before descriptor preparation",
                            sampledTexture->GetExternalIndex());
                    activeRenderPassUsesSampledTexture = true;
                    break;
                }
            }
        }
        if (activeRenderPassUsesSampledTexture) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            activeRenderPass = nullptr;
        }
        Bool needSampledTextureTransitions = false;
        auto& sampledResources = m_sampledResourcesScratch;
        sampledResources.assign(sampledTextures.size(), nullptr);
        for (SizeT sampledIndex = 0; sampledIndex < sampledTextures.size(); ++sampledIndex) {
            auto* sampledTexture = sampledTextures[sampledIndex];
            if (!sampledTexture) {
                continue;
            }

            auto* textureResource = m_textureManager->SyncTextureAndGetDescriptor(*sampledTexture);
            MOBILEGL_ASSERT(textureResource != nullptr,
                            "%s: SyncTextureAndGetDescriptor failed for textureId=%d",
                            __func__, sampledTexture->GetExternalIndex());
            sampledResources[sampledIndex] = textureResource;
            MGLOG_D("SetupDraw: sampled textureId=%d layout(before)=%s(%d)",
                    sampledTexture->GetExternalIndex(), VkImageLayoutToString(textureResource->layout),
                    static_cast<Int>(textureResource->layout));
            if (m_clearManager->HasPendingClear(sampledTexture) ||
                !IsValidSampledImageLayout(textureResource->layout)) {
                // Out-of-pass work is needed (deferred clear materialization or
                // a sampled-layout transition). When the open frame recording
                // has not referenced this image yet, that work can execute
                // ahead of the WHOLE recording - record it into the pre-pass
                // stream instead of splitting the active render pass (ANGLE's
                // outside-render-pass command stream, restricted to the
                // provably reorderable case).
                if (activeRenderPass != nullptr &&
                    !m_frameContext.GetCurrent().hasPreCommandBufferRecorded &&
                    !m_textureManager->WasTouchedThisRecording(*textureResource)) {
                    VkCommandBuffer preCommandBuffer = m_frameContext.BeginPreCommandRecording();
                    const Bool preClearReady =
                        MaterializePendingClearForTexture(preCommandBuffer, *sampledTexture);
                    MOBILEGL_ASSERT(preClearReady,
                                    "%s: pre-pass MaterializePendingClearForTexture failed for textureId=%d",
                                    __func__, sampledTexture->GetExternalIndex());
                    const Bool preTransitionReady =
                        m_textureManager->TransitionTextureForSampling(preCommandBuffer, *sampledTexture);
                    MOBILEGL_ASSERT(preTransitionReady,
                                    "%s: pre-pass TransitionTextureForSampling failed for textureId=%d",
                                    __func__, sampledTexture->GetExternalIndex());
                    continue;
                }
                needSampledTextureTransitions = true;
            }
        }

        if (activeRenderPass && needSampledTextureTransitions) {
            MGLOG_D("SetupDraw: ending active render pass before sampled texture transitions");
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            activeRenderPass = nullptr;
        }

        for (SizeT sampledIndex = 0; sampledIndex < sampledTextures.size(); ++sampledIndex) {
            auto* sampledTexture = sampledTextures[sampledIndex];
            if (!sampledTexture) {
                continue;
            }
            // Fast path: the first loop already resolved this texture, nothing
            // is pending against it, and its layout is still sampleable (the
            // layout re-check covers an EndRenderPass between the loops having
            // rewritten an attachment's layout). Skipping the materialize +
            // transition + re-resolve chain here is the difference between one
            // pointer read and three calls per sampled texture per draw.
            if (auto* fastResource = sampledResources[sampledIndex];
                fastResource != nullptr && !m_clearManager->HasPendingClear(sampledTexture) &&
                IsValidSampledImageLayout(fastResource->layout)) {
                m_textureManager->StampResourceRecordingUse(*fastResource);
                continue;
            }
            const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sampledTexture);
            MOBILEGL_ASSERT(clearReady, "%s: MaterializePendingClearForTexture failed for textureId=%d",
                            __func__, sampledTexture->GetExternalIndex());
            const Bool ready = m_textureManager->TransitionTextureForSampling(frame.commandBuffer, *sampledTexture);
            MOBILEGL_ASSERT(ready, "%s: TransitionTextureForSampling failed for textureId=%d",
                            __func__, sampledTexture->GetExternalIndex());
            auto* transitionedResource = m_textureManager->SyncTextureAndGetDescriptor(*sampledTexture);
            MOBILEGL_ASSERT(transitionedResource != nullptr,
                            "%s: post-transition SyncTextureAndGetDescriptor failed for textureId=%d",
                            __func__, sampledTexture->GetExternalIndex());
            // Pre-pass stream bookkeeping: the draw about to be recorded reads
            // this image, so later out-of-pass work on it can no longer jump
            // ahead of the recording.
            m_textureManager->StampResourceRecordingUse(*transitionedResource);
            MGLOG_D("SetupDraw: sampled textureId=%d layout(after)=%s(%d)",
                    sampledTexture->GetExternalIndex(), VkImageLayoutToString(transitionedResource->layout),
                    static_cast<Int>(transitionedResource->layout));
        }

        // Depth/stencil participation of THIS draw, for the default-FBO depth-less
        // pass flavor (GL: a disabled depth/stencil test neither reads nor writes
        // its buffer).
        const Bool drawUsesDepthStencil =
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::DepthTest) ||
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::StencilTest);
        auto* renderPassEntry =
            &m_renderPassManager->GetOrCreateRenderPass(*drawFbo, m_imageIndexAcquired, drawUsesDepthStencil);
        if (activeRenderPass && !activeRenderPass->CompatibleWith(*renderPassEntry)) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            activeRenderPass = nullptr;
            renderPassEntry =
                &m_renderPassManager->GetOrCreateRenderPass(*drawFbo, m_imageIndexAcquired, drawUsesDepthStencil);
        }
        if (renderPassEntry->attachmentCount == 0 || renderPassEntry->extent.x() <= 0 || renderPassEntry->extent.y() <= 0) {
            MGLOG_D("SetupDraw skipped: drawFbo=%u resolved to an empty render pass (attachmentCount=%u extent=%dx%d)",
                    drawFbo->GetExternalIndex(),
                    renderPassEntry->attachmentCount,
                    renderPassEntry->extent.x(),
                    renderPassEntry->extent.y());
            return false;
        }

        // Vertex-input pre-flight, run before pipeline creation so that a bad attribute can never be
        // baked into a cached VkPipeline.
        {
            const auto& vertexInputState = m_vertexInputStateFactory->GetOrCreateVertexInputState(vao);
            const Uint32 activeAttribMask = programObj.activeVertexInputLocationMask;

            // An enabled array whose GL type has no VkFormat mapping never reaches the vertex input
            // state, which makes it indistinguishable from a disabled array: the draw would treat it as
            // "missing" and silently feed the shader the current attribute value instead of the app's
            // vertex data. Fail loudly rather than render wrong pixels.
            const Uint32 brokenAttribMask = vertexInputState.unsupportedAttribMask & activeAttribMask;
            if (brokenAttribMask != 0) {
                MGLOG_E_ONCE("SetupDraw skipped: program=%u reads vertex attribute location mask 0x%x whose enabled "
                        "array has no supported vertex format",
                        program.GetExternalIndex(), brokenAttribMask);
                return false;
            }

            // Every genuinely disabled attribute the shader reads must have a current-value type we can
            // synthesize a binding for; otherwise the upload below would push a null payload.
            const Uint32 missingAttribMask =
                activeAttribMask & ~vertexInputState.attributeLocationMask;
            for (Uint32 location = 0; location < kMaxVertexAttribs; ++location) {
                if ((missingAttribMask & (1u << location)) == 0) continue;

                const GLenum glType = programObj.vertexInputTypes[location];
                if (MG_State::GLState::ClassifyVertexAttribType(glType).baseType ==
                    MG_State::GLState::VertexAttribBaseType::Unsupported) {
                    MGLOG_E_ONCE("SetupDraw skipped: program=%u location=%u has no enabled array and its shader input "
                            "type 0x%x is not supported as a current generic vertex attribute",
                            program.GetExternalIndex(), location, glType);
                    return false;
                }
            }
        }

        auto pipeline = GetOrCreatePipeline(mode, program, programObj, transformFlags, vao, *renderPassEntry);
        // GetOrCreatePipeline documents a VK_NULL_HANDLE return (empty stages, or a driver that
        // rejected vkCreateGraphicsPipelines). Binding it dereferences null inside the driver -
        // 9 of the 15 CTS process deaths were exactly this vkCmdBindPipeline. A draw that has no
        // pipeline is a skipped draw, which is what every other failure below already does.
        // MGLOG_E, latched: the condition is a property of the program, so an unlatched line
        // here is one per draw forever. Parked at MGLOG_I until the Log.h ordering was fixed.
        if (pipeline == VK_NULL_HANDLE) {
            MGLOG_E_ONCE("SetupDraw skipped: no graphics pipeline for program=%u (creation failed or the "
                    "program has no shader stages)",
                    program.GetExternalIndex());
            return false;
        }
        activeRenderPass = VkRenderPassManager::GetActiveRenderPass();

        // Begin render pass, and handle clear
        if (activeRenderPass && activeRenderPass->CompatibleWith(*renderPassEntry)) {
            ClearAttachmentsOnActiveRenderPass(frame.commandBuffer, *renderPassEntry);
        } else {
            // No active render pass or active one not compatible.
            // Restart a new render pass
            Bool ok = VkRenderPassManager::BeginRenderPass(frame.commandBuffer, *renderPassEntry);
            MOBILEGL_ASSERT(ok, "%s: BeginRenderPass failed", __func__);
        }

        if (!g_dynamicStateShadow.graphicsPipelineValid || g_dynamicStateShadow.graphicsPipeline != pipeline) {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            g_dynamicStateShadow.graphicsPipelineValid = true;
            g_dynamicStateShadow.graphicsPipeline = pipeline;
        }

        const Bool boundUniforms = m_uniformManager->BindProgramUniformBuffers(
            frame.commandBuffer, program, programObj, m_frameContext.GetCurrentFrameIndex(),
            VK_PIPELINE_BIND_POINT_GRAPHICS, nullptr, false,
            m_samplerImageBindingOverridesScratch.empty() ? nullptr : &m_samplerImageBindingOverridesScratch);
        if (!boundUniforms) {
            MGLOG_E_ONCE("SetupDraw skipped: BindProgramUniformBuffers failed");
            return false;
        }

        auto vtxUploadOk = UploadAndBindVertexBuffers(
            frame.commandBuffer, vao, programObj, drawParams, pIndexBufferView);
        if (!vtxUploadOk) {
            MGLOG_E_ONCE("SetupDraw skipped: failed to upload vertex buffers");
            return false;
        }

        if (aspects & DrawSetupAspect::IndexBuffer) {
            auto idxUploadOk = UploadAndBindIndexBuffer(frame, vao, pIndexBufferView);
            MOBILEGL_ASSERT(idxUploadOk, "SetupDraw skipped: failed to upload index buffer");
        }

        ApplyDynamicDrawStateTail(frame, renderPassEntry->extent, drawFbo->IsDefaultFramebuffer(),
                                  ResolveDrawViewportCount(programObj.writesViewportIndexBuiltin));

        // Snapshot the fully resolved configuration for the consecutive-draw
        // fast path (see TrySetupDrawFastPath).
        {
            auto& snap = *fillSnap;
            const auto* nowActiveRenderPass = VkRenderPassManager::GetActiveRenderPass();
            if (nowActiveRenderPass != nullptr && !programObj.hasStorageImages) {
                snap.valid = true;
                snap.aspects = aspects.GetRaw();
                snap.mode = mode;
                snap.programLifetimeId = program.GetLifetimeId();
                snap.programVersion = program.GetBackendStateVersion();
                snap.vao = &vao;
                snap.vaoLifetimeId = vao.GetLifetimeId();
                snap.vaoConfigVersion = vao.GetConfigVersion();
                snap.drawFbo = drawFbo.get();
                snap.drawFboLifetimeId = drawFbo->GetLifetimeId();
                snap.fboVersion = drawFbo->GetObjectVersion();
                snap.drawFboIsDefault = drawFboIsDefault;
                snap.viewportCount = ResolveDrawViewportCount(programObj.writesViewportIndexBuiltin);
                snap.renderStateVersion = MG_State::pGLContext->GetPipelineStateVersion();
                snap.bindGeneration = MG_State::pGLContext->GetTextureBindGeneration();
                snap.baseTransformFlags = GetBaseTransformFlagsRaw(drawFboIsDefault);
                snap.resolvedTransformFlags = transformFlags.GetRaw();
                snap.renderPassHash = nowActiveRenderPass->hash;
                snap.imageIndex = m_imageIndexAcquired;
                snap.textureEraseEpoch = m_textureManager->GetResourceEraseEpoch();
                snap.textureImageEpoch = m_textureManager->GetTextureImageEpoch();
                snap.renderbufferImageEpoch = m_renderPassManager->GetRenderbufferImageEpoch();
                snap.drawUsesDepthStencil = drawUsesDepthStencil;
                snap.renderPassExtent = renderPassEntry->extent;
                snap.renderPassColorCount = renderPassEntry->colorAttachmentCount;
                snap.pipeline = pipeline;
                // The layout identity the fast path's aux-memo compare answers against.
                // A memo hit here, not a rebuild: the pre-flight above resolved this
                // VAO's entry already, so this re-reads the VAO's stamped state memo.
                snap.vaoLayoutHash = m_vertexInputStateFactory->GetOrCreateVertexInputState(vao).layoutHash;
                // Entry pointer memo: only when nothing since the resolve restructured
                // the factory cache (see programFactoryEpochAtResolve above).
                if (m_programFactory->GetCacheStructureEpoch() == programFactoryEpochAtResolve) {
                    snap.programObj = &programObj;
                    snap.programFactoryEpoch = programFactoryEpochAtResolve;
                } else {
                    snap.programObj = nullptr;
                    snap.programFactoryEpoch = 0;
                }
                snap.samplingResolutionGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
                Uint64 snapContentSum = 0;
                Uint64 snapParamsSum = 0;
                // Per-entry copies of this draw's sampled set (the scratch vectors
                // will be overwritten by the next full-path draw of ANY program).
                // Record each resource's layout VALUE for the descriptor-reuse hint;
                // transitions above updated the resources in place, so this reads the
                // layouts the descriptors just resolved against.
                snap.sampledTextures = sampledTextures;
                snap.sampledResources = sampledResources;
                snap.sampledBindingRecords = m_sampledBindingRecordsScratch;
                snap.sampledLayouts.assign(sampledTextures.size(), VK_IMAGE_LAYOUT_UNDEFINED);
                for (SizeT i = 0; i < sampledTextures.size(); ++i) {
                    const auto* sampledTexture = sampledTextures[i];
                    if (sampledTexture == nullptr) {
                        continue;
                    }
                    snapContentSum += sampledTexture->GetContentVersion();
                    snapParamsSum += sampledTexture->GetTextureParamsVersion();
                    if (sampledResources[i] != nullptr) {
                        snap.sampledLayouts[i] = sampledResources[i]->layout;
                    }
                }
                snap.sampledContentSum = snapContentSum;
                snap.sampledParamsSum = snapParamsSum;
            } else {
                snap.valid = false;
            }
        }
        return true;
    }

    void VulkanRenderer::DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
        m_textureManager->CollectGarbage();
        auto& frame = m_frameContext.GetCurrent();
        // The DISPATCH accessor: with a pipeline bound this is its compute stage program
        // itself, never the graphics composite (which carries no compute stage at all).
        const auto& program = *MG_State::pGLContext->GetProgramForDispatch();
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) {
            MGLOG_E_ONCE("DispatchCompute skipped: program=%u has no optimized SPIR-V",
                    program.GetExternalIndex());
            return;
        }
        ProgramFactory::CompileOptionFlags transformFlags = 0;
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, transformFlags);

        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }

        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        if (!PrepareStorageImageTextures(frame, program, programObj)) {
            MGLOG_E_ONCE("DispatchCompute skipped: storage image preparation failed");
            return;
        }
        if (!PrepareSamplerImageFeedbackSnapshots(frame, program, programObj,
                                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
            MGLOG_E_ONCE("DispatchCompute skipped: sampler/image feedback snapshot failed");
            return;
        }

        const VkPipeline pipeline = GetOrCreateComputePipeline(programObj);
        if (pipeline == VK_NULL_HANDLE) {
            MGLOG_E_ONCE("DispatchCompute skipped: compute pipeline creation failed for program=%u",
                    program.GetExternalIndex());
            return;
        }

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const Bool boundUniforms = m_uniformManager->BindProgramUniformBuffers(
            frame.commandBuffer, program, programObj, m_frameContext.GetCurrentFrameIndex(),
            VK_PIPELINE_BIND_POINT_COMPUTE, nullptr, false,
            m_samplerImageBindingOverridesScratch.empty() ? nullptr : &m_samplerImageBindingOverridesScratch);
        if (!boundUniforms) {
            MGLOG_E_ONCE("DispatchCompute skipped: BindProgramUniformBuffers failed");
            return;
        }

        MGLOG_D("DirectVulkan: glDispatchCompute(%u, %u, %u)", numGroupsX, numGroupsY, numGroupsZ);
        vkCmdDispatch(frame.commandBuffer, numGroupsX, numGroupsY, numGroupsZ);
    }

    void VulkanRenderer::DispatchComputeIndirect(GLintptr indirect) {
        m_textureManager->CollectGarbage();
        auto& frame = m_frameContext.GetCurrent();
        // See DispatchCompute: the dispatch accessor, not the draw one.
        const auto& program = *MG_State::pGLContext->GetProgramForDispatch();
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) {
            MGLOG_E_ONCE("DispatchComputeIndirect skipped: program=%u has no optimized SPIR-V",
                    program.GetExternalIndex());
            return;
        }
        ProgramFactory::CompileOptionFlags transformFlags = 0;
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, transformFlags);

        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }

        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        if (!PrepareStorageImageTextures(frame, program, programObj)) {
            MGLOG_E_ONCE("DispatchComputeIndirect skipped: storage image preparation failed");
            return;
        }
        if (!PrepareSamplerImageFeedbackSnapshots(frame, program, programObj,
                                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
            MGLOG_E_ONCE("DispatchComputeIndirect skipped: sampler/image feedback snapshot failed");
            return;
        }

        const VkPipeline pipeline = GetOrCreateComputePipeline(programObj);
        if (pipeline == VK_NULL_HANDLE) {
            MGLOG_E_ONCE("DispatchComputeIndirect skipped: compute pipeline creation failed for program=%u",
                    program.GetExternalIndex());
            return;
        }

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const Bool boundUniforms = m_uniformManager->BindProgramUniformBuffers(
            frame.commandBuffer, program, programObj, m_frameContext.GetCurrentFrameIndex(),
            VK_PIPELINE_BIND_POINT_COMPUTE, nullptr, false,
            m_samplerImageBindingOverridesScratch.empty() ? nullptr : &m_samplerImageBindingOverridesScratch);
        if (!boundUniforms) {
            MGLOG_E_ONCE("DispatchComputeIndirect skipped: BindProgramUniformBuffers failed");
            return;
        }

        auto indirectBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DispatchIndirect).GetBoundObject();
        if (!indirectBuffer) {
            MGLOG_E_ONCE("DispatchComputeIndirect skipped: GL_DISPATCH_INDIRECT_BUFFER is not bound");
            return;
        }
        indirectBuffer->SyncPersistentMappedRange();

        BufferSlice slice{};
        if (!m_bufferManager.AcquireResidentSlice(BufferKind::Indirect, indirectBuffer, slice)) {
            MGLOG_E_ONCE("DispatchComputeIndirect skipped: failed to sync indirect dispatch buffer");
            return;
        }

        MGLOG_D("DirectVulkan: glDispatchComputeIndirect(offset=%zu)", static_cast<SizeT>(indirect));
        vkCmdDispatchIndirect(frame.commandBuffer, slice.buffer, slice.offset + static_cast<VkDeviceSize>(indirect));
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
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             1, &memoryBarrier, 0, nullptr, 0, nullptr);
    }

    VulkanRenderer::ScissoredClearPrep VulkanRenderer::PrepareScissoredClear(
            const MG_State::GLState::FramebufferObject& framebuffer, VkClearRect& outClearRect) {
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }

        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        auto* renderPassEntry = &m_renderPassManager->GetOrCreateRenderPass(framebuffer, m_imageIndexAcquired);
        if (renderPassEntry->attachmentCount == 0 ||
            renderPassEntry->extent.x() <= 0 || renderPassEntry->extent.y() <= 0) {
            return ScissoredClearPrep::NoOp;
        }

        VkClearRect clearRect{};
        clearRect.rect = framebuffer.IsDefaultFramebuffer()
            ? MakeDefaultFramebufferScissorRect(MG_State::pGLContext->GetScissorBox(),
                                                renderPassEntry->extent,
                                                m_swapchainObject.GetPreTransform())
            : MakeClampedScissorRect(MG_State::pGLContext->GetScissorBox(), renderPassEntry->extent);
        clearRect.baseArrayLayer = 0;
        // GL 3.3 §4.4.7: clearing a layered framebuffer clears every layer.
        clearRect.layerCount = renderPassEntry->layers;
        if (clearRect.rect.extent.width == 0 || clearRect.rect.extent.height == 0) {
            return ScissoredClearPrep::NoOp;
        }
        // A scissor that covers the whole target is a whole-surface clear; the deferred loadOp
        // path is equivalent and cheaper (no render pass churn, loadOp=CLEAR on tilers).
        if (clearRect.rect.offset.x == 0 && clearRect.rect.offset.y == 0 &&
            clearRect.rect.extent.width == static_cast<Uint32>(renderPassEntry->extent.x()) &&
            clearRect.rect.extent.height == static_cast<Uint32>(renderPassEntry->extent.y())) {
            return ScissoredClearPrep::NotNeeded;
        }

        if (activeRenderPass && !activeRenderPass->CompatibleWith(*renderPassEntry)) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            activeRenderPass = nullptr;
            // Re-resolve: ending the pass updates tracked attachment layouts, which feed the
            // entry's load ops and initial layouts.
            renderPassEntry = &m_renderPassManager->GetOrCreateRenderPass(framebuffer, m_imageIndexAcquired);
        }
        // A still-active pass is necessarily compatible here: the block above ended any
        // incompatible one and nothing since can change the active pass.
        if (activeRenderPass) {
            // Materialize any older whole-attachment clear before applying this
            // ordered, scissored clear.
            ClearAttachmentsOnActiveRenderPass(frame.commandBuffer, *renderPassEntry);
        } else {
            const Bool began = VkRenderPassManager::BeginRenderPass(frame.commandBuffer, *renderPassEntry);
            MOBILEGL_ASSERT(began, "%s: BeginRenderPass failed", __func__);
            if (!began) {
                return ScissoredClearPrep::NoOp;
            }
        }
        outClearRect = clearRect;
        return ScissoredClearPrep::Ready;
    }

    void VulkanRenderer::Clear(GLbitfield mask) {
        m_clearManager->CollectGarbage();
        if ((mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) == 0) {
            return;
        }
        // GL 3.3 §3.1: when RASTERIZER_DISCARD is enabled, Clear and ClearBuffer* are ignored.
        if (MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::RasterizerDiscard)) {
            return;
        }
        auto* fbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject().get();
        MOBILEGL_ASSERT(fbo, "VulkanRenderer::Clear: draw framebuffer not found (fbo == nullptr)");
        if (IsUnsupportedFramebufferForDirectVulkan(*fbo)) {
            RecordUnsupportedFramebufferError(__func__);
            return;
        }

        ClearFramebufferPayload payload {
            .color = MG_State::pGLContext->GetClearColor(),
            .depth = MG_State::pGLContext->GetClearDepth(),
            .stencil = MG_State::pGLContext->GetClearStencil()
        };

        // A render-pass loadOp clear always covers the complete attachment, while
        // OpenGL glClear is clipped by GL_SCISSOR_TEST. Blaze3D relies on this for
        // GuiItemAtlas: animated items clear only their atlas slot before being
        // redrawn. Queueing that clear as a loadOp erases every cached static item.
        if (MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest)) {
            VkClearRect clearRect{};
            switch (PrepareScissoredClear(*fbo, clearRect)) {
            case ScissoredClearPrep::NoOp:
                return;
            case ScissoredClearPrep::NotNeeded:
                break;  // full-coverage scissor: the deferred whole-surface path below is equivalent
            case ScissoredClearPrep::Ready: {
                VkClearAttachment clearAttachments[MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS + 1];
                Uint32 clearAttachmentCount = 0;

                if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
                    const auto& drawBuffers = fbo->GetDrawBuffers();
                    for (Uint32 drawBufferIndex = 0; drawBufferIndex < drawBuffers.size(); ++drawBufferIndex) {
                        const auto attachmentType = drawBuffers[drawBufferIndex];
                        if (attachmentType == FramebufferAttachmentType::None) {
                            continue;
                        }
                        const auto& attachment = fbo->GetAttachment(attachmentType);
                        if (!attachment.IsComplete()) {
                            continue;
                        }

                        const BoolVec4 colorMask = MG_State::pGLContext->GetColorMaskIndexed(drawBufferIndex);
                        if (!colorMask.r() && !colorMask.g() && !colorMask.b() && !colorMask.a()) {
                            continue;
                        }
                        if (!colorMask.r() || !colorMask.g() || !colorMask.b() || !colorMask.a()) {
                            MGLOG_W_ONCE("DirectVulkan: scissored glClear with a partial color mask is not supported");
                            continue;
                        }

                        MG_State::GLState::ITextureObject* colorTexture = nullptr;
                        if (attachment.IsTexture()) {
                            colorTexture = attachment.GetTexture().get();
                        }
                        VkClearAttachment clearAttachment{};
                        clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        clearAttachment.colorAttachment = drawBufferIndex;
                        // glClear only ever supplies float values (ClearFramebufferPayload has no
                        // other form), so the float member is always the right one here.
                        clearAttachment.clearValue.color = {
                            payload.color.x(), payload.color.y(), payload.color.z(),
                            ColorFormatLacksAlpha(colorTexture) ? 1.0f : payload.color.w()
                        };
                        clearAttachments[clearAttachmentCount++] = clearAttachment;
                    }
                }

                VkImageAspectFlags depthStencilAspects = 0;
                if ((mask & GL_DEPTH_BUFFER_BIT) != 0 && MG_State::pGLContext->GetDepthMask()) {
                    const auto& depthAttachment = fbo->GetAttachment(FramebufferAttachmentType::Depth);
                    if (depthAttachment.IsComplete()) {
                        depthStencilAspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
                    }
                }
                if ((mask & GL_STENCIL_BUFFER_BIT) != 0) {
                    const auto& stencilAttachment = fbo->GetAttachment(FramebufferAttachmentType::Stencil);
                    if (stencilAttachment.IsComplete()) {
                        // GL 3.3 §4.2.3: the clear is masked by the front stencil write mask.
                        // vkCmdClearAttachments writes every bit, so only a full (8-bit stencil) or
                        // zero mask can be expressed; treat a partial mask like a partial color mask.
                        const Uint32 stencilWriteMask =
                            MG_State::pGLContext->GetStencilState(StencilFace::Front).WriteMask;
                        if ((stencilWriteMask & 0xFFu) == 0xFFu) {
                            depthStencilAspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
                        } else if (stencilWriteMask != 0) {
                            MGLOG_W_ONCE("DirectVulkan: scissored glClear with a partial stencil write mask is not supported");
                        }
                    }
                }
                if (depthStencilAspects != 0) {
                    VkClearAttachment clearAttachment{};
                    clearAttachment.aspectMask = depthStencilAspects;
                    clearAttachment.clearValue.depthStencil = {payload.depth, payload.stencil};
                    clearAttachments[clearAttachmentCount++] = clearAttachment;
                }

                if (clearAttachmentCount != 0) {
                    vkCmdClearAttachments(m_frameContext.GetCurrent().commandBuffer,
                                          clearAttachmentCount, clearAttachments,
                                          1, &clearRect);
                }
                return;
            }
            }
        }

        // GL 3.3 §4.2.3: glClear honors the write masks. Mirror the scissored path's
        // gating for the deferred path: drop fully-masked planes, warn on partial
        // masks vkCmdClear*/loadOp clears cannot express.
        GLbitfield deferredMask = mask;
        if ((deferredMask & GL_DEPTH_BUFFER_BIT) != 0 && !MG_State::pGLContext->GetDepthMask()) {
            deferredMask &= ~static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT);
        }
        if ((deferredMask & GL_STENCIL_BUFFER_BIT) != 0) {
            const Uint32 stencilWriteMask = MG_State::pGLContext->GetStencilState(StencilFace::Front).WriteMask;
            if ((stencilWriteMask & 0xFFu) != 0xFFu) {
                if (stencilWriteMask != 0) {
                    MGLOG_W_ONCE("DirectVulkan: deferred glClear with a partial stencil write mask is not supported");
                }
                deferredMask &= ~static_cast<GLbitfield>(GL_STENCIL_BUFFER_BIT);
            }
        }
        if ((deferredMask & GL_COLOR_BUFFER_BIT) != 0) {
            const auto& drawBuffers = fbo->GetDrawBuffers();
            Bool anyFullMask = false;
            Bool anyRestrictedMask = false;
            for (Uint32 drawBufferIndex = 0; drawBufferIndex < drawBuffers.size(); ++drawBufferIndex) {
                if (drawBuffers[drawBufferIndex] == FramebufferAttachmentType::None) {
                    continue;
                }
                const BoolVec4 colorMask = MG_State::pGLContext->GetColorMaskIndexed(drawBufferIndex);
                const Bool full = colorMask.r() && colorMask.g() && colorMask.b() && colorMask.a();
                if (full) {
                    anyFullMask = true;
                } else {
                    anyRestrictedMask = true;
                    if (colorMask.r() || colorMask.g() || colorMask.b() || colorMask.a()) {
                        MGLOG_W_ONCE("DirectVulkan: deferred glClear with a partial color mask is not supported");
                    }
                }
            }
            if (!anyFullMask) {
                deferredMask &= ~static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
            } else if (anyRestrictedMask) {
                // Mixed per-buffer masks: queue only the fully-writable texture targets
                // individually and drop the framebuffer-level color clear.
                for (Uint32 drawBufferIndex = 0; drawBufferIndex < drawBuffers.size(); ++drawBufferIndex) {
                    const auto attachmentType = drawBuffers[drawBufferIndex];
                    if (attachmentType == FramebufferAttachmentType::None) {
                        continue;
                    }
                    const BoolVec4 colorMask = MG_State::pGLContext->GetColorMaskIndexed(drawBufferIndex);
                    if (!(colorMask.r() && colorMask.g() && colorMask.b() && colorMask.a())) {
                        continue;
                    }
                    const auto& attachment = fbo->GetAttachment(attachmentType);
                    if (attachment.IsRenderbuffer()) {
                        m_renderPassManager->QueueRenderbufferClear(
                            {.mask = GL_COLOR_BUFFER_BIT, .color = payload.color}, attachment);
                    } else if (attachment.IsTexture()) {
                        m_clearManager->QueueClear({.mask = GL_COLOR_BUFFER_BIT, .color = payload.color},
                                                   attachment);
                    }
                }
                deferredMask &= ~static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
            }
        }
        if (deferredMask == 0) {
            return;
        }

        m_clearManager->QueueClear(deferredMask, payload, *fbo);
        m_renderPassManager->QueueRenderbufferClear(deferredMask, payload, *fbo);
    }

    void VulkanRenderer::QueueClearBufferPayloadForFramebuffer(
            const MG_State::GLState::FramebufferObject& framebuffer, GLenum buffer, GLint drawbuffer,
            const ClearAttachmentPayload& clearPayload) {
        m_clearManager->CollectGarbage();
        // GL 3.3 §3.1: when RASTERIZER_DISCARD is enabled, Clear and ClearBuffer* are ignored.
        if (MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::RasterizerDiscard)) {
            return;
        }
        if (IsUnsupportedFramebufferForDirectVulkan(framebuffer)) {
            RecordUnsupportedFramebufferError(__func__);
            return;
        }

        // Validate (buffer, drawbuffer) up front so GL errors fire regardless of which clear
        // path is taken below.
        switch (buffer) {
            case GL_COLOR:
                if (drawbuffer < 0 ||
                    drawbuffer >= static_cast<GLint>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS)) {
                    RecordClearBufferError(__func__, ErrorCode::InvalidValue, "color drawbuffer index is out of range");
                    return;
                }
                break;
            case GL_DEPTH:
                if (drawbuffer != 0) {
                    RecordClearBufferError(__func__, ErrorCode::InvalidValue, "depth clear requires drawbuffer 0");
                    return;
                }
                break;
            case GL_STENCIL:
                if (drawbuffer != 0) {
                    RecordClearBufferError(__func__, ErrorCode::InvalidValue, "stencil clear requires drawbuffer 0");
                    return;
                }
                break;
            case GL_DEPTH_STENCIL:
                if (drawbuffer != 0) {
                    RecordClearBufferError(__func__, ErrorCode::InvalidValue, "depth/stencil clear requires drawbuffer 0");
                    return;
                }
                break;
            default:
                RecordClearBufferError(__func__, ErrorCode::InvalidEnum, "unsupported clear buffer target");
                return;
        }

        // GL 3.3 §4.2.3: ClearBuffer* is clipped by GL_SCISSOR_TEST exactly like Clear.
        if (MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest)) {
            VkClearRect clearRect{};
            switch (PrepareScissoredClear(framebuffer, clearRect)) {
            case ScissoredClearPrep::NoOp:
                return;
            case ScissoredClearPrep::NotNeeded:
                break;  // full-coverage scissor: the deferred whole-surface path below is equivalent
            case ScissoredClearPrep::Ready:
                RecordScissoredClearBuffer(framebuffer, buffer, drawbuffer, clearPayload, clearRect);
                return;
            }
        }

        auto queueAttachmentClear = [&](FramebufferAttachmentType attachmentType,
                                        const ClearAttachmentPayload& payload) {
            if (attachmentType == FramebufferAttachmentType::None || payload.mask == 0) {
                return;
            }
            const auto& attachment = framebuffer.GetAttachment(attachmentType);
            if (attachment.IsRenderbuffer()) {
                m_renderPassManager->QueueRenderbufferClear(payload, attachment);
                return;
            }
            if (!attachment.IsTexture()) {
                return;
            }
            m_clearManager->QueueClear(payload, attachment);
        };

        // GL 3.3 §4.2.3: ClearBuffer* honors the write masks like Clear. Deferred
        // clears cannot express partial masks; warn and skip those.
        const auto depthClearAllowed = [&]() -> Bool { return MG_State::pGLContext->GetDepthMask(); };
        const auto stencilClearAllowed = [&]() -> Bool {
            const Uint32 stencilWriteMask = MG_State::pGLContext->GetStencilState(StencilFace::Front).WriteMask;
            if ((stencilWriteMask & 0xFFu) == 0xFFu) {
                return true;
            }
            if (stencilWriteMask != 0) {
                MGLOG_W_ONCE("DirectVulkan: deferred glClearBuffer with a partial stencil write mask is not supported");
            }
            return false;
        };

        switch (buffer) {
            case GL_COLOR: {
                const BoolVec4 colorMask = MG_State::pGLContext->GetColorMaskIndexed(static_cast<Uint32>(drawbuffer));
                if (!colorMask.r() && !colorMask.g() && !colorMask.b() && !colorMask.a()) {
                    return;
                }
                if (!(colorMask.r() && colorMask.g() && colorMask.b() && colorMask.a())) {
                    MGLOG_W_ONCE("DirectVulkan: deferred glClearBuffer with a partial color mask is not supported");
                    return;
                }
                queueAttachmentClear(framebuffer.GetDrawBuffers()[drawbuffer], clearPayload);
                return;
            }
            case GL_DEPTH:
                if (depthClearAllowed()) {
                    queueAttachmentClear(FramebufferAttachmentType::Depth, clearPayload);
                }
                return;
            case GL_STENCIL:
                if (stencilClearAllowed()) {
                    queueAttachmentClear(FramebufferAttachmentType::Stencil, clearPayload);
                }
                return;
            case GL_DEPTH_STENCIL: {
                ClearAttachmentPayload allowedPayload = clearPayload;
                if (!depthClearAllowed()) {
                    allowedPayload.mask &= ~static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT);
                }
                if (!stencilClearAllowed()) {
                    allowedPayload.mask &= ~static_cast<GLbitfield>(GL_STENCIL_BUFFER_BIT);
                }
                if ((allowedPayload.mask & GL_DEPTH_BUFFER_BIT) != 0) {
                    queueAttachmentClear(FramebufferAttachmentType::Depth, allowedPayload);
                }
                if ((allowedPayload.mask & GL_STENCIL_BUFFER_BIT) != 0) {
                    queueAttachmentClear(FramebufferAttachmentType::Stencil, allowedPayload);
                }
                return;
            }
            default:
                return;
        }
    }

    void VulkanRenderer::RecordScissoredClearBuffer(const MG_State::GLState::FramebufferObject& framebuffer,
                                                    GLenum buffer, GLint drawbuffer,
                                                    const ClearAttachmentPayload& clearPayload,
                                                    const VkClearRect& clearRect) {
        VkClearAttachment clearAttachment{};

        if (buffer == GL_COLOR) {
            const auto attachmentType = framebuffer.GetDrawBuffers()[drawbuffer];
            if (attachmentType == FramebufferAttachmentType::None) {
                return;
            }
            const auto& attachment = framebuffer.GetAttachment(attachmentType);
            if (!attachment.IsComplete()) {
                return;
            }
            const BoolVec4 colorMask = MG_State::pGLContext->GetColorMaskIndexed(static_cast<Uint>(drawbuffer));
            if (!colorMask.r() && !colorMask.g() && !colorMask.b() && !colorMask.a()) {
                return;
            }
            if (!colorMask.r() || !colorMask.g() || !colorMask.b() || !colorMask.a()) {
                MGLOG_W_ONCE("DirectVulkan: scissored glClearBuffer with a partial color mask is not supported");
                return;
            }
            MG_State::GLState::ITextureObject* colorTexture = nullptr;
            if (attachment.IsTexture()) {
                colorTexture = attachment.GetTexture().get();
            }
            clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clearAttachment.colorAttachment = static_cast<Uint32>(drawbuffer);
            clearAttachment.clearValue.color =
                MakeVkClearColorValue(clearPayload, ColorFormatLacksAlpha(colorTexture));
        } else {
            VkImageAspectFlags aspects = 0;
            if ((clearPayload.mask & GL_DEPTH_BUFFER_BIT) != 0 && MG_State::pGLContext->GetDepthMask() &&
                framebuffer.GetAttachment(FramebufferAttachmentType::Depth).IsComplete()) {
                aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
            }
            if ((clearPayload.mask & GL_STENCIL_BUFFER_BIT) != 0 &&
                framebuffer.GetAttachment(FramebufferAttachmentType::Stencil).IsComplete()) {
                // GL 3.3 §4.2.3: the clear is masked by the front stencil write mask (see Clear).
                const Uint32 stencilWriteMask =
                    MG_State::pGLContext->GetStencilState(StencilFace::Front).WriteMask;
                if ((stencilWriteMask & 0xFFu) == 0xFFu) {
                    aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
                } else if (stencilWriteMask != 0) {
                    MGLOG_W_ONCE("DirectVulkan: scissored glClearBuffer with a partial stencil write mask is not supported");
                }
            }
            if (aspects == 0) {
                return;
            }
            clearAttachment.aspectMask = aspects;
            clearAttachment.clearValue.depthStencil = {clearPayload.depth, clearPayload.stencil};
        }

        vkCmdClearAttachments(m_frameContext.GetCurrent().commandBuffer, 1, &clearAttachment, 1, &clearRect);
    }

    void VulkanRenderer::QueueClearBufferPayload(GLenum buffer, GLint drawbuffer,
                                                 const ClearAttachmentPayload& clearPayload) {
        auto* fbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject().get();
        if (!fbo) {
            return;
        }
        QueueClearBufferPayloadForFramebuffer(*fbo, buffer, drawbuffer, clearPayload);
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

    void VulkanRenderer::ClearNamedFramebufferfv(
            const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint drawbuffer,
            const GLfloat* value) {
        if (!framebuffer || value == nullptr) {
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
        QueueClearBufferPayloadForFramebuffer(*framebuffer, buffer, drawbuffer, payload);
    }

    // The integer clears carry the same payload as their target-based siblings; only the
    // destination differs, so they queue against the named framebuffer rather than the bound one.
    void VulkanRenderer::ClearNamedFramebufferiv(
            const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint drawbuffer,
            const GLint* value) {
        if (!framebuffer || value == nullptr) {
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
        QueueClearBufferPayloadForFramebuffer(*framebuffer, buffer, drawbuffer, payload);
    }

    void VulkanRenderer::ClearNamedFramebufferuiv(
            const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint drawbuffer,
            const GLuint* value) {
        if (!framebuffer || value == nullptr) {
            return;
        }
        ClearAttachmentPayload payload{};
        if (buffer == GL_COLOR) {
            payload.mask = GL_COLOR_BUFFER_BIT;
            payload.colorEncoding = ClearColorEncoding::Uint;
            payload.colorUint = UintVec4(value[0], value[1], value[2], value[3]);
        }
        QueueClearBufferPayloadForFramebuffer(*framebuffer, buffer, drawbuffer, payload);
    }

    void VulkanRenderer::ClearNamedFramebufferfi(
            const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer, GLint drawbuffer,
            GLfloat depth, GLint stencil) {
        if (!framebuffer) {
            return;
        }
        ClearAttachmentPayload payload{};
        payload.mask = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        // Vulkan clear values require depth in [0,1] (VUID-VkClearDepthStencilValue-depth-00022).
        payload.depth = std::clamp(depth, 0.0f, 1.0f);
        payload.stencil = static_cast<Uint32>(stencil);
        QueueClearBufferPayloadForFramebuffer(*framebuffer, buffer, drawbuffer, payload);
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

    Bool VulkanRenderer::ClearDepthSliceWithRenderPass(VkCommandBuffer commandBuffer,
                                                       MG_State::GLState::ITextureObject& texture, Uint32 mipLevel,
                                                       Uint32 depthSlice, const VkClearValue& clearValue) {
        auto* resource = m_textureManager->SyncTextureAndGetDescriptor(texture);
        if (resource == nullptr || resource->image == VK_NULL_HANDLE) return false;
        if (m_frameContext.GetCurrentFrameIndex() >= m_deferredDepthMipmapCleanup.size()) return false;

        // A 2D view over one z slice. Returns VK_NULL_HANDLE when the image is not
        // 2D-array-compatible, which is the whole reason this can fail.
        const VkImageView sliceView = m_textureManager->GetOrCreateAttachmentViewAtMipLevel(
            texture, mipLevel, depthSlice, 1, VK_IMAGE_VIEW_TYPE_2D);
        if (sliceView == VK_NULL_HANDLE) return false;

        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = resource->format;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Hand the slice back in the layout the caller already tracks for the whole image, so its
        // closing barrier stays truthful and resource->layout is never touched from in here.
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) return false;

        const Uint32 levelWidth = std::max(resource->extent.width >> mipLevel, 1u);
        const Uint32 levelHeight = std::max(resource->extent.height >> mipLevel, 1u);

        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &sliceView;
        framebufferInfo.width = levelWidth;
        framebufferInfo.height = levelHeight;
        framebufferInfo.layers = 1;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
            vkDestroyRenderPass(m_device, renderPass, nullptr);
            return false;
        }

        VkRenderPassBeginInfo beginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        beginInfo.renderPass = renderPass;
        beginInfo.framebuffer = framebuffer;
        beginInfo.renderArea.extent = {levelWidth, levelHeight};
        beginInfo.clearValueCount = 1;
        beginInfo.pClearValues = &clearValue;
        // The load op is the whole operation: begin and end with nothing in between.
        vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(commandBuffer);

        // The image view is owned and memoised by the texture resource; only these two are throwaway.
        auto& deferredCleanup = m_deferredDepthMipmapCleanup[m_frameContext.GetCurrentFrameIndex()];
        deferredCleanup.renderPasses.push_back(renderPass);
        deferredCleanup.framebuffers.push_back(framebuffer);
        return true;
    }

    Bool VulkanRenderer::MaterializePendingClearForTexture(VkCommandBuffer commandBuffer,
                                                           MG_State::GLState::ITextureObject& texture) {
        Vector<PendingClearEntry> pendingClears;
        if (!m_clearManager->GetPendingClears(&texture, pendingClears)) {
            return true;
        }
        // A pass may stay open on the FRAME command buffer while this clear is
        // recorded into the pre-pass stream (a different command buffer that
        // executes strictly before the frame's commands).
        MOBILEGL_ASSERT(VkRenderPassManager::GetActiveRenderPass() == nullptr ||
                            commandBuffer != m_frameContext.GetCurrent().commandBuffer,
                        "MaterializePendingClearForTexture requires no active render pass on the target buffer");

        auto* resource = m_textureManager->SyncTextureAndGetDescriptor(texture);
        MOBILEGL_ASSERT(resource != nullptr,
                        "MaterializePendingClearForTexture: SyncTextureAndGetDescriptor failed for textureId=%d",
                        texture.GetExternalIndex());

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(resource->layout, srcStageMask, srcAccessMask);

        VkImageLayout clearLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        Bool ok = VkTextureManager::TransitionImageLayout(
            commandBuffer, resource->image, resource->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
            resource->aspect, 0, resource->mipLevels);
        MOBILEGL_ASSERT(ok,
                        "MaterializePendingClearForTexture: failed to transition textureId=%d to TRANSFER_DST",
                        texture.GetExternalIndex());

        VkImageLayout sampledLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (const auto& pendingClear : pendingClears) {
            MOBILEGL_ASSERT(pendingClear.key.mipLevel < resource->mipLevels,
                            "MaterializePendingClearForTexture: textureId=%d pending clear mip=%u out of range %u",
                            texture.GetExternalIndex(), pendingClear.key.mipLevel, resource->mipLevels);
            // FIXME: a layered clear of a GL_TEXTURE_3D texture still reads back wrong.
            // KHR-GL44/45/46.geometry_shader.layered_framebuffer.clear_call_support fails on
            // DirectVulkan: it attaches a 4-deep 3D texture with glFramebufferTexture (layered),
            // clears with glClearBufferiv, then reads each slice back through
            // glFramebufferTextureLayer and gets zeros. Those cases exist only in the GL44+ lists,
            // above the 4.0 this backend reports, so they are outside the current conformance
            // claim - but the feature (layered attachment, GL 3.2) is not, so an application can
            // reach this.
            //
            // Already ruled out by bisecting with temporary bypasses, so do not re-test these:
            //   - the per-slice render-pass clear below (disabling it changes nothing)
            //   - the per-target gate in FramebufferTextureLayer_State (it already permits
            //     Texture3D here; bypassing it changes nothing)
            //   - VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT on the 3D image (not requesting it
            //     changes nothing)
            // What IS fixed here is the subresource range below: a layered GL clear queues
            // layerCount = depth, which is illegal for a VK_IMAGE_TYPE_3D image, and the old code
            // passed it straight through - running the case standalone against the previous build
            // trips MOBILEGL_ASSERT(baseArrayLayer + layerCount <= arrayLayers) as 0 + 4 <= 1.
            //
            // Note when picking this up: the case does not reproduce standalone the way it behaves
            // in a batch run (batch passed before this change, standalone asserted), so it depends
            // on state left by earlier cases. Reproduce it inside a chunk, not on its own.
            //
            // A 3D image keeps its GL layers on the z axis (arrayLayers == 1), so the pending
            // clear's "layer" is a slice index bounded by the mip level's depth.
            const Bool clearAddressesDepthSlices = resource->viewType == VK_IMAGE_VIEW_TYPE_3D;
            const Uint32 clearableLayers = clearAddressesDepthSlices
                                               ? std::max(resource->depth >> pendingClear.key.mipLevel, 1u)
                                               : resource->arrayLayers;
            MOBILEGL_ASSERT(pendingClear.key.baseArrayLayer + pendingClear.key.layerCount <= clearableLayers,
                            "MaterializePendingClearForTexture: textureId=%d pending clear layer span [%u, %u) exceeds %u",
                            texture.GetExternalIndex(), pendingClear.key.baseArrayLayer,
                            pendingClear.key.baseArrayLayer + pendingClear.key.layerCount, clearableLayers);
            // Whether this clear names a strict SUBSET of the level. A layered attachment
            // (glFramebufferTexture) queues layerCount = the whole depth, a single-slice one
            // (glFramebufferTextureLayer) queues 1 - so the key already distinguishes them, and it is
            // the clear's span that decides, not the image's slice count. Reading the latter sent a
            // layered clear of a 3D texture down the per-slice path, where it cleared slice zero and
            // left the rest stale (geometry_shader.layered_framebuffer.clear_call_support).
            const Bool clearsWholeLevel =
                pendingClear.key.baseArrayLayer == 0 && pendingClear.key.layerCount >= clearableLayers;
            if (clearAddressesDepthSlices && clearableLayers > 1 && !clearsWholeLevel) {
                // vkCmdClearColorImage cannot clear a subset of a 3D image's slices:
                // VUID-vkCmdClearColorImage-baseArrayLayer-01472 pins baseArrayLayer to 0 and
                // layerCount to 1 for VK_IMAGE_TYPE_3D, i.e. the whole mip level. A render pass whose
                // only content is its LOAD_OP_CLEAR does address exactly one slice, because its
                // attachment is a 2D view over that slice.
                auto clearPayload3D = pendingClear.payload;
                PreCompensateSrgbClearColor(clearPayload3D, resource->format);
                VkClearValue sliceClearValue{};
                sliceClearValue.color = MakeVkClearColorValue(clearPayload3D, ColorFormatLacksAlpha(&texture));
                if (!ClearDepthSliceWithRenderPass(commandBuffer, texture, pendingClear.key.mipLevel,
                                                   pendingClear.key.baseArrayLayer, sliceClearValue)) {
                    // The device or the format refused VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, so
                    // there is no way to name this slice. Leaving it uncleared is wrong pixels;
                    // asserting would abort a process that glFramebufferTextureLayer can reach at will.
                    MGLOG_W_ONCE("MaterializePendingClearForTexture: textureId=%d slice %u could not be cleared "
                            "(no 2D-array-compatible view)",
                            texture.GetExternalIndex(), pendingClear.key.baseArrayLayer);
                }
                continue;
            }

            VkImageSubresourceRange subresourceRange{};
            subresourceRange.baseMipLevel = pendingClear.key.mipLevel;
            subresourceRange.levelCount = 1;
            // VUID-vkCmdClearColorImage-baseArrayLayer-01472: for a VK_IMAGE_TYPE_3D image the range
            // must name baseArrayLayer 0 and layerCount 1, which Vulkan reads as "the whole mip
            // level" - the z extent is not an array dimension. A layered GL clear queues
            // layerCount = depth, which is the right GL answer and an illegal Vulkan one.
            subresourceRange.baseArrayLayer = clearAddressesDepthSlices ? 0u : pendingClear.key.baseArrayLayer;
            subresourceRange.layerCount = clearAddressesDepthSlices ? 1u : pendingClear.key.layerCount;

            auto clearPayload = pendingClear.payload;
            if ((resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
                subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                PreCompensateSrgbClearColor(clearPayload, resource->format);
                const VkClearColorValue clearValue =
                    MakeVkClearColorValue(clearPayload, ColorFormatLacksAlpha(&texture));
                vkCmdClearColorImage(commandBuffer, resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &clearValue, 1, &subresourceRange);
            } else {
                VkImageAspectFlags clearAspectMask = 0;
                if ((resource->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 &&
                    (clearPayload.mask & GL_DEPTH_BUFFER_BIT) != 0) {
                    clearAspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
                }
                if ((resource->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
                    (clearPayload.mask & GL_STENCIL_BUFFER_BIT) != 0) {
                    clearAspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                }
                MOBILEGL_ASSERT(clearAspectMask != 0,
                                "MaterializePendingClearForTexture: textureId=%d has no matching depth/stencil clear mask",
                                texture.GetExternalIndex());
                subresourceRange.aspectMask = clearAspectMask;
                VkClearDepthStencilValue clearValue{};
                clearValue.depth = clearPayload.depth;
                clearValue.stencil = clearPayload.stencil;
                vkCmdClearDepthStencilImage(commandBuffer, resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            &clearValue, 1, &subresourceRange);
                sampledLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            }
        }

        ok = VkTextureManager::TransitionImageLayout(
            commandBuffer, resource->image, clearLayout, sampledLayout,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, resource->aspect, 0, resource->mipLevels);
        MOBILEGL_ASSERT(ok,
                        "MaterializePendingClearForTexture: failed to transition textureId=%d to sampled layout",
                        texture.GetExternalIndex());
        resource->layout = sampledLayout;

        m_clearManager->PopPendingClear(&texture);
        MGLOG_D("MaterializePendingClearForTexture: textureId=%d pending clear materialized",
                texture.GetExternalIndex());
        return true;
    }

    Bool VulkanRenderer::MaterializePendingClearForRenderbuffer(
        VkCommandBuffer commandBuffer, const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbuffer) {
        if (renderbuffer == nullptr) {
            return true;
        }
        ClearAttachmentPayload clearPayload{};
        if (!m_renderPassManager->GetPendingRenderbufferClear(renderbuffer.get(), clearPayload)) {
            return true;
        }
        MOBILEGL_ASSERT(VkRenderPassManager::GetActiveRenderPass() == nullptr,
                        "MaterializePendingClearForRenderbuffer requires no active render pass");

        auto* resource = m_renderPassManager->GetOrCreateRenderbufferResource(renderbuffer);
        if (resource == nullptr) {
            MGLOG_E_ONCE("MaterializePendingClearForRenderbuffer: no resource for renderbuffer %u",
                    renderbuffer->GetExternalIndex());
            return false;
        }

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(resource->layout, srcStageMask, srcAccessMask);

        Bool ok = VkTextureManager::TransitionImageLayout(
            commandBuffer, resource->image, resource->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
            resource->aspect, 0, 1);
        MOBILEGL_ASSERT(ok,
                        "MaterializePendingClearForRenderbuffer: failed to transition renderbuffer %u to TRANSFER_DST",
                        renderbuffer->GetExternalIndex());

        VkImageSubresourceRange subresourceRange{};
        subresourceRange.baseMipLevel = 0;
        subresourceRange.levelCount = 1;
        subresourceRange.baseArrayLayer = 0;
        subresourceRange.layerCount = 1;

        VkImageLayout steadyLayout;
        if ((resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
            subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            PreCompensateSrgbClearColor(clearPayload, resource->format);
            // RGB renderbuffers are backed by an RGBA image; the missing alpha reads as 1.
            const VkClearColorValue clearValue = MakeVkClearColorValue(
                clearPayload,
                MG_Util::GetBaseInternalFormatComponentCount(renderbuffer->GetInternalFormat()) == 3);
            vkCmdClearColorImage(commandBuffer, resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clearValue, 1, &subresourceRange);
            steadyLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
            VkImageAspectFlags clearAspectMask = 0;
            if ((resource->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 &&
                (clearPayload.mask & GL_DEPTH_BUFFER_BIT) != 0) {
                clearAspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            }
            if ((resource->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
                (clearPayload.mask & GL_STENCIL_BUFFER_BIT) != 0) {
                clearAspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            if (clearAspectMask != 0) {
                subresourceRange.aspectMask = clearAspectMask;
                VkClearDepthStencilValue clearValue{};
                clearValue.depth = clearPayload.depth;
                clearValue.stencil = clearPayload.stencil;
                vkCmdClearDepthStencilImage(commandBuffer, resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            &clearValue, 1, &subresourceRange);
            }
            steadyLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }

        VkImageLayout clearLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ok = VkTextureManager::TransitionImageLayout(
            commandBuffer, resource->image, clearLayout, steadyLayout,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_TRANSFER_READ_BIT,
            resource->aspect, 0, 1);
        MOBILEGL_ASSERT(ok,
                        "MaterializePendingClearForRenderbuffer: failed to transition renderbuffer %u to steady layout",
                        renderbuffer->GetExternalIndex());
        resource->layout = steadyLayout;

        m_renderPassManager->PopPendingRenderbufferClear(renderbuffer.get());
        MGLOG_D("MaterializePendingClearForRenderbuffer: renderbuffer %u pending clear materialized",
                renderbuffer->GetExternalIndex());
        return true;
    }

    void VulkanRenderer::DestroyMultisampleResolveScratchImage() {
        if (m_msResolveScratch.image != VK_NULL_HANDLE) {
            vmaDestroyImage(m_allocator, m_msResolveScratch.image, m_msResolveScratch.allocation);
        }
        m_msResolveScratch = {};
    }

    Bool VulkanRenderer::AcquireMultisampleResolveScratchImage(VkCommandBuffer commandBuffer, VkFormat format,
                                                               VkExtent2D extent) {
        if (extent.width == 0 || extent.height == 0 || format == VK_FORMAT_UNDEFINED) {
            return false;
        }
        // Grow-only, and never shrink: these blits repeat at one or two sizes, so the steady state
        // is one allocation for the whole process.
        if (m_msResolveScratch.image == VK_NULL_HANDLE || m_msResolveScratch.format != format ||
            m_msResolveScratch.extent.width < extent.width || m_msResolveScratch.extent.height < extent.height) {
            const VkExtent2D grown = {std::max(extent.width, m_msResolveScratch.extent.width),
                                      std::max(extent.height, m_msResolveScratch.extent.height)};
            DestroyMultisampleResolveScratchImage();

            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = {grown.width, grown.height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo allocationInfo{};
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            if (vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &m_msResolveScratch.image,
                               &m_msResolveScratch.allocation, nullptr) != VK_SUCCESS) {
                // Soft failure: the caller keeps the direct resolve, which is what shipped before.
                MGLOG_E_ONCE("AcquireMultisampleResolveScratchImage: vmaCreateImage failed (format=%d %ux%u)",
                        static_cast<Int>(format), grown.width, grown.height);
                m_msResolveScratch = {};
                return false;
            }
            m_msResolveScratch.format = format;
            m_msResolveScratch.extent = grown;
            m_msResolveScratch.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(m_msResolveScratch.layout, srcStageMask, srcAccessMask);
        if (!VkTextureManager::TransitionImageLayout(commandBuffer, m_msResolveScratch.image,
                                                     m_msResolveScratch.layout,
                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcStageMask,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask,
                                                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
            return false;
        }
        return true;
    }

    // The aspects a depth/stencil format actually carries. VkTextureManager keeps its own copy of
    // this private, and the swapchain's depth/stencil image has no TextureResource to ask.
    static VkImageAspectFlags GetDepthStencilAspectMaskForFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_NONE;
        }
    }

    // The depth/stencil half of MaterializePendingClearForDefaultFramebuffer. Separate only
    // because the image, the aspects and the clear value are all different from the colour one;
    // the reason it exists is the same - a readback with no intervening draw has no render pass
    // to fold the parked clear into.
    Bool VulkanRenderer::MaterializePendingDepthStencilClearForDefaultFramebuffer(
        VkCommandBuffer commandBuffer, const MG_State::GLState::FramebufferAttachmentObject& attachment,
        const ClearAttachmentPayload& payload) {
        const VkImage depthStencilImage = m_swapchainObject.GetDepthStencilImage(m_imageIndexAcquired);
        if (depthStencilImage == VK_NULL_HANDLE) {
            return false;
        }
        const VkImageAspectFlags imageAspects =
            GetDepthStencilAspectMaskForFormat(m_swapchainObject.GetDepthStencilFormat());
        VkImageAspectFlags clearAspects = 0;
        if ((payload.mask & GL_DEPTH_BUFFER_BIT) != 0) clearAspects |= (imageAspects & VK_IMAGE_ASPECT_DEPTH_BIT);
        if ((payload.mask & GL_STENCIL_BUFFER_BIT) != 0) clearAspects |= (imageAspects & VK_IMAGE_ASPECT_STENCIL_BIT);
        if (clearAspects == 0) {
            // Nothing this image can express; drop the pending clear rather than leave it to a
            // later render pass that would load it against an aspect that does not exist.
            m_clearManager->PopPendingClear(attachment);
            return true;
        }

        VkImageLayout currentLayout = m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired);
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(currentLayout, srcStageMask, srcAccessMask);
        VkImageLayout clearLayout = currentLayout;
        if (!VkTextureManager::TransitionImageLayout(commandBuffer, depthStencilImage, clearLayout,
                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcStageMask,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask,
                                                     VK_ACCESS_TRANSFER_WRITE_BIT, imageAspects)) {
            return false;
        }

        VkClearDepthStencilValue clearValue{};
        clearValue.depth = payload.depth;
        clearValue.stencil = payload.stencil;
        VkImageSubresourceRange range{};
        range.aspectMask = clearAspects;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        vkCmdClearDepthStencilImage(commandBuffer, depthStencilImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue,
                                    1, &range);

        VkImageLayout settledLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionDestinationState(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, dstStageMask,
                                           dstAccessMask);
        if (!VkTextureManager::TransitionImageLayout(commandBuffer, depthStencilImage, settledLayout,
                                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT, dstStageMask,
                                                     VK_ACCESS_TRANSFER_WRITE_BIT, dstAccessMask, imageAspects)) {
            return false;
        }
        m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired,
                                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        // The image now holds real values, so the next render pass must LOAD them rather than
        // treat the attachment as undefined and discard the clear that just executed.
        m_swapchainObject.SetDepthStencilContentDefined(m_imageIndexAcquired, true);

        m_clearManager->PopPendingClear(attachment);
        MGLOG_D("MaterializePendingClearForDefaultFramebuffer: swapchain depth/stencil image %u pending clear "
                "materialized (aspects=0x%x)",
                m_imageIndexAcquired, static_cast<Uint32>(clearAspects));
        return true;
    }

    // A glClear on the DEFAULT framebuffer is parked as a pending clear and folded into the next
    // render pass's loadOp. With no draw in between there is no render pass, so a readback that
    // followed such a clear blitted the untouched swapchain image and returned the PREVIOUS
    // frame's colour - which is exactly what the whole KHR-GL40.draw_indirect.negative-* family
    // sees (clear, an erroring draw that never executes, glReadPixels expecting zeroes).
    //
    // Materializing it means clearing the acquired swapchain image itself, which is why this
    // cannot reuse MaterializePendingClearForTexture: the default FBO's colour attachment is a
    // placeholder ITextureObject, and syncing it would allocate and clear an unrelated image.
    Bool VulkanRenderer::MaterializePendingClearForDefaultFramebuffer(VkCommandBuffer commandBuffer,
                                                                      MG_State::GLState::FramebufferObject& fbo,
                                                                      FramebufferAttachmentType attachmentType) {
        if (!fbo.IsDefaultFramebuffer() || attachmentType == FramebufferAttachmentType::None) {
            return true;
        }
        const auto& attachment = fbo.GetAttachment(attachmentType);
        if (!attachment.IsTexture() || attachment.IsRenderbuffer()) {
            return true;
        }
        ClearAttachmentPayload payload{};
        if (!m_clearManager->GetPendingClear(attachment, payload)) {
            return true;
        }
        MOBILEGL_ASSERT(VkRenderPassManager::GetActiveRenderPass() == nullptr ||
                            commandBuffer != m_frameContext.GetCurrent().commandBuffer,
                        "MaterializePendingClearForDefaultFramebuffer requires no active render pass");

        if ((payload.mask & GL_COLOR_BUFFER_BIT) == 0) {
            return MaterializePendingDepthStencilClearForDefaultFramebuffer(commandBuffer, attachment, payload);
        }

        const VkImage swapchainImage = m_swapchainObject.GetImage(m_imageIndexAcquired);
        if (swapchainImage == VK_NULL_HANDLE) {
            return false;
        }
        VkImageLayout currentLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(currentLayout, srcStageMask, srcAccessMask);
        VkImageLayout clearLayout = currentLayout;
        if (!VkTextureManager::TransitionImageLayout(commandBuffer, swapchainImage, clearLayout,
                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcStageMask,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask,
                                                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
            return false;
        }

        // The clear colour goes in verbatim, alpha included. Forcing opaque alpha here is what
        // makes a glClear(0,0,0,0) read back as (0,0,0,1) - the default framebuffer's placeholder
        // attachment can describe an alpha-less format while the swapchain image it stands for
        // has a real alpha channel.
        VkClearColorValue clearColor{};
        clearColor.float32[0] = payload.color.x();
        clearColor.float32[1] = payload.color.y();
        clearColor.float32[2] = payload.color.z();
        clearColor.float32[3] = payload.color.w();
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        vkCmdClearColorImage(commandBuffer, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1,
                             &range);

        VkImageLayout settledLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionDestinationState(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dstStageMask, dstAccessMask);
        if (!VkTextureManager::TransitionImageLayout(commandBuffer, swapchainImage, settledLayout,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT, dstStageMask,
                                                     VK_ACCESS_TRANSFER_WRITE_BIT, dstAccessMask,
                                                     VK_IMAGE_ASPECT_COLOR_BIT)) {
            return false;
        }
        m_swapchainObject.SetImageLayout(m_imageIndexAcquired, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        // Popped, not left behind: the clear has executed, so letting the next render pass load
        // it again as a loadOp would erase whatever is drawn between here and there.
        m_clearManager->PopPendingClear(attachment);
        MGLOG_D("MaterializePendingClearForDefaultFramebuffer: swapchain image %u pending clear materialized",
                m_imageIndexAcquired);
        return true;
    }

    Bool VulkanRenderer::TryBlitToDefaultFramebufferWithShader(FrameContext::FrameData& frame,
                                                               MG_State::GLState::FramebufferObject& readFbo,
                                                               MG_State::GLState::FramebufferObject& drawFbo,
                                                               GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                                               GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                                               GLenum filter) {
        const Bool drawIsDefaultFbo = drawFbo.IsDefaultFramebuffer();
        if (!drawIsDefaultFbo) {
            return false;
        }

        BlitImageBinding srcBinding{};
        BlitImageBinding dstBinding{};
        if (!ResolveColorBlitBinding(readFbo, true, m_imageIndexAcquired, m_swapchainObject, *m_textureManager,
                                     *m_renderPassManager, srcBinding) ||
            !ResolveColorBlitBinding(drawFbo, false, m_imageIndexAcquired, m_swapchainObject, *m_textureManager,
                                     *m_renderPassManager, dstBinding)) {
            return false;
        }
        if (srcBinding.trackedLayout == nullptr) {
            MGLOG_E_ONCE("BlitFramebuffer skipped: shader blit to default framebuffer requires a texture-backed source framebuffer");
            return false;
        }

        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        if (activeRenderPass != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const auto& attachment = readFbo.GetAttachment(readFbo.GetReadBuffer());
        auto sourceTexture = attachment.GetTexture();
        MOBILEGL_ASSERT(sourceTexture != nullptr, "TryBlitToDefaultFramebufferWithShader: source texture is null");
        const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sourceTexture);
        MOBILEGL_ASSERT(clearReady,
                        "TryBlitToDefaultFramebufferWithShader: failed to materialize pending clear for textureId=%d",
                        sourceTexture->GetExternalIndex());
        const Bool ready = m_textureManager->TransitionTextureForSampling(frame.commandBuffer, *sourceTexture);
        if (!ready) {
            MGLOG_E_ONCE("BlitFramebuffer skipped: failed to transition source textureId=%d for sampling",
                    sourceTexture->GetExternalIndex());
            return false;
        }
        if (m_textureManager->SyncTextureAndGetDescriptor(*sourceTexture) == nullptr) {
            MGLOG_E_ONCE("BlitFramebuffer skipped: failed to resolve source textureId=%d after sampling transition",
                    sourceTexture->GetExternalIndex());
            return false;
        }
        const VkImageView sourceImageView =
            m_textureManager->GetOrCreateSampledViewAtMipLevel(*sourceTexture, srcBinding.mipLevel);
        MOBILEGL_ASSERT(sourceImageView != VK_NULL_HANDLE,
                        "TryBlitToDefaultFramebufferWithShader: failed to create sampled view for textureId=%d mip=%u",
                        sourceTexture->GetExternalIndex(), srcBinding.mipLevel);

        // A color-only blit never touches depth/stencil: let the default-FBO pass
        // it opens skip the depth attachment (depth-less flavor).
        auto& renderPassEntry =
            m_renderPassManager->GetOrCreateRenderPass(drawFbo, m_imageIndexAcquired, /*drawUsesDepthStencil=*/false);
        const Bool ok = VkRenderPassManager::BeginRenderPass(frame.commandBuffer, renderPassEntry);
        MOBILEGL_ASSERT(ok, "%s: BeginRenderPass failed", __func__);

        ApplyGLViewportState(frame.commandBuffer, renderPassEntry.extent,
                             m_swapchainObject.GetPreTransform(), drawIsDefaultFbo);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<Uint32>(renderPassEntry.extent.x()), static_cast<Uint32>(renderPassEntry.extent.y())};
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

        MOBILEGL_ASSERT(m_blitResources.program != nullptr, "TryBlitToDefaultFramebufferWithShader: blit program is null");
        const VkPipeline pipeline = GetOrCreateBlitPipeline(renderPassEntry);
        MOBILEGL_ASSERT(pipeline != VK_NULL_HANDLE, "TryBlitToDefaultFramebufferWithShader: blit pipeline is null");
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        // The blit pipeline's narrower dynamic set (viewport/scissor only)
        // leaves the other dynamic states undefined; its raw viewport/scissor
        // writes also bypass the shadow.
        ResetDynamicStateShadow();

        auto* blitProgramData = static_cast<Uint8*>(m_blitResources.program->MapUBO());
        MOBILEGL_ASSERT(blitProgramData != nullptr, "TryBlitToDefaultFramebufferWithShader: blit UBO is null");
        std::fill(blitProgramData, blitProgramData + m_blitResources.program->GetUBOSize(), Uint8{0});

        BlitUniformData blitUniformData{};
        const float srcWidth = static_cast<float>(srcBinding.extent.x());
        const float srcHeight = static_cast<float>(srcBinding.extent.y());
        const float dstWidth = static_cast<float>(dstBinding.extent.x());
        const float dstHeight = static_cast<float>(dstBinding.extent.y());
        float dstNormWidth = dstWidth;
        float dstNormHeight = dstHeight;
        switch (m_swapchainObject.GetPreTransform()) {
            case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
                dstNormWidth = dstHeight;
                dstNormHeight = dstWidth;
                break;
            default:
                break;
        }
        blitUniformData.srcRect[0] = static_cast<float>(srcX0) / srcWidth;
        blitUniformData.srcRect[1] = static_cast<float>(srcY0) / srcHeight;
        blitUniformData.srcRect[2] = static_cast<float>(srcX1 - srcX0) / srcWidth;
        blitUniformData.srcRect[3] = static_cast<float>(srcY1 - srcY0) / srcHeight;
        blitUniformData.dstRect[0] = static_cast<float>(dstX0) / dstNormWidth;
        blitUniformData.dstRect[1] = static_cast<float>(dstY0) / dstNormHeight;
        blitUniformData.dstRect[2] = static_cast<float>(dstX1 - dstX0) / dstNormWidth;
        blitUniformData.dstRect[3] = static_cast<float>(dstY1 - dstY0) / dstNormHeight;
        blitUniformData.surfaceTransform = static_cast<Int>(ToBlitSurfaceTransform(m_swapchainObject.GetPreTransform()));

        auto writeUniform = [&](Int location, const void* data, SizeT size) {
            MOBILEGL_ASSERT(location >= 0, "TryBlitToDefaultFramebufferWithShader: invalid uniform location");
            const Uint offset = m_blitResources.program->GetUniformOffset(static_cast<Uint>(location));
            // A RETURN, not only an assert - see GenerateDepthMipmapWithShader's copy of this
            // guard: kInvalidUniformOffset must not reach the memcpy in a release build.
            if (offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
                offset + size > m_blitResources.program->GetUBOSize()) {
                MOBILEGL_ASSERT(false, "TryBlitToDefaultFramebufferWithShader: uniform write out of bounds");
                return;
            }
            memcpy(blitProgramData + offset, data, size);
        };
        writeUniform(m_blitResources.srcRectLocation, blitUniformData.srcRect, sizeof(blitUniformData.srcRect));
        writeUniform(m_blitResources.dstRectLocation, blitUniformData.dstRect, sizeof(blitUniformData.dstRect));
        writeUniform(m_blitResources.surfaceTransformLocation, &blitUniformData.surfaceTransform,
                     sizeof(blitUniformData.surfaceTransform));
        m_blitResources.program->MarkUBOContentDirty();

        const auto samplerBindingOverride = UniformManager::SamplerBindingOverride{
            .binding = m_blitResources.samplerBinding,
            .texture = sourceTexture.get(),
            .sampler = (filter == GL_LINEAR ? m_blitResources.linearSampler.get()
                                            : m_blitResources.nearestSampler.get()),
            .imageView = sourceImageView,
        };
        ProgramFactory::CompileOptionFlags blitTransformFlags = 0;
        const auto& blitProgramObj = m_programFactory->GetOrCreateProgram(*m_blitResources.program, blitTransformFlags);
        const Bool bound = m_uniformManager->BindProgramUniformBuffers(
            frame.commandBuffer, *m_blitResources.program, blitProgramObj, m_frameContext.GetCurrentFrameIndex(),
            VK_PIPELINE_BIND_POINT_GRAPHICS, &samplerBindingOverride);
        MOBILEGL_ASSERT(bound, "TryBlitToDefaultFramebufferWithShader: BindProgramUniformBuffers failed");
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        return true;
    }

    void VulkanRenderer::BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                         GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                         GLbitfield mask, GLenum filter) {
        auto readFbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
        auto drawFbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        BlitNamedFramebuffer(readFbo, drawFbo, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    }

    void VulkanRenderer::BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFbo,
                                              const SharedPtr<MG_State::GLState::FramebufferObject>& drawFbo,
                                              GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                              GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                              GLbitfield mask, GLenum filter) {
        static constexpr GLbitfield kSupportedBlitMask =
            GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        if ((mask & ~kSupportedBlitMask) != 0) {
            MGLOG_E_ONCE("BlitFramebuffer skipped: unsupported mask bits=0x%x", static_cast<Uint32>(mask));
            return;
        }
        const Bool isColorBlit = (mask & GL_COLOR_BUFFER_BIT) != 0;
        const Bool isDepthBlit = (mask & GL_DEPTH_BUFFER_BIT) != 0;
        const Bool isStencilBlit = (mask & GL_STENCIL_BUFFER_BIT) != 0;
        if (!isColorBlit && !isDepthBlit && !isStencilBlit) {
            return;
        }
        if (filter != GL_NEAREST && filter != GL_LINEAR) {
            MGLOG_E_ONCE("BlitFramebuffer skipped: unsupported filter=0x%x", static_cast<Uint32>(filter));
            return;
        }
        if ((isDepthBlit || isStencilBlit) && filter != GL_NEAREST) {
            MGLOG_E_ONCE("BlitFramebuffer skipped: depth/stencil blits require GL_NEAREST");
            return;
        }

        // The scissor test clips blit writes: intersect the destination rectangle with
        // the scissor box and shrink the source proportionally.
        if (MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest)) {
            const IntVec4& scissor = MG_State::pGLContext->GetScissorBox();
            const auto clipAxis = [](GLint& d0, GLint& d1, GLint& s0, GLint& s1, GLint clipLo, GLint clipHi) -> Bool {
                const Bool dstFlipped = d1 < d0;
                GLint lo = dstFlipped ? d1 : d0;
                GLint hi = dstFlipped ? d0 : d1;
                const GLint newLo = std::max(lo, clipLo);
                const GLint newHi = std::min(hi, clipHi);
                if (newLo >= newHi) {
                    return false;
                }
                const double srcSpan = static_cast<double>(s1 - s0);
                const double dstSpan = static_cast<double>(d1 - d0);
                const double scale = dstSpan != 0.0 ? srcSpan / dstSpan : 0.0;
                const GLint origD0 = d0;
                const GLint clippedD0 = dstFlipped ? newHi : newLo;
                const GLint clippedD1 = dstFlipped ? newLo : newHi;
                s0 = s0 + static_cast<GLint>(std::lround((clippedD0 - origD0) * scale));
                s1 = s0 + static_cast<GLint>(std::lround((clippedD1 - clippedD0) * scale));
                d0 = clippedD0;
                d1 = clippedD1;
                return true;
            };
            if (!clipAxis(dstX0, dstX1, srcX0, srcX1, scissor.x(), scissor.x() + scissor.z()) ||
                !clipAxis(dstY0, dstY1, srcY0, srcY1, scissor.y(), scissor.y() + scissor.w())) {
                return; // fully scissored out
            }
        }

        MOBILEGL_ASSERT(readFbo != nullptr, "VulkanRenderer::BlitFramebuffer: read framebuffer is null");
        MOBILEGL_ASSERT(drawFbo != nullptr, "VulkanRenderer::BlitFramebuffer: draw framebuffer is null");
        if (IsUnsupportedFramebufferForDirectVulkan(*readFbo) ||
            IsUnsupportedFramebufferForDirectVulkan(*drawFbo)) {
            RecordUnsupportedFramebufferError(__func__);
            return;
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }

        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        if (activeRenderPass != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const Bool readIsDefaultFbo = readFbo->IsDefaultFramebuffer();
        const Bool drawIsDefaultFbo = drawFbo->IsDefaultFramebuffer();
        if (isColorBlit && drawIsDefaultFbo &&
            RequiresShaderBlitToDefaultFramebuffer(m_swapchainObject.GetPreTransform())) {
            if (TryBlitToDefaultFramebufferWithShader(frame, *readFbo, *drawFbo,
                                                      srcX0, srcY0, srcX1, srcY1,
                                                      dstX0, dstY0, dstX1, dstY1, filter)) {
                return;
            }
            MGLOG_E_ONCE("BlitFramebuffer skipped: rotated blit to default framebuffer requires a texture-backed source framebuffer");
            return;
        }

        Vector<VkImageAspectFlagBits> depthStencilAspects;
        if (isDepthBlit) depthStencilAspects.push_back(VK_IMAGE_ASPECT_DEPTH_BIT);
        if (isStencilBlit) depthStencilAspects.push_back(VK_IMAGE_ASPECT_STENCIL_BIT);
        for (const VkImageAspectFlagBits depthStencilAspect : depthStencilAspects) {
            BlitImageBinding srcBinding{};
            BlitImageBinding dstBinding{};
            if (!ResolveFramebufferBlitBinding(*readFbo, true, m_imageIndexAcquired, m_swapchainObject,
                                               *m_textureManager, *m_renderPassManager,
                                               depthStencilAspect, srcBinding) ||
                !ResolveFramebufferBlitBinding(*drawFbo, false, m_imageIndexAcquired, m_swapchainObject,
                                               *m_textureManager, *m_renderPassManager,
                                               depthStencilAspect, dstBinding)) {
                // A buffer named in the mask but absent from either framebuffer copies
                // nothing for that buffer; the other requested buffers still blit.
                continue;
            }

            if (srcX1 < srcX0 || srcY1 < srcY0 || dstX1 < dstX0 || dstY1 < dstY0) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: depth blits with flipped rectangles are not supported yet");
                continue;
            }

            const Int srcWidth = srcX1 - srcX0;
            const Int srcHeight = srcY1 - srcY0;
            const Int dstWidth = dstX1 - dstX0;
            const Int dstHeight = dstY1 - dstY0;
            if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: degenerate depth blit rectangle");
                continue;
            }
            // A scaling depth blit is legal GL and vkCmdBlitImage scales natively; only a same-size
            // pair can take the cheaper vkCmdCopyImage.
            const Bool depthBlitScales = srcWidth != dstWidth || srcHeight != dstHeight;

            if (!readIsDefaultFbo) {
                const auto sourceAttachmentType = ResolveFramebufferCopyAttachmentType(*readFbo, true, srcBinding.aspectMask);
                const auto& sourceAttachment = readFbo->GetAttachment(sourceAttachmentType);
                if (auto sourceTexture = sourceAttachment.GetTexture(); sourceTexture != nullptr) {
                    const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sourceTexture);
                    MOBILEGL_ASSERT(clearReady,
                                    "BlitFramebuffer: failed to materialize pending clear for depth source textureId=%d",
                                    sourceTexture->GetExternalIndex());
                } else if (sourceAttachment.IsRenderbuffer()) {
                    const Bool clearReady = MaterializePendingClearForRenderbuffer(frame.commandBuffer,
                                                                                   sourceAttachment.GetRenderbuffer());
                    MOBILEGL_ASSERT(clearReady,
                                    "BlitFramebuffer: failed to materialize pending clear for depth source renderbuffer %u",
                                    sourceAttachment.GetRenderbuffer()->GetExternalIndex());
                }
            }

            const auto destAttachmentType =
                ResolveFramebufferCopyAttachmentType(*drawFbo, false, dstBinding.aspectMask);
            if (drawIsDefaultFbo) {
                // Same ordering rule for the default framebuffer's depth/stencil - see the
                // colour twin below.
                const Bool dstClearReady = MaterializePendingClearForDefaultFramebuffer(
                    frame.commandBuffer, *drawFbo, destAttachmentType);
                MOBILEGL_ASSERT(dstClearReady,
                                "BlitFramebuffer: failed to materialize the default framebuffer's pending "
                                "depth/stencil clear");
            } else {
                // A clear queued for the destination predates this blit in API order;
                // execute it now, or its deferred materialization would later stomp the
                // copied contents (MC 26.3 OIT clears cloud_depth, then blits the main
                // depth into it - the stale loadOp=CLEAR erased the copy).
                const auto& destAttachment = drawFbo->GetAttachment(destAttachmentType);
                if (auto destTexture = destAttachment.GetTexture(); destTexture != nullptr) {
                    const Bool dstClearReady = MaterializePendingClearForTexture(frame.commandBuffer, *destTexture);
                    MOBILEGL_ASSERT(dstClearReady,
                                    "BlitFramebuffer: failed to materialize pending clear for depth destination textureId=%d",
                                    destTexture->GetExternalIndex());
                } else if (destAttachment.IsRenderbuffer()) {
                    const Bool dstClearReady = MaterializePendingClearForRenderbuffer(frame.commandBuffer,
                                                                                      destAttachment.GetRenderbuffer());
                    MOBILEGL_ASSERT(dstClearReady,
                                    "BlitFramebuffer: failed to materialize pending clear for depth destination renderbuffer %u",
                                    destAttachment.GetRenderbuffer()->GetExternalIndex());
                }
            }

            const VkImageLayout srcOriginalLayout = readIsDefaultFbo
                ? m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired)
                : *srcBinding.trackedLayout;
            if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                MGLOG_E_ONCE("BlitFramebuffer skipped: depth source image layout is undefined");
                continue;
            }

            const VkImageLayout dstOriginalLayout = drawIsDefaultFbo
                ? m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired)
                : *dstBinding.trackedLayout;
            const VkImageLayout dstRestoreLayout = dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                : dstOriginalLayout;

            // vkCmdCopyImage requires identical depth formats; a mismatched pair (e.g.
            // a D24S8 renderbuffer into a DEPTH_COMPONENT24 texture backed by the
            // D32_SFLOAT fallback) round-trips the region through the host with a
            // per-texel re-encode instead.
            if (srcBinding.format != dstBinding.format) {
                if (readIsDefaultFbo || drawIsDefaultFbo) {
                    MGLOG_E_ONCE("BlitFramebuffer skipped: cross-format depth/stencil blit with the default framebuffer");
                    continue;
                }
                if (!BlitDepthAcrossFormats(frame, srcBinding.image, srcBinding.format, srcBinding.trackedLayout,
                                            srcBinding.mipLevel, srcBinding.baseArrayLayer, dstBinding.image,
                                            dstBinding.format, dstBinding.trackedLayout, dstBinding.mipLevel,
                                            dstBinding.baseArrayLayer, srcX0, srcY0, dstX0, dstY0, srcX1 - srcX0,
                                            srcY1 - srcY0, srcOriginalLayout, dstRestoreLayout,
                                            depthStencilAspect == VK_IMAGE_ASPECT_STENCIL_BIT)) {
                    continue;
                }
                continue;
            }

            VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags srcAccessMask = 0;
            GetImageTransitionSourceState(srcOriginalLayout, srcStageMask, srcAccessMask);
            // Both blit regions below name `baseArrayLayer` from their binding, and a layered depth
            // attachment puts that above 0. These barriers carry a mip range only - their layer
            // range is every layer (see VkTextureManager::TransitionImageLayout).
            if (readIsDefaultFbo) {
                VkImageLayout srcTrackedLayout = srcOriginalLayout;
                Bool ok = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, srcBinding.image, srcTrackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, srcBinding.aspectMask,
                    srcBinding.mipLevel, srcBinding.mipLevelCount);
                MOBILEGL_ASSERT(ok, "%s: failed to transition swapchain depth source image", __func__);
                m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired, srcTrackedLayout);
            } else {
                Bool ok = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, srcBinding.image, *srcBinding.trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, srcBinding.aspectMask,
                    srcBinding.mipLevel, srcBinding.mipLevelCount);
                MOBILEGL_ASSERT(ok, "%s: failed to transition depth source image", __func__);
            }

            VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags dstAccessMask = 0;
            GetImageTransitionSourceState(dstOriginalLayout, dstStageMask, dstAccessMask);
            if (drawIsDefaultFbo) {
                VkImageLayout dstTrackedLayout = dstOriginalLayout;
                Bool ok = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, dstBinding.image, dstTrackedLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, dstBinding.aspectMask,
                    dstBinding.mipLevel, dstBinding.mipLevelCount);
                MOBILEGL_ASSERT(ok, "%s: failed to transition swapchain depth destination image", __func__);
                m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired, dstTrackedLayout);
            } else {
                Bool ok = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, dstBinding.image, *dstBinding.trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, dstBinding.aspectMask,
                    dstBinding.mipLevel, dstBinding.mipLevelCount);
                MOBILEGL_ASSERT(ok, "%s: failed to transition depth destination image", __func__);
            }

            // The default framebuffer is stored display-side-up, so a rect aimed at it (or read
            // from it) has to be converted out of GL's bottom-origin space - the same conversion
            // the colour blit below applies. vkCmdCopyImage cannot express it (it has no second
            // offset to invert), so a default-framebuffer side forces the vkCmdBlitImage form even
            // at equal size. Without this a scissored depth blit into the default framebuffer
            // wrote the MIRRORED band: KHR-GL*.framebuffer_blit.scissor_blit clips to the lower
            // left quadrant, and the depth landed in the upper one.
            const Bool depthBlitNeedsOrientation = readIsDefaultFbo || drawIsDefaultFbo;
            if (depthBlitScales || depthBlitNeedsOrientation) {
                // vkCmdCopyImage cannot resize; NEAREST is the only filter Vulkan allows for a
                // depth/stencil blit anyway, and the GL front end already rejects the others.
                VkImageBlit blitRegion{};
                blitRegion.srcSubresource.aspectMask = srcBinding.aspectMask;
                blitRegion.srcSubresource.mipLevel = srcBinding.mipLevel;
                blitRegion.srcSubresource.baseArrayLayer = srcBinding.baseArrayLayer;
                blitRegion.srcSubresource.layerCount = srcBinding.layerCount;
                blitRegion.srcOffsets[0] = {srcX0, srcY0, 0};
                blitRegion.srcOffsets[1] = {srcX1, srcY1, 1};
                blitRegion.dstSubresource.aspectMask = dstBinding.aspectMask;
                blitRegion.dstSubresource.mipLevel = dstBinding.mipLevel;
                blitRegion.dstSubresource.baseArrayLayer = dstBinding.baseArrayLayer;
                blitRegion.dstSubresource.layerCount = dstBinding.layerCount;
                blitRegion.dstOffsets[0] = {dstX0, dstY0, 0};
                blitRegion.dstOffsets[1] = {dstX1, dstY1, 1};
                if (readIsDefaultFbo) {
                    ApplyNativeBlitDefaultFramebufferSourceTransform(m_swapchainObject.GetPreTransform(), srcBinding,
                                                                     blitRegion);
                }
                if (drawIsDefaultFbo) {
                    ApplyNativeBlitDefaultFramebufferTransform(m_swapchainObject.GetPreTransform(), dstBinding,
                                                               blitRegion);
                }
                vkCmdBlitImage(frame.commandBuffer,
                               srcBinding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dstBinding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blitRegion, VK_FILTER_NEAREST);
            } else {
                VkImageCopy copyRegion{};
                copyRegion.srcSubresource.aspectMask = srcBinding.aspectMask;
                copyRegion.srcSubresource.mipLevel = srcBinding.mipLevel;
                copyRegion.srcSubresource.baseArrayLayer = srcBinding.baseArrayLayer;
                copyRegion.srcSubresource.layerCount = srcBinding.layerCount;
                copyRegion.srcOffset = {srcX0, srcY0, 0};
                copyRegion.dstSubresource.aspectMask = dstBinding.aspectMask;
                copyRegion.dstSubresource.mipLevel = dstBinding.mipLevel;
                copyRegion.dstSubresource.baseArrayLayer = dstBinding.baseArrayLayer;
                copyRegion.dstSubresource.layerCount = dstBinding.layerCount;
                copyRegion.dstOffset = {dstX0, dstY0, 0};
                copyRegion.extent = {static_cast<Uint32>(srcWidth), static_cast<Uint32>(srcHeight), 1};

                vkCmdCopyImage(frame.commandBuffer,
                               srcBinding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dstBinding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copyRegion);
            }

            VkPipelineStageFlags srcRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags srcRestoreAccessMask = 0;
            GetImageTransitionDestinationState(srcOriginalLayout, srcRestoreStageMask, srcRestoreAccessMask);
            if (readIsDefaultFbo) {
                VkImageLayout srcTrackedLayout = m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired);
                Bool ok = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, srcBinding.image, srcTrackedLayout, srcOriginalLayout,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                    VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask, srcBinding.aspectMask,
                    srcBinding.mipLevel, srcBinding.mipLevelCount);
                MOBILEGL_ASSERT(ok, "%s: failed to restore swapchain depth source image layout", __func__);
                m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired, srcTrackedLayout);
            } else {
                Bool ok = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, srcBinding.image, *srcBinding.trackedLayout, srcOriginalLayout,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                    VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask, srcBinding.aspectMask,
                    srcBinding.mipLevel, srcBinding.mipLevelCount);
                MOBILEGL_ASSERT(ok, "%s: failed to restore depth source image layout", __func__);
            }

            VkPipelineStageFlags dstRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags dstRestoreAccessMask = 0;
            GetImageTransitionDestinationState(dstRestoreLayout, dstRestoreStageMask, dstRestoreAccessMask);
            if (drawIsDefaultFbo) {
                VkImageLayout dstTrackedLayout = m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired);
                Bool ok = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, dstBinding.image, dstTrackedLayout, dstRestoreLayout,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                    VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, dstBinding.aspectMask,
                    dstBinding.mipLevel, dstBinding.mipLevelCount);
                MOBILEGL_ASSERT(ok, "%s: failed to restore swapchain depth destination image layout", __func__);
                m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired, dstTrackedLayout);
            } else {
                Bool ok = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, dstBinding.image, *dstBinding.trackedLayout, dstRestoreLayout,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                    VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, dstBinding.aspectMask,
                    dstBinding.mipLevel, dstBinding.mipLevelCount);
                MOBILEGL_ASSERT(ok, "%s: failed to restore depth destination image layout", __func__);
            }
        }
        if (!isColorBlit) {
            return;
        }

        BlitImageBinding srcBinding{};
        BlitImageBinding dstBinding{};
        if (!ResolveColorBlitBinding(*readFbo, true, m_imageIndexAcquired, m_swapchainObject, *m_textureManager,
                                     *m_renderPassManager, srcBinding) ||
            !ResolveColorBlitBinding(*drawFbo, false, m_imageIndexAcquired, m_swapchainObject, *m_textureManager,
                                     *m_renderPassManager, dstBinding)) {
            return;
        }

        if (!readIsDefaultFbo) {
            const auto& sourceAttachment = readFbo->GetAttachment(readFbo->GetReadBuffer());
            auto sourceTexture = sourceAttachment.GetTexture();
            if (sourceTexture != nullptr) {
                const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sourceTexture);
                MOBILEGL_ASSERT(clearReady,
                                "BlitFramebuffer: failed to materialize pending clear for source textureId=%d",
                                sourceTexture->GetExternalIndex());
            } else if (sourceAttachment.IsRenderbuffer()) {
                const Bool clearReady =
                    MaterializePendingClearForRenderbuffer(frame.commandBuffer, sourceAttachment.GetRenderbuffer());
                MOBILEGL_ASSERT(clearReady,
                                "BlitFramebuffer: failed to materialize pending clear for source renderbuffer %u",
                                sourceAttachment.GetRenderbuffer()->GetExternalIndex());
            }
        }

        if (drawIsDefaultFbo) {
            // The default framebuffer needs the same ordering, and needed it before anything
            // consumed its parked clear: Minecraft clears the default framebuffer, renders the
            // world into its own framebuffer and BLITS the result out, so nothing between the
            // clear and the blit ever opens a render pass on the default framebuffer to fold the
            // clear in as a loadOp. The clear therefore stayed pending across the whole frame,
            // and the first path that did materialize it - the readback - executed it AFTER the
            // blit and handed back a blank frame (every DirectVulkan retrace, ssim 0.000005).
            const Bool dstClearReady = MaterializePendingClearForDefaultFramebuffer(
                frame.commandBuffer, *drawFbo, drawFbo->GetDrawBuffers()[0]);
            MOBILEGL_ASSERT(dstClearReady,
                            "BlitFramebuffer: failed to materialize the default framebuffer's pending clear");
        } else {
            // A clear queued for the destination predates this blit in API order; execute
            // it now, or its deferred materialization would later stomp the blitted color.
            const auto& destAttachment = drawFbo->GetAttachment(drawFbo->GetDrawBuffers()[0]);
            auto destTexture = destAttachment.GetTexture();
            if (destTexture != nullptr) {
                const Bool dstClearReady = MaterializePendingClearForTexture(frame.commandBuffer, *destTexture);
                MOBILEGL_ASSERT(dstClearReady,
                                "BlitFramebuffer: failed to materialize pending clear for destination textureId=%d",
                                destTexture->GetExternalIndex());
            } else if (destAttachment.IsRenderbuffer()) {
                const Bool dstClearReady =
                    MaterializePendingClearForRenderbuffer(frame.commandBuffer, destAttachment.GetRenderbuffer());
                MOBILEGL_ASSERT(dstClearReady,
                                "BlitFramebuffer: failed to materialize pending clear for destination renderbuffer %u",
                                destAttachment.GetRenderbuffer()->GetExternalIndex());
            }
        }

        VkImageLayout srcLayout = readIsDefaultFbo
            ? m_swapchainObject.GetImageLayout(m_imageIndexAcquired)
            : *srcBinding.trackedLayout;
        VkImageLayout dstLayout = drawIsDefaultFbo
            ? m_swapchainObject.GetImageLayout(m_imageIndexAcquired)
            : *dstBinding.trackedLayout;
        const VkImageLayout srcOriginalLayout = srcLayout;
        const VkImageLayout dstOriginalLayout = dstLayout;
        const VkImageLayout dstRestoreLayout = dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            : dstOriginalLayout;

        if (readIsDefaultFbo && srcLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            MGLOG_E_ONCE("BlitFramebuffer skipped: swapchain source image layout is undefined");
            return;
        }
        if (srcLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            MGLOG_E_ONCE("BlitFramebuffer skipped: source image layout is undefined");
            return;
        }

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcLayout, srcStageMask, srcAccessMask);
        if (readIsDefaultFbo) {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, srcBinding.aspectMask);
            MOBILEGL_ASSERT(ok, "%s: failed to transition swapchain source image", __func__);
            m_swapchainObject.SetImageLayout(m_imageIndexAcquired, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        } else {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, *srcBinding.trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, srcBinding.aspectMask, 0, srcBinding.mipLevelCount);
            MOBILEGL_ASSERT(ok, "%s: failed to transition source image", __func__);
        }

        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionSourceState(dstLayout, dstStageMask, dstAccessMask);
        if (drawIsDefaultFbo) {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstBinding.image, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, dstBinding.aspectMask);
            MOBILEGL_ASSERT(ok, "%s: failed to transition swapchain destination image", __func__);
            m_swapchainObject.SetImageLayout(m_imageIndexAcquired, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        } else {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstBinding.image, *dstBinding.trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, dstBinding.aspectMask, 0, dstBinding.mipLevelCount);
            MOBILEGL_ASSERT(ok, "%s: failed to transition destination image", __func__);
        }

        VkImageBlit blitRegion{};
        blitRegion.srcSubresource.aspectMask = srcBinding.aspectMask;
        blitRegion.srcSubresource.mipLevel = srcBinding.mipLevel;
        blitRegion.srcSubresource.baseArrayLayer = srcBinding.baseArrayLayer;
        blitRegion.srcSubresource.layerCount = srcBinding.layerCount;
        blitRegion.srcOffsets[0] = {srcX0, srcY0, 0};
        blitRegion.srcOffsets[1] = {srcX1, srcY1, 1};
        blitRegion.dstSubresource.aspectMask = dstBinding.aspectMask;
        blitRegion.dstSubresource.mipLevel = dstBinding.mipLevel;
        blitRegion.dstSubresource.baseArrayLayer = dstBinding.baseArrayLayer;
        blitRegion.dstSubresource.layerCount = dstBinding.layerCount;
        blitRegion.dstOffsets[0] = {dstX0, dstY0, 0};
        blitRegion.dstOffsets[1] = {dstX1, dstY1, 1};
        if (readIsDefaultFbo) {
            ApplyNativeBlitDefaultFramebufferSourceTransform(m_swapchainObject.GetPreTransform(), srcBinding,
                                                             blitRegion);
        }
        if (drawIsDefaultFbo) {
            ApplyNativeBlitDefaultFramebufferTransform(m_swapchainObject.GetPreTransform(), dstBinding, blitRegion);
        }

        if (srcBinding.sampleCount != VK_SAMPLE_COUNT_1_BIT && dstBinding.sampleCount == VK_SAMPLE_COUNT_1_BIT) {
            // GL multisample resolve blits are 1:1 by spec; vkCmdBlitImage cannot read a
            // multisampled source, so the samples have to come down through vkCmdResolveImage.
            const Uint32 resolveWidth = static_cast<Uint32>(std::abs(srcX1 - srcX0));
            const Uint32 resolveHeight = static_cast<Uint32>(std::abs(srcY1 - srcY0));

            // vkCmdResolveImage takes ONE offset per side, so it cannot express the axis inversion
            // that a default-framebuffer rect needs - it would land the mirrored band. When the
            // transforms above actually moved the region, split the operation: resolve into a
            // single-sample scratch image at raw offsets, then blit THAT into the destination with
            // the (already transformed) region, which vkCmdBlitImage can invert.
            const Bool regionWasTransformed =
                (readIsDefaultFbo || drawIsDefaultFbo) &&
                (blitRegion.srcOffsets[0].x != srcX0 || blitRegion.srcOffsets[0].y != srcY0 ||
                 blitRegion.srcOffsets[1].x != srcX1 || blitRegion.srcOffsets[1].y != srcY1 ||
                 blitRegion.dstOffsets[0].x != dstX0 || blitRegion.dstOffsets[0].y != dstY0 ||
                 blitRegion.dstOffsets[1].x != dstX1 || blitRegion.dstOffsets[1].y != dstY1);
            const Bool useScratchResolve =
                regionWasTransformed && resolveWidth > 0 && resolveHeight > 0 &&
                AcquireMultisampleResolveScratchImage(frame.commandBuffer, srcBinding.format,
                                                      {resolveWidth, resolveHeight});

            VkImageResolve resolveRegion{};
            resolveRegion.srcSubresource = blitRegion.srcSubresource;
            resolveRegion.dstSubresource = blitRegion.dstSubresource;
            resolveRegion.extent = {resolveWidth, resolveHeight, 1};
            if (useScratchResolve) {
                // The scratch copy is a plain single-layer colour image, and the resolve reads the
                // SOURCE band the (possibly inverted) transformed region names - taking its min so
                // an inverted pair still describes the same band.
                resolveRegion.srcOffset = {std::min(blitRegion.srcOffsets[0].x, blitRegion.srcOffsets[1].x),
                                           std::min(blitRegion.srcOffsets[0].y, blitRegion.srcOffsets[1].y), 0};
                resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                resolveRegion.dstSubresource.mipLevel = 0;
                resolveRegion.dstSubresource.baseArrayLayer = 0;
                resolveRegion.dstSubresource.layerCount = 1;
                resolveRegion.dstOffset = {0, 0, 0};
                vkCmdResolveImage(frame.commandBuffer,
                                  srcBinding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  m_msResolveScratch.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  1, &resolveRegion);

                VkImageLayout scratchLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                const Bool scratchReady = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, m_msResolveScratch.image, scratchLayout,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
                MOBILEGL_ASSERT(scratchReady, "%s: failed to transition the resolve scratch image", __func__);
                m_msResolveScratch.layout = scratchLayout;

                // Second leg: the scratch image holds the resolved band at its own origin, so the
                // source side of the region becomes the whole scratch rect and only the
                // destination keeps the transform.
                VkImageBlit scratchBlit = blitRegion;
                scratchBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                scratchBlit.srcSubresource.mipLevel = 0;
                scratchBlit.srcSubresource.baseArrayLayer = 0;
                scratchBlit.srcSubresource.layerCount = 1;
                scratchBlit.srcOffsets[0] = {0, 0, 0};
                scratchBlit.srcOffsets[1] = {static_cast<Int32>(resolveWidth), static_cast<Int32>(resolveHeight), 1};
                vkCmdBlitImage(frame.commandBuffer,
                               m_msResolveScratch.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dstBinding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &scratchBlit, filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            } else {
                resolveRegion.srcOffset = {std::min(srcX0, srcX1), std::min(srcY0, srcY1), 0};
                resolveRegion.dstOffset = {std::min(dstX0, dstX1), std::min(dstY0, dstY1), 0};
                vkCmdResolveImage(frame.commandBuffer,
                                  srcBinding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  dstBinding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  1, &resolveRegion);
            }
        } else {
            vkCmdBlitImage(frame.commandBuffer,
                           srcBinding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dstBinding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blitRegion, filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }

        VkPipelineStageFlags srcRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcRestoreAccessMask = 0;
        GetImageTransitionDestinationState(srcOriginalLayout, srcRestoreStageMask, srcRestoreAccessMask);
        if (readIsDefaultFbo) {
            VkImageLayout srcTrackedLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, srcTrackedLayout, srcOriginalLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask, srcBinding.aspectMask);
            MOBILEGL_ASSERT(ok, "%s: failed to restore swapchain source image layout", __func__);
            m_swapchainObject.SetImageLayout(m_imageIndexAcquired, srcTrackedLayout);
        } else {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, *srcBinding.trackedLayout, srcOriginalLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask, srcBinding.aspectMask, 0, srcBinding.mipLevelCount);
            MOBILEGL_ASSERT(ok, "%s: failed to restore source image layout", __func__);
        }

        VkPipelineStageFlags dstRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstRestoreAccessMask = 0;
        GetImageTransitionDestinationState(dstRestoreLayout, dstRestoreStageMask, dstRestoreAccessMask);
        if (drawIsDefaultFbo) {
            VkImageLayout dstTrackedLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstBinding.image, dstTrackedLayout, dstRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, dstBinding.aspectMask);
            MOBILEGL_ASSERT(ok, "%s: failed to restore swapchain destination image layout", __func__);
            m_swapchainObject.SetImageLayout(m_imageIndexAcquired, dstTrackedLayout);
        } else {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstBinding.image, *dstBinding.trackedLayout, dstRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, dstBinding.aspectMask, 0, dstBinding.mipLevelCount);
            MOBILEGL_ASSERT(ok, "%s: failed to restore destination image layout", __func__);
        }
    }

    void VulkanRenderer::CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                           GLint x, GLint y, GLsizei width, GLsizei height) {
        if (width <= 0 || height <= 0) {
            return;
        }

        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (textureTarget != TextureTarget::Texture2D) {
            RecordTextureCopyError(__func__, ErrorCode::InvalidOperation,
                                   "CopyTexSubImage2D currently only supports GL_TEXTURE_2D destinations.");
            return;
        }
        if (level < 0) {
            RecordTextureCopyError(__func__, ErrorCode::InvalidValue,
                                   "CopyTexSubImage2D level must be non-negative.");
            return;
        }

        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto destinationTexture = textureUnit.GetBindingSlot(textureTarget).GetBoundObject();
        if (destinationTexture == nullptr) {
            RecordTextureCopyError(__func__, ErrorCode::InvalidOperation,
                                   "CopyTexSubImage2D requires a bound destination texture.");
            return;
        }

        auto readFbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
        if (readFbo == nullptr) {
            RecordTextureCopyError(__func__, ErrorCode::InvalidOperation,
                                   "CopyTexSubImage2D requires a framebuffer bound to GL_READ_FRAMEBUFFER.");
            return;
        }
        if (IsUnsupportedFramebufferForDirectVulkan(*readFbo)) {
            RecordTextureCopyError(__func__, ErrorCode::InvalidFramebufferOperation,
                                   "CopyTexSubImage2D does not support the current non-default read framebuffer configuration on DirectVulkan.");
            return;
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }

        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const Bool readIsDefaultFbo = readFbo->IsDefaultFramebuffer();

        BlitImageBinding dstBinding{};
        if (!ResolveTextureCopyDestinationBinding(*destinationTexture, static_cast<Uint32>(level), *m_textureManager,
                                                  dstBinding)) {
            RecordTextureCopyError(__func__, ErrorCode::InvalidOperation,
                                   "CopyTexSubImage2D failed to resolve the destination texture.");
            return;
        }

        BlitImageBinding srcBinding{};
        if (!ResolveTextureCopySourceBinding(*readFbo, m_imageIndexAcquired, m_swapchainObject, *m_textureManager,
                                             *m_renderPassManager, dstBinding.aspectMask, srcBinding)) {
            RecordTextureCopyError(__func__, ErrorCode::InvalidOperation,
                                   "CopyTexSubImage2D requires a complete read attachment compatible with the destination texture.");
            return;
        }

        if (!readIsDefaultFbo) {
            const auto sourceAttachmentType = ResolveFramebufferCopyAttachmentType(*readFbo, true, srcBinding.aspectMask);
            const auto& sourceAttachment = readFbo->GetAttachment(sourceAttachmentType);
            auto sourceTexture = sourceAttachment.GetTexture();
            MOBILEGL_ASSERT(sourceTexture != nullptr, "CopyTexSubImage2D: source texture attachment is null");
            const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sourceTexture);
            MOBILEGL_ASSERT(clearReady,
                            "CopyTexSubImage2D: failed to materialize pending clear for source textureId=%d",
                            sourceTexture->GetExternalIndex());
        }

        {
            // A clear queued for the destination predates this copy in API order;
            // execute it now so the deferred materialization cannot stomp the copy.
            const Bool dstClearReady = MaterializePendingClearForTexture(frame.commandBuffer, *destinationTexture);
            MOBILEGL_ASSERT(dstClearReady,
                            "CopyTexSubImage2D: failed to materialize pending clear for destination textureId=%d",
                            destinationTexture->GetExternalIndex());
        }

        const Bool srcUsesSwapchainDepth = readIsDefaultFbo && (srcBinding.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) == 0;
        const VkImageLayout srcOriginalLayout = readIsDefaultFbo
            ? (srcUsesSwapchainDepth
                ? m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired)
                : m_swapchainObject.GetImageLayout(m_imageIndexAcquired))
            : *srcBinding.trackedLayout;
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            RecordTextureCopyError(__func__, ErrorCode::InvalidOperation,
                                   "CopyTexSubImage2D source image has undefined layout.");
            return;
        }

        const VkImageLayout dstOriginalLayout = *dstBinding.trackedLayout;
        const VkImageLayout dstRestoreLayout = dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? ((dstBinding.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            : dstOriginalLayout;

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcOriginalLayout, srcStageMask, srcAccessMask);
        if (readIsDefaultFbo) {
            VkImageLayout srcTrackedLayout = srcOriginalLayout;
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, srcTrackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, srcBinding.aspectMask,
                srcBinding.mipLevel, srcBinding.mipLevelCount);
            MOBILEGL_ASSERT(ok, "%s: failed to transition swapchain source image", __func__);
            if (srcUsesSwapchainDepth) {
                m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired, srcTrackedLayout);
            } else {
                m_swapchainObject.SetImageLayout(m_imageIndexAcquired, srcTrackedLayout);
            }
        } else {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, *srcBinding.trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, srcBinding.aspectMask,
                srcBinding.mipLevel, srcBinding.mipLevelCount);
            MOBILEGL_ASSERT(ok, "%s: failed to transition source image", __func__);
        }

        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionSourceState(dstOriginalLayout, dstStageMask, dstAccessMask);
        Bool dstReady = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, dstBinding.image, *dstBinding.trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
            dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, dstBinding.aspectMask,
            dstBinding.mipLevel, dstBinding.mipLevelCount);
        MOBILEGL_ASSERT(dstReady, "%s: failed to transition destination image", __func__);

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = srcBinding.aspectMask;
        copyRegion.srcSubresource.mipLevel = srcBinding.mipLevel;
        copyRegion.srcSubresource.baseArrayLayer = srcBinding.baseArrayLayer;
        copyRegion.srcSubresource.layerCount = srcBinding.layerCount;
        // KNOWN GAP, deliberately not half-fixed here: when the read framebuffer is the default
        // one this samples GL rows [y, y+h) counted from the TOP of a display-oriented image, so
        // it takes the mirrored band AND writes it into the (GL-oriented) destination texture
        // upside down. Correcting only the offset would swap one wrong answer for another,
        // because vkCmdCopyImage cannot reverse rows: this path has to become a vkCmdBlitImage
        // with an inverted source Y pair, the way BlitFramebuffer above now does it. Tracked
        // separately; the four sites behind the 1,759-case orientation defect are the viewport,
        // the scissor, the ReadPixels copy offset and the readback remap.
        if (readIsDefaultFbo) {
            MGLOG_D("DirectVulkan::CopyTexSubImage2D: copying from the DEFAULT framebuffer still uses the raw GL "
                    "Y origin (x=%d y=%d w=%d h=%d); the result is the mirrored band, stored flipped",
                    x, y, width, height);
        }
        copyRegion.srcOffset = {x, y, 0};
        copyRegion.dstSubresource.aspectMask = dstBinding.aspectMask;
        copyRegion.dstSubresource.mipLevel = dstBinding.mipLevel;
        copyRegion.dstSubresource.baseArrayLayer = dstBinding.baseArrayLayer;
        copyRegion.dstSubresource.layerCount = dstBinding.layerCount;
        copyRegion.dstOffset = {xoffset, yoffset, 0};
        copyRegion.extent = {static_cast<Uint32>(width), static_cast<Uint32>(height), 1};
        vkCmdCopyImage(frame.commandBuffer,
                       srcBinding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstBinding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copyRegion);

        VkPipelineStageFlags srcRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcRestoreAccessMask = 0;
        GetImageTransitionDestinationState(srcOriginalLayout, srcRestoreStageMask, srcRestoreAccessMask);
        if (readIsDefaultFbo) {
            VkImageLayout srcTrackedLayout = srcUsesSwapchainDepth
                ? m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired)
                : m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, srcTrackedLayout, srcOriginalLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask, srcBinding.aspectMask,
                srcBinding.mipLevel, srcBinding.mipLevelCount);
            MOBILEGL_ASSERT(ok, "%s: failed to restore swapchain source image layout", __func__);
            if (srcUsesSwapchainDepth) {
                m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired, srcTrackedLayout);
            } else {
                m_swapchainObject.SetImageLayout(m_imageIndexAcquired, srcTrackedLayout);
            }
        } else {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, *srcBinding.trackedLayout, srcOriginalLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask, srcBinding.aspectMask,
                srcBinding.mipLevel, srcBinding.mipLevelCount);
            MOBILEGL_ASSERT(ok, "%s: failed to restore source image layout", __func__);
        }

        VkPipelineStageFlags dstRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstRestoreAccessMask = 0;
        GetImageTransitionDestinationState(dstRestoreLayout, dstRestoreStageMask, dstRestoreAccessMask);
        Bool dstRestored = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, dstBinding.image, *dstBinding.trackedLayout, dstRestoreLayout,
            VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
            VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, dstBinding.aspectMask,
            dstBinding.mipLevel, dstBinding.mipLevelCount);
        MOBILEGL_ASSERT(dstRestored, "%s: failed to restore destination image layout", __func__);
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
        struct CopyImageEndpoint {
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

        Bool TryResolveCopyImageEndpoint(TextureTarget target,
                                         const VkTextureManager::TextureResource& resource, Uint32 mipLevel,
                                         GLint glZ, GLsizei glDepth, CopyImageEndpoint& outEndpoint) {
            if (glZ < 0 || glDepth <= 0) {
                return false;
            }
            const Uint32 baseSlice = static_cast<Uint32>(glZ);
            switch (target) {
            case TextureTarget::Texture1D:
            case TextureTarget::Texture2D:
            case TextureTarget::TextureRectangle:
            case TextureTarget::Texture2DMultisample:
                // Not layered at all: GL still requires the z/depth pair, and it can only name the
                // one slice these targets have.
                outEndpoint = {};
                return baseSlice == 0 && glDepth == 1;
            case TextureTarget::Texture3D:
                outEndpoint.slicesAreDepth = true;
                outEndpoint.baseSlice = baseSlice;
                outEndpoint.availableSlices = std::max(1u, resource.depth >> mipLevel);
                return true;
            case TextureTarget::Texture2DArray:
            case TextureTarget::Texture2DMultisampleArray:
            case TextureTarget::TextureCubeMap:
            case TextureTarget::TextureCubeMapArray:
                // A cube map is an array of six faces here (see TryResolveTextureShapeInfo), and GL
                // numbers its faces on the same z axis an array texture numbers its layers, so both
                // arrive as a plain layer range.
                outEndpoint.slicesAreDepth = false;
                outEndpoint.baseSlice = baseSlice;
                outEndpoint.availableSlices = resource.arrayLayers;
                return true;
            default:
                // GL_TEXTURE_1D_ARRAY carries its layers on the Y axis (srcY/srcHeight), which
                // would have to be remapped against a Vulkan extent that also has to stay height 1
                // for a VK_IMAGE_TYPE_1D image; GL_TEXTURE_BUFFER has no image at all. Declined
                // rather than mis-addressed.
                return false;
            }
        }
    } // namespace

    void VulkanRenderer::CopyImageSubData(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                          const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        MOBILEGL_ASSERT(srcTexture != nullptr && dstTexture != nullptr,
                        "CopyImageSubData requires valid source and destination textures.");
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
        if (srcTexture.get() == dstTexture.get()) {
            MGLOG_E_ONCE("%s: in-place copy on textureId=%d is not supported; declining the copy", __func__,
                         srcTexture->GetExternalIndex());
            return;
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }

        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        auto* srcResource = m_textureManager->SyncTextureAndGetDescriptor(*srcTexture);
        auto* dstResource = m_textureManager->SyncTextureAndGetDescriptor(*dstTexture);
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
        if (srcResource == nullptr || dstResource == nullptr) {
            MGLOG_E_ONCE("%s: source or destination texture failed to sync; declining the copy", __func__);
            return;
        }
        if (srcLevel < 0 || dstLevel < 0 || static_cast<Uint32>(srcLevel) >= srcResource->mipLevels ||
            static_cast<Uint32>(dstLevel) >= dstResource->mipLevels) {
            MGLOG_E_ONCE("%s: mip level out of range (src %d of %u, dst %d of %u); declining the copy", __func__,
                         srcLevel, srcResource->mipLevels, dstLevel, dstResource->mipLevels);
            return;
        }
        const VkImageAspectFlags copyAspectMask =
            srcResource->aspect & dstResource->aspect &
            (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        MOBILEGL_ASSERT(copyAspectMask != 0 &&
                        (srcResource->aspect & copyAspectMask) == srcResource->aspect &&
                        (dstResource->aspect & copyAspectMask) == dstResource->aspect,
                        "CopyImageSubData source and destination aspects are incompatible.");
        const Uint32 srcMipLevel = static_cast<Uint32>(srcLevel);
        const Uint32 dstMipLevel = static_cast<Uint32>(dstLevel);
        const Uint32 srcMipWidth = std::max(1u, srcResource->extent.width >> srcMipLevel);
        const Uint32 srcMipHeight = std::max(1u, srcResource->extent.height >> srcMipLevel);
        const Uint32 dstMipWidth = std::max(1u, dstResource->extent.width >> dstMipLevel);
        const Uint32 dstMipHeight = std::max(1u, dstResource->extent.height >> dstMipLevel);
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
        CopyImageEndpoint srcEndpoint;
        CopyImageEndpoint dstEndpoint;
        if (!TryResolveCopyImageEndpoint(srcTextureTarget, *srcResource, srcMipLevel, srcZ, srcDepth, srcEndpoint) ||
            !TryResolveCopyImageEndpoint(dstTextureTarget, *dstResource, dstMipLevel, dstZ, srcDepth, dstEndpoint)) {
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
        if (srcEndpoint.baseSlice + copySliceCount > srcEndpoint.availableSlices ||
            dstEndpoint.baseSlice + copySliceCount > dstEndpoint.availableSlices) {
            MGLOG_E_ONCE("%s: slice range outside image bounds (srcZ=%d of %u, dstZ=%d of %u, depth=%d); "
                         "declining the copy",
                         __func__, srcZ, srcEndpoint.availableSlices, dstZ, dstEndpoint.availableSlices, srcDepth);
            return;
        }

        const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *srcTexture);
        MOBILEGL_ASSERT(clearReady, "%s: failed to materialize pending clear for source textureId=%d",
                        __func__, srcTexture->GetExternalIndex());
        // A clear still parked on the destination would otherwise materialize AFTER this copy and
        // wipe the texels it just wrote.
        const Bool dstClearReady = MaterializePendingClearForTexture(frame.commandBuffer, *dstTexture);
        MOBILEGL_ASSERT(dstClearReady, "%s: failed to materialize pending clear for destination textureId=%d",
                        __func__, dstTexture->GetExternalIndex());

        const VkImageLayout srcOriginalLayout = srcResource->layout;
        const VkImageLayout dstOriginalLayout = dstResource->layout;
        // A layout of UNDEFINED means nothing has ever been written to the image, which on the
        // SOURCE side is glTexStorage without an upload: legal GL, and the texels it copies are
        // undefined by the same spec sentence that lets the application ask. Both sides therefore
        // take the same shape - transition the whole image out of UNDEFINED and settle it on a
        // real layout afterwards, since UNDEFINED is not a layout a barrier may transition BACK to.
        const auto resolveRestoreLayout = [copyAspectMask](VkImageLayout originalLayout) {
            if (originalLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
                return originalLayout;
            }
            return (copyAspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        };
        const VkImageLayout srcRestoreLayout = resolveRestoreLayout(srcOriginalLayout);
        const VkImageLayout dstRestoreLayout = resolveRestoreLayout(dstOriginalLayout);

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcOriginalLayout, srcStageMask, srcAccessMask);
        VkImageLayout srcCopyLayout = srcOriginalLayout;
        // The barriers below name a MIP range only. Their layer range is not a parameter:
        // TransitionImageLayout always covers every layer of the image, which is a superset of the
        // [baseSlice, baseSlice + depth) the slice mapping above hands the copy.
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool srcReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcResource->image, srcResource->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT,
                srcResource->aspect, 0, srcResource->mipLevels);
            MOBILEGL_ASSERT(srcReady, "%s: failed to transition undefined source image", __func__);
            srcCopyLayout = srcResource->layout;
        } else {
            Bool srcReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcResource->image, srcCopyLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, copyAspectMask, srcMipLevel, 1);
            MOBILEGL_ASSERT(srcReady, "%s: failed to transition source image", __func__);
        }

        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionSourceState(dstOriginalLayout, dstStageMask, dstAccessMask);
        VkImageLayout dstCopyLayout = dstOriginalLayout;
        if (dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstResource->image, dstResource->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                dstResource->aspect, 0, dstResource->mipLevels);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition undefined destination image", __func__);
            dstCopyLayout = dstResource->layout;
        } else {
            Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstResource->image, dstCopyLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, copyAspectMask, dstMipLevel, 1);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition destination image", __func__);
        }

        // The GL slice count reaches Vulkan on the layer axis of whichever endpoint is NOT 3D, and
        // on extent.depth as soon as either endpoint IS: a 3D image's subresource is always the
        // single layer (0, 1) and its slices are counted by the depth of the copy extent. With two
        // non-3D endpoints both layer counts carry it and extent.depth stays 1.
        const Bool copyCrossesDepthAxis = srcEndpoint.slicesAreDepth || dstEndpoint.slicesAreDepth;
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = copyAspectMask;
        copyRegion.srcSubresource.mipLevel = srcMipLevel;
        copyRegion.srcSubresource.baseArrayLayer = srcEndpoint.BaseArrayLayer();
        copyRegion.srcSubresource.layerCount = srcEndpoint.slicesAreDepth ? 1u : copySliceCount;
        copyRegion.srcOffset = {srcX, srcY, srcEndpoint.OffsetZ()};
        copyRegion.dstSubresource.aspectMask = copyAspectMask;
        copyRegion.dstSubresource.mipLevel = dstMipLevel;
        copyRegion.dstSubresource.baseArrayLayer = dstEndpoint.BaseArrayLayer();
        copyRegion.dstSubresource.layerCount = dstEndpoint.slicesAreDepth ? 1u : copySliceCount;
        copyRegion.dstOffset = {dstX, dstY, dstEndpoint.OffsetZ()};
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
                       srcResource->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstResource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copyRegion);

        VkPipelineStageFlags srcRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcRestoreAccessMask = 0;
        GetImageTransitionDestinationState(srcRestoreLayout, srcRestoreStageMask, srcRestoreAccessMask);
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool srcRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcResource->image, srcResource->layout, srcRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask,
                srcResource->aspect, 0, srcResource->mipLevels);
            MOBILEGL_ASSERT(srcRestored, "%s: failed to restore undefined source image layout", __func__);
        } else {
            Bool srcRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcResource->image, srcCopyLayout, srcRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask, copyAspectMask, srcMipLevel, 1);
            MOBILEGL_ASSERT(srcRestored, "%s: failed to restore source image layout", __func__);
        }

        VkPipelineStageFlags dstRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstRestoreAccessMask = 0;
        GetImageTransitionDestinationState(dstRestoreLayout, dstRestoreStageMask, dstRestoreAccessMask);
        if (dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool dstRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstResource->image, dstResource->layout, dstRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask,
                dstResource->aspect, 0, dstResource->mipLevels);
            MOBILEGL_ASSERT(dstRestored, "%s: failed to restore undefined destination image layout", __func__);
        } else {
            Bool dstRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstResource->image, dstCopyLayout, dstRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, copyAspectMask, dstMipLevel, 1);
            MOBILEGL_ASSERT(dstRestored, "%s: failed to restore destination image layout", __func__);
        }

    }

    Bool VulkanRenderer::FinishPendingGpuWork() {
        MakeXfbWritesVisible();
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            return true;
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }
        return SubmitReadbackCommandsAndWait(frame);
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

        VkResult result = vkWaitForFences(m_device, 1, &frame.imageInFlightFence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
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
        // The wait proved every submission complete, so the full frame-boundary
        // drain applies: descriptor cursors, transient arenas, deferred
        // texture/buffer releases, retired command buffers and the converted
        // vertex-stream cache all rewind here, keeping present-less readback
        // loops bounded (Present is the only other drain point).
        TryDrainFrameTransients();
        return true;
    }

    void VulkanRenderer::ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                    void* pixels) {
        if (width <= 0 || height <= 0) {
            return;
        }

        auto readFbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
        if (readFbo == nullptr) {
            MGLOG_E_ONCE("DirectVulkan::ReadPixels skipped: no read framebuffer is bound");
            return;
        }

        if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL || format == GL_STENCIL_INDEX) {
            ReadDepthStencilPixels(*readFbo, x, y, width, height, format, type, pixels);
            return;
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const Bool readIsDefaultFbo = readFbo->IsDefaultFramebuffer();
        // Materialize any pending clear on the read-buffer attachment BEFORE resolving the
        // blit binding below: for a renderbuffer/texture that has never been part of any
        // render pass yet (e.g. a GL_NONE draw buffer slot whose attachment is only ever
        // touched via an explicit glReadBuffer), materializing lazily creates its backing
        // Vulkan resource for the first time. UnorderedMap is open-addressing and may
        // rehash on that insertion, invalidating any RenderbufferResource*/TextureResource*
        // obtained beforehand - so ResolveColorBlitBinding's cached `trackedLayout` pointer
        // must be taken AFTER this, never before it.
        //
        // The default framebuffer needs this just as much, and used to be excluded: its clear is
        // parked the same way, and with no draw between the clear and the readback no render
        // pass ever runs to fold it in, so the readback returned the previous frame's image
        // (KHR-GL40.draw_indirect.negative-*). It only takes a different materializer because the
        // image to clear is the acquired swapchain image, not the attachment's placeholder
        // texture.
        if (readIsDefaultFbo) {
            const Bool clearReady = MaterializePendingClearForDefaultFramebuffer(frame.commandBuffer, *readFbo,
                                                                                 readFbo->GetReadBuffer());
            MOBILEGL_ASSERT(clearReady, "ReadPixels: failed to materialize the default framebuffer's pending clear");
        } else {
            const auto& sourceAttachment = readFbo->GetAttachment(readFbo->GetReadBuffer());
            auto sourceTexture = sourceAttachment.GetTexture();
            if (sourceTexture != nullptr) {
                const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sourceTexture);
                MOBILEGL_ASSERT(clearReady,
                                "ReadPixels: failed to materialize pending clear for source textureId=%d",
                                sourceTexture->GetExternalIndex());
            } else if (sourceAttachment.IsRenderbuffer()) {
                const Bool clearReady =
                    MaterializePendingClearForRenderbuffer(frame.commandBuffer, sourceAttachment.GetRenderbuffer());
                MOBILEGL_ASSERT(clearReady,
                                "ReadPixels: failed to materialize pending clear for source renderbuffer %u",
                                sourceAttachment.GetRenderbuffer()->GetExternalIndex());
            }
        }

        BlitImageBinding srcBinding{};
        if (!ResolveColorBlitBinding(*readFbo, true, m_imageIndexAcquired, m_swapchainObject, *m_textureManager,
                                     *m_renderPassManager, srcBinding)) {
            return;
        }

        const VkImageLayout srcOriginalLayout = readIsDefaultFbo
            ? m_swapchainObject.GetImageLayout(m_imageIndexAcquired)
            : *srcBinding.trackedLayout;
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            MGLOG_E_ONCE("DirectVulkan::ReadPixels skipped: source image layout is undefined");
            return;
        }

        const VkFormat srcFormat = srcBinding.format;
        const SizeT sourceTexelSize = GetReadbackTexelSize(srcFormat);
        if (sourceTexelSize == 0) {
            MGLOG_E_ONCE("DirectVulkan::ReadPixels skipped: unsupported source format=%d",
                    static_cast<Int>(srcFormat));
            return;
        }
        const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(width) *
                                          static_cast<VkDeviceSize>(height) * sourceTexelSize;
        VkBufferObject readback;
        if (!readback.Create({
                .allocator = m_allocator,
                .size = readbackSize,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            })) {
            MGLOG_E_ONCE("DirectVulkan::ReadPixels skipped: failed to create readback buffer");
            return;
        }

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcOriginalLayout, srcStageMask, srcAccessMask);
        // The copy below reads `srcBinding.baseArrayLayer`, which for a glFramebufferTextureLayer
        // attachment is any layer of the array - the barrier covers all of them (see
        // VkTextureManager::TransitionImageLayout), so the layer being read is one it moved.
        if (readIsDefaultFbo) {
            VkImageLayout trackedLayout = srcOriginalLayout;
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, srcBinding.aspectMask);
            MOBILEGL_ASSERT(ok, "%s: failed to transition swapchain source image", __func__);
            m_swapchainObject.SetImageLayout(m_imageIndexAcquired, trackedLayout);
        } else {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, *srcBinding.trackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, srcBinding.aspectMask,
                srcBinding.mipLevel, 1);
            MOBILEGL_ASSERT(ok, "%s: failed to transition source image", __func__);
        }

        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = srcBinding.aspectMask;
        copyRegion.imageSubresource.mipLevel = srcBinding.mipLevel;
        copyRegion.imageSubresource.baseArrayLayer = srcBinding.baseArrayLayer;
        copyRegion.imageSubresource.layerCount = 1;
        // The GL rect, aimed at the default framebuffer's stored orientation. Using the GL y
        // verbatim copied rows [y, y+h) counted from the TOP of the image, i.e. the wrong band for
        // every read that was not full-height.
        VkOffset2D copyOffset{x, y};
        VkExtent2D copyExtent{static_cast<Uint32>(width), static_cast<Uint32>(height)};
        if (readIsDefaultFbo) {
            const VkExtent2D defaultFboExtent = m_swapchainObject.GetExtent();
            const Bool mapped = MapDefaultFramebufferReadbackRect(
                x, y, width, height, defaultFboExtent, m_swapchainObject.GetPreTransform(), &copyOffset,
                &copyExtent);
            MOBILEGL_ASSERT(mapped, "ReadPixels: default framebuffer read rectangle is out of bounds");
            if (!mapped) return;
        }
        copyRegion.imageOffset = {copyOffset.x, copyOffset.y, static_cast<Int32>(srcBinding.depthOffset)};
        copyRegion.imageExtent = {copyExtent.width, copyExtent.height, 1};
        vkCmdCopyImageToBuffer(frame.commandBuffer, srcBinding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.GetHandle(), 1, &copyRegion);

        VkPipelineStageFlags restoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags restoreAccessMask = 0;
        GetImageTransitionDestinationState(srcOriginalLayout, restoreStageMask, restoreAccessMask);
        if (readIsDefaultFbo) {
            VkImageLayout trackedLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, trackedLayout, srcOriginalLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, restoreStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, restoreAccessMask, srcBinding.aspectMask);
            MOBILEGL_ASSERT(ok, "%s: failed to restore swapchain source image layout", __func__);
            m_swapchainObject.SetImageLayout(m_imageIndexAcquired, trackedLayout);
        } else {
            Bool ok = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcBinding.image, *srcBinding.trackedLayout, srcOriginalLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, restoreStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, restoreAccessMask, srcBinding.aspectMask,
                srcBinding.mipLevel, 1);
            MOBILEGL_ASSERT(ok, "%s: failed to restore source image layout", __func__);
        }

        if (!SubmitReadbackCommandsAndWait(frame)) {
            return;
        }
        const auto* mapped = static_cast<const Uint8*>(readback.Map());
        if (mapped == nullptr) {
            MGLOG_E_ONCE("DirectVulkan::ReadPixels skipped: failed to map readback buffer");
            return;
        }
        if (!readback.Invalidate(readbackSize)) {
            MGLOG_E_ONCE("DirectVulkan::ReadPixels skipped: failed to invalidate readback buffer");
            return;
        }
        if (readIsDefaultFbo) {
            const VkSurfaceTransformFlagBitsKHR preTransform = m_swapchainObject.GetPreTransform();
            // No full-extent gate any more: the remap works on the copied rect, and the copy was
            // already aimed with the same mapping. The gate is exactly what made every partial
            // read of the default framebuffer come back in Vulkan row order.
            Vector<Uint8> remapped(static_cast<SizeT>(width) * static_cast<SizeT>(height) * sourceTexelSize);
            if (RemapDefaultFramebufferReadback(mapped, static_cast<Uint32>(width),
                                                static_cast<Uint32>(height), preTransform, sourceTexelSize,
                                                remapped.data())) {
                PackReadbackToClientOrPbo(remapped.data(), srcFormat, width, height, 1, format, type, pixels,
                                          /*applyPackImageParams=*/false, /*applyReadColorClamp=*/true);
                return;
            }
            MGLOG_D("DirectVulkan::ReadPixels: default-FBO remap failed (w=%d h=%d preTransform=%d); falling back "
                    "to raw readback",
                    width, height, static_cast<Int>(preTransform));
        }
        PackReadbackToClientOrPbo(mapped, srcFormat, width, height, 1, format, type, pixels,
                                  /*applyPackImageParams=*/false, /*applyReadColorClamp=*/true);
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

    void VulkanRenderer::ReadDepthStencilPixels(MG_State::GLState::FramebufferObject& readFbo, GLint x, GLint y,
                                                GLsizei width, GLsizei height, GLenum format, GLenum type,
                                                void* pixels) {
        if (width <= 0 || height <= 0) {
            return;
        }

        const Bool wantDepth = format != GL_STENCIL_INDEX;
        const Bool wantStencil = format != GL_DEPTH_COMPONENT;
        // GL_DEPTH_STENCIL requires both halves; the state layer already rejected
        // framebuffers lacking either, so resolving via the depth attachment is enough.
        const auto attachmentType = wantDepth ? MobileGL::FramebufferAttachmentType::Depth
                                              : MobileGL::FramebufferAttachmentType::Stencil;
        const Bool readIsDefaultFbo = readFbo.IsDefaultFramebuffer();
        if (!readIsDefaultFbo) {
            const auto& attachment = readFbo.GetAttachment(attachmentType);
            if (!attachment.IsValid() || attachment.IsEmpty()) {
                MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: no depth/stencil attachment image");
                return;
            }
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        // The default framebuffer's depth/stencil lives in the swapchain, not in an
        // attachment object: its placeholder ITextureObject describes the format but backs no
        // image, so the branches below would have synced (and read back) an unrelated one.
        // Declining outright is what made every glReadPixels(GL_DEPTH_COMPONENT/
        // GL_STENCIL_INDEX) of the default framebuffer leave the caller's buffer untouched -
        // the whole KHR-GL*.framebuffer_blit family checks exactly that before it blits.
        if (readIsDefaultFbo) {
            const VkImage swapchainDepthImage = m_swapchainObject.GetDepthStencilImage(m_imageIndexAcquired);
            if (swapchainDepthImage == VK_NULL_HANDLE) {
                MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: the default framebuffer has no "
                        "depth/stencil image");
                return;
            }
            // Per aspect, because the default framebuffer carries a SEPARATE placeholder
            // attachment for depth and for stencil (MG_Impl/Init.cpp) and each parks its own
            // pending clear; materializing only one would read the other back un-cleared.
            if (wantDepth) {
                const Bool clearReady = MaterializePendingClearForDefaultFramebuffer(
                    frame.commandBuffer, readFbo, MobileGL::FramebufferAttachmentType::Depth);
                MOBILEGL_ASSERT(clearReady,
                                "ReadDepthStencilPixels: failed to materialize the default framebuffer's pending "
                                "depth clear");
            }
            if (wantStencil) {
                const Bool clearReady = MaterializePendingClearForDefaultFramebuffer(
                    frame.commandBuffer, readFbo, MobileGL::FramebufferAttachmentType::Stencil);
                MOBILEGL_ASSERT(clearReady,
                                "ReadDepthStencilPixels: failed to materialize the default framebuffer's pending "
                                "stencil clear");
            }
            const VkFormat swapchainDepthFormat = m_swapchainObject.GetDepthStencilFormat();
            VkImageLayout trackedLayout = m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired);
            ReadDepthStencilImageToClient(swapchainDepthImage, swapchainDepthFormat, &trackedLayout,
                                          GetDepthStencilAspectMaskForFormat(swapchainDepthFormat), 0, 0, x, y,
                                          width, height, format, type, pixels,
                                          /*defaultFramebufferOrientation=*/true);
            m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired, trackedLayout);
            return;
        }

        const auto& attachment = readFbo.GetAttachment(attachmentType);
        VkImage image = VK_NULL_HANDLE;
        VkFormat vkFormat = VK_FORMAT_UNDEFINED;
        VkImageLayout* trackedLayout = nullptr;
        VkImageAspectFlags imageAspect = VK_IMAGE_ASPECT_NONE;
        Uint32 mipLevel = 0;
        Uint32 baseArrayLayer = 0;
        if (attachment.IsTexture() && attachment.GetTexture()) {
            auto textureObject = attachment.GetTexture();
            const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *textureObject);
            MOBILEGL_ASSERT(clearReady, "ReadDepthStencilPixels: failed to materialize pending clear for textureId=%d",
                            textureObject->GetExternalIndex());
            auto* resource = m_textureManager->SyncTextureAndGetDescriptor(*textureObject);
            if (resource == nullptr || resource->image == VK_NULL_HANDLE) {
                MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: failed to sync depth textureId=%u",
                        textureObject->GetExternalIndex());
                return;
            }
            image = resource->image;
            vkFormat = resource->format;
            trackedLayout = &resource->layout;
            imageAspect = resource->aspect;
            mipLevel = static_cast<Uint32>(std::max(attachment.GetTextureLevel(), 0));
            baseArrayLayer = static_cast<Uint32>(std::max(attachment.GetTextureLayer(), 0));
        } else if (attachment.IsRenderbuffer() && attachment.GetRenderbuffer()) {
            const auto& renderbufferObject = attachment.GetRenderbuffer();
            const Bool clearReady = MaterializePendingClearForRenderbuffer(frame.commandBuffer, renderbufferObject);
            MOBILEGL_ASSERT(clearReady,
                            "ReadDepthStencilPixels: failed to materialize pending clear for renderbuffer %u",
                            renderbufferObject->GetExternalIndex());
            auto* resource = m_renderPassManager->GetOrCreateRenderbufferResource(renderbufferObject);
            if (resource == nullptr || resource->image == VK_NULL_HANDLE) {
                MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: failed to resolve renderbuffer %u",
                        renderbufferObject->GetExternalIndex());
                return;
            }
            image = resource->image;
            vkFormat = resource->format;
            trackedLayout = &resource->layout;
            imageAspect = resource->aspect;
        } else {
            return;
        }

        ReadDepthStencilImageToClient(image, vkFormat, trackedLayout, imageAspect, mipLevel, baseArrayLayer, x, y,
                                      width, height, format, type, pixels);
    }

    void VulkanRenderer::ReadDepthStencilImageToClient(VkImage image, VkFormat vkFormat, VkImageLayout* trackedLayout,
                                                       VkImageAspectFlags imageAspect, Uint32 mipLevel,
                                                       Uint32 baseArrayLayer, GLint x, GLint y, GLsizei width,
                                                       GLsizei height, GLenum format, GLenum type, void* pixels,
                                                       Bool defaultFramebufferOrientation) {
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

        VkBufferImageCopy regions[2]{};
        Uint32 regionCount = 0;
        if (wantDepth) {
            auto& region = regions[regionCount++];
            region.bufferOffset = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            region.imageSubresource.mipLevel = mipLevel;
            region.imageSubresource.baseArrayLayer = baseArrayLayer;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {copyOffset.x, copyOffset.y, 0};
            region.imageExtent = {copyExtent.width, copyExtent.height, 1};
        }
        if (wantStencil) {
            auto& region = regions[regionCount++];
            region.bufferOffset = stencilOffset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            region.imageSubresource.mipLevel = mipLevel;
            region.imageSubresource.baseArrayLayer = baseArrayLayer;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {copyOffset.x, copyOffset.y, 0};
            region.imageExtent = {copyExtent.width, copyExtent.height, 1};
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

        const auto depthValueAt = [&](SizeT i) -> Float {
            switch (vkFormat) {
            case VK_FORMAT_D16_UNORM: {
                Uint16 raw = 0;
                Memcpy(&raw, depthSrc + i * 2, sizeof(raw));
                return static_cast<Float>(raw) / 65535.0f;
            }
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D24_UNORM_S8_UINT: {
                Uint32 raw = 0;
                Memcpy(&raw, depthSrc + i * 4, sizeof(raw));
                return static_cast<Float>(raw & 0xFFFFFFu) / static_cast<Float>(0xFFFFFFu);
            }
            default: { // D32_SFLOAT / D32_SFLOAT_S8_UINT
                Float raw = 0.0f;
                Memcpy(&raw, depthSrc + i * 4, sizeof(raw));
                return raw;
            }
            }
        };

        SizeT dstPixelBytes = 0;
        switch (type) {
        case GL_FLOAT:
        case GL_UNSIGNED_INT:
        case GL_INT:
        case GL_UNSIGNED_INT_24_8:
            dstPixelBytes = 4;
            break;
        case GL_UNSIGNED_SHORT:
        case GL_SHORT:
            dstPixelBytes = 2;
            break;
        case GL_UNSIGNED_BYTE:
        case GL_BYTE:
            dstPixelBytes = 1;
            break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
            dstPixelBytes = 8;
            break;
        default:
            MGLOG_E_ONCE("DirectVulkan::ReadDepthStencilPixels skipped: unsupported type=0x%x", type);
            return;
        }

        // GL 4.6 core 18.2.8: a GL_STENCIL_INDEX read reports the index itself, unconverted, in
        // whatever width the client asked for. Only the packed types mix depth in. Deciding this
        // once - rather than per type, where GL_FLOAT and GL_UNSIGNED_SHORT used to emit a depth
        // value that is meaningless for a stencil-only image - is what makes the CTS's
        // (GL_STENCIL_INDEX, GL_INT) read return 7 instead of nothing.
        const Bool stencilOnly = format == GL_STENCIL_INDEX;

        Vector<Uint8> packed(pixelCount * dstPixelBytes);
        for (SizeT i = 0; i < pixelCount; ++i) {
            Uint8* dst = packed.data() + i * dstPixelBytes;
            switch (type) {
            case GL_FLOAT: {
                const Float value = stencilOnly ? static_cast<Float>(stencilSrc[i]) : depthValueAt(i);
                Memcpy(dst, &value, sizeof(value));
                break;
            }
            case GL_UNSIGNED_SHORT:
            case GL_SHORT: {
                const Uint16 value = stencilOnly
                    ? static_cast<Uint16>(stencilSrc[i])
                    : static_cast<Uint16>(std::lround(static_cast<double>(depthValueAt(i)) * 65535.0));
                Memcpy(dst, &value, sizeof(value));
                break;
            }
            case GL_UNSIGNED_INT:
            case GL_INT: {
                const Uint32 value = stencilOnly
                    ? stencilSrc[i]
                    : static_cast<Uint32>(static_cast<double>(depthValueAt(i)) * 4294967295.0);
                Memcpy(dst, &value, sizeof(value));
                break;
            }
            case GL_UNSIGNED_BYTE:
            case GL_BYTE: {
                dst[0] = stencilSrc[i];
                break;
            }
            case GL_UNSIGNED_INT_24_8: {
                const Uint32 depth24 =
                    static_cast<Uint32>(std::lround(static_cast<double>(depthValueAt(i)) * 16777215.0)) & 0xFFFFFFu;
                const Uint32 value = (depth24 << 8) | stencilSrc[i];
                Memcpy(dst, &value, sizeof(value));
                break;
            }
            case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: {
                const Float depthValue = depthValueAt(i);
                const Uint32 stencilValue = stencilSrc[i];
                Memcpy(dst, &depthValue, sizeof(depthValue));
                Memcpy(dst + 4, &stencilValue, sizeof(stencilValue));
                break;
            }
            default:
                break;
            }
        }

        // Store honoring the client pack state (single slice).
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
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

    void VulkanRenderer::GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto textureObject = activeUnit.GetBindingSlot(textureTarget).GetBoundObject();
        GetTextureImage(textureObject, textureUploadTarget, level, format, type, -1, pixels);
    }

    void VulkanRenderer::GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                         TextureUploadTarget textureUploadTarget, GLint level, GLenum format,
                                         GLenum type, GLsizei bufSize, GLvoid* pixels) {
        if (textureObject == nullptr || textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            return;
        }

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (level < 0 || static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MGLOG_E_ONCE("DirectVulkan::GetTexImage skipped: level %d is out of range", level);
            return;
        }

        auto* resource = m_textureManager->SyncTextureAndGetDescriptor(*textureObject);
        if (resource == nullptr || resource->image == VK_NULL_HANDLE) {
            MGLOG_E_ONCE("DirectVulkan::GetTexImage skipped: failed to sync textureId=%u",
                    textureObject->GetExternalIndex());
            return;
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }
        const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *textureObject);
        MOBILEGL_ASSERT(clearReady,
                        "GetTexImage: failed to materialize pending clear for textureId=%d",
                        textureObject->GetExternalIndex());

        if ((resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
            if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL || format == GL_STENCIL_INDEX) {
                const auto levelSize =
                    textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, static_cast<Uint>(level));
                const Bool isCubeFace = textureUploadTarget >= TextureUploadTarget::CubeMapPositiveX &&
                    textureUploadTarget <= TextureUploadTarget::CubeMapNegativeZ;
                const Uint32 arrayLayer = isCubeFace
                    ? static_cast<Uint32>(textureUploadTarget) -
                        static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX)
                    : 0;
                ReadDepthStencilImageToClient(resource->image, resource->format, &resource->layout, resource->aspect,
                                              static_cast<Uint32>(level), arrayLayer, 0, 0, levelSize.x(),
                                              levelSize.y(), format, type, pixels);
            } else {
                MGLOG_E_ONCE("DirectVulkan::GetTexImage skipped: color query of a non-color texture");
            }
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, static_cast<Uint>(level));
        const GLsizei width = texelSize.x();
        const GLsizei height = texelSize.y();
        if (width <= 0 || height <= 0) {
            return;
        }
        // GetTexImage returns every slice of a 3D level and every layer of an array
        // level; GL_PACK_IMAGE_HEIGHT / GL_PACK_SKIP_IMAGES apply to the 3D/array
        // destination layout (GL 3.3 section 6.1.4).
        const auto imageTextureTarget = textureObject->GetTarget();
        const Bool is3dImage = imageTextureTarget == TextureTarget::Texture3D;
        const Bool isArrayImage = imageTextureTarget == TextureTarget::Texture1DArray ||
                                  imageTextureTarget == TextureTarget::Texture2DArray ||
                                  imageTextureTarget == TextureTarget::TextureCubeMapArray;
        const GLsizei depthSlices = is3dImage ? std::max<GLsizei>(texelSize.z(), 1) : 1;
        const GLsizei arrayLayers = isArrayImage ? static_cast<GLsizei>(resource->arrayLayers) : 1;
        const GLsizei sliceCount = std::max<GLsizei>(depthSlices * arrayLayers, 1);
        if (bufSize >= 0) {
            const Int dstChannels = GetReadbackChannelCount(format);
            if ((type == GL_UNSIGNED_BYTE || type == GL_FLOAT) && dstChannels > 0) {
                const SizeT dstComponentSize = type == GL_FLOAT ? sizeof(Float) : sizeof(Uint8);
                const SizeT minSize = static_cast<SizeT>(width) * static_cast<SizeT>(height) *
                                      static_cast<SizeT>(dstChannels) * dstComponentSize;
                if (static_cast<SizeT>(bufSize) < minSize) {
                    MGLOG_E_ONCE("DirectVulkan::GetTextureImage skipped: destination buffer is too small");
                    return;
                }
            }
        }

        const SizeT sourceTexelSize = GetReadbackTexelSize(resource->format);
        if (sourceTexelSize == 0) {
            MGLOG_E_ONCE("DirectVulkan::GetTexImage skipped: unsupported source format=%d",
                    static_cast<Int>(resource->format));
            return;
        }
        const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(width) *
                                          static_cast<VkDeviceSize>(height) *
                                          static_cast<VkDeviceSize>(sliceCount) * sourceTexelSize;
        VkBufferObject readback;
        if (!readback.Create({
                .allocator = m_allocator,
                .size = readbackSize,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            })) {
            MGLOG_E_ONCE("DirectVulkan::GetTexImage skipped: failed to create readback buffer");
            return;
        }

        const VkImageLayout originalLayout = resource->layout;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(originalLayout, srcStageMask, srcAccessMask);
        // The copy below reads EVERY layer of the level, which is exactly the range
        // TransitionImageLayout barriers cover.
        Bool ok = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, resource->image, resource->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
            srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, resource->aspect,
            static_cast<Uint32>(level), 1);
        MOBILEGL_ASSERT(ok, "%s: failed to transition texture image", __func__);

        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = resource->aspect;
        copyRegion.imageSubresource.mipLevel = static_cast<Uint32>(level);
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = static_cast<Uint32>(arrayLayers);
        copyRegion.imageExtent = {static_cast<Uint32>(width), static_cast<Uint32>(height),
                                  static_cast<Uint32>(depthSlices)};
        vkCmdCopyImageToBuffer(frame.commandBuffer, resource->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.GetHandle(), 1, &copyRegion);

        VkPipelineStageFlags restoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags restoreAccessMask = 0;
        GetImageTransitionDestinationState(originalLayout, restoreStageMask, restoreAccessMask);
        ok = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, resource->image, resource->layout, originalLayout,
            VK_PIPELINE_STAGE_TRANSFER_BIT, restoreStageMask,
            VK_ACCESS_TRANSFER_READ_BIT, restoreAccessMask, resource->aspect,
            static_cast<Uint32>(level), 1);
        MOBILEGL_ASSERT(ok, "%s: failed to restore texture image layout", __func__);

        if (!SubmitReadbackCommandsAndWait(frame)) {
            return;
        }
        const auto* mapped = static_cast<const Uint8*>(readback.Map());
        if (mapped == nullptr) {
            MGLOG_E_ONCE("DirectVulkan::GetTextureImage skipped: failed to map readback buffer");
            return;
        }
        if (!readback.Invalidate(readbackSize)) {
            MGLOG_E_ONCE("DirectVulkan::GetTextureImage skipped: failed to invalidate readback buffer");
            return;
        }
        PackReadbackToClientOrPbo(mapped, resource->format, width, height, sliceCount, format, type, pixels,
                                  /*applyPackImageParams=*/is3dImage || isArrayImage);
    }

    void VulkanRenderer::GenerateMipmap(GLenum target) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        // The other mipmappable targets - 1D, 1D array, cube map array - are legal GL and the front
        // end lets them through, so reaching one here is a coverage gap in this backend, not a
        // broken invariant. Declining leaves the mip chain unwritten; asserting took the process
        // down with it.
        if (textureTarget != TextureTarget::Texture2D && textureTarget != TextureTarget::Texture2DArray &&
            textureTarget != TextureTarget::Texture3D && textureTarget != TextureTarget::TextureCubeMap &&
            // A 1D texture needs nothing special: its storage extent is {width, 1, 1}, so the blit
            // loop below already emits the y and z offsets of 0 and 1 that a 1D image requires.
            textureTarget != TextureTarget::Texture1D) {
            MGLOG_W_ONCE("GenerateMipmap: unsupported target %s", MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
            return;
        }

        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto texture = textureUnit.GetBindingSlot(textureTarget).GetBoundObject();
        MOBILEGL_ASSERT(texture != nullptr, "GenerateMipmap requires a bound texture.");
        MOBILEGL_ASSERT(texture->IsComplete(), "GenerateMipmap requires a complete texture.");

        auto* mipmapTexture = MG_State::GLState::AsMipmapTexture(texture.get());
        MOBILEGL_ASSERT(mipmapTexture != nullptr, "GenerateMipmap requires a mipmapped texture object.");

        const Uint32 currentMipLevelCount = static_cast<Uint32>(mipmapTexture->GetMipmapLevelCount());
        MOBILEGL_ASSERT(currentMipLevelCount > 0, "GenerateMipmap requires level 0 storage.");

        const Uint32 baseMipLevel = std::min(static_cast<Uint32>(texture->GetLevelRange().x()), currentMipLevelCount - 1);

        // A texture that has only ever defined level 0 carries a single-level backing, so defining
        // the rest of the chain below recreates the image and carries the old contents over with a
        // copy that is submitted and waited on out of band. Anything this frame has already
        // recorded into the old image is not submitted yet, so that copy would read pre-flush
        // content and every generated level would descend from a stale level 0 - the same hazard
        // the storage-usage upgrade flushes for before its own preserve-copy.
        if (m_textureManager->NeedsMipChainGrowth(*texture) && HasPendingRecordedWork()) {
            if (FlushPendingCommands()) {
                // Fresh command buffer: the sampled-descriptor-set memo describes bindings that
                // only existed in the retired one.
                m_lastSampledSetValid = false;
            }
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
        }

        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *texture);
        MOBILEGL_ASSERT(clearReady,
                        "GenerateMipmap: failed to materialize pending clear for textureId=%d",
                        texture->GetExternalIndex());

        auto* resource = m_textureManager->SyncTextureAndGetDescriptor(*texture);
        MOBILEGL_ASSERT(resource != nullptr && resource->image != VK_NULL_HANDLE,
                        "GenerateMipmap failed to sync the backend texture.");

        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, resource->format, &formatProperties);
        const VkFormatFeatureFlags optimalTilingFeatures = formatProperties.optimalTilingFeatures;
        const Bool isDepthOrStencilTexture =
            (resource->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
        const Bool supportsNativeBlit =
            (optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
            (optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        if (!isDepthOrStencilTexture && !supportsNativeBlit) {
            MGLOG_W_ONCE("GenerateMipmap skipped for textureId=%d because Vulkan format %d does not support blit-based mip generation",
                texture->GetExternalIndex(), static_cast<Int>(resource->format));
            return;
        }
        if (isDepthOrStencilTexture) {
            MOBILEGL_ASSERT((resource->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) == 0,
                            "GenerateMipmap: depth-stencil mipmap generation is not supported yet.");
        }

        const Bool allocatedMipmapStorage = EnsureGenerateMipmapStorageAllocated(*mipmapTexture, baseMipLevel);
        MOBILEGL_ASSERT(allocatedMipmapStorage, "GenerateMipmap could not allocate a full mip chain for this texture.");

        resource = m_textureManager->SyncTextureAndGetDescriptor(*texture);
        MOBILEGL_ASSERT(resource != nullptr && resource->image != VK_NULL_HANDLE,
                "GenerateMipmap failed to resync the backend texture after allocating mip storage.");
        if (resource->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            const VkImageLayout finalLayout = ResolveGenerateMipmapFinalLayout(resource->aspect);
            Bool transitioned = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, resource->image, resource->layout, finalLayout,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, VK_ACCESS_SHADER_READ_BIT, resource->aspect, 0, resource->mipLevels);
            MOBILEGL_ASSERT(transitioned, "GenerateMipmap: failed to transition uninitialized mip chain");
            return;
        }

        const IntVec3 storageBaseTexelSize = {
            static_cast<Int>(resource->extent.width),
            static_cast<Int>(resource->extent.height),
            static_cast<Int>(resource->depth),
        };
        const IntVec3 baseTexelSize = ComputeMipTexelSize(storageBaseTexelSize, baseMipLevel);
        const Uint32 requiredMipLevelCount = baseMipLevel + ComputeFullMipLevelCount(baseTexelSize);
        const Uint32 generateMipLevelCount = std::min(requiredMipLevelCount, resource->mipLevels);
        if (generateMipLevelCount <= baseMipLevel + 1) {
            resource->layout = ResolveGenerateMipmapFinalLayout(resource->aspect);
            return;
        }

        const VkImageLayout originalLayout = resource->layout;
        const VkImageLayout finalLayout = ResolveGenerateMipmapFinalLayout(resource->aspect);
        if (isDepthOrStencilTexture && !supportsNativeBlit) {
            const Bool supportsShaderDepthMipmap =
                (optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
                (optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            MOBILEGL_ASSERT(resource->aspect == VK_IMAGE_ASPECT_DEPTH_BIT,
                            "GenerateMipmap: shader fallback only supports depth-only textures.");
            MOBILEGL_ASSERT(textureTarget == TextureTarget::Texture2D && resource->depth == 1 && resource->arrayLayers == 1,
                            "GenerateMipmap: shader fallback only supports single-layer GL_TEXTURE_2D depth textures.");
            MOBILEGL_ASSERT(supportsShaderDepthMipmap,
                            "GenerateMipmap: depth texture format %d lacks sampled/depth-attachment support for shader fallback.",
                            static_cast<Int>(resource->format));
            const Bool depthReady = GenerateDepthMipmapWithShader(frame, *texture, *resource,
                                                                  baseMipLevel, generateMipLevelCount,
                                                                  storageBaseTexelSize, originalLayout, finalLayout);
            MOBILEGL_ASSERT(depthReady,
                            "GenerateMipmap: depth fallback failed for textureId=%d target=%d internalFormat=%d vkFormat=%d",
                            texture->GetExternalIndex(), static_cast<Int>(texture->GetTarget()),
                            static_cast<Int>(texture->GetFormat()), static_cast<Int>(resource->format));
            return;
        }

        const VkFilter blitFilter = isDepthOrStencilTexture
            ? VK_FILTER_NEAREST
            : ((optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0
                ? VK_FILTER_LINEAR
                : VK_FILTER_NEAREST);

        VkPipelineStageFlags originalSrcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags originalSrcAccessMask = 0;
        GetImageTransitionSourceState(originalLayout, originalSrcStageMask, originalSrcAccessMask);

        VkPipelineStageFlags finalDstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags finalDstAccessMask = 0;
        GetImageTransitionDestinationState(finalLayout, finalDstStageMask, finalDstAccessMask);

        if (originalLayout != finalLayout) {
            if (baseMipLevel > 0) {
                VkImageLayout lowerMipLayout = originalLayout;
                const Bool lowerReady = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, resource->image, lowerMipLayout, finalLayout,
                    originalSrcStageMask, finalDstStageMask,
                    originalSrcAccessMask, finalDstAccessMask,
                    resource->aspect, 0, baseMipLevel);
                MOBILEGL_ASSERT(lowerReady, "%s: failed to transition lower untouched mip levels", __func__);
            }

            if (generateMipLevelCount < resource->mipLevels) {
                VkImageLayout upperMipLayout = originalLayout;
                const Bool upperReady = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, resource->image, upperMipLayout, finalLayout,
                    originalSrcStageMask, finalDstStageMask,
                    originalSrcAccessMask, finalDstAccessMask,
                    resource->aspect, generateMipLevelCount, resource->mipLevels - generateMipLevelCount);
                MOBILEGL_ASSERT(upperReady, "%s: failed to transition upper untouched mip levels", __func__);
            }
        }

        VkImageLayout srcMipLayout = originalLayout;
        Bool srcReady = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, resource->image, srcMipLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            originalSrcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
            originalSrcAccessMask, VK_ACCESS_TRANSFER_READ_BIT,
            resource->aspect, baseMipLevel, 1);
        MOBILEGL_ASSERT(srcReady, "%s: failed to transition base mip level to transfer source", __func__);

        // Every generated level starts from originalLayout and ends up TRANSFER_DST_OPTIMAL, and
        // the loop below only ever moves a level OUT of that layout after it has been written - so
        // the whole range can be prepared in one barrier instead of one per level. That turns a
        // 12-level chain's 3(N-1)+1 barrier commands into 2(N-1)+2. Each level is still
        // individually transitioned to TRANSFER_SRC before it is read, so the write-then-read
        // dependency between consecutive levels is unchanged.
        if (generateMipLevelCount > baseMipLevel + 1) {
            VkImageLayout dstRangeLayout = originalLayout;
            const Bool dstRangeReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, resource->image, dstRangeLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                originalSrcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                originalSrcAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                resource->aspect, baseMipLevel + 1, generateMipLevelCount - (baseMipLevel + 1));
            MOBILEGL_ASSERT(dstRangeReady, "%s: failed to transition mip levels to transfer destination", __func__);
        }

        for (Uint32 level = baseMipLevel + 1; level < generateMipLevelCount; ++level) {
            const IntVec3 srcTexelSize = ComputeMipTexelSize(storageBaseTexelSize, level - 1);
            const IntVec3 dstTexelSize = ComputeMipTexelSize(storageBaseTexelSize, level);

            VkImageBlit blitRegion{};
            blitRegion.srcSubresource.aspectMask = resource->aspect;
            blitRegion.srcSubresource.mipLevel = level - 1;
            blitRegion.srcSubresource.baseArrayLayer = 0;
            blitRegion.srcSubresource.layerCount = resource->arrayLayers;
            blitRegion.srcOffsets[0] = {0, 0, 0};
            blitRegion.srcOffsets[1] = {srcTexelSize.x(), srcTexelSize.y(), srcTexelSize.z()};
            blitRegion.dstSubresource.aspectMask = resource->aspect;
            blitRegion.dstSubresource.mipLevel = level;
            blitRegion.dstSubresource.baseArrayLayer = 0;
            blitRegion.dstSubresource.layerCount = resource->arrayLayers;
            blitRegion.dstOffsets[0] = {0, 0, 0};
            blitRegion.dstOffsets[1] = {dstTexelSize.x(), dstTexelSize.y(), dstTexelSize.z()};

            vkCmdBlitImage(frame.commandBuffer,
                           resource->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blitRegion, blitFilter);

            VkImageLayout finishedSrcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            Bool srcRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, resource->image, finishedSrcLayout, finalLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, finalDstStageMask,
                VK_ACCESS_TRANSFER_READ_BIT, finalDstAccessMask,
                resource->aspect, level - 1, 1);
            MOBILEGL_ASSERT(srcRestored, "%s: failed to transition mip level %u to final layout", __func__, level - 1);

            if (level + 1 < generateMipLevelCount) {
                VkImageLayout nextSrcLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                Bool nextSrcReady = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, resource->image, nextSrcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    resource->aspect, level, 1);
                MOBILEGL_ASSERT(nextSrcReady, "%s: failed to prepare mip level %u as next transfer source", __func__, level);
            } else {
                VkImageLayout lastMipLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                Bool lastMipReady = VkTextureManager::TransitionImageLayout(
                    frame.commandBuffer, resource->image, lastMipLayout, finalLayout,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, finalDstStageMask,
                    VK_ACCESS_TRANSFER_WRITE_BIT, finalDstAccessMask,
                    resource->aspect, level, 1);
                MOBILEGL_ASSERT(lastMipReady, "%s: failed to transition last mip level to final layout", __func__);
            }
        }

        resource->layout = finalLayout;

        // The chain above is GPU work recorded into this frame's command buffer, which is not
        // submitted until the frame ends - but a texture upload goes out on a command buffer of
        // its own the moment it happens. A glTexSubImage2D into a level this just generated
        // would therefore reach the GPU FIRST and be overwritten by these blits, which is how
        // KHR-GL40.texture_gather.base-level lost the texels it wrote into level 1 right after
        // generating the chain. Submitting here is what orders the two.
        if (HasPendingRecordedWork() && FlushPendingCommands()) {
            // Fresh command buffer: the sampled-descriptor-set memo describes bindings that
            // only existed in the retired one.
            m_lastSampledSetValid = false;
        }
    }

    Uint32 VulkanRenderer::CurrentXfbCounterSlot() {
        const Uint name = MG_State::pGLContext->GetBoundTransformFeedbackName();
        const auto it = m_xfbCounterSlotByObject.find(name);
        if (it != m_xfbCounterSlotByObject.end()) {
            return it->second;
        }
        // Past the tracked set every object shares slot group 0. Only concurrently-paused
        // spans need distinct groups, and applications do not keep sixteen of those open.
        const Uint32 slot = m_xfbNextCounterSlot < kXfbCounterObjectSlots ? m_xfbNextCounterSlot++ : 0;
        m_xfbCounterSlotByObject[name] = slot;
        return slot;
    }

    Bool VulkanRenderer::BeginXfbCaptureForDraw(FrameContext::FrameData& frame) {
        if (!m_transformFeedbackFeatureEnabled || MG_State::pGLContext == nullptr ||
            !MG_State::pGLContext->IsTransformFeedbackActive()) {
            return false;
        }
        // A paused span captures nothing, and the counter buffers keep their values, so the
        // next resumed draw appends exactly where the last captured one stopped - which is
        // what pause/resume means (ARB_transform_feedback2).
        if (MG_State::pGLContext->IsTransformFeedbackPaused()) {
            return false;
        }
        const auto& program = MG_State::pGLContext->GetTransformFeedbackProgram();
        if (!program || program->GetTransformFeedbackVaryingCount() == 0) {
            return false;
        }
        const SizeT bufferCount = std::min<SizeT>(program->GetTransformFeedbackBufferCount(), 4);
        if (bufferCount == 0) {
            return false;
        }

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

        VkBuffer buffers[4] = {};
        VkDeviceSize offsets[4] = {};
        VkDeviceSize sizes[4] = {};
        for (SizeT i = 0; i < bufferCount; ++i) {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::TransformFeedback,
                                                                      static_cast<Uint>(i));
            const auto& bufferObject = point.GetBoundObject();
            if (bufferObject == nullptr) {
                return false;
            }
            // Host-visible coherent GPU residency: the capture writes land where
            // MapBuffer/GetBufferSubData read. Coherence makes them visible once they
            // have happened, so the buffer is also flagged for the wait that a later CPU
            // read has to perform - the capture is a GPU write like any shader's.
            bufferObject->EnsureGpuResidentStorage();
            bufferObject->MarkGpuWritten();
            BufferSlice slice{};
            if (!m_bufferManager.AcquireResidentSlice(BufferKind::Vertex, bufferObject, slice)) {
                MGLOG_E_ONCE("BeginXfbCaptureForDraw: failed to acquire capture buffer %zu", i);
                return false;
            }
            const Range1D range = point.GetRange();
            const VkDeviceSize rangeStart = static_cast<VkDeviceSize>(range.start);
            const VkDeviceSize rangeSize = range.end > range.start
                ? static_cast<VkDeviceSize>(range.end - range.start)
                : VK_WHOLE_SIZE;
            buffers[i] = slice.buffer;
            offsets[i] = slice.offset + rangeStart;
            sizes[i] = rangeSize;
        }

        s_vkCmdBindTransformFeedbackBuffersEXT(frame.commandBuffer, 0, static_cast<Uint32>(bufferCount), buffers,
                                               offsets, sizes);

        const Uint32 counterSlot = CurrentXfbCounterSlot();
        const Uint64 generation = MG_State::pGLContext->GetTransformFeedbackGeneration();
        const Bool resume = m_xfbCountersValid[counterSlot] && m_xfbLastSeenGeneration[counterSlot] == generation;
        m_xfbLastSeenGeneration[counterSlot] = generation;

        VkBuffer counterBuffers[4] = {};
        VkDeviceSize counterOffsets[4] = {};
        for (SizeT i = 0; i < bufferCount; ++i) {
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
        const auto& program = MG_State::pGLContext->GetTransformFeedbackProgram();
        const SizeT bufferCount = program ? std::min<SizeT>(program->GetTransformFeedbackBufferCount(), 4) : 0;
        const Uint32 counterSlot = CurrentXfbCounterSlot();
        VkBuffer counterBuffers[4] = {};
        VkDeviceSize counterOffsets[4] = {};
        for (SizeT i = 0; i < bufferCount; ++i) {
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
        BeginXfbQueryForDraw(commandBuffer);
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
        m_xfbQueryActiveSlots[kind].clear();
        m_xfbQueryCaptureActive[kind] = true;
        return true;
    }

    void VulkanRenderer::StopXfbQueryCapture(Uint32 kind, Vector<Uint32>& outSlots) {
        if (kind > 1) {
            return;
        }
        outSlots = Move(m_xfbQueryActiveSlots[kind]);
        m_xfbQueryActiveSlots[kind].clear();
        m_xfbQueryCaptureActive[kind] = false;
    }

    Bool VulkanRenderer::ResolveXfbQueryResult(const Vector<Uint32>& slots, Bool wantGenerated, Uint64& outPrimitives) {
        outPrimitives = 0;
        if (slots.empty() || m_xfbQueryPool == VK_NULL_HANDLE) {
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
            Uint64 pair[2] = {0, 0}; // {primitivesWritten, primitivesNeeded}
            const VkResult result =
                vkGetQueryPoolResults(m_device, m_xfbQueryPool, slot, 1, sizeof(pair), pair, sizeof(pair),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (result == VK_SUCCESS) {
                outPrimitives += pair[wantGenerated ? 1 : 0];
            }
        }
        return true;
    }

    void VulkanRenderer::BeginXfbQueryForDraw(VkCommandBuffer commandBuffer) {
        m_xfbQuerySlotOpen = false;
        if ((!m_xfbQueryCaptureActive[0] && !m_xfbQueryCaptureActive[1]) || m_xfbQueryPool == VK_NULL_HANDLE) {
            return;
        }
        const Uint32 slot = m_xfbQuerySlotCursor;
        m_xfbQuerySlotCursor = (m_xfbQuerySlotCursor + 1) % kXfbQuerySlots;
        // Slots are never host-reset at read time (both GL targets may reference one
        // slot); recycle them here instead.
        s_vkResetQueryPool(m_device, m_xfbQueryPool, slot, 1);
        s_vkCmdBeginQueryIndexedEXT(commandBuffer, m_xfbQueryPool, slot, 0, 0);
        for (Uint32 kind = 0; kind < 2; ++kind) {
            if (m_xfbQueryCaptureActive[kind]) {
                m_xfbQueryActiveSlots[kind].push_back(slot);
            }
        }
        m_xfbQuerySlotOpen = true;
        m_xfbQueryOpenSlot = slot;
    }

    void VulkanRenderer::EndXfbQueryForDraw(VkCommandBuffer commandBuffer) {
        if (!m_xfbQuerySlotOpen) {
            return;
        }
        s_vkCmdEndQueryIndexedEXT(commandBuffer, m_xfbQueryPool, m_xfbQueryOpenSlot, 0);
        m_xfbQuerySlotOpen = false;
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
        BeginXfbQueryForDraw(commandBuffer);
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
                static Vector<VkMultiDrawInfoEXT> infos;
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
                static Vector<VkMultiDrawIndexedInfoEXT> infos;
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
            const RenderStateParameters& rsp = MG_State::pGLContext->GetRenderStateParameters();
            if (rsp.PrimitiveRestartEnabled || rsp.PrimitiveRestartFixedIndexEnabled) {
                mergeGranularity = 0;
            }
        }
        const DrawIndexedCmdParam* pParams = payload.pParams;
        Uint32 drawCount = payload.drawCount;
        static Vector<DrawIndexedCmdParam> mergedParams;
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

    // Byte size of the command structures GL defines for the indirect draws (GL 4.6 core
    // 10.3.10): four uint32 for DrawArraysIndirectCommand, five for DrawElementsIndirect-
    // Command. These bound the read out of GL_DRAW_INDIRECT_BUFFER and are the default
    // stride, so they must be GL's sizes and not this renderer's own draw-parameter
    // structs - DrawCmdParam carries two extra members and is 24 bytes, which made every
    // glDrawArraysIndirect on a tightly-sized indirect buffer look out of range and draw
    // nothing.
    constexpr SizeT kGLDrawArraysIndirectCommandBytes = 4 * sizeof(Uint32);
    constexpr SizeT kGLDrawElementsIndirectCommandBytes = 5 * sizeof(Uint32);

    void VulkanRenderer::MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect,
                                                        GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride) {
        auto& frame = m_frameContext.GetCurrent();

        if (maxdrawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = kGLDrawElementsIndirectCommandBytes;
        }
        if (stride < static_cast<GLsizei>(kGLDrawElementsIndirectCommandBytes)) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, kGLDrawElementsIndirectCommandBytes);
            return;
        }

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: unsupported index type 0x%x", type);
            return;
        }

        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto* indexBuffer = vao.GetIndexBufferBindingSlot().GetBoundObject().get();
        if (!indexBuffer) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: no element array buffer is bound");
            return;
        }

        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset +
            static_cast<SizeT>(stride) * static_cast<SizeT>(maxdrawcount - 1) + kGLDrawElementsIndirectCommandBytes;
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (!drawBuffer || commandBytes > drawBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }

        auto parameterBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
        if (!parameterBuffer || static_cast<SizeT>(drawcount) + sizeof(Uint32) > parameterBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
            return;
        }

        DrawCmdParam vertexRange{};
        vertexRange.vertexCount = static_cast<Uint32>(indexBuffer->GetSize() / indexSize);
        vertexRange.instanceCount = 1;

        IndexBufferView indexBufferView{};
        indexBufferView.indexType = type;
        indexBufferView.indexByteOffset = 0;
        indexBufferView.indexByteSize = indexBuffer->GetSize();

        if (!SetupDraw(frame, mode, DrawSetupAspect::IndexBuffer | DrawSetupAspect::IndirectDrawBuffer,
                       vertexRange, &indexBufferView)) {
            return;
        }

        drawBuffer->SyncPersistentMappedRange();
        parameterBuffer->SyncPersistentMappedRange();

        BufferSlice drawSlice{};
        if (!m_bufferManager.AcquireResidentSlice(BufferKind::Indirect, drawBuffer, drawSlice)) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: failed to sync draw indirect buffer");
            return;
        }
        BufferSlice parameterSlice{};
        if (!m_bufferManager.AcquireResidentSlice(BufferKind::Indirect, parameterBuffer, parameterSlice)) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: failed to sync parameter buffer");
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);
        // vkCmdDrawIndexedIndirectCount with maxDrawCount > 1 additionally requires the
        // multiDrawIndirect device feature; fall back to the CPU readback loop otherwise.
        if (m_drawIndirectCountExtensionEnabled && s_vkCmdDrawIndexedIndirectCount &&
            (m_multiDrawIndirectFeatureEnabled || maxdrawcount == 1)) {
            MGLOG_D("DirectVulkan: glMultiDrawElementsIndirectCountARB(max=%d stride=%d)", maxdrawcount, stride);
            s_vkCmdDrawIndexedIndirectCount(frame.commandBuffer,
                                            drawSlice.buffer,
                                            drawSlice.offset + static_cast<VkDeviceSize>(commandOffset),
                                            parameterSlice.buffer,
                                            parameterSlice.offset + static_cast<VkDeviceSize>(drawcount),
                                            static_cast<Uint32>(maxdrawcount),
                                            static_cast<Uint32>(stride));
            return;
        }

        const Uint8* parameterData = parameterBuffer->MappedData();
        const Uint8* drawData = drawBuffer->MappedData();
        Uint32 actualDrawCount = 0;
        std::memcpy(&actualDrawCount, parameterData + drawcount, sizeof(actualDrawCount));
        actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
        for (Uint32 idraw = 0; idraw < actualDrawCount; ++idraw) {
            DrawIndexedCmdParam cmd{};
            std::memcpy(&cmd, drawData + commandOffset + static_cast<SizeT>(idraw) * stride, sizeof(cmd));
            vkCmdDrawIndexed(frame.commandBuffer, cmd.indexCount, cmd.instanceCount, cmd.firstIndex,
                             cmd.vertexOffset, cmd.firstInstance);
        }
    }

    void VulkanRenderer::MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect,
                                                   GLsizei drawcount, GLsizei stride) {
        auto& frame = m_frameContext.GetCurrent();

        if (drawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = kGLDrawElementsIndirectCommandBytes;
        }
        if (stride < static_cast<GLsizei>(kGLDrawElementsIndirectCommandBytes)) {
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: stride %d is smaller than command size %zu",
                    stride, kGLDrawElementsIndirectCommandBytes);
            return;
        }

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: unsupported index type 0x%x", type);
            return;
        }

        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto* indexBuffer = vao.GetIndexBufferBindingSlot().GetBoundObject().get();
        if (!indexBuffer) {
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: no element array buffer is bound");
            return;
        }

        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset +
            static_cast<SizeT>(stride) * static_cast<SizeT>(drawcount - 1) + kGLDrawElementsIndirectCommandBytes;
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (!drawBuffer || commandBytes > drawBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }

        // The command parameters live on the GPU; the CPU-visible range that any single
        // command may address is the whole element array buffer.
        DrawCmdParam vertexRange{};
        vertexRange.vertexCount = static_cast<Uint32>(indexBuffer->GetSize() / indexSize);
        vertexRange.instanceCount = 1;

        IndexBufferView indexBufferView{};
        indexBufferView.indexType = type;
        indexBufferView.indexByteOffset = 0;
        indexBufferView.indexByteSize = indexBuffer->GetSize();

        if (!SetupDraw(frame, mode, DrawSetupAspect::IndexBuffer | DrawSetupAspect::IndirectDrawBuffer,
                       vertexRange, &indexBufferView)) {
            return;
        }

        BufferSlice drawSlice{};
        if (!m_bufferManager.AcquireResidentSlice(BufferKind::Indirect, drawBuffer, drawSlice)) {
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: failed to sync draw indirect buffer");
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);
        MGLOG_D("DirectVulkan: glMultiDrawElementsIndirect(drawcount=%d stride=%d)", drawcount, stride);
        if (drawcount == 1 ||
            (!m_multiDrawForceUnrollIndirect && m_multiDrawIndirectFeatureEnabled && stride % 4 == 0)) {
            vkCmdDrawIndexedIndirect(frame.commandBuffer,
                                     drawSlice.buffer,
                                     drawSlice.offset + static_cast<VkDeviceSize>(commandOffset),
                                     static_cast<Uint32>(drawcount),
                                     static_cast<Uint32>(stride));
            return;
        }

        // multiDrawIndirect device feature unavailable: one indirect draw per command is
        // valid without it and still consumes the GPU-written parameters.
        for (GLsizei idraw = 0; idraw < drawcount; ++idraw) {
            vkCmdDrawIndexedIndirect(frame.commandBuffer,
                                     drawSlice.buffer,
                                     drawSlice.offset + static_cast<VkDeviceSize>(commandOffset) +
                                         static_cast<VkDeviceSize>(idraw) * static_cast<VkDeviceSize>(stride),
                                     1, 0);
        }
    }

    void VulkanRenderer::MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount,
                                                 GLsizei stride) {
        auto& frame = m_frameContext.GetCurrent();

        if (drawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = kGLDrawArraysIndirectCommandBytes;
        }
        if (stride < static_cast<GLsizei>(kGLDrawArraysIndirectCommandBytes)) {
            MGLOG_E_ONCE("MultiDrawArraysIndirect skipped: stride %d is smaller than command size %zu",
                    stride, kGLDrawArraysIndirectCommandBytes);
            return;
        }

        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset +
            static_cast<SizeT>(stride) * static_cast<SizeT>(drawcount - 1) + kGLDrawArraysIndirectCommandBytes;
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (!drawBuffer || commandBytes > drawBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawArraysIndirect skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }

        // The command parameters live on the GPU, so the vertex range is unknown here;
        // resident vertex buffers are uploaded in full regardless.
        DrawCmdParam vertexRange{};
        vertexRange.vertexCount = 0;
        vertexRange.instanceCount = 1;

        if (!SetupDraw(frame, mode, DrawSetupAspect::IndirectDrawBuffer, vertexRange)) {
            return;
        }

        BufferSlice drawSlice{};
        if (!m_bufferManager.AcquireResidentSlice(BufferKind::Indirect, drawBuffer, drawSlice)) {
            MGLOG_E_ONCE("MultiDrawArraysIndirect skipped: failed to sync draw indirect buffer");
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);
        MGLOG_D("DirectVulkan: glMultiDrawArraysIndirect(drawcount=%d stride=%d)", drawcount, stride);
        if (drawcount == 1 ||
            (!m_multiDrawForceUnrollIndirect && m_multiDrawIndirectFeatureEnabled && stride % 4 == 0)) {
            vkCmdDrawIndirect(frame.commandBuffer,
                              drawSlice.buffer,
                              drawSlice.offset + static_cast<VkDeviceSize>(commandOffset),
                              static_cast<Uint32>(drawcount),
                              static_cast<Uint32>(stride));
            return;
        }

        for (GLsizei idraw = 0; idraw < drawcount; ++idraw) {
            vkCmdDrawIndirect(frame.commandBuffer,
                              drawSlice.buffer,
                              drawSlice.offset + static_cast<VkDeviceSize>(commandOffset) +
                                  static_cast<VkDeviceSize>(idraw) * static_cast<VkDeviceSize>(stride),
                              1, 0);
        }
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
            return true;
        }
        // Every submission is recorded with the frame serial it was made under, so the wait can be
        // narrowed to the first submission at or past the requested serial instead of draining the
        // whole queue. OnSubmitsCompletedUpTo calls NotifyFrameSerialComplete for every record it
        // retires, so the completed-serial floor still advances correctly after one fence wait.
        for (const auto& record : m_inFlightSubmits) {
            if (record.frameSerial < serial || record.fence == VK_NULL_HANDLE) {
                continue;
            }
            if (vkWaitForFences(m_device, 1, &record.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                break; // fall through to the drain below
            }
            OnSubmitsCompletedUpTo(record.submitIndex);
            // Deliberately no NotifyDeviceIdle() here: that claims every submission has retired,
            // which is only true after a real queue drain. Work past this record may still run.
            TryDrainFrameTransients();
            return true;
        }

        // No usable record - fall back to draining the graphics queue. This over-waits (bounded by
        // the in-flight frame count) but never deadlocks.
        const VkResult result = vkQueueWaitIdle(m_graphicsQueue);
        if (result != VK_SUCCESS) {
            MGLOG_E_ONCE("WaitForFrameSerial: vkQueueWaitIdle returned %d", result);
            return false;
        }
        m_bufferManager.NotifyDeviceIdle();
        OnSubmitsCompletedUpTo(m_submitCounter);
        // The queue was just drained; take the free frame-boundary drain when
        // nothing is recorded (present-less timer-query loops). No-op otherwise.
        TryDrainFrameTransients();
        return true;
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
        return frame.isCommandRecording || frame.hasCommandBufferRecorded;
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

    void VulkanRenderer::OnSubmitsCompletedUpTo(Uint64 submitIndex) {
        m_completedSubmitCounter = std::max(m_completedSubmitCounter, submitIndex);
        while (!m_inFlightSubmits.empty() && m_inFlightSubmits.front().submitIndex <= submitIndex) {
            SubmitRecord record = m_inFlightSubmits.front();
            m_inFlightSubmits.erase(m_inFlightSubmits.begin());
            // Frame-serial completion piggybacks on submission completion.
            // NotifyFrameSerialComplete refuses the current (still-recording)
            // serial, so mid-frame flush records do not mark it early.
            m_bufferManager.NotifyFrameSerialComplete(record.frameSerial);
            if (!record.pooledFence || m_device == VK_NULL_HANDLE) {
                continue; // frame-slot fences are reset/destroyed by FrameContext
            }
            if (vkResetFences(m_device, 1, &record.fence) == VK_SUCCESS) {
                m_freeSubmitFences.push_back(record.fence);
            } else {
                vkDestroyFence(m_device, record.fence, nullptr);
            }
        }
        // Mid-frame-flushed command buffers whose submission just completed can
        // be freed now; present-less flush loops have no other reclaim point.
        m_frameContext.FreeRetiredCommandBuffersCompletedUpTo(m_completedSubmitCounter);
    }

    Bool VulkanRenderer::TryDrainFrameTransients() {
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
        if ((m_drainsSinceLastPresent % 8) != 0) {
            return true;
        }
        if (m_textureManager) {
            m_textureManager->BeginFrame(frameIndex);
        }
        m_bufferManager.BeginFrame(frameIndex);
        // The cached conversion slices point into the transient arena the
        // BeginFrame above just rewound; drop them together.
        m_convertedVertexStreams.clear();
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
        if (m_vertexInputStateFactory) {
            m_vertexInputStateFactory->OnFrameBoundary();
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
        const VkResult result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, fence);
        if (result != VK_SUCCESS) {
            MGLOG_E_ONCE("SubmitPendingCommandBuffer: vkQueueSubmit returned %d", result);
            return false;
        }
        frame.imageAvailableSemaphoreConsumed = true;
        frame.hasCommandBufferRecorded = false;
        frame.hasPreCommandBufferRecorded = false;
        RegisterSubmit(fence, pooledFence);
        frame.lastSubmitIndex = m_submitCounter;
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
        if (!frame.isCommandRecording && !frame.hasCommandBufferRecorded) {
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
            if (vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS) {
                OnSubmitsCompletedUpTo(m_submitCounter);
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
        for (const auto& record : m_inFlightSubmits) {
            if (record.submitIndex >= submitIndex) {
                const VkResult result = vkWaitForFences(m_device, 1, &record.fence, VK_TRUE, timeoutNs);
                if (result == VK_SUCCESS) {
                    OnSubmitsCompletedUpTo(record.submitIndex);
                    // The wait already stalled the pipeline; if it happens to
                    // have drained everything (present-less fence loops), take
                    // the free frame-boundary drain. No-op otherwise.
                    TryDrainFrameTransients();
                    return true;
                }
                if (result != VK_TIMEOUT) {
                    MGLOG_E_ONCE("WaitForSubmitIndex: vkWaitForFences returned %d", result);
                }
                return false;
            }
        }
        // No in-flight record at or beyond the index: it was already observed
        // complete via a fence wait on a later submission.
        return true;
    }

    void VulkanRenderer::OnFrameCommandRecordingBegan(VkCommandBuffer commandBuffer) {
        // Dynamic state does not survive a command-buffer boundary.
        ResetDynamicStateShadow();
        InvalidateSetupDrawSnapshots();
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
        return (MG_State::pGLContext != nullptr &&
                MG_State::pGLContext->GetProvokingVertexMode() == ProvokingVertexMode::FirstVertex)
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
                // The dropped recording is never submitted, so once the fence
                // poll shows the pre-suspension submissions complete the frame
                // transients (descriptor sets, transient arenas, deferred
                // releases, conversion caches) can rewind; without this a
                // minimized-window app accumulates them for the whole
                // suspension.
                TryDrainFrameTransients();
                MGLOG_D("Present skipped: no usable swapchain (zero-area window)");
                return;
            }
            m_presentSuspended = false;
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
            g_dynamicStateShadow.graphicsPipelineValid = false;
            InvalidateSetupDrawSnapshots();
        }
        m_vertexInputStateFactory->OnFrameBoundary();
        m_samplerManager->OnFrameBoundary();
        auto& frame = m_frameContext.GetCurrent();
        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        if (activeRenderPass)
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);

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
        VK_VERIFY(vkQueueSubmit(m_graphicsQueue, 1, &submitPacket.submitInfo, frame.imageInFlightFence));
        RegisterSubmit(frame.imageInFlightFence, /*pooledFence=*/false);
        frame.lastSubmitIndex = m_submitCounter;
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
        VK_VERIFY(result, "Present, vkQueuePresentKHR");
        // EGL swap semantics: the presented color buffer's content is undefined the
        // next time this image is acquired (EGL_BUFFER_DESTROYED, the default swap
        // behaviour), and EVERY ancillary depth/stencil buffer's content is
        // undefined after any swap. The render-pass manager turns the undefined
        // attachments' next tile loads into LOAD_OP_DONT_CARE.
        m_swapchainObject.SetImageContentDefined(m_imageIndexAcquired, false);
        m_swapchainObject.SetAllDepthStencilContentUndefined();
        // The authoritative check, done here - after the frame is presented, before the next
        // acquire. This is what makes a launcher-side resolution change take effect: shrinking
        // the window's buffer (SurfaceHolder.setFixedSize) moves currentExtent, the swapchain
        // follows, and the compositor scales the smaller image up to the view for free.
        if (!m_swapchainResizeRequested && SwapchainIsOutOfDate()) {
            m_swapchainResizeRequested = true;
        }
        if (m_swapchainResizeRequested) {
            MGLOG_D("Present, processing requested swapchain resize");
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
        VK_VERIFY(result, "Present, vkAcquireNextImageKHR");
        // The acquired slot's fence has been waited: its last submission
        // (and, in queue order, everything before it) is complete. The frame
        // serials those submissions carried advance the buffer-manager floor
        // inside OnSubmitsCompletedUpTo.
        OnSubmitsCompletedUpTo(m_frameContext.GetCurrent().lastSubmitIndex);
        CollectDeferredDepthMipmapCleanup(m_frameContext.GetCurrentFrameIndex());
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_convertedVertexStreams.clear();
        // Descriptor-set reuse cursors rewind exactly once per frame, here,
        // after the slot's fence wait proved its previous sets GPU-idle. (The
        // per-draw-path lazy rewind missed frames whose recording was opened
        // by a staged buffer copy or timer-query timestamp, leaking a fresh
        // descriptor set per draw for the whole frame; it would also be unsafe
        // after a mid-frame FlushPendingCommands, which does not wait.)
        m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
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
        // MOBILEGL_DISABLE_ROBUST_BUFFER_ACCESS leaves it off to measure or dodge its GPU cost.
        deviceFeatures.robustBufferAccess = MG_Config::Features.DisableRobustBufferAccess
                                                ? VK_FALSE
                                                : supportedDeviceFeatures.robustBufferAccess;
        deviceFeatures.geometryShader = supportedDeviceFeatures.geometryShader;
        deviceFeatures.tessellationShader = supportedDeviceFeatures.tessellationShader;
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
        const Bool descriptorIndexingCore = m_physicalDevice.properties.apiVersion >= VK_API_VERSION_1_2;
        const Bool descriptorIndexingExtension =
            IsExtensionSupported(availableExtensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        auto getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
        if (getPhysicalDeviceProperties2 == nullptr) {
            getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2KHR"));
        }
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
            // MOBILEGL_DISABLE_SUBGROUP escape hatch, so the module lowerings can never
            // disagree with the advertised capabilities.
            const Bool usableSubgroups =
                subgroupProperties.subgroupSize > 0 &&
                (subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
                (subgroupProperties.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
            if (usableSubgroups && !MG_Config::Features.DisableSubgroup) {
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
                                 m_config.MaxFramesInFlight, desiredExtent);
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

    void VulkanRenderer::CreateSurface() {
        if (!m_window) {
#if defined VK_USE_PLATFORM_METAL_EXT
            m_window = reinterpret_cast<NativeWindowType>(
                CreateInternalMetalLayer(m_config.SurfaceWidth, m_config.SurfaceHeight, &m_platformDisplay));
            m_platformLibrary = reinterpret_cast<void*>(m_window);
#elif defined VK_USE_PLATFORM_ANDROID_KHR
            if (m_headlessSurfaceSupported) {
                auto* createHeadlessSurface = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                    vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"));
                MOBILEGL_ASSERT(createHeadlessSurface != nullptr,
                                "VK_EXT_headless_surface is not available for DirectVulkan pbuffer surface");
                VkHeadlessSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
                VK_VERIFY(createHeadlessSurface(m_instance, &sci, nullptr, &m_surface),
                          "vkCreateHeadlessSurfaceEXT failed");
                return;
            }
            // Windowless context on a driver without VK_EXT_headless_surface: give
            // the WSI an AImageReader's ANativeWindow. It is a real, valid producer
            // surface that is attached to no display and whose images this code never
            // acquires, which is exactly the "drawable nobody sees" the Xlib fallback
            // below builds out of an unmapped window. libmediandk is dlopen'd rather
            // than linked so a device without it degrades to the old error instead of
            // failing to load the library at all.
            {
                void* mediaLib = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
                MOBILEGL_ASSERT(mediaLib != nullptr,
                                "VK_EXT_headless_surface is unavailable and libmediandk.so could not be loaded "
                                "for the pbuffer surface fallback");
                using AImageReaderNewFn = int (*)(int32_t, int32_t, int32_t, int32_t, void**);
                using AImageReaderGetWindowFn = int (*)(void*, void**);
                auto* imageReaderNew = reinterpret_cast<AImageReaderNewFn>(dlsym(mediaLib, "AImageReader_new"));
                auto* imageReaderGetWindow =
                    reinterpret_cast<AImageReaderGetWindowFn>(dlsym(mediaLib, "AImageReader_getWindow"));
                MOBILEGL_ASSERT(imageReaderNew != nullptr && imageReaderGetWindow != nullptr,
                                "libmediandk.so is missing AImageReader_new/AImageReader_getWindow");

                constexpr int32_t kAndroidFormatRgba8888 = 0x1; // AIMAGE_FORMAT_RGBA_8888
                const int32_t width = static_cast<int32_t>(std::max<Uint32>(m_config.SurfaceWidth, 1));
                const int32_t height = static_cast<int32_t>(std::max<Uint32>(m_config.SurfaceHeight, 1));
                void* reader = nullptr;
                // maxImages must cover the swapchain's images; the reader never
                // acquires any, so this only sizes its buffer queue.
                const int status = imageReaderNew(width, height, kAndroidFormatRgba8888, 8, &reader);
                MOBILEGL_ASSERT(status == 0 && reader != nullptr,
                                "AImageReader_new failed (%d) for the pbuffer surface fallback", status);
                void* nativeWindow = nullptr;
                const int windowStatus = imageReaderGetWindow(reader, &nativeWindow);
                MOBILEGL_ASSERT(windowStatus == 0 && nativeWindow != nullptr,
                                "AImageReader_getWindow failed (%d) for the pbuffer surface fallback", windowStatus);
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
            return;
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
            return;
#endif
        }
#if defined VK_USE_PLATFORM_ANDROID_KHR
        auto* nativeWindow = static_cast<ANativeWindow*>(m_window);
        if (!nativeWindow) throw RuntimeError("ANativeWindowType is null");

        VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        sci.window = nativeWindow;
        VK_VERIFY(vkCreateAndroidSurfaceKHR(m_instance, &sci, nullptr, &m_surface), "vkCreateAndroidSurfaceKHR failed");
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
        auto* display = xOpenDisplay(displayName);
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
        // Handle cases like minimize on Windows, where swapchain could return a 0x0 extent
        const auto swapchainCapabilities =
            SwapchainObject::GetSwapchainCapabilities(m_physicalDevice.handle, m_surface);
        if (swapchainCapabilities.capabilities.currentExtent.width == 0 ||
            swapchainCapabilities.capabilities.currentExtent.height == 0) {
            return false;
        }

        vkDeviceWaitIdle(m_device);
        OnSubmitsCompletedUpTo(m_submitCounter);

        if (m_timerQueryManager) {
            // The in-progress command buffer is abandoned below (its recording
            // flags are force-cleared), so timestamp writes recorded into it
            // will never execute; resolve or invalidate all pending records now
            // to keep later waits from hanging on never-available queries.
            m_timerQueryManager->InvalidatePendingRecords();
        }

        DestroyDeferredDepthMipmapCleanup();
        m_deferredDepthMipmapCleanup.assign(m_frameContext.GetFrameCount(), {});

        ShutdownSwapchain();

        CreateSwapchain();
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
        g_dynamicStateShadow.graphicsPipelineValid = false;
        InvalidateSetupDrawSnapshots();
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
            m_convertedVertexStreams.clear();
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

    VkInstance VulkanRenderer::GetInstance() const {
        return m_instance;
    }

    Bool VulkanRenderer::IsDrawIndirectCountExtensionEnabled() const {
        return m_drawIndirectCountExtensionEnabled;
    }

    void VulkanRenderer::ClearAttachmentsOnActiveRenderPass(VkCommandBuffer commandBuffer,
                                                            const RenderPassEntry &compatibleRenderPassEntry) {
        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        MOBILEGL_ASSERT(activeRenderPass, "No render pass active");
        VkClearRect clearRect{};
        clearRect.rect.offset = {0, 0};
        clearRect.rect.extent = {
                static_cast<Uint32>(activeRenderPass->extent.x()),
                static_cast<Uint32>(activeRenderPass->extent.y())
        };
        clearRect.baseArrayLayer = 0;
        // Compatible entries share the framebuffer layer count; layered attachments clear every layer.
        clearRect.layerCount = compatibleRenderPassEntry.layers;

        for (const auto& pending : compatibleRenderPassEntry.pendingClearAttachments) {
            if (!pending.hasInlinePayload && pending.key.texture == nullptr) {
                continue;
            }

            ClearAttachmentPayload clearPayload{};
            SharedPtr<MG_State::GLState::ITextureObject> liveTexture;
            if (pending.hasInlinePayload) {
                // The inline payload is baked into the cached RenderPassEntry and outlives
                // its consumption at pass begin (loadOp CLEAR). Replaying it here would
                // wipe every draw already recorded in the pass, so only clear while the
                // renderbuffer's clear is still actually pending, and take the live
                // payload (a newer glClear may carry different values).
                if (!m_renderPassManager->GetPendingRenderbufferClear(pending.renderbuffer, clearPayload)) {
                    continue;
                }
                if ((clearPayload.mask & GL_COLOR_BUFFER_BIT) != 0 && pending.renderbuffer != nullptr &&
                    MG_Util::GetBaseInternalFormatComponentCount(pending.renderbuffer->GetInternalFormat()) == 3) {
                    // RGB renderbuffers are backed by an RGBA image; the missing alpha reads as 1.
                    clearPayload.color = FloatVec4(clearPayload.color.x(), clearPayload.color.y(),
                                                   clearPayload.color.z(), 1.0f);
                }
            } else {
                if (!m_clearManager->GetPendingClear(pending.key, clearPayload, liveTexture)) {
                    continue;
                }
            }

            VkClearAttachment clearAttachment{};
            clearAttachment.clearValue.depthStencil = {1.0f, 0};
            if ((clearPayload.mask & GL_COLOR_BUFFER_BIT) != 0) {
                clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                // VkClearAttachment::colorAttachment indexes the subpass pColorAttachments (draw-buffer
                // slot space, with UNUSED holes), not the compacted attachment descriptions.
                clearAttachment.colorAttachment = pending.colorAttachmentSlot;
                clearAttachment.clearValue.color =
                        MakeVkClearColorValue(clearPayload, ColorFormatLacksAlpha(liveTexture.get()));
            } else {
                if ((clearPayload.mask & GL_DEPTH_BUFFER_BIT) != 0) {
                    clearAttachment.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
                    clearAttachment.clearValue.depthStencil.depth = clearPayload.depth;
                }
                if ((clearPayload.mask & GL_STENCIL_BUFFER_BIT) != 0) {
                    clearAttachment.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                    clearAttachment.clearValue.depthStencil.stencil = clearPayload.stencil;
                }
                if (clearAttachment.aspectMask == 0) {
                    continue;
                }
            }

            vkCmdClearAttachments(commandBuffer, 1, &clearAttachment, 1, &clearRect);
            if (pending.hasInlinePayload) {
                m_renderPassManager->PopPendingRenderbufferClear(pending.renderbuffer);
            } else {
                m_clearManager->PopPendingClear(pending.key);
            }
        }
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
