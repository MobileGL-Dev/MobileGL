// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/GL_Texture.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Texture.h"
#include "Config.h"
#include "MG_State/GLState/TextureState/TextureObject2D.h"
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
    static SharedPtr<MG_State::GLState::ITextureObject> nullTextureObject;
    static UnorderedMap<Uint, Bool> g_autoGenerateMipmapByTextureId;

    Bool GetTexParameteriv_State(GLenum target, GLenum pname, GLint* params);

    namespace {
        void SetTextureBorderColorFromFloats(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             const GLfloat* params) {
            textureObject->SetBorderColor(FloatVec4(params[0], params[1], params[2], params[3]));
        }

        void SetTextureBorderColorFromInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                           const GLint* params) {
            textureObject->SetBorderColor(FloatVec4(static_cast<Float>(params[0]), static_cast<Float>(params[1]),
                                                    static_cast<Float>(params[2]), static_cast<Float>(params[3])));
        }

        void SetTextureBorderColorFromIntegerInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                  const GLint* params) {
            textureObject->SetBorderColorI(IntVec4(params[0], params[1], params[2], params[3]));
        }

        void SetTextureBorderColorFromUnsignedInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                   const GLuint* params) {
            textureObject->SetBorderColorUI(UintVec4(params[0], params[1], params[2], params[3]));
        }

        Bool SetTextureSwizzleParamsFromInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             const GLint* params, const char* caller) {
            Vec4<TextureSwizzleParam> swizzleParams;
            for (int i = 0; i < 4; ++i) {
                swizzleParams[i] = MG_Util::ConvertGLEnumToTextureSwizzleParam(params[i]);
                if (TextureSwizzleParam::Unknown == swizzleParams[i]) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "`params` is not valid."));
                    return false;
                }
            }
            textureObject->SetSwizzleParamRGBA(swizzleParams);
            return true;
        }

        template <typename Fn>
        void WithTemporarilyBoundNamedTexture(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             Fn&& fn) {
            if (!textureObject) return;

            auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureObject->GetTarget());
            const auto previousBinding = bindingSlot.GetBoundObject();
            bindingSlot.Bind(textureObject);
            fn(MG_Util::ConvertTextureTargetToGLEnum(textureObject->GetTarget()));
            bindingSlot.Bind(previousBinding);
        }

        SizeT ComputeTextureStorageByteSize(TextureInternalFormat textureInternalFormat, GLsizei width, GLsizei height,
                                            GLsizei depth) {
            GLenum realInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat);
            GLenum realFormat = GL_RGBA;
            GLenum realType = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                realInternalFormat, PixelFormatNormalizeOptionBit::None, &realInternalFormat, &realFormat, &realType);
            return static_cast<SizeT>(width) * static_cast<SizeT>(height) * static_cast<SizeT>(depth) *
                   MG_Util::GetInternalBytesPerPixel(textureInternalFormat,
                                                     MG_Util::ConvertGLEnumToTexturePixelDataType(realType));
        }

        GLint GetTextureComponentType(TextureInternalFormat textureInternalFormat, GLint size, Bool depthComponent,
                                      Bool stencilComponent) {
            if (size <= 0) return GL_NONE;
            if (stencilComponent) return GL_UNSIGNED_INT;
            if (depthComponent) {
                return (textureInternalFormat == TextureInternalFormat::DepthComponent32F ||
                        textureInternalFormat == TextureInternalFormat::Depth32FStencil8)
                           ? GL_FLOAT
                           : GL_UNSIGNED_NORMALIZED;
            }
            switch (textureInternalFormat) {
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
                return GL_INT;
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
                return GL_UNSIGNED_INT;
            case TextureInternalFormat::R16F:
            case TextureInternalFormat::RG16F:
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGBA16F:
            case TextureInternalFormat::R32F:
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::R11FG11FB10F:
            case TextureInternalFormat::RGB9E5:
                return GL_FLOAT;
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return GL_SIGNED_NORMALIZED;
            default:
                return GL_UNSIGNED_NORMALIZED;
            }
        }

        GLint GetTextureLevelComponentParameter(TextureInternalFormat textureInternalFormat, GLenum pname) {
            const ComponentSizes componentSizes = MG_Util::GetComponentSizesForInternalFormat(textureInternalFormat);
            switch (pname) {
            case GL_TEXTURE_RED_SIZE:
                return componentSizes.Red;
            case GL_TEXTURE_GREEN_SIZE:
                return componentSizes.Green;
            case GL_TEXTURE_BLUE_SIZE:
                return componentSizes.Blue;
            case GL_TEXTURE_ALPHA_SIZE:
                return componentSizes.Alpha;
            case GL_TEXTURE_DEPTH_SIZE:
                return componentSizes.Depth;
            case GL_TEXTURE_STENCIL_SIZE:
                return componentSizes.Stencil;
            case GL_TEXTURE_RED_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Red, false, false);
            case GL_TEXTURE_GREEN_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Green, false, false);
            case GL_TEXTURE_BLUE_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Blue, false, false);
            case GL_TEXTURE_ALPHA_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Alpha, false, false);
            case GL_TEXTURE_DEPTH_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Depth, true, false);
            default:
                MOBILEGL_ASSERT(false, "Invalid texture level component pname: %d", pname);
                return 0;
            }
        }

        Uint ComputeFullMipmapLevelCount(const IntVec3& baseTexelSize) {
            Int maxDimension = std::max<Int>(
                baseTexelSize.x(),
                std::max<Int>(baseTexelSize.y(), std::max<Int>(baseTexelSize.z(), 1)));
            Uint mipLevelCount = 1;
            while (maxDimension > 1) {
                maxDimension = std::max<Int>(maxDimension / 2, 1);
                ++mipLevelCount;
            }
            return mipLevelCount;
        }

        IntVec3 ComputeMipmapTexelSize(const IntVec3& baseTexelSize, Uint relativeLevel) {
            return {
                std::max<Int>(baseTexelSize.x() >> static_cast<Int>(relativeLevel), 1),
                std::max<Int>(baseTexelSize.y() >> static_cast<Int>(relativeLevel), 1),
                std::max<Int>(baseTexelSize.z() >> static_cast<Int>(relativeLevel), 1),
            };
        }

        Bool EnsureGeneratedMipmapStorageAllocated(
            MG_State::GLState::TextureObjectMipmap& texture,
            TextureUploadTarget uploadTarget) {
            const Uint existingLevelCount = texture.GetMipmapLevelCount();
            if (existingLevelCount == 0) {
                return false;
            }

            const IntVec3 baseTexelSize = texture.GetMipmapTexelSize(uploadTarget, 0);
            const SizeT baseByteSize = texture.GetMipmapByteSize(uploadTarget, 0);
            const SizeT baseTexelCount = static_cast<SizeT>(baseTexelSize.x()) *
                                         static_cast<SizeT>(baseTexelSize.y()) *
                                         static_cast<SizeT>(baseTexelSize.z());
            if (baseTexelSize.x() <= 0 || baseTexelSize.y() <= 0 || baseTexelSize.z() <= 0 ||
                baseByteSize == 0 || baseTexelCount == 0 || (baseByteSize % baseTexelCount) != 0) {
                return false;
            }

            const SizeT bytesPerTexel = baseByteSize / baseTexelCount;
            const Uint requiredLevelCount = ComputeFullMipmapLevelCount(baseTexelSize);
            for (Uint level = existingLevelCount; level < requiredLevelCount; ++level) {
                const IntVec3 levelTexelSize = ComputeMipmapTexelSize(baseTexelSize, level);
                const SizeT levelByteSize = bytesPerTexel * static_cast<SizeT>(levelTexelSize.x()) *
                                            static_cast<SizeT>(levelTexelSize.y()) *
                                            static_cast<SizeT>(levelTexelSize.z());
                texture.AllocateStorage(uploadTarget, level, {levelTexelSize, levelByteSize});
                texture.MarkStorageDirty(uploadTarget, level, false);
            }
            return true;
        }

        Bool IsMultisampleTextureTarget(TextureTarget target) {
            return target == TextureTarget::Texture2DMultisample ||
                   target == TextureTarget::Texture2DMultisampleArray;
        }

        Int GetMaxSupportedTextureSamples(TextureInternalFormat textureInternalFormat) {
            if (MG_Backend::pActiveBackendObject == nullptr) {
                return std::numeric_limits<Int>::max();
            }

            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            if (MG_Util::IsDepthFormatInternalFormat(textureInternalFormat) ||
                MG_Util::IsStencilFormatInternalFormat(textureInternalFormat)) {
                return std::max(dynamicParameters.MaxDepthTextureSamples, 1);
            }

            GLenum normalizedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat);
            GLenum normalizedFormat = GL_RGBA;
            GLenum normalizedType = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                normalizedInternalFormat, PixelFormatNormalizeOptionBit::None, &normalizedInternalFormat,
                &normalizedFormat, &normalizedType);
            const Bool isIntegerFormat = normalizedFormat == GL_RED_INTEGER || normalizedFormat == GL_RG_INTEGER ||
                                         normalizedFormat == GL_RGB_INTEGER || normalizedFormat == GL_RGBA_INTEGER;
            return std::max(isIntegerFormat ? dynamicParameters.MaxIntegerSamples
                                            : dynamicParameters.MaxColorTextureSamples,
                            1);
        }

        Bool ValidateTextureMultisampleStorage(TextureTarget textureTarget, GLsizei samples, GLsizei width,
                                               GLsizei height, GLsizei depth, TextureInternalFormat textureInternalFormat,
                                               const char* caller) {
            if (!IsMultisampleTextureTarget(textureTarget)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Target is not a multisample texture target."));
                return false;
            }
            if (samples <= 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Sample count must be positive."));
                return false;
            }
            if (width < 0 || height < 0 || depth < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture size must be non-negative."));
                return false;
            }
            if (textureTarget == TextureTarget::Texture2DMultisample && depth != 1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "2D multisample textures must use depth 1."));
                return false;
            }
            if (textureTarget == TextureTarget::Texture2DMultisampleArray && depth == 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "2D multisample array textures must have at least one layer."));
                return false;
            }

            const Int maxSamples = GetMaxSupportedTextureSamples(textureInternalFormat);
            if (samples > maxSamples) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", caller,
                        std::format("Sample count {} exceeds the supported maximum {} for this texture format.",
                                    samples, maxSamples)));
                return false;
            }
            return true;
        }

        void AllocateMultisampleTextureStorage(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                               TextureUploadTarget textureUploadTarget,
                                               TextureInternalFormat textureInternalFormat, GLsizei samples,
                                               GLsizei width, GLsizei height, GLsizei depth,
                                               GLboolean fixedsamplelocations) {
            MOBILEGL_ASSERT(textureObject != nullptr, "AllocateMultisampleTextureStorage requires a texture object");
            MOBILEGL_ASSERT(textureObject->GetStorageType() == TextureStorageType::Mipmap,
                            "AllocateMultisampleTextureStorage requires mipmap-backed storage");

            auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
            textureObject->SetInternalFormat(textureInternalFormat);
            textureObject->SetSamples(samples);
            textureObject->SetFixedSampleLocations(fixedsamplelocations == GL_TRUE);
            textureMipmapObject->AllocateStorage(textureUploadTarget, 0, {{width, height, depth}, 0});
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, 0, false);
        }
    } // namespace

    const SharedPtr<MG_State::GLState::ITextureObject>& GetTextureObjectByName(GLuint texture, const char* caller) {
        if (texture == 0 || !TextureImpl::ValidateTextureName(texture, true)) return nullTextureObject;

        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!TextureImpl::ValidateTextureObject(textureObject)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::format("Texture object {} does not exist.", texture)));
            return nullTextureObject;
        }
        return textureObject;
    }

    TextureUploadTarget GetPrimaryUploadTarget(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject) {
        if (!textureObject) return TextureUploadTarget::Unknown;
        const auto& uploadTargets = textureObject->GetUploadTargets();
        return uploadTargets.empty() ? TextureUploadTarget::Unknown : uploadTargets[0];
    }

    void TextureParameterObject_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLenum pname,
                                      GLint param, const char* caller) {
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
        case GL_GENERATE_MIPMAP:
            g_autoGenerateMipmapByTextureId[textureObject->GetExternalIndex()] = (param != GL_FALSE);
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void TextureParameterObjectf_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLenum pname,
                                       GLfloat param, const char* caller) {
        if (!textureObject) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            textureObject->GetSamplerObject()->SetMagFilter(MG_Util::ConvertGLEnumToSamplerFilterMode((GLenum)param));
            break;
        case GL_TEXTURE_MIN_FILTER:
            textureObject->GetSamplerObject()->SetMinFilter(MG_Util::ConvertGLEnumToSamplerFilterMode((GLenum)param));
            textureObject->GetSamplerObject()->SetMipmapMode(MG_Util::ConvertGLEnumToSamplerMipmapMode((GLenum)param));
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
            textureObject->SetBaseLevel((Uint)param);
            break;
        case GL_TEXTURE_MAX_LEVEL:
            textureObject->SetMaxLevel((Uint)param);
            break;
        case GL_TEXTURE_WRAP_S:
            textureObject->GetSamplerObject()->SetWrapS(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_WRAP_T:
            textureObject->GetSamplerObject()->SetWrapT(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_WRAP_R:
            textureObject->GetSamplerObject()->SetWrapR(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_COMPARE_MODE:
            textureObject->GetSamplerObject()->SetCompareMode(
                MG_Util::ConvertGLEnumToSamplerCompareMode((GLenum)param));
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            textureObject->GetSamplerObject()->SetSamplerCompareFunc(
                MG_Util::ConvertGLEnumToSamplerCompareFunc((GLenum)param));
            break;
        case GL_TEXTURE_LOD_BIAS:
            textureObject->GetSamplerObject()->SetLodBias(param);
            break;
        case GL_GENERATE_MIPMAP:
            g_autoGenerateMipmapByTextureId[textureObject->GetExternalIndex()] = (param != 0.0f);
            break;
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
            if (param != GL_DEPTH_COMPONENT && param != GL_STENCIL_INDEX) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Invalid GL_DEPTH_STENCIL_TEXTURE_MODE value."));
            }
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void GetTextureParameterObjectiv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                           GLenum pname, GLint* params, const char* caller) {
        if (!textureObject || !params) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            *params = (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(textureObject->GetSamplerObject()->GetMagFilter(),
                                                                       SamplerMipmapMode::None);
            break;
        case GL_TEXTURE_MIN_FILTER:
            *params = (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(
                textureObject->GetSamplerObject()->GetMinFilter(), textureObject->GetSamplerObject()->GetMipmapMode());
            break;
        case GL_TEXTURE_MIN_LOD:
            *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMinLod());
            break;
        case GL_TEXTURE_MAX_LOD:
            *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMaxLod());
            break;
        case GL_TEXTURE_BASE_LEVEL:
            *params = static_cast<GLint>(textureObject->GetLevelRange().x());
            break;
        case GL_TEXTURE_MAX_LEVEL:
            *params = static_cast<GLint>(textureObject->GetLevelRange().y());
            break;
        case GL_TEXTURE_WRAP_S:
            *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapS());
            break;
        case GL_TEXTURE_WRAP_T:
            *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapT());
            break;
        case GL_TEXTURE_WRAP_R:
            *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapR());
            break;
        case GL_TEXTURE_COMPARE_MODE:
            *params =
                (GLint)MG_Util::ConvertSamplerCompareModeToGLEnum(textureObject->GetSamplerObject()->GetCompareMode());
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            *params = (GLint)MG_Util::ConvertSamplerCompareFuncToGLEnum(
                textureObject->GetSamplerObject()->GetSamplerCompareFunc());
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "pname is not a valid texture parameter."));
            return;
        }
    }

    const SharedPtr<MG_State::GLState::ITextureObject>& GetTextureObjectByTarget(
        TextureUploadTarget textureUploadTarget, TextureTarget textureTarget) {
        if (TextureImpl::IsProxyTextureTarget(textureUploadTarget)) {
            auto& textureObject =
                TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget);
            if (!TextureImpl::ValidateTextureObject(textureObject)) return nullTextureObject;
            return textureObject;
        } else {
            auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            auto& textureObject = bindingSlot.GetBoundObject();
            if (!TextureImpl::ValidateTextureObject(textureObject)) return nullTextureObject;
            return textureObject;
        }
    }

    void GenerateMipmap_Backend(GLenum target) {
        MG_Backend::gBackendFunctionsTable.GL.GenerateMipmap(target);
    }

    void MaybeAutoGenerateMipmap(GLenum target, const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                 Bool isProxy, GLint level) {
        if (isProxy || level != 0 || !textureObject) {
            return;
        }
        const auto it = g_autoGenerateMipmapByTextureId.find(textureObject->GetExternalIndex());
        if (it == g_autoGenerateMipmapByTextureId.end() || !it->second) {
            return;
        }
        GenerateMipmap_Backend(target);
    }

    void TexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                             GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, depth)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width, yoffset, height, zoffset,
                                                         depth))
            return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureObject->GetFormat(),
                                                                           texturePixelDataType))
            return;

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        const void* originalPixels = pixels;
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->GetDataReadOnly()->data()) +
                             reinterpret_cast<SizeT>(pixels);
        }
        if (!originalPixels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }

        SizeT inputSize = 0;
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureObject->GetFormat(),
            textureInputFormat, texturePixelDataType, {width, height, depth}, false, inputSize);
        if (!processedPixels || inputSize == 0) {
            if (processedPixels) free(processedPixels);
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureObject->GetFormat(), texturePixelDataType);
        const SizeT srcRowSize = static_cast<SizeT>(width) * internalBpp;
        const SizeT srcSliceSize = static_cast<SizeT>(height) * srcRowSize;
        const SizeT destRowSize = static_cast<SizeT>(texelSize.x()) * internalBpp;
        const SizeT destSliceSize = static_cast<SizeT>(texelSize.y()) * destRowSize;

        const auto* srcData = static_cast<const Uint8*>(processedPixels);
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadTarget, level));
        if (destData) {
            for (GLsizei z = 0; z < depth; ++z) {
                for (GLsizei y = 0; y < height; ++y) {
                    const SizeT destRowOffset =
                        static_cast<SizeT>(zoffset + z) * destSliceSize +
                        static_cast<SizeT>(yoffset + y) * destRowSize +
                        static_cast<SizeT>(xoffset) * internalBpp;
                    const SizeT srcRowOffset =
                        static_cast<SizeT>(z) * srcSliceSize + static_cast<SizeT>(y) * srcRowSize;
                    Memcpy(destData + destRowOffset, srcData + srcRowOffset, srcRowSize);
                }
            }
        }

        free(processedPixels);
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        MaybeAutoGenerateMipmap(target, textureObject, false, level);
    }

    void TexSubImage2D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, const void* pixels) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
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
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        // TODO: GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the buffer object's data store is currently mapped.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the data would be unpacked from the buffer object such that the
        // memory reads required would exceed the data store size. GL_INVALID_OPERATION is generated if a
        // non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER target and data is not evenly
        // divisible into the number of bytes needed to store in memory a datum indicated by type.

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
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
        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level);

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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
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
        const SizeT srcStride = srcRowSize;
        const SizeT destRowSize = static_cast<SizeT>(texelSize.x()) * internalBpp;

        if (xoffset + width > static_cast<GLsizei>(texelSize.x()) ||
            yoffset + height > static_cast<GLsizei>(texelSize.y())) {
            MGLOG_E("TexSubImage2D_State: Specified region exceeds texture dimensions");
            free(processedPixels);
            return;
        }

        const auto* srcData = static_cast<const Uint8*>(processedPixels);
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadTarget, level));

        if (destData) {
            for (GLsizei y = 0; y < height; y++) {
                const SizeT destRowOffset = (yoffset + y) * destRowSize + xoffset * internalBpp;
                const SizeT srcRowOffset = y * srcStride;
                Memcpy(destData + destRowOffset, srcData + srcRowOffset, srcRowSize);
            }
        }

        free(processedPixels);

        MGLOG_D("%s: mark mip %d as dirty", __func__, level);
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        MaybeAutoGenerateMipmap(target, textureObject, false, level);
    }

    void TexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                             const GLvoid* pixels) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, 1)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, 1, 1)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureObject->GetFormat(),
                                                                           texturePixelDataType))
            return;

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        const void* originalPixels = pixels;
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->GetDataReadOnly()->data()) +
                             reinterpret_cast<SizeT>(pixels);
        }
        if (!originalPixels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }

        SizeT inputSize = 0;
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureObject->GetFormat(),
            textureInputFormat, texturePixelDataType, {width, 1, 1}, false, inputSize);
        if (!processedPixels || inputSize == 0) {
            if (processedPixels) free(processedPixels);
            return;
        }

        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureObject->GetFormat(), texturePixelDataType);
        const SizeT copySize = static_cast<SizeT>(width) * internalBpp;
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadTarget, level));
        if (destData) {
            Memcpy(destData + static_cast<SizeT>(xoffset) * internalBpp, processedPixels, copySize);
        }

        free(processedPixels);
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        MaybeAutoGenerateMipmap(target, textureObject, false, level);
    }

    // TexParameteriv/TexParameterfv are introduced in OpenGL 4.0, so do not support them for now.
    void TexParameterf_State(GLenum target, GLenum pname, GLfloat param) {

        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!textureObject) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            textureObject->GetSamplerObject()->SetMagFilter(MG_Util::ConvertGLEnumToSamplerFilterMode((GLenum)param));
            break;
        case GL_TEXTURE_MIN_FILTER:
            textureObject->GetSamplerObject()->SetMinFilter(MG_Util::ConvertGLEnumToSamplerFilterMode((GLenum)param));
            textureObject->GetSamplerObject()->SetMipmapMode(MG_Util::ConvertGLEnumToSamplerMipmapMode((GLenum)param));
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
            textureObject->SetBaseLevel((Uint)param);
            break;
        case GL_TEXTURE_MAX_LEVEL:
            textureObject->SetMaxLevel((Uint)param);
            break;
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A: {
            auto swizzleParam = MG_Util::ConvertGLEnumPnameToTextureSwizzleParam(pname);
            auto swizzleValue = MG_Util::ConvertGLEnumToTextureSwizzleParam((GLenum)param);
            textureObject->SetSwizzleParam(swizzleParam, swizzleValue);
            break;
        }
        case GL_TEXTURE_WRAP_S:
            textureObject->GetSamplerObject()->SetWrapS(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_WRAP_T:
            textureObject->GetSamplerObject()->SetWrapT(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_WRAP_R:
            textureObject->GetSamplerObject()->SetWrapR(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_COMPARE_MODE:
            textureObject->GetSamplerObject()->SetCompareMode(
                MG_Util::ConvertGLEnumToSamplerCompareMode((GLenum)param));
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            textureObject->GetSamplerObject()->SetSamplerCompareFunc(
                MG_Util::ConvertGLEnumToSamplerCompareFunc((GLenum)param));
            break;
        case GL_TEXTURE_LOD_BIAS:
            textureObject->GetSamplerObject()->SetLodBias(param);
            break;
        case GL_GENERATE_MIPMAP:
            g_autoGenerateMipmapByTextureId[textureObject->GetExternalIndex()] = (param != 0.0f);
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            // Not supported in this function
        case GL_TEXTURE_BORDER_COLOR:
            // Not supported in this function
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void TexParameteri_State(GLenum target, GLenum pname, GLint param) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!textureObject) return;

        TextureParameterObject_State(textureObject, pname, param, __func__);
    }

    // Quick and dirty TexParameter*v implementation to make NeoForge happy.
    // TODO: implement the missing part
    void TexParameterfv_State(GLenum target, GLenum pname, const GLfloat* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            SetTextureBorderColorFromFloats(textureObject, params);
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            GLint signedParams[4] = {static_cast<GLint>(params[0]), static_cast<GLint>(params[1]),
                                     static_cast<GLint>(params[2]), static_cast<GLint>(params[3])};
            if (!SetTextureSwizzleParamsFromInts(textureObject, signedParams, __func__)) {
                return;
            }
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
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            SetTextureBorderColorFromInts(textureObject, params);
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            if (!SetTextureSwizzleParamsFromInts(textureObject, params, __func__)) {
                return;
            }
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
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            SetTextureBorderColorFromIntegerInts(textureObject, params);
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            GLint signedParams[4] = {static_cast<GLint>(params[0]), static_cast<GLint>(params[1]),
                                     static_cast<GLint>(params[2]), static_cast<GLint>(params[3])};
            if (!SetTextureSwizzleParamsFromInts(textureObject, signedParams, __func__)) {
                return;
            }
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
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            SetTextureBorderColorFromUnsignedInts(textureObject, params);
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
            if (!textureObject) return;

            Vec4<TextureSwizzleParam> swizzleParams;
            for (int i = 0; i < 4; i++) {
                swizzleParams[i] = MG_Util::ConvertGLEnumToTextureSwizzleParam(static_cast<GLint>(params[i]));
                if (TextureSwizzleParam::Unknown == swizzleParams[i]) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`params` is not valid."));
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
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (textureTarget != TextureTarget::Texture2DMultisampleArray) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Target must be GL_TEXTURE_2D_MULTISAMPLE_ARRAY or its proxy."));
            return;
        }

        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!ValidateTextureMultisampleStorage(textureTarget, samples, width, height, depth, textureInternalFormat,
                                               __func__))
            return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        const Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        AllocateMultisampleTextureStorage(textureObject, textureUploadTarget, textureInternalFormat, samples, width,
                                          height, depth, fixedsamplelocations);
    }

    void TexImage2DMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLboolean fixedsamplelocations) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (textureTarget != TextureTarget::Texture2DMultisample) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Target must be GL_TEXTURE_2D_MULTISAMPLE or its proxy."));
            return;
        }

        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!ValidateTextureMultisampleStorage(textureTarget, samples, width, height, 1, textureInternalFormat,
                                               __func__))
            return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        const Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        AllocateMultisampleTextureStorage(textureObject, textureUploadTarget, textureInternalFormat, samples, width,
                                          height, 1, fixedsamplelocations);
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
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, depth)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        // TODO: GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the buffer object's data store is currently mapped.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and the data would be unpacked from the buffer object such that the memory reads required would
        // exceed the data store size.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and data is not evenly divisible into the number of bytes needed to store in memory a datum
        // indicated by type.
        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        // ======================= Processing ================================
        if (internalformat == GL_ALPHA || format == GL_ALPHA) {
            textureObject->SetSwizzleParamRGBA({TextureSwizzleParam::Zero, TextureSwizzleParam::Zero,
                                                TextureSwizzleParam::Zero, TextureSwizzleParam::Red});
        }

        SizeT imageSize = 0;
        const SizeT inputBpp = MG_Util::GetInputBytesPerPixel(textureInputFormat, texturePixelDataType);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);
        const SizeT internalBytes = width * height * depth * internalBpp;

        textureObject->SetInternalFormat(textureInternalFormat);

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

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        // Allocate in TextureObject
        textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{width, height, depth}, internalBytes});

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
            textureMipmapObject->UpdateMipmapSubData(textureUploadTarget, level, texelInput);
        }

        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);

        free(processedPixels);
    }

    void TexImage2D_State(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                          GLenum format, GLenum type, const void* pixels) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        MGLOG_D("%s called with target: %s (%s), level: %d, internalformat: %s (%s), width: %d, height: %d, "
                "border: %d, format: %s (%s), type: %s (%s), pixels: %p",
                __func__, MG_Util::ConvertTextureUploadTargetToString(textureUploadTarget).c_str(),
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
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

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
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        // ======================= Processing ================================
        if (internalformat == GL_ALPHA || format == GL_ALPHA) {
            textureObject->SetSwizzleParamRGBA({TextureSwizzleParam::Zero, TextureSwizzleParam::Zero,
                                                TextureSwizzleParam::Zero, TextureSwizzleParam::Red});
        }

        SizeT imageSize = 0;
        const SizeT inputBpp = MG_Util::GetInputBytesPerPixel(textureInputFormat, texturePixelDataType);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);
        const SizeT internalBytes = width * height * internalBpp;

        MGLOG_D("%s: working on texture %d", __func__, textureObject->GetExternalIndex());

        MGLOG_D("%s: texture object had internal format %s, new format %s", __func__,
                MG_Util::ConvertTextureInternalFormatToString(textureObject->GetFormat()).c_str(),
                MG_Util::ConvertTextureInternalFormatToString(textureInternalFormat).c_str());
        textureObject->SetInternalFormat(textureInternalFormat);

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

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        // Allocate in TextureObject
        if (isProxy) {
            MGLOG_D("%s: isProxy = true, not allocating", __func__);
        } else {
            MGLOG_D("%s: Allocating %d bytes at mip %d", __func__, internalBytes, level);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level,
                                                 {{width, height, 1}, internalBytes});
        }

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
            textureMipmapObject->UpdateMipmapSubData(textureUploadTarget, level, texelInput);
        }

        free(processedPixels);

        MGLOG_D("%s: mark mip %d as dirty", __func__, level);
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        MaybeAutoGenerateMipmap(target, textureObject, isProxy, level);
    }

    void TexImage1D_State(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format,
                          GLenum type, const GLvoid* pixels) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalFormat);

        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, 1)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, 1, 1)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        textureInternalFormat =
            MG_Util::ConvertInternalFormatToSized(textureInternalFormat, textureInputFormat, texturePixelDataType);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        if (internalFormat == GL_ALPHA || format == GL_ALPHA) {
            textureObject->SetSwizzleParamRGBA({TextureSwizzleParam::Zero, TextureSwizzleParam::Zero,
                                                TextureSwizzleParam::Zero, TextureSwizzleParam::Red});
        }

        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);
        const SizeT internalBytes = static_cast<SizeT>(width) * internalBpp;
        textureObject->SetInternalFormat(textureInternalFormat);

        const void* originalPixels = pixels;
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->GetDataReadOnly()->data()) +
                             reinterpret_cast<SizeT>(pixels);
        }

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (!isProxy) {
            textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{width, 1, 1}, internalBytes});
        }

        if (!originalPixels) {
            return;
        }

        SizeT imageSize = 0;
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureInternalFormat,
            textureInputFormat, texturePixelDataType, {width, 1, 1}, false, imageSize);
        if (processedPixels && imageSize > 0) {
            DataPtr texelInput{processedPixels, std::min(imageSize, internalBytes)};
            textureMipmapObject->UpdateMipmapSubData(textureUploadTarget, level, texelInput);
        }

        free(processedPixels);
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        MaybeAutoGenerateMipmap(target, textureObject, isProxy, level);
    }

    void TexBuffer_State(GLenum target, GLenum internalformat, GLuint buffer) {
        // ======================= Converting ================================
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        // TODO: make sure `internalformat` is in one of supported format for TexBuffer
        auto& bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "`buffer` is not zero and is not the name of an existing buffer object."));
            return;
        }

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Buffer) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
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
        if (params == nullptr) return;

        if (pname == GL_TEXTURE_BORDER_COLOR) {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
            auto& textureObject =
                isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                        : bindingSlot.GetBoundObject();

            if (!TextureImpl::ValidateTextureObject(textureObject)) return;

            const auto& borderColor = textureObject->GetBorderColorUI();
            params[0] = borderColor.x();
            params[1] = borderColor.y();
            params[2] = borderColor.z();
            params[3] = borderColor.w();
            return;
        }

        GLint signedParams[4] = {0, 0, 0, 0};
        if (!GetTexParameteriv_State(target, pname, signedParams)) return;
        const int componentCount = pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA ? 4 : 1;
        for (int i = 0; i < componentCount; ++i) {
            params[i] = static_cast<GLuint>(signedParams[i]);
        }
    }

    void GetTexParameterIiv_State(GLenum target, GLenum pname, GLint* params) {
        if (params == nullptr) return;

        if (pname == GL_TEXTURE_BORDER_COLOR) {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
            auto& textureObject =
                isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                        : bindingSlot.GetBoundObject();

            if (!TextureImpl::ValidateTextureObject(textureObject)) return;

            const auto& borderColor = textureObject->GetBorderColorI();
            params[0] = borderColor.x();
            params[1] = borderColor.y();
            params[2] = borderColor.z();
            params[3] = borderColor.w();
            return;
        }

        GetTexParameteriv_State(target, pname, params);
    }

    Bool GetTexParameteriv_State(GLenum target, GLenum pname, GLint* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) return false;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(
                    textureObject->GetSamplerObject()->GetMagFilter(), SamplerMipmapMode::None);
            }
            break;
        case GL_TEXTURE_MIN_FILTER:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(
                    textureObject->GetSamplerObject()->GetMinFilter(),
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
            if (params) {
                *params = static_cast<GLint>(textureObject->GetLevelRange().x());
            }
            break;
        case GL_TEXTURE_MAX_LEVEL:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetLevelRange().y());
            }
            break;
        case GL_TEXTURE_BORDER_COLOR:
            if (params) {
                const auto& borderColor = textureObject->GetBorderColor();
                params[0] = static_cast<GLint>(borderColor.x());
                params[1] = static_cast<GLint>(borderColor.y());
                params[2] = static_cast<GLint>(borderColor.z());
                params[3] = static_cast<GLint>(borderColor.w());
            }
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            if (params) {
                const auto& swizzleParams = textureObject->GetAllSwizzleParams();
                params[0] = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[0]));
                params[1] = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[1]));
                params[2] = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[2]));
                params[3] = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[3]));
            }
            break;
        case GL_TEXTURE_SWIZZLE_R:
            if (params) {
                *params = static_cast<GLint>(
                    MG_Util::ConvertTextureSwizzleParamToGLEnum(textureObject->GetSwizzleParam(TextureSwizzleParam::Red)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_G:
            if (params) {
                *params = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Green)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_B:
            if (params) {
                *params = static_cast<GLint>(
                    MG_Util::ConvertTextureSwizzleParamToGLEnum(textureObject->GetSwizzleParam(TextureSwizzleParam::Blue)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_A:
            if (params) {
                *params = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Alpha)));
            }
            break;
        case GL_TEXTURE_WRAP_S:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapS());
            }
            break;
        case GL_TEXTURE_WRAP_T:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapT());
            }
            break;
        case GL_TEXTURE_WRAP_R:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapR());
            }
            break;
        case GL_TEXTURE_COMPARE_MODE:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerCompareModeToGLEnum(
                    textureObject->GetSamplerObject()->GetCompareMode());
            }
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerCompareFuncToGLEnum(
                    textureObject->GetSamplerObject()->GetSamplerCompareFunc());
            }
            break;
        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexParameteriv_State",
                                                                           "pname is not a valid texture parameter."));
            return false;
        }

        return true;
    }

    void GetTexParameterfv_State(GLenum target, GLenum pname, GLfloat* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertSamplerFilterModeToGLEnum(
                    textureObject->GetSamplerObject()->GetMagFilter(), SamplerMipmapMode::None);
            }
            break;
        case GL_TEXTURE_MIN_FILTER:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertSamplerFilterModeToGLEnum(
                    textureObject->GetSamplerObject()->GetMinFilter(),
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
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetLevelRange().x());
            }
            break;
        case GL_TEXTURE_MAX_LEVEL:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetLevelRange().y());
            }
            break;
        case GL_TEXTURE_BORDER_COLOR:
            if (params) {
                const auto& borderColor = textureObject->GetBorderColor();
                params[0] = borderColor.x();
                params[1] = borderColor.y();
                params[2] = borderColor.z();
                params[3] = borderColor.w();
            }
            break;
        case GL_TEXTURE_SWIZZLE_R:
            if (params) {
                *params = static_cast<GLfloat>(
                    MG_Util::ConvertTextureSwizzleParamToGLEnum(textureObject->GetSwizzleParam(TextureSwizzleParam::Red)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_G:
            if (params) {
                *params = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Green)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_B:
            if (params) {
                *params = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Blue)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_A:
            if (params) {
                *params = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Alpha)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            if (params) {
                const auto& swizzleParams = textureObject->GetAllSwizzleParams();
                params[0] = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[0]));
                params[1] = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[1]));
                params[2] = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[2]));
                params[3] = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[3]));
            }
            break;
        case GL_TEXTURE_WRAP_S:
            if (params) {
                *params =
                    (GLfloat)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapS());
            }
            break;
        case GL_TEXTURE_WRAP_T:
            if (params) {
                *params =
                    (GLfloat)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapT());
            }
            break;
        case GL_TEXTURE_WRAP_R:
            if (params) {
                *params =
                    (GLfloat)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapR());
            }
            break;
        case GL_TEXTURE_COMPARE_MODE:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertSamplerCompareModeToGLEnum(
                    textureObject->GetSamplerObject()->GetCompareMode());
            }
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertSamplerCompareFuncToGLEnum(
                    textureObject->GetSamplerObject()->GetSamplerCompareFunc());
            }
            break;
        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexParameterfv_State",
                                                                           "pname is not a valid texture parameter."));
            return;
        }
    }

    void GetTexLevelParameteriv_State(GLenum target, GLint level, GLenum pname, GLint* params) {
        MGLOG_D("GetTexLevelParameteriv_State called");
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_WIDTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).x();
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
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).y();
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
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).z();
                    break;
                }
                default:
                    THROW_UNIMPL_EXCEPTION;
                }
            }
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            if (params) {
                *params = (GLint)MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
            }
            break;
        case GL_TEXTURE_SAMPLES:
            if (params) {
                *params = textureObject->GetSamples();
            }
            break;
        case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS:
            if (params) {
                *params = textureObject->HasFixedSampleLocations() ? GL_TRUE : GL_FALSE;
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
        case GL_TEXTURE_STENCIL_SIZE:
            if (params) {
                *params = GetTextureLevelComponentParameter(textureObject->GetFormat(), pname);
            }
            break;
        case GL_TEXTURE_COMPRESSED:
            if (params) {
                *params = GL_FALSE;
            }
            break;
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
            if (params) {
                *params = 0;
            }
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexLevelParameteriv_State",
                                                                     "pname is not a valid texture level parameter."));
            return;
        }
        if (params) MGLOG_D("returned %u", *params);
    }

    void GetTexLevelParameterfv_State(GLenum target, GLint level, GLenum pname, GLfloat* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_WIDTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = (GLfloat)textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).x();
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
                    *params = (GLfloat)textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).y();
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
                    *params = (GLfloat)textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).z();
                    break;
                }
                default:
                    THROW_UNIMPL_EXCEPTION;
                }
            }
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
            }
            break;
        case GL_TEXTURE_SAMPLES:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetSamples());
            }
            break;
        case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS:
            if (params) {
                *params = textureObject->HasFixedSampleLocations() ? 1.0f : 0.0f;
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
        case GL_TEXTURE_STENCIL_SIZE:
            if (params) {
                *params = static_cast<GLfloat>(GetTextureLevelComponentParameter(textureObject->GetFormat(), pname));
            }
            break;
        case GL_TEXTURE_COMPRESSED:
            if (params) {
                *params = 0.0f;
            }
            break;
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
            if (params) {
                *params = 0.0f;
            }
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexLevelParameterfv_State",
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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GenTextures_State", "n must be non-negative"));
            return;
        }

        // ======================= Processing ================================
        Vector<Uint> textureNames;
        MG_State::pGLContext->GenTextureNames(n, textureNames);
        Memcpy(textures, textureNames.data(), n * sizeof(GLuint));
    }

    void DeleteTextures_State(GLsizei n, const GLuint* textures) {
        // ===================== Error Checking ==============================
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteTextures_State", "n must be non-negative."));
            return;
        }

        if (!textures) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteTextures_State",
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

    void CopyImageSubData_Backend(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                                  GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                  const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                                  GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                  GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        auto copyImageSubData = MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData;
        if (!copyImageSubData) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support image-to-image copies."));
            return;
        }
        copyImageSubData(srcTexture, srcTarget, srcLevel, srcX, srcY, srcZ, dstTexture, dstTarget, dstLevel, dstX,
                         dstY, dstZ, srcWidth, srcHeight, srcDepth);
    }

    Bool ValidateCopyImageSubData_State(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                                        GLenum srcTarget, GLint srcLevel,
                                        const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                                        GLenum dstTarget, GLint dstLevel,
                                        GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        if (!TextureImpl::ValidateTextureObject(srcTexture) || !TextureImpl::ValidateTextureObject(dstTexture)) {
            return false;
        }
        const auto srcTextureTarget = MG_Util::ConvertGLEnumToTextureTarget(srcTarget);
        const auto dstTextureTarget = MG_Util::ConvertGLEnumToTextureTarget(dstTarget);
        if (!TextureImpl::ValidateTextureTarget(srcTextureTarget) ||
            !TextureImpl::ValidateTextureTarget(dstTextureTarget)) {
            return false;
        }
        if (!TextureImpl::ValidateTextureTargetUniformity(srcTexture, srcTextureTarget) ||
            !TextureImpl::ValidateTextureTargetUniformity(dstTexture, dstTextureTarget)) {
            return false;
        }
        if (!TextureImpl::ValidateTextureLevelNumber(srcLevel) ||
            !TextureImpl::ValidateTextureLevelNumber(dstLevel)) {
            return false;
        }
        if (srcWidth < 0 || srcHeight < 0 || srcDepth < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Copy dimensions must be non-negative."));
            return false;
        }
        if (srcWidth == 0 || srcHeight == 0 || srcDepth == 0) {
            return false;
        }
        if (!TextureImpl::ValidateBaseInternalFormatMatch(srcTexture->GetFormat(), dstTexture->GetFormat())) {
            return false;
        }
        return true;
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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyTexImage2D_State",
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
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyTexImage2D_State",                                     \
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
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        TexImage2D_State(target, level, (GLint)realInternalFormat, width, height, border, format, type, nullptr);
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
        const Int activeUnit = MG_State::pGLContext->GetActiveTextureUnit();
        MGLOG_D("BindTexture_State called with target: 0x%X, texture: %u, unit: %d", target, texture, activeUnit);
        // ======================= Converting ================================
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;

        // Name 0 unbinds the current target from the active texture unit.
        if (texture == 0) {
            auto& currentUnit = MG_State::pGLContext->GetTextureUnitObject(activeUnit);
            auto& bindingSlot = currentUnit.GetBindingSlot(textureTarget);
            bindingSlot.Bind(nullptr);
            return;
        }

        // Some desktop-side helper code saves GL_ACTIVE_TEXTURE and later feeds it back into glBindTexture
        // as if it were a texture name. Treating that as a no-op preserves the previous "invalid bind does not
        // change texture state" behavior, but avoids poisoning the error state every frame.
        if (!MG_State::pGLContext->ValidateTextureName(texture) && texture >= GL_TEXTURE0 && texture <= GL_TEXTURE31) {
            return;
        }

        if (!MG_State::pGLContext->ValidateTextureName(texture)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BindTexture_State", "Invalid texture name"));
            return;
        }

        if (!TextureImpl::ValidateTextureName(texture, true)) return;

        // ======================= Processing ================================
        Bool doesTextureExist = MG_State::pGLContext->ValidateTextureObject(texture);
        if (!doesTextureExist) {
            MG_State::pGLContext->CreateTextureObject(texture, textureTarget);
        }
        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);

        // ===================== Error Checking ==============================
        if (doesTextureExist && !TextureImpl::ValidateTextureTargetUniformity(textureObject, textureTarget)) return;

        // ======================= Processing ================================
        auto& currentUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = currentUnit.GetBindingSlot(textureTarget);
        bindingSlot.Bind(textureObject);
    }

    void ActiveTexture_State(GLenum texture) {
        // ===================== Error Checking ==============================
        if (texture < GL_TEXTURE0 || texture > GL_TEXTURE31) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ActiveTexture_State",
                    std::format("Texture must be one of GL_TEXTUREi, where i is in the range 0 to 31, but got "
                                "invalid enum: 0x{:X}, which may stand for unit {}.",
                                texture, texture - GL_TEXTURE0)));
            return;
        }

        // ======================= Processing ================================
        const Int unit = (Int)texture - GL_TEXTURE0;
        MGLOG_D("ActiveTexture_State: unit = %d", unit);
        MG_State::pGLContext->SetActiveTextureUnit(unit);
    }

    void GetTexImage_Backend(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        MG_Backend::gBackendFunctionsTable.GL.GetTexImage(target, level, format, type, pixels);
    }

    // Add to GL_Texture.cpp
    Bool GetTexImage_State(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid texture target"));
            return false;
        }

        // Validate level
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Level must be non-negative"));
            return false;
        }

        // Validate format
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid format"));
            return false;
        }

        // Validate type
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid pixel data type"));
            return false;
        }

        // Get texture object
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                                           "No valid texture bound to target"));
            return false;
        }

        // Check texture completeness
        if (!textureObject->IsComplete()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Texture is incomplete"));
            return false;
        }

        // Check PBO state
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();

        if (pixelPackBufferObject) {
            // Check if PBO is mapped
            if (pixelPackBufferObject->IsMapped()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                                              "Pixel pack buffer is currently mapped"));
                return false;
            }

            // Check alignment
            const SizeT typeSize = MG_Util::GetTexturePixelDataTypeSize(texturePixelDataType);
            if (reinterpret_cast<uintptr_t>(pixels) % typeSize != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                 "Pixel data not aligned for pixel pack buffer"));
                return false;
            }
        }

        // Special case for depth/stencil
        if (textureInputFormat == TextureInputFormat::StencilIndex) {
            if (textureObject->GetFormat() != TextureInternalFormat::DepthStencil &&
                textureObject->GetFormat() != TextureInternalFormat::Depth24Stencil8 &&
                textureObject->GetFormat() != TextureInternalFormat::Depth32FStencil8) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State",
                                                 "No stencil buffer for stencil index format"));
                return false;
            }
        }

        return true;
    }

    void CopyTextureImageToClientOrPBO_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                            TextureUploadTarget textureUploadTarget, GLint level, GLenum format,
                                            GLenum type, GLsizei bufSize, void* pixels, const char* caller) {
        if (!textureObject) return;

        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture storage is not mipmap-backed."));
            return;
        }

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture level is out of range."));
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level);
        const void* src = textureMipmapObject->MapMipmapData(textureUploadTarget, level);
        if (!src) return;

        SizeT packedSize = 0;
        void* packedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataPack(
            src, MG_State::pGLContext->GetPixelStoreParameters(false), textureObject->GetFormat(), texturePixelDataType,
            textureInputFormat, texturePixelDataType, texelSize, false, packedSize);
        if (!packedPixels || packedSize == 0) {
            if (packedPixels) free(packedPixels);
            return;
        }

        if (bufSize >= 0 && static_cast<SizeT>(bufSize) < packedSize) {
            free(packedPixels);
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Destination buffer is too small."));
            return;
        }

        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        if (pixelPackBufferObject) {
            const SizeT offset = reinterpret_cast<SizeT>(pixels);
            if (offset + packedSize > pixelPackBufferObject->GetSize()) {
                free(packedPixels);
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Pixel pack buffer is too small."));
                return;
            }
            pixelPackBufferObject->UploadSubData({packedPixels, packedSize}, offset);
        } else if (pixels) {
            Memcpy(pixels, packedPixels, packedSize);
        }

        free(packedPixels);
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void CreateTextures(GLenum target, GLsizei n, GLuint* textures) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "n must be non-negative."));
            return;
        }
        if (n > 0 && !textures) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture output pointer cannot be null."));
            return;
        }

        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;

        Vector<Uint> textureNames;
        MG_State::pGLContext->GenTextureNames(n, textureNames);
        for (GLsizei i = 0; i < n; ++i) {
            textures[i] = textureNames[i];
            MG_State::pGLContext->CreateTextureObject(textureNames[i], textureTarget);
        }
    }

    void TextureStorage1D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (levels < 1) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "levels must be positive."));
            return;
        }
        if (!TextureImpl::ValidateTextureSizeRange(width, 1, 1)) return;

        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        const auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        textureObject->SetInternalFormat(textureInternalFormat);
        for (GLsizei level = 0; level < levels; ++level) {
            const GLsizei levelWidth = std::max<GLsizei>(1, width >> level);
            const SizeT byteSize = ComputeTextureStorageByteSize(textureInternalFormat, levelWidth, 1, 1);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{levelWidth, 1, 1}, byteSize});
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, false);
        }
    }

    void TextureStorage2D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (levels < 1) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "levels must be positive."));
            return;
        }
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;

        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        GLenum realInternalFormat = internalformat;
        GLenum realFormat = GL_RGBA;
        GLenum realType = GL_UNSIGNED_BYTE;
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(
            MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat), PixelFormatNormalizeOptionBit::None,
            &realInternalFormat, &realFormat, &realType);
        const SizeT bytesPerPixel = MG_Util::GetInternalBytesPerPixel(
            textureInternalFormat, MG_Util::ConvertGLEnumToTexturePixelDataType(realType));

        textureObject->SetInternalFormat(textureInternalFormat);
        for (GLsizei level = 0; level < levels; ++level) {
            const GLsizei levelWidth = std::max<GLsizei>(1, width >> level);
            const GLsizei levelHeight = std::max<GLsizei>(1, height >> level);
            const SizeT byteSize = static_cast<SizeT>(levelWidth) * static_cast<SizeT>(levelHeight) * bytesPerPixel;
            textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{levelWidth, levelHeight, 1}, byteSize});
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, false);
        }
    }

    void TextureStorage3D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                          GLsizei depth) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (levels < 1) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "levels must be positive."));
            return;
        }
        if (!TextureImpl::ValidateTextureSizeRange(width, height, depth)) return;

        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        const auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        textureObject->SetInternalFormat(textureInternalFormat);
        for (GLsizei level = 0; level < levels; ++level) {
            const GLsizei levelWidth = std::max<GLsizei>(1, width >> level);
            const GLsizei levelHeight = std::max<GLsizei>(1, height >> level);
            const GLsizei levelDepth = std::max<GLsizei>(1, depth >> level);
            const SizeT byteSize = ComputeTextureStorageByteSize(textureInternalFormat, levelWidth, levelHeight,
                                                                 levelDepth);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level,
                                                 {{levelWidth, levelHeight, levelDepth}, byteSize});
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, false);
        }
    }

    void TextureStorage2DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLboolean fixedsamplelocations) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexStorage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
        });
    }

    void TextureStorage3DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexStorage3DMultisample(target, samples, internalformat, width, height, depth, fixedsamplelocations);
        });
    }

    void TexStorage1D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        TextureStorage1D(textureObject->GetExternalIndex(), levels, internalformat, width);
    }

    void TexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        TextureStorage2D(textureObject->GetExternalIndex(), levels, internalformat, width, height);
    }

    void TexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                      GLsizei depth) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        TextureStorage3D(textureObject->GetExternalIndex(), levels, internalformat, width, height, depth);
    }

    void TexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLboolean fixedsamplelocations) {
        TexImage2DMultisample_State(target, samples, internalformat, width, height, fixedsamplelocations);
    }

    void TexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
        TexImage3DMultisample_State(target, samples, internalformat, width, height, depth, fixedsamplelocations);
    }

    void TextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                           const void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexSubImage1D_State(target, level, xoffset, width, format, type, pixels);
        });
    }

    void TextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                           GLenum format, GLenum type, const void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;

        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureObject->GetFormat(),
                                                                           texturePixelDataType))
            return;

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level is out of range."));
            return;
        }
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width, yoffset, height)) return;

        const void* originalPixels = pixels;
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->GetDataReadOnly()->data()) +
                             reinterpret_cast<SizeT>(pixels);
        }
        if (!originalPixels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }

        SizeT inputSize = 0;
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureObject->GetFormat(),
            textureInputFormat, texturePixelDataType, {width, height, 1}, false, inputSize);
        if (!processedPixels || inputSize == 0) {
            if (processedPixels) free(processedPixels);
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureObject->GetFormat(), texturePixelDataType);
        const SizeT srcRowSize = static_cast<SizeT>(width) * internalBpp;
        const SizeT destRowSize = static_cast<SizeT>(texelSize.x()) * internalBpp;

        const auto* srcData = static_cast<const Uint8*>(processedPixels);
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadTarget, level));
        if (destData) {
            for (GLsizei y = 0; y < height; ++y) {
                const SizeT destRowOffset = static_cast<SizeT>(yoffset + y) * destRowSize +
                                            static_cast<SizeT>(xoffset) * internalBpp;
                const SizeT srcRowOffset = static_cast<SizeT>(y) * srcRowSize;
                Memcpy(destData + destRowOffset, srcData + srcRowOffset, srcRowSize);
            }
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        }
        free(processedPixels);
    }

    void TextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                           GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexSubImage3D_State(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
        });
    }

    void TextureParameteri(GLuint texture, GLenum pname, GLint param) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        TextureParameterObject_State(textureObject, pname, param, __func__);
    }

    void TextureParameterf(GLuint texture, GLenum pname, GLfloat param) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        TextureParameterObjectf_State(textureObject, pname, param, __func__);
    }

    void TextureParameterfv(GLuint texture, GLenum pname, const GLfloat* params) {
        if (!params) return;
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { TexParameterfv_State(target, pname, params); });
    }

    void TextureParameteriv(GLuint texture, GLenum pname, const GLint* params) {
        if (!params) return;
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { TexParameteriv_State(target, pname, params); });
    }

    void TextureParameterIiv(GLuint texture, GLenum pname, const GLint* params) {
        if (!params) return;
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexParameterIiv_State(target, pname, params);
        });
    }

    void TextureParameterIuiv(GLuint texture, GLenum pname, const GLuint* params) {
        if (!params) return;
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexParameterIuiv_State(target, pname, params);
        });
    }

    void BindTextureUnit(GLuint unit, GLuint texture) {
        if (unit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture unit is out of range."));
            return;
        }

        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(static_cast<Int>(unit));
        if (texture == 0) {
            for (auto& slot : textureUnit.GetAllBindingSlots()) {
                slot.Bind(nullptr);
            }
            return;
        }

        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        textureUnit.GetBindingSlot(textureObject->GetTarget()).Bind(textureObject);
    }

    void GetTextureImage(GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        const auto uploadTarget = GetPrimaryUploadTarget(textureObject);
        if (MG_Backend::pActiveBackendObject != nullptr &&
            MG_Backend::pActiveBackendObject->GetBackendType() == BackendType::DirectVulkan &&
            MG_Backend::gBackendFunctionsTable.GL.GetTextureImage != nullptr) {
            MG_Backend::gBackendFunctionsTable.GL.GetTextureImage(textureObject, uploadTarget, level, format, type,
                                                                  bufSize, pixels);
            return;
        }
        CopyTextureImageToClientOrPBO_State(textureObject, uploadTarget, level, format, type, bufSize, pixels,
                                            __func__);
    }

    void GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                            GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (level < 0 || xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0 || depth < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture sub-image range is invalid."));
            return;
        }
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        const auto uploadTarget = GetPrimaryUploadTarget(textureObject);
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level is out of range."));
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, static_cast<Uint>(level));
        const Bool isFullLevelRead = xoffset == 0 && yoffset == 0 && zoffset == 0 &&
                                     width == texelSize.x() && height == texelSize.y() &&
                                     depth == texelSize.z();
        if (!isFullLevelRead) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Partial texture sub-image readback is not implemented yet."));
            return;
        }

        GetTextureImage(texture, level, format, type, bufSize, pixels);
    }

    void GetTextureParameteriv(GLuint texture, GLenum pname, GLint* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GetTexParameteriv_State(target, pname, params); });
    }

    void GetTextureParameterfv(GLuint texture, GLenum pname, GLfloat* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GetTexParameterfv_State(target, pname, params); });
    }

    void GetTextureParameterIiv(GLuint texture, GLenum pname, GLint* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GetTexParameterIiv_State(target, pname, params); });
    }

    void GetTextureParameterIuiv(GLuint texture, GLenum pname, GLuint* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GetTexParameterIuiv_State(target, pname, params); });
    }

    void GetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            GetTexLevelParameteriv_State(target, level, pname, params);
        });
    }

    void GetTextureLevelParameterfv(GLuint texture, GLint level, GLenum pname, GLfloat* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            GetTexLevelParameterfv_State(target, level, pname, params);
        });
    }

    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format) {
        if (unit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Image texture unit is out of range."));
            return;
        }
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level must be non-negative."));
            return;
        }
        if (layer < 0 && layered == GL_FALSE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture layer must be non-negative."));
            return;
        }
        if (access != GL_READ_ONLY && access != GL_WRITE_ONLY && access != GL_READ_WRITE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Invalid image texture access."));
            return;
        }

        SharedPtr<MG_State::GLState::ITextureObject> textureObject;
        if (texture != 0) {
            if (!MG_State::pGLContext->ValidateTextureObject(texture)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture name is not a texture object."));
                return;
            }
            textureObject = MG_State::pGLContext->GetTextureObject(texture);
        }

        auto bindImageTexture = MG_Backend::gBackendFunctionsTable.GL.BindImageTexture;
        if (!bindImageTexture) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support image texture binding."));
            return;
        }

        MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(unit))
            .Bind(textureObject, level, layered, layer, access, format);
        bindImageTexture(unit, texture, level, layered, layer, access, format);
    }

    void GenerateMipmap(GLenum target) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& textureObject = activeUnit.GetBindingSlot(textureTarget).GetBoundObject();
        if (textureObject) {
            auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
            MOBILEGL_ASSERT(mipmapTexture != nullptr, "GenerateMipmap requires mipmap texture storage.");
        }
        GenerateMipmap_Backend(target);
    }

    void GenerateTextureMipmap(GLuint texture) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (textureObject) {
            auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
            MOBILEGL_ASSERT(mipmapTexture != nullptr, "GenerateTextureMipmap requires mipmap texture storage.");
        }
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GenerateMipmap_Backend(target); });
    }

    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        if (!GetTexImage_State(target, level, format, type, pixels)) return;
        if (MG_Backend::gBackendFunctionsTable.GL.GetTexImage != nullptr) {
            GetTexImage_Backend(target, level, format, type, pixels);
            return;
        }
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        const auto& textureObject = activeUnit.GetBindingSlot(textureTarget).GetBoundObject();
        CopyTextureImageToClientOrPBO_State(textureObject, textureUploadTarget, level, format, type, -1, pixels,
                                            __func__);
    }

    void GetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint* params) {
        if (!params || bufSize <= 0) return;

        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        const Bool isRenderbufferTarget = target == GL_RENDERBUFFER;
        if (!isRenderbufferTarget && !TextureImpl::ValidateTextureTarget(textureTarget)) return;

        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;

        GLenum preferredInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat);
        GLenum imageFormat = GL_RGBA;
        GLenum imageType = GL_UNSIGNED_BYTE;
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(preferredInternalFormat, PixelFormatNormalizeOptionBit::None,
                                                              &preferredInternalFormat, &imageFormat, &imageType);

        auto writeValues = [&](std::initializer_list<GLint> values) {
            GLsizei index = 0;
            for (GLint value : values) {
                if (index >= bufSize) break;
                params[index++] = value;
            }
            while (index < bufSize) {
                params[index++] = 0;
            }
        };

        const Bool isDepthFormat = MG_Util::IsDepthFormatInternalFormat(textureInternalFormat);
        const Bool isStencilFormat = MG_Util::IsStencilFormatInternalFormat(textureInternalFormat);
        const ComponentSizes componentSizes = MG_Util::GetComponentSizesForInternalFormat(textureInternalFormat);
        const Bool isIntegerFormat = imageFormat == GL_RED_INTEGER || imageFormat == GL_RG_INTEGER ||
                                     imageFormat == GL_RGB_INTEGER || imageFormat == GL_RGBA_INTEGER;

        SizeT targetIndex = isRenderbufferTarget ? MG_Backend::GetRenderbufferFormatCapabilityTargetIndex()
                                                 : MG_Backend::GetFormatCapabilityTargetIndex(textureTarget);
        if (targetIndex >= MG_Backend::kFormatCapabilityTargetCount) return;
        const SizeT formatIndex = static_cast<SizeT>(textureInternalFormat);

        MG_Backend::FormatCapabilityFlags fullCaps{};
        MG_Backend::FormatCapabilityFlags caveatCaps{};
        const Vector<Int>* sampleCounts = nullptr;
        if (MG_Backend::pActiveBackendObject) {
            const auto& cache = MG_Backend::pActiveBackendObject->GetFormatCapabilities();
            fullCaps = cache.FullCaps[targetIndex][formatIndex];
            caveatCaps = cache.CaveatCaps[targetIndex][formatIndex];
            sampleCounts = &cache.SampleCounts[targetIndex][formatIndex];
        }

        auto hasFull = [&](MG_Backend::FormatCapability capability) {
            return MG_Backend::HasFormatCapability(fullCaps, capability);
        };
        auto hasCaveat = [&](MG_Backend::FormatCapability capability) {
            return MG_Backend::HasFormatCapability(caveatCaps, capability);
        };
        auto supportFor = [&](MG_Backend::FormatCapability capability) -> GLint {
            if (hasFull(capability)) return GL_FULL_SUPPORT;
            if (hasCaveat(capability)) return GL_CAVEAT_SUPPORT;
            return GL_NONE;
        };
        auto supportForWithFallback = [&](MG_Backend::FormatCapability primary,
                                          MG_Backend::FormatCapability fallback) -> GLint {
            if (hasFull(primary)) return GL_FULL_SUPPORT;
            if (hasCaveat(primary) || hasFull(fallback) || hasCaveat(fallback)) return GL_CAVEAT_SUPPORT;
            return GL_NONE;
        };
        switch (pname) {
        case GL_INTERNALFORMAT_SUPPORTED:
            writeValues({(hasFull(MG_Backend::FormatCapability::Creatable) ||
                          hasCaveat(MG_Backend::FormatCapability::Creatable))
                             ? GL_TRUE
                             : GL_FALSE});
            return;
        case GL_INTERNALFORMAT_PREFERRED:
            writeValues({static_cast<GLint>(preferredInternalFormat)});
            return;
        case GL_INTERNALFORMAT_RED_SIZE:
            writeValues({componentSizes.Red});
            return;
        case GL_INTERNALFORMAT_GREEN_SIZE:
            writeValues({componentSizes.Green});
            return;
        case GL_INTERNALFORMAT_BLUE_SIZE:
            writeValues({componentSizes.Blue});
            return;
        case GL_INTERNALFORMAT_ALPHA_SIZE:
            writeValues({componentSizes.Alpha});
            return;
        case GL_INTERNALFORMAT_DEPTH_SIZE:
            writeValues({componentSizes.Depth});
            return;
        case GL_INTERNALFORMAT_STENCIL_SIZE:
            writeValues({componentSizes.Stencil});
            return;
        case GL_INTERNALFORMAT_SHARED_SIZE:
            writeValues({textureInternalFormat == TextureInternalFormat::RGB9E5 ? 5 : 0});
            return;
        case GL_INTERNALFORMAT_RED_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Red, false, false)});
            return;
        case GL_INTERNALFORMAT_GREEN_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Green, false, false)});
            return;
        case GL_INTERNALFORMAT_BLUE_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Blue, false, false)});
            return;
        case GL_INTERNALFORMAT_ALPHA_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Alpha, false, false)});
            return;
        case GL_INTERNALFORMAT_DEPTH_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Depth, true, false)});
            return;
        case GL_INTERNALFORMAT_STENCIL_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Stencil, false, true)});
            return;
        case GL_TEXTURE_IMAGE_FORMAT:
            writeValues({static_cast<GLint>(imageFormat)});
            return;
        case GL_TEXTURE_IMAGE_TYPE:
            writeValues({static_cast<GLint>(imageType)});
            return;
        case GL_TEXTURE_COMPRESSED:
        case GL_TEXTURE_COMPRESSED_BLOCK_WIDTH:
        case GL_TEXTURE_COMPRESSED_BLOCK_HEIGHT:
        case GL_TEXTURE_COMPRESSED_BLOCK_SIZE:
            writeValues({0});
            return;
        case GL_COLOR_COMPONENTS:
            writeValues({(!isDepthFormat && !isStencilFormat) ? GL_TRUE : GL_FALSE});
            return;
        case GL_DEPTH_COMPONENTS:
            writeValues({isDepthFormat ? GL_TRUE : GL_FALSE});
            return;
        case GL_STENCIL_COMPONENTS:
            writeValues({isStencilFormat ? GL_TRUE : GL_FALSE});
            return;
        case GL_FRAMEBUFFER_RENDERABLE:
            writeValues({supportFor(MG_Backend::FormatCapability::FramebufferRenderable)});
            return;
        case GL_FRAMEBUFFER_RENDERABLE_LAYERED:
            writeValues({supportFor(MG_Backend::FormatCapability::FramebufferLayered)});
            return;
        case GL_FILTER:
            writeValues({supportForWithFallback(MG_Backend::FormatCapability::LinearFilter,
                                                MG_Backend::FormatCapability::Sampled)});
            return;
        case GL_MIPMAP:
            writeValues({supportForWithFallback(MG_Backend::FormatCapability::GenerateMipmap,
                                                MG_Backend::FormatCapability::Sampled)});
            return;
        case GL_TEXTURE_GATHER:
        case GL_TEXTURE_GATHER_SHADOW:
            writeValues({supportFor(MG_Backend::FormatCapability::TextureGather)});
            return;
        case GL_TEXTURE_SHADOW:
            writeValues({supportFor(MG_Backend::FormatCapability::TextureShadow)});
            return;
        case GL_NUM_SAMPLE_COUNTS:
            writeValues({sampleCounts != nullptr ? static_cast<GLint>(sampleCounts->size()) : 0});
            return;
        case GL_SAMPLES:
            if (sampleCounts != nullptr) {
                GLsizei index = 0;
                for (Int sampleCount : *sampleCounts) {
                    if (index >= bufSize) break;
                    params[index++] = sampleCount;
                }
                while (index < bufSize) {
                    params[index++] = 0;
                }
                return;
            }
            writeValues({});
            return;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname is not supported by GetInternalformativ."));
            return;
        }
    }

    void GetMultisamplefv(GLenum pname, GLuint index, GLfloat* val) {
        if (val == nullptr) return;
        if (pname != GL_SAMPLE_POSITION) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Only GL_SAMPLE_POSITION is supported."));
            return;
        }

        const Int maxSamples = MG_Backend::pActiveBackendObject != nullptr
            ? std::max(MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxSamples, 1)
            : 1;
        if (static_cast<Int>(index) >= maxSamples) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Sample index is out of range."));
            return;
        }

        // Keep sample positions deterministic even before the backend exposes vendor-specific patterns.
        val[0] = 0.5f;
        val[1] = 0.5f;
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

    void CopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        auto srcTexture = GetTextureObjectByName(srcName, __func__);
        auto dstTexture = GetTextureObjectByName(dstName, __func__);
        if (!ValidateCopyImageSubData_State(srcTexture, srcTarget, srcLevel, dstTexture, dstTarget, dstLevel,
                                            srcWidth, srcHeight, srcDepth)) {
            return;
        }
        CopyImageSubData_Backend(srcTexture, srcTarget, srcLevel, srcX, srcY, srcZ, dstTexture, dstTarget, dstLevel,
                                 dstX, dstY, dstZ, srcWidth, srcHeight, srcDepth);
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
