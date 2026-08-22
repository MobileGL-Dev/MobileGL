// MobileGL - MobileGL/MG_Impl/GLImpl/Debug/GL_Debug.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Impl::GLImpl {
    // KHR_debug, core since GL 4.3 (GL 4.6 core 20). Applications use these to annotate a capture
    // and to name their objects; Better Clouds calls all four for exactly that.
    //
    // MobileGL implements the STATE and the ERRORS, and deliberately does not forward the calls to
    // the host driver. Two independent reasons:
    //
    //  * glObjectLabel names a FRONTEND object. MobileGL's texture 5 is not the ES driver's
    //    texture 5 (and under DirectVulkan it is not a driver object at all), so forwarding the
    //    pair verbatim would label an unrelated object or a nonexistent one - worse than not
    //    labelling.
    //  * A debug GROUP is only meaningful if it brackets the commands the application issued
    //    inside it. Neither backend emits its work at the moment the GL call arrives: DirectGLES
    //    defers and reorders state sync and uploads around draws, and DirectVulkan is usually not
    //    even recording a command buffer here. A forwarded push/pop would therefore enclose the
    //    wrong commands, which is a misleading capture rather than a helpful one.
    //
    // What the application can rely on is the observable contract: the group stack depth is real
    // (GL_DEBUG_GROUP_STACK_DEPTH tracks it, and over/underflow raise the errors KHR_debug
    // specifies), and a label written with glObjectLabel comes back from glGetObjectLabel.
    void PushDebugGroup(GLenum source, GLuint id, GLsizei length, const GLchar* message);
    void PopDebugGroup();
    void DebugMessageInsert(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                            const GLchar* buf);
    void ObjectLabel(GLenum identifier, GLuint name, GLsizei length, const GLchar* label);
    void GetObjectLabel(GLenum identifier, GLuint name, GLsizei bufSize, GLsizei* length, GLchar* label);

    // Current depth of the debug group stack, for GL_DEBUG_GROUP_STACK_DEPTH. The base group the
    // context is created with counts, so this is never below 1 (GL 4.6 core 20.6).
    GLint GetDebugGroupStackDepth();
} // namespace MobileGL::MG_Impl::GLImpl
