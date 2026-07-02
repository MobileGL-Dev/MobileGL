// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRenderer.h"
#include "VertexInputStateFactory.h"
#include "VertexInputStateBuilder.h"

#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/ProgramState/ProgramObject.h"
#include "MG_State/GLState/ProgramState/ShaderObject.h"
#include "MG_State/GLState/SamplerState/SamplerObject.h"
#include "MG_State/GLState/TextureState/TextureObject.h"
#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_Util/Converters/GLToMG/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToVk/RenderStateEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Metrics/TextureMetrics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vulkan/vulkan_core.h>

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

    static Float ResolveColorClearAlpha(const MG_State::GLState::ITextureObject* texture, Float requestedAlpha) {
        if (texture != nullptr && MG_Util::GetBaseInternalFormatComponentCount(texture->GetFormat()) == 3) {
            return 1.0f;
        }
        return requestedAlpha;
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

    static void ApplyGLViewportState(VkCommandBuffer commandBuffer,
                                     const IntVec2& framebufferExtent,
                                     VkSurfaceTransformFlagBitsKHR preTransform,
                                     Bool isDefaultFramebuffer) {
        const IntVec4& viewportState = MG_State::pGLContext->GetViewport();
        const FloatVec2& depthRange = MG_State::pGLContext->GetDepthRange();
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

        VkViewport viewport{};
        viewport.x = static_cast<float>(viewportX);
        viewport.y = static_cast<float>(viewportY);
        viewport.width = static_cast<float>(viewportWidth);
        viewport.height = static_cast<float>(viewportHeight);
        viewport.minDepth = depthRange.x();
        viewport.maxDepth = depthRange.y();
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
        vkCmdSetDepthBias(commandBuffer, MG_State::pGLContext->GetPolygonOffsetUnits(), 0.0f,
                          MG_State::pGLContext->GetPolygonOffsetFactor());
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

    static VkRect2D MakeDefaultFramebufferScissorRect(const IntVec4& scissorBox,
                                                      const IntVec2& framebufferExtent,
                                                      VkSurfaceTransformFlagBitsKHR preTransform) {
        if (!IsQuarterTurnPreTransform(preTransform)) {
            return MakeClampedScissorRect(scissorBox, framebufferExtent);
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
        return scissor;
    }

    static void ApplyStencilState(VkCommandBuffer commandBuffer) {
        const StencilFaceState& frontStencil = MG_State::pGLContext->GetStencilState(StencilFace::Front);
        const StencilFaceState& backStencil = MG_State::pGLContext->GetStencilState(StencilFace::Back);

        vkCmdSetStencilCompareMask(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, frontStencil.ValueMask);
        vkCmdSetStencilCompareMask(commandBuffer, VK_STENCIL_FACE_BACK_BIT, backStencil.ValueMask);
        vkCmdSetStencilWriteMask(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, frontStencil.WriteMask);
        vkCmdSetStencilWriteMask(commandBuffer, VK_STENCIL_FACE_BACK_BIT, backStencil.WriteMask);
        vkCmdSetStencilReference(commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                 static_cast<Uint32>(std::max(frontStencil.Ref, 0)));
        vkCmdSetStencilReference(commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                 static_cast<Uint32>(std::max(backStencil.Ref, 0)));
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

    static Uint32 BuildVertexInputAttributeMask(const Vector<VkVertexInputAttributeDescription>& attributes) {
        Uint32 attributeMask = 0;
        for (const auto& attribute : attributes) {
            if (attribute.location < 32) {
                attributeMask |= (1u << attribute.location);
            }
        }
        return attributeMask;
    }

    static Bool ShouldDumpVertexInputStats() {
        static const Bool enabled = [] {
            const char* value = std::getenv("MOBILEGL_VERTEX_INPUT_STATS");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    static const char* PresentDumpPath() {
        const char* value = std::getenv("MOBILEGL_PRESENT_DUMP_PATH");
        return value != nullptr && value[0] != '\0' ? value : nullptr;
    }

    static Bool PresentDumpMatchesTargetCall() {
        const char* target = std::getenv("MOBILEGL_PRESENT_DUMP_CALL");
        if (target == nullptr || target[0] == '\0') {
            return true;
        }
        const char* current = std::getenv("MOBILEGL_PRESENT_CURRENT_CALL");
        return current != nullptr && std::strcmp(target, current) == 0;
    }

    static void DumpVertexInputStats(Uint32 location, const MG_State::GLState::VertexAttribute& attr,
                                     Uint32 firstVertex, Uint32 vertexCount) {
        if (!ShouldDumpVertexInputStats() || attr.Type != DataType::Float32 || attr.Size <= 0 || attr.Size > 4) {
            return;
        }

        const SizeT componentSize = sizeof(float);
        const SizeT elementSize = componentSize * static_cast<SizeT>(attr.Size);
        const SizeT stride = attr.Stride > 0 ? static_cast<SizeT>(attr.Stride) : elementSize;
        const Uint8* base = nullptr;
        SizeT available = 0;
        if (attr.Buffer) {
            const auto& data = attr.Buffer->GetDataReadOnly();
            if (!data || attr.Offset >= data->size()) {
                return;
            }
            base = data->data() + attr.Offset;
            available = data->size() - attr.Offset;
        } else {
            base = reinterpret_cast<const Uint8*>(attr.Offset);
            available = static_cast<SizeT>(firstVertex + vertexCount) * stride;
        }
        if (base == nullptr || vertexCount == 0 || available < elementSize) {
            return;
        }

        float minValues[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float maxValues[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Bool initialized = false;
        SizeT sampled = 0;
        const Uint32 maxSamples = std::min<Uint32>(vertexCount, 256);
        for (Uint32 sample = 0; sample < maxSamples; ++sample) {
            const SizeT offset = static_cast<SizeT>(firstVertex + sample) * stride;
            if (offset + elementSize > available) {
                break;
            }

            const auto* values = reinterpret_cast<const float*>(base + offset);
            for (Int component = 0; component < attr.Size; ++component) {
                if (!initialized) {
                    minValues[component] = values[component];
                    maxValues[component] = values[component];
                } else {
                    minValues[component] = std::min(minValues[component], values[component]);
                    maxValues[component] = std::max(maxValues[component], values[component]);
                }
            }
            initialized = true;
            ++sampled;
        }

        if (!initialized) {
            return;
        }

        std::fprintf(stderr,
                     "MOBILEGL_VERTEX_INPUT_STATS loc=%u buffer=%u size=%d stride=%zu first=%u count=%u sampled=%zu "
                     "min=(%.3f,%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f,%.3f)\n",
                     location,
                     attr.Buffer ? attr.Buffer->GetExternalIndex() : 0u,
                     attr.Size,
                     stride,
                     firstVertex,
                     vertexCount,
                     sampled,
                     minValues[0],
                     minValues[1],
                     minValues[2],
                     minValues[3],
                     maxValues[0],
                     maxValues[1],
                     maxValues[2],
                     maxValues[3]);
    }

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
            const auto trackedTexture = trackedAttachment.texture.lock();
            if (trackedTexture && trackedTexture.get() == &texture) {
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
    outColor = texture(uSource, vTexCoord);
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
                                                         ::MobileGL::TextureUploadTarget uploadTarget,
                                                         Uint32 baseMipLevel) {
            const Uint32 existingMipLevelCount = static_cast<Uint32>(texture.GetMipmapLevelCount());
            if (existingMipLevelCount <= baseMipLevel) {
                return false;
            }

            const IntVec3 baseTexelSize = texture.GetMipmapTexelSize(uploadTarget, baseMipLevel);
            const SizeT baseByteSize = texture.GetMipmapByteSize(uploadTarget, baseMipLevel);
            if (baseTexelSize.x() <= 0 || baseTexelSize.y() <= 0 || baseTexelSize.z() <= 0 || baseByteSize == 0) {
                return false;
            }

            const SizeT baseTexelCount = static_cast<SizeT>(baseTexelSize.x()) * static_cast<SizeT>(baseTexelSize.y()) *
                                         static_cast<SizeT>(baseTexelSize.z());
            if (baseTexelCount == 0 || (baseByteSize % baseTexelCount) != 0) {
                return false;
            }

            const SizeT bytesPerTexel = baseByteSize / baseTexelCount;
            const Uint32 requiredMipLevelCount = baseMipLevel + ComputeFullMipLevelCount(baseTexelSize);
            if (existingMipLevelCount >= requiredMipLevelCount) {
                return true;
            }

            for (Uint32 level = existingMipLevelCount; level < requiredMipLevelCount; ++level) {
                const IntVec3 levelTexelSize = ComputeMipTexelSize(baseTexelSize, level - baseMipLevel);
                const SizeT levelByteSize = bytesPerTexel * static_cast<SizeT>(levelTexelSize.x()) *
                                            static_cast<SizeT>(levelTexelSize.y()) *
                                            static_cast<SizeT>(levelTexelSize.z());
                texture.AllocateStorage(uploadTarget, level, {levelTexelSize, levelByteSize});
                texture.MarkStorageDirty(uploadTarget, level, false);
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
            if (!IsCubeMapFaceUploadTarget(uploadTarget)) {
                return 0;
            }
            return static_cast<Uint32>(uploadTarget) - static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
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
            IntVec2 extent = {0, 0};
            Uint32 mipLevel = 0;
            Uint32 mipLevelCount = 1;
            Uint32 baseArrayLayer = 0;
            Uint32 layerCount = 1;
            const char* label = nullptr;
        };

        static Uint32 ComputeMaxProgramBindings(const VkPhysicalDeviceProperties& properties) {
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
                                            VkTextureManager& textureManager, BlitImageBinding& outBinding) {
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
                    MGLOG_E("BlitFramebuffer skipped: default framebuffer color attachment %d is not supported",
                            static_cast<Int>(attachmentType));
                    return false;
                }
                outBinding.image = swapchainObject.GetImage(swapchainImageIndex);
                outBinding.trackedLayout = nullptr;
                outBinding.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                const auto extent = swapchainObject.GetExtent();
                outBinding.extent = {static_cast<Int>(extent.width), static_cast<Int>(extent.height)};
                outBinding.mipLevel = 0;
                outBinding.mipLevelCount = 1;
                outBinding.baseArrayLayer = 0;
                outBinding.layerCount = 1;
                return true;
            }

            if (attachmentType < FramebufferAttachmentType::Color0 || attachmentType > FramebufferAttachmentType::Color31) {
                MGLOG_E("BlitFramebuffer only supports color attachments right now (attachment=%d)",
                        static_cast<Int>(attachmentType));
                return false;
            }

            const auto& attachment = fbo.GetAttachment(attachmentType);
            if (!attachment.IsComplete()) {
                MGLOG_E("BlitFramebuffer skipped: %s framebuffer color attachment is incomplete",
                        isReadFramebuffer ? "read" : "draw");
                return false;
            }
            if (attachment.IsRenderbuffer()) {
                MGLOG_E("BlitFramebuffer skipped: renderbuffer attachments are not supported yet");
                return false;
            }
            if (!attachment.IsTexture()) {
                MGLOG_E("BlitFramebuffer skipped: unsupported framebuffer attachment type");
                return false;
            }

            auto* texture = attachment.GetTexture().get();
            MOBILEGL_ASSERT(texture != nullptr, "ResolveColorBlitBinding: texture attachment is null");

            auto* resource = textureManager.SyncTextureAndGetDescriptor(*texture);
            if (resource == nullptr) {
                MGLOG_E("BlitFramebuffer skipped: failed to sync %s framebuffer textureId=%d",
                        outBinding.label, texture->GetExternalIndex());
                return false;
            }
            if ((resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
                MGLOG_E("BlitFramebuffer skipped: %s framebuffer attachment textureId=%d is not a color image",
                        outBinding.label, texture->GetExternalIndex());
                return false;
            }

            outBinding.image = resource->image;
            outBinding.trackedLayout = &resource->layout;
            outBinding.aspectMask = resource->aspect;
            const auto attachmentExtent = attachment.GetSize();
            outBinding.extent = {attachmentExtent.x(), attachmentExtent.y()};
            outBinding.mipLevel = static_cast<Uint32>(std::max(attachment.GetTextureLevel(), 0));
            outBinding.mipLevelCount = resource->mipLevels;
            outBinding.baseArrayLayer = ResolveAttachmentBaseArrayLayer(attachment);
            outBinding.layerCount = 1;
            return true;
        }

        static Bool ResolveFramebufferBlitBinding(MG_State::GLState::FramebufferObject& fbo, Bool isReadFramebuffer,
                                                  Uint32 swapchainImageIndex, SwapchainObject& swapchainObject,
                                                  VkTextureManager& textureManager,
                                                  VkImageAspectFlags requiredAspectMask,
                                                  BlitImageBinding& outBinding) {
            const Bool isDefaultFbo = fbo.IsDefaultFramebuffer();
            const auto attachmentType = ResolveFramebufferCopyAttachmentType(fbo, isReadFramebuffer, requiredAspectMask);
            if (attachmentType == FramebufferAttachmentType::None) {
                MGLOG_E("BlitFramebuffer skipped: unsupported aspect mask=0x%x",
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
                    MGLOG_E("BlitFramebuffer skipped: swapchain depth image missing required aspect mask=0x%x",
                            static_cast<Uint32>(requiredAspectMask));
                    return false;
                }

                outBinding.image = swapchainObject.GetDepthStencilImage(swapchainImageIndex);
                outBinding.aspectMask = requiredAspectMask;
                return true;
            }

            const auto& attachment = fbo.GetAttachment(attachmentType);
            if (!attachment.IsComplete()) {
                MGLOG_E("BlitFramebuffer skipped: %s framebuffer attachment is incomplete", outBinding.label);
                return false;
            }
            if (attachment.IsRenderbuffer()) {
                MGLOG_E("BlitFramebuffer skipped: renderbuffer attachments are not supported yet");
                return false;
            }
            if (!attachment.IsTexture()) {
                MGLOG_E("BlitFramebuffer skipped: unsupported framebuffer attachment type");
                return false;
            }

            auto* texture = attachment.GetTexture().get();
            MOBILEGL_ASSERT(texture != nullptr, "ResolveFramebufferBlitBinding: texture attachment is null");
            auto* resource = textureManager.SyncTextureAndGetDescriptor(*texture);
            if (resource == nullptr) {
                MGLOG_E("BlitFramebuffer skipped: failed to sync %s framebuffer textureId=%d",
                        outBinding.label, texture->GetExternalIndex());
                return false;
            }
            if ((resource->aspect & requiredAspectMask) != requiredAspectMask) {
                MGLOG_E("BlitFramebuffer skipped: %s framebuffer attachment textureId=%d is missing aspect mask=0x%x",
                        outBinding.label, texture->GetExternalIndex(), static_cast<Uint32>(requiredAspectMask));
                return false;
            }

            outBinding.image = resource->image;
            outBinding.trackedLayout = &resource->layout;
            outBinding.aspectMask = requiredAspectMask;
            const auto attachmentExtent = attachment.GetSize();
            outBinding.extent = {attachmentExtent.x(), attachmentExtent.y()};
            outBinding.mipLevel = static_cast<Uint32>(std::max(attachment.GetTextureLevel(), 0));
            outBinding.mipLevelCount = resource->mipLevels;
            outBinding.baseArrayLayer = ResolveAttachmentBaseArrayLayer(attachment);
            outBinding.layerCount = 1;
            return true;
        }

        static Bool ResolveTextureCopyDestinationBinding(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel,
                                                         VkTextureManager& textureManager, BlitImageBinding& outBinding) {
            auto* resource = textureManager.SyncTextureAndGetDescriptor(texture);
            if (resource == nullptr) {
                MGLOG_E("CopyTexSubImage2D skipped: failed to sync destination textureId=%d",
                        texture.GetExternalIndex());
                return false;
            }
            const VkImageAspectFlags copyAspectMask =
                resource->aspect & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            if (copyAspectMask == 0) {
                MGLOG_E("CopyTexSubImage2D skipped: destination textureId=%d uses unsupported aspect mask=0x%x",
                        texture.GetExternalIndex());
                return false;
            }
            if (mipLevel >= resource->mipLevels) {
                MGLOG_E("CopyTexSubImage2D skipped: destination textureId=%d mip=%u out of range (mips=%u)",
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
                                                    VkImageAspectFlags requiredAspectMask,
                                                    BlitImageBinding& outBinding) {
            const Bool isDefaultFbo = fbo.IsDefaultFramebuffer();
            const auto attachmentType = ResolveFramebufferCopyAttachmentType(fbo, true, requiredAspectMask);
            if (attachmentType == FramebufferAttachmentType::None) {
                MGLOG_E("CopyTexSubImage2D skipped: unsupported source aspect mask=0x%x",
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
                    MGLOG_E("CopyTexSubImage2D skipped: swapchain depth image missing required aspect mask=0x%x",
                            static_cast<Uint32>(requiredAspectMask));
                    return false;
                }

                outBinding.image = swapchainObject.GetDepthStencilImage(swapchainImageIndex);
                outBinding.aspectMask = requiredAspectMask;
                return true;
            }

            const auto& attachment = fbo.GetAttachment(attachmentType);
            if (!attachment.IsComplete()) {
                MGLOG_E("CopyTexSubImage2D skipped: read framebuffer attachment %d is incomplete",
                        static_cast<Int>(attachmentType));
                return false;
            }
            if (attachment.IsRenderbuffer()) {
                MGLOG_E("CopyTexSubImage2D skipped: renderbuffer read attachments are not supported yet");
                return false;
            }
            if (!attachment.IsTexture()) {
                MGLOG_E("CopyTexSubImage2D skipped: unsupported read framebuffer attachment type");
                return false;
            }

            auto* texture = attachment.GetTexture().get();
            MOBILEGL_ASSERT(texture != nullptr, "ResolveTextureCopySourceBinding: source texture attachment is null");
            auto* resource = textureManager.SyncTextureAndGetDescriptor(*texture);
            if (resource == nullptr) {
                MGLOG_E("CopyTexSubImage2D skipped: failed to sync read framebuffer textureId=%d",
                        texture->GetExternalIndex());
                return false;
            }
            if ((resource->aspect & requiredAspectMask) != requiredAspectMask) {
                MGLOG_E("CopyTexSubImage2D skipped: read framebuffer textureId=%d aspect mask=0x%x does not satisfy requested mask=0x%x",
                        texture->GetExternalIndex(), static_cast<Uint32>(resource->aspect),
                        static_cast<Uint32>(requiredAspectMask));
                return false;
            }

            outBinding.image = resource->image;
            outBinding.trackedLayout = &resource->layout;
            outBinding.aspectMask = requiredAspectMask;
            const auto attachmentExtent = attachment.GetSize();
            outBinding.extent = {attachmentExtent.x(), attachmentExtent.y()};
            outBinding.mipLevel = static_cast<Uint32>(std::max(attachment.GetTextureLevel(), 0));
            outBinding.mipLevelCount = 1;
            outBinding.baseArrayLayer = ResolveAttachmentBaseArrayLayer(attachment);
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

        static Bool PresentStatsEnabled() {
            const char* value = std::getenv("MOBILEGL_PRESENT_STATS");
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        static Bool IsBgraVkFormat(VkFormat format) {
            switch (format) {
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                case VK_FORMAT_B8G8R8A8_USCALED:
                case VK_FORMAT_B8G8R8A8_SSCALED:
                    return true;
                default:
                    return false;
            }
        }

        static VkExtent2D GetPresentedDumpExtent(VkExtent2D rawExtent,
                                                 VkSurfaceTransformFlagBitsKHR preTransform) {
            if (IsQuarterTurnPreTransform(preTransform)) {
                return {rawExtent.height, rawExtent.width};
            }
            return rawExtent;
        }

        static const Uint8* GetPresentedDumpPixel(const Uint8* rawPixels,
                                                  VkExtent2D rawExtent,
                                                  VkSurfaceTransformFlagBitsKHR preTransform,
                                                  Uint32 presentedX,
                                                  Uint32 presentedY) {
            Uint32 rawX = presentedX;
            Uint32 rawY = presentedY;
            switch (preTransform) {
                case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
                    rawX = rawExtent.width - 1 - presentedY;
                    rawY = presentedX;
                    break;
                case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
                    rawX = rawExtent.width - 1 - presentedX;
                    rawY = rawExtent.height - 1 - presentedY;
                    break;
                case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
                    rawX = presentedY;
                    rawY = rawExtent.height - 1 - presentedX;
                    break;
                default:
                    break;
            }
            return rawPixels + (static_cast<SizeT>(rawY) * rawExtent.width + rawX) * 4;
        }

        static Bool WritePresentedDumpPpm(const char* path,
                                          const Uint8* rawPixels,
                                          VkExtent2D rawExtent,
                                          VkFormat rawFormat,
                                          VkSurfaceTransformFlagBitsKHR preTransform) {
            FILE* dump = std::fopen(path, "wb");
            if (dump == nullptr) {
                return false;
            }

            const VkExtent2D presentedExtent = GetPresentedDumpExtent(rawExtent, preTransform);
            const Bool presentIsBgra = IsBgraVkFormat(rawFormat);
            std::fprintf(dump, "P6\n%u %u\n255\n", presentedExtent.width, presentedExtent.height);
            for (Uint32 y = 0; y < presentedExtent.height; ++y) {
                for (Uint32 x = 0; x < presentedExtent.width; ++x) {
                    const Uint8* p = GetPresentedDumpPixel(rawPixels, rawExtent, preTransform, x, y);
                    const Uint8 rgb[3] = {
                        presentIsBgra ? p[2] : p[0],
                        p[1],
                        presentIsBgra ? p[0] : p[2],
                    };
                    std::fwrite(rgb, 1, sizeof(rgb), dump);
                }
            }
            std::fclose(dump);
            return true;
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

        static void StoreReadbackPixel(const Uint8* src, Bool srcIsBgra, GLenum dstFormat, Uint8* dst) {
            const Uint8 r = srcIsBgra ? src[2] : src[0];
            const Uint8 g = src[1];
            const Uint8 b = srcIsBgra ? src[0] : src[2];
            const Uint8 a = src[3];

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

        static Bool PackReadbackToClientOrPbo(const Uint8* srcPixels, VkFormat srcFormat, GLsizei width,
                                              GLsizei height, GLenum format, GLenum type, void* pixels) {
            if (width <= 0 || height <= 0) {
                return true;
            }
            if (type != GL_UNSIGNED_BYTE) {
                MGLOG_E("DirectVulkan readback skipped: unsupported type=0x%x", type);
                return false;
            }

            const Int dstChannels = GetReadbackChannelCount(format);
            if (dstChannels == 0) {
                MGLOG_E("DirectVulkan readback skipped: unsupported format=0x%x", format);
                return false;
            }

            const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
            const SizeT rowPixels = static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : width);
            const SizeT dstRowStride = AlignPixelRow(rowPixels * static_cast<SizeT>(dstChannels),
                                                     packParams.Alignment);
            const SizeT dstOffset = static_cast<SizeT>(std::max(packParams.SkipRows, 0)) * dstRowStride +
                                    static_cast<SizeT>(std::max(packParams.SkipPixels, 0)) *
                                        static_cast<SizeT>(dstChannels);
            const SizeT packedSize = dstOffset +
                                     (static_cast<SizeT>(height - 1) * dstRowStride) +
                                     (static_cast<SizeT>(width) * static_cast<SizeT>(dstChannels));
            Vector<Uint8> packed(packedSize, 0);

            const Bool srcIsBgra = IsBgraVkFormat(srcFormat);
            for (GLsizei row = 0; row < height; ++row) {
                const Uint8* srcRow = srcPixels + static_cast<SizeT>(row) * static_cast<SizeT>(width) * 4;
                Uint8* dstRow = packed.data() + dstOffset + static_cast<SizeT>(row) * dstRowStride;
                for (GLsizei col = 0; col < width; ++col) {
                    StoreReadbackPixel(srcRow + static_cast<SizeT>(col) * 4,
                                       srcIsBgra,
                                       format,
                                       dstRow + static_cast<SizeT>(col) * static_cast<SizeT>(dstChannels));
                }
            }

            const auto& pixelPackBufferObject =
                MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
            if (pixelPackBufferObject) {
                const SizeT pboOffset = reinterpret_cast<SizeT>(pixels);
                if (pboOffset + packed.size() > pixelPackBufferObject->GetSize()) {
                    MGLOG_E("DirectVulkan readback skipped: pixel pack buffer is too small");
                    return false;
                }
                pixelPackBufferObject->UploadSubData({packed.data(), packed.size()}, pboOffset);
                return true;
            }

            if (pixels != nullptr && !packed.empty()) {
                Memcpy(pixels, packed.data(), packed.size());
            }
            return true;
        }
    } // namespace

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
            MGLOG_E("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            MGLOG_W("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            MGLOG_I("Vulkan Debug: [%s] %s", typeToString(messageType), pCallbackData->pMessage);
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
        });
        MOBILEGL_ASSERT(succeeded, "VkBufferManager initialization failed.");
        m_textureManager = MakeUnique<VkTextureManager>();
        MOBILEGL_ASSERT(m_textureManager != nullptr, "VkTextureManager creation failed.");
        succeeded = m_textureManager->Initialize(
            {m_device, m_physicalDevice.handle, m_allocator, m_commandPool, m_graphicsQueue,
             m_frameContext.GetFrameCount()});
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

        const Uint32 maxProgramBindings = ComputeMaxProgramBindings(m_physicalDevice.properties);
        MGLOG_I("DirectVulkan: using %u program descriptor bindings", maxProgramBindings);
        if (IsPowerVRDevice(m_physicalDevice.properties)) {
            m_config.DisablePipelineCache = true;
            MGLOG_W("DirectVulkan: disabling pipeline cache on PowerVR device %s",
                    m_physicalDevice.properties.deviceName);
        }

        RecreateSwapchain();

        m_pipelineFactory = MakeUnique<PipelineFactory>(m_device, m_config);
        MOBILEGL_ASSERT(m_pipelineFactory != nullptr, "PipelineFactory creation failed.");
        m_programFactory = MakeUnique<ProgramFactory>(m_device, m_config, maxProgramBindings);
        MOBILEGL_ASSERT(m_programFactory != nullptr, "ProgramFactory creation failed.");

        m_samplerManager = MakeUnique<VkSamplerManager>();
        MOBILEGL_ASSERT(m_samplerManager != nullptr, "VkSamplerManager creation failed.");
        succeeded = m_samplerManager->Initialize({m_device, &m_config});
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
        m_vertexInputStateFactory = MakeUnique<VertexInputStateFactory>(m_config);
        MOBILEGL_ASSERT(m_vertexInputStateFactory != nullptr, "VertexInputStateFactory creation failed.");

        // Prime the first frame so Render() always targets an acquired swapchain image.
        VkResult acquireResult =
            m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
            MGLOG_D("Initialize, vkAcquireNextImageKHR got %d, recreating swapchain", acquireResult);
            RecreateSwapchain();
            acquireResult =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        }
        VK_VERIFY(acquireResult, "Initialize, WaitAndAcquireNextImage");
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_transientVertexIndexBufferSlicesThisFrame.clear();

        MGLOG_D("VulkanRenderer initialized");
    }

    void VulkanRenderer::Shutdown() {
        VK_VERIFY(vkDeviceWaitIdle(m_device));

        DestroyDeferredDepthMipmapCleanup();
        DestroyComputePipelines();

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
        m_bufferManager.Shutdown();
        m_transientVertexIndexBufferSlicesThisFrame.clear();

        m_frameContext.Destroy(m_device, m_commandPool);

        if (m_uniformManager) {
            m_uniformManager->Shutdown();
            m_uniformManager.reset();
        }
        m_programFactory.reset();

        ShutdownSwapchain();
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

        if (m_surface != VK_NULL_HANDLE) {
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

        if (m_debugMessenger != VK_NULL_HANDLE) {
            DestroyDebugMessenger();
            m_debugMessenger = VK_NULL_HANDLE;
        }

        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
        MGLOG_I("VulkanRenderer shut down completed");
    }

    Bool VulkanRenderer::UploadAndBindVertexBuffers(
        VkCommandBuffer commandBuffer, const MG_State::GLState::VertexArrayObject& vao, const DrawCmdParam& drawParams) {
        auto& vertexInputState = m_vertexInputStateFactory->GetOrCreateVertexInputState(vao);
        const auto& program = *MG_State::pGLContext->GetCurrentProgram();
        const auto transformFlags = GetShaderTransformFlags(m_swapchainObject.GetPreTransform());
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, transformFlags);
        const Uint32 activeAttribMask = programObj.activeVertexInputLocationMask;
        const Uint32 vertexInputAttribMask = BuildVertexInputAttributeMask(vertexInputState.attributes);
        const Uint32 missingAttribMask = activeAttribMask & ~vertexInputAttribMask;

        const auto bindingCount = vertexInputState.bindings.size() + static_cast<SizeT>(std::popcount(missingAttribMask));

        Vector<VkBuffer> vkBuffers(bindingCount, VK_NULL_HANDLE);
        Vector<VkDeviceSize> vkOffsets(bindingCount, 0);

        auto findBufferByKey = [&](SizeT bufferKey) -> const MG_State::GLState::BufferObject* {
            const auto& attrs = vao.GetAllAttributes();
            for (Uint32 location = 0; location < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS; ++location) {
                const auto& attr = attrs[location];
                if (!attr.Buffer) {
                    continue;
                }
                const auto* buffer = attr.Buffer.get();
                if (reinterpret_cast<SizeT>(buffer) == bufferKey) {
                    return buffer;
                }
            }
            return nullptr;
        };

        for (SizeT binding = 0; binding < bindingCount; ++binding) {
            if (binding >= vertexInputState.bindings.size()) {
                break;
            }
            const Uint32 bindingLocation = binding < vertexInputState.bindingAttributeLocations.size()
                                               ? vertexInputState.bindingAttributeLocations[binding]
                                               : static_cast<Uint32>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
            if (bindingLocation < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS) {
                DumpVertexInputStats(bindingLocation, vao.GetAttribute(bindingLocation),
                                     drawParams.firstVertex, drawParams.vertexCount);
            }
            const Bool usesClientMemory = binding < vertexInputState.bindingUsesClientMemory.size() &&
                                          vertexInputState.bindingUsesClientMemory[binding];
            if (usesClientMemory) {
                const Uint32 location = bindingLocation;
                MOBILEGL_ASSERT(location < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS,
                                "UploadAndBindVertexStreams failed to resolve client attribute location");

                const auto& attr = vao.GetAttribute(location);
                const SizeT componentSize = VertexInputStateFactory::GetComponentSize(attr.Type);
                const SizeT elementSize = componentSize * static_cast<SizeT>(attr.Size);
                const SizeT stride = attr.Stride > 0 ? static_cast<SizeT>(attr.Stride) : elementSize;
                const auto* clientData = reinterpret_cast<const Uint8*>(attr.Offset);
                if (!clientData || componentSize == 0 || elementSize == 0 || stride == 0) {
                    MGLOG_E("UploadAndBindVertexStreams skipped: invalid client vertex attribute at location %u", location);
                    return false;
                }

                const Uint32 lastVertex = drawParams.vertexCount > 0
                                              ? drawParams.firstVertex + drawParams.vertexCount - 1
                                              : drawParams.firstVertex;
                const SizeT uploadSize = static_cast<SizeT>(lastVertex) * stride + elementSize;
                BufferSlice slice{};
                if (!m_bufferManager.UploadTransient(BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(),
                                                     clientData, static_cast<VkDeviceSize>(uploadSize), 16, slice)) {
                    MOBILEGL_ASSERT(false,
                                    "UploadAndBindVertexStreams skipped: failed to upload client attribute binding %zu",
                                    binding);
                    return false;
                }

                vkBuffers[binding] = slice.buffer;
                vkOffsets[binding] = slice.offset;
                continue;
            }

            const SizeT bufferKey = vertexInputState.bindingBufferKeys[binding];
            const MG_State::GLState::BufferObject* sourceBuffer = findBufferByKey(bufferKey);
            MOBILEGL_ASSERT(sourceBuffer != nullptr, "UploadAndBindVertexStreams failed to resolve source buffer");
            auto sourceBufferShared = MG_State::pGLContext->GetBufferObject(sourceBuffer->GetExternalIndex());
            MOBILEGL_ASSERT(sourceBufferShared != nullptr,
                            "UploadAndBindVertexStreams failed to resolve shared source buffer");
            sourceBufferShared->MarkPersistentMappedRangeDirty();
            BufferSlice slice{};
            const Bool isDirty = (sourceBufferShared->GetChangeBits() & BufferChangeBits::DirtyBit);
            const Uint64 changeSerial = sourceBufferShared->GetChangeSerial();
            const SizeT sourceSize = sourceBufferShared->GetSize();
            auto cachedTransient = m_transientVertexIndexBufferSlicesThisFrame.find(sourceBufferShared.get());
            if (cachedTransient != m_transientVertexIndexBufferSlicesThisFrame.end() && !isDirty &&
                cachedTransient->second.changeSerial == changeSerial && cachedTransient->second.size == sourceSize) {
                slice = cachedTransient->second.slice;
            } else if (ShouldUseTransientVertexIndexBuffer(*sourceBufferShared) || isDirty) {
                const auto sourceData = sourceBufferShared->GetDataReadOnly();
                if (!m_bufferManager.UploadTransient(BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(),
                                                     sourceData->data(), static_cast<VkDeviceSize>(sourceSize), 16,
                                                     slice)) {
                    MOBILEGL_ASSERT(false, "UploadAndBindVertexStreams skipped: failed to upload transient binding %zu", binding);
                    return false;
                }
                m_transientVertexIndexBufferSlicesThisFrame[sourceBufferShared.get()] = {
                    .slice = slice,
                    .changeSerial = changeSerial,
                    .size = sourceSize,
                };
                m_bufferManager.DowngradeResidentBufferToTransient(sourceBufferShared);
                sourceBufferShared->ClearDirty();
            } else {
                if (!m_bufferManager.SyncResidentBuffer(BufferKind::Vertex, sourceBufferShared, slice)) {
                    MGLOG_E("UploadAndBindVertexStreams skipped: failed to sync resident binding %zu", binding);
                    return false;
                }
            }
            vkBuffers[binding] = slice.buffer;
            const SizeT baseOffset =
                binding < vertexInputState.bindingBaseOffsets.size() ? vertexInputState.bindingBaseOffsets[binding] : 0;
            MOBILEGL_ASSERT(baseOffset <= sourceSize,
                            "UploadAndBindVertexStreams skipped: binding %zu base offset %zu exceeds buffer size %zu",
                            binding, baseOffset, sourceSize);
            vkOffsets[binding] = slice.offset + static_cast<VkDeviceSize>(baseOffset);
        }

        SizeT syntheticBinding = vertexInputState.bindings.size();
        for (Uint32 location = 0; location < 32; ++location) {
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
            MOBILEGL_ASSERT(supported,
                            "DirectVulkan does not support current generic vertex attribute type yet: program=%u location=%u type=0x%x",
                            program.GetExternalIndex(), location, glType);

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
            vkCmdBindVertexBuffers(commandBuffer, 0, static_cast<Uint32>(bindingCount), vkBuffers.data(),
                                   vkOffsets.data());
        }
        return true;
    }

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

        const auto* indexBuffer = vao.GetIndexBufferBindingSlot().GetBoundObject().get();
        MOBILEGL_ASSERT(indexBuffer != nullptr, "UploadAndBindIndexBuffer requires bound EBO");
        const SizeT indexSize = MG_Util::GetGLTypeSize(pIndexBufferView->indexType);
        const SizeT indexDataSizeBytes = pIndexBufferView->indexByteSize;
        MOBILEGL_ASSERT(pIndexBufferView->indexByteOffset + indexDataSizeBytes <= indexBuffer->GetSize(),
                        "DrawElements index range out of bounds");

        BufferSlice slice{};
        auto indexBufferShared = MG_State::pGLContext->GetBufferObject(indexBuffer->GetExternalIndex());
        MOBILEGL_ASSERT(indexBufferShared != nullptr, "UploadAndBindIndexBuffer failed to resolve shared EBO");
        indexBufferShared->MarkPersistentMappedRangeDirty();
        const Bool isDirty = (indexBufferShared->GetChangeBits() & BufferChangeBits::DirtyBit);
        const Uint64 changeSerial = indexBufferShared->GetChangeSerial();
        const SizeT indexBufferSize = indexBufferShared->GetSize();
        auto cachedTransient = m_transientVertexIndexBufferSlicesThisFrame.find(indexBufferShared.get());
        if (cachedTransient != m_transientVertexIndexBufferSlicesThisFrame.end() && !isDirty &&
            cachedTransient->second.changeSerial == changeSerial && cachedTransient->second.size == indexBufferSize) {
            slice = cachedTransient->second.slice;
            vkCmdBindIndexBuffer(frame.commandBuffer,
                                 slice.buffer,
                                 slice.offset + static_cast<VkDeviceSize>(pIndexBufferView->indexByteOffset),
                                 vkIndexType);
            return true;
        }
        if (ShouldUseTransientVertexIndexBuffer(*indexBufferShared) || isDirty) {
            const auto indexData = indexBufferShared->GetDataReadOnly();
            MOBILEGL_ASSERT(indexData != nullptr && !indexData->empty(), "DrawElements requires non-empty EBO data");
            if (!m_bufferManager.UploadTransient(BufferKind::Index, m_frameContext.GetCurrentFrameIndex(),
                                                 indexData->data(),
                                                 static_cast<VkDeviceSize>(indexBufferSize), indexSize,
                                                 slice)) {
                MOBILEGL_ASSERT(false, "DrawElements skipped: failed to prepare transient index buffer");
                return false;
            }
            m_transientVertexIndexBufferSlicesThisFrame[indexBufferShared.get()] = {
                .slice = slice,
                .changeSerial = changeSerial,
                .size = indexBufferSize,
            };
            m_bufferManager.DowngradeResidentBufferToTransient(indexBufferShared);
            indexBufferShared->ClearDirty();
            vkCmdBindIndexBuffer(frame.commandBuffer,
                                 slice.buffer,
                                 slice.offset + static_cast<VkDeviceSize>(pIndexBufferView->indexByteOffset),
                                 vkIndexType);
            return true;
        }
        if (!m_bufferManager.SyncResidentBuffer(BufferKind::Index, indexBufferShared, slice)) {
            MGLOG_E("DrawElements skipped: failed to sync resident index buffer");
            return false;
        }
        vkCmdBindIndexBuffer(frame.commandBuffer, slice.buffer,
                             slice.offset + static_cast<VkDeviceSize>(pIndexBufferView->indexByteOffset), vkIndexType);
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
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .stages = &programObj.stages,
            .vertexInputState = &kEmptyVertexInputState
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
            MOBILEGL_ASSERT(offset + size <= m_depthMipmapResources.program->GetUBOSize(),
                            "GenerateDepthMipmapWithShader: uniform write out of bounds");
            memcpy(depthProgramData + offset, data, size);
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

        auto vertexInputHash = m_vertexInputStateFactory->ComputeHash(vao);
        auto& vis = m_vertexInputStateFactory->GetOrCreateVertexInputState(vao, vertexInputHash);
        const Uint32 vertexInputAttribMask = BuildVertexInputAttributeMask(vis.attributes);
        const Uint32 activeAttribMask = programObj.activeVertexInputLocationMask;
        const Uint32 missingAttribMask = activeAttribMask & ~vertexInputAttribMask;
        Vector<VkVertexInputAttributeDescription> patchedAttributes = vis.attributes;
        Bool hasPatchedVertexAttributes = false;
        for (auto& attribute : patchedAttributes) {
            if (attribute.location >= 32 || (activeAttribMask & (1u << attribute.location)) == 0) {
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

            MGLOG_W("GetOrCreatePipeline: patching vertex input location=%u format=%d -> %d to match shader input type=%u for program=%u",
                    attribute.location,
                    static_cast<Int>(attribute.format),
                    static_cast<Int>(patchedFormat),
                    static_cast<Uint32>(shaderInputType),
                    program.GetExternalIndex());
            attribute.format = patchedFormat;
            hasPatchedVertexAttributes = true;
        }
        VertexInputStateBuilder syntheticVertexInputBuilder;
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
            for (Uint32 location = 0; location < 32; ++location) {
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
            pipelineVertexInputState = &syntheticVertexInputBuilder.Build();
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
        const StencilFaceState& frontStencil = MG_State::pGLContext->GetStencilState(StencilFace::Front);
        const StencilFaceState& backStencil = MG_State::pGLContext->GetStencilState(StencilFace::Back);
        auto mask = MG_State::pGLContext->GetColorMask();
        const auto colorWriteMask = static_cast<VkColorComponentFlags>(
            (mask.r() ? VK_COLOR_COMPONENT_R_BIT : 0u) |
            (mask.g() ? VK_COLOR_COMPONENT_G_BIT : 0u) |
            (mask.b() ? VK_COLOR_COMPONENT_B_BIT : 0u) |
            (mask.a() ? VK_COLOR_COMPONENT_A_BIT : 0u));

        PipelineFactory::PipelineCreatePayload payload {
            .programHash = programObj.hash,
            .vertexInputHash = vertexInputHash,
            .pipelineLayout = programObj.pipelineLayout,
            .renderPass = renderPassEntry.renderPass,
            .colorAttachmentCount = renderPassEntry.colorAttachmentCount,
            .rasterizationSamples = renderPassEntry.sampleCount,
            .subpass = 0,
            .topology = MG_Util::ConvertPrimitiveModeToVkEnum(mode),
            .cullMode = cullFaceEnabled
                ? MG_Util::ConvertCullFaceModeToVkEnum(MG_State::pGLContext->GetCullFaceMode(), invertClockwise)
                : VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
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
            .stages = &programObj.stages,
            .vertexInputState = pipelineVertexInputState
        };
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
        MOBILEGL_ASSERT(
            (fragmentOutputMask >> payload.colorAttachmentCount) == 0,
            "GetOrCreatePipeline: fragmentOutputMask=0x%x exceeds colorAttachmentCount=%u for program=%u",
            fragmentOutputMask,
            payload.colorAttachmentCount,
            program.GetExternalIndex());
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
            VkColorComponentFlags attachmentColorWriteMask = colorWriteMask;
            Bool effectiveBlendEnabled = blendEnabled;
            MG_State::GLState::ITextureObject* colorAttachmentTexture = nullptr;
            if (!isDefaultDrawFbo && i < drawBuffers.size()) {
                const auto drawBuffer = drawBuffers[i];
                colorAttachmentTexture = resolveCompleteColorAttachmentTexture(i);
                if (drawBuffer == FramebufferAttachmentType::None || colorAttachmentTexture == nullptr) {
                    // GL ignores writes and per-target blend state for GL_NONE draw buffer slots.
                    // Depth-only or otherwise unattached draw buffers should also discard color writes.
                    attachmentColorWriteMask = 0;
                    effectiveBlendEnabled = false;
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
                        MGLOG_W(
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
                } else {
                    auto* texture = colorAttachmentTexture;
                    MOBILEGL_ASSERT(texture != nullptr,
                                    "GetOrCreatePipeline: blend is enabled on draw buffer %u but no complete texture attachment is bound",
                                    i);
                    textureExternalIndex = texture->GetExternalIndex();
                    auto* textureResource = m_textureManager->SyncTextureAndGetDescriptor(*texture);
                    MOBILEGL_ASSERT(textureResource != nullptr,
                                    "GetOrCreatePipeline: failed to sync blend color attachment textureId=%d",
                                    texture->GetExternalIndex());
                    colorAttachmentFormat = textureResource->format;
                }

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
                VkFormatProperties formatProperties{};
                vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, colorAttachmentFormat, &formatProperties);
                MOBILEGL_ASSERT(
                    (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0,
                    "GetOrCreatePipeline: blend is enabled on color attachment %u for format=%d textureId=%d, but the format lacks VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT (program=%u)",
                    i,
                    static_cast<Int>(colorAttachmentFormat),
                    textureExternalIndex,
                    program.GetExternalIndex());
#endif
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
        return m_pipelineFactory->GetOrCreatePipeline(payload);
    }

    Bool VulkanRenderer::SetupDraw(FrameContext::FrameData& frame, GLenum mode, Flags<DrawSetupAspect> aspects,
                                   const DrawCmdParam& drawParams,
                                   const IndexBufferView* pIndexBufferView) {
        m_textureManager->CollectGarbage();
        const auto& drawFbo =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        if (drawFbo != nullptr && IsUnsupportedFramebufferForDirectVulkan(*drawFbo)) {
            RecordUnsupportedFramebufferError(__func__);
            return false;
        }
        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto& program = *MG_State::pGLContext->GetCurrentProgram();
        ProgramFactory::CompileOptionFlags transformFlags = GetShaderTransformFlags(m_swapchainObject.GetPreTransform());
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, transformFlags);

        // Begin command recording if not yet
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }

        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();

        // Check if any of the textures to sample have pending clears,
        // which probably indicates it's been gone through codepath like `fbo attach` -> `clear` -> `fbo detach`, and
        // without draws in between to give it a chance to materialize such clear.
        // Deal with this situation here.
        Vector<MG_State::GLState::ITextureObject*> sampledTextures;
        Bool hasSampledTextures = m_uniformManager->CollectSampledTextures(program, programObj, sampledTextures);
        MOBILEGL_ASSERT(hasSampledTextures, "%s: CollectSampledTextures failed", __func__);
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
        for (auto* sampledTexture : sampledTextures) {
            if (!sampledTexture) {
                continue;
            }

            auto* textureResource = m_textureManager->SyncTextureAndGetDescriptor(*sampledTexture);
            MOBILEGL_ASSERT(textureResource != nullptr,
                            "%s: SyncTextureAndGetDescriptor failed for textureId=%d",
                            __func__, sampledTexture->GetExternalIndex());
            MGLOG_D("SetupDraw: sampled textureId=%d layout(before)=%s(%d)",
                    sampledTexture->GetExternalIndex(), VkImageLayoutToString(textureResource->layout),
                    static_cast<Int>(textureResource->layout));
            if (m_clearManager->HasPendingClear(sampledTexture) ||
                !IsValidSampledImageLayout(textureResource->layout)) {
                needSampledTextureTransitions = true;
                break;
            }
        }

        if (activeRenderPass && needSampledTextureTransitions) {
            MGLOG_D("SetupDraw: ending active render pass before sampled texture transitions");
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            activeRenderPass = nullptr;
        }

        for (auto* sampledTexture : sampledTextures) {
            if (!sampledTexture) {
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
            MGLOG_D("SetupDraw: sampled textureId=%d layout(after)=%s(%d)",
                    sampledTexture->GetExternalIndex(), VkImageLayoutToString(transitionedResource->layout),
                    static_cast<Int>(transitionedResource->layout));
        }

        auto* renderPassEntry = &m_renderPassManager->GetOrCreateRenderPass(*drawFbo, m_imageIndexAcquired);
        if (activeRenderPass && !activeRenderPass->CompatibleWith(*renderPassEntry)) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            activeRenderPass = nullptr;
            renderPassEntry = &m_renderPassManager->GetOrCreateRenderPass(*drawFbo, m_imageIndexAcquired);
        }
        if (renderPassEntry->attachmentCount == 0 || renderPassEntry->extent.x() <= 0 || renderPassEntry->extent.y() <= 0) {
            MGLOG_D("SetupDraw skipped: drawFbo=%u resolved to an empty render pass (attachmentCount=%u extent=%dx%d)",
                    drawFbo->GetExternalIndex(),
                    renderPassEntry->attachmentCount,
                    renderPassEntry->extent.x(),
                    renderPassEntry->extent.y());
            return false;
        }

        auto pipeline = GetOrCreatePipeline(mode, program, programObj, transformFlags, vao, *renderPassEntry);
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

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        const Bool boundUniforms = m_uniformManager->BindProgramUniformBuffers(
            frame.commandBuffer, program, programObj, m_frameContext.GetCurrentFrameIndex());
        if (!boundUniforms) {
            MGLOG_E("SetupDraw skipped: BindProgramUniformBuffers failed");
            return false;
        }

        auto vtxUploadOk = UploadAndBindVertexBuffers(frame.commandBuffer, vao, drawParams);
        if (!vtxUploadOk) {
            MGLOG_E("SetupDraw skipped: failed to upload vertex buffers");
            return false;
        }

        if (aspects & DrawSetupAspect::IndexBuffer) {
            auto idxUploadOk = UploadAndBindIndexBuffer(frame, vao, pIndexBufferView);
            MOBILEGL_ASSERT(idxUploadOk, "SetupDraw skipped: failed to upload index buffer");
        }

        ApplyGLViewportState(frame.commandBuffer, renderPassEntry->extent,
                             m_swapchainObject.GetPreTransform(), drawFbo->IsDefaultFramebuffer());
        ApplyBlendConstants(frame.commandBuffer);
        ApplyPolygonOffsetState(frame.commandBuffer);
        ApplyLineWidthState(frame.commandBuffer);
        ApplyStencilState(frame.commandBuffer);

        Bool scissorEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest);
        VkRect2D scissor{};
        if (scissorEnabled) {
            const auto& scissorBox = MG_State::pGLContext->GetScissorBox();
            scissor = drawFbo->IsDefaultFramebuffer()
                ? MakeDefaultFramebufferScissorRect(scissorBox, renderPassEntry->extent,
                                                    m_swapchainObject.GetPreTransform())
                : MakeClampedScissorRect(scissorBox, renderPassEntry->extent);
        } else {
            scissor.offset = {0, 0};
            scissor.extent = { (Uint)renderPassEntry->extent.x(), (Uint)renderPassEntry->extent.y() };
        }
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        return true;
    }

    void VulkanRenderer::DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
        m_textureManager->CollectGarbage();
        auto& frame = m_frameContext.GetCurrent();
        const auto& program = *MG_State::pGLContext->GetCurrentProgram();
        ProgramFactory::CompileOptionFlags transformFlags = 0;
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, transformFlags);

        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }

        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const VkPipeline pipeline = GetOrCreateComputePipeline(programObj);
        if (pipeline == VK_NULL_HANDLE) {
            MGLOG_E("DispatchCompute skipped: compute pipeline creation failed for program=%u",
                    program.GetExternalIndex());
            return;
        }

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const Bool boundUniforms = m_uniformManager->BindProgramUniformBuffers(
            frame.commandBuffer, program, programObj, m_frameContext.GetCurrentFrameIndex(),
            VK_PIPELINE_BIND_POINT_COMPUTE);
        if (!boundUniforms) {
            MGLOG_E("DispatchCompute skipped: BindProgramUniformBuffers failed");
            return;
        }

        MGLOG_D("DirectVulkan: glDispatchCompute(%u, %u, %u)", numGroupsX, numGroupsY, numGroupsZ);
        vkCmdDispatch(frame.commandBuffer, numGroupsX, numGroupsY, numGroupsZ);
    }

    void VulkanRenderer::DispatchComputeIndirect(GLintptr indirect) {
        m_textureManager->CollectGarbage();
        auto& frame = m_frameContext.GetCurrent();
        const auto& program = *MG_State::pGLContext->GetCurrentProgram();
        ProgramFactory::CompileOptionFlags transformFlags = 0;
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, transformFlags);

        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }

        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const VkPipeline pipeline = GetOrCreateComputePipeline(programObj);
        if (pipeline == VK_NULL_HANDLE) {
            MGLOG_E("DispatchComputeIndirect skipped: compute pipeline creation failed for program=%u",
                    program.GetExternalIndex());
            return;
        }

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const Bool boundUniforms = m_uniformManager->BindProgramUniformBuffers(
            frame.commandBuffer, program, programObj, m_frameContext.GetCurrentFrameIndex(),
            VK_PIPELINE_BIND_POINT_COMPUTE);
        if (!boundUniforms) {
            MGLOG_E("DispatchComputeIndirect skipped: BindProgramUniformBuffers failed");
            return;
        }

        auto indirectBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DispatchIndirect).GetBoundObject();
        if (!indirectBuffer) {
            MGLOG_E("DispatchComputeIndirect skipped: GL_DISPATCH_INDIRECT_BUFFER is not bound");
            return;
        }
        indirectBuffer->MarkPersistentMappedRangeDirty();

        BufferSlice slice{};
        if (!m_bufferManager.SyncResidentBuffer(BufferKind::Indirect, indirectBuffer, slice)) {
            MGLOG_E("DispatchComputeIndirect skipped: failed to sync indirect dispatch buffer");
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
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
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

    void VulkanRenderer::Clear(GLbitfield mask) {
        m_clearManager->CollectGarbage();
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
        m_clearManager->QueueClear(mask, payload, *fbo);
        m_renderPassManager->QueueRenderbufferClear(mask, payload, *fbo);
    }

    void VulkanRenderer::QueueClearBufferPayloadForFramebuffer(
            const MG_State::GLState::FramebufferObject& framebuffer, GLenum buffer, GLint drawbuffer,
            const ClearAttachmentPayload& clearPayload) {
        m_clearManager->CollectGarbage();
        if (IsUnsupportedFramebufferForDirectVulkan(framebuffer)) {
            RecordUnsupportedFramebufferError(__func__);
            return;
        }

        auto queueAttachmentClear = [&](FramebufferAttachmentType attachmentType) {
            if (attachmentType == FramebufferAttachmentType::None) {
                return;
            }
            const auto& attachment = framebuffer.GetAttachment(attachmentType);
            if (attachment.IsRenderbuffer()) {
                m_renderPassManager->QueueRenderbufferClear(clearPayload, attachment);
                return;
            }
            if (!attachment.IsTexture()) {
                return;
            }
            m_clearManager->QueueClear(clearPayload, attachment);
        };

        switch (buffer) {
            case GL_COLOR: {
                if (drawbuffer < 0 ||
                    drawbuffer >= static_cast<GLint>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS)) {
                    RecordClearBufferError(__func__, ErrorCode::InvalidValue, "color drawbuffer index is out of range");
                    return;
                }
                queueAttachmentClear(framebuffer.GetDrawBuffers()[drawbuffer]);
                return;
            }
            case GL_DEPTH:
                if (drawbuffer != 0) {
                    RecordClearBufferError(__func__, ErrorCode::InvalidValue, "depth clear requires drawbuffer 0");
                    return;
                }
                queueAttachmentClear(FramebufferAttachmentType::Depth);
                return;
            case GL_STENCIL:
                if (drawbuffer != 0) {
                    RecordClearBufferError(__func__, ErrorCode::InvalidValue, "stencil clear requires drawbuffer 0");
                    return;
                }
                queueAttachmentClear(FramebufferAttachmentType::Stencil);
                return;
            case GL_DEPTH_STENCIL:
                if (drawbuffer != 0) {
                    RecordClearBufferError(__func__, ErrorCode::InvalidValue, "depth/stencil clear requires drawbuffer 0");
                    return;
                }
                queueAttachmentClear(FramebufferAttachmentType::Depth);
                queueAttachmentClear(FramebufferAttachmentType::Stencil);
                return;
            default:
                RecordClearBufferError(__func__, ErrorCode::InvalidEnum, "unsupported clear buffer target");
                return;
        }
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
        payload.depth = depth;
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
                payload.depth = value[0];
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
                payload.depth = value[0];
                break;
            default:
                break;
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
        payload.depth = depth;
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
                payload.color = FloatVec4(static_cast<Float>(value[0]), static_cast<Float>(value[1]),
                                          static_cast<Float>(value[2]), static_cast<Float>(value[3]));
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
                payload.color = FloatVec4(static_cast<Float>(value[0]), static_cast<Float>(value[1]),
                                          static_cast<Float>(value[2]), static_cast<Float>(value[3]));
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

    Bool VulkanRenderer::MaterializePendingClearForTexture(VkCommandBuffer commandBuffer,
                                                           MG_State::GLState::ITextureObject& texture) {
        Vector<PendingClearEntry> pendingClears;
        if (!m_clearManager->GetPendingClears(&texture, pendingClears)) {
            return true;
        }
        MOBILEGL_ASSERT(VkRenderPassManager::GetActiveRenderPass() == nullptr,
                        "MaterializePendingClearForTexture requires no active render pass");

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
            resource->aspect, 0, resource->mipLevels, resource->arrayLayers);
        MOBILEGL_ASSERT(ok,
                        "MaterializePendingClearForTexture: failed to transition textureId=%d to TRANSFER_DST",
                        texture.GetExternalIndex());

        VkImageLayout sampledLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (const auto& pendingClear : pendingClears) {
            MOBILEGL_ASSERT(pendingClear.key.mipLevel < resource->mipLevels,
                            "MaterializePendingClearForTexture: textureId=%d pending clear mip=%u out of range %u",
                            texture.GetExternalIndex(), pendingClear.key.mipLevel, resource->mipLevels);
            MOBILEGL_ASSERT(pendingClear.key.baseArrayLayer + pendingClear.key.layerCount <= resource->arrayLayers,
                            "MaterializePendingClearForTexture: textureId=%d pending clear layer span [%u, %u) exceeds arrayLayers=%u",
                            texture.GetExternalIndex(), pendingClear.key.baseArrayLayer,
                            pendingClear.key.baseArrayLayer + pendingClear.key.layerCount, resource->arrayLayers);

            VkImageSubresourceRange subresourceRange{};
            subresourceRange.baseMipLevel = pendingClear.key.mipLevel;
            subresourceRange.levelCount = 1;
            subresourceRange.baseArrayLayer = pendingClear.key.baseArrayLayer;
            subresourceRange.layerCount = pendingClear.key.layerCount;

            const auto& clearPayload = pendingClear.payload;
            if ((resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
                subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                VkClearColorValue clearValue{};
                clearValue.float32[0] = clearPayload.color.x();
                clearValue.float32[1] = clearPayload.color.y();
                clearValue.float32[2] = clearPayload.color.z();
                clearValue.float32[3] = ResolveColorClearAlpha(&texture, clearPayload.color.w());
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
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, resource->aspect, 0, resource->mipLevels,
            resource->arrayLayers);
        MOBILEGL_ASSERT(ok,
                        "MaterializePendingClearForTexture: failed to transition textureId=%d to sampled layout",
                        texture.GetExternalIndex());
        resource->layout = sampledLayout;

        m_clearManager->PopPendingClear(&texture);
        MGLOG_D("MaterializePendingClearForTexture: textureId=%d pending clear materialized",
                texture.GetExternalIndex());
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
        if (!ResolveColorBlitBinding(readFbo, true, m_imageIndexAcquired, m_swapchainObject, *m_textureManager, srcBinding) ||
            !ResolveColorBlitBinding(drawFbo, false, m_imageIndexAcquired, m_swapchainObject, *m_textureManager, dstBinding)) {
            return false;
        }
        if (srcBinding.trackedLayout == nullptr) {
            MGLOG_E("BlitFramebuffer skipped: shader blit to default framebuffer requires a texture-backed source framebuffer");
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
            MGLOG_E("BlitFramebuffer skipped: failed to transition source textureId=%d for sampling",
                    sourceTexture->GetExternalIndex());
            return false;
        }
        if (m_textureManager->SyncTextureAndGetDescriptor(*sourceTexture) == nullptr) {
            MGLOG_E("BlitFramebuffer skipped: failed to resolve source textureId=%d after sampling transition",
                    sourceTexture->GetExternalIndex());
            return false;
        }
        const VkImageView sourceImageView =
            m_textureManager->GetOrCreateSampledViewAtMipLevel(*sourceTexture, srcBinding.mipLevel);
        MOBILEGL_ASSERT(sourceImageView != VK_NULL_HANDLE,
                        "TryBlitToDefaultFramebufferWithShader: failed to create sampled view for textureId=%d mip=%u",
                        sourceTexture->GetExternalIndex(), srcBinding.mipLevel);

        auto& renderPassEntry = m_renderPassManager->GetOrCreateRenderPass(drawFbo, m_imageIndexAcquired);
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
            MOBILEGL_ASSERT(offset + size <= m_blitResources.program->GetUBOSize(),
                            "TryBlitToDefaultFramebufferWithShader: uniform write out of bounds");
            memcpy(blitProgramData + offset, data, size);
        };
        writeUniform(m_blitResources.srcRectLocation, blitUniformData.srcRect, sizeof(blitUniformData.srcRect));
        writeUniform(m_blitResources.dstRectLocation, blitUniformData.dstRect, sizeof(blitUniformData.dstRect));
        writeUniform(m_blitResources.surfaceTransformLocation, &blitUniformData.surfaceTransform,
                     sizeof(blitUniformData.surfaceTransform));

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
        static constexpr GLbitfield kSupportedBlitMask = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
        if ((mask & ~kSupportedBlitMask) != 0) {
            MGLOG_E("BlitFramebuffer skipped: unsupported mask bits=0x%x", static_cast<Uint32>(mask));
            return;
        }
        const Bool isColorBlit = (mask & GL_COLOR_BUFFER_BIT) != 0;
        const Bool isDepthBlit = (mask & GL_DEPTH_BUFFER_BIT) != 0;
        if (!isColorBlit && !isDepthBlit) {
            return;
        }
        if (isColorBlit && isDepthBlit) {
            MGLOG_E("BlitFramebuffer skipped: combined color+depth blits are not supported yet (mask=0x%x)",
                    static_cast<Uint32>(mask));
            return;
        }
        if (filter != GL_NEAREST && filter != GL_LINEAR) {
            MGLOG_E("BlitFramebuffer skipped: unsupported filter=0x%x", static_cast<Uint32>(filter));
            return;
        }
        if (isDepthBlit && filter != GL_NEAREST) {
            MGLOG_E("BlitFramebuffer skipped: depth blits currently require GL_NEAREST");
            return;
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
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
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
            MGLOG_E("BlitFramebuffer skipped: rotated blit to default framebuffer requires a texture-backed source framebuffer");
            return;
        }

        if (isDepthBlit) {
            BlitImageBinding srcBinding{};
            BlitImageBinding dstBinding{};
            if (!ResolveFramebufferBlitBinding(*readFbo, true, m_imageIndexAcquired, m_swapchainObject,
                                               *m_textureManager, VK_IMAGE_ASPECT_DEPTH_BIT, srcBinding) ||
                !ResolveFramebufferBlitBinding(*drawFbo, false, m_imageIndexAcquired, m_swapchainObject,
                                               *m_textureManager, VK_IMAGE_ASPECT_DEPTH_BIT, dstBinding)) {
                return;
            }

            if (srcX1 < srcX0 || srcY1 < srcY0 || dstX1 < dstX0 || dstY1 < dstY0) {
                MGLOG_E("BlitFramebuffer skipped: depth blits with flipped rectangles are not supported yet");
                return;
            }

            const Int srcWidth = srcX1 - srcX0;
            const Int srcHeight = srcY1 - srcY0;
            const Int dstWidth = dstX1 - dstX0;
            const Int dstHeight = dstY1 - dstY0;
            if (srcWidth <= 0 || srcHeight <= 0 || srcWidth != dstWidth || srcHeight != dstHeight) {
                MGLOG_E("BlitFramebuffer skipped: depth blits currently require matching source and destination extents");
                return;
            }

            if (!readIsDefaultFbo) {
                const auto sourceAttachmentType = ResolveFramebufferCopyAttachmentType(*readFbo, true, srcBinding.aspectMask);
                const auto& sourceAttachment = readFbo->GetAttachment(sourceAttachmentType);
                auto sourceTexture = sourceAttachment.GetTexture();
                MOBILEGL_ASSERT(sourceTexture != nullptr, "BlitFramebuffer: depth source texture attachment is null");
                const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sourceTexture);
                MOBILEGL_ASSERT(clearReady,
                                "BlitFramebuffer: failed to materialize pending clear for depth source textureId=%d",
                                sourceTexture->GetExternalIndex());
            }

            const VkImageLayout srcOriginalLayout = readIsDefaultFbo
                ? m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired)
                : *srcBinding.trackedLayout;
            if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                MGLOG_E("BlitFramebuffer skipped: depth source image layout is undefined");
                return;
            }

            const VkImageLayout dstOriginalLayout = drawIsDefaultFbo
                ? m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired)
                : *dstBinding.trackedLayout;
            const VkImageLayout dstRestoreLayout = dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
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
            return;
        }

        BlitImageBinding srcBinding{};
        BlitImageBinding dstBinding{};
        if (!ResolveColorBlitBinding(*readFbo, true, m_imageIndexAcquired, m_swapchainObject, *m_textureManager, srcBinding) ||
            !ResolveColorBlitBinding(*drawFbo, false, m_imageIndexAcquired, m_swapchainObject, *m_textureManager, dstBinding)) {
            return;
        }

        if (!readIsDefaultFbo) {
            const auto& sourceAttachment = readFbo->GetAttachment(readFbo->GetReadBuffer());
            auto sourceTexture = sourceAttachment.GetTexture();
            MOBILEGL_ASSERT(sourceTexture != nullptr, "BlitFramebuffer: source texture attachment is null");
            const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sourceTexture);
            MOBILEGL_ASSERT(clearReady,
                            "BlitFramebuffer: failed to materialize pending clear for source textureId=%d",
                            sourceTexture->GetExternalIndex());
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
            MGLOG_E("BlitFramebuffer skipped: swapchain source image layout is undefined");
            return;
        }
        if (srcLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            MGLOG_E("BlitFramebuffer skipped: source image layout is undefined");
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
        if (drawIsDefaultFbo) {
            ApplyNativeBlitDefaultFramebufferTransform(m_swapchainObject.GetPreTransform(), dstBinding, blitRegion);
        }

        vkCmdBlitImage(frame.commandBuffer,
                       srcBinding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstBinding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blitRegion, filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

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
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
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
                                             dstBinding.aspectMask, srcBinding)) {
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

    void VulkanRenderer::CopyImageSubData(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                          const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        MOBILEGL_ASSERT(srcWidth > 0 && srcHeight > 0 && srcDepth > 0,
                        "CopyImageSubData requires positive copy dimensions.");
        MOBILEGL_ASSERT(srcTexture != nullptr && dstTexture != nullptr,
                        "CopyImageSubData requires valid source and destination textures.");

        const auto srcTextureTarget = MG_Util::ConvertGLEnumToTextureTarget(srcTarget);
        const auto dstTextureTarget = MG_Util::ConvertGLEnumToTextureTarget(dstTarget);
        MOBILEGL_ASSERT(srcTextureTarget == TextureTarget::Texture2D && dstTextureTarget == TextureTarget::Texture2D,
                        "CopyImageSubData currently only supports GL_TEXTURE_2D sources and destinations.");
        MOBILEGL_ASSERT(srcDepth == 1 && srcZ == 0 && dstZ == 0,
                        "CopyImageSubData currently only supports single-layer 2D copies.");
        MOBILEGL_ASSERT(srcTexture.get() != dstTexture.get(),
                        "CopyImageSubData does not support in-place texture copies yet.");

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }

        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        auto* srcResource = m_textureManager->SyncTextureAndGetDescriptor(*srcTexture);
        auto* dstResource = m_textureManager->SyncTextureAndGetDescriptor(*dstTexture);
        MOBILEGL_ASSERT(srcResource != nullptr && dstResource != nullptr,
                        "CopyImageSubData failed to sync source or destination texture.");
        MOBILEGL_ASSERT(srcLevel >= 0 && dstLevel >= 0 &&
                        static_cast<Uint32>(srcLevel) < srcResource->mipLevels &&
                        static_cast<Uint32>(dstLevel) < dstResource->mipLevels,
                        "CopyImageSubData mip level is out of range.");
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
        MOBILEGL_ASSERT(srcX >= 0 && srcY >= 0 && dstX >= 0 && dstY >= 0 &&
                        static_cast<Uint32>(srcX + srcWidth) <= srcMipWidth &&
                        static_cast<Uint32>(srcY + srcHeight) <= srcMipHeight &&
                        static_cast<Uint32>(dstX + srcWidth) <= dstMipWidth &&
                        static_cast<Uint32>(dstY + srcHeight) <= dstMipHeight,
                        "CopyImageSubData region is outside source or destination bounds.");

        const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *srcTexture);
        MOBILEGL_ASSERT(clearReady, "%s: failed to materialize pending clear for source textureId=%d",
                        __func__, srcTexture->GetExternalIndex());

        const VkImageLayout srcOriginalLayout = srcResource->layout;
        const VkImageLayout dstOriginalLayout = dstResource->layout;
        MOBILEGL_ASSERT(srcOriginalLayout != VK_IMAGE_LAYOUT_UNDEFINED,
                        "CopyImageSubData source image has undefined layout.");
        const VkImageLayout dstRestoreLayout = dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? ((copyAspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            : dstOriginalLayout;

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcOriginalLayout, srcStageMask, srcAccessMask);
        VkImageLayout srcCopyLayout = srcOriginalLayout;
        Bool srcReady = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, srcResource->image, srcCopyLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
            srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, copyAspectMask, srcMipLevel, 1);
        MOBILEGL_ASSERT(srcReady, "%s: failed to transition source image", __func__);

        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionSourceState(dstOriginalLayout, dstStageMask, dstAccessMask);
        VkImageLayout dstCopyLayout = dstOriginalLayout;
        if (dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstResource->image, dstResource->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                dstResource->aspect, 0, dstResource->mipLevels, dstResource->arrayLayers);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition undefined destination image", __func__);
            dstCopyLayout = dstResource->layout;
        } else {
            Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstResource->image, dstCopyLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, copyAspectMask, dstMipLevel, 1);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition destination image", __func__);
        }

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = copyAspectMask;
        copyRegion.srcSubresource.mipLevel = srcMipLevel;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.srcOffset = {srcX, srcY, 0};
        copyRegion.dstSubresource.aspectMask = copyAspectMask;
        copyRegion.dstSubresource.mipLevel = dstMipLevel;
        copyRegion.dstSubresource.baseArrayLayer = 0;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.dstOffset = {dstX, dstY, 0};
        copyRegion.extent = {static_cast<Uint32>(srcWidth), static_cast<Uint32>(srcHeight), 1};
        vkCmdCopyImage(frame.commandBuffer,
                       srcResource->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstResource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copyRegion);

        VkPipelineStageFlags srcRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcRestoreAccessMask = 0;
        GetImageTransitionDestinationState(srcOriginalLayout, srcRestoreStageMask, srcRestoreAccessMask);
        Bool srcRestored = VkTextureManager::TransitionImageLayout(
            frame.commandBuffer, srcResource->image, srcCopyLayout, srcOriginalLayout,
            VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask,
            VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccessMask, copyAspectMask, srcMipLevel, 1);
        MOBILEGL_ASSERT(srcRestored, "%s: failed to restore source image layout", __func__);

        VkPipelineStageFlags dstRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstRestoreAccessMask = 0;
        GetImageTransitionDestinationState(dstRestoreLayout, dstRestoreStageMask, dstRestoreAccessMask);
        if (dstOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool dstRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstResource->image, dstResource->layout, dstRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask,
                dstResource->aspect, 0, dstResource->mipLevels, dstResource->arrayLayers);
            MOBILEGL_ASSERT(dstRestored, "%s: failed to restore undefined destination image layout", __func__);
        } else {
            Bool dstRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstResource->image, dstCopyLayout, dstRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask,
                VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, copyAspectMask, dstMipLevel, 1);
            MOBILEGL_ASSERT(dstRestored, "%s: failed to restore destination image layout", __func__);
        }
    }

    Bool VulkanRenderer::SubmitReadbackCommandsAndWait(FrameContext::FrameData& frame) {
        if (frame.isCommandRecording) {
            m_frameContext.EndCommandRecording();
            frame.hasCommandBufferRecorded = true;
        }
        if (!frame.hasCommandBufferRecorded) {
            return true;
        }

        VkPipelineStageFlags waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkSemaphore waitSemaphore = frame.imageAvailableSemaphore;
        if (!frame.imageAvailableSemaphoreConsumed) {
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &waitSemaphore;
            submitInfo.pWaitDstStageMask = &waitDstStageMask;
        }
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;

        VkResult result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.imageInFlightFence);
        if (result != VK_SUCCESS) {
            MGLOG_E("DirectVulkan readback: vkQueueSubmit returned %d", result);
            return false;
        }
        frame.imageAvailableSemaphoreConsumed = true;

        result = vkWaitForFences(m_device, 1, &frame.imageInFlightFence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            MGLOG_E("DirectVulkan readback: vkWaitForFences returned %d", result);
            return false;
        }
        result = vkResetFences(m_device, 1, &frame.imageInFlightFence);
        if (result != VK_SUCCESS) {
            MGLOG_E("DirectVulkan readback: vkResetFences returned %d", result);
            return false;
        }

        frame.hasCommandBufferRecorded = false;
        frame.isCommandRecording = false;
        return true;
    }

    void VulkanRenderer::ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                    void* pixels) {
        if (width <= 0 || height <= 0) {
            return;
        }

        auto readFbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
        if (readFbo == nullptr) {
            MGLOG_E("DirectVulkan::ReadPixels skipped: no read framebuffer is bound");
            return;
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }

        const Bool readIsDefaultFbo = readFbo->IsDefaultFramebuffer();
        BlitImageBinding srcBinding{};
        if (!ResolveColorBlitBinding(*readFbo, true, m_imageIndexAcquired, m_swapchainObject, *m_textureManager,
                                     srcBinding)) {
            return;
        }
        if (!readIsDefaultFbo) {
            const auto& sourceAttachment = readFbo->GetAttachment(readFbo->GetReadBuffer());
            auto sourceTexture = sourceAttachment.GetTexture();
            if (sourceTexture != nullptr) {
                const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *sourceTexture);
                MOBILEGL_ASSERT(clearReady,
                                "ReadPixels: failed to materialize pending clear for source textureId=%d",
                                sourceTexture->GetExternalIndex());
            }
        }

        const VkImageLayout srcOriginalLayout = readIsDefaultFbo
            ? m_swapchainObject.GetImageLayout(m_imageIndexAcquired)
            : *srcBinding.trackedLayout;
        if (PresentStatsEnabled()) {
            std::fprintf(stderr,
                         "MOBILEGL_READPIXELS_BEGIN defaultFbo=%s x=%d y=%d width=%d height=%d srcLayout=%d recording=%s recorded=%s imageIndex=%u\n",
                         readIsDefaultFbo ? "true" : "false",
                         x, y, width, height,
                         static_cast<Int>(srcOriginalLayout),
                         frame.isCommandRecording ? "true" : "false",
                         frame.hasCommandBufferRecorded ? "true" : "false",
                         m_imageIndexAcquired);
        }
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            MGLOG_E("DirectVulkan::ReadPixels skipped: source image layout is undefined");
            return;
        }

        const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
        VkBufferObject readback;
        if (!readback.Create({
                .allocator = m_allocator,
                .size = readbackSize,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            })) {
            MGLOG_E("DirectVulkan::ReadPixels skipped: failed to create readback buffer");
            return;
        }

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcOriginalLayout, srcStageMask, srcAccessMask);
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
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {x, y, 0};
        copyRegion.imageExtent = {static_cast<Uint32>(width), static_cast<Uint32>(height), 1};
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
            MGLOG_E("DirectVulkan::ReadPixels skipped: failed to map readback buffer");
            return;
        }
        if (PresentStatsEnabled()) {
            const SizeT pixelCount = static_cast<SizeT>(width) * static_cast<SizeT>(height);
            SizeT nonBlack = 0;
            SizeT nonTransparent = 0;
            for (SizeT i = 0; i < pixelCount; ++i) {
                const Uint8* p = mapped + i * 4;
                if (p[0] != 0 || p[1] != 0 || p[2] != 0) {
                    ++nonBlack;
                }
                if (p[3] != 0) {
                    ++nonTransparent;
                }
            }
            std::fprintf(stderr,
                         "MOBILEGL_READPIXELS_STATS nonBlack=%zu/%zu alpha=%zu/%zu\n",
                         nonBlack, pixelCount, nonTransparent, pixelCount);
        }
        const VkFormat srcFormat = readIsDefaultFbo ? m_swapchainObject.GetSurfaceFormat().format : VK_FORMAT_R8G8B8A8_UNORM;
        PackReadbackToClientOrPbo(mapped, srcFormat, width, height, format, type, pixels);
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
            MGLOG_E("DirectVulkan::GetTexImage skipped: level %d is out of range", level);
            return;
        }

        auto* resource = m_textureManager->SyncTextureAndGetDescriptor(*textureObject);
        if (resource == nullptr || resource->image == VK_NULL_HANDLE) {
            MGLOG_E("DirectVulkan::GetTexImage skipped: failed to sync textureId=%u",
                    textureObject->GetExternalIndex());
            return;
        }
        if ((resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
            MGLOG_E("DirectVulkan::GetTexImage skipped: only color textures are supported right now");
            return;
        }

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }
        if (VkRenderPassManager::GetActiveRenderPass() != nullptr) {
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }
        const Bool clearReady = MaterializePendingClearForTexture(frame.commandBuffer, *textureObject);
        MOBILEGL_ASSERT(clearReady,
                        "GetTexImage: failed to materialize pending clear for textureId=%d",
                        textureObject->GetExternalIndex());

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, static_cast<Uint>(level));
        const GLsizei width = texelSize.x();
        const GLsizei height = texelSize.y();
        if (width <= 0 || height <= 0) {
            return;
        }
        if (bufSize >= 0) {
            const Int dstChannels = GetReadbackChannelCount(format);
            if (type == GL_UNSIGNED_BYTE && dstChannels > 0) {
                const SizeT minSize = static_cast<SizeT>(width) * static_cast<SizeT>(height) *
                                      static_cast<SizeT>(dstChannels);
                if (static_cast<SizeT>(bufSize) < minSize) {
                    MGLOG_E("DirectVulkan::GetTextureImage skipped: destination buffer is too small");
                    return;
                }
            }
        }

        const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
        VkBufferObject readback;
        if (!readback.Create({
                .allocator = m_allocator,
                .size = readbackSize,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            })) {
            MGLOG_E("DirectVulkan::GetTexImage skipped: failed to create readback buffer");
            return;
        }

        const VkImageLayout originalLayout = resource->layout;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(originalLayout, srcStageMask, srcAccessMask);
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
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {static_cast<Uint32>(width), static_cast<Uint32>(height), 1};
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
            MGLOG_E("DirectVulkan::GetTextureImage skipped: failed to map readback buffer");
            return;
        }
        PackReadbackToClientOrPbo(mapped, resource->format, width, height, format, type, pixels);
    }

    void VulkanRenderer::GenerateMipmap(GLenum target) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        const auto uploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        MOBILEGL_ASSERT(textureTarget == TextureTarget::Texture2D || textureTarget == TextureTarget::Texture3D,
                        "GenerateMipmap currently only supports GL_TEXTURE_2D and GL_TEXTURE_3D.");

        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto texture = textureUnit.GetBindingSlot(textureTarget).GetBoundObject();
        MOBILEGL_ASSERT(texture != nullptr, "GenerateMipmap requires a bound texture.");
        MOBILEGL_ASSERT(texture->IsComplete(), "GenerateMipmap requires a complete texture.");

        auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(texture.get());
        MOBILEGL_ASSERT(mipmapTexture != nullptr, "GenerateMipmap requires a mipmapped texture object.");

        const Uint32 currentMipLevelCount = static_cast<Uint32>(mipmapTexture->GetMipmapLevelCount());
        MOBILEGL_ASSERT(currentMipLevelCount > 0, "GenerateMipmap requires level 0 storage.");

        const Uint32 baseMipLevel = std::min(static_cast<Uint32>(texture->GetLevelRange().x()), currentMipLevelCount - 1);

        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) {
            m_frameContext.BeginCommandRecording();
            m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
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
            MGLOG_W("GenerateMipmap skipped for textureId=%d because Vulkan format %d does not support blit-based mip generation",
                texture->GetExternalIndex(), static_cast<Int>(resource->format));
            return;
        }
        if (isDepthOrStencilTexture) {
            MOBILEGL_ASSERT((resource->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) == 0,
                            "GenerateMipmap: depth-stencil mipmap generation is not supported yet.");
        }

        const Bool allocatedMipmapStorage =
            EnsureGenerateMipmapStorageAllocated(*mipmapTexture, uploadTarget, baseMipLevel);
        MOBILEGL_ASSERT(allocatedMipmapStorage, "GenerateMipmap could not allocate a full mip chain for this texture.");

        resource = m_textureManager->SyncTextureAndGetDescriptor(*texture);
        MOBILEGL_ASSERT(resource != nullptr && resource->image != VK_NULL_HANDLE,
                "GenerateMipmap failed to resync the backend texture after allocating mip storage.");
        if (resource->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            const VkImageLayout finalLayout = ResolveGenerateMipmapFinalLayout(resource->aspect);
            Bool transitioned = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, resource->image, resource->layout, finalLayout,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, VK_ACCESS_SHADER_READ_BIT, resource->aspect, 0, resource->mipLevels, resource->arrayLayers);
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

        for (Uint32 level = baseMipLevel + 1; level < generateMipLevelCount; ++level) {
            VkImageLayout dstMipLayout = originalLayout;
            Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, resource->image, dstMipLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                originalSrcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                originalSrcAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                resource->aspect, level, 1);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition mip level %u to transfer destination", __func__, level);

            const IntVec3 srcTexelSize = ComputeMipTexelSize(storageBaseTexelSize, level - 1);
            const IntVec3 dstTexelSize = ComputeMipTexelSize(storageBaseTexelSize, level);

            VkImageBlit blitRegion{};
            blitRegion.srcSubresource.aspectMask = resource->aspect;
            blitRegion.srcSubresource.mipLevel = level - 1;
            blitRegion.srcSubresource.baseArrayLayer = 0;
            blitRegion.srcSubresource.layerCount = 1;
            blitRegion.srcOffsets[0] = {0, 0, 0};
            blitRegion.srcOffsets[1] = {srcTexelSize.x(), srcTexelSize.y(), srcTexelSize.z()};
            blitRegion.dstSubresource.aspectMask = resource->aspect;
            blitRegion.dstSubresource.mipLevel = level;
            blitRegion.dstSubresource.baseArrayLayer = 0;
            blitRegion.dstSubresource.layerCount = 1;
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
    }

    void VulkanRenderer::DrawArrays(const DrawCmd& payload) {
        auto& frame = m_frameContext.GetCurrent();

        if (!SetupDraw(frame, payload.mode, 0, payload.params)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        VkCommandBuffer& commandBuffer = frame.commandBuffer;

        vkCmdDraw(commandBuffer,
            payload.params.vertexCount,
            payload.params.instanceCount,
            payload.params.firstVertex,
            payload.params.firstInstance);
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

        if (!SetupDraw(frame, payload.mode, DrawSetupAspect::IndexBuffer, vertexRange,
                       &payload.indexBufferView)) {
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        VkCommandBuffer& commandBuffer = frame.commandBuffer;

        vkCmdDrawIndexed(commandBuffer,
            payload.params.indexCount,
            payload.params.instanceCount,
            payload.params.firstIndex,
            payload.params.vertexOffset,
            payload.params.firstInstance);
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

        VkCommandBuffer& commandBuffer = frame.commandBuffer;

        for (Uint32 idraw = 0; idraw < payload.drawCount; ++idraw) {
            vkCmdDrawIndexed(commandBuffer,
                             payload.pParams[idraw].indexCount,
                             payload.pParams[idraw].instanceCount,
                             payload.pParams[idraw].firstIndex,
                             payload.pParams[idraw].vertexOffset,
                             payload.pParams[idraw].firstInstance);
        }
    }

    void VulkanRenderer::MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect,
                                                        GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride) {
        auto& frame = m_frameContext.GetCurrent();

        if (maxdrawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawIndexedCmdParam);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawIndexedCmdParam))) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawIndexedCmdParam));
            return;
        }

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: unsupported index type 0x%x", type);
            return;
        }

        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto* indexBuffer = vao.GetIndexBufferBindingSlot().GetBoundObject().get();
        if (!indexBuffer) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: no element array buffer is bound");
            return;
        }

        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset +
            static_cast<SizeT>(stride) * static_cast<SizeT>(maxdrawcount - 1) + sizeof(DrawIndexedCmdParam);
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (!drawBuffer || commandBytes > drawBuffer->GetSize()) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }

        auto parameterBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
        if (!parameterBuffer || static_cast<SizeT>(drawcount) + sizeof(Uint32) > parameterBuffer->GetSize()) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
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

        drawBuffer->MarkPersistentMappedRangeDirty();
        parameterBuffer->MarkPersistentMappedRangeDirty();

        BufferSlice drawSlice{};
        if (!m_bufferManager.SyncResidentBuffer(BufferKind::Indirect, drawBuffer, drawSlice)) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: failed to sync draw indirect buffer");
            return;
        }
        BufferSlice parameterSlice{};
        if (!m_bufferManager.SyncResidentBuffer(BufferKind::Indirect, parameterBuffer, parameterSlice)) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: failed to sync parameter buffer");
            return;
        }

        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);
        if (m_drawIndirectCountExtensionEnabled && s_vkCmdDrawIndexedIndirectCount) {
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

        const auto parameterData = parameterBuffer->GetDataReadOnly();
        const auto drawData = drawBuffer->GetDataReadOnly();
        if (!parameterData || !drawData) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: CPU fallback cannot read buffers");
            return;
        }
        Uint32 actualDrawCount = 0;
        std::memcpy(&actualDrawCount, parameterData->data() + drawcount, sizeof(actualDrawCount));
        actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
        for (Uint32 idraw = 0; idraw < actualDrawCount; ++idraw) {
            DrawIndexedCmdParam cmd{};
            std::memcpy(&cmd, drawData->data() + commandOffset + static_cast<SizeT>(idraw) * stride, sizeof(cmd));
            vkCmdDrawIndexed(frame.commandBuffer, cmd.indexCount, cmd.instanceCount, cmd.firstIndex,
                             cmd.vertexOffset, cmd.firstInstance);
        }
    }

    void VulkanRenderer::Present() {
        MOBILEGL_ASSERT(m_imageIndexAcquired < m_swapchainObject.GetImageCount(),
                        "Present, acquired image index out of range");
        auto& frame = m_frameContext.GetCurrent();
        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        if (activeRenderPass)
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);

        VkBufferObject presentStatsReadback;
        VkDeviceSize presentStatsReadbackSize = 0;
        const VkExtent2D presentStatsExtent = m_swapchainObject.GetExtent();
        const char* presentDumpPath = PresentDumpPath();
        const Bool shouldDumpPresent = presentDumpPath != nullptr && PresentDumpMatchesTargetCall();
        const Bool collectPresentStats = (PresentStatsEnabled() || shouldDumpPresent) && frame.isCommandRecording &&
                                         presentStatsExtent.width > 0 && presentStatsExtent.height > 0;
        if (PresentStatsEnabled() && presentDumpPath != nullptr) {
            const char* targetCall = std::getenv("MOBILEGL_PRESENT_DUMP_CALL");
            const char* currentCall = std::getenv("MOBILEGL_PRESENT_CURRENT_CALL");
            std::fprintf(stderr,
                         "MOBILEGL_PRESENT_DUMP_GATE target=%s current=%s shouldDump=%s recording=%s size=%ux%u path=%s\n",
                         targetCall != nullptr ? targetCall : "",
                         currentCall != nullptr ? currentCall : "",
                         shouldDumpPresent ? "true" : "false",
                         frame.isCommandRecording ? "true" : "false",
                         presentStatsExtent.width, presentStatsExtent.height,
                         presentDumpPath);
        }
        if (collectPresentStats) {
            presentStatsReadbackSize = static_cast<VkDeviceSize>(presentStatsExtent.width) *
                                       static_cast<VkDeviceSize>(presentStatsExtent.height) * 4;
            const Bool readbackCreated = presentStatsReadback.Create({
                .allocator = m_allocator,
                .size = presentStatsReadbackSize,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO,
                .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            });
            MOBILEGL_ASSERT(readbackCreated, "Present stats: failed to create readback buffer");

            VkImageLayout statsOriginalLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
            Bool toTransferSrc = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, m_swapchainObject.GetImage(m_imageIndexAcquired),
                statsOriginalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            MOBILEGL_ASSERT(toTransferSrc, "Present stats: failed to transition swapchain for readback");

            VkBufferImageCopy copyRegion{};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = 0;
            copyRegion.bufferImageHeight = 0;
            copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.mipLevel = 0;
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageOffset = {0, 0, 0};
            copyRegion.imageExtent = {presentStatsExtent.width, presentStatsExtent.height, 1};
            vkCmdCopyImageToBuffer(frame.commandBuffer,
                                   m_swapchainObject.GetImage(m_imageIndexAcquired),
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   presentStatsReadback.GetHandle(),
                                   1,
                                   &copyRegion);

            Bool restoreLayout = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, m_swapchainObject.GetImage(m_imageIndexAcquired),
                statsOriginalLayout, m_swapchainObject.GetImageLayout(m_imageIndexAcquired),
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
            MOBILEGL_ASSERT(restoreLayout, "Present stats: failed to restore swapchain layout after readback");
        }
        if (frame.isCommandRecording) {
            m_frameContext.EndCommandRecording();
            frame.hasCommandBufferRecorded = true;
        }

        const auto acquiredImageLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
        const Bool needsLayoutTransitionForPresent =
            m_frameContext.TransitionToPresent(m_swapchainObject.GetImage(m_imageIndexAcquired), acquiredImageLayout);
        const Bool shouldSubmitCommandBuffer = frame.hasCommandBufferRecorded || needsLayoutTransitionForPresent;

        // 1) Submit current frame work.
        auto submitPacket = m_frameContext.GetSubmitInfo(shouldSubmitCommandBuffer, m_imageIndexAcquired);
        VK_VERIFY(vkQueueSubmit(m_graphicsQueue, 1, &submitPacket.submitInfo, frame.imageInFlightFence));
        if (collectPresentStats) {
            VK_VERIFY(vkWaitForFences(m_device, 1, &frame.imageInFlightFence, VK_TRUE, UINT64_MAX),
                      "Present stats, vkWaitForFences");
            const auto* pixels = static_cast<const Uint8*>(presentStatsReadback.Map());
            MOBILEGL_ASSERT(pixels != nullptr, "Present stats: failed to map readback buffer");
            SizeT nonBlack = 0;
            SizeT nonTransparent = 0;
            SizeT colored = 0;
            const SizeT pixelCount = static_cast<SizeT>(presentStatsExtent.width) *
                                     static_cast<SizeT>(presentStatsExtent.height);
            for (SizeT i = 0; i < pixelCount; ++i) {
                const Uint8* p = pixels + i * 4;
                if (p[0] != 0 || p[1] != 0 || p[2] != 0) {
                    ++nonBlack;
                }
                const Uint8 minRgb = std::min(p[0], std::min(p[1], p[2]));
                const Uint8 maxRgb = std::max(p[0], std::max(p[1], p[2]));
                if (maxRgb - minRgb > 24) {
                    ++colored;
                }
                if (p[3] != 0) {
                    ++nonTransparent;
                }
            }
            if (PresentStatsEnabled()) {
                std::fprintf(stderr,
                             "MOBILEGL_PRESENT_STATS nonBlack=%zu/%zu colored=%zu/%zu alpha=%zu/%zu size=%ux%u\n",
                             nonBlack, pixelCount, colored, pixelCount, nonTransparent, pixelCount,
                             presentStatsExtent.width, presentStatsExtent.height);
            }
            if (shouldDumpPresent) {
                const VkFormat presentFormat = m_swapchainObject.GetSurfaceFormat().format;
                if (WritePresentedDumpPpm(presentDumpPath, pixels, presentStatsExtent, presentFormat,
                                          m_swapchainObject.GetPreTransform())) {
                    if (PresentStatsEnabled()) {
                        std::fprintf(stderr, "MOBILEGL_PRESENT_DUMP_WRITTEN path=%s bytes=%zu\n",
                                     presentDumpPath, static_cast<SizeT>(pixelCount * 3));
                    }
                } else if (PresentStatsEnabled()) {
                    std::fprintf(stderr, "MOBILEGL_PRESENT_DUMP_FAILED path=%s\n", presentDumpPath);
                }
            }
        }
        frame.isCommandRecording = false;
        frame.hasCommandBufferRecorded = false;
        m_swapchainObject.SetImageLayout(m_imageIndexAcquired, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // 2) Present current frame.
        auto presentPacket = m_frameContext.GetPresentInfo(m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        auto result = vkQueuePresentKHR(m_presentQueue, &presentPacket.presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            MGLOG_D("Present, vkQueuePresentKHR got %d, recreating swapchain", result);
            RecreateSwapchain();
            m_swapchainResizeRequested = false;
            result = VK_SUCCESS;
        }
        VK_VERIFY(result, "Present, vkQueuePresentKHR");
        if (m_swapchainResizeRequested) {
            MGLOG_D("Present, processing requested swapchain resize");
            RecreateSwapchain();
            m_swapchainResizeRequested = false;
        }

        // 3) Advance frame slot.
        m_frameContext.AdvanceToNext();

        // 4) Wait/reset/acquire for next frame.
        result = m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            MGLOG_D("Present, vkAcquireNextImageKHR got %d, recreating swapchain", result);
            RecreateSwapchain();
            m_swapchainResizeRequested = false;
            result =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        }
        VK_VERIFY(result, "Present, vkAcquireNextImageKHR");
        CollectDeferredDepthMipmapCleanup(m_frameContext.GetCurrentFrameIndex());
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_transientVertexIndexBufferSlicesThisFrame.clear();
    }

    void VulkanRenderer::CreateInstance() {
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

        if (m_validationLayersEnabled) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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
        if (m_validationLayersEnabled) {
            MGLOG_I("Enabling validation layer...");
            instanceInfo.enabledLayerCount = static_cast<uint32_t>(std::size(s_validationLayerNames));
            instanceInfo.ppEnabledLayerNames = s_validationLayerNames;
            instanceInfo.pNext = &debugMessengerCreateInfo;
        } else {
            instanceInfo.enabledLayerCount = 0;
            instanceInfo.pNext = nullptr;
        }

        VK_VERIFY(vkCreateInstance(&instanceInfo, nullptr, &m_instance), "vkCreateInstance failed");

        if (m_validationLayersEnabled) VK_VERIFY(SetupDebugMessenger());
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
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            MGLOG_E("No physical devices supporting Vulkan found.");
        } else {
            MGLOG_I("Found %d physical device(s).", deviceCount);
        }

        MOBILEGL_ASSERT(deviceCount > 0, "No physical devices found.");

        Vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());
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
        deviceFeatures.geometryShader = supportedDeviceFeatures.geometryShader;
        deviceFeatures.independentBlend = supportedDeviceFeatures.independentBlend;
        deviceFeatures.logicOp = supportedDeviceFeatures.logicOp;
        deviceFeatures.shaderClipDistance = supportedDeviceFeatures.shaderClipDistance;
        deviceFeatures.shaderCullDistance = supportedDeviceFeatures.shaderCullDistance;
        deviceFeatures.wideLines = supportedDeviceFeatures.wideLines;
        m_logicOpFeatureEnabled = deviceFeatures.logicOp == VK_TRUE;
        deviceFeatures.shaderInt64 = supportedDeviceFeatures.shaderInt64;
        deviceFeatures.drawIndirectFirstInstance = supportedDeviceFeatures.drawIndirectFirstInstance;
        m_logicOpFeatureEnabled = deviceFeatures.logicOp == VK_TRUE;

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
        MGLOG_I("VK_KHR_draw_indirect_count enabled: %s", m_drawIndirectCountExtensionEnabled ? "true" : "false");

        m_indexTypeUint8ExtensionEnabled = false;
        const char* indexTypeUint8ExtensionName = nullptr;
        if (IsExtensionSupported(availableExtensions, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME)) {
            indexTypeUint8ExtensionName = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME;
        } else if (IsExtensionSupported(availableExtensions, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME)) {
            indexTypeUint8ExtensionName = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME;
        }

        VkPhysicalDeviceIndexTypeUint8Features indexTypeUint8Features{};
        indexTypeUint8Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES;
        if (indexTypeUint8ExtensionName != nullptr) {
            VkPhysicalDeviceFeatures2 featureQuery{};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &indexTypeUint8Features;
            auto getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFeatures2"));
            if (getPhysicalDeviceFeatures2 == nullptr) {
                getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                    vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFeatures2KHR"));
            }
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

        deviceCreateInfo.enabledExtensionCount = static_cast<Uint32>(enabledDeviceExtensions.size());
        deviceCreateInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
        MGLOG_I("Device feature support: geometryShader=%s independentBlend=%s logicOp=%s shaderClipDistance=%s "
                "shaderCullDistance=%s wideLines=%s shaderInt64=%s drawIndirectFirstInstance=%s",
            supportedDeviceFeatures.geometryShader ? "true" : "false",
            supportedDeviceFeatures.independentBlend ? "true" : "false",
            supportedDeviceFeatures.logicOp ? "true" : "false",
            supportedDeviceFeatures.shaderClipDistance ? "true" : "false",
            supportedDeviceFeatures.shaderCullDistance ? "true" : "false",
            supportedDeviceFeatures.wideLines ? "true" : "false",
            supportedDeviceFeatures.shaderInt64 ? "true" : "false",
            supportedDeviceFeatures.drawIndirectFirstInstance ? "true" : "false");
        MGLOG_I("Device feature enabled: geometryShader=%s independentBlend=%s logicOp=%s shaderClipDistance=%s "
                "shaderCullDistance=%s wideLines=%s shaderInt64=%s drawIndirectFirstInstance=%s",
            deviceFeatures.geometryShader ? "true" : "false",
            deviceFeatures.independentBlend ? "true" : "false",
            deviceFeatures.logicOp ? "true" : "false",
            deviceFeatures.shaderClipDistance ? "true" : "false",
            deviceFeatures.shaderCullDistance ? "true" : "false",
            deviceFeatures.wideLines ? "true" : "false",
            deviceFeatures.shaderInt64 ? "true" : "false",
            deviceFeatures.drawIndirectFirstInstance ? "true" : "false");
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
        MGLOG_I("index type uint8 enabled: %s", m_indexTypeUint8ExtensionEnabled ? "true" : "false");
        MGLOG_I("Logical device created.");

        // Queues
        vkGetDeviceQueue(m_device, m_physicalDevice.queueFamilies.graphicsFamily, 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_physicalDevice.queueFamilies.presentFamily, 0, &m_presentQueue);
        MGLOG_I("Queues got successfully.");
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
#else
            auto* createHeadlessSurface =
                reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                    vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"));
            MOBILEGL_ASSERT(createHeadlessSurface != nullptr,
                            "VK_EXT_headless_surface is not available for DirectVulkan pbuffer surface");
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
        sci.hwnd = hwnd;
        VK_VERIFY(vkCreateWin32SurfaceKHR(m_instance, &sci, nullptr, &m_surface), "vkCreateWin32SurfaceKHR failed");
#elif defined VK_USE_PLATFORM_METAL_EXT
        MOBILEGL_ASSERT(m_window, "CAMetalLayer is null");

        VkMetalSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
        sci.pLayer = reinterpret_cast<const void*>(m_window);
        VK_VERIFY(vkCreateMetalSurfaceEXT(m_instance, &sci, nullptr, &m_surface), "vkCreateMetalSurfaceEXT failed");
#elif defined VK_USE_PLATFORM_XLIB_KHR
        MOBILEGL_ASSERT(m_window, "X11 Window is null");

        void* x11Lib = dlopen("libX11.so.6", RTLD_LOCAL | RTLD_NOW);
        if (!x11Lib) {
            x11Lib = dlopen("libX11.so", RTLD_LOCAL | RTLD_NOW);
        }
        MOBILEGL_ASSERT(x11Lib != nullptr, "Failed to open libX11 while creating Vulkan Xlib surface");
        using XOpenDisplayFn = Display* (*)(const char*);
        using XCloseDisplayFn = int (*)(Display*);
        auto* xOpenDisplay = reinterpret_cast<XOpenDisplayFn>(dlsym(x11Lib, "XOpenDisplay"));
        auto* xCloseDisplay = reinterpret_cast<XCloseDisplayFn>(dlsym(x11Lib, "XCloseDisplay"));
        MOBILEGL_ASSERT(xOpenDisplay != nullptr && xCloseDisplay != nullptr,
                        "Failed to resolve XOpenDisplay/XCloseDisplay while creating Vulkan Xlib surface");

        auto* display = xOpenDisplay(std::getenv("DISPLAY"));
        MOBILEGL_ASSERT(display != nullptr, "XOpenDisplay failed while creating Vulkan Xlib surface");
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
        Uint32 extensionCount = 0;
        VK_VERIFY(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
        Vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
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

    void VulkanRenderer::RecreateSwapchain() {
        // Handle cases like minimize on Windows, where swapchain could return a 0x0 extent
        const auto swapchainCapabilities =
            SwapchainObject::GetSwapchainCapabilities(m_physicalDevice.handle, m_surface);
        if (swapchainCapabilities.capabilities.currentExtent.width == 0 ||
            swapchainCapabilities.capabilities.currentExtent.height == 0) {
            return;
        }

        vkDeviceWaitIdle(m_device);

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
        DestroyComputePipelines();
        if (m_frameContext.GetFrameCount() > 0) {
            m_frameContext.GetCurrent().isCommandRecording = false;
            m_frameContext.GetCurrent().hasCommandBufferRecorded = false;
        }
        const Bool okArena = m_bufferManager.RecreateTransientArenas(m_frameContext.GetFrameCount());
        MOBILEGL_ASSERT(okArena, "RecreateSwapchain: buffer manager transient arena initialization failed");
        if (m_frameContext.GetFrameCount() > 0) {
            if (m_textureManager) {
                m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
            }
            m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
            m_transientVertexIndexBufferSlicesThisFrame.clear();
        }
    }

    const PhysicalDevice& VulkanRenderer::GetPhysicalDevice() const {
        return m_physicalDevice;
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
        clearRect.layerCount = 1;

        for (const auto& pending : compatibleRenderPassEntry.pendingClearAttachments) {
            if (!pending.hasInlinePayload && pending.key.texture == nullptr) {
                continue;
            }

            ClearAttachmentPayload clearPayload{};
            SharedPtr<MG_State::GLState::ITextureObject> liveTexture;
            if (pending.hasInlinePayload) {
                clearPayload = pending.inlinePayload;
            } else {
                if (!m_clearManager->GetPendingClear(pending.key, clearPayload, liveTexture)) {
                    continue;
                }
            }

            VkClearAttachment clearAttachment{};
            clearAttachment.clearValue.depthStencil = {1.0f, 0};
            if ((clearPayload.mask & GL_COLOR_BUFFER_BIT) != 0) {
                clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                clearAttachment.colorAttachment = pending.attachmentIndex;
                clearAttachment.clearValue.color = {
                        clearPayload.color.x(),
                        clearPayload.color.y(),
                        clearPayload.color.z(),
                        ResolveColorClearAlpha(liveTexture.get(), clearPayload.color.w())
                };
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
        m_computePipelines.emplace(programObj.hash, pipeline);
        return pipeline;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
