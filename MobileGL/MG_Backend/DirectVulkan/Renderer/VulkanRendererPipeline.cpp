// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRendererPipeline.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRendererInternal.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    static VkPipelineColorBlendAttachmentState MakeColorBlendAttachmentState(
        Bool blendEnable, VkBlendFactor srcColorBlendFactor, VkBlendFactor dstColorBlendFactor, VkBlendOp colorBlendOp,
        VkBlendFactor srcAlphaBlendFactor, VkBlendFactor dstAlphaBlendFactor, VkBlendOp alphaBlendOp,
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

    static DefaultFramebufferRectMapping GetDefaultFramebufferRectMapping(VkSurfaceTransformFlagBitsKHR preTransform) {
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

    static void ShadowedSetScissor(VkCommandBuffer commandBuffer, const VkRect2D& scissor) {
        auto& shadow = *g_dynamicStateShadow;
        if (shadow.scissorValid && shadow.scissor.offset.x == scissor.offset.x &&
            shadow.scissor.offset.y == scissor.offset.y && shadow.scissor.extent.width == scissor.extent.width &&
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
    static VkViewport ComputeGLViewport(Uint32 index, const IntVec2& framebufferExtent,
                                        VkSurfaceTransformFlagBitsKHR preTransform, Bool isDefaultFramebuffer) {
        // Snapped to integers. The viewport is float STATE (glViewportIndexedf may set a
        // fractional origin, and GetFloati_v hands it back verbatim), but what rasterizes here is
        // the rounded rectangle - a deliberate, documented infidelity rather than a spec claim:
        // MobileGL passes the driver's VIEWPORT_SUBPIXEL_BITS through, so it does advertise
        // subpixel viewport precision it does not deliver. Nothing in KHR-GL43.viewport_array or
        // in Minecraft sets a fractional viewport (the conformance checks are all on the state
        // round trip), which is why the honest-but-lossy path was kept over widening every
        // default-framebuffer Y-flip/pre-transform helper to floats. See the KNOWN INFIDELITY
        // note in MG_IntegrationTest/Scenarios/AdvertisedLimitsScenario.cpp.
        const FloatVec4& stored = MG_Pipe::gPipeInputs->GetViewportIndexed(index);
        const IntVec4 viewportState(
            static_cast<Int>(std::lround(stored.x())), static_cast<Int>(std::lround(stored.y())),
            static_cast<Int>(std::lround(stored.z())), static_cast<Int>(std::lround(stored.w())));
        const FloatVec2& depthRange = MG_Pipe::gPipeInputs->GetDepthRangeIndexed(index);
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
            viewportX = MapDefaultFramebufferRectAxis(viewportX, viewportWidth, framebufferExtent.x(), mapping.mirrorX);
            viewportY = MapDefaultFramebufferRectAxis(viewportY, viewportHeight, framebufferExtent.y(), mapping.flipY);
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

    static void ApplyGLViewportState(VkCommandBuffer commandBuffer, const IntVec2& framebufferExtent,
                                     VkSurfaceTransformFlagBitsKHR preTransform, Bool isDefaultFramebuffer) {
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
        const FloatVec4& blendColor = MG_Pipe::gPipeInputs->GetBlendColor();
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

    static void ApplyPolygonOffsetState(VkCommandBuffer commandBuffer, Bool depthBiasClampSupported) {
        const Float constantFactor = MG_Pipe::gPipeInputs->GetPolygonOffsetUnits();
        const Float slopeFactor = MG_Pipe::gPipeInputs->GetPolygonOffsetFactor();
        // GL_POLYGON_OFFSET_CLAMP and depthBiasClamp share their semantics exactly (positive caps
        // the offset from above, negative from below, zero disables). Without the device feature
        // the value must be 0 (VUID-vkCmdSetDepthBias-depthBiasClamp-00790), so the offset then
        // goes through unclamped.
        const Float clamp =
            depthBiasClampSupported ? MG_Pipe::gPipeInputs->GetRenderStateParameters().PolygonOffsetClamp : 0.0f;
        auto& shadow = *g_dynamicStateShadow;
        if (shadow.depthBiasValid && shadow.depthBiasConstantFactor == constantFactor &&
            shadow.depthBiasSlopeFactor == slopeFactor && shadow.depthBiasClamp == clamp) {
            return;
        }
        shadow.depthBiasValid = true;
        shadow.depthBiasConstantFactor = constantFactor;
        shadow.depthBiasSlopeFactor = slopeFactor;
        shadow.depthBiasClamp = clamp;
        vkCmdSetDepthBias(commandBuffer, constantFactor, clamp, slopeFactor);
    }

    static void ApplyLineWidthState(VkCommandBuffer commandBuffer) {
        Float lineWidth = MG_Pipe::gPipeInputs->GetLineWidth();
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

    static VkRect2D MakeDefaultFramebufferScissorRect(const IntVec4& scissorBox, const IntVec2& framebufferExtent,
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
        const StencilFaceState& frontStencil = MG_Pipe::gPipeInputs->GetStencilState(StencilFace::Front);
        const StencilFaceState& backStencil = MG_Pipe::gPipeInputs->GetStencilState(StencilFace::Back);
        const Uint32 frontReference = static_cast<Uint32>(std::max(frontStencil.Ref, 0));
        const Uint32 backReference = static_cast<Uint32>(std::max(backStencil.Ref, 0));

        auto& shadow = *g_dynamicStateShadow;
        if (shadow.stencilValid && shadow.stencilFrontCompareMask == frontStencil.ValueMask &&
            shadow.stencilBackCompareMask == backStencil.ValueMask &&
            shadow.stencilFrontWriteMask == frontStencil.WriteMask &&
            shadow.stencilBackWriteMask == backStencil.WriteMask && shadow.stencilFrontReference == frontReference &&
            shadow.stencilBackReference == backReference) {
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

    static Bool TryCoerceVertexFormatNumericDomain(VkFormat sourceFormat, NumericDomain targetDomain,
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
            outFormat =
                targetDomain == NumericDomain::Uint ? VK_FORMAT_R16G16B16A16_UINT : VK_FORMAT_R16G16B16A16_SSCALED;
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
            outFormat =
                targetDomain == NumericDomain::Sint ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_USCALED;
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
        if (!MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::Multisample)) return kFullCoverage;
        if (!MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::SampleMask)) return kFullCoverage;
        return MG_Pipe::gPipeInputs->GetRenderStateParameters().SampleMaskValue;
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
        return MG_Pipe::MGPipeComputePipelineSubsetHash(MG_Pipe::gPipeInputs->GetRenderStateParameters());
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
        const RenderStateParameters& rsp = MG_Pipe::gPipeInputs->GetRenderStateParameters();
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
                                                            const ProgramFactory::VkProgramObject& programObj,
                                                            ProgramFactory::CompileOptionFlags transformFlags,
                                                            const VertexInputStateFactory::BackendVertexInputState& vis,
                                                            const RenderPassEntry& renderPassEntry,
                                                            Bool primitiveRestartEnable, Bool layeredFramebuffer,
                                                            Uint64 wireRenderPassCompatibilityId) {
        Bool invertClockwise = transformFlags & ProgramFactory::CompileOptionBit::PositionYFlip;
        const Bool nonLayeredStages = !layeredFramebuffer && !programObj.nonLayeredStages.empty();
        // GL 4.6 core 17.3.3: the shader's gl_SampleMask is a multisample fragment operation, a
        // no-op on a single-sample framebuffer or with MULTISAMPLE disabled - the same gate
        // ResolveEffectiveSampleMask applies to the fixed-function mask.
        const Bool fullCoverageSampleMask = programObj.fullCoverageFragmentModule != VK_NULL_HANDLE &&
                                            (renderPassEntry.sampleCount == VK_SAMPLE_COUNT_1_BIT ||
                                             !MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::Multisample));
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
        const Uint64 renderPassHash = wireRenderPassCompatibilityId != 0 ? 0 : renderPassEntry.hash;
        // The pipeline-relevant subset only: glViewport / glScissor / glBlendColor / glStencilMask
        // and friends are dynamic state or not pipeline state at all, and keying the memo on the
        // all-state counter made any of them evict a perfectly good VkPipeline. The memo compares
        // the VALUE hash of that subset, never the version itself: the version is monotonic, so
        // per-draw state flips (GL_BLEND toggles) would otherwise miss entries the memo holds.
        // The version only guards recomputing the hash - unchanged version, unchanged bytes.
        const Uint renderStateVersion = MG_Pipe::gPipeInputs->GetPipelineStateVersion();
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
                : ResolveFallbackPipelineStateHash(renderStateVersion, renderPassEntry.colorAttachmentCount,
                                                   renderPassEntry.sampleCount);
        for (Uint32 i = 0; i < m_pipelineMemoCount; ++i) {
            const PipelineMemoEntry& entry = m_pipelineMemo[i];
            if (entry.pipeline != VK_NULL_HANDLE && entry.mode == mode && entry.programHash == programObj.hash &&
                entry.vertexInputHash == vertexLayoutHash && entry.renderPassHash == renderPassHash &&
                entry.wireRenderPassCompatibilityId == wireRenderPassCompatibilityId &&
                entry.pipelineStateHash == pipelineStateHash && entry.renderStateCso == renderStateCso &&
                entry.primitiveRestartEnable == primitiveRestartEnable && entry.nonLayeredStages == nonLayeredStages &&
                entry.fullCoverageSampleMask == fullCoverageSampleMask && entry.transformFlags == transformFlags) {
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
                programObj.fragmentInputComponentCount, limits.maxFragmentInputComponents, program.GetExternalIndex(),
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
                MOBILEGL_ASSERT(programObj.producerOutputComponentCount <= producerOutputLimit,
                                "GetOrCreatePipeline: producerOutputComponents=%u exceeds stage limit=%u program=%u "
                                "producerStage=%d",
                                programObj.producerOutputComponentCount, producerOutputLimit,
                                program.GetExternalIndex(), static_cast<Int>(programObj.rasterizationProducerStage));
            }
        }
#endif

        const Uint32 vertexInputAttribMask = vis.attributeLocationMask;
        const Uint32 activeAttribMask = programObj.activeVertexInputLocationMask;
        const Uint32 missingAttribMask = activeAttribMask & ~vertexInputAttribMask;
        // The fetch format takes the shader input's signedness (VUID-VkGraphicsPipelineCreateInfo-
        // Input-08733). That renames the bits of 32-bit data, but extends 8/16-bit data by the
        // input's rule instead of the data's; the program variant SetupWireDraw picks for exactly
        // those attributes (ResolveNarrowIntegerVertexInputs) puts GL's value back in the shader.
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
            const Bool canPatch =
                TryCoerceVertexFormatNumericDomain(attribute.format, shaderInputDomain, patchedFormat);
            MOBILEGL_ASSERT(
                canPatch,
                "GetOrCreatePipeline: vertex input location=%u format=%d mismatches shader input type=%u program=%u",
                attribute.location, static_cast<Int>(attribute.format), static_cast<Uint32>(shaderInputType),
                program.GetExternalIndex());

            MGLOG_W_ONCE("GetOrCreatePipeline: patching vertex input location=%u format=%d -> %d to match shader input "
                         "type=%u for program=%u",
                         attribute.location, static_cast<Int>(attribute.format), static_cast<Int>(patchedFormat),
                         static_cast<Uint32>(shaderInputType), program.GetExternalIndex());
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
                const Bool supported =
                    TryGetCurrentVertexAttributeFormat(programObj.vertexInputTypes[location], format);
                MOBILEGL_ASSERT(supported,
                                "DirectVulkan does not support current generic vertex attribute type yet: program=%u "
                                "location=%u type=0x%x activeAttribMask=0x%x vertexInputAttribMask=0x%x",
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
        auto cullFaceEnabled = MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::CullFace);
        auto depthTestEnabled = MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::DepthTest);
        auto polygonOffsetFillEnabled = MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::PolygonOffsetFill) &&
                                        DrawModeUsesPolygonFill(mode);
        auto rasterizerDiscardEnabled = MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::RasterizerDiscard);
        auto colorLogicOpEnabled =
            MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::ColorLogicOp) && m_logicOpFeatureEnabled;
        auto stencilTestEnabled = MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::StencilTest);
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
        const StencilFaceState& frontStencil = MG_Pipe::gPipeInputs->GetStencilState(StencilFace::Front);
        const StencilFaceState& backStencil = MG_Pipe::gPipeInputs->GetStencilState(StencilFace::Back);
        const VkPolygonMode requestedPolygonMode =
            MG_Util::ConvertPolygonModeToVkEnum(MG_Pipe::gPipeInputs->GetPolygonModeFront());
        // VK_POLYGON_MODE_LINE/_POINT require the fillModeNonSolid device feature; fall back to
        // VK_POLYGON_MODE_FILL when the device lacks it.
        const VkPolygonMode effectivePolygonMode =
            (requestedPolygonMode == VK_POLYGON_MODE_FILL || m_fillModeNonSolidFeatureEnabled) ? requestedPolygonMode
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
                   t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
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
        PipelineFactory::PipelineCreatePayload payload{
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
                                   MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::SampleShading),
            .minSampleShading = MG_Pipe::gPipeInputs->GetMinSampleShadingValue(),
            // Word 1 keeps its all-ones initialiser: GL has no state for samples 32..63.
            .sampleMask = {ResolveEffectiveSampleMask(renderPassEntry.sampleCount), 0xffffffffu},
            .subpass = 0,
            .topology = vkTopology,
            .primitiveRestartEnable = primitiveRestartEnabled,
            .patchControlPoints = static_cast<Uint32>(MG_Pipe::gPipeInputs->GetPatchVertices()),
            .viewportCount = ResolveDrawViewportCount(programObj.writesViewportIndexBuiltin),
            .nonLayeredStages = nonLayeredStages,
            .fullCoverageFragmentModule =
                fullCoverageSampleMask ? programObj.fullCoverageFragmentModule : VK_NULL_HANDLE,
            .polygonMode = effectivePolygonMode,
            .cullMode = cullFaceEnabled
                            ? MG_Util::ConvertCullFaceModeToVkEnum(MG_Pipe::gPipeInputs->GetCullFaceMode(), false)
                            : VK_CULL_MODE_NONE,
            // GL's counter-clockwise is Vulkan's clockwise (the two area formulas differ in sign
            // over the same rows), and a Y-negated position - the default framebuffer's flip or
            // an upper-left clip origin - reverses the winding once more. Expressed as the front
            // face rather than as swapped cull bits so gl_FrontFacing agrees with culling, and
            // so glFrontFace(GL_CW) is honoured.
            .frontFace = ((MG_Pipe::gPipeInputs->GetRenderStateParameters().FrontFaceModeSetting ==
                           FrontFaceMode::Clockwise) != invertClockwise)
                             ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                             : VK_FRONT_FACE_CLOCKWISE,
            // Read the geometry stage off the program's own shader list rather than
            // programObj.rasterizationProducerStage: that field is filled by the clip-fixup analysis,
            // which does not run for every program, so it reads Unknown for exactly the
            // geometry-plus-capture programs this guard exists to catch. Both inputs are link-time
            // facts folded into programObj.hash, which is what the pipeline memo and the
            // SetupDrawSnapshot fast path key on - so no memo can hand back a pipeline built for the
            // other mode. IsTransformFeedbackActive() would be a live bug here: neither memo key
            // moves on glBeginTransformFeedback.
            .provokingVertexMode = SelectProvokingVertexMode(vkTopology, ProgramCapturesXfbFromGeometryStage(program)),
            .depthTestEnable = depthTestEnabled,
            .depthWriteEnable = depthTestEnabled && MG_Pipe::gPipeInputs->GetDepthMask(),
            .depthBiasEnable = polygonOffsetFillEnabled,
            .rasterizerDiscardEnable = rasterizerDiscardEnabled,
            .logicOpEnable = colorLogicOpEnabled,
            .stencilTestEnable = stencilTestEnabled,
            .depthCompareOp = MG_Util::ConvertDepthTestFuncToVkEnum(MG_Pipe::gPipeInputs->GetDepthFunc()),
            .logicOp = MG_Util::ConvertLogicOperationToVkEnum(MG_Pipe::gPipeInputs->GetLogicOp()),
            .frontStencilFailOp = MG_Util::ConvertStencilOperationToVkEnum(frontStencil.FailOp),
            .frontStencilPassOp = MG_Util::ConvertStencilOperationToVkEnum(frontStencil.PassDepthPassOp),
            .frontStencilDepthFailOp = MG_Util::ConvertStencilOperationToVkEnum(frontStencil.PassDepthFailOp),
            .frontStencilCompareOp = MG_Util::ConvertDepthTestFuncToVkEnum(frontStencil.Func),
            .backStencilFailOp = MG_Util::ConvertStencilOperationToVkEnum(backStencil.FailOp),
            .backStencilPassOp = MG_Util::ConvertStencilOperationToVkEnum(backStencil.PassDepthPassOp),
            .backStencilDepthFailOp = MG_Util::ConvertStencilOperationToVkEnum(backStencil.PassDepthFailOp),
            .backStencilCompareOp = MG_Util::ConvertDepthTestFuncToVkEnum(backStencil.Func),
            .fragmentReplacesDepth = programObj.fragmentReplacesDepth,
            .fragmentShadingRateDynamic = m_fragmentShadingRate.Pipeline,
            .stages = nonLayeredStages ? &programObj.nonLayeredStages : &programObj.stages,
            .vertexInputState = pipelineVertexInputState,
            .stageSpirvDigests = &programObj.stageSpirvDigests};
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
            const FloatVec4& defaultOuterLevel = MG_Pipe::gPipeInputs->GetPatchDefaultOuterLevel();
            const FloatVec2& defaultInnerLevel = MG_Pipe::gPipeInputs->GetPatchDefaultInnerLevel();
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
            MGLOG_D("GetOrCreatePipeline: disabling depth/stencil tests for program=%u because render pass has no "
                    "depth attachment (attachmentCount=%u colorAttachmentCount=%u)",
                    program.GetExternalIndex(), renderPassEntry.attachmentCount, renderPassEntry.colorAttachmentCount);
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
            drawBuffers[i] = wireFbo->DrawBuffers[i] < 0
                                 ? FramebufferAttachmentType::None
                                 : static_cast<FramebufferAttachmentType>(
                                       static_cast<Int>(FramebufferAttachmentType::Color0) + wireFbo->DrawBuffers[i]);
        for (Uint32 i = 0; i < payload.colorAttachmentCount; ++i) {
            BlendFactor srcRGB = BlendFactor::One;
            BlendFactor dstRGB = BlendFactor::Zero;
            BlendFactor srcAlpha = BlendFactor::One;
            BlendFactor dstAlpha = BlendFactor::Zero;
            BlendEquation colorEquation = BlendEquation::Add;
            BlendEquation alphaEquation = BlendEquation::Add;
            MG_Pipe::gPipeInputs->GetBlendFuncIndexed(i, srcRGB, dstRGB, srcAlpha, dstAlpha);
            MG_Pipe::gPipeInputs->GetBlendEquationIndexed(i, colorEquation, alphaEquation);
            const Bool blendEnabled = MG_Pipe::gPipeInputs->IsCapabilityEnabledIndexed(CapabilityInput::Blend, i);
            // Per-draw-buffer color write mask (glColorMaski). Divergent per-attachment masks require
            // the independentBlend device feature; when it is absent, fall back to draw buffer 0's
            // mask for every attachment (matching the non-indexed glColorMask broadcast).
            const BoolVec4 bufferMask =
                MG_Pipe::gPipeInputs->GetColorMaskIndexed(m_independentBlendFeatureEnabled ? i : 0);
            VkColorComponentFlags attachmentColorWriteMask = static_cast<VkColorComponentFlags>(
                (bufferMask.r() ? VK_COLOR_COMPONENT_R_BIT : 0u) | (bufferMask.g() ? VK_COLOR_COMPONENT_G_BIT : 0u) |
                (bufferMask.b() ? VK_COLOR_COMPONENT_B_BIT : 0u) | (bufferMask.a() ? VK_COLOR_COMPONENT_A_BIT : 0u));
            Bool effectiveBlendEnabled = blendEnabled;
            if (isDefaultDrawFbo && i < drawBuffers.size() && drawBuffers[i] == FramebufferAttachmentType::None) {
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
                                "GetOrCreatePipeline: color attachment %u is out of draw buffer range %zu", i,
                                drawBuffers.size());

                VkFormat colorAttachmentFormat = VK_FORMAT_UNDEFINED;
                Int textureExternalIndex = -1;
                if (!isDefaultDrawFbo) {
                    const Int32 slot = wireFbo->DrawBuffers[i];
                    colorAttachmentFormat =
                        ResolveWireImage(*wireFbo, wireFbo->Color[slot], VK_IMAGE_ASPECT_COLOR_BIT).format;
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
                        MGLOG_E_ONCE(
                            "GetOrCreatePipeline: format=%d lacks VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT; "
                            "disabling blending on attachments with this format (first hit: attachment %u textureId=%d "
                            "program=%u)",
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
                MGLOG_E_ONCE("GetOrCreatePipeline: dual-source blending (GL_SRC1_* blend factor) was requested on "
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
                effectiveBlendEnabled, MG_Util::ConvertBlendFactorToVkEnum(srcRGB),
                MG_Util::ConvertBlendFactorToVkEnum(dstRGB), MG_Util::ConvertBlendEquationToVkEnum(colorEquation),
                MG_Util::ConvertBlendFactorToVkEnum(srcAlpha), MG_Util::ConvertBlendFactorToVkEnum(dstAlpha),
                MG_Util::ConvertBlendEquationToVkEnum(alphaEquation), attachmentColorWriteMask);
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
            entry.nonLayeredStages = nonLayeredStages;
            entry.fullCoverageSampleMask = fullCoverageSampleMask;
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
                                                  VkSurfaceTransformFlagBitsKHR preTransform, Bool isDefaultFbo) const {
        const auto& parameters = MG_Pipe::gPipeInputs->GetRenderStateParameters();
        if (!GLScissorTestClips(index)) {
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
                                                        VkSurfaceTransformFlagBitsKHR preTransform, Bool isDefaultFbo) {
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

    // A rasterizer-discard draw consumes the dynamic state it was recorded with on some drivers
    // (WireDiscardStateProbe.h): every dynamic value is forgotten, so the next draw's tail sets its own
    // again instead of trusting what the shadow says the command buffer holds. Binds stay valid.
    void VulkanRenderer::ForgetDynamicStateAfterDiscardDraw() {
        auto& shadow = *g_dynamicStateShadow;
        shadow.viewportValid = false;
        shadow.scissorValid = false;
        shadow.blendConstantsValid = false;
        shadow.depthBiasValid = false;
        shadow.lineWidthValid = false;
        shadow.stencilValid = false;
        shadow.fragmentShadingRateValid = false;
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
            ApplyPolygonOffsetState(frame.commandBuffer, m_depthBiasClampFeatureEnabled);
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
        const Uint paramsVersion = MG_Pipe::gPipeInputs->GetRenderStateParametersVersion();
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
            const RenderStateParameters& p = MG_Pipe::gPipeInputs->GetRenderStateParameters();
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
            key.polygonOffsetClamp = p.PolygonOffsetClamp;
            key.lineWidth = p.LineWidth;
            // StencilStates[0] is Front, [1] is Back (RenderState::GetStencilFaceIndex),
            // the same order ApplyStencilState reads them in.
            for (Uint32 face = 0; face < 2; ++face) {
                key.stencilValueMask[face] = p.StencilStates[face].ValueMask;
                key.stencilWriteMask[face] = p.StencilStates[face].WriteMask;
                key.stencilRef[face] = p.StencilStates[face].Ref;
            }
            key.scissorEnabled = GLScissorTestClips(0);
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
        ApplyPolygonOffsetState(frame.commandBuffer, m_depthBiasClampFeatureEnabled);
        ApplyLineWidthState(frame.commandBuffer);
        ApplyStencilState(frame.commandBuffer);
        VkRect2D scissor{};
        if (key.scissorEnabled) {
            const IntVec4 scissorBox(key.scissorBox[0], key.scissorBox[1], key.scissorBox[2], key.scissorBox[3]);
            scissor = isDefaultFbo ? MakeDefaultFramebufferScissorRect(scissorBox, extent, preTransform)
                                   : MakeClampedScissorRect(scissorBox, extent);
        } else {
            scissor.offset = {0, 0};
            scissor.extent = {(Uint)extent.x(), (Uint)extent.y()};
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
                                   const DrawCmdParam& drawParams, const IndexBufferView* pIndexBufferView) {
        // First, before anything of this draw is recorded or resolved - on the wire arm too: a
        // split server records every draw its client sends into the same frame command buffer, so
        // the same loading frame grows it the same way there.
        SplitOversizedRecording();
        // A draw into the window: the frame's work so far goes out now, ahead of the acquire wait,
        // while nothing of this draw is recorded yet (its descriptor binds must land in the
        // recording the draw does). ResolveWireImage would split later, mid-setup.
        if (const auto* fbo = MG_Pipe::MGPipeApplier().DrawFramebuffer(); fbo != nullptr && fbo->IsDefault)
            SubmitAheadOfAcquiredImage();
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
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        memoryBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

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
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
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

        const auto stageIt = std::find_if(
            programObj.stages.begin(), programObj.stages.end(),
            [](const VkPipelineShaderStageCreateInfo& stage) { return stage.stage == VK_SHADER_STAGE_COMPUTE_BIT; });
        MOBILEGL_ASSERT(stageIt != programObj.stages.end(), "GetOrCreateComputePipeline: program has no compute stage");
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
