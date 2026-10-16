// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRendererInternal.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "RenderPassGuard.h"
#include "VulkanRenderer.h"
#include "SubmitFencePrefix.h"
#include "MG_Util/X11/DisplayGuard.h"
#include "OffscreenSurfaceRoute.h"

#include "MG_Backend/DirectVulkan/SubgroupSupportPolicy.h"
#include "MG_Backend/ClientReadback.h"
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
#include <MG_Backend/BackendObject.h>
// P7 wave 0: the seam WireFramebuffer.cpp's MagmaWireFatal and WireDraw.cpp's
// WireBufferLegacyFatal die through. Declared by MG_Pipe on purpose - see PipeSessionFail.h.
#include <MG_Pipe/PipeSessionFail.h>
// P5c (T5 / tx): the server's staged-texture shadow GenerateMipmap defines its chain on.
#include <MG_Backend/Record/StagedTextureStore.h>
// P7 wave 2 package B3: rule I's tally for WireDraw.cpp's silent draw drops.
#include "WireDeclineTally.h"
#include "WireColorBlitFilter.h"
#include "WireDepthResolveArm.h"
#include "WireCopySliceProbe.h"
#include "WireDiscardStateProbe.h"
#include "WireDepthResolveProbe.h"
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Remote/Server/ServerLoop.h>
// Shared images: the present side resolves the image it imports (WireSharedImage.cpp).
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
#include <unistd.h> // close(): shared-image sync_file descriptors (WireSharedImage.cpp)
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
        template <typename Fn>
        Fn ObjcMsgSend() {
            return reinterpret_cast<Fn>(objc_msgSend);
        }
    } // namespace
#endif

    static Bool IsQuarterTurnPreTransform(VkSurfaceTransformFlagBitsKHR preTransform) {
        return preTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
               preTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
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
        // The wire buffer manager's store-destroy epoch the vertex/index halves were stamped at:
        // a wire store can be destroyed mid-recording and its VkBuffer handle value re-minted, so
        // an equal handle is only the same buffer while this epoch has not moved.
        Uint64 bindStoreEpoch = 0;
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
        Float depthBiasClamp = 0.0f;
        Bool lineWidthValid = false;
        Float lineWidth = 0.0f;
        Bool stencilValid = false;
        Uint32 stencilFrontCompareMask = 0;
        Uint32 stencilBackCompareMask = 0;
        Uint32 stencilFrontWriteMask = 0;
        Uint32 stencilBackWriteMask = 0;
        Uint32 stencilFrontReference = 0;
        Uint32 stencilBackReference = 0;
        // glShadingRateEXT / glShadingRateCombinerOpsEXT, in RenderStateParameters' packed form
        // (MGPackShadingRate). Only pipelines built with fragmentShadingRateDynamic carry the state.
        Bool fragmentShadingRateValid = false;
        Uint8 fragmentShadingRate = 0;
        Uint8 fragmentShadingRateCombinerOp0 = 0;
        Uint8 fragmentShadingRateCombinerOp1 = 0;
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
        //   ApplyPolygonOffsetState : PolygonOffsetUnits, PolygonOffsetFactor, PolygonOffsetClamp
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
            Float polygonOffsetClamp = 0.0f;
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
                    if (depthRange[i] != other.depthRange[i] || stencilValueMask[i] != other.stencilValueMask[i] ||
                        stencilWriteMask[i] != other.stencilWriteMask[i] || stencilRef[i] != other.stencilRef[i]) {
                        return false;
                    }
                }
                return polygonOffsetFactor == other.polygonOffsetFactor &&
                       polygonOffsetUnits == other.polygonOffsetUnits &&
                       polygonOffsetClamp == other.polygonOffsetClamp && lineWidth == other.lineWidth &&
                       extentX == other.extentX && extentY == other.extentY && preTransform == other.preTransform &&
                       scissorEnabled == other.scissorEnabled && isDefaultFbo == other.isDefaultFbo;
            }
        };
        DynamicTailKey dynamicTailKey{};
    };
    // Per Magma session (MagmaSession.h): it shadows the session's own command buffer. Inline,
    // so every file of the renderer shares the one instance.
    inline SessionLocal<DynamicStateShadow> g_dynamicStateShadow;

    static void ResetDynamicStateShadow() {
        *g_dynamicStateShadow = {};
    }

    // Whether the GL scissor test clips viewport index `index`. The scissor rectangles start
    // all-zero, and all-zero there means "never written" (RenderState's constructor), not an
    // empty box: GL's initial box is the whole window, so until the application's first
    // glScissor an enabled test clips nothing. Read as a box it would clip EVERYTHING, so every
    // reader of the box asks this first. The resolution matches the viewport's (ComputeGLViewport
    // resolves a never-written viewport to the current framebuffer's extent).
    static Bool GLScissorTestClips(Uint32 index = 0) {
        const RenderStateParameters& parameters = MG_Pipe::gPipeInputs->GetRenderStateParameters();
        const Uint32 bit = 1u << index;
        return (parameters.ScissorTestEnabledMask & bit) != 0 && (parameters.ScissorBoxWrittenMask & bit) != 0;
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

    namespace {
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
                outSrcStageMask =
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                outSrcAccessMask =
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
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
    } // namespace

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
                Uint16 word{};
                Memcpy(&word, depth + i * 2, sizeof(word));
                return static_cast<Float>(word) / 65535.0f;
            }
            if (sourceFormat == VK_FORMAT_X8_D24_UNORM_PACK32 || sourceFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
                Uint32 word{};
                Memcpy(&word, depth + i * 4, sizeof(word));
                return static_cast<Float>(word & 0xffffffu) / 16777215.0f;
            }
            Float value{};
            Memcpy(&value, depth + i * sizeof(value), sizeof(value));
            return value;
        };
        SizeT size = 0;
        switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE:
            size = 1;
            break;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
        case GL_HALF_FLOAT:
            size = 2;
            break;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT:
            size = 4;
            break;
        case GL_UNSIGNED_INT_24_8:
            size = 4;
            break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
            size = 8;
            break;
        default:
            return false;
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
                Memcpy(dst, &value, sizeof(value));
                break;
            }
            case GL_HALF_FLOAT: {
                const Uint16 value = MG_Util::EncodeFloatToHalfBits(stencilOnly ? static_cast<Float>(s) : d);
                Memcpy(dst, &value, sizeof(value));
                break;
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
                Memcpy(dst, &value, sizeof(value));
                break;
            }
            case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
                Memcpy(dst, &d, sizeof(d));
                Memcpy(dst + 4, &s, sizeof(s));
                break;
            default: {
                const Bool signedType = type == GL_BYTE || type == GL_SHORT || type == GL_INT;
                const Uint64 maximum =
                    signedType ? ((Uint64{1} << (size * 8 - 1)) - 1) : ((Uint64{1} << (size * 8)) - 1);
                const Uint32 value = stencilOnly ? s
                                                 : static_cast<Uint32>(std::llround(std::clamp<double>(d, 0.0, 1.0) *
                                                                                    static_cast<double>(maximum)));
                Memcpy(dst, &value, size);
                break;
            }
            }
        }
        return true;
    }

    // P7 wave 0. THE MESSAGE IS UNCHANGED; where it goes is not. This funnel is how eleven of
    // the fifteen P7-marked refusals die (the other four are WireDescriptorFatal's), and until now
    // every one of them raised its own abort: no SessionFault frame for the peer, no
    // SessionFaultCount() for exit gate S8, and nothing tying the hand-typed word to
    // FatalFamilies.def. MGPipeSessionFail hands the line to the hook MG_Remote installs at
    // server-role init, whose arm for UnmigratedVerb is SessionFail(MGFatalFamily::
    // UnmigratedVerb, ...) - and with no hook installed it logs and aborts exactly as this did.
    [[noreturn]] static void MagmaWireFatal(const char* detail) {
        MG_Pipe::MGPipeSessionFail(MG_Pipe::MGPipeFatalFamily::UnmigratedVerb,
                                   "MGPipe: Fatal{UnmigratedVerb, \"Magma:%s\"}", detail);
    }

    // AND THE SAME PROBLEM AGAIN FOR THE SHADER MULTISAMPLE RESOLVE (P7 wave 2-B2). lavapipe
    // carries VK_KHR_create_renderpass2 AND VK_KHR_depth_stencil_resolve, so the extension arm
    // is taken for every depth/stencil resolve this host can run and the shader arm - the one
    // that exists for the device WITHOUT them, which is where the session used to die at
    // `Magma:multisample-depth-resolve-capability` - would never execute under any lane. The
    // knob drops the extension arm. It does not pretend the extension is missing anywhere else:
    // nothing but the arm choice reads it. Inline, so the renderer reads the variable once.
    inline Bool MagmaWireForcedShaderDepthResolve() {
        static const Bool forced = [] {
            const char* value = std::getenv("MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE");
            return value && *value && value[0] != '0';
        }();
        return forced;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
