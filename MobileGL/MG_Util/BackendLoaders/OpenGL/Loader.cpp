// MobileGL - MobileGL/MG_Util/BackendLoaders/OpenGL/Loader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Loader.h"
#include "MG_Util/Types.h"
#include <Config.h>
#include <cmath>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace MobileGL::MG_Util::BackendLoader {
    static Bool UseAngle() {
        return MG_Config::Features.UseAngle;
    }

#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS) && defined(__ANDROID__)
    static Bool IsTraceAngleLibrary(const String& name) {
        return name == "libGLESv2_angle.so" || name == "libEGL_angle.so";
    }

    static Bool IsAllowedTraceAngleVariant(const String& variant) {
        return variant == "ec889e6ea831" || variant == "90a62123d794";
    }

    static Bool ResolveTraceAngleLibraryPath(const String& name, String& path) {
        const String& variant = MG_Config::Features.TraceAngleVariant;
        if (!IsAllowedTraceAngleVariant(variant)) {
            MGLOG_F("Rejected trace ANGLE variant '%s'", variant.c_str());
            return false;
        }

        Dl_info mobileGlInfo{};
        if (dladdr(reinterpret_cast<const void*>(&ResolveTraceAngleLibraryPath), &mobileGlInfo) == 0 ||
            mobileGlInfo.dli_fname == nullptr) {
            MGLOG_F("Failed to resolve signed trace native library directory");
            return false;
        }

        String nativeLibraryPath = mobileGlInfo.dli_fname;
        const SizeT separator = nativeLibraryPath.find_last_of('/');
        if (separator == String::npos) {
            MGLOG_F("Invalid MobileGL library path: %s", nativeLibraryPath.c_str());
            return false;
        }

        const SizeT extension = name.rfind(".so");
        if (extension == String::npos) {
            MGLOG_F("Invalid trace ANGLE library name: %s", name.c_str());
            return false;
        }
        path = nativeLibraryPath.substr(0, separator + 1) + name.substr(0, extension) + "_" + variant + ".so";
        return true;
    }

#endif

    static void* OpenLib(const Vector<String>& names) {
#if defined(_WIN32)
        for (const auto& name : names) {
            if (HMODULE lib = LoadLibraryA(name.c_str())) {
                MGLOG_I("Loaded GL backend library: %s", name.c_str());
                return reinterpret_cast<void*>(lib);
            }
        }
#elif !defined(__APPLE__) || defined(MOBILEGL_IOS)
        static const String LibPathPrefixes[] = {
#if defined(MOBILEGL_IOS)
            "@rpath/", "@executable_path/Frameworks/", "@loader_path/Frameworks/",
#else
            "/opt/vc/lib/", "/usr/local/lib/", "/usr/lib/", "/usr/lib/x86_64-linux-gnu/",
            "/usr/lib64/", "/lib64/",
#endif
            "" // Keep this last so the dynamic loader can use LD_LIBRARY_PATH.
        };

        void* lib = nullptr;

        Int flags = RTLD_LOCAL | RTLD_NOW;
        for (const auto& prefix : LibPathPrefixes) {
            for (const auto& name : names) {
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS) && defined(__ANDROID__)
                if (UseAngle() && IsTraceAngleLibrary(name)) {
                    String signedPath;
                    if (!ResolveTraceAngleLibraryPath(name, signedPath)) {
                        return nullptr;
                    }
                    lib = dlopen(signedPath.c_str(), flags);
                    if (lib == nullptr) {
                        MGLOG_F("Failed to open signed trace ANGLE library %s: %s",
                                signedPath.c_str(), dlerror());
                        return nullptr;
                    }
                    MGLOG_I("Loaded signed trace ANGLE library: %s", signedPath.c_str());
                    return lib;
                }
#endif
                String path_name = prefix + name;
                if ((lib = dlopen(path_name.c_str(), flags))) {
                    MGLOG_I("Loaded GL backend library: %s", path_name.c_str());
                    return lib;
                }
            }
        }
#endif
        return nullptr;
    }

    inline void* ProcAddress(void* lib, const char* name) {
#if defined(_WIN32)
        return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#elif !defined(__APPLE__) || defined(MOBILEGL_IOS)
        return dlsym(lib, name);
#else
        return nullptr;
#endif
    }

    void AcquireGLESFunctions(MG_External::GLESFunctionsTable& funcs,
                              MG_External::EGL::eglGetProcAddress_PTR procAddress) {
        if (!procAddress) {
            MGLOG_E("eglGetProcAddress is nullptr, cannot load GLES functions");
            return;
        }

#define INIT_GLES_FUNC(name)                                                                                           \
    do {                                                                                                               \
        funcs.name = (MG_External::GLES::name##_PTR)procAddress(#name);                                                \
        if (!funcs.name) {                                                                                             \
            MGLOG_E("Failed to load GLES function: %s", #name);                                                        \
        }                                                                                                              \
    } while (0);

// Optional (extension-provided) entry points: a null pointer is expected on drivers that lack the
// extension, so absence is not an error. The call site null-checks before use.
#define INIT_GLES_FUNC_OPTIONAL(name)                                                                                  \
    do {                                                                                                               \
        funcs.name = (MG_External::GLES::name##_PTR)procAddress(#name);                                                \
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
            INIT_GLES_FUNC(glLogicOp)
            INIT_GLES_FUNC(glPointSize)
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
            INIT_GLES_FUNC(glShaderStorageBlockBinding)
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
            INIT_GLES_FUNC(glQueryCounterEXT)
            INIT_GLES_FUNC(glGetQueryObjectui64vEXT)
            INIT_GLES_FUNC(glBindFragDataLocationEXT)
            INIT_GLES_FUNC(glMapBufferOES)
            INIT_GLES_FUNC_OPTIONAL(glPolygonModeNV)
            INIT_GLES_FUNC_OPTIONAL(glPolygonModeANGLE)
            INIT_GLES_FUNC_OPTIONAL(glColorMaskiEXT)
            INIT_GLES_FUNC_OPTIONAL(glColorMaskiOES)
            // Extension-only multi-draw entry points. eglGetProcAddress may legally return a
            // non-NULL stub for these on drivers that do not implement them (NVIDIA's ES driver
            // returns one for glMultiDrawElementsBaseVertexEXT that silently drops every draw),
            // so pointer presence proves nothing: callers must gate on the extension-derived
            // SupportsMultiDrawIndirect / SupportsMultiDrawElementsBaseVertex capability flags,
            // never on these pointers alone.
            INIT_GLES_FUNC_OPTIONAL(glMultiDrawArraysIndirectEXT)
            INIT_GLES_FUNC_OPTIONAL(glMultiDrawElementsIndirectEXT)
            INIT_GLES_FUNC_OPTIONAL(glMultiDrawElementsBaseVertexEXT)
        }
    }

    void AcquireEGLFunctions(MG_External::EGLFunctionsTable& funcs) {
        void* eglLib = nullptr;
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS) && defined(__ANDROID__)
        void* angleGlesLib = nullptr;
#endif
#if defined(_WIN32)
        // ANGLE is the GLES provider on Windows regardless of UseAngle(). Preload
        // libGLESv2.dll so libEGL.dll resolves its dependency from the same directory.
        if (!OpenLib({"libGLESv2.dll"})) {
            MGLOG_E("Failed to open ANGLE libGLESv2.dll");
            return;
        }
        eglLib = OpenLib({"libEGL.dll"});
#else
        if (UseAngle()) {
            void* glesLib = OpenLib({"libGLESv2_angle.so"});
            if (!glesLib) {
                MGLOG_E("Failed to open ANGLE libGLESv2_angle.so");
                return;
            }
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS) && defined(__ANDROID__)
            angleGlesLib = glesLib;
#endif
            eglLib = OpenLib({"libEGL_angle.so"});
            if (!eglLib) {
                MGLOG_E("Failed to open ANGLE libEGL_angle.so");
                return;
            }
        } else {
#if defined(MOBILEGL_IOS)
            eglLib = OpenLib({"libtinygl4angle.dylib"});
#else
            eglLib = OpenLib({"libEGL.so"});
#endif
        }
#endif // !_WIN32

        if (!eglLib) {
            MGLOG_E("Failed to open EGL library");
            return;
        }

        auto resolveEGLProc = [&](const char* name) -> void* {
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS) && defined(__ANDROID__)
            if (UseAngle()) {
                // Avoid the wrapper's canonical libGLESv2_angle.so lookup:
                // resolve its forwarding target from the verified variant.
                String target = "EGL_";
                target += name + 3;
                return ProcAddress(angleGlesLib, target.c_str());
            }
#endif
            return ProcAddress(eglLib, name);
        };

#define INIT_EGL_FUNC(name)                                                                                            \
    do {                                                                                                               \
        funcs.name = (MG_External::EGL::name##_PTR)resolveEGLProc(#name);                                              \
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
    }

    // Detects whether indirect draws leak the command's baseInstance word ("reserved, must
    // be zero" in unextended ES) into gl_InstanceID. Conforming ES drivers keep
    // gl_InstanceID zero-based, but ANGLE's Vulkan backend forwards the command verbatim to
    // vkCmdDraw*Indirect and compiles gl_InstanceID to SPIR-V InstanceIndex, which includes
    // firstInstance. The DirectGLES native indirect-draw path uses this answer to keep
    // gl_InstanceID zero-based in rewritten shaders (PromoteDrawParameterGlobalsToUniforms).
    Bool ProbeIndirectInstanceIdIncludesBaseInstance(const MG_External::GLESCapabilities& caps,
                                                     const MG_External::GLESFunctionsTable& f) {
        const Bool esVersionOk =
            caps.GLESVersion.Major > 3 || (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 1);
        if (!esVersionOk || !f.glDrawArraysIndirect || !f.glBindBufferBase || !f.glMapBufferRange ||
            !f.glUnmapBuffer || !f.glMemoryBarrier || !f.glCreateShader || !f.glCreateProgram) {
            return false;
        }
        GLint maxVertexSsboBlocks = 0;
        f.glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &maxVertexSsboBlocks);
        if (maxVertexSsboBlocks < 1) {
            // The native indirect machinery cannot read the command buffer from the vertex
            // stage on this driver anyway; assume conforming zero-based gl_InstanceID.
            MGLOG_I("baseInstance probe skipped: GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = %d", maxVertexSsboBlocks);
            return false;
        }
        while (f.glGetError() != GL_NO_ERROR) {
        }

        const char* vsSource = "#version 310 es\n"
                               "layout(std430, binding = 0) buffer MgProbeResult { highp int mg_probeValue; };\n"
                               "void main() {\n"
                               "    mg_probeValue = gl_InstanceID;\n"
                               "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n"
                               "    gl_PointSize = 1.0;\n"
                               "}\n";
        const char* fsSource = "#version 310 es\n"
                               "void main() {}\n";
        const auto compileShader = [&f](GLenum type, const char* src) -> GLuint {
            const GLuint shader = f.glCreateShader(type);
            if (shader == 0) {
                return 0;
            }
            f.glShaderSource(shader, 1, &src, nullptr);
            f.glCompileShader(shader);
            GLint status = GL_FALSE;
            f.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status != GL_TRUE) {
                f.glDeleteShader(shader);
                return 0;
            }
            return shader;
        };
        const GLuint vs = compileShader(GL_VERTEX_SHADER, vsSource);
        const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSource);
        GLuint program = 0;
        if (vs != 0 && fs != 0) {
            program = f.glCreateProgram();
            if (program != 0) {
                f.glAttachShader(program, vs);
                f.glAttachShader(program, fs);
                f.glLinkProgram(program);
                GLint status = GL_FALSE;
                f.glGetProgramiv(program, GL_LINK_STATUS, &status);
                if (status != GL_TRUE) {
                    f.glDeleteProgram(program);
                    program = 0;
                }
            }
        }
        if (vs != 0) f.glDeleteShader(vs);
        if (fs != 0) f.glDeleteShader(fs);
        if (program == 0) {
            MGLOG_I("baseInstance probe skipped: probe program failed to build (vs=%u fs=%u)", vs, fs);
            while (f.glGetError() != GL_NO_ERROR) {
            }
            return false;
        }

        constexpr GLuint kProbeBaseInstance = 7;
        struct {
            GLuint count;
            GLuint instanceCount;
            GLuint first;
            GLuint baseInstance;
        } command = {1, 1, 0, kProbeBaseInstance};
        const GLint sentinel = -1;
        // ES makes indirect draws INVALID_OPERATION on the default vertex array object.
        GLuint vao = 0;
        f.glGenVertexArrays(1, &vao);
        f.glBindVertexArray(vao);
        GLuint buffers[2] = {0, 0}; // [0] = result SSBO, [1] = indirect command buffer
        f.glGenBuffers(2, buffers);
        f.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
        f.glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(sentinel), &sentinel, GL_STATIC_DRAW);
        f.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[0]);
        f.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[1]);
        f.glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(command), &command, GL_STATIC_DRAW);

        // The default framebuffer may be incomplete (e.g. surfaceless contexts) and draws
        // are validated against completeness even under GL_RASTERIZER_DISCARD, so give the
        // probe its own 1x1 target.
        GLuint framebuffer = 0;
        GLuint renderbuffer = 0;
        f.glGenFramebuffers(1, &framebuffer);
        f.glGenRenderbuffers(1, &renderbuffer);
        f.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        f.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
        f.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        f.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);

        f.glUseProgram(program);
        f.glEnable(GL_RASTERIZER_DISCARD);
        f.glDrawArraysIndirect(GL_POINTS, nullptr);
        f.glDisable(GL_RASTERIZER_DISCARD);
        f.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

        Bool includesBase = false;
        const GLenum drawError = f.glGetError();
        if (drawError == GL_NO_ERROR) {
            f.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
            const void* mapped = f.glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLint), GL_MAP_READ_BIT);
            if (mapped != nullptr) {
                GLint written = -1;
                std::memcpy(&written, mapped, sizeof(written));
                f.glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                includesBase = written == static_cast<GLint>(kProbeBaseInstance);
                MGLOG_I("baseInstance probe: shader observed gl_InstanceID = %d (baseInstance was %u)", written,
                        kProbeBaseInstance);
            } else {
                MGLOG_I("baseInstance probe inconclusive: result map failed");
            }
        } else {
            MGLOG_I("baseInstance probe inconclusive: draw raised GL error 0x%x", drawError);
        }

        f.glUseProgram(0);
        f.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        f.glBindRenderbuffer(GL_RENDERBUFFER, 0);
        f.glDeleteFramebuffers(1, &framebuffer);
        f.glDeleteRenderbuffers(1, &renderbuffer);
        f.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        f.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        f.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        f.glBindVertexArray(0);
        f.glDeleteVertexArrays(1, &vao);
        f.glDeleteBuffers(2, buffers);
        f.glDeleteProgram(program);
        while (f.glGetError() != GL_NO_ERROR) {
        }
        return includesBase;
    }

    Bool FillInGLESCapabilities(MG_External::GLESCapabilities& caps, const MG_External::GLESFunctionsTable& glesFuncs) {
        if (!glesFuncs.glGetString || !glesFuncs.glGetIntegerv) {
            MGLOG_E("Required GLES functions are not loaded, cannot query capabilities");
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
        // Combined below: glMultiDrawElementsBaseVertexEXT exists only where EXT/OES
        // draw_elements_base_vertex interacts with GL_EXT_multi_draw_arrays.
        Bool hasMultiDrawIndirectExtension = false;
        Bool hasDrawElementsBaseVertexExtension = false;
        Bool hasMultiDrawArraysExtension = false;
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
                if (std::strcmp(extension, "GL_EXT_render_snorm") == 0) {
                    caps.SupportsRenderSnorm = true;
                }
                if (std::strcmp(extension, "GL_EXT_sRGB_write_control") == 0) {
                    caps.SupportsSrgbWriteControl = true;
                }
                if (std::strcmp(extension, "GL_EXT_texture_filter_anisotropic") == 0) {
                    caps.SupportsTextureFilterAnisotropy = true;
                }
                if (std::strcmp(extension, "GL_EXT_texture_border_clamp") == 0 ||
                    std::strcmp(extension, "GL_OES_texture_border_clamp") == 0) {
                    caps.SupportsTextureBorderClamp = true;
                }
                if (std::strcmp(extension, "GL_EXT_texture_cube_map_array") == 0 ||
                    std::strcmp(extension, "GL_OES_texture_cube_map_array") == 0) {
                    caps.SupportsTextureCubeMapArray = true;
                }
                if (std::strcmp(extension, "GL_EXT_base_instance") == 0) {
                    caps.SupportsBaseInstance = true;
                }
                if (std::strcmp(extension, "GL_EXT_disjoint_timer_query") == 0) {
                    caps.SupportsDisjointTimerQuery = true;
                }
                if (std::strcmp(extension, "GL_KHR_parallel_shader_compile") == 0) {
                    caps.SupportsParallelShaderCompile = true;
                }
                if (std::strcmp(extension, "GL_EXT_blend_func_extended") == 0) {
                    caps.SupportsDualSourceBlend = true;
                }
                if (std::strcmp(extension, "GL_NV_shader_noperspective_interpolation") == 0) {
                    caps.SupportsNoperspectiveInterpolation = true;
                }
                if (std::strcmp(extension, "GL_OES_shader_multisample_interpolation") == 0) {
                    caps.SupportsShaderMultisampleInterpolation = true;
                }
                if (std::strcmp(extension, "GL_EXT_multi_draw_indirect") == 0) {
                    hasMultiDrawIndirectExtension = true;
                }
                if (std::strcmp(extension, "GL_EXT_draw_elements_base_vertex") == 0 ||
                    std::strcmp(extension, "GL_OES_draw_elements_base_vertex") == 0) {
                    hasDrawElementsBaseVertexExtension = true;
                }
                if (std::strcmp(extension, "GL_EXT_multi_draw_arrays") == 0) {
                    hasMultiDrawArraysExtension = true;
                }
            }
        }
        // The pointer check on top of the extension check makes each flag sufficient on its own
        // at a call site; the extension check on top of the pointer keeps a stub returned by
        // eglGetProcAddress (see AcquireGLESFunctions) from ever counting as support.
        caps.SupportsMultiDrawIndirect = hasMultiDrawIndirectExtension &&
                                         glesFuncs.glMultiDrawArraysIndirectEXT != nullptr &&
                                         glesFuncs.glMultiDrawElementsIndirectEXT != nullptr;
        caps.SupportsMultiDrawElementsBaseVertex = hasDrawElementsBaseVertexExtension &&
                                                   hasMultiDrawArraysExtension &&
                                                   glesFuncs.glMultiDrawElementsBaseVertexEXT != nullptr;
        // Core from ES 3.2 on, so an extension string is not required there; below 3.2 the
        // extension is, and the pointer still has to have resolved either way.
        const Bool esAtLeast32 = caps.GLESVersion.Major > 3 ||
                                 (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 2);
        const Bool esAtLeast31 = caps.GLESVersion.Major > 3 ||
                                 (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 1);
        caps.SupportsDrawElementsBaseVertex = (esAtLeast32 || hasDrawElementsBaseVertexExtension) &&
                                              glesFuncs.glDrawElementsBaseVertex != nullptr;
        caps.SupportsComputeShader = esAtLeast31 && glesFuncs.glDispatchCompute != nullptr &&
                                     glesFuncs.glMemoryBarrier != nullptr &&
                                     glesFuncs.glCreateShader != nullptr &&
                                     glesFuncs.glCreateProgram != nullptr;
        caps.SupportsShaderMultisampleInterpolation =
            caps.SupportsShaderMultisampleInterpolation || caps.GLESVersion.Major > 3 ||
            (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 2);

        // Detect optional raster/color-mask entry points by whether they loaded. glColorMaski is GLES
        // 3.2 core (no extension string), so pointer presence is the reliable signal for all of these.
        caps.SupportsPolygonMode =
            glesFuncs.glPolygonModeNV != nullptr || glesFuncs.glPolygonModeANGLE != nullptr;
        caps.SupportsIndexedColorMask = glesFuncs.glColorMaski != nullptr ||
                                        glesFuncs.glColorMaskiEXT != nullptr ||
                                        glesFuncs.glColorMaskiOES != nullptr;
        MGLOG_I("    glPolygonMode (NV/ANGLE): %s", caps.SupportsPolygonMode ? "yes" : "no");
        MGLOG_I("    indexed glColorMaski: %s", caps.SupportsIndexedColorMask ? "yes" : "no");
        MGLOG_I("    dual-source blend (EXT_blend_func_extended): %s",
                caps.SupportsDualSourceBlend ? "yes" : "no");
        MGLOG_I("    multi-draw indirect (EXT_multi_draw_indirect): %s",
                caps.SupportsMultiDrawIndirect ? "yes" : "no");
        MGLOG_I("    multi-draw base vertex (EXT/OES_draw_elements_base_vertex + EXT_multi_draw_arrays): %s",
                caps.SupportsMultiDrawElementsBaseVertex ? "yes" : "no");
        MGLOG_I("    draw elements base vertex (ES 3.2 core or EXT/OES_draw_elements_base_vertex): %s",
                caps.SupportsDrawElementsBaseVertex ? "yes" : "no");
        MGLOG_I("    compute shaders (ES 3.1 core): %s", caps.SupportsComputeShader ? "yes" : "no");

        MGLOG_I("OpenGL ES capabilities:");
        glesFuncs.glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &caps.UniformBufferOffsetAlignment);
        MGLOG_I("    GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: %d", caps.UniformBufferOffsetAlignment);
        GLfloat aliasedLineWidthRange[2] = {1.0f, 1.0f};
        GLfloat smoothLineWidthRange[2] = {1.0f, 1.0f};
        GLfloat smoothLineWidthGranularity = 1.0f;
        GLfloat aliasedPointSizeRange[2] = {1.0f, 1.0f};
        GLfloat viewportBoundsRange[2] = {0.0f, 0.0f};
        GLint maxViewportDims[2] = {16384, 16384};
        GLint viewportSubpixelBits = 0;
        GLint max3DTextureSize = 16384;
        GLint maxArrayTextureLayers = 2048;
        GLint maxCubeMapTextureSize = 16384;
        GLint maxFramebufferWidth = 16384;
        GLint maxFramebufferHeight = 16384;
        GLint maxFramebufferLayers = 2048;
        GLint maxRenderbufferSize = 16384;
        GLint maxTextureSize = 16384;
        GLint maxColorTextureSamples = 1;
        GLint maxDepthTextureSamples = 1;
        GLint maxFramebufferSamples = 1;
        GLint maxIntegerSamples = 1;
        GLint maxSamples = 1;
        GLint maxSampleMaskWords = 1;
        GLint maxTextureImageUnits = 32;
        GLint maxVertexTextureImageUnits = 32;
        GLint maxComputeTextureImageUnits = 32;
        GLint maxCombinedTextureImageUnits = 192;
        GLint maxVertexAttribs = 16;
        GLint maxComputeShaderStorageBlocks = 8;
        GLint maxCombinedShaderStorageBlocks = 32;
        GLint maxComputeUniformBlocks = 12;
        GLint maxComputeWorkGroupInvocations = 128;
        GLint maxShaderStorageBufferBindings = 8;
        GLint maxTextureBufferSize = 65536;
        GLint maxUniformBufferBindings = 24;
        GLint maxUniformBlockSize = 16384;
        GLint maxImageUnits = 8;
        GLint maxCombinedImageUniforms = 8;
        GLint maxVertexImageUniforms = 0;
        GLint maxGeometryImageUniforms = 0;
        GLint maxFragmentImageUniforms = 8;
        GLint maxComputeImageUniforms = 8;
        GLint maxDrawBuffers = 8;
        GLint maxColorAttachments = 8;
        GLint maxClipDistances = 8;
        GLint maxViewports = 16;
        GLfloat minFragmentInterpolationOffset = -0.5f;
        GLfloat maxFragmentInterpolationOffset = 0.4375f;
        GLint fragmentInterpolationOffsetBits = 4;
        // Core minimums of both APIs (GL 4.6 table 23.53, ES 3.1 table 20.40); the probe
        // below only ever widens them.
        GLint minProgramTextureGatherOffset = -8;
        GLint maxProgramTextureGatherOffset = 7;
        GLint maxPatchVertices = 32;
        GLint maxTessGenLevel = 64;
        glesFuncs.glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, aliasedLineWidthRange);
        glesFuncs.glGetFloatv(GL_SMOOTH_LINE_WIDTH_RANGE, smoothLineWidthRange);
        glesFuncs.glGetFloatv(GL_SMOOTH_LINE_WIDTH_GRANULARITY, &smoothLineWidthGranularity);
        glesFuncs.glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, aliasedPointSizeRange);
        glesFuncs.glGetFloatv(GL_VIEWPORT_BOUNDS_RANGE, viewportBoundsRange);
        glesFuncs.glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max3DTextureSize);
        glesFuncs.glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxArrayTextureLayers);
        glesFuncs.glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &maxCubeMapTextureSize);
        glesFuncs.glGetIntegerv(GL_MAX_FRAMEBUFFER_WIDTH, &maxFramebufferWidth);
        glesFuncs.glGetIntegerv(GL_MAX_FRAMEBUFFER_HEIGHT, &maxFramebufferHeight);
        glesFuncs.glGetIntegerv(GL_MAX_FRAMEBUFFER_LAYERS, &maxFramebufferLayers);
        glesFuncs.glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbufferSize);
        glesFuncs.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
        glesFuncs.glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &maxColorTextureSamples);
        glesFuncs.glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &maxDepthTextureSamples);
        glesFuncs.glGetIntegerv(GL_MAX_FRAMEBUFFER_SAMPLES, &maxFramebufferSamples);
        glesFuncs.glGetIntegerv(GL_MAX_INTEGER_SAMPLES, &maxIntegerSamples);
        glesFuncs.glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        glesFuncs.glGetIntegerv(GL_MAX_SAMPLE_MASK_WORDS, &maxSampleMaskWords);
        // MobileGL's sample-mask state only stores a single 32-bit word (see
        // RenderState::SampleMaskValue) and SampleMaski_State() rejects any
        // maskNumber other than 0. Advertising the real driver's value here (e.g.
        // NVIDIA's GLES driver reports 2) makes glSampleMaski(1, ...) - which
        // dEQP's per-case gluStateReset always issues up to GL_MAX_SAMPLE_MASK_WORDS -
        // raise GL_INVALID_VALUE and aborts the whole glcts process after every
        // single test case. 1 is a spec-legal value (the minimum required), so cap
        // to what is actually implemented instead of forwarding the raw driver limit.
        maxSampleMaskWords = std::min(maxSampleMaskWords, 1);
        glesFuncs.glGetIntegerv(GL_MAX_PATCH_VERTICES, &maxPatchVertices);
        glesFuncs.glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTessGenLevel);
        glesFuncs.glGetIntegerv(GL_MIN_PROGRAM_TEXTURE_GATHER_OFFSET, &minProgramTextureGatherOffset);
        glesFuncs.glGetIntegerv(GL_MAX_PROGRAM_TEXTURE_GATHER_OFFSET, &maxProgramTextureGatherOffset);
        // A driver that leaves the probe untouched (pre-ES 3.1, or an ignored enum) must not
        // drag the advertised range below what GL 4.0 requires of us.
        minProgramTextureGatherOffset = std::min(minProgramTextureGatherOffset, -8);
        maxProgramTextureGatherOffset = std::max(maxProgramTextureGatherOffset, 7);
        glesFuncs.glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureImageUnits);
        glesFuncs.glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &maxVertexTextureImageUnits);
        glesFuncs.glGetIntegerv(GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS, &maxComputeTextureImageUnits);
        glesFuncs.glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxCombinedTextureImageUnits);
        glesFuncs.glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxVertexAttribs);
        glesFuncs.glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &maxComputeShaderStorageBlocks);
        glesFuncs.glGetIntegerv(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, &maxCombinedShaderStorageBlocks);
        glesFuncs.glGetIntegerv(GL_MAX_COMPUTE_UNIFORM_BLOCKS, &maxComputeUniformBlocks);
        glesFuncs.glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxComputeWorkGroupInvocations);
        glesFuncs.glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxShaderStorageBufferBindings);
        glesFuncs.glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTextureBufferSize);
        glesFuncs.glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &maxUniformBufferBindings);
        glesFuncs.glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxUniformBlockSize);
        glesFuncs.glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
        glesFuncs.glGetIntegerv(GL_MAX_COMBINED_IMAGE_UNIFORMS, &maxCombinedImageUniforms);
        glesFuncs.glGetIntegerv(GL_MAX_VERTEX_IMAGE_UNIFORMS, &maxVertexImageUniforms);
        glesFuncs.glGetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &maxFragmentImageUniforms);
        glesFuncs.glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &maxComputeImageUniforms);
        // Geometry shaders and their image-uniform query are core only in ES 3.2. DirectGLES
        // emits ESSL 3.10 on an ES 3.1 context, so reporting zero there is both legal and an
        // accurate description of what the backend compiler can consume.
        if (caps.GLESVersion.Major > 3 ||
            (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 2)) {
            glesFuncs.glGetIntegerv(GL_MAX_GEOMETRY_IMAGE_UNIFORMS, &maxGeometryImageUniforms);
        }
        glesFuncs.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
        glesFuncs.glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxColorAttachments);
        glesFuncs.glGetIntegerv(GL_MAX_CLIP_DISTANCES, &maxClipDistances);
        glesFuncs.glGetIntegerv(GL_MAX_VIEWPORTS, &maxViewports);
        glesFuncs.glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewportDims);
        glesFuncs.glGetIntegerv(GL_VIEWPORT_SUBPIXEL_BITS, &viewportSubpixelBits);
        if (caps.SupportsShaderMultisampleInterpolation && glesFuncs.glGetFloatv) {
            const auto drainErrors = [&glesFuncs]() {
                Bool hadError = false;
                if (glesFuncs.glGetError) {
                    while (glesFuncs.glGetError() != GL_NO_ERROR) hadError = true;
                }
                return hadError;
            };

            // Isolate these optional queries from errors raised by preceding capability
            // probes, then consume any query error so initialization never leaks it into
            // the application's first glGetError call.
            drainErrors();
            glesFuncs.glGetFloatv(GL_MIN_FRAGMENT_INTERPOLATION_OFFSET, &minFragmentInterpolationOffset);
            glesFuncs.glGetFloatv(GL_MAX_FRAGMENT_INTERPOLATION_OFFSET, &maxFragmentInterpolationOffset);
            glesFuncs.glGetIntegerv(GL_FRAGMENT_INTERPOLATION_OFFSET_BITS, &fragmentInterpolationOffsetBits);
            if (drainErrors()) {
                MGLOG_W("Fragment interpolation limit query failed; using OpenGL minimums");
                minFragmentInterpolationOffset = -0.5f;
                maxFragmentInterpolationOffset = 0.4375f;
                fragmentInterpolationOffsetBits = 4;
            }
        }
        // Only legal to query once the extension has been seen in the loop above, hence not batched
        // with the unconditional probes: on a driver without it this raises GL_INVALID_ENUM.
        // Core from ES 3.2 on, whatever the extension string says.
        if (caps.GLESVersion.Major > 3 || (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 2)) {
            caps.SupportsTextureBorderClamp = true;
            caps.SupportsTextureCubeMapArray = true;
        }
        if (caps.SupportsTextureFilterAnisotropy) {
            GLfloat maxTextureMaxAnisotropy = 1.0f;
            glesFuncs.glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxTextureMaxAnisotropy);
            caps.MaxTextureMaxAnisotropy = std::max(maxTextureMaxAnisotropy, 1.0f);
        }
        caps.AliasedLineWidthRangeMin = aliasedLineWidthRange[0];
        caps.AliasedLineWidthRangeMax = aliasedLineWidthRange[1];
        caps.SmoothLineWidthRangeMin = smoothLineWidthRange[0];
        caps.SmoothLineWidthRangeMax = smoothLineWidthRange[1];
        caps.SmoothLineWidthGranularity = smoothLineWidthGranularity;
        caps.PointSizeRangeMin = aliasedPointSizeRange[0];
        caps.PointSizeRangeMax = aliasedPointSizeRange[1];
        caps.PointSizeGranularity = 1.0f;
        caps.Max3DTextureSize = max3DTextureSize;
        caps.MaxArrayTextureLayers = maxArrayTextureLayers;
        caps.MaxCubeMapTextureSize = maxCubeMapTextureSize;
        caps.MaxFramebufferWidth = maxFramebufferWidth;
        caps.MaxFramebufferHeight = maxFramebufferHeight;
        caps.MaxFramebufferLayers = maxFramebufferLayers;
        caps.MaxRenderbufferSize = maxRenderbufferSize;
        caps.MaxTextureSize = maxTextureSize;
        caps.MaxColorTextureSamples = maxColorTextureSamples;
        caps.MaxDepthTextureSamples = maxDepthTextureSamples;
        caps.MaxFramebufferSamples = maxFramebufferSamples;
        caps.MaxIntegerSamples = maxIntegerSamples;
        caps.MaxSamples = maxSamples;
        caps.MaxSampleMaskWords = maxSampleMaskWords;
        caps.MaxPatchVertices = maxPatchVertices;
        caps.MaxTessGenLevel = maxTessGenLevel;
        caps.MinProgramTextureGatherOffset = minProgramTextureGatherOffset;
        caps.MaxProgramTextureGatherOffset = maxProgramTextureGatherOffset;
        caps.MaxTextureImageUnits = maxTextureImageUnits;
        caps.MaxVertexTextureImageUnits = maxVertexTextureImageUnits;
        caps.MaxComputeTextureImageUnits = maxComputeTextureImageUnits;
        caps.MaxCombinedTextureImageUnits = maxCombinedTextureImageUnits;
        caps.MaxVertexAttribs = maxVertexAttribs;
        caps.MaxComputeShaderStorageBlocks = maxComputeShaderStorageBlocks;
        caps.MaxCombinedShaderStorageBlocks = maxCombinedShaderStorageBlocks;
        caps.MaxComputeUniformBlocks = maxComputeUniformBlocks;
        caps.MaxComputeWorkGroupInvocations = maxComputeWorkGroupInvocations;
        caps.MaxShaderStorageBufferBindings = maxShaderStorageBufferBindings;
        caps.MaxTextureBufferSize = maxTextureBufferSize;
        // Through glesFuncs, like every other capability query here: a bare glGetIntegerv resolves
        // to MobileGL's own exported entry point, which answers this pname from the very
        // capability table being filled in - so the driver's real alignment never arrived and the
        // backend reported an unconstrained offset it cannot honour.
        GLint textureBufferOffsetAlignment = 1;
        glesFuncs.glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, &textureBufferOffsetAlignment);
        // Core in ES 3.2 and in EXT_texture_buffer; an older context rejects the pname and leaves
        // the default in place.
        if (glesFuncs.glGetError) {
            while (glesFuncs.glGetError() != GL_NO_ERROR) {}
        }
        caps.TextureBufferOffsetAlignment = std::max(1, textureBufferOffsetAlignment);
        caps.MaxUniformBufferBindings = maxUniformBufferBindings;
        caps.MaxUniformBlockSize = maxUniformBlockSize;
        caps.MaxImageUnits = maxImageUnits;
        caps.MaxCombinedImageUniforms = maxCombinedImageUniforms;
        caps.MaxVertexImageUniforms = maxVertexImageUniforms;
        caps.MaxGeometryImageUniforms = maxGeometryImageUniforms;
        caps.MaxFragmentImageUniforms = maxFragmentImageUniforms;
        caps.MaxComputeImageUniforms = maxComputeImageUniforms;
        caps.MaxDrawBuffers = maxDrawBuffers;
        caps.MaxColorAttachments = maxColorAttachments;
        caps.MaxClipDistances = maxClipDistances;
        caps.MaxViewports = maxViewports;
        caps.MaxViewportWidth = maxViewportDims[0];
        caps.MaxViewportHeight = maxViewportDims[1];
        caps.ViewportBoundsRangeMin = viewportBoundsRange[0];
        caps.ViewportBoundsRangeMax = viewportBoundsRange[1];
        caps.ViewportSubpixelBits = viewportSubpixelBits;
        caps.MinFragmentInterpolationOffset =
            std::isfinite(minFragmentInterpolationOffset) && minFragmentInterpolationOffset <= -0.5f
                ? minFragmentInterpolationOffset
                : -0.5f;
        caps.MaxFragmentInterpolationOffset = 0.4375f;
        caps.FragmentInterpolationOffsetBits = 4;
        if (fragmentInterpolationOffsetBits >= 4 && std::isfinite(maxFragmentInterpolationOffset)) {
            const Float requiredMaxOffset = 0.5f - std::ldexp(1.0f, -fragmentInterpolationOffsetBits);
            if (maxFragmentInterpolationOffset >= requiredMaxOffset) {
                caps.MaxFragmentInterpolationOffset = maxFragmentInterpolationOffset;
                caps.FragmentInterpolationOffsetBits = fragmentInterpolationOffsetBits;
            }
        }
        MGLOG_I("    GL_ALIASED_LINE_WIDTH_RANGE: [%.3f, %.3f]", caps.AliasedLineWidthRangeMin,
                caps.AliasedLineWidthRangeMax);
        MGLOG_I("    GL_SMOOTH_LINE_WIDTH_RANGE: [%.3f, %.3f]", caps.SmoothLineWidthRangeMin,
                caps.SmoothLineWidthRangeMax);
        MGLOG_I("    GL_SMOOTH_LINE_WIDTH_GRANULARITY: %.3f", caps.SmoothLineWidthGranularity);
        MGLOG_I("    GL_ALIASED_POINT_SIZE_RANGE: [%.3f, %.3f]", caps.PointSizeRangeMin, caps.PointSizeRangeMax);
        MGLOG_I("    GL_MAX_3D_TEXTURE_SIZE: %d", caps.Max3DTextureSize);
        MGLOG_I("    GL_MAX_ARRAY_TEXTURE_LAYERS: %d", caps.MaxArrayTextureLayers);
        MGLOG_I("    GL_MAX_CUBE_MAP_TEXTURE_SIZE: %d", caps.MaxCubeMapTextureSize);
        MGLOG_I("    GL_MAX_FRAMEBUFFER_WIDTH: %d", caps.MaxFramebufferWidth);
        MGLOG_I("    GL_MAX_FRAMEBUFFER_HEIGHT: %d", caps.MaxFramebufferHeight);
        MGLOG_I("    GL_MAX_FRAMEBUFFER_LAYERS: %d", caps.MaxFramebufferLayers);
        MGLOG_I("    GL_MAX_RENDERBUFFER_SIZE: %d", caps.MaxRenderbufferSize);
        MGLOG_I("    GL_MAX_TEXTURE_SIZE: %d", caps.MaxTextureSize);
        MGLOG_I("    GL_MAX_COLOR_TEXTURE_SAMPLES: %d", caps.MaxColorTextureSamples);
        MGLOG_I("    GL_MAX_DEPTH_TEXTURE_SAMPLES: %d", caps.MaxDepthTextureSamples);
        MGLOG_I("    GL_MAX_FRAMEBUFFER_SAMPLES: %d", caps.MaxFramebufferSamples);
        MGLOG_I("    GL_MAX_INTEGER_SAMPLES: %d", caps.MaxIntegerSamples);
        MGLOG_I("    GL_MAX_SAMPLES: %d", caps.MaxSamples);
        MGLOG_I("    GL_MAX_SAMPLE_MASK_WORDS: %d", caps.MaxSampleMaskWords);
        MGLOG_I("    GL_MAX_TEXTURE_IMAGE_UNITS: %d", caps.MaxTextureImageUnits);
        MGLOG_I("    GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: %d", caps.MaxVertexTextureImageUnits);
        MGLOG_I("    GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS: %d", caps.MaxComputeTextureImageUnits);
        MGLOG_I("    GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: %d", caps.MaxCombinedTextureImageUnits);
        MGLOG_I("    GL_MAX_VERTEX_ATTRIBS: %d", caps.MaxVertexAttribs);
        MGLOG_I("    GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS: %d", caps.MaxComputeShaderStorageBlocks);
        MGLOG_I("    GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS: %d", caps.MaxCombinedShaderStorageBlocks);
        MGLOG_I("    GL_MAX_COMPUTE_UNIFORM_BLOCKS: %d", caps.MaxComputeUniformBlocks);
        MGLOG_I("    GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS: %d", caps.MaxComputeWorkGroupInvocations);
        MGLOG_I("    GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS: %d", caps.MaxShaderStorageBufferBindings);
        MGLOG_I("    GL_MAX_TEXTURE_BUFFER_SIZE: %d", caps.MaxTextureBufferSize);
        MGLOG_I("    GL_MAX_UNIFORM_BUFFER_BINDINGS: %d", caps.MaxUniformBufferBindings);
        MGLOG_I("    GL_MAX_UNIFORM_BLOCK_SIZE: %d", caps.MaxUniformBlockSize);
        MGLOG_I("    GL_MAX_IMAGE_UNITS: %d", caps.MaxImageUnits);
        MGLOG_I("    GL_MAX_COMBINED_IMAGE_UNIFORMS: %d", caps.MaxCombinedImageUniforms);
        MGLOG_I("    GL_MAX_VERTEX_IMAGE_UNIFORMS: %d", caps.MaxVertexImageUniforms);
        MGLOG_I("    GL_MAX_GEOMETRY_IMAGE_UNIFORMS: %d", caps.MaxGeometryImageUniforms);
        MGLOG_I("    GL_MAX_FRAGMENT_IMAGE_UNIFORMS: %d", caps.MaxFragmentImageUniforms);
        MGLOG_I("    GL_MAX_COMPUTE_IMAGE_UNIFORMS: %d", caps.MaxComputeImageUniforms);
        MGLOG_I("    GL_MAX_DRAW_BUFFERS: %d", caps.MaxDrawBuffers);
        MGLOG_I("    GL_MAX_COLOR_ATTACHMENTS: %d", caps.MaxColorAttachments);
        MGLOG_I("    GL_MAX_CLIP_DISTANCES: %d", caps.MaxClipDistances);
        MGLOG_I("    GL_MAX_VIEWPORTS: %d", caps.MaxViewports);
        MGLOG_I("    GL_MAX_VIEWPORT_DIMS: [%d, %d]", caps.MaxViewportWidth, caps.MaxViewportHeight);
        MGLOG_I("    GL_VIEWPORT_BOUNDS_RANGE: [%.3f, %.3f]", caps.ViewportBoundsRangeMin,
                caps.ViewportBoundsRangeMax);
        MGLOG_I("    GL_VIEWPORT_SUBPIXEL_BITS: %d", caps.ViewportSubpixelBits);

        caps.IndirectDrawInstanceIdIncludesBaseInstance =
            ProbeIndirectInstanceIdIncludesBaseInstance(caps, glesFuncs);
        MGLOG_I("    Indirect draw gl_InstanceID includes baseInstance: %s",
                caps.IndirectDrawInstanceIdIncludesBaseInstance ? "true" : "false");

        caps.IsAngleRenderer = caps.GLESRendererString.find("ANGLE") != String::npos;
        caps.IsAngleLlvmpipeRenderer =
            caps.IsAngleRenderer && caps.GLESRendererString.find("llvmpipe") != String::npos;
        caps.AvoidSamplerMipmapMinFilter =
            caps.IsAngleLlvmpipeRenderer && MG_Config::Features.AvoidSamplerMipmapMinFilter;
        MGLOG_I("    GL_EXT_disjoint_timer_query supported: %s",
                caps.SupportsDisjointTimerQuery ? "true" : "false");
        MGLOG_I("    GL_KHR_parallel_shader_compile supported: %s",
                caps.SupportsParallelShaderCompile ? "true" : "false");
        MGLOG_I("    ANGLE renderer: %s", caps.IsAngleRenderer ? "true" : "false");
        MGLOG_I("    ANGLE llvmpipe renderer: %s", caps.IsAngleLlvmpipeRenderer ? "true" : "false");
        MGLOG_I("    Avoid sampler mipmap min filter: %s",
                caps.AvoidSamplerMipmapMinFilter ? "true" : "false");

        return true;
    }
} // namespace MobileGL::MG_Util::BackendLoader
