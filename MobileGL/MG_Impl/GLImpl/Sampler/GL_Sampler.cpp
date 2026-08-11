// MobileGL - MobileGL/MG_Impl/GLImpl/Sampler/GL_Sampler.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Sampler.h"
#include "Validators.h"
#include "../Getter/GL_Getter.h"
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
            // Four components, and GL puts no range on them - a border colour outside [0,1] is
            // clamped when a fixed-point format is sampled, not rejected here. The scalar readers
            // below would look at one component and invent an error.
            case GL_TEXTURE_BORDER_COLOR:
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
        case GL_TEXTURE_BORDER_COLOR:
            // The only four-component sampler parameter: the caller's form decides which
            // representation is authoritative, and SamplerObject keeps the other two in step.
            if (isFloat) {
                const auto* values = (const GLfloat*)param;
                samplerObj->SetBorderColor(FloatVec4(values[0], values[1], values[2], values[3]));
            } else if (isUnsignedInteger) {
                const auto* values = (const GLuint*)param;
                samplerObj->SetBorderColorUI(UintVec4(values[0], values[1], values[2], values[3]));
            } else {
                const auto* values = (const GLint*)param;
                samplerObj->SetBorderColorI(IntVec4(values[0], values[1], values[2], values[3]));
            }
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
        case GL_TEXTURE_BORDER_COLOR: {
            if (isFloat) {
                const auto& color = samplerObj->GetBorderColor();
                auto* out = (GLfloat*)params;
                out[0] = color.x();
                out[1] = color.y();
                out[2] = color.z();
                out[3] = color.w();
            } else if (isUnsignedInteger) {
                const auto& color = samplerObj->GetBorderColorUI();
                auto* out = (GLuint*)params;
                out[0] = color.x();
                out[1] = color.y();
                out[2] = color.z();
                out[3] = color.w();
            } else {
                const auto& color = samplerObj->GetBorderColorI();
                auto* out = (GLint*)params;
                out[0] = color.x();
                out[1] = color.y();
                out[2] = color.z();
                out[3] = color.w();
            }
            break;
        }
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
        // Unlike textures/buffers, glGenSamplers CREATES the sampler objects: each name
        // is immediately a sampler (glIsSampler == GL_TRUE before any bind).
        for (GLsizei i = 0; i < count; ++i) {
            MG_State::pGLContext->CreateSamplerObject(names[i]);
        }
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

    // The number of texture units a sampler may be bound to. GL 3.3 core 3.8.2 names
    // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, which is what the backend advertises; the frontend's
    // MAX_TEXTURE_IMAGE_UNITS is only the capacity of the unit array, so it is a clamp on the
    // answer and never the answer itself - gating on it alone accepts every unit up to 192 no
    // matter what the driver reports.
    static GLint GetSamplerBindableTextureUnitCount() {
        GLint maxTextureUnits = 0;
        GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
        return std::min<GLint>(std::max(maxTextureUnits, 0), MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
    }

    void BindSampler_State(GLuint unit, GLuint sampler) {
        MGLOG_D("BindSampler_State: unit = %u, sampler = %u", unit, sampler);
        if (static_cast<Uint64>(unit) >= static_cast<Uint64>(GetSamplerBindableTextureUnitCount())) {
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
        // ARB_multi_bind: the whole [first, first + count) range is checked up front and a
        // range that runs past the last texture unit is INVALID_OPERATION - not the
        // INVALID_VALUE the single-bind BindSampler_State reports per element, and nothing is
        // bound when it fails. Both gates read the same limit (see
        // GetSamplerBindableTextureUnitCount), so an out-of-range multi-bind can no longer slip
        // past this check and be caught one element at a time with the wrong error class.
        const GLint maxTextureUnits = GetSamplerBindableTextureUnitCount();
        if (static_cast<Uint64>(first) + static_cast<Uint64>(count) > static_cast<Uint64>(maxTextureUnits)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BindSamplers",
                                             "first + count exceeds the number of texture units."));
            return;
        }

        // ARB_multi_bind adds one rule the single-bind path does not have: "samplers will not be
        // created if they do not exist", so a name that is not an existing sampler OBJECT is
        // INVALID_OPERATION here (KHR-GL44.multi_bind.errors_bind_samplers). Per element, not
        // all-or-nothing - the extension defines glBindSamplers as a loop, so a bad entry costs
        // its own texture unit and leaves the rest of the range bound.
        for (GLsizei i = 0; i < count; ++i) {
            const GLuint sampler = samplers ? samplers[i] : 0;
            if (sampler != 0 && !MG_State::pGLContext->ValidateSamplerObject(sampler)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", "BindSamplers",
                        std::format("samplers[{}] ({}) is not the name of an existing sampler object.", i, sampler)));
                continue;
            }
            BindSampler_State(first + i, sampler);
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
