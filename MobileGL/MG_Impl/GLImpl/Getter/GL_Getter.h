// MobileGL - MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.h
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
    const GLubyte* GetString(GLenum name);
    const GLubyte* GetStringi(GLenum name, GLuint index);
    void GetBooleanv(GLenum pname, GLboolean* params);
    void GetFloatv(GLenum pname, GLfloat* params);
    void GetDoublev(GLenum pname, GLdouble* params);
    void GetIntegerv(GLenum pname, GLint* params);
    void GetInteger64v(GLenum pname, GLint64* params);
    void GetIntegeri_v(GLenum target, GLuint index, GLint* data);
    void GetFloati_v(GLenum target, GLuint index, GLfloat* data);
    void GetDoublei_v(GLenum target, GLuint index, GLdouble* data);
    void GetInteger64i_v(GLenum target, GLuint index, GLint64* data);
    GLenum GetError();
    GLenum GetGraphicsResetStatus();
    // The GL_MAX_SAMPLES value MobileGL advertises, i.e. the driver's value floored to the GL
    // core minimum of 4. This is the RENDERBUFFER ceiling; the three per-category texture
    // ceilings below have a minimum of one and are reported as probed.
    GLint GetAdvertisedMaxSamples();
    // Exactly what GL_MAX_COLOR_TEXTURE_SAMPLES / GL_MAX_DEPTH_TEXTURE_SAMPLES /
    // GL_MAX_INTEGER_SAMPLES report: the probed backend limit floored at the GL 4.6 core minimum
    // of ONE (table 23.53). Exported so the frontend's storage validation enforces exactly what
    // the query promised - it used to floor both at 4 and then let the backend quietly
    // under-allocate whatever the driver could not actually provide.
    GLint GetAdvertisedColorTextureMaxSamples();
    GLint GetAdvertisedDepthTextureMaxSamples();
    GLint GetAdvertisedIntegerMaxSamples();
    // What glGetIntegerv(GL_SAMPLES) answers for the CURRENT draw framebuffer: the largest sample
    // count over its attachments, and 0 for a single-sample or default framebuffer (GL 4.6 core
    // 9.2.3 / 22.2 - GL_SAMPLE_BUFFERS is 1 exactly when this is non-zero).
    //
    // Shared rather than duplicated because two callers need the identical number and disagreeing
    // would be a silent bug: the query itself, and the draw path's write of the reserved
    // gl_NumSamples stand-in - a shader comparing gl_NumSamples against glGetIntegerv(GL_SAMPLES)
    // is exactly what the sample_variables CTS does.
    GLint ResolveDrawFramebufferSampleCount();
} // namespace MobileGL::MG_Impl::GLImpl
