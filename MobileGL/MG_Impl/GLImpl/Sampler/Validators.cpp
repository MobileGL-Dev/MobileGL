// MobileGL - MobileGL/MG_Impl/GLImpl/Sampler/Validators.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Validators.h"
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl::SamplerImpl {
    Bool ValidateSamplerName(GLuint sampler) {
        if (!MG_State::pGLContext->ValidateSamplerName(sampler)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerName",
                                                                      std::format("Invalid sampler name {}", sampler)));
            return false;
        }
        return true;
    }

    Bool ValidateSamplerObject(GLuint sampler) {
        if (!MG_State::pGLContext->ValidateSamplerObject(sampler)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerObject",
                                             std::format("Sampler object {} does not exist", sampler)));
            return false;
        }
        return true;
    }

    Bool ValidateSamplerParam(GLenum pname, GLenum param) {
        using namespace MG_Util;
        switch (pname) {
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_WRAP_R:
            if (MG_Util::ConvertGLEnumToSamplerWrapMode(param) == SamplerWrapMode::Unknown) {
                MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                                  MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerParam",
                                                                               "Invalid wrap mode parameter"));
                return false;
            }
            break;

        case GL_TEXTURE_MIN_FILTER:
            if (MG_Util::ConvertGLEnumToSamplerFilterMode(param) == SamplerFilterMode::Unknown) {
                MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                                  MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerParam",
                                                                               "Invalid min filter parameter"));
                return false;
            }
            break;

        case GL_TEXTURE_MAG_FILTER:
            if (param != GL_NEAREST && param != GL_LINEAR) {
                MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                                  MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerParam",
                                                                               "Invalid mag filter parameter"));
                return false;
            }
            break;

        case GL_TEXTURE_COMPARE_MODE:
            if (param != GL_NONE && param != GL_COMPARE_REF_TO_TEXTURE) {
                MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                                  MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerParam",
                                                                               "Invalid compare mode parameter"));
                return false;
            }
            break;

        case GL_TEXTURE_COMPARE_FUNC:
            // The eight depth-compare functions are contiguous from GL_NEVER (0x0200) to
            // GL_ALWAYS (0x0207); GL_LEQUAL sits in the middle of that block, so starting
            // the range there rejected NEVER/LESS/EQUAL and let GREATER/NOTEQUAL/GEQUAL
            // through only by accident of them being above LEQUAL.
            if (param < GL_NEVER || param > GL_ALWAYS) {
                MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                                  MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerParam",
                                                                               "Invalid compare function parameter"));
                return false;
            }
            break;

        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerParam",
                                                                           "Invalid pname for sampler parameter"));
            return false;
        }
        return true;
    }

    Bool ValidateSamplerFloatParam(GLenum pname, GLfloat param) {
        switch (pname) {
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
        case GL_TEXTURE_LOD_BIAS:
            return true;

        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (!(param >= 1.0f)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerFloatParam",
                                                 "GL_TEXTURE_MAX_ANISOTROPY_EXT must be at least 1.0."));
                return false;
            }
            return true;

        case GL_TEXTURE_BORDER_COLOR:
            if (param < 0.0f || param > 1.0f) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerFloatParam",
                                                                          "Border color component out of [0,1] range"));
                return false;
            }
            return true;

        default:
            return ValidateSamplerParam(pname, static_cast<GLenum>(param));
        }
    }

    Bool ValidateSamplerIntParam(GLenum pname, GLint param) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR:
            if (param < 0 || param > 255) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerIntParam",
                                                 "Border color component out of [0,255] range"));
                return false;
            }
            return true;

        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (param < 1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateSamplerIntParam",
                                                 "GL_TEXTURE_MAX_ANISOTROPY_EXT must be at least 1."));
                return false;
            }
            return true;

        default:
            return ValidateSamplerParam(pname, static_cast<GLenum>(param));
        }
    }
} // namespace MobileGL::MG_Impl::GLImpl::SamplerImpl
