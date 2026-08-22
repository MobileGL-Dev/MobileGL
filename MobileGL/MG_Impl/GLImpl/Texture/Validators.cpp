// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/Validators.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Validators.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Metrics/TextureMetrics.h>

namespace MobileGL::MG_Impl::GLImpl::TextureImpl {
    Bool ValidateTextureTarget(TextureTarget target) {
        if (target == TextureTarget::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureTarget", "Invalid texture target"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureUploadTarget(TextureUploadTarget textureUploadTarget) {
        if (textureUploadTarget == TextureUploadTarget::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureUploadTarget",
                                                                     "Invalid texture upload target"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureName(Uint texture, Bool allowZero) {
        if (texture == 0) {
            if (allowZero) return true;

            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureName", "Texture name cannot be zero"));
            return false;
        }

        if (!MG_State::pGLContext->ValidateTextureName(texture)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureName", "Invalid texture name"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureInputFormat(TextureInputFormat format) {
        if (format == TextureInputFormat::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInputFormat",
                                                                     "Invalid texture input format"));
            return false;
        }
        return true;
    }

    Bool ValidateTexturePixelDataType(TexturePixelDataType texturePixelDataType) {
        if (texturePixelDataType == TexturePixelDataType::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTexturePixelDataType",
                                                                     "Invalid texture pixel data type"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureLevelNumber(GLint level) {
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureLevelNumber",
                                                                      "Texture level must be non-negative"));
            return false;
        }

        Int maxTextureSize = MG_Backend::DynamicBackendParameters{}.MaxTextureSize;
        if (MG_Backend::pActiveBackendObject) {
            maxTextureSize = MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxTextureSize;
        }
        Int maxLevel = 0;
        for (Int size = std::max(maxTextureSize, 1); size > 1; size >>= 1) {
            ++maxLevel;
        }
        if (level > maxLevel) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureLevelNumber",
                                                                      "Texture level exceeds GL_MAX_TEXTURE_SIZE"));
            return false;
        }

        return true;
    }

    Bool ValidateTextureSizeWithTextureUploadTarget(TextureUploadTarget target, GLsizei width, GLsizei height) {
        if (target == TextureUploadTarget::CubeMapPositiveX || target == TextureUploadTarget::CubeMapNegativeX ||
            target == TextureUploadTarget::CubeMapPositiveY || target == TextureUploadTarget::CubeMapNegativeY ||
            target == TextureUploadTarget::CubeMapPositiveZ || target == TextureUploadTarget::CubeMapNegativeZ) {
            if (width != height) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSizeWithTarget",
                                                 "Width and height must be equal for cube map textures"));
                return false;
            }
        }

        if (!(target == TextureUploadTarget::Texture1DArray || target == TextureUploadTarget::ProxyTexture1DArray)) {
            if (height < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSizeWithTarget",
                                                 "Height must be greater than or equal to zero"));
                return false;
            }
            // TODO: GL_INVALID_VALUE is generated if target is not GL_TEXTURE_1D_ARRAY or GL_PROXY_TEXTURE_1D_ARRAY
            // and height is greater than GL_MAX_TEXTURE_SIZE.
        }

        if (target == TextureUploadTarget::Texture1DArray || target == TextureUploadTarget::ProxyTexture1DArray) {
            if (height < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSizeWithTarget",
                                                 "Height must be greater than or equal to zero"));
                return false;
            }
            // TODO: GL_INVALID_VALUE is generated if target is GL_TEXTURE_1D_ARRAY or GL_PROXY_TEXTURE_1D_ARRAY and
            // height is greater than GL_MAX_ARRAY_TEXTURE_LAYERS.
        }

        return true;
    }

    Bool ValidateTextureSizeRange(Int width, Int height, Int depth) {
        if (width < 0 || height < 0 || depth < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSizeRange",
                                                                      "Width and height must be greater than zero"));
            return false;
        }

        // TODO: GL_INVALID_VALUE is generated if width is greater than GL_MAX_TEXTURE_SIZE.

        return true;
    }

    Bool ValidateTextureInternalFormat(TextureInternalFormat format) {
        if (format == TextureInternalFormat::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInternalFormat",
                                                                     "Invalid texture sized internal format"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureBorderNumber(Int border) {
        if (border != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureBorderNumber", "Border must be zero"));
            return false;
        }
        return true;
    }

    Bool IsIntegerColorInputFormat(TextureInputFormat format) {
        return format == TextureInputFormat::RInteger || format == TextureInputFormat::RGInteger ||
               format == TextureInputFormat::RGBInteger || format == TextureInputFormat::BGRInteger ||
               format == TextureInputFormat::RGBAInteger || format == TextureInputFormat::BGRAInteger ||
               format == TextureInputFormat::GreenInteger || format == TextureInputFormat::BlueInteger ||
               format == TextureInputFormat::AlphaInteger;
    }

    Bool IsIntegerColorInternalFormat(TextureInternalFormat internalFormat) {
        switch (internalFormat) {
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::R32UI:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RG32UI:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA32I:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::RGB10A2UI:
                return true;
            default:
                return false;
        }
    }

    static Bool IsDepthLikeInternalFormat(TextureInternalFormat internalFormat) {
        switch (internalFormat) {
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthComponent16:
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32: // not core, kept for Minecraft 1.21.5+
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
            case TextureInternalFormat::DepthStencil:
            // Stencil-only is not a colour format either: a colour client format read against a
            // STENCIL_INDEX8 texture has to be the same INVALID_OPERATION as against a depth one.
            case TextureInternalFormat::StencilIndex8:
                return true;
            default:
                return false;
        }
    }

    static Bool IsDepthLikeInputFormat(TextureInputFormat format) {
        return format == TextureInputFormat::DepthComponent || format == TextureInputFormat::DepthStencil ||
               format == TextureInputFormat::StencilIndex;
    }

    // Client-memory format<->type pairing rules shared by pixel uploads (TexImage*) and readbacks
    // (ReadPixels, GetTexImage). Mirrors the desktop-GL validity matrix used by GL CTS packed_pixels
    // (glcPackedPixelsTests isFormatValid): packed types constrain the formats they may pair with, and
    // integer formats reject floating-point types; violations raise GL_INVALID_OPERATION.
    Bool ValidateClientFormatTypePairing(TextureInputFormat format, TexturePixelDataType type) {
        const auto recordInvalidOperation = [](const char* message) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateClientFormatTypePairing", message));
            return false;
        };

        if (type == TexturePixelDataType::UnsignedByte332 || type == TexturePixelDataType::UnsignedByte233Rev ||
            type == TexturePixelDataType::UnsignedShort565 || type == TexturePixelDataType::UnsignedShort565Rev) {
            if (format != TextureInputFormat::RGB && format != TextureInputFormat::RGBInteger) {
                return recordInvalidOperation("Packed RGB type requires RGB or RGB_INTEGER format");
            }
        }

        if (type == TexturePixelDataType::UnsignedInt101111Rev || type == TexturePixelDataType::UnsignedInt5999Rev) {
            if (format != TextureInputFormat::RGB) {
                return recordInvalidOperation("Packed float RGB type requires RGB format");
            }
        }

        if (type == TexturePixelDataType::UnsignedShort4444 || type == TexturePixelDataType::UnsignedShort4444Rev ||
            type == TexturePixelDataType::UnsignedShort5551 || type == TexturePixelDataType::UnsignedShort1555Rev ||
            type == TexturePixelDataType::UnsignedInt8888 || type == TexturePixelDataType::UnsignedInt8888Rev ||
            type == TexturePixelDataType::UnsignedInt1010102 || type == TexturePixelDataType::UnsignedInt2101010Rev) {
            if (format != TextureInputFormat::RGBA && format != TextureInputFormat::BGRA &&
                format != TextureInputFormat::RGBAInteger && format != TextureInputFormat::BGRAInteger) {
                return recordInvalidOperation("Packed RGBA type requires RGBA/BGRA (integer) format");
            }
        }

        if (type == TexturePixelDataType::UnsignedInt248 || type == TexturePixelDataType::Float32UnsignedInt248Rev) {
            if (format != TextureInputFormat::DepthStencil) {
                return recordInvalidOperation("Packed depth-stencil type requires DEPTH_STENCIL format");
            }
        }

        if (format == TextureInputFormat::DepthStencil && type != TexturePixelDataType::UnsignedInt248 &&
            type != TexturePixelDataType::Float32UnsignedInt248Rev) {
            return recordInvalidOperation("DEPTH_STENCIL format requires a packed depth-stencil type");
        }

        if (IsIntegerColorInputFormat(format) &&
            (type == TexturePixelDataType::Float || type == TexturePixelDataType::HalfFloat)) {
            return recordInvalidOperation("Integer format cannot be used with a floating-point type");
        }

        return true;
    }

    // Mirrors the desktop-GL validity matrix used by GL CTS packed_pixels (glcPackedPixelsTests
    // isFormatValid, INPUT_TEXIMAGE): packed-type/format pairing, depth-vs-color mismatch, and
    // integer-ness matching all raise GL_INVALID_OPERATION instead of reaching the upload path.
    Bool ValidateTextureInternalFormatCompatibleWithInput(TextureInputFormat format,
                                                          TextureInternalFormat internalFormat,
                                                          TexturePixelDataType type) {
        const auto recordInvalidOperation = [](const char* message) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInternalFormatCompatibleWithInput",
                                             message));
            return false;
        };

        if (!ValidateClientFormatTypePairing(format, type)) {
            return false;
        }

        // The stencil-only transfer path arrived with GL 4.4 / ARB_texture_stencil8, and only ever
        // pairs with stencil-only storage: against a depth, depth-stencil or colour internal format
        // STENCIL_INDEX keeps the pre-4.4 answer (GL CTS packed_pixels feeds exactly that pairing
        // and expects INVALID_OPERATION).
        if (format == TextureInputFormat::StencilIndex &&
            internalFormat != TextureInternalFormat::StencilIndex8) {
            return recordInvalidOperation("STENCIL_INDEX requires a stencil-only internal format");
        }

        if (IsDepthLikeInputFormat(format) != IsDepthLikeInternalFormat(internalFormat)) {
            return recordInvalidOperation("Depth/stencil-ness of format and internal format must match");
        }

        if (IsIntegerColorInputFormat(format) != IsIntegerColorInternalFormat(internalFormat)) {
            return recordInvalidOperation("Integer-ness of format and internal format must match");
        }

        return true;
    }

    Bool ValidateTextureLevelWithUploadTarget(TextureUploadTarget target, Int level) {
        if (target == TextureUploadTarget::TextureRectangle || target == TextureUploadTarget::ProxyTextureRectangle) {
            if (level != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureLevelWithUploadTarget",
                                                 "Level must be zero for rectangle textures"));
                return false;
            }
        }
        if (target == TextureUploadTarget::Texture2DMultisample ||
            target == TextureUploadTarget::ProxyTexture2DMultisample ||
            target == TextureUploadTarget::Texture2DMultisampleArray ||
            target == TextureUploadTarget::ProxyTexture2DMultisampleArray) {
            if (level != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureLevelWithUploadTarget",
                                                 "Level must be zero for multisample textures"));
                return false;
            }
        }
        return true;
    }

    Bool ValidateTextureLevelExists(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, Int level,
                                    const char* caller) {
        // A null object is somebody else's error to report - ValidateTextureObject runs
        // first at every call site and has already recorded it.
        if (!textureObject) return false;

        const auto* mipmapTexture = MG_State::GLState::AsMipmapTexture(textureObject.get());
        if (mipmapTexture == nullptr) {
            // The only non-mipmap storage class is a buffer texture, and GL_TEXTURE_BUFFER is
            // not a target glCopyImageSubData accepts at all (it is in the CTS's invalid-target
            // set). Declining here is not the error code the spec asks for - that would be
            // INVALID_ENUM from a target check this validator is not - but it does keep a
            // texture with no image levels whatsoever from reaching a backend that would
            // dereference a backend texture it never created.
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Texture has no mipmap levels to address."));
            return false;
        }

        // What this number is, exactly, because two other things are almost it and neither is
        // safe to assume: it is the number of level SLOTS the shadow has allocated - holes
        // included, since MipmapStorage::AllocateLevel grows to level+1 and never fills the gap.
        // For a cube map MipmapUploadTargetArray reports face +X's chain rather than the union.
        //
        // The guarantee that matters is one-sided: this count is always >= the level count the
        // backends derive (VkTextureManager::GetUploadMipLevelCount stops at the first level
        // with a non-positive extent, so it can only be shorter). That is the safe direction -
        // no copy to a level the texture genuinely has is ever rejected here. It is NOT an
        // exact match, so the backends keep their own range guard for the band in between: a
        // chain with a hole (level 0 and 2 defined, 1 not) is accepted by this predicate and
        // declined by the backend, which is a silent no-op rather than a copy. That band is a
        // backend storage limitation, not a validation one - rejecting it here with
        // INVALID_VALUE would be refusing a copy the spec permits.
        const Uint levelCount = mipmapTexture->GetMipmapLevelCount();

        if (levelCount == 0) {
            // No image has ever been defined on this texture, so the fault is the texture,
            // not the number: GL 4.6 core 18.3.2 asks for INVALID_OPERATION when an object a
            // copy names is an incomplete texture.
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Texture has no image defined at any level."));
            return false;
        }
        if (level < 0 || static_cast<Uint>(level) >= levelCount) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Texture level does not exist in this texture."));
            return false;
        }
        return true;
    }

    Bool ValidateTextureObject(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject) {
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureObject", "Texture object is null"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureNotDefault(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                   const char* caller) {
        if (textureObject && textureObject->GetExternalIndex() == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "This operation is not allowed on the default texture (zero is "
                                             "bound to the target)."));
            return false;
        }
        return true;
    }

    Bool ValidateTextureTargetUniformity(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                         TextureTarget target) {
        if (!textureObject) return true; // should be created later
        TextureTarget prevTarget = textureObject->GetTarget();
        if (prevTarget != target) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureTargetUniformity",
                                             "Texture target does not match the previously created texture"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureSubImageOffsets(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, Int xoffset,
                                        Int width, Int yoffset, Int height, Int zoffset, Int depth) {
        auto baseSize = textureObject->GetBaseSize();
        if (xoffset < 0 || (xoffset + width) > baseSize.x()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSubImageOffsets",
                                             "xoffset must be non-negative and (xoffset + width) must not exceed "
                                             "the texture width."));
            return false;
        }
        if (baseSize.y() == 0) return true;

        if (yoffset < 0 || (yoffset + height) > baseSize.y()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSubImageOffsets",
                                             "yoffset must be non-negative and (yoffset + height) must not exceed "
                                             "the texture height."));
            return false;
        }
        if (baseSize.z() == 0) return true;

        if (zoffset < 0 || (zoffset + depth) > baseSize.z()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSubImageOffsets",
                                             "zoffset must be non-negative and (zoffset + depth) must not exceed "
                                             "the texture depth."));
            return false;
        }
        return true;
    }

    namespace {
        // Component set of an UNSIZED base internal format, as the bitmask GL 4.6 SS 8.6
        // reasons about. Colour components are independent bits so "subset" is a plain
        // mask test; depth and stencil are their own components and never satisfy a
        // colour request (or each other).
        enum : Uint32 {
            kComponentR = 1u << 0,
            kComponentG = 1u << 1,
            kComponentB = 1u << 2,
            kComponentA = 1u << 3,
            kComponentDepth = 1u << 4,
            kComponentStencil = 1u << 5,
        };

        Uint32 BaseFormatComponents(TextureInternalFormat unsizedFormat) {
            switch (unsizedFormat) {
            case TextureInternalFormat::Red:
                return kComponentR;
            case TextureInternalFormat::RG:
                return kComponentR | kComponentG;
            case TextureInternalFormat::RGB:
                return kComponentR | kComponentG | kComponentB;
            case TextureInternalFormat::RGBA:
                return kComponentR | kComponentG | kComponentB | kComponentA;
            case TextureInternalFormat::DepthComponent:
                return kComponentDepth;
            case TextureInternalFormat::DepthStencil:
                return kComponentDepth | kComponentStencil;
            default:
                return 0;
            }
        }
    } // namespace

    CopyImageTexelBlock ResolveCopyImageTexelBlock(TextureInternalFormat format, GLenum compressedFormat) {
        CopyImageTexelBlock block{};
        if (compressedFormat != GL_NONE) {
            const auto info = MG_Util::GetCompressedFormatInfo(compressedFormat);
            if (info.blockByteSize != 0) {
                block.byteSize = info.blockByteSize;
                block.blockWidth = info.blockWidth;
                block.blockHeight = info.blockHeight;
                block.compressed = true;
                return block;
            }
        }
        // The size MobileGL actually stores a texel of this format in, which for every format GL
        // gives a required size is that required size. The handful of legacy formats GL leaves
        // implementation-defined (R3_G3_B2, RGB4/5/10/12, RGBA2/12) have no view class in table
        // 8.22 to be compared against anyway, and this is the size that decides whether a raw
        // copy between them would in fact preserve the bytes.
        block.byteSize = MG_Util::GetSizedInternalFormatSizeInBytes(format);
        return block;
    }

    Bool ValidateCopyImageFormatCompatibility(const CopyImageTexelBlock& srcBlock,
                                              const CopyImageTexelBlock& dstBlock) {
        if (srcBlock.byteSize == 0 || dstBlock.byteSize == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateCopyImageFormatCompatibility",
                                             "A copied image has no storage whose texel size is known."));
            return false;
        }
        if (srcBlock.byteSize != dstBlock.byteSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateCopyImageFormatCompatibility",
                    std::format("The two images' texel blocks are different sizes ({} vs. {} bytes), so the "
                                "formats are not copy-compatible.",
                                srcBlock.byteSize, dstBlock.byteSize)));
            return false;
        }
        // Two compressed images additionally have to agree on the SHAPE of the block, not only
        // its size: an 8-byte 4x4 block and a hypothetical 8-byte 8x8 one hold different texel
        // counts, and GL 4.6 core 18.3.2 requires both dimensions to match.
        if (srcBlock.compressed && dstBlock.compressed &&
            (srcBlock.blockWidth != dstBlock.blockWidth || srcBlock.blockHeight != dstBlock.blockHeight)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateCopyImageFormatCompatibility",
                    std::format("The two compressed images have different block dimensions ({}x{} vs. {}x{}).",
                                srcBlock.blockWidth, srcBlock.blockHeight, dstBlock.blockWidth,
                                dstBlock.blockHeight)));
            return false;
        }
        return true;
    }

    Bool ValidateCopyImageBlockAlignment(const CopyImageTexelBlock& block, Int x, Int y, Int width, Int height,
                                         Int imageWidth, Int imageHeight, const char* endpointName) {
        if (!block.compressed) return true;
        const Int blockWidth = static_cast<Int>(block.blockWidth);
        const Int blockHeight = static_cast<Int>(block.blockHeight);
        if (blockWidth <= 1 && blockHeight <= 1) return true;
        // The origin is unconditional; the extent gets the "or it reaches the edge of the image"
        // exemption GL 4.6 core 18.3.2 grants, which is what lets a 16x16 BPTC image be copied
        // whole even when the last block is partial.
        const Bool originAligned = (x % blockWidth == 0) && (y % blockHeight == 0);
        const Bool widthOk = (width % blockWidth == 0) || (x + width == imageWidth);
        const Bool heightOk = (height % blockHeight == 0) || (y + height == imageHeight);
        if (originAligned && widthOk && heightOk) return true;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidValue,
            MakeUnique<GenericErrorInfo>(
                "MG_Impl/GLImpl", "ValidateCopyImageBlockAlignment",
                std::format("The {} region [{}, {}] + [{} x {}] is not aligned to the {}x{} compressed block "
                            "grid of a {} x {} image.",
                            endpointName, x, y, width, height, blockWidth, blockHeight, imageWidth, imageHeight)));
        return false;
    }

    Bool ValidateCopyTexImageBaseFormatSubset(TextureInternalFormat destFormat, TextureInternalFormat srcFormat) {
        const auto unsizedDest = MG_Util::ConvertInternalFormatToUnsized(destFormat);
        const auto unsizedSrc = MG_Util::ConvertInternalFormatToUnsized(srcFormat);
        // GL 4.6 SS 8.6: glCopyTexImage* may request a SUBSET of the read buffer's components,
        // not an exact match - GL_RGB from an RGBA8 framebuffer is textbook legal and is what
        // Minecraft and its mods do. glCopyTexImage2D used to run the exact-match predicate
        // above and turn its rejection into an uncaught exception through the C GL ABI, so the
        // app died rather than seeing a GL error.
        const Uint32 destComponents = BaseFormatComponents(unsizedDest);
        const Uint32 srcComponents = BaseFormatComponents(unsizedSrc);
        if (destComponents == 0 || srcComponents == 0 || (destComponents & ~srcComponents) != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateCopyTexImageBaseFormatSubset",
                    std::format("the read buffer's base internal format {} does not provide every component of "
                                "the requested internal format {}",
                                MG_Util::ConvertTextureInternalFormatToString(unsizedSrc),
                                MG_Util::ConvertTextureInternalFormatToString(unsizedDest))));
            return false;
        }
        return true;
    }

    // GL 4.6 core table 8.21 ("Compatible internal formats for TextureView"), transcribed whole.
    // Written against the raw GLenum rather than TextureInternalFormat on purpose: MobileGL's own
    // enum collapses every compressed format onto uncompressed storage and drops formats it
    // cannot carry, so classifying the converted value would silently widen the compatibility
    // rule - GL_COMPRESSED_RG_RGTC2 and GL_RGBA8 would end up in the same class.
    TextureViewClass GetTextureViewClass(GLenum internalformat) {
        switch (internalformat) {
        case GL_RGBA32F:
        case GL_RGBA32UI:
        case GL_RGBA32I:
            return TextureViewClass::Bits128;
        case GL_RGB32F:
        case GL_RGB32UI:
        case GL_RGB32I:
            return TextureViewClass::Bits96;
        case GL_RGBA16F:
        case GL_RG32F:
        case GL_RGBA16UI:
        case GL_RG32UI:
        case GL_RGBA16I:
        case GL_RG32I:
        case GL_RGBA16:
        case GL_RGBA16_SNORM:
            return TextureViewClass::Bits64;
        case GL_RGB16:
        case GL_RGB16_SNORM:
        case GL_RGB16F:
        case GL_RGB16UI:
        case GL_RGB16I:
            return TextureViewClass::Bits48;
        case GL_RG16F:
        case GL_R11F_G11F_B10F:
        case GL_R32F:
        case GL_RGB10_A2UI:
        case GL_RGBA8UI:
        case GL_RG16UI:
        case GL_R32UI:
        case GL_RGBA8I:
        case GL_RG16I:
        case GL_R32I:
        case GL_RGB10_A2:
        case GL_RGBA8:
        case GL_RG16:
        case GL_RGBA8_SNORM:
        case GL_RG16_SNORM:
        case GL_SRGB8_ALPHA8:
        case GL_RGB9_E5:
            return TextureViewClass::Bits32;
        case GL_RGB8:
        case GL_RGB8_SNORM:
        case GL_SRGB8:
        case GL_RGB8UI:
        case GL_RGB8I:
            return TextureViewClass::Bits24;
        case GL_R16F:
        case GL_RG8UI:
        case GL_R16UI:
        case GL_RG8I:
        case GL_R16I:
        case GL_RG8:
        case GL_R16:
        case GL_RG8_SNORM:
        case GL_R16_SNORM:
            return TextureViewClass::Bits16;
        case GL_R8UI:
        case GL_R8I:
        case GL_R8:
        case GL_R8_SNORM:
            return TextureViewClass::Bits8;
        case GL_COMPRESSED_RED_RGTC1:
        case GL_COMPRESSED_SIGNED_RED_RGTC1:
            return TextureViewClass::Rgtc1Red;
        case GL_COMPRESSED_RG_RGTC2:
        case GL_COMPRESSED_SIGNED_RG_RGTC2:
            return TextureViewClass::Rgtc2Rg;
        case GL_COMPRESSED_RGBA_BPTC_UNORM:
        case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
            return TextureViewClass::BptcUnorm;
        case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT:
        case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT:
            return TextureViewClass::BptcFloat;
        default:
            // Every depth/stencil format, every S3TC/ETC/ASTC format and every unsized format
            // reaches here. The caller must then demand an EXACT format match.
            return TextureViewClass::None;
        }
    }

    // GL 4.6 core table 8.20 ("Legal texture targets for TextureView").
    Bool IsLegalTextureViewTargetPair(TextureTarget origTarget, TextureTarget viewTarget) {
        switch (origTarget) {
        case TextureTarget::Texture1D:
            return viewTarget == TextureTarget::Texture1D || viewTarget == TextureTarget::Texture1DArray;
        case TextureTarget::Texture2D:
            return viewTarget == TextureTarget::Texture2D || viewTarget == TextureTarget::Texture2DArray;
        case TextureTarget::Texture3D:
            return viewTarget == TextureTarget::Texture3D;
        case TextureTarget::TextureCubeMap:
            return viewTarget == TextureTarget::TextureCubeMap || viewTarget == TextureTarget::Texture2D ||
                   viewTarget == TextureTarget::Texture2DArray || viewTarget == TextureTarget::TextureCubeMapArray;
        case TextureTarget::TextureRectangle:
            return viewTarget == TextureTarget::TextureRectangle;
        case TextureTarget::Texture1DArray:
            return viewTarget == TextureTarget::Texture1DArray || viewTarget == TextureTarget::Texture1D;
        case TextureTarget::Texture2DArray:
            return viewTarget == TextureTarget::Texture2DArray || viewTarget == TextureTarget::Texture2D ||
                   viewTarget == TextureTarget::TextureCubeMap || viewTarget == TextureTarget::TextureCubeMapArray;
        case TextureTarget::TextureCubeMapArray:
            return viewTarget == TextureTarget::TextureCubeMapArray || viewTarget == TextureTarget::Texture2DArray ||
                   viewTarget == TextureTarget::Texture2D || viewTarget == TextureTarget::TextureCubeMap;
        case TextureTarget::Texture2DMultisample:
        case TextureTarget::Texture2DMultisampleArray:
            return viewTarget == TextureTarget::Texture2DMultisample ||
                   viewTarget == TextureTarget::Texture2DMultisampleArray;
        case TextureTarget::TextureBuffer:
            // The table lists no legal target for a buffer texture: its storage is a buffer
            // object, and there is nothing to make a view of.
            return false;
        default:
            return false;
        }
    }

    Uint RequiredTextureViewLayerCount(TextureTarget viewTarget) {
        switch (viewTarget) {
        case TextureTarget::TextureCubeMap:
            return 6;
        case TextureTarget::Texture1D:
        case TextureTarget::Texture2D:
        case TextureTarget::Texture3D:
        case TextureTarget::TextureRectangle:
        case TextureTarget::Texture2DMultisample:
            return 1;
        default:
            // 1D/2D array, cube-map array, 2D multisample array: any count (the cube-map array's
            // "multiple of 6" is checked by the caller).
            return 0;
        }
    }
} // namespace MobileGL::MG_Impl::GLImpl::TextureImpl
