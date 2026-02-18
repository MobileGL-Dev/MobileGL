// MobileGL - MobileGL/MG_Impl/GLImpl/Drawing/GL_Drawing.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Drawing.h"
#include <Config.h>
#include <MG_State/GLState/Core.h>
#include <MG_Backend/BackendObjects.h>

namespace MobileGL::MG_Impl::GLImpl {
    void Clear_Backend(GLbitfield mask) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.Clear(mask);
    }

    void DrawElements_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElements(mode, count, type, indices);
    }

    void MultiDrawElements_Backend(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                   GLsizei drawcount) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawElements(mode, count, type, indices, drawcount);
    }

    void MultiDrawElementsBaseVertex_Backend(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                             GLsizei drawcount, const GLint* basevertex) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawElementsBaseVertex(mode, count, type, indices, drawcount,
                                                                          basevertex);
    }

    void DrawArrays_Backend(GLenum mode, GLint first, GLsizei count) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawArrays(mode, first, count);
    }

    void DrawElementsBaseVertex_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                        GLint basevertex) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }

    void MultiDrawElementsIndirect_Backend(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                                           GLsizei stride) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawElementsIndirect(mode, type, indirect, drawcount, stride);
    }

    void MultiDrawArraysIndirect_Backend(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawArraysIndirect(mode, indirect, drawcount, stride);
    }

    void DrawRangeElementsBaseVertex_Backend(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                             const void* indices, GLint basevertex) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawRangeElementsBaseVertex(mode, start, end, count, type, indices,
                                                                          basevertex);
    }

    void DrawRangeElements_Backend(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                   const void* indices) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawRangeElements(mode, start, end, count, type, indices);
    }

    void DrawElementsInstancedBaseVertexBaseInstance_Backend(GLenum mode, GLsizei count, GLenum type,
                                                             const void* indices, GLsizei instancecount,
                                                             GLint basevertex, GLuint baseinstance) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsInstancedBaseVertexBaseInstance(
            mode, count, type, indices, instancecount, basevertex, baseinstance);
    }

    void DrawElementsInstancedBaseVertex_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                 GLsizei instancecount, GLint basevertex) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount,
                                                                              basevertex);
    }

    void DrawElementsInstancedBaseInstance_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                   GLsizei instancecount, GLuint baseinstance) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsInstancedBaseInstance(mode, count, type, indices,
                                                                                instancecount, baseinstance);
    }

    void DrawElementsInstanced_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                       GLsizei instancecount) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsInstanced(mode, count, type, indices, instancecount);
    }

    void DrawElementsIndirect_Backend(GLenum mode, GLenum type, const void* indirect) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsIndirect(mode, type, indirect);
    }
    void DrawArraysInstancedBaseInstance_Backend(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                                 GLuint baseinstance) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawArraysInstancedBaseInstance(mode, first, count, instancecount,
                                                                              baseinstance);
    }

    void DrawArraysInstanced_Backend(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawArraysInstanced(mode, first, count, instancecount);
    }

    void DrawArraysIndirect_Backend(GLenum mode, const void* indirect) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawArraysIndirect(mode, indirect);
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
        MultiDrawElementsIndirect_Backend(mode, type, indirect, drawcount, stride);
    }

    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
        MultiDrawArraysIndirect_Backend(mode, indirect, drawcount, stride);
    }

    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex) {
        DrawRangeElementsBaseVertex_Backend(mode, start, end, count, type, indices, basevertex);
    }

    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
        DrawRangeElements_Backend(mode, start, end, count, type, indices);
    }

    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
        DrawElementsInstancedBaseVertexBaseInstance_Backend(mode, count, type, indices, instancecount, basevertex,
                                                            baseinstance);
    }

    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        DrawElementsInstancedBaseVertex_Backend(mode, count, type, indices, instancecount, basevertex);
    }

    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        DrawElementsInstancedBaseInstance_Backend(mode, count, type, indices, instancecount, baseinstance);
    }

    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
        DrawElementsInstanced_Backend(mode, count, type, indices, instancecount);
    }

    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        DrawElementsIndirect_Backend(mode, type, indirect);
    }

    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {
        DrawArraysInstancedBaseInstance_Backend(mode, first, count, instancecount, baseinstance);
    }

    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        DrawArraysInstanced_Backend(mode, first, count, instancecount);
    }

    void DrawArraysIndirect(GLenum mode, const void* indirect) {
        DrawArraysIndirect_Backend(mode, indirect);
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
        DrawElementsBaseVertex_Backend(mode, count, type, indices, basevertex);
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        DrawArrays_Backend(mode, first, count);
    }

    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                           GLsizei drawcount) {
        MultiDrawElements_Backend(mode, count, type, indices, drawcount);
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {
        MultiDrawElementsBaseVertex_Backend(mode, count, type, indices, drawcount, basevertex);
    }

    void Clear(GLbitfield mask) {
        Clear_Backend(mask);
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        DrawElements_Backend(mode, count, type, indices);
    }

} // namespace MobileGL::MG_Impl::GLImpl
