// MobileGL - MobileGL/MG_Impl/GLImpl/RenderState/GL_RenderState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Impl::GLImpl {
    /* @INSERTION_POINT:FUNCTION_DECLARATION@ */
    void BlendEquationi(GLuint buf, GLenum mode);
    void BlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha);
    void BlendFunci(GLuint buf, GLenum src, GLenum dst);
    void BlendFuncSeparatei(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    void GetBooleani_v(GLenum target, GLuint index, GLboolean* data);
    void Disablei(GLenum target, GLuint index);
    void Enablei(GLenum target, GLuint index);
    void BlendFunc(GLenum sfactor, GLenum dfactor);
    void Viewport(GLint x, GLint y, GLsizei width, GLsizei height);
    void StencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
    void StencilOp(GLenum fail, GLenum zfail, GLenum zpass);
    void StencilMaskSeparate(GLenum face, GLuint mask);
    void StencilMask(GLuint mask);
    void StencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
    void StencilFunc(GLenum func, GLint ref, GLuint mask);
    void Scissor(GLint x, GLint y, GLsizei width, GLsizei height);
    void SampleCoverage(GLfloat value, GLboolean invert);
    void PolygonOffset(GLfloat factor, GLfloat units);
    void PolygonMode(GLenum face, GLenum mode);
    void PointSize(GLfloat size);
    void PointParameterf(GLenum pname, GLfloat param);
    void PointParameteri(GLenum pname, GLint param);
    void PointParameterfv(GLenum pname, const GLfloat* params);
    void PointParameteriv(GLenum pname, const GLint* params);
    void PixelStorei(GLenum pname, GLint param);
    void PixelStoref(GLenum pname, GLfloat param);
    void LogicOp(GLenum opcode);
    void LineWidth(GLfloat width);
    GLboolean IsEnabledi(GLenum target, GLuint index);
    GLboolean IsEnabled(GLenum cap);
    void Hint(GLenum target, GLenum mode);
    void FrontFace(GLenum mode);
    void ProvokingVertex(GLenum mode);
    void Enable(GLenum cap);
    void Disable(GLenum cap);
    void DepthRange(GLclampd near_val, GLclampd far_val);
    void DepthMask(GLboolean flag);
    void DepthFunc(GLenum func);
    void CullFace(GLenum mode);
    void ColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void ColorMaski(GLuint index, GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void ClampColor(GLenum target, GLenum clamp);
    void PrimitiveRestartIndex(GLuint index);
    void BlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
    void BlendEquation(GLenum mode);
    void BlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
    void BlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void ClearStencil(GLint s);
    void ClearDepth(GLclampd depth);
    void ClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
} // namespace MobileGL::MG_Impl::GLImpl
