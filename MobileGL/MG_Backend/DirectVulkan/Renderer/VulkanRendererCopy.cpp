// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRendererCopy.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRendererInternal.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
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
                outDstStageMask =
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                outDstAccessMask =
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
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
    } // namespace

    SizeT VulkanRenderer::GetReadbackTexelSize(VkFormat sourceFormat) {
        const VKU_FORMAT_INFO formatInfo = vkuGetFormatInfo(sourceFormat);
        if (formatInfo.texels_per_block != 1) {
            return 0;
        }
        return formatInfo.texel_block_size;
    }

    Bool VulkanRenderer::MapDefaultFramebufferReadbackRect(GLint x, GLint y, GLsizei width, GLsizei height,
                                                           VkExtent2D imageExtent,
                                                           VkSurfaceTransformFlagBitsKHR preTransform,
                                                           VkOffset2D* imageOffset, VkExtent2D* imageCopyExtent) {
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

        if (mappedX < 0 || mappedY < 0 || mappedWidth > imageExtent.width || mappedHeight > imageExtent.height ||
            static_cast<Uint64>(mappedX) + mappedWidth > imageExtent.width ||
            static_cast<Uint64>(mappedY) + mappedHeight > imageExtent.height) {
            return false;
        }
        *imageOffset = {mappedX, mappedY};
        *imageCopyExtent = {mappedWidth, mappedHeight};
        return true;
    }

    Bool VulkanRenderer::RemapDefaultFramebufferReadback(const Uint8* rawPixels, Uint32 logicalWidth,
                                                         Uint32 logicalHeight,
                                                         VkSurfaceTransformFlagBitsKHR preTransform, SizeT texelSize,
                                                         Uint8* outPixels) {
        if (rawPixels == nullptr || outPixels == nullptr || logicalWidth == 0 || logicalHeight == 0 || texelSize == 0) {
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
                       rawPixels + (static_cast<SizeT>(srcY) * rawWidth + srcX) * texelSize, texelSize);
            }
        }
        return true;
    }

    Bool VulkanRenderer::ConvertReadbackPixels(const Uint8* sourcePixels, VkFormat sourceFormat, GLsizei width,
                                               GLsizei height, GLenum destinationFormat, GLenum destinationType,
                                               SizeT destinationRowStride, Uint8* destinationPixels) {
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
            const Uint8* sourceRow =
                sourcePixels + static_cast<SizeT>(row) * static_cast<SizeT>(width) * sourceTexelSize;
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

    void VulkanRenderer::Clear(GLbitfield mask) {
        const auto* fbo = MG_Pipe::MGPipeApplier().DrawFramebuffer();
        if (!fbo) MagmaWireFatal("clear-framebuffer-record");
        ClearAttachmentPayload payload{};
        payload.mask = mask;
        payload.color = MG_Pipe::gPipeInputs->GetClearColor();
        payload.depth = MG_Pipe::gPipeInputs->GetClearDepth();
        payload.stencil = MG_Pipe::gPipeInputs->GetClearStencil();
        ClearWireFramebuffer(*fbo, payload);
        return;
    }

    void VulkanRenderer::QueueClearBufferPayload(GLenum buffer, GLint drawbuffer,
                                                 const ClearAttachmentPayload& clearPayload) {
        const auto* fbo = MG_Pipe::MGPipeApplier().DrawFramebuffer();
        if (!fbo) MagmaWireFatal("clear-buffer-framebuffer-record");
        ClearWireFramebuffer(*fbo, clearPayload, buffer == GL_COLOR ? drawbuffer : -1);
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

    void VulkanRenderer::BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                         GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        BlitWireFramebuffers(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        return;
    }

    void VulkanRenderer::CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                           GLsizei width, GLsizei height) {
        CopyWireFramebufferToTexture(target, level, xoffset, yoffset, x, y, width, height);
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
            // A transfer read of this image canonicalizes E5B9G9R9 words: the copy reads it through
            // the raw route (WireE5RawCopy.h) instead.
            Bool rawE5 = false;
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

    void VulkanRenderer::CopyImageSubData(const CopyImageEndpoint& srcEndpoint, GLenum srcTarget, GLint srcLevel,
                                          GLint srcX, GLint srcY, GLint srcZ, const CopyImageEndpoint& dstEndpoint,
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
            out.rawE5 = !renderbuffer && m_textureManager->WireE5RawReadApplies(*resource);
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
        const Bool inPlace = wire && srcImage.image != VK_NULL_HANDLE && srcImage.image == dstImage.image;
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
                const auto storage =
                    m_textureManager->ResolveWireTextureStorage(endpoint.TextureHandle, mappedMip, mappedLayer);
                if (MG_Pipe::MGPipeHandleIsNull(storage)) MagmaWireFatal("copy-image-view-record");
                mip = static_cast<GLint>(mappedMip);
                layer = static_cast<GLint>(mappedLayer);
                return storage;
            };
            (void)mapSubresource(srcEndpoint, srcLevel, srcZ);
            dstStorageHandle = mapSubresource(dstEndpoint, dstLevel, dstZ);
        }
        srcLevel = std::max<GLint>(srcLevel, 0);
        dstLevel = std::max<GLint>(dstLevel, 0);
        srcZ = std::max<GLint>(srcZ, 0);
        dstZ = std::max<GLint>(dstZ, 0);
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
        MOBILEGL_ASSERT(copyAspectMask != 0 && (srcImage.aspect & copyAspectMask) == srcImage.aspect &&
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
        if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0 || static_cast<Uint32>(srcX + srcWidth) > srcMipWidth ||
            static_cast<Uint32>(srcY + srcHeight) > srcMipHeight ||
            static_cast<Uint32>(dstX + srcWidth) > dstMipWidth ||
            static_cast<Uint32>(dstY + srcHeight) > dstMipHeight) {
            MGLOG_E_ONCE("%s: region outside image bounds (src %dx%d+%d+%d of %ux%u, dst +%d+%d of %ux%u); "
                         "declining the copy",
                         __func__, srcWidth, srcHeight, srcX, srcY, srcMipWidth, srcMipHeight, dstX, dstY, dstMipWidth,
                         dstMipHeight);
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
            const Bool slicesOverlap =
                srcSliceBegin < dstSliceBegin + copySliceCount && dstSliceBegin < srcSliceBegin + copySliceCount;
            const Bool rectOverlaps =
                srcX < dstX + srcWidth && dstX < srcX + srcWidth && srcY < dstY + srcHeight && dstY < srcY + srcHeight;
            if (slicesOverlap && rectOverlaps) {
                MGLOG_E_ONCE("%s: in-place copy on objectId=%u overlaps itself at level %u "
                             "(src %dx%d+%d+%d slice %u, dst +%d+%d slice %u, %u slice(s)); GL leaves "
                             "the result undefined, so the copy is declined rather than recorded",
                             __func__, CopyImageEndpointName(srcEndpoint), srcMipLevel, srcWidth, srcHeight, srcX, srcY,
                             srcSliceBegin, dstX, dstY, dstSliceBegin, copySliceCount);
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

        const auto materializeClear = [this, &frame](const CopyImageEndpoint& endpoint) { return true; };
        const Bool clearReady = materializeClear(srcEndpoint);
        MOBILEGL_ASSERT(clearReady, "%s: failed to materialize pending clear for source objectId=%u", __func__,
                        CopyImageEndpointName(srcEndpoint));
        // A clear still parked on the destination would otherwise materialize AFTER this copy and
        // wipe the texels it just wrote.
        const Bool dstClearReady = materializeClear(dstEndpoint);
        MOBILEGL_ASSERT(dstClearReady, "%s: failed to materialize pending clear for destination objectId=%u", __func__,
                        CopyImageEndpointName(dstEndpoint));

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
            const Bool depthStencil = (copyAspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
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
        // An E5B9G9R9 source the raw route reads is read by a shader, which GENERAL also allows.
        const VkImageLayout copySourceLayout =
            (inPlace || srcImage.rawE5) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkImageLayout copyDestinationLayout =
            inPlace ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        const VkAccessFlags copySourceAccess =
            inPlace ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT) : VK_ACCESS_TRANSFER_READ_BIT;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcOriginalLayout, srcStageMask, srcAccessMask);
        VkImageLayout srcCopyLayout = srcOriginalLayout;
        // The barriers below name a MIP range only. Their layer range is not a parameter:
        // TransitionImageLayout always covers every layer of the image, which is a superset of the
        // [baseSlice, baseSlice + depth) the slice mapping above hands the copy.
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool srcReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcImage.image, *srcImage.trackedLayout, copySourceLayout, srcStageMask,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask, copySourceAccess, srcImage.aspect, 0,
                srcImage.mipLevels);
            MOBILEGL_ASSERT(srcReady, "%s: failed to transition undefined source image", __func__);
            srcCopyLayout = *srcImage.trackedLayout;
        } else {
            Bool srcReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcImage.image, srcCopyLayout, copySourceLayout, srcStageMask,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask, copySourceAccess,
                inPlace ? srcImage.aspect : copyAspectMask, inPlace ? 0u : srcMipLevel,
                inPlace ? srcImage.mipLevels : 1u);
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
                dstStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT, dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                dstImage.aspect, 0, dstImage.mipLevels);
            MOBILEGL_ASSERT(dstReady, "%s: failed to transition undefined destination image", __func__);
            dstCopyLayout = *dstImage.trackedLayout;
        } else {
            Bool dstReady = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstImage.image, dstCopyLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstStageMask,
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, copyAspectMask,
                dstMipLevel, 1);
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
                copyRegion.srcSubresource.baseArrayLayer, copyRegion.srcSubresource.layerCount, copyRegion.srcOffset.z,
                MG_Util::ConvertTextureTargetToString(dstTextureTarget).c_str(), dstMipLevel,
                copyRegion.dstSubresource.baseArrayLayer, copyRegion.dstSubresource.layerCount, copyRegion.dstOffset.z,
                srcWidth, srcHeight, copyRegion.extent.depth);
        VkImage copySourceImage = srcImage.image;
        VkImageLayout copySourceImageLayout = copySourceLayout;
        if (srcImage.rawE5) {
            // The words leave through a shader into a twin, and the twin is what is copied. A
            // twin that cannot be made leaves the plain copy (canonicalized words) in place.
            WireE5RawCopyRegion region;
            region.source = srcImage.image;
            region.volume = srcSlices.slicesAreDepth;
            region.level = srcMipLevel;
            region.baseLayer = copyRegion.srcSubresource.baseArrayLayer;
            region.layerCount = copyRegion.srcSubresource.layerCount;
            region.offset = copyRegion.srcOffset;
            region.extent = {copyRegion.extent.width, copyRegion.extent.height,
                             srcSlices.slicesAreDepth ? copySliceCount : 1u};
            auto twin = MakeUnique<VkTextureManager::TextureResource>();
            WireE5RawCopyObjects objects;
            if (m_textureManager->RecordWireE5RawTwin(frame.commandBuffer, region, copySourceLayout, *twin, objects)) {
                copySourceImage = twin->image;
                copySourceImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, region.layerCount};
                copyRegion.srcOffset = {0, 0, 0};
                WireRetiredObjects retired{};
                retired.textures.push_back(Move(twin));
                retired.imageViews = {objects.sourceView, objects.twinView};
                retired.descriptorPools = {objects.descriptorPool};
                RetireWireObjects(Move(retired));
            }
        }
        // Cross-format copies between a layered image and a 3D one land only their first slice on
        // some drivers (WireCopySliceProbe.h); there, the copy goes one slice per region.
        const VkFormat copySourceFormat = copySourceImage == srcImage.image ? srcImage.format : VK_FORMAT_R32_UINT;
        if (m_wireSplitCrossFormatSliceCopies && copySourceFormat != dstImage.format &&
            srcSlices.slicesAreDepth != dstSlices.slicesAreDepth && copySliceCount > 1) {
            Vector<VkImageCopy> slices(copySliceCount, copyRegion);
            for (Uint32 slice = 0; slice < copySliceCount; ++slice) {
                VkImageCopy& one = slices[slice];
                one.extent.depth = 1;
                if (srcSlices.slicesAreDepth)
                    one.srcOffset.z += static_cast<Int32>(slice);
                else
                    one.srcSubresource = {copyRegion.srcSubresource.aspectMask, copyRegion.srcSubresource.mipLevel,
                                          copyRegion.srcSubresource.baseArrayLayer + slice, 1};
                if (dstSlices.slicesAreDepth)
                    one.dstOffset.z += static_cast<Int32>(slice);
                else
                    one.dstSubresource = {copyRegion.dstSubresource.aspectMask, copyRegion.dstSubresource.mipLevel,
                                          copyRegion.dstSubresource.baseArrayLayer + slice, 1};
            }
            vkCmdCopyImage(frame.commandBuffer, copySourceImage, copySourceImageLayout, dstImage.image,
                           copyDestinationLayout, copySliceCount, slices.data());
        } else {
            vkCmdCopyImage(frame.commandBuffer, copySourceImage, copySourceImageLayout, dstImage.image,
                           copyDestinationLayout, 1, &copyRegion);
        }
        // The copy coordinates above are already storage-relative. Name that
        // storage here too, so a texture view's offsets are not applied twice.
        if (wire && !dstImage.isRenderbuffer)
            m_textureManager->MarkWireTextureGpuWritten(dstStorageHandle, dstMipLevel, dstSlices.BaseArrayLayer(),
                                                        dstSlices.slicesAreDepth ? 1u : copySliceCount);

        VkPipelineStageFlags srcRestoreStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcRestoreAccessMask = 0;
        GetImageTransitionDestinationState(srcRestoreLayout, srcRestoreStageMask, srcRestoreAccessMask);
        if (srcOriginalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Bool srcRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcImage.image, *srcImage.trackedLayout, srcRestoreLayout,
                VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStageMask, copySourceAccess, srcRestoreAccessMask,
                srcImage.aspect, 0, srcImage.mipLevels);
            MOBILEGL_ASSERT(srcRestored, "%s: failed to restore undefined source image layout", __func__);
        } else {
            Bool srcRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, srcImage.image, srcCopyLayout, srcRestoreLayout, VK_PIPELINE_STAGE_TRANSFER_BIT,
                srcRestoreStageMask, copySourceAccess, srcRestoreAccessMask, inPlace ? srcImage.aspect : copyAspectMask,
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
                VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStageMask, VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask,
                dstImage.aspect, 0, dstImage.mipLevels);
            MOBILEGL_ASSERT(dstRestored, "%s: failed to restore undefined destination image layout", __func__);
        } else {
            Bool dstRestored = VkTextureManager::TransitionImageLayout(
                frame.commandBuffer, dstImage.image, dstCopyLayout, dstRestoreLayout, VK_PIPELINE_STAGE_TRANSFER_BIT,
                dstRestoreStageMask, VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccessMask, copyAspectMask, dstMipLevel,
                1);
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

        // A SLOT THAT HAS NOT ACQUIRED STILL HOLDS ITS FENCE SIGNALED. The slot fences are created
        // signaled and only WaitAndAcquireNextImage resets them, so while the first acquire is
        // deferred (no swapchain yet: a zero-area or hidden window, which is what an offscreen
        // context created through a helper window is) the current slot's fence is still signaled
        // here. Submitting it in that state is invalid (VUID-vkQueueSubmit-fence-00063), and in
        // practice the wait below then returns at once - before the copy has run - so the readback
        // hands back the buffer's initial zeros. The late signal from that submission could also
        // land after the reset below, leaving the NEXT readback to submit a signaled fence again.
        //
        // Such a fence is unsignaled before the submit, and handed back signaled afterwards: the
        // deferred acquire (Present) waits on it with no submission of its own, and a fence this
        // readback reset and left unsignaled would never signal for that wait.
        const Bool slotFenceIdle = vkGetFenceStatus(m_device, frame.imageInFlightFence) == VK_SUCCESS;
        if (slotFenceIdle) {
            const VkResult resetResult = vkResetFences(m_device, 1, &frame.imageInFlightFence);
            if (resetResult != VK_SUCCESS) {
                MGLOG_E_ONCE("DirectVulkan readback: vkResetFences on the idle slot fence returned %d", resetResult);
                return false;
            }
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
        VkResult result = vkWaitForFences(m_device, static_cast<Uint32>(readbackFences.size()), readbackFences.data(),
                                          VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            NoteDeviceLoss(result, "readback vkWaitForFences");
            MGLOG_E_ONCE("DirectVulkan readback: vkWaitForFences returned %d", result);
            return false;
        }
        OnSubmitsCompletedUpTo(frame.lastSubmitIndex);
        if (!slotFenceIdle) {
            result = vkResetFences(m_device, 1, &frame.imageInFlightFence);
            if (result != VK_SUCCESS) {
                MGLOG_E_ONCE("DirectVulkan readback: vkResetFences returned %d", result);
                return false;
            }
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
        ReadWirePixels(x, y, width, height, format, type, pixels);
        return;
    }

    Bool VulkanRenderer::BlitDepthAcrossFormats(FrameContext::FrameData& frame, VkImage srcImage, VkFormat srcFormat,
                                                VkImageLayout* srcTrackedLayout, Uint32 srcMipLevel,
                                                Uint32 srcBaseArrayLayer, VkImage dstImage, VkFormat dstFormat,
                                                VkImageLayout* dstTrackedLayout, Uint32 dstMipLevel,
                                                Uint32 dstBaseArrayLayer, GLint srcX, GLint srcY, GLint dstX,
                                                GLint dstY, GLint width, GLint height, VkImageLayout srcRestoreLayout,
                                                VkImageLayout dstRestoreLayout, Bool stencilAspect) {
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
            VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, aspectMaskForFormat(srcFormat),
            srcMipLevel, 1);
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
        ok = VkTextureManager::TransitionImageLayout(frame.commandBuffer, srcImage, *srcTrackedLayout, srcRestoreLayout,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT, srcRestoreStage,
                                                     VK_ACCESS_TRANSFER_READ_BIT, srcRestoreAccess,
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
            VK_PIPELINE_STAGE_TRANSFER_BIT, dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT, aspectMaskForFormat(dstFormat),
            dstMipLevel, 1);
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
        ok = VkTextureManager::TransitionImageLayout(frame.commandBuffer, dstImage, *dstTrackedLayout, dstRestoreLayout,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT, dstRestoreStage,
                                                     VK_ACCESS_TRANSFER_WRITE_BIT, dstRestoreAccess,
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
                x, y, width, height, defaultFboExtent, m_swapchainObject.GetPreTransform(), &copyOffset, &copyExtent);
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
                remapped =
                    RemapDefaultFramebufferReadback(depthSrc, static_cast<Uint32>(width), static_cast<Uint32>(height),
                                                    preTransform, depthCopyBytes, remappedDepth.data());
            }
            if (remapped && wantStencil) {
                remappedStencil.resize(pixelCount);
                remapped =
                    RemapDefaultFramebufferReadback(stencilSrc, static_cast<Uint32>(width), static_cast<Uint32>(height),
                                                    preTransform, 1, remappedStencil.data());
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
        const auto& pixelPackBufferObject = wireReplyHasNoPackBuffer;
        const auto packParams = MG_Pipe::gPipeInputs->GetPixelStoreParameters(false);
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
} // namespace MobileGL::MG_Backend::DirectVulkan
