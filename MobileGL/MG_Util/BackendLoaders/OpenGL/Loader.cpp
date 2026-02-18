// MobileGL - MobileGL/MG_Util/BackendLoaders/OpenGL/Loader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Loader.h"
#include "MG_Util/Types.h"

namespace MobileGL::MG_Util::BackendLoader {
    static void* OpenLib(const Vector<String>& names) {
#if !defined(__WIN32) && !defined(_WIN32) && !defined(__APPLE__)
        static const String LibPathPrefixes[] = {
            "/opt/vc/lib/", "/usr/local/lib/", "/usr/lib/", "/usr/lib/x86_64-linux-gnu/",
            "" // We should put this to the end of the list to avoid breaking `LD_LIBRARY_PATH` usage
        };

        void* lib = nullptr;

        Int flags = RTLD_LOCAL | RTLD_NOW;
        for (const auto& prefix : LibPathPrefixes) {
            for (const auto& name : names) {
                String path_name = prefix + name;
                if ((lib = dlopen(path_name.c_str(), flags))) {
                    return lib;
                }
            }
        }
#endif
        return nullptr;
    }

    inline void* ProcAddress(void* lib, const char* name) {
#if !defined(__WIN32) && !defined(_WIN32) && !defined(__APPLE__)
        return dlsym(lib, name);
#else
        return nullptrptr;
#endif
    }

    Bool AcquireGLESFunctions(MG_External::GLESFunctionsTable& funcs,
                              MG_External::EGL::eglGetProcAddress_PTR procAddress) {
        if (!procAddress) {
            MGLOG_E("eglGetProcAddress is nullptr, cannot load GLES functions");
            return false;
        }

#define INIT_GLES_FUNC(name)                                                                                           \
    do {                                                                                                               \
        funcs.name = (MG_External::GLES::name##_PTR)procAddress(#name);                                                \
        if (!funcs.name) {                                                                                             \
            MGLOG_E("Failed to load GLES function: %s", #name);                                                        \
        }                                                                                                              \
    } while (0);

        {
            INIT_GLES_FUNC(glActiveTexture)
            INIT_GLES_FUNC(glAttachShader)
            INIT_GLES_FUNC(glBindAttribLocation)
            INIT_GLES_FUNC(glBindBuffer)
            INIT_GLES_FUNC(glBindFramebuffer)
            INIT_GLES_FUNC(glBindRenderbuffer)
            INIT_GLES_FUNC(glBindTexture)
            INIT_GLES_FUNC(glBlendColor)
            INIT_GLES_FUNC(glBlendEquation)
            INIT_GLES_FUNC(glBlendEquationSeparate)
            INIT_GLES_FUNC(glBlendFunc)
            INIT_GLES_FUNC(glBlendFuncSeparate)
            INIT_GLES_FUNC(glBufferData)
            INIT_GLES_FUNC(glBufferSubData)
            INIT_GLES_FUNC(glCheckFramebufferStatus)
            INIT_GLES_FUNC(glClear)
            INIT_GLES_FUNC(glClearColor)
            INIT_GLES_FUNC(glClearDepthf)
            INIT_GLES_FUNC(glClearStencil)
            INIT_GLES_FUNC(glColorMask)
            INIT_GLES_FUNC(glCompileShader)
            INIT_GLES_FUNC(glCompressedTexImage2D)
            INIT_GLES_FUNC(glCompressedTexSubImage2D)
            //    INIT_GLES_FUNC(glCopyTexImage1D)
            INIT_GLES_FUNC(glCopyTexImage2D)
            INIT_GLES_FUNC(glCopyTexSubImage2D)
            INIT_GLES_FUNC(glCreateProgram)
            INIT_GLES_FUNC(glCreateShader)
            INIT_GLES_FUNC(glCullFace)
            INIT_GLES_FUNC(glDeleteBuffers)
            INIT_GLES_FUNC(glDeleteFramebuffers)
            INIT_GLES_FUNC(glDeleteProgram)
            INIT_GLES_FUNC(glDeleteRenderbuffers)
            INIT_GLES_FUNC(glDeleteShader)
            INIT_GLES_FUNC(glDeleteTextures)
            INIT_GLES_FUNC(glDepthFunc)
            INIT_GLES_FUNC(glDepthMask)
            INIT_GLES_FUNC(glDepthRangef)
            INIT_GLES_FUNC(glDetachShader)
            INIT_GLES_FUNC(glDisable)
            INIT_GLES_FUNC(glDisableVertexAttribArray)
            INIT_GLES_FUNC(glDrawArrays)
            INIT_GLES_FUNC(glDrawElements)
            INIT_GLES_FUNC(glEnable)
            INIT_GLES_FUNC(glEnableVertexAttribArray)
            INIT_GLES_FUNC(glFinish)
            INIT_GLES_FUNC(glFlush)
            INIT_GLES_FUNC(glFramebufferRenderbuffer)
            INIT_GLES_FUNC(glFramebufferTexture2D)
            INIT_GLES_FUNC(glFrontFace)
            INIT_GLES_FUNC(glGenBuffers)
            INIT_GLES_FUNC(glGenerateMipmap)
            INIT_GLES_FUNC(glGenFramebuffers)
            INIT_GLES_FUNC(glGenRenderbuffers)
            INIT_GLES_FUNC(glGenTextures)
            INIT_GLES_FUNC(glGetActiveAttrib)
            INIT_GLES_FUNC(glGetActiveUniform)
            INIT_GLES_FUNC(glGetAttachedShaders)
            INIT_GLES_FUNC(glGetAttribLocation)
            INIT_GLES_FUNC(glGetBooleanv)
            INIT_GLES_FUNC(glGetBufferParameteriv)
            INIT_GLES_FUNC(glGetError)
            INIT_GLES_FUNC(glGetString)
            INIT_GLES_FUNC(glGetStringi)
            INIT_GLES_FUNC(glGetFloatv)
            INIT_GLES_FUNC(glGetFramebufferAttachmentParameteriv)
            INIT_GLES_FUNC(glGetIntegerv)
            INIT_GLES_FUNC(glGetProgramiv)
            INIT_GLES_FUNC(glGetProgramInfoLog)
            INIT_GLES_FUNC(glGetRenderbufferParameteriv)
            INIT_GLES_FUNC(glGetShaderiv)
            INIT_GLES_FUNC(glGetShaderInfoLog)
            INIT_GLES_FUNC(glGetShaderPrecisionFormat)
            INIT_GLES_FUNC(glGetShaderSource)
            INIT_GLES_FUNC(glGetTexParameterfv)
            INIT_GLES_FUNC(glGetTexParameteriv)
            INIT_GLES_FUNC(glGetUniformfv)
            INIT_GLES_FUNC(glGetUniformiv)
            INIT_GLES_FUNC(glGetUniformLocation)
            INIT_GLES_FUNC(glGetVertexAttribfv)
            INIT_GLES_FUNC(glGetVertexAttribiv)
            INIT_GLES_FUNC(glGetVertexAttribPointerv)
            INIT_GLES_FUNC(glHint)
            INIT_GLES_FUNC(glIsBuffer)
            INIT_GLES_FUNC(glIsEnabled)
            INIT_GLES_FUNC(glIsFramebuffer)
            INIT_GLES_FUNC(glIsProgram)
            INIT_GLES_FUNC(glIsRenderbuffer)
            INIT_GLES_FUNC(glIsShader)
            INIT_GLES_FUNC(glIsTexture)
            INIT_GLES_FUNC(glLineWidth)
            INIT_GLES_FUNC(glLinkProgram)
            INIT_GLES_FUNC(glPixelStorei)
            INIT_GLES_FUNC(glPolygonOffset)
            INIT_GLES_FUNC(glReadPixels)
            INIT_GLES_FUNC(glReleaseShaderCompiler)
            INIT_GLES_FUNC(glRenderbufferStorage)
            INIT_GLES_FUNC(glSampleCoverage)
            INIT_GLES_FUNC(glScissor)
            INIT_GLES_FUNC(glShaderBinary)
            INIT_GLES_FUNC(glShaderSource)
            INIT_GLES_FUNC(glStencilFunc)
            INIT_GLES_FUNC(glStencilFuncSeparate)
            INIT_GLES_FUNC(glStencilMask)
            INIT_GLES_FUNC(glStencilMaskSeparate)
            INIT_GLES_FUNC(glStencilOp)
            INIT_GLES_FUNC(glStencilOpSeparate)
            //    INIT_GLES_FUNC(glTexImage1D)
            INIT_GLES_FUNC(glTexImage2D)
            //    INIT_GLES_FUNC(glTexStorage1D)
            INIT_GLES_FUNC(glTexParameterf)
            INIT_GLES_FUNC(glTexParameterfv)
            INIT_GLES_FUNC(glTexParameteri)
            INIT_GLES_FUNC(glTexParameteriv)
            INIT_GLES_FUNC(glTexSubImage2D)
            INIT_GLES_FUNC(glUniform1f)
            INIT_GLES_FUNC(glUniform1fv)
            INIT_GLES_FUNC(glUniform1i)
            INIT_GLES_FUNC(glUniform1iv)
            INIT_GLES_FUNC(glUniform2f)
            INIT_GLES_FUNC(glUniform2fv)
            INIT_GLES_FUNC(glUniform2i)
            INIT_GLES_FUNC(glUniform2iv)
            INIT_GLES_FUNC(glUniform3f)
            INIT_GLES_FUNC(glUniform3fv)
            INIT_GLES_FUNC(glUniform3i)
            INIT_GLES_FUNC(glUniform3iv)
            INIT_GLES_FUNC(glUniform4f)
            INIT_GLES_FUNC(glUniform4fv)
            INIT_GLES_FUNC(glUniform4i)
            INIT_GLES_FUNC(glUniform4iv)
            INIT_GLES_FUNC(glUniformMatrix2fv)
            INIT_GLES_FUNC(glUniformMatrix3fv)
            INIT_GLES_FUNC(glUniformMatrix4fv)
            INIT_GLES_FUNC(glUseProgram)
            INIT_GLES_FUNC(glValidateProgram)
            INIT_GLES_FUNC(glVertexAttrib1f)
            INIT_GLES_FUNC(glVertexAttrib1fv)
            INIT_GLES_FUNC(glVertexAttrib2f)
            INIT_GLES_FUNC(glVertexAttrib2fv)
            INIT_GLES_FUNC(glVertexAttrib3f)
            INIT_GLES_FUNC(glVertexAttrib3fv)
            INIT_GLES_FUNC(glVertexAttrib4f)
            INIT_GLES_FUNC(glVertexAttrib4fv)
            INIT_GLES_FUNC(glVertexAttribPointer)
            INIT_GLES_FUNC(glViewport)
            INIT_GLES_FUNC(glReadBuffer)
            INIT_GLES_FUNC(glDrawRangeElements)
            INIT_GLES_FUNC(glTexImage3D)
            INIT_GLES_FUNC(glTexSubImage3D)
            INIT_GLES_FUNC(glCopyTexSubImage3D)
            INIT_GLES_FUNC(glCompressedTexImage3D)
            INIT_GLES_FUNC(glCompressedTexSubImage3D)
            INIT_GLES_FUNC(glGenQueries)
            INIT_GLES_FUNC(glDeleteQueries)
            INIT_GLES_FUNC(glIsQuery)
            INIT_GLES_FUNC(glBeginQuery)
            INIT_GLES_FUNC(glEndQuery)
            INIT_GLES_FUNC(glGetQueryiv)
            INIT_GLES_FUNC(glGetQueryObjectuiv)
            INIT_GLES_FUNC(glUnmapBuffer)
            INIT_GLES_FUNC(glGetBufferPointerv)
            INIT_GLES_FUNC(glDrawBuffers)
            INIT_GLES_FUNC(glUniformMatrix2x3fv)
            INIT_GLES_FUNC(glUniformMatrix3x2fv)
            INIT_GLES_FUNC(glUniformMatrix2x4fv)
            INIT_GLES_FUNC(glUniformMatrix4x2fv)
            INIT_GLES_FUNC(glUniformMatrix3x4fv)
            INIT_GLES_FUNC(glUniformMatrix4x3fv)
            INIT_GLES_FUNC(glBlitFramebuffer)
            INIT_GLES_FUNC(glRenderbufferStorageMultisample)
            INIT_GLES_FUNC(glFramebufferTextureLayer)
            INIT_GLES_FUNC(glFlushMappedBufferRange)
            INIT_GLES_FUNC(glBindVertexArray)
            INIT_GLES_FUNC(glDeleteVertexArrays)
            INIT_GLES_FUNC(glGenVertexArrays)
            INIT_GLES_FUNC(glIsVertexArray)
            INIT_GLES_FUNC(glGetIntegeri_v)
            INIT_GLES_FUNC(glBeginTransformFeedback)
            INIT_GLES_FUNC(glEndTransformFeedback)
            INIT_GLES_FUNC(glBindBufferRange)
            INIT_GLES_FUNC(glBindBufferBase)
            INIT_GLES_FUNC(glTransformFeedbackVaryings)
            INIT_GLES_FUNC(glGetTransformFeedbackVarying)
            INIT_GLES_FUNC(glVertexAttribIPointer)
            INIT_GLES_FUNC(glGetVertexAttribIiv)
            INIT_GLES_FUNC(glGetVertexAttribIuiv)
            INIT_GLES_FUNC(glVertexAttribI4i)
            INIT_GLES_FUNC(glVertexAttribI4ui)
            INIT_GLES_FUNC(glVertexAttribI4iv)
            INIT_GLES_FUNC(glVertexAttribI4uiv)
            INIT_GLES_FUNC(glGetUniformuiv)
            INIT_GLES_FUNC(glGetFragDataLocation)
            INIT_GLES_FUNC(glUniform1ui)
            INIT_GLES_FUNC(glUniform2ui)
            INIT_GLES_FUNC(glUniform3ui)
            INIT_GLES_FUNC(glUniform4ui)
            INIT_GLES_FUNC(glUniform1uiv)
            INIT_GLES_FUNC(glUniform2uiv)
            INIT_GLES_FUNC(glUniform3uiv)
            INIT_GLES_FUNC(glUniform4uiv)
            INIT_GLES_FUNC(glClearBufferiv)
            INIT_GLES_FUNC(glClearBufferuiv)
            INIT_GLES_FUNC(glClearBufferfv)
            INIT_GLES_FUNC(glClearBufferfi)
            INIT_GLES_FUNC(glCopyBufferSubData)
            INIT_GLES_FUNC(glGetUniformIndices)
            INIT_GLES_FUNC(glGetActiveUniformsiv)
            INIT_GLES_FUNC(glGetUniformBlockIndex)
            INIT_GLES_FUNC(glGetActiveUniformBlockiv)
            INIT_GLES_FUNC(glGetActiveUniformBlockName)
            INIT_GLES_FUNC(glUniformBlockBinding)
            INIT_GLES_FUNC(glDrawArraysInstanced)
            INIT_GLES_FUNC(glDrawElementsInstanced)
            INIT_GLES_FUNC(glFenceSync)
            INIT_GLES_FUNC(glIsSync)
            INIT_GLES_FUNC(glDeleteSync)
            INIT_GLES_FUNC(glClientWaitSync)
            INIT_GLES_FUNC(glWaitSync)
            INIT_GLES_FUNC(glGetInteger64v)
            INIT_GLES_FUNC(glGetSynciv)
            INIT_GLES_FUNC(glGetInteger64i_v)
            INIT_GLES_FUNC(glGetBufferParameteri64v)
            INIT_GLES_FUNC(glGenSamplers)
            INIT_GLES_FUNC(glDeleteSamplers)
            INIT_GLES_FUNC(glIsSampler)
            INIT_GLES_FUNC(glBindSampler)
            INIT_GLES_FUNC(glSamplerParameteri)
            INIT_GLES_FUNC(glSamplerParameteriv)
            INIT_GLES_FUNC(glSamplerParameterf)
            INIT_GLES_FUNC(glSamplerParameterfv)
            INIT_GLES_FUNC(glGetSamplerParameteriv)
            INIT_GLES_FUNC(glGetSamplerParameterfv)
            INIT_GLES_FUNC(glVertexAttribDivisor)
            INIT_GLES_FUNC(glBindTransformFeedback)
            INIT_GLES_FUNC(glDeleteTransformFeedbacks)
            INIT_GLES_FUNC(glGenTransformFeedbacks)
            INIT_GLES_FUNC(glIsTransformFeedback)
            INIT_GLES_FUNC(glPauseTransformFeedback)
            INIT_GLES_FUNC(glResumeTransformFeedback)
            INIT_GLES_FUNC(glGetProgramBinary)
            INIT_GLES_FUNC(glProgramBinary)
            INIT_GLES_FUNC(glProgramParameteri)
            INIT_GLES_FUNC(glInvalidateFramebuffer)
            INIT_GLES_FUNC(glInvalidateSubFramebuffer)
            INIT_GLES_FUNC(glTexStorage2D)
            INIT_GLES_FUNC(glTexStorage3D)
            INIT_GLES_FUNC(glGetInternalformativ)
            INIT_GLES_FUNC(glDispatchCompute)
            INIT_GLES_FUNC(glDispatchComputeIndirect)
            INIT_GLES_FUNC(glDrawArraysIndirect)
            INIT_GLES_FUNC(glDrawElementsIndirect)
            INIT_GLES_FUNC(glFramebufferParameteri)
            INIT_GLES_FUNC(glGetFramebufferParameteriv)
            INIT_GLES_FUNC(glGetProgramInterfaceiv)
            INIT_GLES_FUNC(glGetProgramResourceIndex)
            INIT_GLES_FUNC(glGetProgramResourceName)
            INIT_GLES_FUNC(glGetProgramResourceiv)
            INIT_GLES_FUNC(glGetProgramResourceLocation)
            INIT_GLES_FUNC(glUseProgramStages)
            INIT_GLES_FUNC(glActiveShaderProgram)
            INIT_GLES_FUNC(glCreateShaderProgramv)
            INIT_GLES_FUNC(glBindProgramPipeline)
            INIT_GLES_FUNC(glDeleteProgramPipelines)
            INIT_GLES_FUNC(glGenProgramPipelines)
            INIT_GLES_FUNC(glIsProgramPipeline)
            INIT_GLES_FUNC(glGetProgramPipelineiv)
            INIT_GLES_FUNC(glProgramUniform1i)
            INIT_GLES_FUNC(glProgramUniform2i)
            INIT_GLES_FUNC(glProgramUniform3i)
            INIT_GLES_FUNC(glProgramUniform4i)
            INIT_GLES_FUNC(glProgramUniform1ui)
            INIT_GLES_FUNC(glProgramUniform2ui)
            INIT_GLES_FUNC(glProgramUniform3ui)
            INIT_GLES_FUNC(glProgramUniform4ui)
            INIT_GLES_FUNC(glProgramUniform1f)
            INIT_GLES_FUNC(glProgramUniform2f)
            INIT_GLES_FUNC(glProgramUniform3f)
            INIT_GLES_FUNC(glProgramUniform4f)
            INIT_GLES_FUNC(glProgramUniform1iv)
            INIT_GLES_FUNC(glProgramUniform2iv)
            INIT_GLES_FUNC(glProgramUniform3iv)
            INIT_GLES_FUNC(glProgramUniform4iv)
            INIT_GLES_FUNC(glProgramUniform1uiv)
            INIT_GLES_FUNC(glProgramUniform2uiv)
            INIT_GLES_FUNC(glProgramUniform3uiv)
            INIT_GLES_FUNC(glProgramUniform4uiv)
            INIT_GLES_FUNC(glProgramUniform1fv)
            INIT_GLES_FUNC(glProgramUniform2fv)
            INIT_GLES_FUNC(glProgramUniform3fv)
            INIT_GLES_FUNC(glProgramUniform4fv)
            INIT_GLES_FUNC(glProgramUniformMatrix2fv)
            INIT_GLES_FUNC(glProgramUniformMatrix3fv)
            INIT_GLES_FUNC(glProgramUniformMatrix4fv)
            INIT_GLES_FUNC(glProgramUniformMatrix2x3fv)
            INIT_GLES_FUNC(glProgramUniformMatrix3x2fv)
            INIT_GLES_FUNC(glProgramUniformMatrix2x4fv)
            INIT_GLES_FUNC(glProgramUniformMatrix4x2fv)
            INIT_GLES_FUNC(glProgramUniformMatrix3x4fv)
            INIT_GLES_FUNC(glProgramUniformMatrix4x3fv)
            INIT_GLES_FUNC(glValidateProgramPipeline)
            INIT_GLES_FUNC(glGetProgramPipelineInfoLog)
            INIT_GLES_FUNC(glBindImageTexture)
            INIT_GLES_FUNC(glGetBooleani_v)
            INIT_GLES_FUNC(glMemoryBarrier)
            INIT_GLES_FUNC(glMemoryBarrierByRegion)
            INIT_GLES_FUNC(glTexStorage2DMultisample)
            INIT_GLES_FUNC(glGetMultisamplefv)
            INIT_GLES_FUNC(glSampleMaski)
            INIT_GLES_FUNC(glGetTexLevelParameteriv)
            INIT_GLES_FUNC(glGetTexLevelParameterfv)
            INIT_GLES_FUNC(glBindVertexBuffer)
            INIT_GLES_FUNC(glVertexAttribFormat)
            INIT_GLES_FUNC(glVertexAttribIFormat)
            INIT_GLES_FUNC(glVertexAttribBinding)
            INIT_GLES_FUNC(glVertexBindingDivisor)
            INIT_GLES_FUNC(glBlendBarrier)
            INIT_GLES_FUNC(glCopyImageSubData)
            INIT_GLES_FUNC(glDebugMessageControl)
            INIT_GLES_FUNC(glDebugMessageInsert)
            INIT_GLES_FUNC(glDebugMessageCallback)
            INIT_GLES_FUNC(glGetDebugMessageLog)
            INIT_GLES_FUNC(glPushDebugGroup)
            INIT_GLES_FUNC(glPopDebugGroup)
            INIT_GLES_FUNC(glObjectLabel)
            INIT_GLES_FUNC(glGetObjectLabel)
            INIT_GLES_FUNC(glObjectPtrLabel)
            INIT_GLES_FUNC(glGetObjectPtrLabel)
            INIT_GLES_FUNC(glGetPointerv)
            INIT_GLES_FUNC(glEnablei)
            INIT_GLES_FUNC(glDisablei)
            INIT_GLES_FUNC(glBlendEquationi)
            INIT_GLES_FUNC(glBlendEquationSeparatei)
            INIT_GLES_FUNC(glBlendFunci)
            INIT_GLES_FUNC(glBlendFuncSeparatei)
            INIT_GLES_FUNC(glColorMaski)
            INIT_GLES_FUNC(glIsEnabledi)
            INIT_GLES_FUNC(glDrawElementsBaseVertex)
            INIT_GLES_FUNC(glDrawRangeElementsBaseVertex)
            INIT_GLES_FUNC(glDrawElementsInstancedBaseVertex)
            INIT_GLES_FUNC(glFramebufferTexture)
            INIT_GLES_FUNC(glPrimitiveBoundingBox)
            INIT_GLES_FUNC(glGetGraphicsResetStatus)
            INIT_GLES_FUNC(glReadnPixels)
            INIT_GLES_FUNC(glGetnUniformfv)
            INIT_GLES_FUNC(glGetnUniformiv)
            INIT_GLES_FUNC(glGetnUniformuiv)
            INIT_GLES_FUNC(glMinSampleShading)
            INIT_GLES_FUNC(glPatchParameteri)
            INIT_GLES_FUNC(glTexParameterIiv)
            INIT_GLES_FUNC(glTexParameterIuiv)
            INIT_GLES_FUNC(glGetTexParameterIiv)
            INIT_GLES_FUNC(glGetTexParameterIuiv)
            INIT_GLES_FUNC(glSamplerParameterIiv)
            INIT_GLES_FUNC(glSamplerParameterIuiv)
            INIT_GLES_FUNC(glGetSamplerParameterIiv)
            INIT_GLES_FUNC(glGetSamplerParameterIuiv)
            INIT_GLES_FUNC(glTexBuffer)
            INIT_GLES_FUNC(glTexBufferRange)
            INIT_GLES_FUNC(glTexStorage3DMultisample)
            INIT_GLES_FUNC(glMapBufferRange)
            INIT_GLES_FUNC(glBufferStorageEXT)
            INIT_GLES_FUNC(glGetQueryObjectivEXT)
            INIT_GLES_FUNC(glGetQueryObjecti64vEXT)
            INIT_GLES_FUNC(glBindFragDataLocationEXT)
            INIT_GLES_FUNC(glMapBufferOES)
            INIT_GLES_FUNC(glMultiDrawArraysIndirectEXT)
            INIT_GLES_FUNC(glMultiDrawElementsIndirectEXT)
            INIT_GLES_FUNC(glMultiDrawElementsBaseVertexEXT)
        }
        return true;
    }

    Bool AcquireEGLFunctions(MG_External::EGLFunctionsTable& funcs) {
        static const Vector<String> EGLLibNames = {"libEGL.so"};
        void* eglLib = OpenLib(EGLLibNames);
        if (!eglLib) {
            MGLOG_E("Failed to open libEGL.so");
            return false;
        }
#define INIT_EGL_FUNC(name)                                                                                            \
    do {                                                                                                               \
        funcs.name = (MG_External::EGL::name##_PTR)ProcAddress(eglLib, #name);                                         \
        if (!funcs.name) {                                                                                             \
            MGLOG_E("Failed to load EGL function: %s", #name);                                                         \
        }                                                                                                              \
    } while (0);

        {
            INIT_EGL_FUNC(eglBindAPI)
            INIT_EGL_FUNC(eglBindTexImage)
            INIT_EGL_FUNC(eglChooseConfig)
            INIT_EGL_FUNC(eglCopyBuffers)
            INIT_EGL_FUNC(eglCreateContext)
            INIT_EGL_FUNC(eglCreatePbufferFromClientBuffer)
            INIT_EGL_FUNC(eglCreatePbufferSurface)
            INIT_EGL_FUNC(eglCreatePixmapSurface)
            INIT_EGL_FUNC(eglCreatePlatformPixmapSurface)
            INIT_EGL_FUNC(eglCreatePlatformWindowSurface)
            INIT_EGL_FUNC(eglCreateWindowSurface)
            INIT_EGL_FUNC(eglDestroyContext)
            INIT_EGL_FUNC(eglDestroySurface)
            INIT_EGL_FUNC(eglGetConfigAttrib)
            INIT_EGL_FUNC(eglGetConfigs)
            INIT_EGL_FUNC(eglGetCurrentContext)
            INIT_EGL_FUNC(eglGetCurrentDisplay)
            INIT_EGL_FUNC(eglGetCurrentSurface)
            INIT_EGL_FUNC(eglGetDisplay)
            INIT_EGL_FUNC(eglGetPlatformDisplay)
            INIT_EGL_FUNC(eglGetError)
            INIT_EGL_FUNC(eglGetProcAddress)
            INIT_EGL_FUNC(eglInitialize)
            INIT_EGL_FUNC(eglMakeCurrent)
            INIT_EGL_FUNC(eglQueryAPI)
            INIT_EGL_FUNC(eglQueryContext)
            INIT_EGL_FUNC(eglQueryString)
            INIT_EGL_FUNC(eglQuerySurface)
            INIT_EGL_FUNC(eglReleaseTexImage)
            INIT_EGL_FUNC(eglReleaseThread)
            INIT_EGL_FUNC(eglSurfaceAttrib)
            INIT_EGL_FUNC(eglSwapBuffers)
            INIT_EGL_FUNC(eglSwapBuffersWithDamageEXT)
            INIT_EGL_FUNC(eglSwapInterval)
            INIT_EGL_FUNC(eglTerminate)
            INIT_EGL_FUNC(eglUnlockSurfaceKHR)
            INIT_EGL_FUNC(eglWaitClient)
            INIT_EGL_FUNC(eglWaitGL)
            INIT_EGL_FUNC(eglWaitNative)
            INIT_EGL_FUNC(eglCreateSync)
            INIT_EGL_FUNC(eglDestroySync)
            INIT_EGL_FUNC(eglClientWaitSync)
            INIT_EGL_FUNC(eglGetSyncAttrib)
            INIT_EGL_FUNC(eglCreateImage)
            INIT_EGL_FUNC(eglDestroyImage)
            INIT_EGL_FUNC(eglGetPlatformDisplay)
            INIT_EGL_FUNC(eglWaitSync)
        }
        return true;
    }

    static EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    static EGLSurface eglSurface = EGL_NO_SURFACE;
    static EGLContext eglContext = EGL_NO_CONTEXT;
    inline void DestroyTempEGLCtx(const MG_External::EGLFunctionsTable& eglFuncs) {
        if (eglSurface != EGL_NO_SURFACE) {
            eglFuncs.eglDestroySurface(eglDisplay, eglSurface);
            eglSurface = EGL_NO_SURFACE;
        }
        if (eglContext != EGL_NO_CONTEXT) {
            eglFuncs.eglDestroyContext(eglDisplay, eglContext);
            eglContext = EGL_NO_CONTEXT;
        }
        if (eglDisplay != EGL_NO_DISPLAY) {
            eglFuncs.eglTerminate(eglDisplay);
            eglDisplay = EGL_NO_DISPLAY;
        }
    }
    inline Bool InitTmpEGLContext(const MG_External::EGLFunctionsTable& eglFuncs) {
        EGLint configAttribs[] = {EGL_RED_SIZE,
                                  8,
                                  EGL_GREEN_SIZE,
                                  8,
                                  EGL_BLUE_SIZE,
                                  8,
                                  EGL_ALPHA_SIZE,
                                  8,
                                  EGL_SURFACE_TYPE,
                                  EGL_PBUFFER_BIT,
                                  EGL_RENDERABLE_TYPE,
                                  EGL_OPENGL_ES2_BIT,
                                  EGL_NONE};

        EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

        EGLint pbAttribs[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};

        EGLConfig pbufConfig;
        EGLint configsFound = 0;

        eglDisplay = eglFuncs.eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay == EGL_NO_DISPLAY) {
            DestroyTempEGLCtx(eglFuncs);
            return false;
        }

        if (eglFuncs.eglInitialize(eglDisplay, nullptr, nullptr) != EGL_TRUE) {
            DestroyTempEGLCtx(eglFuncs);
            return false;
        }

        if (eglFuncs.eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            DestroyTempEGLCtx(eglFuncs);
            return false;
        }

        if (eglFuncs.eglChooseConfig(eglDisplay, configAttribs, &pbufConfig, 1, &configsFound) != EGL_TRUE) {
            DestroyTempEGLCtx(eglFuncs);
            return false;
        }

        if (configsFound == 0) {
            configAttribs[6] = 0;
            if (eglFuncs.eglChooseConfig(eglDisplay, configAttribs, &pbufConfig, 1, &configsFound) != EGL_TRUE) {
                DestroyTempEGLCtx(eglFuncs);
                return false;
            }
            if (!configsFound) {
                DestroyTempEGLCtx(eglFuncs);
                return false;
            }
        }

        eglContext = eglFuncs.eglCreateContext(eglDisplay, pbufConfig, EGL_NO_CONTEXT, ctxAttribs);
        if (eglContext == EGL_NO_CONTEXT) {
            DestroyTempEGLCtx(eglFuncs);
            return false;
        }

        eglSurface = eglFuncs.eglCreatePbufferSurface(eglDisplay, pbufConfig, pbAttribs);
        if (eglSurface == EGL_NO_SURFACE) {
            DestroyTempEGLCtx(eglFuncs);
            return false;
        }

        if (eglFuncs.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext) != EGL_TRUE) {
            DestroyTempEGLCtx(eglFuncs);
            return false;
        }
        return true;
    }

    Bool FillInGLESCapabilities(MG_External::GLESCapabilities& caps, const MG_External::GLESFunctionsTable& glesFuncs,
                                const MG_External::EGLFunctionsTable& eglFuncs) {
        if (!glesFuncs.glGetString || !glesFuncs.glGetIntegerv) {
            MGLOG_E("Required GLES functions are not loaded, cannot query capabilities");
            return false;
        }
        if (!eglFuncs.eglGetDisplay || !eglFuncs.eglInitialize || !eglFuncs.eglChooseConfig ||
            !eglFuncs.eglCreateContext || !eglFuncs.eglCreatePbufferSurface || !eglFuncs.eglMakeCurrent) {
            MGLOG_E("Required EGL functions are not loaded, cannot create temporary EGL context");
            return false;
        }

        Bool eglCtxCreated = InitTmpEGLContext(eglFuncs);
        if (!eglCtxCreated) {
            MGLOG_E("Failed to create temporary EGL context, cannot query GLES capabilities");
            return false;
        }

        auto* vendorName = glesFuncs.glGetString(GL_VENDOR);
        MGLOG_I("GL_VENDOR: %s", vendorName);
        auto* gpuName = glesFuncs.glGetString(GL_RENDERER);
        MGLOG_I("GL_RENDERER: %s", gpuName);
        glesFuncs.glGetIntegerv(GL_MAJOR_VERSION, &caps.GLESVersion.Major);
        glesFuncs.glGetIntegerv(GL_MINOR_VERSION, &caps.GLESVersion.Minor);

        caps.GLESVersionString = String((char*)glesFuncs.glGetString(GL_VERSION));
        caps.GLESRendererString = String((char*)gpuName);
        caps.GLESVendorString = String((char*)vendorName);
        caps.GLESShadingLanguageVersionString = String((char*)glesFuncs.glGetString(GL_SHADING_LANGUAGE_VERSION));

        GLint extCount = 0;
        glesFuncs.glGetIntegerv(GL_NUM_EXTENSIONS, &extCount);
        MGLOG_I("Detected %d OpenGL ES extensions:", extCount);
        for (GLint i = 0; i < extCount; ++i) {
            const char* extension = (const char*)glesFuncs.glGetStringi(GL_EXTENSIONS, i);
            if (extension) {
                MGLOG_I("    %s", extension);
                if (std::strcmp(extension, "GL_EXT_buffer_storage") == 0) {
                    caps.SupportsPersistentMapping = true;
                }
                if (std::strcmp(extension, "GL_EXT_texture_norm16") == 0) {
                    caps.SupportsNorm16Texture = true;
                }
            }
        }

        MGLOG_I("OpenGL ES capabilities:");
        glesFuncs.glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &caps.UniformBufferOffsetAlignment);
        MGLOG_I("    GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: %d", caps.UniformBufferOffsetAlignment);

        DestroyTempEGLCtx(eglFuncs);
        return true;
    }
} // namespace MobileGL::MG_Util::BackendLoader
