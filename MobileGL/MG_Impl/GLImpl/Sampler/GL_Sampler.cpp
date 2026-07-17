// MobileGL - MobileGL/MG_Impl/GLImpl/Sampler/GL_Sampler.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Sampler.h"
#include "Validators.h"
#include <MG_State/GLState/Core.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        Float ReadSamplerScalar(const void* param, Bool isFloat, Bool isUnsignedInteger) {
            if (isFloat) return *(const GLfloat*)param;
            if (isUnsignedInteger) return static_cast<Float>(*(const GLuint*)param);
            return static_cast<Float>(*(const GLint*)param);
        }

        Bool ValidateSamplerParameterValue(GLenum pname, const void* param, Bool isFloat, Bool isUnsignedInteger) {
            if (param == nullptr) return false;

            switch (pname) {
            case GL_TEXTURE_MIN_LOD:
            case GL_TEXTURE_MAX_LOD:
            case GL_TEXTURE_LOD_BIAS:
                return true;
            case GL_TEXTURE_MAX_ANISOTROPY_EXT:
                if (ReadSamplerScalar(param, isFloat, isUnsignedInteger) >= 1.0f) return true;
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "SetSamplerParam_State",
                                                 "GL_TEXTURE_MAX_ANISOTROPY_EXT must be at least 1.0."));
                return false;
            default:
                break;
            }

            if (isFloat) {
                return SamplerImpl::ValidateSamplerFloatParam(pname, *(const GLfloat*)param);
            }
            if (isUnsignedInteger) {
                return SamplerImpl::ValidateSamplerIntParam(pname, static_cast<GLint>(*(const GLuint*)param));
            }
            return SamplerImpl::ValidateSamplerIntParam(pname, *(const GLint*)param);
        }
    } // namespace

    void SetSamplerParam_State(GLuint sampler, GLenum pname, const void* param, bool isFloat,
                               bool isUnsignedInteger) {
        if (param == nullptr) return;
        if (!SamplerImpl::ValidateSamplerName(sampler)) return;

        Bool doesSamplerObjectCreated = MG_State::pGLContext->ValidateSamplerObject(sampler);
        if (!doesSamplerObjectCreated) {
            // Create one for compatibility
            MG_State::pGLContext->CreateSamplerObject(sampler);
        }
        auto& samplerObj = MG_State::pGLContext->GetSamplerObject(sampler);
        if (!SamplerImpl::ValidateSamplerObject(sampler)) return;
        if (!ValidateSamplerParameterValue(pname, param, isFloat, isUnsignedInteger)) return;

        using namespace MG_Util;
        switch (pname) {
        case GL_TEXTURE_WRAP_S:
            samplerObj->SetWrapS(MG_Util::ConvertGLEnumToSamplerWrapMode(*(const GLint*)param));
            break;
        case GL_TEXTURE_WRAP_T:
            samplerObj->SetWrapT(MG_Util::ConvertGLEnumToSamplerWrapMode(*(const GLint*)param));
            break;
        case GL_TEXTURE_WRAP_R:
            samplerObj->SetWrapR(MG_Util::ConvertGLEnumToSamplerWrapMode(*(const GLint*)param));
            break;
        case GL_TEXTURE_MIN_FILTER:
            samplerObj->SetMinFilter(MG_Util::ConvertGLEnumToSamplerFilterMode(*(const GLint*)param));
            samplerObj->SetMipmapMode(MG_Util::ConvertGLEnumToSamplerMipmapMode(*(const GLint*)param));
            break;
        case GL_TEXTURE_MAG_FILTER:
            samplerObj->SetMagFilter(MG_Util::ConvertGLEnumToSamplerFilterMode(*(const GLint*)param));
            break;
        case GL_TEXTURE_MIN_LOD:
            samplerObj->SetLodRange(*(const GLfloat*)param, samplerObj->GetMaxLod());
            break;
        case GL_TEXTURE_MAX_LOD:
            samplerObj->SetLodRange(samplerObj->GetMinLod(), *(const GLfloat*)param);
            break;
        case GL_TEXTURE_LOD_BIAS:
            samplerObj->SetLodBias(*(const GLfloat*)param);
            break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            samplerObj->SetMaxAnisotropy(ReadSamplerScalar(param, isFloat, isUnsignedInteger));
            break;
        case GL_TEXTURE_COMPARE_MODE:
            samplerObj->SetCompareMode(MG_Util::ConvertGLEnumToSamplerCompareMode(*(const GLint*)param));
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            samplerObj->SetSamplerCompareFunc(MG_Util::ConvertGLEnumToSamplerCompareFunc(*(const GLint*)param));
            break;
        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "SetSamplerParam_State",
                                                                           "Invalid pname for sampler parameter"));
        }
    }

    void GetSamplerParam_State(GLuint sampler, GLenum pname, void* params, bool isFloat,
                               bool isUnsignedInteger) {
        if (params == nullptr) return;
        if (!SamplerImpl::ValidateSamplerName(sampler)) return;

        Bool doesSamplerObjectCreated = MG_State::pGLContext->ValidateSamplerObject(sampler);
        if (!doesSamplerObjectCreated) {
            // Create one for compatibility
            MG_State::pGLContext->CreateSamplerObject(sampler);
        }
        auto& samplerObj = MG_State::pGLContext->GetSamplerObject(sampler);
        if (!SamplerImpl::ValidateSamplerObject(sampler)) return;

        using namespace MG_Util;
        switch (pname) {
        case GL_TEXTURE_WRAP_S:
            *(GLuint*)params = MG_Util::ConvertSamplerWrapModeToGLEnum(samplerObj->GetWrapS());
            break;
        case GL_TEXTURE_WRAP_T:
            *(GLuint*)params = MG_Util::ConvertSamplerWrapModeToGLEnum(samplerObj->GetWrapT());
            break;
        case GL_TEXTURE_WRAP_R:
            *(GLuint*)params = MG_Util::ConvertSamplerWrapModeToGLEnum(samplerObj->GetWrapR());
            break;
        case GL_TEXTURE_MIN_FILTER:
            *(GLuint*)params =
                MG_Util::ConvertSamplerFilterModeToGLEnum(samplerObj->GetMinFilter(), samplerObj->GetMipmapMode());
            break;
        case GL_TEXTURE_MAG_FILTER:
            *(GLuint*)params =
                MG_Util::ConvertSamplerFilterModeToGLEnum(samplerObj->GetMagFilter(), SamplerMipmapMode::None);
            break;
        case GL_TEXTURE_MIN_LOD:
            *(GLfloat*)params = samplerObj->GetMinLod();
            break;
        case GL_TEXTURE_MAX_LOD:
            *(GLfloat*)params = samplerObj->GetMaxLod();
            break;
        case GL_TEXTURE_LOD_BIAS:
            *(GLfloat*)params = samplerObj->GetLodBias();
            break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (isFloat) {
                *(GLfloat*)params = samplerObj->GetMaxAnisotropy();
            } else if (isUnsignedInteger) {
                *(GLuint*)params = static_cast<GLuint>(samplerObj->GetMaxAnisotropy());
            } else {
                *(GLint*)params = static_cast<GLint>(samplerObj->GetMaxAnisotropy());
            }
            break;
        case GL_TEXTURE_COMPARE_MODE:
            *(GLuint*)params = MG_Util::ConvertSamplerCompareModeToGLEnum(samplerObj->GetCompareMode());
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            *(GLuint*)params = MG_Util::ConvertSamplerCompareFuncToGLEnum(samplerObj->GetSamplerCompareFunc());
            break;
        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetSamplerParam_State",
                                                                           "Invalid pname for sampler parameter"));
        }
    }

    GLboolean IsSampler_State(GLuint sampler) {
        return MG_State::pGLContext->ValidateSamplerObject(sampler) ? GL_TRUE : GL_FALSE;
    }

    // migrate below functions without "_State" into this section
    void GenSamplers_State(GLsizei count, GLuint* samplers) {
        if (count < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GenSamplers", "count must be non-negative"));
            return;
        }

        static thread_local Vector<GLuint> names;
        MG_State::pGLContext->GenSamplerNames(count, names);
        Memcpy(samplers, names.data(), count * sizeof(GLuint));
    }

    void DeleteSamplers_State(GLsizei count, const GLuint* samplers) {
        if (count < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteSamplers", "count must be non-negative"));
            return;
        }

        for (GLsizei i = 0; i < count; ++i) {
            if (samplers[i] != 0) {
                MG_State::pGLContext->MarkSamplerObjectForDeletion(samplers[i]);
            }
        }
    }

    void CreateSamplers_State(GLsizei n, GLuint* samplers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GenSamplers", "count must be non-negative"));
            return;
        }

        static thread_local Vector<GLuint> names;
        MG_State::pGLContext->GenSamplerNames(n, names);
        Memcpy(samplers, names.data(), n * sizeof(GLuint));
        for (GLsizei i = 0; i < n; ++i) {
            samplers[i] = names[i];
            MG_State::pGLContext->CreateSamplerObject(names[i]);
        }
    }

    void BindSampler_State(GLuint unit, GLuint sampler) {
        MGLOG_D("BindSampler_State: unit = %u, sampler = %u", unit, sampler);
        if (unit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BindSampler", "texture unit out of range"));
            return;
        }

        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject((Int)unit);
        MG_State::pGLContext->NoteTextureUnitTouched((Int)unit);
        if (sampler == 0) {
            textureUnit.SetSamplerObject(nullptr);
        } else {
            // GL 3.3 core 3.8.2: BindSampler on a name GenSamplers never returned - or one already
            // deleted - is INVALID_OPERATION. SamplerParameter* raises INVALID_VALUE for the same
            // name, which is why this cannot go through the shared SamplerImpl validator.
            if (!MG_State::pGLContext->ValidateSamplerName(sampler)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BindSampler_State",
                                                 std::format("Invalid sampler name {}", sampler)));
                return;
            }
            Bool doesSamplerObjectCreated = MG_State::pGLContext->ValidateSamplerObject(sampler);
            if (!doesSamplerObjectCreated) {
                MG_State::pGLContext->CreateSamplerObject(sampler);
            }
            auto& samplerObject = MG_State::pGLContext->GetSamplerObject(sampler);

            textureUnit.SetSamplerObject(MG_State::pGLContext->GetSamplerObject(sampler));
        }
    }

    void BindSamplers_State(GLuint first, GLsizei count, const GLuint* samplers) {
        if (count < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BindSamplers", "count must be non-negative"));
            return;
        }

        for (GLsizei i = 0; i < count; ++i) {
            BindSampler_State(first + i, samplers ? samplers[i] : 0);
        }
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void GetSamplerParameteriv(GLuint sampler, GLenum pname, GLint* params) {
        GetSamplerParam_State(sampler, pname, params, false, false);
    }

    void SamplerParameterIuiv(GLuint sampler, GLenum pname, const GLuint* param) {
        SetSamplerParam_State(sampler, pname, param, false, true);
    }

    void SamplerParameterIiv(GLuint sampler, GLenum pname, const GLint* param) {
        SetSamplerParam_State(sampler, pname, param, false, false);
    }

    void SamplerParameteriv(GLuint sampler, GLenum pname, const GLint* param) {
        SetSamplerParam_State(sampler, pname, param, false, false);
    }

    void SamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat* param) {
        SetSamplerParam_State(sampler, pname, param, true, false);
    }

    void SamplerParameteri(GLuint sampler, GLenum pname, GLint param) {
        SamplerParameteriv(sampler, pname, &param);
    }

    void SamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) {
        SamplerParameterfv(sampler, pname, &param);
    }

    GLboolean IsSampler(GLuint sampler) {
        return IsSampler_State(sampler);
    }

    void GetSamplerParameterIuiv(GLuint sampler, GLenum pname, GLuint* params) {
        GetSamplerParam_State(sampler, pname, params, false, true);
    }

    void GetSamplerParameterIiv(GLuint sampler, GLenum pname, GLint* params) {
        GetSamplerParam_State(sampler, pname, params, false, false);
    }

    void GetSamplerParameterfv(GLuint sampler, GLenum pname, GLfloat* params) {
        GetSamplerParam_State(sampler, pname, params, true, false);
    }

    void GenSamplers(GLsizei count, GLuint* samplers) {
        GenSamplers_State(count, samplers);
    }

    void DeleteSamplers(GLsizei count, const GLuint* samplers) {
        DeleteSamplers_State(count, samplers);
    }

    void CreateSamplers(GLsizei n, GLuint* samplers) {
        CreateSamplers_State(n, samplers);
    }

    void BindSamplers(GLuint first, GLsizei count, const GLuint* samplers) {
        BindSamplers_State(first, count, samplers);
    }

    void BindSampler(GLuint unit, GLuint sampler) {
        BindSampler_State(unit, sampler);
    }
} // namespace MobileGL::MG_Impl::GLImpl
