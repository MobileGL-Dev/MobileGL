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
    Bool ValidateTextureInternalFormatCompatibleWithInput(TextureInputFormat format,
                                                          TextureInternalFormat internalFormat,
                                                          TexturePixelDataType type);
    Bool ValidateTextureLevelWithUploadTarget(TextureUploadTarget target, Int level);
    Bool ValidateTextureObject(SharedPtr<MG_State::GLState::ITextureObject> textureObject);
    Bool ValidateTextureTargetUniformity(SharedPtr<MG_State::GLState::ITextureObject> textureObject,
                                         TextureTarget target);
    Bool ValidateTextureSubImageOffsets(SharedPtr<MG_State::GLState::ITextureObject> textureObject, Int xoffset,
                                        Int width, Int yoffset = 0, Int height = 0, Int zoffset = 0, Int depth = 0);
    Bool ValidateBaseInternalFormatMatch(TextureInternalFormat format1, TextureInternalFormat format2);
} // namespace MobileGL::MG_Impl::GLImpl::TextureImpl
