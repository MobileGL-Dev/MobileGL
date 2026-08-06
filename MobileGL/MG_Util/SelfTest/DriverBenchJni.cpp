// MobileGL - MobileGL/MG_Util/SelfTest/DriverBenchJni.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// In-process driver benchmark for the plugin APK's POST screen: runs the
// shared MG_Benchmark case bodies THROUGH MobileGL's full translation stack
// on the backend the caller names, and returns per-case timings as JSON.
//
// Process contract (enforced by BenchService, which hosts this in its own
// android:process and exits afterwards): the backend is latched by
// MobileGL::Initialize() from MOBILEGL_BACKEND_TYPE, and Espryt's teardown
// terminates the process-default EGL display - both make a bench run
// unrepeatable and unsafe next to a live UI. One process, one backend, one
// run.
#ifdef __ANDROID__

#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

// Includes.h first: it pins GL_GLES_PROTOTYPES=0 so the GLES headers declare
// types and enums without also declaring the entry points this library defines.
#include <Includes.h>

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <MG_Impl/EGLImpl/EGLImpl.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Drawing/GL_Drawing.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Program/GL_Program.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
// Disable/BlendFuncSeparate live in GL_RenderState.h; DrawBuffers in GL_Framebuffer.h - both already included.
#include <MG_Impl/GLImpl/Sampler/GL_Sampler.h>
#include <MG_Impl/GLImpl/Sync/GL_Sync.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/GLImpl/VertexArray/GL_VertexArray.h>

// Bind every gl*/egl* name the shared cases use to MobileGL's own frontend,
// by name and not by linkage. Writing the bare gl* symbols here would leave the
// binding to the dynamic linker, and this library legitimately has the platform
// libEGL/libGLESv3 in its own lookup scope - a bench that quietly measured the
// device driver instead of the translation layer would look like very good news.
#define glActiveTexture MobileGL::MG_Impl::GLImpl::ActiveTexture
#define glAttachShader MobileGL::MG_Impl::GLImpl::AttachShader
#define glBindAttribLocation MobileGL::MG_Impl::GLImpl::BindAttribLocation
#define glBindBuffer MobileGL::MG_Impl::GLImpl::BindBuffer
#define glBindBufferRange MobileGL::MG_Impl::GLImpl::BindBufferRange
#define glBindFramebuffer MobileGL::MG_Impl::GLImpl::BindFramebuffer
#define glBindRenderbuffer MobileGL::MG_Impl::GLImpl::BindRenderbuffer
#define glBindSampler MobileGL::MG_Impl::GLImpl::BindSampler
#define glBindTexture MobileGL::MG_Impl::GLImpl::BindTexture
#define glBindVertexArray MobileGL::MG_Impl::GLImpl::BindVertexArray
#define glBufferData MobileGL::MG_Impl::GLImpl::BufferData
#define glBufferSubData MobileGL::MG_Impl::GLImpl::BufferSubData
#define glCheckFramebufferStatus MobileGL::MG_Impl::GLImpl::CheckFramebufferStatus
#define glClear MobileGL::MG_Impl::GLImpl::Clear
#define glClearColor MobileGL::MG_Impl::GLImpl::ClearColor
#define glClientWaitSync MobileGL::MG_Impl::GLImpl::ClientWaitSync
#define glCompileShader MobileGL::MG_Impl::GLImpl::CompileShader
#define glCreateProgram MobileGL::MG_Impl::GLImpl::CreateProgram
#define glCreateShader MobileGL::MG_Impl::GLImpl::CreateShader
#define glDeleteSync MobileGL::MG_Impl::GLImpl::DeleteSync
#define glDrawArrays MobileGL::MG_Impl::GLImpl::DrawArrays
#define glDrawElements MobileGL::MG_Impl::GLImpl::DrawElements
#define glDrawElementsBaseVertex MobileGL::MG_Impl::GLImpl::DrawElementsBaseVertex
#define glEnable MobileGL::MG_Impl::GLImpl::Enable
#define glDisable MobileGL::MG_Impl::GLImpl::Disable
#define glBlendFuncSeparate MobileGL::MG_Impl::GLImpl::BlendFuncSeparate
#define glDrawBuffers MobileGL::MG_Impl::GLImpl::DrawBuffers
#define glEnableVertexAttribArray MobileGL::MG_Impl::GLImpl::EnableVertexAttribArray
#define glFenceSync MobileGL::MG_Impl::GLImpl::FenceSync
#define glFramebufferRenderbuffer MobileGL::MG_Impl::GLImpl::FramebufferRenderbuffer
#define glGenBuffers MobileGL::MG_Impl::GLImpl::GenBuffers
#define glGenFramebuffers MobileGL::MG_Impl::GLImpl::GenFramebuffers
#define glGenRenderbuffers MobileGL::MG_Impl::GLImpl::GenRenderbuffers
#define glGenSamplers MobileGL::MG_Impl::GLImpl::GenSamplers
#define glGenTextures MobileGL::MG_Impl::GLImpl::GenTextures
#define glGenVertexArrays MobileGL::MG_Impl::GLImpl::GenVertexArrays
#define glGenerateMipmap MobileGL::MG_Impl::GLImpl::GenerateMipmap
#define glGetError MobileGL::MG_Impl::GLImpl::GetError
#define glGetIntegerv MobileGL::MG_Impl::GLImpl::GetIntegerv
#define glGetProgramiv MobileGL::MG_Impl::GLImpl::GetProgramiv
#define glGetShaderInfoLog MobileGL::MG_Impl::GLImpl::GetShaderInfoLog
#define glGetShaderiv MobileGL::MG_Impl::GLImpl::GetShaderiv
#define glGetString MobileGL::MG_Impl::GLImpl::GetString
#define glGetUniformLocation MobileGL::MG_Impl::GLImpl::GetUniformLocation
#define glLinkProgram MobileGL::MG_Impl::GLImpl::LinkProgram
#define glMultiDrawElementsBaseVertex MobileGL::MG_Impl::GLImpl::MultiDrawElementsBaseVertex
#define glPixelStorei MobileGL::MG_Impl::GLImpl::PixelStorei
#define glRenderbufferStorage MobileGL::MG_Impl::GLImpl::RenderbufferStorage
#define glSamplerParameteri MobileGL::MG_Impl::GLImpl::SamplerParameteri
#define glShaderSource MobileGL::MG_Impl::GLImpl::ShaderSource
#define glTexImage2D MobileGL::MG_Impl::GLImpl::TexImage2D
#define glTexParameteri MobileGL::MG_Impl::GLImpl::TexParameteri
#define glTexSubImage2D MobileGL::MG_Impl::GLImpl::TexSubImage2D
#define glUniform1i MobileGL::MG_Impl::GLImpl::Uniform1i
#define glUniform3f MobileGL::MG_Impl::GLImpl::Uniform3f
#define glUniform3fv MobileGL::MG_Impl::GLImpl::Uniform3fv
#define glUniformMatrix4fv MobileGL::MG_Impl::GLImpl::UniformMatrix4fv
#define glUseProgram MobileGL::MG_Impl::GLImpl::UseProgram
#define glVertexAttribPointer MobileGL::MG_Impl::GLImpl::VertexAttribPointer
#define glViewport MobileGL::MG_Impl::GLImpl::Viewport
#define glFinish() ((void)0) // MobileGL's own glFinish is a no-op; never paced on

#define eglChooseConfig MobileGL::MG_Impl::EGLImpl::ChooseConfig
#define eglCreateContext MobileGL::MG_Impl::EGLImpl::CreateContext
#define eglCreatePbufferSurface MobileGL::MG_Impl::EGLImpl::CreatePbufferSurface
#define eglGetDisplay MobileGL::MG_Impl::EGLImpl::GetDisplay
#define eglGetError MobileGL::MG_Impl::EGLImpl::GetError
#define eglInitialize MobileGL::MG_Impl::EGLImpl::Initialize
#define eglMakeCurrent MobileGL::MG_Impl::EGLImpl::MakeCurrent

namespace {

    uint64_t NowNs() {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    }

    int CompareU64(const void* a, const void* b) {
        const uint64_t x = *static_cast<const uint64_t*>(a);
        const uint64_t y = *static_cast<const uint64_t*>(b);
        return x < y ? -1 : x > y;
    }
    #define now_ns NowNs
    #define cmp_u64 CompareU64

    // Failure funnel for the shared scene builder: remember the first failure,
    // let the run finish reporting it instead of aborting the service process.
    std::string g_benchFailure;
    void bench_gl_failed(const char* what, const char* detail) {
        if (!g_benchFailure.empty()) return;
        g_benchFailure = std::string(what) + (detail && detail[0] ? std::string(": ") + detail : "");
    }

    int g_frames = 120;
    int g_warmup = 30;

    // MobileGL exports the desktop multi-draw entry point on every platform, so
    // the shared cases always get the real call here (see the .inc for why this
    // is routed through a hook at all).
    void bench_multi_draw_elements_base_vertex(GLenum mode, const GLsizei* counts, GLenum type,
                                               const void* const* offsets, GLsizei drawCount,
                                               const GLint* baseVertices) {
        glMultiDrawElementsBaseVertex(mode, counts, type, offsets, drawCount, baseVertices);
    }

    #include "../../MG_Benchmark/Driver/DriverBenchCases.inc"

    // Frame boundary: a real fence wait. MobileGL's glFinish/glFlush are
    // deliberate no-ops, so a glFinish-paced loop would time only CPU submit.
    void EndFrameWait() {
        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (sync != nullptr) {
            glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync(sync);
            return;
        }
        glFinish();
    }

    std::string EscapeJson(const std::string& value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (const char c : value) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) {
                    char buffer[8];
                    snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned char>(c));
                    out += buffer;
                } else {
                    out += c;
                }
            }
        }
        return out;
    }

    struct EglBench {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLSurface surface = EGL_NO_SURFACE;
        EGLContext context = EGL_NO_CONTEXT;
    };

    // MobileGL's own EGL, driven exactly like a launcher drives it. No
    // eglTerminate here: the service process exits right after the run, and
    // Espryt's display teardown is precisely the hazard being avoided.
    bool BootEgl(EglBench& out, std::string& error) {
        out.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (out.display == EGL_NO_DISPLAY) { error = "eglGetDisplay failed"; return false; }
        EGLint major = 0, minor = 0;
        if (eglInitialize(out.display, &major, &minor) != EGL_TRUE) {
            char buffer[48];
            snprintf(buffer, sizeof buffer, "eglInitialize failed (0x%04x)", eglGetError());
            error = buffer;
            return false;
        }
        const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                                        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                        EGL_NONE};
        EGLConfig config = nullptr;
        EGLint numConfigs = 0;
        if (eglChooseConfig(out.display, configAttribs, &config, 1, &numConfigs) != EGL_TRUE ||
            numConfigs < 1) {
            error = "eglChooseConfig found no pbuffer config";
            return false;
        }
        const EGLint pbufferAttribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
        out.surface = eglCreatePbufferSurface(out.display, config, pbufferAttribs);
        if (out.surface == EGL_NO_SURFACE) {
            char buffer[56];
            snprintf(buffer, sizeof buffer, "eglCreatePbufferSurface failed (0x%04x)", eglGetError());
            error = buffer;
            return false;
        }
        const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        out.context = eglCreateContext(out.display, config, EGL_NO_CONTEXT, contextAttribs);
        if (out.context == EGL_NO_CONTEXT) {
            char buffer[48];
            snprintf(buffer, sizeof buffer, "eglCreateContext failed (0x%04x)", eglGetError());
            error = buffer;
            return false;
        }
        if (eglMakeCurrent(out.display, out.surface, out.surface, out.context) != EGL_TRUE) {
            char buffer[48];
            snprintf(buffer, sizeof buffer, "eglMakeCurrent failed (0x%04x)", eglGetError());
            error = buffer;
            return false;
        }
        return true;
    }

    std::string RunBench(const std::string& backendType, int frames, int warmup) {
        // Must land before the first EGL call: Initialize() reads the
        // environment exactly once per process.
        setenv("MOBILEGL_BACKEND_TYPE", backendType.c_str(), 1);

        EglBench egl;
        std::string error;
        if (!BootEgl(egl, error)) {
            return std::string("{\"error\":\"") + EscapeJson(error) + "\"}";
        }

        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));

        g_benchFailure.clear();
        build_resources();
        if (!g_benchFailure.empty()) {
            return std::string("{\"error\":\"") + EscapeJson(g_benchFailure) + "\"}";
        }

        if (frames < 8) frames = 8;
        if (frames > 512) frames = 512;
        if (warmup < 2) warmup = 2;
        if (warmup > 128) warmup = 128;
        g_frames = frames;
        g_warmup = warmup;

        std::string rows;
        static uint64_t samples[512];
        for (int c = 0; c < kBenchCaseCount; ++c) {
            const BenchCaseDesc& benchCase = kBenchCases[c];
            EndFrameWait();
            for (int i = 0; i < g_warmup; ++i) {
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                benchCase.fn(i, benchCase.a, benchCase.b);
                EndFrameWait();
            }
            for (int i = 0; i < g_frames; ++i) {
                const uint64_t t0 = NowNs();
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                benchCase.fn(i, benchCase.a, benchCase.b);
                EndFrameWait();
                samples[i] = NowNs() - t0;
            }
            qsort(samples, static_cast<size_t>(g_frames), sizeof(uint64_t), CompareU64);
            const uint64_t median = samples[g_frames / 2];
            const double frameMs = static_cast<double>(median) / 1e6;
            const double nsPerOp =
                benchCase.opsPerFrame > 0 ? static_cast<double>(median) / static_cast<double>(benchCase.opsPerFrame) : 0.0;
            const GLenum caseError = glGetError();

            char row[256];
            snprintf(row, sizeof row,
                     "%s{\"case\":\"%s\",\"frames\":%d,\"opsPerFrame\":%ld,\"medianFrameMs\":%.3f,"
                     "\"nsPerOp\":%.1f,\"fps\":%.1f,\"glError\":%u}",
                     rows.empty() ? "" : ",", benchCase.name, g_frames, benchCase.opsPerFrame, frameMs,
                     nsPerOp, 1e9 / static_cast<double>(median), caseError);
            rows += row;
        }

        std::string json = "{\"backend\":\"" + EscapeJson(backendType) + "\"";
        json += ",\"renderer\":\"" + EscapeJson(renderer ? renderer : "unknown") + "\"";
        json += ",\"version\":\"" + EscapeJson(version ? version : "unknown") + "\"";
        json += ",\"frames\":" + std::to_string(g_frames);
        json += ",\"cases\":[" + rows + "]}";

        // Minimal teardown: unbind so the driver flushes, then let the
        // process exit reclaim everything. eglTerminate stays un-called on
        // purpose (Espryt would take the process-default display down).
        eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return json;
    }

} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_top_mobilegl_plugin_BenchService_nativeRunDriverBench(
    JNIEnv* env, jclass, jstring backendType, jint frames, jint warmupFrames) {
    const char* backendChars = backendType ? env->GetStringUTFChars(backendType, nullptr) : nullptr;
    const std::string backend = backendChars ? backendChars : "DirectGLES";
    if (backendChars) env->ReleaseStringUTFChars(backendType, backendChars);

    std::string result;
    try {
        result = RunBench(backend, frames, warmupFrames);
    } catch (const std::exception& e) {
        result = std::string("{\"error\":\"") + EscapeJson(e.what()) + "\"}";
    } catch (...) {
        result = "{\"error\":\"unknown native exception\"}";
    }
    return env->NewStringUTF(result.c_str());
}

#endif // __ANDROID__
