// MobileGL - MobileGL/MG_Util/BackendLoaders/OpenGL/Loader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Loader.h"
#include "MG_Util/SelfTest/DriverBugProbes.h"
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
        return MG_Config::Features.EsprytUseAngle;
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
            // Optional: absent on an ES 3.2 core driver, and absent on ES 3.1 without the
            // matching extension. The tier resolution below picks whichever spelling the
            // driver's own support actually comes from.
            // Optional by nature: ES never made texture views core, so both spellings are
            // absent on plenty of drivers and neither absence is an error.
            INIT_GLES_FUNC_OPTIONAL(glTextureViewEXT)
            INIT_GLES_FUNC_OPTIONAL(glTextureViewOES)
            INIT_GLES_FUNC_OPTIONAL(glTexBufferEXT)
            INIT_GLES_FUNC_OPTIONAL(glTexBufferOES)
            INIT_GLES_FUNC_OPTIONAL(glTexBufferRangeEXT)
            INIT_GLES_FUNC_OPTIONAL(glTexBufferRangeOES)
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

            INIT_GLES_FUNC_OPTIONAL(glDrawArraysInstancedBaseInstanceEXT)
            INIT_GLES_FUNC_OPTIONAL(glDrawElementsInstancedBaseInstanceEXT)
            INIT_GLES_FUNC_OPTIONAL(glDrawElementsInstancedBaseVertexBaseInstanceEXT)
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
            // Versioned SONAME first. The unversioned "libEGL.so" is a development
            // symlink: it ships in libegl-dev/mesa-libEGL-devel, NOT in the runtime
            // package, so a machine that can run GL perfectly well may not have it -
            // every stock Ubuntu/Debian runtime image, the GitHub Actions runners
            // included. Asking only for the unversioned name there makes dlopen fail,
            // which used to leave the whole EGL function table null and take the next
            // call through a null pointer (SIGSEGV inside InitDisplayAndContext).
            // Developer machines have both names, which is exactly why this only ever
            // showed up in CI.
            eglLib = OpenLib({"libEGL.so.1", "libEGL.so"});
#endif
        }
#endif // !_WIN32

        if (!eglLib) {
            // MGLOG_F, not MGLOG_E: with no EGL there is no rendering at all, so this is a
            // bring-up abort rather than a recoverable error. It was forced to F while the
            // Log.h ordering compiled MGLOG_E out of every shipping and CI build; F is still
            // the right level on its own merits, so it stays.
            MGLOG_F("Failed to open EGL library: none of libEGL.so.1 / libEGL.so could be "
                    "dlopened; every EGL entry point will be null");
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
            /* MGLOG_F for the same reason as the open failure above: a null entry     */                              \
            /* point is a crash waiting for its first caller, and MGLOG_E is compiled  */                              \
            /* out at the INFO level every shipping and CI build uses.                 */                              \
            MGLOG_F("Failed to load EGL function: %s", #name);                                                         \
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
        // Read from caps, not re-queried: the per-stage limits are resolved (and their query
        // errors drained) before this probe runs, so asking the driver again would be a second
        // round trip that can disagree with the number MobileGL actually advertises - and, on
        // the early return below, would leave its own GL_INVALID_ENUM in the queue for the
        // application's first glGetError to find.
        const GLint maxVertexSsboBlocks = caps.MaxVertexShaderStorageBlocks;
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

    // GL 4.6 table 23.65 admits exactly four answers for GL_LAYER_PROVOKING_VERTEX and
    // GL_VIEWPORT_INDEX_PROVOKING_VERTEX. Anything else means the driver wrote something MobileGL
    // cannot forward as a convention, and GL_UNDEFINED_VERTEX - a legal answer, not a placeholder
    // - is the accurate thing to say about it.
    static GLenum NormalizeProvokingVertexConvention(GLint driverValue) {
        switch (static_cast<GLenum>(driverValue)) {
        case GL_FIRST_VERTEX_CONVENTION:
        case GL_LAST_VERTEX_CONVENTION:
        case GL_PROVOKING_VERTEX:
        case GL_UNDEFINED_VERTEX:
            return static_cast<GLenum>(driverValue);
        default:
            return GL_UNDEFINED_VERTEX;
        }
    }

    static const char* ProvokingVertexConventionName(GLenum convention) {
        switch (convention) {
        case GL_FIRST_VERTEX_CONVENTION:
            return "GL_FIRST_VERTEX_CONVENTION";
        case GL_LAST_VERTEX_CONVENTION:
            return "GL_LAST_VERTEX_CONVENTION";
        case GL_PROVOKING_VERTEX:
            return "GL_PROVOKING_VERTEX";
        default:
            return "GL_UNDEFINED_VERTEX";
        }
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
        // Resolved into caps.TextureBufferSupport below, once the ES version is also known.
        Bool hasExtTextureBuffer = false;
        Bool hasOesTextureBuffer = false;
        // Resolved into caps.SupportsTextureView below. Two spellings of one extension; the
        // entry points differ only in suffix, so unlike the buffer-texture tier there is nothing
        // downstream that needs to know WHICH one answered.
        Bool hasExtTextureView = false;
        Bool hasOesTextureView = false;
        // Combined with the three entry points below; DirectGLES emulates baseInstance when this
        // comes out false, so a stub pointer counting as support would silently break the draws.
        Bool hasBaseInstanceExtension = false;
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
                if (std::strcmp(extension, "GL_EXT_color_buffer_float") == 0) {
                    caps.SupportsColorBufferFloat = true;
                }
                if (std::strcmp(extension, "GL_EXT_color_buffer_half_float") == 0) {
                    caps.SupportsColorBufferHalfFloat = true;
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
                if (std::strcmp(extension, "GL_EXT_texture_view") == 0) {
                    hasExtTextureView = true;
                }
                if (std::strcmp(extension, "GL_OES_texture_view") == 0) {
                    hasOesTextureView = true;
                }
                if (std::strcmp(extension, "GL_EXT_texture_buffer") == 0) {
                    hasExtTextureBuffer = true;
                }
                if (std::strcmp(extension, "GL_OES_texture_buffer") == 0) {
                    hasOesTextureBuffer = true;
                }
                if (std::strcmp(extension, "GL_EXT_base_instance") == 0) {
                    hasBaseInstanceExtension = true;
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
                if (std::strcmp(extension, "GL_NV_image_formats") == 0) {
                    caps.SupportsExtendedImageFormats = true;
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
                if (std::strcmp(extension, "GL_EXT_clip_cull_distance") == 0) {
                    caps.SupportsClipDistance = true;
                }
                if (std::strcmp(extension, "GL_OES_viewport_array") == 0) {
                    caps.SupportsViewportArray = true;
                }
                // EXT wins where both are advertised: it is the spelling the Android Extension
                // Pack mandates, so it is the one a driver is most likely to have tested.
                if (std::strcmp(extension, "GL_EXT_tessellation_point_size") == 0) {
                    caps.TessellationPointSizeSupport = MG_External::GLESCapabilities::PointSizeTier::ExtensionEXT;
                } else if (std::strcmp(extension, "GL_OES_tessellation_point_size") == 0 &&
                           caps.TessellationPointSizeSupport == MG_External::GLESCapabilities::PointSizeTier::None) {
                    caps.TessellationPointSizeSupport = MG_External::GLESCapabilities::PointSizeTier::ExtensionOES;
                }
                if (std::strcmp(extension, "GL_EXT_geometry_point_size") == 0) {
                    caps.GeometryPointSizeSupport = MG_External::GLESCapabilities::PointSizeTier::ExtensionEXT;
                } else if (std::strcmp(extension, "GL_OES_geometry_point_size") == 0 &&
                           caps.GeometryPointSizeSupport == MG_External::GLESCapabilities::PointSizeTier::None) {
                    caps.GeometryPointSizeSupport = MG_External::GLESCapabilities::PointSizeTier::ExtensionOES;
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
        // All three, not any: DirectGLES picks native-vs-emulated once per draw entry point off
        // this single flag, so a driver that resolved only some of them must count as absent.
        caps.SupportsBaseInstance = hasBaseInstanceExtension &&
                                    glesFuncs.glDrawArraysInstancedBaseInstanceEXT != nullptr &&
                                    glesFuncs.glDrawElementsInstancedBaseInstanceEXT != nullptr &&
                                    glesFuncs.glDrawElementsInstancedBaseVertexBaseInstanceEXT != nullptr;
        // ES has no core texture views at any version, so this is extension-only by nature.
        // Each spelling must bring its OWN entry point: a driver that advertises the OES string
        // is not required to export glTextureViewEXT.
        caps.SupportsTextureView = (hasExtTextureView && glesFuncs.glTextureViewEXT != nullptr) ||
                                   (hasOesTextureView && glesFuncs.glTextureViewOES != nullptr);
        // Escape hatch for the integration suite: the no-extension path is the one MobileGL has
        // to refuse honestly rather than emulate, and on a driver that HAS the extension there
        // would otherwise be no way to exercise that refusal (see TextureViewScenario).
        if (std::getenv("MOBILEGL_DISABLE_TEXTURE_VIEW") != nullptr) {
            MGLOG_I("MOBILEGL_DISABLE_TEXTURE_VIEW is set; reporting no EXT/OES_texture_view support");
            caps.SupportsTextureView = false;
        }
        // Core from ES 3.2 on, so an extension string is not required there; below 3.2 the
        // extension is, and the pointer still has to have resolved either way.
        const Bool esAtLeast32 = caps.GLESVersion.Major > 3 ||
                                 (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 2);
        const Bool esAtLeast31 = caps.GLESVersion.Major > 3 ||
                                 (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 1);
        caps.SupportsDrawIndirect = esAtLeast31 && glesFuncs.glDrawArraysIndirect != nullptr &&
                                    glesFuncs.glDrawElementsIndirect != nullptr;
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
        MGLOG_I("    draw indirect (ES 3.1 core): %s", caps.SupportsDrawIndirect ? "yes" : "no");
        MGLOG_I("    multi-draw indirect (EXT_multi_draw_indirect): %s",
                caps.SupportsMultiDrawIndirect ? "yes" : "no");
        MGLOG_I("    multi-draw base vertex (EXT/OES_draw_elements_base_vertex + EXT_multi_draw_arrays): %s",
                caps.SupportsMultiDrawElementsBaseVertex ? "yes" : "no");
        MGLOG_I("    draw elements base vertex (ES 3.2 core or EXT/OES_draw_elements_base_vertex): %s",
                caps.SupportsDrawElementsBaseVertex ? "yes" : "no");
        MGLOG_I("    compute shaders (ES 3.1 core): %s", caps.SupportsComputeShader ? "yes" : "no");
        MGLOG_I("    base instance (EXT_base_instance; emulated by attribute offsets when absent): %s",
                caps.SupportsBaseInstance ? "yes" : "no");
        MGLOG_I("    clip distances (EXT_clip_cull_distance): %s", caps.SupportsClipDistance ? "yes" : "no");
        MGLOG_I("    viewport array (OES_viewport_array; gl_ViewportIndex collapses to viewport 0 when absent): %s",
                caps.SupportsViewportArray ? "yes" : "no");
        {
            const auto pointSizeTierName = [](MG_External::GLESCapabilities::PointSizeTier tier) {
                switch (tier) {
                    case MG_External::GLESCapabilities::PointSizeTier::ExtensionEXT: return "EXT";
                    case MG_External::GLESCapabilities::PointSizeTier::ExtensionOES: return "OES";
                    default: return "no";
                }
            };
            MGLOG_I("    tessellation gl_PointSize (EXT/OES_tessellation_point_size): %s",
                    pointSizeTierName(caps.TessellationPointSizeSupport));
            MGLOG_I("    geometry gl_PointSize (EXT/OES_geometry_point_size): %s",
                    pointSizeTierName(caps.GeometryPointSizeSupport));
        }

        // LOAD-BEARING STRING, not just a banner. android-plugin/trace-replay-ci.sh's
        // is_angle_surface_lost() greps mobilegl.log for exactly "OpenGL ES capabilities:" to
        // decide whether MobileGL got far enough to have a working context: if the probe ran,
        // a later surface loss is a real defect rather than an emulator fault worth retrying.
        // Demoting this line, renaming it, or moving it before the context is usable silently
        // inverts that retry logic. It is init-phase, so MGLOG_I is correct and it stays.
        MGLOG_I("OpenGL ES capabilities:");
        glesFuncs.glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &caps.UniformBufferOffsetAlignment);
        MGLOG_I("    GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: %d", caps.UniformBufferOffsetAlignment);
        // ES 3.1 core, so no extension gate - but a driver that somehow leaves it at zero would
        // make every storage-range offset legal, so an unusable answer keeps the 256 default.
        GLint shaderStorageOffsetAlignment = 0;
        glesFuncs.glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &shaderStorageOffsetAlignment);
        while (glesFuncs.glGetError() != GL_NO_ERROR) {
        }
        if (shaderStorageOffsetAlignment > 0) {
            caps.ShaderStorageBufferOffsetAlignment = shaderStorageOffsetAlignment;
        }
        MGLOG_I("    GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT: %d", caps.ShaderStorageBufferOffsetAlignment);
        GLfloat aliasedLineWidthRange[2] = {1.0f, 1.0f};
        GLfloat smoothLineWidthRange[2] = {1.0f, 1.0f};
        GLfloat smoothLineWidthGranularity = 1.0f;
        GLfloat aliasedPointSizeRange[2] = {1.0f, 1.0f};
        // GL 4.6 core table 23.60 sets the MINIMUM VIEWPORT_BOUNDS_RANGE at [-32768, 32767], and
        // KHR-GL43.viewport_array.queries asserts exactly that floor. GLES has no such query, so
        // the glGetFloatv below raises GL_INVALID_ENUM and leaves this untouched - starting it at
        // {0, 0} advertised a range that admits no viewport origin at all.
        GLfloat viewportBoundsRange[2] = {-32768.0f, 32767.0f};
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
        // ES 3.2 table 21.44 minimums. Zero for the four graphics stages below fragment is not a
        // placeholder - it is what the spec permits and what ARM's GLES driver actually reports,
        // so a probe that never runs (pre-ES 3.2, unsupported pname) leaves behind the truthful
        // answer rather than an optimistic one.
        GLint maxVertexShaderStorageBlocks = 0;
        GLint maxTessControlShaderStorageBlocks = 0;
        GLint maxTessEvaluationShaderStorageBlocks = 0;
        GLint maxGeometryShaderStorageBlocks = 0;
        GLint maxFragmentShaderStorageBlocks = 4;
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
        // Zero is a legal answer, not a placeholder. GL_MAX_CLIP_DISTANCES exists in ES only as
        // GL_MAX_CLIP_DISTANCES_EXT under GL_EXT_clip_cull_distance, so on a driver without that
        // extension there is nowhere to put a clip distance at all: SPIRV-Cross emits
        // gl_ClipDistance behind an `#extension ... : require` the ESSL compiler rejects, and
        // DirectGLES has no state to forward the per-distance enables into (see the gate in
        // DirectGLES::SyncRenderState). Starting at 8 meant a probe that could never run left an
        // optimistic 8 behind, so the frontend promised eight clip planes and every draw with a
        // clipping program silently rendered nothing. The guarded probe below only ever widens it.
        GLint maxClipDistances = 0;
        // The cull half of the same extension, and the same "zero is a legal answer" rule: a cull
        // distance discards the whole primitive, so promising eight on a driver that has none does
        // not fail loudly, it drops every draw of a culling program.
        GLint maxCullDistances = 0;
        GLint maxCombinedClipAndCullDistances = 0;
        GLint maxViewports = 16;
        // GL_UNDEFINED_VERTEX is what stands when the probes below cannot run, and it is a legal
        // answer rather than a placeholder: with neither geometry shaders nor a viewport array
        // there is no layered or multi-viewport draw for a convention to describe.
        GLenum layerProvokingVertex = GL_UNDEFINED_VERTEX;
        GLenum viewportIndexProvokingVertex = GL_UNDEFINED_VERTEX;
        GLfloat minFragmentInterpolationOffset = -0.5f;
        GLfloat maxFragmentInterpolationOffset = 0.4375f;
        GLint fragmentInterpolationOffsetBits = 4;
        // Core minimums of both APIs (GL 4.6 table 23.53, ES 3.1 table 20.40); the probe
        // below only ever widens them.
        GLint minProgramTextureGatherOffset = -8;
        GLint maxProgramTextureGatherOffset = 7;
        GLint maxPatchVertices = 32;
        GLint maxTessGenLevel = 64;
        // Function-scope, and used by every probe group below rather than redeclared inside each
        // one. Returns whether anything was drained, which is what lets a group tell "the driver
        // answered" from "the driver rejected the pname and left my local alone".
        const auto drainErrors = [&glesFuncs]() {
            Bool hadError = false;
            if (glesFuncs.glGetError) {
                while (glesFuncs.glGetError() != GL_NO_ERROR) hadError = true;
            }
            return hadError;
        };

        // THE GENERATOR OF THIS WHOLE BUG FAMILY, closed here. A bare glGetIntegerv/glGetFloatv
        // of a pname the driver does not have does two damaging things at once: it leaves the
        // local at whatever the declaration initialised it to - an optimistic number the frontend
        // then advertises as a capability - and it leaves a GL_INVALID_ENUM in the queue where
        // the next unrelated probe's caller, or the application's first glGetError, gets blamed
        // for it. The per-stage storage block, fragment interpolation and buffer texture probes
        // below already drain and fall back; this unconditional run did neither, which is how
        // GL_MAX_CLIP_DISTANCES came to be advertised as 8 on a driver with no clip distances at
        // all. Every pname here that is not ES core is now either gated on the capability that
        // makes it exist or floored at the value a rejected probe would have left, and the whole
        // run is bracketed by a drain.
        drainErrors();
        glesFuncs.glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, aliasedLineWidthRange);
        // GL_SMOOTH_LINE_WIDTH_RANGE / GL_SMOOTH_LINE_WIDTH_GRANULARITY (0x0B22 / 0x0B23) are
        // desktop-only - ES has never had an antialiased line width query - so on a real GLES
        // driver these two raise GL_INVALID_ENUM. Kept as probes rather than dropped because the
        // ANGLE and desktop-GL hosts MobileGL also runs on do answer them; the initialisers are
        // the GL 4.6 table 23.55 minimum of [1, 1], which is both the honest answer for a driver
        // that cannot say and what an untouched out-param already holds.
        glesFuncs.glGetFloatv(GL_SMOOTH_LINE_WIDTH_RANGE, smoothLineWidthRange);
        glesFuncs.glGetFloatv(GL_SMOOTH_LINE_WIDTH_GRANULARITY, &smoothLineWidthGranularity);
        glesFuncs.glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, aliasedPointSizeRange);
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
        // The multisample ceilings above are ES 3.1 state apart from GL_MAX_SAMPLES, which is ES
        // 3.0, so a 3.0 context rejects five of the six and leaves whatever the out-param held.
        // One sample is what a rejected probe leaves behind and is also the smallest legal
        // answer, so clamp rather than trust: a zero reaching GL_Getter would have the frontend
        // reject the very sample count it just advertised (see GetAdvertisedMaxSamples).
        maxColorTextureSamples = std::max(maxColorTextureSamples, 1);
        maxDepthTextureSamples = std::max(maxDepthTextureSamples, 1);
        maxFramebufferSamples = std::max(maxFramebufferSamples, 1);
        maxIntegerSamples = std::max(maxIntegerSamples, 1);
        maxSamples = std::max(maxSamples, 1);
        maxSampleMaskWords = std::max(maxSampleMaskWords, 1);
        // ES 3.2 core, or EXT_tessellation_shader on 3.1. Probed rather than version-gated so a
        // 3.1 driver that HAS the extension still gets to answer; the clamp below is what makes a
        // rejected query safe, since GL 4.6 table 23.66 and ES 3.2 table 21.45 set the same
        // minimums the initialisers carry and neither API permits less.
        glesFuncs.glGetIntegerv(GL_MAX_PATCH_VERTICES, &maxPatchVertices);
        glesFuncs.glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTessGenLevel);
        maxPatchVertices = std::max(maxPatchVertices, 32);
        maxTessGenLevel = std::max(maxTessGenLevel, 64);
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
        // GL_MAX_TEXTURE_BUFFER_SIZE is deliberately NOT batched here: like
        // GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT below, the pname only exists once buffer textures do,
        // so on a driver without them it raises GL_INVALID_ENUM, leaves the local at MobileGL's own
        // floor, and - because nothing drains the queue until the alignment probe far below - lets
        // that error be misattributed to any query in between. It is queried in the guarded block
        // that resolves caps.TextureBufferSupport instead.
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
        // Closes the bracket opened before the run: every local above now holds either the
        // driver's answer or a floor, and nothing this function asked for is left in the error
        // queue for a later probe - or the application - to be blamed for.
        if (drainErrors()) {
            MGLOG_W("One or more capability queries were rejected by this driver; the affected "
                    "limits keep MobileGL's spec-minimum floors");
        }
        // Per-stage storage-block counts. Deliberately NOT batched with the unconditional probes
        // above, for the reason GL_MAX_TEXTURE_BUFFER_SIZE is not: the vertex and fragment pnames
        // are ES 3.1, but the tessellation and geometry ones only exist from ES 3.2 on (or under
        // EXT_tessellation_shader / EXT_geometry_shader), so on an older context they raise
        // GL_INVALID_ENUM, leave the local untouched, and - with nothing draining the queue until
        // some later probe - let that error be misattributed to an unrelated query in between, or
        // leak into the application's first glGetError.
        //
        // A stage whose probe does not run keeps the spec minimum, which for all four graphics
        // stages is 0. That is the honest answer: DirectGLES emits ESSL 3.10 on an ES 3.1 context,
        // where those stages do not exist at all.
        {
            // Isolate from errors raised by the preceding probes so the drain below reports on
            // these queries only.
            drainErrors();
            glesFuncs.glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &maxVertexShaderStorageBlocks);
            glesFuncs.glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &maxFragmentShaderStorageBlocks);
            if (drainErrors()) {
                MGLOG_W("Per-stage shader storage block query failed for the vertex/fragment "
                        "stages; assuming the ES minimums (vertex 0, fragment 4)");
                maxVertexShaderStorageBlocks = 0;
                maxFragmentShaderStorageBlocks = 4;
            }
            if (esAtLeast32) {
                glesFuncs.glGetIntegerv(GL_MAX_TESS_CONTROL_SHADER_STORAGE_BLOCKS,
                                        &maxTessControlShaderStorageBlocks);
                glesFuncs.glGetIntegerv(GL_MAX_TESS_EVALUATION_SHADER_STORAGE_BLOCKS,
                                        &maxTessEvaluationShaderStorageBlocks);
                glesFuncs.glGetIntegerv(GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS, &maxGeometryShaderStorageBlocks);
                if (drainErrors()) {
                    MGLOG_W("Per-stage shader storage block query failed for the tessellation/"
                            "geometry stages; assuming the ES minimum of 0");
                    maxTessControlShaderStorageBlocks = 0;
                    maxTessEvaluationShaderStorageBlocks = 0;
                    maxGeometryShaderStorageBlocks = 0;
                }
            }
            // A driver is free to report a negative or nonsensical count into an untouched
            // out-param; clamp before anything downstream treats it as a capacity.
            maxVertexShaderStorageBlocks = std::max(maxVertexShaderStorageBlocks, 0);
            maxTessControlShaderStorageBlocks = std::max(maxTessControlShaderStorageBlocks, 0);
            maxTessEvaluationShaderStorageBlocks = std::max(maxTessEvaluationShaderStorageBlocks, 0);
            maxGeometryShaderStorageBlocks = std::max(maxGeometryShaderStorageBlocks, 0);
            maxFragmentShaderStorageBlocks = std::max(maxFragmentShaderStorageBlocks, 0);
        }
        glesFuncs.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
        glesFuncs.glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxColorAttachments);
        // GL_MAX_CLIP_DISTANCES is 0x0D32, which ES only ever spells GL_MAX_CLIP_DISTANCES_EXT and
        // only ever has under GL_EXT_clip_cull_distance. The extension was already resolved into
        // caps.SupportsClipDistance a few hundred lines above and is the same flag DirectGLES
        // gates the CLIP_DISTANCEi enable forwarding on, so ask the driver only where the pname
        // exists; everywhere else the honest 0 stands and no GL_INVALID_ENUM is left behind for an
        // unrelated query - or the application's first glGetError - to trip over.
        if (caps.SupportsClipDistance) {
            drainErrors();
            glesFuncs.glGetIntegerv(GL_MAX_CLIP_DISTANCES, &maxClipDistances);
            if (drainErrors()) {
                MGLOG_W("GL_EXT_clip_cull_distance is advertised but GL_MAX_CLIP_DISTANCES was "
                        "rejected; reporting no clip distances");
                maxClipDistances = 0;
            }
            // GL_MAX_CULL_DISTANCES_EXT (0x82F9) and GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES_EXT
            // (0x82FA) are the same tokens as their desktop spellings and arrive with the same
            // extension, so they are probed under the same guard and the same drain sandwich.
            drainErrors();
            glesFuncs.glGetIntegerv(GL_MAX_CULL_DISTANCES, &maxCullDistances);
            if (drainErrors()) {
                MGLOG_W("GL_EXT_clip_cull_distance is advertised but GL_MAX_CULL_DISTANCES was "
                        "rejected; reporting no cull distances");
                maxCullDistances = 0;
            }
            drainErrors();
            glesFuncs.glGetIntegerv(GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES, &maxCombinedClipAndCullDistances);
            if (drainErrors()) {
                MGLOG_W("GL_EXT_clip_cull_distance is advertised but "
                        "GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES was rejected; deriving it from the pair");
                maxCombinedClipAndCullDistances = 0;
            }
        }
        glesFuncs.glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewportDims);
        // GL_LAYER_PROVOKING_VERTEX is ES 3.2 core (it arrives with geometry shaders, which is
        // what gl_Layer needs). Ask the driver where the pname exists rather than asserting a
        // convention: it is a statement about which vertex of a primitive supplies gl_Layer, and
        // MobileGL forwards the geometry stage to the driver rather than implementing the
        // selection itself, so the driver's answer IS MobileGL's answer. Below ES 3.2 there are
        // no layered draws to have a convention for and GL_UNDEFINED_VERTEX stands, which GL 4.6
        // table 23.65 explicitly permits.
        if (esAtLeast32) {
            GLint driverLayerConvention = static_cast<GLint>(GL_UNDEFINED_VERTEX);
            drainErrors();
            glesFuncs.glGetIntegerv(GL_LAYER_PROVOKING_VERTEX, &driverLayerConvention);
            if (!drainErrors()) {
                layerProvokingVertex = NormalizeProvokingVertexConvention(driverLayerConvention);
            }
        }
        // GL_MAX_VIEWPORTS (0x825B), GL_VIEWPORT_SUBPIXEL_BITS (0x825C) and GL_VIEWPORT_BOUNDS_RANGE
        // (0x825D) all arrive with GL_OES_viewport_array and exist nowhere in ES core, so on the
        // drivers DirectGLES actually runs on all three raise GL_INVALID_ENUM. The values MobileGL
        // advertises do not change by asking: GL_Getter answers GL_MAX_VIEWPORTS from the frontend
        // state width (indexed viewport entry points validate against RenderStateParameters::
        // MAX_VIEWPORTS, so a device answer of 1 would reject indices the state can legitimately
        // hold), floors GL_SUBPIXEL_BITS at its own 4, and the bounds range is clamped to the core
        // minimum below. What changes is that the errors stop being manufactured.
        if (caps.SupportsViewportArray) {
            GLint driverViewportIndexConvention = static_cast<GLint>(GL_UNDEFINED_VERTEX);
            drainErrors();
            glesFuncs.glGetIntegerv(GL_MAX_VIEWPORTS, &maxViewports);
            glesFuncs.glGetIntegerv(GL_VIEWPORT_SUBPIXEL_BITS, &viewportSubpixelBits);
            glesFuncs.glGetIntegerv(GL_VIEWPORT_INDEX_PROVOKING_VERTEX, &driverViewportIndexConvention);
            if (glesFuncs.glGetFloatv) {
                glesFuncs.glGetFloatv(GL_VIEWPORT_BOUNDS_RANGE, viewportBoundsRange);
            }
            if (drainErrors()) {
                MGLOG_W("GL_OES_viewport_array is advertised but its viewport limit queries were "
                        "rejected; keeping the OpenGL core minimums");
                maxViewports = 16;
                viewportSubpixelBits = 0;
                viewportBoundsRange[0] = -32768.0f;
                viewportBoundsRange[1] = 32767.0f;
            } else {
                viewportIndexProvokingVertex =
                    NormalizeProvokingVertexConvention(driverViewportIndexConvention);
            }
        }
        if (caps.SupportsShaderMultisampleInterpolation && glesFuncs.glGetFloatv) {
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
        // Buffer-texture tier. Core from ES 3.2 on; below that the EXT spelling is preferred over
        // the OES one purely because SPIRV-Cross emits GL_EXT_texture_buffer natively, so a driver
        // with both needs no directive retargeting. The entry point has to have resolved either
        // way - the extension string alone is not support (see the multi-draw note above).
        {
            using Tier = MG_External::GLESCapabilities::TextureBufferTier;
            // Each tier needs the entry point that tier's support actually ships. Gating all
            // three on the unsuffixed name - the ES 3.2 CORE spelling - would make every
            // EXT/OES driver look unsupported on a strict loader, and would make MobileGL call
            // a core entry point the driver never exported on a permissive one. The suffixed
            // name is preferred where the support is an extension, with the core name accepted
            // as a fallback because drivers that expose both alias them.
            const Bool hasCoreEntryPoint = glesFuncs.glTexBuffer != nullptr;
            if (esAtLeast32 && hasCoreEntryPoint) {
                caps.TextureBufferSupport = Tier::CoreEs32;
            } else if (hasExtTextureBuffer && (glesFuncs.glTexBufferEXT != nullptr || hasCoreEntryPoint)) {
                caps.TextureBufferSupport = Tier::ExtensionEXT;
            } else if (hasOesTextureBuffer && (glesFuncs.glTexBufferOES != nullptr || hasCoreEntryPoint)) {
                caps.TextureBufferSupport = Tier::ExtensionOES;
            } else {
                caps.TextureBufferSupport = Tier::None;
            }

            // Assigned unconditionally, like every other capability in this function, so a
            // second fill on a reused struct cannot keep a stale true.
            caps.MaxTextureBufferSizeIsDriverReported = false;
            if (caps.TextureBufferSupport != Tier::None) {
                // Drain first: an error left by any earlier probe would otherwise read as this
                // query having failed, and the value would be discarded as a non-answer.
                if (glesFuncs.glGetError) {
                    while (glesFuncs.glGetError() != GL_NO_ERROR) {
                    }
                }
                glesFuncs.glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTextureBufferSize);
                if (glesFuncs.glGetError) {
                    Bool queryFailed = false;
                    while (glesFuncs.glGetError() != GL_NO_ERROR) {
                        queryFailed = true;
                    }
                    caps.MaxTextureBufferSizeIsDriverReported = !queryFailed;
                } else {
                    caps.MaxTextureBufferSizeIsDriverReported = true;
                }
            }
            // On the None tier the local keeps MobileGL's floor and
            // MaxTextureBufferSizeIsDriverReported stays false. The floor, not 0, is what the
            // frontend goes on advertising: MobileGL reports an OpenGL 4.x context, where buffer
            // textures are core and GL_MAX_TEXTURE_BUFFER_SIZE has a spec minimum of 65536, so 0
            // would be an illegal answer that no conformant app is prepared to read (several
            // divide by it or size an allocation with it). The dishonesty is contained by making
            // the missing capability loud instead - at capability init here, at glTexBuffer, at
            // program build, and as its own driver POST row - because GL offers no way to say
            // "buffer textures exist but cannot work".
            if (caps.TextureBufferSupport == Tier::None) {
                // Two ways to land here, and they are worth telling apart: the ordinary one (too
                // old, no extension) and the pathological one (the driver says it has them but
                // no entry point resolved), which is a driver or loader fault, not a missing
                // feature.
                const Bool claimsSupport = esAtLeast32 || hasExtTextureBuffer || hasOesTextureBuffer;
                MGLOG_I("    Buffer textures: UNSUPPORTED (%s). Any shader sampling a "
                        "samplerBuffer will fail to compile, and MobileGL keeps advertising "
                        "GL_MAX_TEXTURE_BUFFER_SIZE = %d because a GL 4.x context may not report 0.",
                        claimsSupport
                            ? "this driver advertises buffer textures but no glTexBuffer entry "
                              "point resolved, so none of them can be called"
                            : "ES core needs 3.2, and neither GL_EXT_texture_buffer nor "
                              "GL_OES_texture_buffer is present",
                        maxTextureBufferSize);
            }
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
        // Not the driver's answer alone: MobileGL emits every vertex input as a
        // layout(location = N) qualifier, so an attribute the driver counts but its ESSL
        // compiler will not let anything DECLARE is not an attribute MobileGL can hand to an
        // application. The probe measures where the qualifier actually stops (see
        // SelfTest::ProbeExplicitVertexInputLocationCeiling - Adreno 830 advertises 32 and
        // refuses the qualifier from 16 up) and answers with the advertised count on every
        // driver that has no such gap and on any run that reaches no verdict, so this only ever
        // lowers the number, and only on evidence.
        const SelfTest::VertexInputLocationCeilingMeasurement& locationCeiling =
            SelfTest::ExplicitVertexInputLocationCeiling(glesFuncs);
        // Guarded on `detected` rather than on the number alone: a probe that reached no verdict
        // has measured nothing, and the clamp must be driven by evidence or not applied at all.
        caps.MaxVertexAttribs = locationCeiling.detected
                                    ? std::min(maxVertexAttribs, locationCeiling.usableLocations)
                                    : maxVertexAttribs;
        if (locationCeiling.detected) {
            MGLOG_I("    GL_MAX_VERTEX_ATTRIBS reduced from the driver's %d to %d: "
                    "layout(location = N) on a vertex input is refused from N = %d upward",
                    maxVertexAttribs, caps.MaxVertexAttribs, locationCeiling.usableLocations);
        }
        caps.MaxComputeShaderStorageBlocks = maxComputeShaderStorageBlocks;
        caps.MaxCombinedShaderStorageBlocks = maxCombinedShaderStorageBlocks;
        caps.MaxVertexShaderStorageBlocks = maxVertexShaderStorageBlocks;
        caps.MaxTessControlShaderStorageBlocks = maxTessControlShaderStorageBlocks;
        caps.MaxTessEvaluationShaderStorageBlocks = maxTessEvaluationShaderStorageBlocks;
        caps.MaxGeometryShaderStorageBlocks = maxGeometryShaderStorageBlocks;
        caps.MaxFragmentShaderStorageBlocks = maxFragmentShaderStorageBlocks;
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
        // A driver is free to write nonsense into an out-param it then rejects, and without the
        // extension the probe above never ran at all - so the flag, not the local, decides.
        caps.MaxClipDistances = caps.SupportsClipDistance ? std::max(maxClipDistances, 0) : 0;
        caps.MaxCullDistances = caps.SupportsClipDistance ? std::max(maxCullDistances, 0) : 0;
        // The combined limit can never be smaller than either half (GL 4.6 core 11.1.3.10 / the
        // EXT spec say so), so a driver that rejected the combined query but answered the other
        // two still gets a usable - and never over-stated - number.
        caps.MaxCombinedClipAndCullDistances =
            caps.SupportsClipDistance
                ? std::max({maxCombinedClipAndCullDistances, caps.MaxClipDistances, caps.MaxCullDistances})
                : 0;
        caps.MaxViewports = maxViewports;
        caps.LayerProvokingVertex = layerProvokingVertex;
        caps.ViewportIndexProvokingVertex = viewportIndexProvokingVertex;
        caps.MaxViewportWidth = maxViewportDims[0];
        caps.MaxViewportHeight = maxViewportDims[1];
        // Only ever WIDER than the core minimum: a driver that answered the query is allowed to
        // exceed the floor but never to sit inside it, and a driver that rejected the query left
        // the floor in place. Written as a clamp rather than a plain assignment so a partial
        // write (one component answered, the other not) cannot narrow the range either.
        caps.ViewportBoundsRangeMin = std::min(viewportBoundsRange[0], -32768.0f);
        caps.ViewportBoundsRangeMax = std::max(viewportBoundsRange[1], 32767.0f);
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
        // Worth a line each: a zero here is what stops an application's storage block from ever
        // working in that stage, and reading it back from an artifact is the difference between
        // "MobileGL dropped my draw" and "this driver has no SSBOs outside compute".
        MGLOG_I("    GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS: %d", caps.MaxVertexShaderStorageBlocks);
        MGLOG_I("    GL_MAX_TESS_CONTROL_SHADER_STORAGE_BLOCKS: %d", caps.MaxTessControlShaderStorageBlocks);
        MGLOG_I("    GL_MAX_TESS_EVALUATION_SHADER_STORAGE_BLOCKS: %d", caps.MaxTessEvaluationShaderStorageBlocks);
        MGLOG_I("    GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS: %d", caps.MaxGeometryShaderStorageBlocks);
        MGLOG_I("    GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS: %d", caps.MaxFragmentShaderStorageBlocks);
        MGLOG_I("    GL_MAX_COMPUTE_UNIFORM_BLOCKS: %d", caps.MaxComputeUniformBlocks);
        MGLOG_I("    GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS: %d", caps.MaxComputeWorkGroupInvocations);
        MGLOG_I("    GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS: %d", caps.MaxShaderStorageBufferBindings);
        // Three distinct states, and the suffix must not conflate them: a driver answer, a floor
        // kept because there are no buffer textures to ask about, and a floor kept because the
        // driver claimed buffer textures but then refused the query (which is a driver bug worth
        // seeing spelled out rather than hidden behind the same wording as the honest case).
        MGLOG_I("    GL_MAX_TEXTURE_BUFFER_SIZE: %d%s", caps.MaxTextureBufferSize,
                caps.MaxTextureBufferSizeIsDriverReported
                    ? ""
                    : (caps.TextureBufferSupport == MG_External::GLESCapabilities::TextureBufferTier::None
                           ? " (MobileGL floor - the driver has no buffer textures to ask)"
                           : " (MobileGL floor - the driver claims buffer textures but rejected the query)"));
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
        // Worth spelling the reason out for the same reason the per-stage storage block counts
        // are: a zero here is what stops an application's gl_ClipDistance from ever clipping, and
        // reading it back from an artifact is the difference between "MobileGL dropped my draw"
        // and "this driver has no clip distances".
        MGLOG_I("    GL_MAX_CLIP_DISTANCES: %d%s", caps.MaxClipDistances,
                caps.SupportsClipDistance ? "" : " (no GL_EXT_clip_cull_distance on this driver)");
        MGLOG_I("    GL_MAX_CULL_DISTANCES: %d", caps.MaxCullDistances);
        MGLOG_I("    GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES: %d", caps.MaxCombinedClipAndCullDistances);
        MGLOG_I("    GL_MAX_VIEWPORTS: %d", caps.MaxViewports);
        MGLOG_I("    GL_MAX_VIEWPORT_DIMS: [%d, %d]", caps.MaxViewportWidth, caps.MaxViewportHeight);
        MGLOG_I("    GL_VIEWPORT_BOUNDS_RANGE: [%.3f, %.3f]", caps.ViewportBoundsRangeMin,
                caps.ViewportBoundsRangeMax);
        MGLOG_I("    GL_VIEWPORT_SUBPIXEL_BITS: %d", caps.ViewportSubpixelBits);
        MGLOG_I("    GL_LAYER_PROVOKING_VERTEX: %s", ProvokingVertexConventionName(caps.LayerProvokingVertex));
        MGLOG_I("    GL_VIEWPORT_INDEX_PROVOKING_VERTEX: %s",
                ProvokingVertexConventionName(caps.ViewportIndexProvokingVertex));

        caps.IndirectDrawInstanceIdIncludesBaseInstance =
            ProbeIndirectInstanceIdIncludesBaseInstance(caps, glesFuncs);
        MGLOG_I("    Indirect draw gl_InstanceID includes baseInstance: %s",
                caps.IndirectDrawInstanceIdIncludesBaseInstance ? "true" : "false");

        // ForceOn means "emit the blocks unlocated", i.e. treat the driver as NOT supporting
        // located blocks - which is why the override reads inverted here. Auto is the probe's
        // own answer and is what every real run uses; the two forced settings exist so the
        // emulation can be exercised on a healthy driver (the integration lane) and turned
        // off again as a negative control.
        switch (MG_Config::Features.EsprytUnlocatedIoBlocks) {
            case MG_Config::QuirkOverride::ForceOn:
                caps.SupportsLocatedInterStageIoBlocks = false;
                MGLOG_I("    Located inter-stage interface blocks: forced OFF by "
                        "MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS; the driver was not probed");
                break;
            case MG_Config::QuirkOverride::ForceOff:
                caps.SupportsLocatedInterStageIoBlocks = true;
                MGLOG_I("    Located inter-stage interface blocks: forced ON by "
                        "MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS; the driver was not probed");
                break;
            case MG_Config::QuirkOverride::Auto:
            default:
                // SelfTest::ProbeLocatedIoBlocksLosePayload - the Mali-G1-Ultra ES driver
                // delivers nothing through an interface block that carries an explicit
                // layout(location=) once a tessellation or geometry stage is in the pipeline.
                // Probed with its own controls rather than matched on a renderer string; see
                // DriverBugProbes.h for the shape and for why the two controls decide what the
                // finding is allowed to claim.
                caps.SupportsLocatedInterStageIoBlocks =
                    !SelfTest::LocatedIoBlocksLosePayload(glesFuncs).detected;
                break;
        }
        MGLOG_I("    Located inter-stage interface blocks transport their payload: %s",
                caps.SupportsLocatedInterStageIoBlocks
                    ? "true"
                    : "false (DirectGLES will emit tessellation/geometry programs' interface "
                      "blocks without a location qualifier)");

        caps.IsAngleRenderer = caps.GLESRendererString.find("ANGLE") != String::npos;
        caps.IsAngleLlvmpipeRenderer =
            caps.IsAngleRenderer && caps.GLESRendererString.find("llvmpipe") != String::npos;
        caps.AvoidSamplerMipmapMinFilter =
            caps.IsAngleLlvmpipeRenderer && MG_Config::Features.EsprytAvoidSamplerMipmapMinFilter;
        caps.AvoidExplicitLodBias =
            caps.IsAngleLlvmpipeRenderer && MG_Config::Features.EsprytAvoidExplicitLodBias;
        MGLOG_I("    GL_EXT_disjoint_timer_query supported: %s",
                caps.SupportsDisjointTimerQuery ? "true" : "false");
        MGLOG_I("    GL_KHR_parallel_shader_compile supported: %s",
                caps.SupportsParallelShaderCompile ? "true" : "false");
        MGLOG_I("    ANGLE renderer: %s", caps.IsAngleRenderer ? "true" : "false");
        MGLOG_I("    ANGLE llvmpipe renderer: %s", caps.IsAngleLlvmpipeRenderer ? "true" : "false");
        MGLOG_I("    Avoid sampler mipmap min filter: %s",
                caps.AvoidSamplerMipmapMinFilter ? "true" : "false");
        MGLOG_I("    Avoid explicit LOD bias: %s", caps.AvoidExplicitLodBias ? "true" : "false");

        // Last line of defence. Capability init is the very first thing that touches the driver,
        // so anything it leaves in the error queue surfaces at the APPLICATION's first
        // glGetError and gets attributed to whatever call the app happened to make. Every group
        // above drains its own, but a probe added later must not be able to reintroduce the leak.
        if (drainErrors()) {
            MGLOG_W("Capability initialization left a GL error behind; it has been consumed so it "
                    "cannot surface at the application's first glGetError");
        }
        return true;
    }
} // namespace MobileGL::MG_Util::BackendLoader
