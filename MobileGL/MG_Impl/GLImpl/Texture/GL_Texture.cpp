// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/GL_Texture.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Texture.h"
#include "Config.h"
#include "MG_Util/Types.h"
#include "Validators.h"
#include "ProxyTexture.h"

#include <MG_State/GLState/Core.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Texture/PixelStoreProcessor.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <MG_Util/Classifiers/TextureEnumClassifier.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>

namespace MobileGL::MG_Impl::GLImpl {
    SharedPtr<MG_State::GLState::ITextureObject> GetTextureObjectByTarget(TextureUploadTarget textureUploadingTarget,
                                                                          TextureTarget textureTarget) {
        SharedPtr<MG_State::GLState::ITextureObject> textureObject = nullptr;
        if (TextureImpl::IsProxyTextureTarget(textureUploadingTarget)) {
            textureObject =
                TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadingTarget);
        } else {
            auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            textureObject = bindingSlot.GetBoundObject();
        }

        if (!TextureImpl::ValidateTextureObject(textureObject)) return nullptr;

        return textureObject;
    }

    void GenerateMipmap_Backend(GLenum target) {
        MG_Backend::gBackendFunctionsTable.GL.GenerateMipmap(target);
    }

    void TexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                             GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        // TODO: implement
    }

    void TexSubImage2D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, const void* pixels) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        //            TextureInternalFormat textureInternalFormat =
        //            MG_Util::ConvertGLEnumToTextureInternalFormat(format);
        MGLOG_D("TexSubImage2D_State: target = %s, level = %d, (%d, %d), format = %s, pixels = %p",
                MG_Util::ConvertGLEnumToString(target).c_str(), level, width, height,
                MG_Util::ConvertTextureInputFormatToString(textureInputFormat).c_str(), pixels);
        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadingTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadingTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadingTarget, level)) return;

        // TODO: GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the buffer object's data store is currently mapped.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the data would be unpacked from the buffer object such that the
        // memory reads required would exceed the data store size. GL_INVALID_OPERATION is generated if a
        // non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER target and data is not evenly
        // divisible into the number of bytes needed to store in memory a datum indicated by type.

        // ======================= Processing ================================
        auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto textureObject = bindingSlot.GetBoundObject();
        TextureInternalFormat textureInternalFormat = textureObject->GetFormat();
        MGLOG_D("%s: working on texture %d", __func__, textureObject->GetExternalIndex());

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width, yoffset, height)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;

        // ======================= Processing ================================
        // Texture object here should always be an object with mipmap
        // Assert this for extra safety.
        // This should automatically compiled out in release,
        // so that we don't take the perf hit of dyn-cast.
        MOBILEGL_ASSERT(nullptr != dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadingTarget, level);

        SizeT inputSize = 0;

        const void* originalPixels = pixels;

        // PBO
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            MGLOG_D("TexSubImage2D_State: Using Pixel Unpack Buffer Object ID: %u",
                    pixelUnpackBufferObject->GetExternalIndex());
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->GetDataReadOnly()->data()) +
                             reinterpret_cast<SizeT>(pixels);
        }

        if (!originalPixels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }
        const auto& unpackParams = MG_State::pGLContext->GetPixelStoreParameters(true);
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, unpackParams, textureInternalFormat, textureInputFormat, texturePixelDataType,
            {width, height, 1}, false, inputSize);

        if (!processedPixels || inputSize == 0) {
            MGLOG_E("TexSubImage2D_State: Failed to process pixel data for TexSubImage2D, width: %d, height: %d", width,
                    height);
            if (processedPixels) free(processedPixels);
            return;
        }

        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);

        const SizeT srcRowSize = static_cast<SizeT>(width) * internalBpp;
        const SizeT srcStride = (srcRowSize + unpackParams.Alignment - 1) & ~(unpackParams.Alignment - 1);
        const SizeT destRowSize = static_cast<SizeT>(texelSize.x()) * internalBpp;

        if (xoffset + width > static_cast<GLsizei>(texelSize.x()) ||
            yoffset + height > static_cast<GLsizei>(texelSize.y())) {
            MGLOG_E("TexSubImage2D_State: Specified region exceeds texture dimensions");
            free(processedPixels);
            return;
        }

        const auto* srcData = static_cast<const Uint8*>(processedPixels);
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadingTarget, level));

        if (destData) {
            for (GLsizei y = 0; y < height; y++) {
                const SizeT destRowOffset = (yoffset + y) * destRowSize + xoffset * internalBpp;
                const SizeT srcRowOffset = y * srcStride;
                Memcpy(destData + destRowOffset, srcData + srcRowOffset, srcRowSize);
            }
        }

        free(processedPixels);

        MGLOG_D("%s: mark mip %d as dirty", __func__, level);
        textureMipmapObject->MarkStorageDirty(textureUploadingTarget, level, true);
    }

    void TexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                             const GLvoid* pixels) {
        // TODO: implement
    }

    // TexParameteriv/TexParameterfv are introduced in OpenGL 4.0, so do not support them for now.
    void TexParameterf_State(GLenum target, GLenum pname, GLfloat param) {

        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        SharedPtr<MG_State::GLState::ITextureObject> textureObject =
            GetTextureObjectByTarget(textureUploadingTarget, textureTarget);
        if (!textureObject) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            textureObject->GetSamplerObject()->SetMagFilter(MG_Util::ConvertGLEnumToSamplerFilterMode(param));
            break;
        case GL_TEXTURE_MIN_FILTER:
            textureObject->GetSamplerObject()->SetMinFilter(MG_Util::ConvertGLEnumToSamplerFilterMode(param));
            textureObject->GetSamplerObject()->SetMipmapMode(MG_Util::ConvertGLEnumToSamplerMipmapMode(param));
            break;
        case GL_TEXTURE_MIN_LOD: {
            Float maxLod = textureObject->GetSamplerObject()->GetMaxLod();
            textureObject->GetSamplerObject()->SetLodRange(param, maxLod);
            break;
        }
        case GL_TEXTURE_MAX_LOD: {
            Float minLod = textureObject->GetSamplerObject()->GetMinLod();
            textureObject->GetSamplerObject()->SetLodRange(minLod, param);
            break;
        }
        case GL_TEXTURE_BASE_LEVEL:
            textureObject->SetBaseLevel(param);
            break;
        case GL_TEXTURE_MAX_LEVEL:
            textureObject->SetMaxLevel(param);
            break;
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A: {
            auto swizzleParam = MG_Util::ConvertGLEnumPnameToTextureSwizzleParam(pname);
            auto swizzleValue = MG_Util::ConvertGLEnumToTextureSwizzleParam(param);
            textureObject->SetSwizzleParam(swizzleParam, swizzleValue);
            break;
        }
        case GL_TEXTURE_WRAP_S:
            textureObject->GetSamplerObject()->SetWrapS(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_WRAP_T:
            textureObject->GetSamplerObject()->SetWrapT(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_WRAP_R:
            textureObject->GetSamplerObject()->SetWrapR(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_COMPARE_MODE:
            textureObject->GetSamplerObject()->SetCompareMode(MG_Util::ConvertGLEnumToSamplerCompareMode(param));
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            textureObject->GetSamplerObject()->SetSamplerCompareFunc(MG_Util::ConvertGLEnumToSamplerCompareFunc(param));
            break;
        case GL_TEXTURE_LOD_BIAS:
            textureObject->GetSamplerObject()->SetLodBias(param);
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            // Not supported in this function
        case GL_TEXTURE_BORDER_COLOR:
            // Not supported in this function
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void TexParameteri_State(GLenum target, GLenum pname, GLint param) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        SharedPtr<MG_State::GLState::ITextureObject> textureObject =
            GetTextureObjectByTarget(textureUploadingTarget, textureTarget);
        if (!textureObject) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            textureObject->GetSamplerObject()->SetMagFilter(MG_Util::ConvertGLEnumToSamplerFilterMode(param));
            break;
        case GL_TEXTURE_MIN_FILTER:
            textureObject->GetSamplerObject()->SetMinFilter(MG_Util::ConvertGLEnumToSamplerFilterMode(param));
            textureObject->GetSamplerObject()->SetMipmapMode(MG_Util::ConvertGLEnumToSamplerMipmapMode(param));
            break;
        case GL_TEXTURE_MIN_LOD: {
            Float maxLod = textureObject->GetSamplerObject()->GetMaxLod();
            textureObject->GetSamplerObject()->SetLodRange(param, maxLod);
            break;
        }
        case GL_TEXTURE_MAX_LOD: {
            Float minLod = textureObject->GetSamplerObject()->GetMinLod();
            textureObject->GetSamplerObject()->SetLodRange(minLod, param);
            break;
        }
        case GL_TEXTURE_BASE_LEVEL:
            textureObject->SetBaseLevel(param);
            break;
        case GL_TEXTURE_MAX_LEVEL:
            textureObject->SetMaxLevel(param);
            break;
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A: {
            auto swizzleParam = MG_Util::ConvertGLEnumPnameToTextureSwizzleParam(pname);
            auto swizzleValue = MG_Util::ConvertGLEnumToTextureSwizzleParam(param);
            textureObject->SetSwizzleParam(swizzleParam, swizzleValue);
            break;
        }
        case GL_TEXTURE_WRAP_S:
            textureObject->GetSamplerObject()->SetWrapS(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_WRAP_T:
            textureObject->GetSamplerObject()->SetWrapT(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_WRAP_R:
            textureObject->GetSamplerObject()->SetWrapR(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_COMPARE_MODE:
            textureObject->GetSamplerObject()->SetCompareMode(MG_Util::ConvertGLEnumToSamplerCompareMode(param));
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            textureObject->GetSamplerObject()->SetSamplerCompareFunc(MG_Util::ConvertGLEnumToSamplerCompareFunc(param));
            break;
        case GL_TEXTURE_LOD_BIAS:
            textureObject->GetSamplerObject()->SetLodBias((GLfloat)param);
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            // Not supported in this function
        case GL_TEXTURE_BORDER_COLOR:
            // Not supported in this function
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    // Quick and dirty TexParameter*v implementation to make NeoForge happy.
    // TODO: implement the missing part
    void TexParameterfv_State(GLenum target, GLenum pname, const GLfloat* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                GetTextureObjectByTarget(textureUploadingTarget, textureTarget);
            if (!textureObject) return;
            THROW_UNIMPL_EXCEPTION;
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                GetTextureObjectByTarget(textureUploadingTarget, textureTarget);
            if (!textureObject) return;

            Vec4<TextureSwizzleParam> swizzleParams;
            for (int i = 0; i < 4; i++) {
                swizzleParams[i] = MG_Util::ConvertGLEnumToTextureSwizzleParam(static_cast<GLint>(params[i]));
                if (TextureSwizzleParam::Unknown == swizzleParams[i]) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`params` is not valid."));
                    return;
                }
            }
            textureObject->SetSwizzleParamRGBA(swizzleParams);
            break;
        }
        default:
            TexParameterf_State(target, pname, *params);
            break;
        }
    }

    void TexParameteriv_State(GLenum target, GLenum pname, const GLint* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                GetTextureObjectByTarget(textureUploadingTarget, textureTarget);
            if (!textureObject) return;

            THROW_UNIMPL_EXCEPTION;
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                GetTextureObjectByTarget(textureUploadingTarget, textureTarget);
            if (!textureObject) return;

            Vec4<TextureSwizzleParam> swizzleParams;
            for (int i = 0; i < 4; i++) {
                swizzleParams[i] = MG_Util::ConvertGLEnumToTextureSwizzleParam(static_cast<GLint>(params[i]));
                if (TextureSwizzleParam::Unknown == swizzleParams[i]) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`params` is not valid."));
                    return;
                }
            }
            textureObject->SetSwizzleParamRGBA(swizzleParams);
            break;
        }
        default:
            TexParameteri_State(target, pname, *params);
            break;
        }
    }

    void TexParameterIiv_State(GLenum target, GLenum pname, const GLint* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            THROW_UNIMPL_EXCEPTION;
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                GetTextureObjectByTarget(textureUploadingTarget, textureTarget);
            if (!textureObject) return;

            Vec4<TextureSwizzleParam> swizzleParams;
            for (int i = 0; i < 4; i++) {
                swizzleParams[i] = MG_Util::ConvertGLEnumToTextureSwizzleParam(static_cast<GLint>(params[i]));
                if (TextureSwizzleParam::Unknown == swizzleParams[i]) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`params` is not valid."));
                    return;
                }
            }
            textureObject->SetSwizzleParamRGBA(swizzleParams);
            break;
        }
        default:
            TexParameteri_State(target, pname, *params);
            break;
        }
    }

    void TexParameterIuiv_State(GLenum target, GLenum pname, const GLuint* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            THROW_UNIMPL_EXCEPTION;
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                GetTextureObjectByTarget(textureUploadingTarget, textureTarget);
            if (!textureObject) return;

            Vec4<TextureSwizzleParam> swizzleParams;
            for (int i = 0; i < 4; i++) {
                swizzleParams[i] = MG_Util::ConvertGLEnumToTextureSwizzleParam(static_cast<GLint>(params[i]));
                if (TextureSwizzleParam::Unknown == swizzleParams[i]) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`params` is not valid."));
                    return;
                }
            }
            textureObject->SetSwizzleParamRGBA(swizzleParams);
            break;
        }
        default:
            TexParameteri_State(target, pname, static_cast<GLint>(*params));
            break;
        }
    }

    void TexImage3DMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
        // TODO: implement
    }

    void TexImage2DMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLboolean fixedsamplelocations) {
        // TODO: implement
    }

    void TexImage3D_State(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                          GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels) {
        MGLOG_D(
            "%s called with target: %s, level: %d, internalformat: %s, width: %d, height: %d, depth: %d, "
            "border: %d, format: %s, type: %s (%u), pixels: %p",
            __func__,
            MG_Util::ConvertTextureUploadTargetToString(MG_Util::ConvertGLEnumToTextureUploadTarget(target)).c_str(),
            level,
            MG_Util::ConvertTextureInternalFormatToString(MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat))
                .c_str(),
            width, height, depth, border,
            MG_Util::ConvertTextureInputFormatToString(MG_Util::ConvertGLEnumToTextureInputFormat(format)).c_str(),
            MG_Util::ConvertTexturePixelDataTypeToString(MG_Util::ConvertGLEnumToTexturePixelDataType(type)).c_str(),
            type, pixels);
        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadingTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadingTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, depth)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadingTarget, level)) return;

        // TODO: GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the buffer object's data store is currently mapped.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and the data would be unpacked from the buffer object such that the memory reads required would
        // exceed the data store size.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and data is not evenly divisible into the number of bytes needed to store in memory a datum
        // indicated by type.
        // ======================= Processing ================================
        SharedPtr<MG_State::GLState::ITextureObject> textureObject = nullptr;
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadingTarget);
        if (isProxy) {
            textureObject =
                TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadingTarget);
        } else {
            auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            textureObject = bindingSlot.GetBoundObject();
        }

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        // ======================= Processing ================================

        SizeT imageSize = 0;
        const SizeT inputBpp = MG_Util::GetInputBytesPerPixel(textureInputFormat, texturePixelDataType);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);
        const SizeT internalBytes = width * height * depth * internalBpp;

        textureObject->SetInternalFormat(textureInternalFormat);

        // if isProxy, no more pixel transfer needed below
        if (isProxy) return;

        const void* originalPixels = pixels;

        // PBO
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            MGLOG_D("%s: Using Pixel Unpack Buffer Object ID: %u", __func__,
                    pixelUnpackBufferObject->GetExternalIndex());
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->GetDataReadOnly()->data()) +
                             reinterpret_cast<SizeT>(pixels);
        }

        MOBILEGL_ASSERT(nullptr != dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        // Allocate in TextureObject
        textureMipmapObject->AllocateStorage(textureUploadingTarget, level, {{width, height, depth}, internalBytes});

        if (!originalPixels) {
            MGLOG_D("%s: No input pixel and no PBO bound, no pixel transfer", __func__);
            return;
        }

        void* processedPixels = nullptr;
        processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureInternalFormat,
            textureInputFormat, texturePixelDataType, {width, height, depth}, false, imageSize);

        if (processedPixels && imageSize > 0) {
            if (imageSize != internalBytes) {
                MGLOG_W("%s: Processed pixel data size (%zu) does not match expected size (%zu). "
                        "This may indicate an alignment or processing issue.",
                        __func__, imageSize, internalBytes);
            }

            const SizeT copySize = std::min(imageSize, internalBytes);
            DataPtr texelInput{processedPixels, copySize};
            textureMipmapObject->UpdateMipmapSubData(textureUploadingTarget, level, texelInput);
        }

        textureMipmapObject->MarkStorageDirty(textureUploadingTarget, level, true);

        free(processedPixels);
    }

    void TexImage2D_State(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                          GLenum format, GLenum type, const void* pixels) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        MGLOG_D("%s called with target: %s (%s), level: %d, internalformat: %s (%s), width: %d, height: %d, "
                "border: %d, format: %s (%s), type: %s (%s), pixels: %p",
                __func__, MG_Util::ConvertTextureUploadTargetToString(textureUploadingTarget).c_str(),
                MG_Util::ConvertGLEnumToString(target).c_str(), level,
                MG_Util::ConvertTextureInternalFormatToString(textureInternalFormat).c_str(),
                MG_Util::ConvertGLEnumToString(internalformat).c_str(), width, height, border,
                MG_Util::ConvertTextureInputFormatToString(textureInputFormat).c_str(),
                MG_Util::ConvertGLEnumToString(format).c_str(),
                MG_Util::ConvertTexturePixelDataTypeToString(texturePixelDataType).c_str(),
                MG_Util::ConvertGLEnumToString(type).c_str(), pixels);
        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadingTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadingTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadingTarget, level)) return;

        // TODO: GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the buffer object's data store is currently mapped.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and the data would be unpacked from the buffer object such that the memory reads required would
        // exceed the data store size.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and data is not evenly divisible into the number of bytes needed to store in memory a datum
        // indicated by type.

        // ======================= Processing ================================
        textureInternalFormat =
            MG_Util::ConvertInternalFormatToSized(textureInternalFormat, textureInputFormat, texturePixelDataType);
        SharedPtr<MG_State::GLState::ITextureObject> textureObject = nullptr;
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadingTarget);
        if (isProxy) {
            textureObject =
                TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadingTarget);
        } else {
            auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            textureObject = bindingSlot.GetBoundObject();
        }

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        // ======================= Processing ================================

        SizeT imageSize = 0;
        const SizeT inputBpp = MG_Util::GetInputBytesPerPixel(textureInputFormat, texturePixelDataType);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);
        const SizeT internalBytes = width * height * internalBpp;

        MGLOG_D("%s: working on texture %d", __func__, textureObject->GetExternalIndex());

        MGLOG_D("%s: texture object had internal format %s, new format %s", __func__,
                MG_Util::ConvertTextureInternalFormatToString(textureObject->GetFormat()).c_str(),
                MG_Util::ConvertTextureInternalFormatToString(textureInternalFormat).c_str());
        textureObject->SetInternalFormat(textureInternalFormat);

        // if isProxy, no more pixel transfer needed below
        if (isProxy) return;

        const void* originalPixels = pixels;

        // PBO
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            MGLOG_D("%s: Using Pixel Unpack Buffer Object ID: %u", __func__,
                    pixelUnpackBufferObject->GetExternalIndex());
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->GetDataReadOnly()->data()) +
                             reinterpret_cast<SizeT>(pixels);
        }

        MOBILEGL_ASSERT(nullptr != dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        // Allocate in TextureObject
        MGLOG_D("%s: Allocating %d bytes at mip %d", __func__, internalBytes, level);
        textureMipmapObject->AllocateStorage(textureUploadingTarget, level, {{width, height, 1}, internalBytes});

        if (!originalPixels) {
            MGLOG_D("%s: No input pixel and no PBO bound, no pixel transfer", __func__);
            return;
        }

        void* processedPixels = nullptr;
        processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureInternalFormat,
            textureInputFormat, texturePixelDataType, {width, height, 1}, false, imageSize);

        if (processedPixels && imageSize > 0) {
            if (imageSize != internalBytes) {
                MGLOG_W("TexImage2D_State: Processed pixel data size (%zu) does not match expected size (%zu). "
                        "This may indicate an alignment or processing issue.",
                        imageSize, internalBytes);
            }

            const SizeT copySize = std::min(imageSize, internalBytes);
            DataPtr texelInput{processedPixels, copySize};
            textureMipmapObject->UpdateMipmapSubData(textureUploadingTarget, level, texelInput);
        }

        free(processedPixels);

        MGLOG_D("%s: mark mip %d as dirty", __func__, level);
        textureMipmapObject->MarkStorageDirty(textureUploadingTarget, level, true);
    }

    void TexImage1D_State(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format,
                          GLenum type, const GLvoid* pixels) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void TexBuffer_State(GLenum target, GLenum internalformat, GLuint buffer) {
        // ======================= Converting ================================
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadingTarget)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        // TODO: make sure `internalformat` is in one of supported format for TexBuffer
        auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "`buffer` is not zero and is not the name of an existing buffer object."));
            return;
        }

        // ======================= Processing ================================
        auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto textureObject = bindingSlot.GetBoundObject();

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Buffer) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "The effective target of `texture` is not `GL_TEXTURE_BUFFER`."));
            return;
        }

        // ======================= Processing ================================
        // Now we can rest assured, and down-cast texture object to texture buffer
        auto* texBufferObject = static_cast<MG_State::GLState::TextureObjectBuffer*>(textureObject.get());
        auto& bufferSlot = texBufferObject->GetBufferBindingSlot();
        bufferSlot.Bind(bufferObject);

        texBufferObject->SetInternalFormat(textureInternalFormat);
    }

    GLboolean IsTexture_State(GLuint texture) {
        // ======================= Processing ================================
        if (!TextureImpl::ValidateTextureName(texture, true)) return GL_FALSE;
        return MG_State::pGLContext->ValidateTextureObject(texture) ? GL_TRUE : GL_FALSE;
    }

    void GetTexParameterIuiv_State(GLenum target, GLenum pname, GLuint* params) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void GetTexParameterIiv_State(GLenum target, GLenum pname, GLint* params) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void GetTexParameteriv_State(GLenum target, GLenum pname, GLint* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        SharedPtr<MG_State::GLState::ITextureObject> textureObject = nullptr;
        if (TextureImpl::IsProxyTextureTarget(textureUploadingTarget)) {
            textureObject = TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadingTarget);
        } else {
            auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            textureObject = bindingSlot.GetBoundObject();
        }

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            if (params) {
                *params = MG_Util::ConvertSamplerFilterModeToGLEnum(textureObject->GetSamplerObject()->GetMagFilter(),
                                                                    SamplerMipmapMode::None);
            }
            break;
        case GL_TEXTURE_MIN_FILTER:
            if (params) {
                *params = MG_Util::ConvertSamplerFilterModeToGLEnum(textureObject->GetSamplerObject()->GetMinFilter(),
                                                                    textureObject->GetSamplerObject()->GetMipmapMode());
            }
            break;
        case GL_TEXTURE_MIN_LOD:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMinLod());
            }
            break;
        case GL_TEXTURE_MAX_LOD:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMaxLod());
            }
            break;
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_BORDER_COLOR:
            break; // TODO
        case GL_TEXTURE_WRAP_S:
            if (params) {
                *params = MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapS());
            }
            break;
        case GL_TEXTURE_WRAP_T:
            if (params) {
                *params = MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapT());
            }
            break;
        case GL_TEXTURE_WRAP_R:
            if (params) {
                *params = MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapR());
            }
            break;
        case GL_TEXTURE_COMPARE_MODE:
            if (params) {
                *params =
                    MG_Util::ConvertSamplerCompareModeToGLEnum(textureObject->GetSamplerObject()->GetCompareMode());
            }
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            if (params) {
                *params = MG_Util::ConvertSamplerCompareFuncToGLEnum(
                    textureObject->GetSamplerObject()->GetSamplerCompareFunc());
            }
            break;
        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexParameteriv_State",
                                                                           "pname is not a valid texture parameter."));
            return;
        }
    }

    void GetTexParameterfv_State(GLenum target, GLenum pname, GLfloat* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        SharedPtr<MG_State::GLState::ITextureObject> textureObject = nullptr;
        if (TextureImpl::IsProxyTextureTarget(textureUploadingTarget)) {
            textureObject = TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadingTarget);
        } else {
            auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            textureObject = bindingSlot.GetBoundObject();
        }

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            if (params) {
                *params = MG_Util::ConvertSamplerFilterModeToGLEnum(textureObject->GetSamplerObject()->GetMagFilter(),
                                                                    SamplerMipmapMode::None);
            }
            break;
        case GL_TEXTURE_MIN_FILTER:
            if (params) {
                *params = MG_Util::ConvertSamplerFilterModeToGLEnum(textureObject->GetSamplerObject()->GetMinFilter(),
                                                                    textureObject->GetSamplerObject()->GetMipmapMode());
            }
            break;
        case GL_TEXTURE_MIN_LOD:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetSamplerObject()->GetMinLod());
            }
            break;
        case GL_TEXTURE_MAX_LOD:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetSamplerObject()->GetMaxLod());
            }
            break;
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A:
        case GL_TEXTURE_SWIZZLE_RGBA:
        case GL_TEXTURE_BORDER_COLOR:
            break; // TODO
        case GL_TEXTURE_WRAP_S:
            if (params) {
                *params = MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapS());
            }
            break;
        case GL_TEXTURE_WRAP_T:
            if (params) {
                *params = MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapT());
            }
            break;
        case GL_TEXTURE_WRAP_R:
            if (params) {
                *params = MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapR());
            }
            break;
        case GL_TEXTURE_COMPARE_MODE:
            if (params) {
                *params =
                    MG_Util::ConvertSamplerCompareModeToGLEnum(textureObject->GetSamplerObject()->GetCompareMode());
            }
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            if (params) {
                *params = MG_Util::ConvertSamplerCompareFuncToGLEnum(
                    textureObject->GetSamplerObject()->GetSamplerCompareFunc());
            }
            break;
        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexParameterfv_State",
                                                                           "pname is not a valid texture parameter."));
            return;
        }
    }

    void GetTexLevelParameteriv_State(GLenum target, GLint level, GLenum pname, GLint* params) {
        MGLOG_D("GetTexLevelParameteriv_State called");
        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadingTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadingTarget, level)) return;

        // ======================= Processing ================================
        SharedPtr<MG_State::GLState::ITextureObject> textureObject = nullptr;

        if (TextureImpl::IsProxyTextureTarget(textureUploadingTarget)) {
            textureObject = TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadingTarget);
        } else {
            auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            textureObject = bindingSlot.GetBoundObject();
        }

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_WIDTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadingTarget, level).x();
                    break;
                }
                default:
                    THROW_UNIMPL_EXCEPTION;
                }
            }
            break;
        case GL_TEXTURE_HEIGHT:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadingTarget, level).y();
                    break;
                }
                default:
                    THROW_UNIMPL_EXCEPTION;
                }
            }
            break;
        case GL_TEXTURE_DEPTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadingTarget, level).z();
                    break;
                }
                default:
                    THROW_UNIMPL_EXCEPTION;
                }
            }
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            if (params) {
                *params = MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
            }
            break;
        case GL_TEXTURE_RED_TYPE:
        case GL_TEXTURE_GREEN_TYPE:
        case GL_TEXTURE_BLUE_TYPE:
        case GL_TEXTURE_ALPHA_TYPE:
        case GL_TEXTURE_DEPTH_TYPE:
        case GL_TEXTURE_RED_SIZE:
        case GL_TEXTURE_GREEN_SIZE:
        case GL_TEXTURE_BLUE_SIZE:
        case GL_TEXTURE_ALPHA_SIZE:
        case GL_TEXTURE_DEPTH_SIZE:
        case GL_TEXTURE_COMPRESSED:
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
            break; // TODO
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexLevelParameteriv_State",
                                                                     "pname is not a valid texture level parameter."));
            return;
        }
    }

    void GetTexLevelParameterfv_State(GLenum target, GLint level, GLenum pname, GLfloat* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadingTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadingTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadingTarget, level)) return;

        // ======================= Processing ================================
        SharedPtr<MG_State::GLState::ITextureObject> textureObject = nullptr;
        if (TextureImpl::IsProxyTextureTarget(textureUploadingTarget)) {
            textureObject = TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadingTarget);
        } else {
            auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            textureObject = bindingSlot.GetBoundObject();
        }

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_WIDTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadingTarget, level).x();
                    break;
                }
                default:
                    THROW_UNIMPL_EXCEPTION;
                }
            }
            break;
        case GL_TEXTURE_HEIGHT:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadingTarget, level).y();
                    break;
                }
                default:
                    THROW_UNIMPL_EXCEPTION;
                }
            }
            break;
        case GL_TEXTURE_DEPTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadingTarget, level).z();
                    break;
                }
                default:
                    THROW_UNIMPL_EXCEPTION;
                }
            }
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            if (params) {
                *params = MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
            }
            break;
        case GL_TEXTURE_RED_TYPE:
        case GL_TEXTURE_GREEN_TYPE:
        case GL_TEXTURE_BLUE_TYPE:
        case GL_TEXTURE_ALPHA_TYPE:
        case GL_TEXTURE_DEPTH_TYPE:
        case GL_TEXTURE_RED_SIZE:
        case GL_TEXTURE_GREEN_SIZE:
        case GL_TEXTURE_BLUE_SIZE:
        case GL_TEXTURE_ALPHA_SIZE:
        case GL_TEXTURE_DEPTH_SIZE:
        case GL_TEXTURE_COMPRESSED:
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
            break; // TODO
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexLevelParameterfv_State",
                                                                     "pname is not a valid texture level parameter."));
            return;
        }
    }

    void GetCompressedTexImage_State(GLenum target, GLint level, void* img) {
        // TODO: implement
    }

    void GenTextures_State(GLsizei n, GLuint* textures) {
        // ===================== Error Checking ==============================
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GenTextures_State", "n must be non-negative"));
            return;
        }

        // ======================= Processing ================================
        auto textureNames = MG_State::pGLContext->GenTextureNames(n);
        Copy(textureNames.data(), textures, textureNames.size());
    }

    void DeleteTextures_State(GLsizei n, const GLuint* textures) {
        // ===================== Error Checking ==============================
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteTextures_State", "n must be non-negative."));
            return;
        }

        if (!textures) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteTextures_State",
                                                                           "Texture names array cannot be null."));
            return;
        }

        // ======================= Processing ================================
        for (SizeT i = 0; i < static_cast<SizeT>(n); ++i) {
            Uint textureName = textures[i];
            if (textureName == 0) continue;
            if (!TextureImpl::ValidateTextureName(textureName, true)) continue;
            MG_State::pGLContext->MarkTextureObjectForDeletion(textureName);
        }
    }

    void CopyTexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                                 GLint y, GLsizei width, GLsizei height) {
        // TODO: implement
    }

    void CopyTexSubImage2D_Backend(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                   GLsizei width, GLsizei height) {
        MG_Backend::gBackendFunctionsTable.GL.CopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    }

    void CopyTexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
        // TODO: implement
    }

    void CopyTexImage2D_State(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                              GLsizei height, GLint border) {
        auto internalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        const auto& currentReadFBO =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
        if (!currentReadFBO) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "CopyTexImage2D_State",
                                             "No framebuffer is currently bound to the GL_READ_FRAMEBUFFER target."));
            return;
        }

        Bool isDepth = MG_Util::IsDepthFormatInternalFormat(internalFormat);
        Bool isStencil = MG_Util::IsStencilFormatInternalFormat(internalFormat);
        TextureInternalFormat srcInternalFormat = TextureInternalFormat::Unknown;
#define GET_SRC_INTERNAL_FORMAT(AttachmentType)                                                                        \
    const auto& srcAttachment = currentReadFBO->GetAttachment(AttachmentType);                                         \
    if (srcAttachment.IsTexture()) {                                                                                   \
        const auto& texObj = srcAttachment.GetTexture();                                                               \
        srcInternalFormat = texObj->GetFormat();                                                                       \
    } else if (srcAttachment.IsRenderbuffer()) {                                                                       \
        const auto& rboObj = srcAttachment.GetRenderbuffer();                                                          \
        srcInternalFormat = rboObj->GetInternalFormat();                                                               \
    } else {                                                                                                           \
        MG_State::pGLContext->RecordError(                                                                             \
            ErrorCode::InvalidOperation,                                                                               \
            MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "CopyTexImage2D_State",                                     \
                                         "The attachment specified by the read buffer is incomplete."));               \
        return;                                                                                                        \
    }
        if (isDepth) {
            GET_SRC_INTERNAL_FORMAT(FramebufferAttachmentType::Depth);
        } else if (isStencil) {
            GET_SRC_INTERNAL_FORMAT(FramebufferAttachmentType::Stencil);
        } else {
            const auto& readBufferType = currentReadFBO->GetReadBuffer();
            GET_SRC_INTERNAL_FORMAT(readBufferType);
        }

        if (!TextureImpl::ValidateBaseInternalFormatMatch(internalFormat, srcInternalFormat)) THROW_UNIMPL_EXCEPTION;

        GLenum outInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(srcInternalFormat);
        GLenum realInternalFormat = GL_RGBA8;
        GLenum format = GL_DEPTH_COMPONENT;
        GLenum type = GL_UNSIGNED_INT;
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(outInternalFormat, PixelFormatNormalizeOptionBit::None,
                                                              &realInternalFormat, &format, &type);
        const auto pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        TexImage2D_State(target, level, realInternalFormat, width, height, border, format, type, nullptr);
        MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).Bind(pixelUnpackBufferObject);
    }

    void CopyTexImage2D_Backend(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                                GLsizei height, GLint border) {
        MG_Backend::gBackendFunctionsTable.GL.CopyTexImage2D(target, level, internalformat, x, y, width, height,
                                                             border);
    }

    void CopyTexImage1D_State(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                              GLint border) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void CompressedTexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                       GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize,
                                       const void* data) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void CompressedTexSubImage2D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                       GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void CompressedTexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format,
                                       GLsizei imageSize, const void* data) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void CompressedTexImage3D_State(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                                    GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void CompressedTexImage2D_State(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                                    GLint border, GLsizei imageSize, const void* data) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void CompressedTexImage1D_State(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border,
                                    GLsizei imageSize, const void* data) {
        // TODO: implement
        THROW_UNIMPL_EXCEPTION;
    }

    void BindTexture_State(GLenum target, GLuint texture) {
        // TODO: deal with condition where texture == 0
        MGLOG_D("BindTexture_State called with target: 0x%X, texture: %u", target, texture);
        // ======================= Converting ================================
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureName(texture, true)) return;
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;

        // ======================= Processing ================================
        auto textureObject = MG_State::pGLContext->GetTextureObject(texture);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureTargetUniformity(textureObject, textureTarget)) return;

        // ======================= Processing ================================
        if (!textureObject) {
            textureObject = MG_State::pGLContext->CreateTextureObject(texture, textureTarget);
        }

        auto& currentUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = currentUnit.GetBindingSlot(textureTarget);
        bindingSlot.Bind(textureObject);
    }

    void ActiveTexture_State(GLenum texture) {
        // ===================== Error Checking ==============================
        if (texture < GL_TEXTURE0 || texture > GL_TEXTURE31) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ActiveTexture_State",
                    std::format("Texture must be one of GL_TEXTUREi, where i is in the range 0 to 31, but got "
                                "invalid enum: 0x{:X}, which may stand for unit {}.",
                                texture, texture - GL_TEXTURE0)));
            return;
        }

        // ======================= Processing ================================
        MG_State::pGLContext->SetActiveTextureUnit((Int)texture - GL_TEXTURE0);
    }

    void GetTexImage_Backend(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        MG_Backend::gBackendFunctionsTable.GL.GetTexImage(target, level, format, type, pixels);
    }

    // Add to GL_Texture.cpp
    void GetTexImage_State(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        // ===================== Error Checking ==============================
        // Validate target
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid texture target"));
            return;
        }

        // Validate level
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Level must be non-negative"));
            return;
        }

        // Validate format
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid format"));
            return;
        }

        // Validate type
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid pixel data type"));
            return;
        }

        // Get texture object
        SharedPtr<MG_State::GLState::ITextureObject> textureObject = nullptr;
        if (TextureImpl::IsProxyTextureTarget(textureUploadTarget)) {
            textureObject = TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget);
        } else {
            auto activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            textureObject = bindingSlot.GetBoundObject();
        }

        if (!TextureImpl::ValidateTextureObject(textureObject)) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                                           "No valid texture bound to target"));
            return;
        }

        // Check texture completeness
        if (!textureObject->IsComplete()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Texture is incomplete"));
            return;
        }

        // Check PBO state
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();

        if (pixelPackBufferObject) {
            // Check if PBO is mapped
            if (pixelPackBufferObject->IsMapped()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation, MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                                              "Pixel pack buffer is currently mapped"));
                return;
            }

            // Check alignment
            const SizeT typeSize = MG_Util::GetTexturePixelDataTypeSize(texturePixelDataType);
            if (reinterpret_cast<uintptr_t>(pixels) % typeSize != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                 "Pixel data not aligned for pixel pack buffer"));
                return;
            }
        }

        // Special case for depth/stencil
        if (textureInputFormat == TextureInputFormat::StencilIndex) {
            if (textureObject->GetFormat() != TextureInternalFormat::DepthStencil &&
                textureObject->GetFormat() != TextureInternalFormat::Depth24Stencil8 &&
                textureObject->GetFormat() != TextureInternalFormat::Depth32FStencil8) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                 "No stencil buffer for stencil index format"));
                return;
            }
        }

        // Check for multisampling
        if (textureObject->GetStorageType() == TextureStorageType::Mipmap) {
            auto mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
            if (mipmapObject->GetMipmapLevelCount() > 1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                 "Multisampled textures not supported for GetTexImage"));
                return;
            }
        }
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void GenerateMipmap(GLenum target) {
        GenerateMipmap_Backend(target);
    }

    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        GetTexImage_State(target, level, format, type, pixels);
        GetTexImage_Backend(target, level, format, type, pixels);
    }

    void TexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                       GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        TexSubImage3D_State(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
    }

    void TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels) {
        TexSubImage2D_State(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }

    void TexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                       const GLvoid* pixels) {
        TexSubImage1D_State(target, level, xoffset, width, format, type, pixels);
    }

    void TexParameterf(GLenum target, GLenum pname, GLfloat param) {
        TexParameterf_State(target, pname, param);
    }

    void TexParameteri(GLenum target, GLenum pname, GLint param) {
        TexParameteri_State(target, pname, param);
    }

    void TexParameterfv(GLenum target, GLenum pname, const GLfloat* params) {
        TexParameterfv_State(target, pname, params);
    }

    void TexParameteriv(GLenum target, GLenum pname, const GLint* params) {
        TexParameteriv_State(target, pname, params);
    }

    void TexParameterIiv(GLenum target, GLenum pname, const GLint* params) {
        TexParameterIiv_State(target, pname, params);
    }

    void TexParameterIuiv(GLenum target, GLenum pname, const GLuint* params) {
        TexParameterIuiv_State(target, pname, params);
    }

    void TexImage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,
                               GLsizei depth, GLboolean fixedsamplelocations) {
        TexImage3DMultisample_State(target, samples, internalformat, width, height, depth, fixedsamplelocations);
    }

    void TexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,
                               GLboolean fixedsamplelocations) {
        TexImage2DMultisample_State(target, samples, internalformat, width, height, fixedsamplelocations);
    }

    void TexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth,
                    GLint border, GLenum format, GLenum type, const void* pixels) {
        TexImage3D_State(target, level, internalformat, width, height, depth, border, format, type, pixels);
    }

    void TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels) {
        TexImage2D_State(target, level, internalformat, width, height, border, format, type, pixels);
    }

    void TexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format,
                    GLenum type, const GLvoid* pixels) {
        TexImage1D_State(target, level, internalFormat, width, border, format, type, pixels);
    }

    void TexBuffer(GLenum target, GLenum internalformat, GLuint buffer) {
        TexBuffer_State(target, internalformat, buffer);
    }

    GLboolean IsTexture(GLuint texture) {
        return IsTexture_State(texture);
    }

    void GetTexParameterIuiv(GLenum target, GLenum pname, GLuint* params) {
        GetTexParameterIuiv_State(target, pname, params);
    }

    void GetTexParameterIiv(GLenum target, GLenum pname, GLint* params) {
        GetTexParameterIiv_State(target, pname, params);
    }

    void GetTexParameteriv(GLenum target, GLenum pname, GLint* params) {
        GetTexParameteriv_State(target, pname, params);
    }

    void GetTexParameterfv(GLenum target, GLenum pname, GLfloat* params) {
        GetTexParameterfv_State(target, pname, params);
    }

    void GetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params) {
        GetTexLevelParameteriv_State(target, level, pname, params);
    }

    void GetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat* params) {
        GetTexLevelParameterfv_State(target, level, pname, params);
    }

    void GetCompressedTexImage(GLenum target, GLint level, void* img) {
        GetCompressedTexImage_State(target, level, img);
    }

    void GenTextures(GLsizei n, GLuint* textures) {
        GenTextures_State(n, textures);
    }

    void DeleteTextures(GLsizei n, const GLuint* textures) {
        DeleteTextures_State(n, textures);
    }

    void CopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y,
                           GLsizei width, GLsizei height) {
        CopyTexSubImage3D_State(target, level, xoffset, yoffset, zoffset, x, y, width, height);
    }

    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height) {
        CopyTexSubImage2D_Backend(target, level, xoffset, yoffset, x, y, width, height);
    }

    void CopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
        CopyTexSubImage1D_State(target, level, xoffset, x, y, width);
    }

    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border) {
        CopyTexImage2D_State(target, level, internalformat, x, y, width, height, border);
        CopyTexImage2D_Backend(target, level, internalformat, x, y, width, height, border);
    }

    void CopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLint border) {
        CopyTexImage1D_State(target, level, internalformat, x, y, width, border);
    }

    void CompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                                 GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data) {
        CompressedTexSubImage3D_State(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize,
                                      data);
    }

    void CompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                 GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
        CompressedTexSubImage2D_State(target, level, xoffset, yoffset, width, height, format, imageSize, data);
    }

    void CompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format,
                                 GLsizei imageSize, const void* data) {
        CompressedTexSubImage1D_State(target, level, xoffset, width, format, imageSize, data);
    }

    void CompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
        CompressedTexImage3D_State(target, level, internalformat, width, height, depth, border, imageSize, data);
    }

    void CompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLint border, GLsizei imageSize, const void* data) {
        CompressedTexImage2D_State(target, level, internalformat, width, height, border, imageSize, data);
    }

    void CompressedTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border,
                              GLsizei imageSize, const void* data) {
        CompressedTexImage1D_State(target, level, internalformat, width, border, imageSize, data);
    }

    void BindTexture(GLenum target, GLuint texture) {
        BindTexture_State(target, texture);
    }

    void ActiveTexture(GLenum texture) {
        ActiveTexture_State(texture);
    }

} // namespace MobileGL::MG_Impl::GLImpl
