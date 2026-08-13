// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/Validators.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include "MG_Util/Types.h"
#include <Includes.h>
#include <MG_State/GLState/TextureState/TextureObject.h>

namespace MobileGL::MG_Impl::GLImpl::TextureImpl {
    Bool ValidateTextureTarget(TextureTarget target);
    Bool ValidateTextureUploadTarget(TextureUploadTarget textureUploadTarget);
    Bool ValidateTextureName(Uint texture, Bool allowZero = false);
    Bool ValidateTextureInputFormat(TextureInputFormat format);
    Bool ValidateTexturePixelDataType(TexturePixelDataType texturePixelDataType);
    Bool ValidateTextureLevelNumber(Int level);
    Bool ValidateTextureSizeWithTextureUploadTarget(TextureUploadTarget target, GLsizei width, GLsizei height);
    Bool ValidateTextureSizeRange(Int width, Int height, Int depth);
    Bool ValidateTextureInternalFormat(TextureInternalFormat format);
    Bool ValidateTextureBorderNumber(Int border);
    Bool IsIntegerColorInputFormat(TextureInputFormat format);
    Bool IsIntegerColorInternalFormat(TextureInternalFormat internalFormat);
    Bool ValidateClientFormatTypePairing(TextureInputFormat format, TexturePixelDataType type);
    Bool ValidateTextureInternalFormatCompatibleWithInput(TextureInputFormat format,
                                                          TextureInternalFormat internalFormat,
                                                          TexturePixelDataType type);
    Bool ValidateTextureLevelWithUploadTarget(TextureUploadTarget target, Int level);
    // "Is <level> a level this texture actually has?", which ValidateTextureLevelNumber above
    // does NOT answer - that one only bounds the index by GL_MAX_TEXTURE_SIZE and knows nothing
    // about the object. Entry points that resolve a level straight into a backend image
    // subresource need this one: a level the texture never had is GL_INVALID_VALUE (GL 4.6 core
    // 18.3.2), and passing it through instead reaches the driver as an out-of-range subresource.
    // Note the error split is per-entry-point, so this is not universally reusable:
    // glClearTexImage owes INVALID_OPERATION for the same out-of-range level and spells its own
    // copy of this predicate in GL_Texture.cpp (GetClearTextureObject).
    Bool ValidateTextureLevelExists(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, Int level,
                                    const char* caller);
    Bool ValidateTextureObject(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject);
    // Rejects the per-target default texture objects (name 0) with GL_INVALID_OPERATION for entry
    // points that require a GenTextures-created texture, e.g. TexStorage* ("An INVALID_OPERATION
    // error is generated if zero is bound to target", ARB_texture_storage).
    Bool ValidateTextureNotDefault(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                   const char* caller);
    Bool ValidateTextureTargetUniformity(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                         TextureTarget target);
    Bool ValidateTextureSubImageOffsets(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, Int xoffset,
                                        Int width, Int yoffset = 0, Int height = 0, Int zoffset = 0, Int depth = 0);
    // The texel block of one glCopyImageSubData endpoint, resolved to the two things the
    // compatibility rule actually asks about. `compressed` is not redundant with a block bigger
    // than 1x1: it is what distinguishes "compressed, and so the region is measured in texels of
    // a blocked image" from "uncompressed, and so it is measured in texels".
    struct CopyImageTexelBlock {
        SizeT byteSize = 0;
        Uint blockWidth = 1;
        Uint blockHeight = 1;
        Bool compressed = false;
    };
    // `compressedFormat` is the GLenum a glCompressedTexImage* upload recorded for the level, or
    // GL_NONE. It has to be asked for separately because MobileGL stores every compressed format
    // in uncompressed storage (ConvertGLEnumToTextureInternalFormat), so the TextureInternalFormat
    // alone can no longer tell a BPTC image from the RGBA8 backing it.
    CopyImageTexelBlock ResolveCopyImageTexelBlock(TextureInternalFormat format, GLenum compressedFormat);
    // GL 4.6 core 18.3.2: the two images must be COMPATIBLE, and compatible means their texel
    // blocks are the same SIZE - not that they share a base internal format. RGBA32UI into
    // RGBA32F is legal (both 128-bit) while RGBA8 into RGBA32F is not, and a compressed image
    // pairs with an uncompressed one whose texel is as big as the compressed block.
    Bool ValidateCopyImageFormatCompatibility(const CopyImageTexelBlock& srcBlock,
                                              const CopyImageTexelBlock& dstBlock);
    // GL 4.6 core 18.3.2: for a compressed image the region's origin must sit on a block
    // boundary and its size must be a whole number of blocks - unless the edge it runs to is
    // the edge of the image.
    Bool ValidateCopyImageBlockAlignment(const CopyImageTexelBlock& block, Int x, Int y, Int width, Int height,
                                         Int imageWidth, Int imageHeight, const char* endpointName);
    // GL 4.6 SS 8.6 subset rule for glCopyTexImage*: the read buffer must supply every component
    // the requested internalformat asks for, but may supply more.
    Bool ValidateCopyTexImageBaseFormatSubset(TextureInternalFormat destFormat, TextureInternalFormat srcFormat);
} // namespace MobileGL::MG_Impl::GLImpl::TextureImpl
