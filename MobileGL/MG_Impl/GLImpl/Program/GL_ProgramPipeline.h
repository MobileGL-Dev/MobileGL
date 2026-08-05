// MobileGL - MobileGL/MG_Impl/GLImpl/Program/GL_ProgramPipeline.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Impl::GLImpl {
    void GenProgramPipelines(GLsizei n, GLuint* pipelines);
    void CreateProgramPipelines(GLsizei n, GLuint* pipelines);
    void DeleteProgramPipelines(GLsizei n, const GLuint* pipelines);
    void BindProgramPipeline(GLuint pipeline);
    GLboolean IsProgramPipeline(GLuint pipeline);
    void GetProgramPipelineiv(GLuint pipeline, GLenum pname, GLint* params);
    void GetProgramPipelineInfoLog(GLuint pipeline, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
    void UseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program);
    void ActiveShaderProgram(GLuint pipeline, GLuint program);
    void ValidateProgramPipeline(GLuint pipeline);
} // namespace MobileGL::MG_Impl::GLImpl
