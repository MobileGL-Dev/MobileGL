// MobileGL - MobileGL/MG_Impl/GLImpl/RenderState/GL_RenderState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_RenderState.h"
#include <cmath>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/RenderStateEnumConverter.h>
#include <MG_Util/Converters/MGToGL/RenderStateEnumConverter.h>
#include <MG_Util/Converters/MGToStr/RenderStateEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    static Float ClampUnitFloat(GLfloat value) {
        return std::clamp(static_cast<Float>(value), 0.0f, 1.0f);
    }

    static Bool ValidateIndexedBlendCapability(GLenum target, GLuint index, const char* functionName) {
        if (target != GL_BLEND) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "Only GL_BLEND is supported for indexed capability state."));
            return false;
        }

        if (index >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", functionName,
                    "Buffer index " + std::to_string(index) + " is out of range. Max supported is " +
                        std::to_string(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS - 1) + "."));
            return false;
        }

        return true;
    }

    static Bool TryConvertBlendEquation(GLenum mode, const char* functionName,
                                        ::MobileGL::BlendEquation& outEquation) {
        outEquation = MG_Util::ConvertGLEnumToBlendEquation(mode);
        if (outEquation != ::MobileGL::BlendEquation::Unknown) return true;

        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidEnum,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                         "Blend equation enum " + MG_Util::ConvertGLEnumToString(mode) +
                                             " is not supported."));
        return false;
    }

    static Bool TryDecodeStencilFace(GLenum face, const char* functionName, Bool& applyFront, Bool& applyBack) {
        switch (face) {
        case GL_FRONT:
            applyFront = true;
            applyBack = false;
            return true;
        case GL_BACK:
            applyFront = false;
            applyBack = true;
            return true;
        case GL_FRONT_AND_BACK:
            applyFront = true;
            applyBack = true;
            return true;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "Stencil face enum " + MG_Util::ConvertGLEnumToString(face) +
                                                 " is not supported."));
            return false;
        }
    }

    static Bool TryConvertStencilOperation(GLenum value, const char* functionName, const char* paramName,
                                           StencilOperation& outOperation) {
        outOperation = MG_Util::ConvertGLEnumToStencilOperation(value);
        if (outOperation != StencilOperation::Unknown) return true;

        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidEnum,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                         String(paramName) + " enum " + MG_Util::ConvertGLEnumToString(value) +
                                             " is not supported."));
        return false;
    }

    void Viewport_State(GLint x, GLint y, GLsizei width, GLsizei height) {
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "Viewport_State",
                                                                           "Width abd height must be non-negative."));
            return;
        }

        MG_State::pGLContext->SetViewport(IntVec4(x, y, width, height));
    }

    void StencilOpSeparate_State(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
        Bool applyFront = false;
        Bool applyBack = false;
        if (!TryDecodeStencilFace(face, "StencilOpSeparate_State", applyFront, applyBack)) return;

        StencilOperation failOp = StencilOperation::Unknown;
        StencilOperation depthFailOp = StencilOperation::Unknown;
        StencilOperation depthPassOp = StencilOperation::Unknown;
        if (!TryConvertStencilOperation(sfail, "StencilOpSeparate_State", "sfail", failOp) ||
            !TryConvertStencilOperation(dpfail, "StencilOpSeparate_State", "dpfail", depthFailOp) ||
            !TryConvertStencilOperation(dppass, "StencilOpSeparate_State", "dppass", depthPassOp)) {
            return;
        }

        if (applyFront) {
            MG_State::pGLContext->SetStencilOp(StencilFace::Front, failOp, depthFailOp, depthPassOp);
        }
        if (applyBack) {
            MG_State::pGLContext->SetStencilOp(StencilFace::Back, failOp, depthFailOp, depthPassOp);
        }
    }

    void StencilOp_State(GLenum fail, GLenum zfail, GLenum zpass) {
        StencilOpSeparate_State(GL_FRONT_AND_BACK, fail, zfail, zpass);
    }

    void StencilMaskSeparate_State(GLenum face, GLuint mask) {
        Bool applyFront = false;
        Bool applyBack = false;
        if (!TryDecodeStencilFace(face, "StencilMaskSeparate_State", applyFront, applyBack)) return;

        if (applyFront) {
            MG_State::pGLContext->SetStencilMask(StencilFace::Front, mask);
        }
        if (applyBack) {
            MG_State::pGLContext->SetStencilMask(StencilFace::Back, mask);
        }
    }

    void StencilMask_State(GLuint mask) {
        StencilMaskSeparate_State(GL_FRONT_AND_BACK, mask);
    }

    void StencilFuncSeparate_State(GLenum face, GLenum func, GLint ref, GLuint mask) {
        Bool applyFront = false;
        Bool applyBack = false;
        if (!TryDecodeStencilFace(face, "StencilFuncSeparate_State", applyFront, applyBack)) return;

        DepthTestFunc depthFunc = MG_Util::ConvertGLEnumToDepthTestFunc(func);
        if (depthFunc == DepthTestFunc::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "StencilFuncSeparate_State",
                                             "Stencil func enum " + MG_Util::ConvertGLEnumToString(func) +
                                                 " is not supported."));
            return;
        }

        const Int clampedRef = std::max(ref, 0);
        if (applyFront) {
            MG_State::pGLContext->SetStencilFunc(StencilFace::Front, depthFunc, clampedRef, mask);
        }
        if (applyBack) {
            MG_State::pGLContext->SetStencilFunc(StencilFace::Back, depthFunc, clampedRef, mask);
        }
    }

    void StencilFunc_State(GLenum func, GLint ref, GLuint mask) {
        StencilFuncSeparate_State(GL_FRONT_AND_BACK, func, ref, mask);
    }

    void Scissor_State(GLint x, GLint y, GLsizei width, GLsizei height) {
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "Scissor_State",
                                                                           "Width abd height must be non-negative."));
            return;
        }

        MG_State::pGLContext->SetScissorBox(IntVec4(x, y, width, height));
    }

    void SampleCoverage_State(GLfloat value, GLboolean invert) {
        MG_State::pGLContext->SetSampleCoverage(std::clamp(static_cast<Float>(value), 0.0f, 1.0f), invert == GL_TRUE);
    }

    void PolygonOffset_State(GLfloat factor, GLfloat units) {
        MG_State::pGLContext->SetPolygonOffset(static_cast<Float>(factor), static_cast<Float>(units));
    }

    void PolygonMode_State(GLenum face, GLenum mode) {
        // GL 3.3 core: separate front/back polygon modes were removed in 3.1, so the only legal
        // face is GL_FRONT_AND_BACK. GL_FRONT / GL_BACK must be rejected (some desktop drivers
        // leniently accept them, but that is non-conformant). Both errors are GL_INVALID_ENUM and
        // leave state untouched.
        if (face != GL_FRONT_AND_BACK) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "glPolygonMode face must be GL_FRONT_AND_BACK in the core profile; got " +
                                                 MG_Util::ConvertGLEnumToString(face) + "."));
            return;
        }
        if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "glPolygonMode mode must be GL_POINT, GL_LINE, or GL_FILL; got " +
                                                 MG_Util::ConvertGLEnumToString(mode) + "."));
            return;
        }
        // Core sets both faces together; keep two slots so GL_POLYGON_MODE round-trips its two values.
        MG_State::pGLContext->SetPolygonMode(mode, mode);
    }

    void PointSize_State(GLfloat size) {
        if (size <= 0.0f) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "PointSize_State",
                                             "Point size must be greater than zero."));
            return;
        }

        MG_State::pGLContext->SetPointSize(static_cast<Float>(size));
    }

    // Single funnel for all four glPointParameter forms. The two GL 3.3 core pnames both carry one
    // component, so the *v forms pass params[0], and the integer forms widen to float. The
    // GL_POINT_SPRITE_COORD_ORIGIN value is an enum passed as a float (36001.0/36002.0 are exact).
    void PointParameter_State(GLenum pname, GLfloat value) {
        switch (pname) {
        case GL_POINT_FADE_THRESHOLD_SIZE:
            if (value < 0.0f) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "GL_POINT_FADE_THRESHOLD_SIZE must be non-negative."));
                return;
            }
            MG_State::pGLContext->SetPointFadeThresholdSize(value);
            return;
        case GL_POINT_SPRITE_COORD_ORIGIN: {
            const GLenum origin = static_cast<GLenum>(std::lround(value));
            if (origin != GL_LOWER_LEFT && origin != GL_UPPER_LEFT) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "GL_POINT_SPRITE_COORD_ORIGIN must be GL_LOWER_LEFT or "
                                                 "GL_UPPER_LEFT."));
                return;
            }
            MG_State::pGLContext->SetPointSpriteCoordOrigin(origin);
            return;
        }
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Unsupported point parameter pname: " + std::to_string(pname)));
            return;
        }
    }

    void PointParameterf_State(GLenum pname, GLfloat param) { PointParameter_State(pname, param); }

    void PointParameteri_State(GLenum pname, GLint param) {
        PointParameter_State(pname, static_cast<GLfloat>(param));
    }

    void PointParameterfv_State(GLenum pname, const GLfloat* params) {
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null."));
            return;
        }
        PointParameter_State(pname, params[0]);
    }

    void PointParameteriv_State(GLenum pname, const GLint* params) {
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null."));
            return;
        }
        PointParameter_State(pname, static_cast<GLfloat>(params[0]));
    }

    void PixelStorei_State(GLenum pname, GLint param) {
        MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), param);
        PixelStoreParam pixelStoreParam = MG_Util::ConvertGLEnumToPixelStoreParam(pname);
        if (pixelStoreParam == PixelStoreParam::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "PixelStorei_State",
                                             "Pixel store param enum " +
                                                 MG_Util::ConvertPixelStoreParamToString(pixelStoreParam) + "(" +
                                                 MG_Util::ConvertGLEnumToString(pname) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetPixelStoreParam(pixelStoreParam, param);
    }

    void LogicOp_State(GLenum opcode) {
        LogicOperation logicOp = MG_Util::ConvertGLEnumToLogicOperation(opcode);
        if (logicOp == LogicOperation::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "LogicOp_State",
                                             "Logic op enum " + MG_Util::ConvertGLEnumToString(opcode) +
                                                 " is not supported."));
            return;
        }

        MG_State::pGLContext->SetLogicOp(logicOp);
    }

    void LineWidth_State(GLfloat width) {
        if (width <= 0.0f) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "LineWidth_State",
                                             "Line width must be greater than zero."));
            return;
        }

        MG_State::pGLContext->SetLineWidth(static_cast<Float>(width));
    }

    GLboolean IsEnabledi_State(GLenum target, GLuint index) {
        if (!ValidateIndexedBlendCapability(target, index, "IsEnabledi_State")) {
            return GL_FALSE;
        }

        CapabilityInput capInput = MG_Util::ConvertGLEnumToCapabilityInput(target);
        if (capInput == CapabilityInput::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "IsEnabledi_State",
                                             "Capability enum " + MG_Util::ConvertCapabilityInputToString(capInput) +
                                                 "(" + MG_Util::ConvertGLEnumToString(target) + ") is not supported."));
            return GL_FALSE;
        }

        return MG_State::pGLContext->IsCapabilityEnabledIndexed(capInput, index) ? GL_TRUE : GL_FALSE;
    }

    void GetBooleani_v_State(GLenum target, GLuint index, GLboolean* data) {
        if (!data) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetBooleani_v_State",
                                             "data pointer cannot be null."));
            return;
        }

        if (target == GL_COLOR_WRITEMASK) {
            // Indexed per-draw-buffer color writemask writes 4 booleans (R,G,B,A) for draw buffer
            // `index`. The non-indexed glGetBooleanv(GL_COLOR_WRITEMASK) reports draw buffer 0.
            if (index >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetBooleani_v_State",
                                                 "Color writemask draw buffer index " + std::to_string(index) +
                                                     " is out of range."));
                return;
            }
            const BoolVec4 mask = MG_State::pGLContext->GetColorMaskIndexed(index);
            data[0] = mask.x() ? GL_TRUE : GL_FALSE;
            data[1] = mask.y() ? GL_TRUE : GL_FALSE;
            data[2] = mask.z() ? GL_TRUE : GL_FALSE;
            data[3] = mask.w() ? GL_TRUE : GL_FALSE;
            return;
        }

        *data = IsEnabledi_State(target, index);
    }

    GLboolean IsEnabled_State(GLenum cap) {
        CapabilityInput capInput = MG_Util::ConvertGLEnumToCapabilityInput(cap);
        if (capInput == CapabilityInput::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "IsEnabled_State",
                                             "Capability enum " + MG_Util::ConvertCapabilityInputToString(capInput) +
                                                 "(" + MG_Util::ConvertGLEnumToString(cap) + ") is not supported."));
            return GL_FALSE;
        }

        return MG_State::pGLContext->IsCapabilityEnabled(capInput) ? GL_TRUE : GL_FALSE;
    }

    void Hint_State(GLenum target, GLenum mode) {
        switch (target) {
        case GL_LINE_SMOOTH_HINT:
        case GL_POLYGON_SMOOTH_HINT:
        case GL_TEXTURE_COMPRESSION_HINT:
        case GL_FRAGMENT_SHADER_DERIVATIVE_HINT:
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Unsupported hint target: " + std::to_string(target)));
            return;
        }
        if (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Hint mode must be GL_FASTEST, GL_NICEST or GL_DONT_CARE."));
            return;
        }
        MG_State::pGLContext->SetHint(target, mode);
    }

    void FrontFace_State(GLenum mode) {
        FrontFaceMode frontFaceMode = MG_Util::ConvertGLEnumToFrontFaceMode(mode);
        if (frontFaceMode == FrontFaceMode::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FrontFace_State",
                                             "Front face mode enum " +
                                                 MG_Util::ConvertFrontFaceModeToString(frontFaceMode) + "(" +
                                                 MG_Util::ConvertGLEnumToString(mode) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetFrontFaceMode(frontFaceMode);
    }

    void ProvokingVertex_State(GLenum mode) {
        ProvokingVertexMode provokingVertexMode = MG_Util::ConvertGLEnumToProvokingVertexMode(mode);
        if (provokingVertexMode == ProvokingVertexMode::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ProvokingVertex_State",
                                             "Provoking vertex mode enum " +
                                                 MG_Util::ConvertProvokingVertexModeToString(provokingVertexMode) +
                                                 "(" + MG_Util::ConvertGLEnumToString(mode) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetProvokingVertexMode(provokingVertexMode);
    }

    void Enable_State(GLenum cap) {
        switch (cap) {
        case GL_TEXTURE_1D:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_CUBE_MAP:
            return;
        default:
            break;
        }

        CapabilityInput capInput = MG_Util::ConvertGLEnumToCapabilityInput(cap);
        if (capInput == CapabilityInput::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "Enable_State",
                                             "Capability enum " + MG_Util::ConvertCapabilityInputToString(capInput) +
                                                 "(" + MG_Util::ConvertGLEnumToString(cap) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetCapability(capInput, true);
    }

    void Disable_State(GLenum cap) {
        switch (cap) {
        case GL_TEXTURE_1D:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_CUBE_MAP:
            return;
        default:
            break;
        }

        CapabilityInput capInput = MG_Util::ConvertGLEnumToCapabilityInput(cap);
        if (capInput == CapabilityInput::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "Disable_State",
                                             "Capability enum " + MG_Util::ConvertCapabilityInputToString(capInput) +
                                                 "(" + MG_Util::ConvertGLEnumToString(cap) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetCapability(capInput, false);
    }

    void DepthRange_State(GLclampd near_val, GLclampd far_val) {
        MG_State::pGLContext->SetDepthRange(
            FloatVec2(ClampUnitFloat(static_cast<GLfloat>(near_val)), ClampUnitFloat(static_cast<GLfloat>(far_val))));
    }

    void DepthMask_State(GLboolean flag) {
        MG_State::pGLContext->SetDepthMask(flag == GL_TRUE);
    }

    void DepthFunc_State(GLenum func) {
        DepthTestFunc depthFunc = MG_Util::ConvertGLEnumToDepthTestFunc(func);
        if (depthFunc == DepthTestFunc::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DepthFunc_State",
                                             "Depth function enum " + MG_Util::ConvertDepthTestFuncToString(depthFunc) +
                                                 "(" + MG_Util::ConvertGLEnumToString(func) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetDepthFunc(depthFunc);
    }

    void CullFace_State(GLenum mode) {
        CullFaceMode cullFaceMode = MG_Util::ConvertGLEnumToCullFaceMode(mode);
        if (cullFaceMode == CullFaceMode::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CullFace_State",
                                             "Cull face mode enum " +
                                                 MG_Util::ConvertCullFaceModeToString(cullFaceMode) + "(" +
                                                 MG_Util::ConvertGLEnumToString(mode) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetCullFaceMode(cullFaceMode);
    }

    void ColorMask_State(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
        // glColorMask broadcasts to every draw buffer. GLboolean coercion: any nonzero value enables
        // the component; only exactly GL_FALSE disables it.
        MG_State::pGLContext->SetColorMask(
            BoolVec4(red != GL_FALSE, green != GL_FALSE, blue != GL_FALSE, alpha != GL_FALSE));
    }

    void ColorMaski_State(GLuint buf, GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
        // Indexed color writemask for the single draw buffer `buf`. buf is a GLuint index, never an
        // enum, so the only error is GL_INVALID_VALUE when it is out of range (mirrors the indexed
        // blend entry points, which bound against MAX_DRAW_BUFFERS).
        if (buf >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "glColorMaski buffer index " + std::to_string(buf) + " is out of range. Max supported is " +
                        std::to_string(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS - 1) + "."));
            return;
        }
        MG_State::pGLContext->SetColorMaskIndexed(
            buf, BoolVec4(red != GL_FALSE, green != GL_FALSE, blue != GL_FALSE, alpha != GL_FALSE));
    }

    void ClampColor_State(GLenum target, GLenum clamp) {
        // GL 3.3 core: the only legal target is GL_CLAMP_READ_COLOR. The compatibility-only
        // GL_CLAMP_VERTEX_COLOR / GL_CLAMP_FRAGMENT_COLOR were removed from the core profile and
        // must be rejected.
        if (target != GL_CLAMP_READ_COLOR) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "glClampColor target must be GL_CLAMP_READ_COLOR in the core profile; "
                                             "got " +
                                                 MG_Util::ConvertGLEnumToString(target) + "."));
            return;
        }
        // clamp must be one of GL_TRUE, GL_FALSE, or GL_FIXED_ONLY. NOTE: the Khronos man page's
        // Errors section wrongly omits GL_FIXED_ONLY, but the spec lists it as legal AND it is the
        // default value, so it must be accepted here.
        if (clamp != GL_TRUE && clamp != GL_FALSE && clamp != GL_FIXED_ONLY) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "glClampColor clamp must be GL_TRUE, GL_FALSE, or GL_FIXED_ONLY; got " +
                                                 MG_Util::ConvertGLEnumToString(clamp) + "."));
            return;
        }
        MG_State::pGLContext->SetClampReadColor(clamp);
    }

    void BlendFuncSeparate_State(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
        BlendFactor srcRGB = MG_Util::ConvertGLEnumToBlendFactor(sfactorRGB);
        BlendFactor dstRGB = MG_Util::ConvertGLEnumToBlendFactor(dfactorRGB);
        BlendFactor srcAlpha = MG_Util::ConvertGLEnumToBlendFactor(sfactorAlpha);
        BlendFactor dstAlpha = MG_Util::ConvertGLEnumToBlendFactor(dfactorAlpha);

        if (srcRGB == BlendFactor::Unknown || dstRGB == BlendFactor::Unknown || srcAlpha == BlendFactor::Unknown ||
            dstAlpha == BlendFactor::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BlendFuncSeparate_State",
                                             "One of the blend factor enums is not supported: srcRGB " +
                                                 MG_Util::ConvertBlendFactorToString(srcRGB) + "(" +
                                                 MG_Util::ConvertGLEnumToString(sfactorRGB) + "), dstRGB " +
                                                 MG_Util::ConvertBlendFactorToString(dstRGB) + "(" +
                                                 MG_Util::ConvertGLEnumToString(dfactorRGB) + "), srcAlpha " +
                                                 MG_Util::ConvertBlendFactorToString(srcAlpha) + "(" +
                                                 MG_Util::ConvertGLEnumToString(sfactorAlpha) + "), dstAlpha " +
                                                 MG_Util::ConvertBlendFactorToString(dstAlpha) + "(" +
                                                 MG_Util::ConvertGLEnumToString(dfactorAlpha) + ")."));
            return;
        }

        MG_State::pGLContext->SetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
    }

    void BlendFunc_State(GLenum sfactor, GLenum dfactor) {
        BlendFuncSeparate_State(sfactor, dfactor, sfactor, dfactor);
    }

    void BlendEquation_State(GLenum mode) {
        ::MobileGL::BlendEquation blendEquation = ::MobileGL::BlendEquation::Unknown;
        if (!TryConvertBlendEquation(mode, "BlendEquation_State", blendEquation)) return;
        MG_State::pGLContext->SetBlendEquation(blendEquation, blendEquation);
    }

    void BlendEquationSeparate_State(GLenum modeRGB, GLenum modeAlpha) {
        ::MobileGL::BlendEquation colorEquation = ::MobileGL::BlendEquation::Unknown;
        if (!TryConvertBlendEquation(modeRGB, "BlendEquationSeparate_State", colorEquation)) return;

        ::MobileGL::BlendEquation alphaEquation = ::MobileGL::BlendEquation::Unknown;
        if (!TryConvertBlendEquation(modeAlpha, "BlendEquationSeparate_State", alphaEquation)) return;

        MG_State::pGLContext->SetBlendEquation(colorEquation, alphaEquation);
    }

    void BlendColor_State(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
        MG_State::pGLContext->SetBlendColor(
            FloatVec4(ClampUnitFloat(red), ClampUnitFloat(green), ClampUnitFloat(blue), ClampUnitFloat(alpha)));
    }

    void ClearStencil_State(GLint s) {
        MG_State::pGLContext->SetClearStencil(static_cast<Int>(s));
    }

    void ClearDepth_State(GLclampd depth) {
        MG_State::pGLContext->SetClearDepth(static_cast<Float>(depth));
    }

    void ClearColor_State(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
        MG_State::pGLContext->SetClearColor(FloatVec4(red, green, blue, alpha));
    }

    void BlendFuncSeparatei_State(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
        if (buf >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "BlendFuncSeparatei_State",
                    "Buffer index " + std::to_string(buf) + " is out of range. Max supported is " +
                        std::to_string(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS - 1) + "."));
            return;
        }

        BlendFactor srcRGBM = MG_Util::ConvertGLEnumToBlendFactor(srcRGB);
        BlendFactor dstRGBM = MG_Util::ConvertGLEnumToBlendFactor(dstRGB);
        BlendFactor srcAlphaM = MG_Util::ConvertGLEnumToBlendFactor(srcAlpha);
        BlendFactor dstAlphaM = MG_Util::ConvertGLEnumToBlendFactor(dstAlpha);
        if (srcRGBM == BlendFactor::Unknown || dstRGBM == BlendFactor::Unknown ||
            srcAlphaM == BlendFactor::Unknown || dstAlphaM == BlendFactor::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BlendFuncSeparatei_State",
                                             "One of the indexed blend factor enums is not supported."));
            return;
        }
        MG_State::pGLContext->SetBlendFuncIndexed(buf, srcRGBM, dstRGBM, srcAlphaM, dstAlphaM);
    }

    void BlendFunci_State(GLuint buf, GLenum src, GLenum dst) {
        BlendFuncSeparatei_State(buf, src, dst, src, dst);
    }

    void BlendEquationSeparatei_State(GLuint buf, GLenum modeRGB, GLenum modeAlpha) {
        if (buf >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "BlendEquationSeparatei_State",
                    "Buffer index " + std::to_string(buf) + " is out of range. Max supported is " +
                        std::to_string(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS - 1) + "."));
            return;
        }

        ::MobileGL::BlendEquation colorEquation = ::MobileGL::BlendEquation::Unknown;
        if (!TryConvertBlendEquation(modeRGB, "BlendEquationSeparatei_State", colorEquation)) return;

        ::MobileGL::BlendEquation alphaEquation = ::MobileGL::BlendEquation::Unknown;
        if (!TryConvertBlendEquation(modeAlpha, "BlendEquationSeparatei_State", alphaEquation)) return;

        MG_State::pGLContext->SetBlendEquationIndexed(buf, colorEquation, alphaEquation);
    }

    void BlendEquationi_State(GLuint buf, GLenum mode) {
        BlendEquationSeparatei_State(buf, mode, mode);
    }

    void Disablei_State(GLenum target, GLuint index) {
        if (!ValidateIndexedBlendCapability(target, index, "Disablei_State")) {
            return;
        }

        auto capInput = MG_Util::ConvertGLEnumToCapabilityInput(target);
        if (capInput == CapabilityInput::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "Disablei_State",
                                             "Capability enum " + MG_Util::ConvertCapabilityInputToString(capInput) +
                                                 "(" + MG_Util::ConvertGLEnumToString(target) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetCapabilityIndexed(capInput, index, false);
    }

    void Enablei_State(GLenum target, GLuint index) {
        if (!ValidateIndexedBlendCapability(target, index, "Enablei_State")) {
            return;
        }

        auto capInput = MG_Util::ConvertGLEnumToCapabilityInput(target);
        if (capInput == CapabilityInput::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "Enablei_State",
                                             "Capability enum " + MG_Util::ConvertCapabilityInputToString(capInput) +
                                                 "(" + MG_Util::ConvertGLEnumToString(target) + ") is not supported."));
            return;
        }

        MG_State::pGLContext->SetCapabilityIndexed(capInput, index, true);
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void BlendEquationi(GLuint buf, GLenum mode) {
        BlendEquationi_State(buf, mode);
    }

    void BlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha) {
        BlendEquationSeparatei_State(buf, modeRGB, modeAlpha);
    }

    void BlendFunci(GLuint buf, GLenum src, GLenum dst) {
        BlendFunci_State(buf, src, dst);
    }

    void BlendFuncSeparatei(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
        BlendFuncSeparatei_State(buf, srcRGB, dstRGB, srcAlpha, dstAlpha);
    }

    void GetBooleani_v(GLenum target, GLuint index, GLboolean* data) {
        GetBooleani_v_State(target, index, data);
    }

    void Disablei(GLenum target, GLuint index) {
        Disablei_State(target, index);
    }

    void Enablei(GLenum target, GLuint index) {
        Enablei_State(target, index);
    }

    void BlendFunc(GLenum sfactor, GLenum dfactor) {
        BlendFunc_State(sfactor, dfactor);
    }

    void Viewport(GLint x, GLint y, GLsizei width, GLsizei height) {
        Viewport_State(x, y, width, height);
    }

    void StencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
        StencilOpSeparate_State(face, sfail, dpfail, dppass);
    }

    void StencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
        StencilOp_State(fail, zfail, zpass);
    }

    void StencilMaskSeparate(GLenum face, GLuint mask) {
        StencilMaskSeparate_State(face, mask);
    }

    void StencilMask(GLuint mask) {
        StencilMask_State(mask);
    }

    void StencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
        StencilFuncSeparate_State(face, func, ref, mask);
    }

    void StencilFunc(GLenum func, GLint ref, GLuint mask) {
        StencilFunc_State(func, ref, mask);
    }

    void Scissor(GLint x, GLint y, GLsizei width, GLsizei height) {
        Scissor_State(x, y, width, height);
    }

    void SampleCoverage(GLfloat value, GLboolean invert) {
        SampleCoverage_State(value, invert);
    }

    void PolygonOffset(GLfloat factor, GLfloat units) {
        PolygonOffset_State(factor, units);
    }

    void PolygonMode(GLenum face, GLenum mode) {
        PolygonMode_State(face, mode);
    }

    void PointSize(GLfloat size) {
        PointSize_State(size);
    }

    void PointParameterf(GLenum pname, GLfloat param) {
        PointParameterf_State(pname, param);
    }

    void PointParameteri(GLenum pname, GLint param) {
        PointParameteri_State(pname, param);
    }

    void PointParameterfv(GLenum pname, const GLfloat* params) {
        PointParameterfv_State(pname, params);
    }

    void PointParameteriv(GLenum pname, const GLint* params) {
        PointParameteriv_State(pname, params);
    }

    void PixelStorei(GLenum pname, GLint param) {
        PixelStorei_State(pname, param);
    }

    void PixelStoref(GLenum pname, GLfloat param) {
        // Boolean pixel-store pnames convert by a zero-test (0.4 -> TRUE); integer pnames round to
        // nearest. Branch before converting so a fractional value cannot round a true flag to false.
        GLint intParam;
        switch (pname) {
        case GL_PACK_SWAP_BYTES:
        case GL_UNPACK_SWAP_BYTES:
        case GL_PACK_LSB_FIRST:
        case GL_UNPACK_LSB_FIRST:
            intParam = (param != 0.0f) ? 1 : 0;
            break;
        default:
            intParam = static_cast<GLint>(std::lround(param));
            break;
        }
        PixelStorei_State(pname, intParam);
    }

    void LogicOp(GLenum opcode) {
        LogicOp_State(opcode);
    }

    void LineWidth(GLfloat width) {
        LineWidth_State(width);
    }

    GLboolean IsEnabledi(GLenum target, GLuint index) {
        return IsEnabledi_State(target, index);
    }

    GLboolean IsEnabled(GLenum cap) {
        return IsEnabled_State(cap);
    }

    void Hint(GLenum target, GLenum mode) {
        Hint_State(target, mode);
    }

    void FrontFace(GLenum mode) {
        FrontFace_State(mode);
    }

    void ProvokingVertex(GLenum mode) {
        ProvokingVertex_State(mode);
    }

    void Enable(GLenum cap) {
        Enable_State(cap);
    }

    void Disable(GLenum cap) {
        Disable_State(cap);
    }

    void DepthRange(GLclampd near_val, GLclampd far_val) {
        DepthRange_State(near_val, far_val);
    }

    void DepthMask(GLboolean flag) {
        DepthMask_State(flag);
    }

    void DepthFunc(GLenum func) {
        DepthFunc_State(func);
    }

    void CullFace(GLenum mode) {
        CullFace_State(mode);
    }

    void ColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
        ColorMask_State(red, green, blue, alpha);
    }

    void ColorMaski(GLuint index, GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
        ColorMaski_State(index, red, green, blue, alpha);
    }

    void ClampColor(GLenum target, GLenum clamp) {
        ClampColor_State(target, clamp);
    }

    void BlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
        BlendFuncSeparate_State(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);
    }

    void BlendEquation(GLenum mode) {
        BlendEquation_State(mode);
    }

    void BlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
        BlendEquationSeparate_State(modeRGB, modeAlpha);
    }

    void BlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
        BlendColor_State(red, green, blue, alpha);
    }

    void ClearStencil(GLint s) {
        ClearStencil_State(s);
    }

    void ClearDepth(GLclampd depth) {
        ClearDepth_State(depth);
    }

    void ClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
        ClearColor_State(red, green, blue, alpha);
    }
} // namespace MobileGL::MG_Impl::GLImpl
