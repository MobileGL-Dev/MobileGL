/* MobileGL - MobileGL/MG_Benchmark/Driver/DriverBench.c
 * Copyright (c) 2025-2026 MobileGL-Dev
 * Licensed under the GNU Lesser General Public License v3.0:
 *   https://www.gnu.org/licenses/gpl-3.0.txt
 *   https://www.gnu.org/licenses/lgpl-3.0.txt
 * SPDX-License-Identifier: LGPL-3.0-only
 * End of Source File Header
 *
 * Headless, EGL-based driver benchmark shaped like Minecraft's GL usage.
 * Unlike the MobileGL_s microbenches next door this exercises a full GL
 * stack: it dlopens ONE EGL provider ($DRIVERBENCH_EGL_LIB - the system
 * libEGL.so.1 for the native driver, or a libMobileGL.so path for either
 * MobileGL backend selected with MOBILEGL_BACKEND_TYPE), creates a desktop-GL
 * context on a small pbuffer, renders into its own FBO and paces frames with
 * glFinish. No window system is required beyond what the provider itself
 * needs - see run_driver_bench.sh.
 *
 * Every case models one hot pattern from captured Minecraft traces:
 *   draw_tiny        back-to-back glDrawElements, shared state (chunk batch)
 *   draw_uniform     per-draw vec3 offset uniform + draw (chunk sections)
 *   draw_multi_vao   per-draw VAO/VBO switch + draw (per-section buffers)
 *   tex_pingpong     per-draw texture bind churn on one unit
 *   program_pingpong alternate two programs + mat4 upload (chunk<->entity)
 *   chunk_upload     glBufferData(NULL) orphan + glBufferSubData + draw
 *   atlas_sprite     N 16x16 glTexSubImage2D into a 1024x512 atlas + draw
 *   lightmap         full 16x16 lightmap respecify per frame + draw
 *   scene_mix        composite frame built from the knobs below
 *
 * Output: one CSV line per case:
 *   case,frames,ops_per_frame,median_frame_ms,ns_per_op,fps
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- EGL constants ---- */
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_FALSE 0
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_DEPTH_SIZE 0x3025
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NONE 0x3038
#define EGL_OPENGL_API 0x30A2
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001

/* ---- GL constants ---- */
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_INT 0x1405
#define GL_SHORT 0x1402
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_DEPTH_TEST 0x0B71
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_NO_ERROR 0
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_REPEAT 0x2901

typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLenum;
typedef char GLchar;
typedef unsigned char GLboolean;
typedef long GLsizeiptr;
typedef long GLintptr;

/* ---- resolved entry points ---- */
static void* (*g_eglGetProcAddress)(const char*);
static void* g_provider;

#define GLF(ret, name, args) static ret(*name) args;
GLF(void, glClear, (unsigned))
GLF(void, glClearColor, (float, float, float, float))
GLF(void, glEnable, (GLenum))
GLF(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
GLF(const unsigned char*, glGetString, (GLenum))
GLF(GLenum, glGetError, (void))
GLF(void, glFinish, (void))
GLF(void, glFlush, (void))
GLF(void, glGenBuffers, (GLsizei, GLuint*))
GLF(void, glBindBuffer, (GLenum, GLuint))
GLF(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))
GLF(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))
GLF(void, glGenVertexArrays, (GLsizei, GLuint*))
GLF(void, glBindVertexArray, (GLuint))
GLF(void, glEnableVertexAttribArray, (GLuint))
GLF(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
GLF(void, glGenTextures, (GLsizei, GLuint*))
GLF(void, glBindTexture, (GLenum, GLuint))
GLF(void, glActiveTexture, (GLenum))
GLF(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))
GLF(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))
GLF(void, glTexParameteri, (GLenum, GLenum, GLint))
GLF(void, glPixelStorei, (GLenum, GLint))
GLF(void, glGetIntegerv, (GLenum, GLint*))
GLF(void, glGenerateMipmap, (GLenum))
GLF(GLuint, glCreateShader, (GLenum))
GLF(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
GLF(void, glCompileShader, (GLuint))
GLF(void, glGetShaderiv, (GLuint, GLenum, GLint*))
GLF(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLF(GLuint, glCreateProgram, (void))
GLF(void, glAttachShader, (GLuint, GLuint))
GLF(void, glLinkProgram, (GLuint))
GLF(void, glGetProgramiv, (GLuint, GLenum, GLint*))
GLF(void, glUseProgram, (GLuint))
GLF(GLint, glGetUniformLocation, (GLuint, const GLchar*))
GLF(void, glUniform1i, (GLint, GLint))
GLF(void, glUniform3f, (GLint, float, float, float))
GLF(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const float*))
GLF(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*))
GLF(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))
GLF(void, glUniform3fv, (GLint, GLsizei, const float*))
GLF(void, glDrawArrays, (GLenum, GLint, GLsizei))
GLF(void, glDrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void*, GLint))
GLF(void, glMultiDrawElementsBaseVertex,
    (GLenum, const GLsizei*, GLenum, const void* const*, GLsizei, const GLint*))
GLF(void, glBindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))
GLF(void, glBindBufferBase, (GLenum, GLuint, GLuint))
GLF(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar*))
GLF(void, glUniformBlockBinding, (GLuint, GLuint, GLuint))
GLF(void, glGenSamplers, (GLsizei, GLuint*))
GLF(void, glBindSampler, (GLuint, GLuint))
GLF(void, glSamplerParameteri, (GLuint, GLenum, GLint))
GLF(void, glGenFramebuffers, (GLsizei, GLuint*))
GLF(void, glBindFramebuffer, (GLenum, GLuint))
GLF(void, glGenRenderbuffers, (GLsizei, GLuint*))
GLF(void, glBindRenderbuffer, (GLenum, GLuint))
GLF(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
GLF(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
GLF(GLenum, glCheckFramebufferStatus, (GLenum))
GLF(void*, glFenceSync, (GLenum, unsigned))
GLF(GLenum, glClientWaitSync, (void*, unsigned, unsigned long long))
GLF(void, glDeleteSync, (void*))

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}


/* Scene, cases and the case table live next door so the Android plugin's
 * in-process benchmark runs byte-identical bodies. */
static void bench_gl_failed(const char* what, const char* detail) {
    fprintf(stderr, "FAIL: %s %s\n", what, detail ? detail : "");
    exit(1);
}

/* GLES has glDrawElementsBaseVertex (3.2 core) but no multi-draw form of it, so
 * against a native mobile driver the multi-draw case issues the same sub-draws
 * one at a time - which is what the extension folds up, and what an application
 * without it would have to write. Desktop GL and MobileGL take the real call. */
static void bench_multi_draw_elements_base_vertex(GLenum mode, const GLsizei* counts, GLenum type,
                                                  const void* const* offsets, GLsizei drawCount,
                                                  const GLint* baseVertices) {
    if (glMultiDrawElementsBaseVertex) {
        glMultiDrawElementsBaseVertex(mode, counts, type, offsets, drawCount, baseVertices);
        return;
    }
    for (GLsizei i = 0; i < drawCount; ++i) {
        glDrawElementsBaseVertex(mode, counts[i], type, offsets[i], baseVertices[i]);
    }
}

#include "DriverBenchCases.inc"

/* ---- bench driver: fence-paced frames on the offscreen FBO ----------------
 * Frames are closed with a real fence wait, not glFinish: MobileGL implements
 * glFinish and glFlush as no-ops (MG_Impl/GLImpl/Exporting/Definitions.cpp),
 * so a glFinish-paced loop would time only the CPU-side submit on a MobileGL
 * backend while timing submit-plus-GPU on the native driver - the two numbers
 * would not describe the same work. A sync object is honoured by every stack
 * measured here.
 */
typedef void (*case_fn)(int frame, long a, long b);
static int g_warmup = 30, g_frames = 120;

static void end_frame_wait(void) {
    if (glFenceSync && glClientWaitSync && glDeleteSync) {
        void* sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (sync) {
            glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync(sync);
            return;
        }
    }
    glFinish();
}

static void run_case(const char* name, case_fn body, long a, long b, long opsPerFrame) {
    static uint64_t samples[4096];
    if (g_frames > 4096) g_frames = 4096;
    end_frame_wait();
    for (int i = 0; i < g_warmup; ++i) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        body(i, a, b);
        end_frame_wait();
    }
    for (int i = 0; i < g_frames; ++i) {
        uint64_t t0 = now_ns();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        body(i, a, b);
        end_frame_wait();
        samples[i] = now_ns() - t0;
    }
    qsort(samples, g_frames, sizeof(uint64_t), cmp_u64);
    uint64_t med = samples[g_frames / 2];
    double frameMs = med / 1e6;
    double nsPerOp = opsPerFrame > 0 ? (double)med / (double)opsPerFrame : 0.0;
    printf("%s,%d,%ld,%.3f,%.1f,%.1f\n", name, g_frames, opsPerFrame, frameMs, nsPerOp,
           1e9 / (double)med);
    fflush(stdout);
    if (glGetError() != GL_NO_ERROR) fprintf(stderr, "WARN: GL error after %s\n", name);
}

/* a = draws per frame */
/* ---- EGL bootstrap: one provider library, pbuffer, desktop-GL context ---- */
static int boot_egl(void) {
    const char* libpath = getenv("DRIVERBENCH_EGL_LIB");
    if (!libpath) libpath = "libEGL.so.1";
    g_provider = dlopen(libpath, RTLD_LAZY | RTLD_LOCAL);
    if (!g_provider) {
        fprintf(stderr, "FAIL: dlopen %s: %s\n", libpath, dlerror());
        return 1;
    }
#define ESYM(name) \
    void* p_##name = dlsym(g_provider, #name); \
    if (!p_##name) { fprintf(stderr, "FAIL: dlsym %s\n", #name); return 1; }
    ESYM(eglGetDisplay)
    ESYM(eglInitialize)
    ESYM(eglChooseConfig)
    ESYM(eglBindAPI)
    ESYM(eglCreateContext)
    ESYM(eglCreatePbufferSurface)
    ESYM(eglMakeCurrent)
    ESYM(eglGetProcAddress)
    ESYM(eglGetError)
    g_eglGetProcAddress = (void* (*)(const char*))p_eglGetProcAddress;

    EGLDisplay dpy = ((EGLDisplay(*)(void*))p_eglGetDisplay)(EGL_DEFAULT_DISPLAY);
    if (!dpy) { fprintf(stderr, "FAIL: eglGetDisplay\n"); return 1; }
    EGLint maj = 0, min = 0;
    if (!((EGLBoolean(*)(EGLDisplay, EGLint*, EGLint*))p_eglInitialize)(dpy, &maj, &min)) {
        fprintf(stderr, "FAIL: eglInitialize (0x%x)\n", ((EGLint(*)(void))p_eglGetError)());
        return 1;
    }
    fprintf(stderr, "EGL %d.%d via %s\n", maj, min, libpath);

    // Desktop GL first (that is what MobileGL exposes and what the cases are
    // written against), GLES 3 second so the same binary can measure a device's
    // native driver as the baseline. The .inc picks ESSL shader sources when the
    // context turns out to be ES.
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) =
        (EGLBoolean(*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))p_eglChooseConfig;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) =
        (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))p_eglCreateContext;
    EGLBoolean (*bindApi)(EGLenum) = (EGLBoolean(*)(EGLenum))p_eglBindAPI;

    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    EGLContext ctx = EGL_NO_CONTEXT;

    if (bindApi(EGL_OPENGL_API)) {
        const EGLint cfgAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                                     EGL_DEPTH_SIZE, 24, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        if (chooseConfig(dpy, cfgAttribs, &cfg, 1, &ncfg) && ncfg >= 1) {
            const EGLint ctxAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                                         EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                         EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
            ctx = createContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
            if (ctx == EGL_NO_CONTEXT) ctx = createContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
        }
    }
    if (ctx == EGL_NO_CONTEXT) {
        if (!bindApi(EGL_OPENGL_ES_API)) {
            fprintf(stderr, "FAIL: neither OpenGL nor OpenGL ES is bindable on this provider\n");
            return 1;
        }
        const EGLint esCfgAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                                       EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
        ncfg = 0;
        if (!chooseConfig(dpy, esCfgAttribs, &cfg, 1, &ncfg) || ncfg < 1) {
            const EGLint relaxed[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_NONE};
            if (!chooseConfig(dpy, relaxed, &cfg, 1, &ncfg) || ncfg < 1) {
                fprintf(stderr, "FAIL: eglChooseConfig\n");
                return 1;
            }
        }
        const EGLint esCtxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        ctx = createContext(dpy, cfg, EGL_NO_CONTEXT, esCtxAttribs);
    }
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "FAIL: eglCreateContext (0x%x)\n", ((EGLint(*)(void))p_eglGetError)());
        return 1;
    }

    const EGLint pbAttribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surf = ((EGLSurface(*)(EGLDisplay, EGLConfig, const EGLint*))p_eglCreatePbufferSurface)(
        dpy, cfg, pbAttribs);
    if (surf == EGL_NO_SURFACE) {
        fprintf(stderr, "FAIL: eglCreatePbufferSurface (0x%x)\n", ((EGLint(*)(void))p_eglGetError)());
        return 1;
    }
    if (!((EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))p_eglMakeCurrent)(dpy, surf,
                                                                                          surf, ctx)) {
        fprintf(stderr, "FAIL: eglMakeCurrent (0x%x)\n", ((EGLint(*)(void))p_eglGetError)());
        return 1;
    }

    /* Core GL entry points: eglGetProcAddress first (EGL 1.5 serves core
     * functions), provider dlsym as fallback (both glvnd and MobileGL export
     * the gl* symbols directly). */
#define RESOLVE(name)                                                              \
    do {                                                                           \
        *(void**)&name = g_eglGetProcAddress(#name);                               \
        if (!name) *(void**)&name = dlsym(g_provider, #name);                      \
        if (!name) { fprintf(stderr, "FAIL: resolve %s\n", #name); return 1; }     \
    } while (0)
    RESOLVE(glClear); RESOLVE(glClearColor); RESOLVE(glEnable); RESOLVE(glViewport);
    RESOLVE(glGetString); RESOLVE(glGetError); RESOLVE(glFinish); RESOLVE(glFlush);
    RESOLVE(glGenBuffers); RESOLVE(glBindBuffer); RESOLVE(glBufferData); RESOLVE(glBufferSubData);
    RESOLVE(glGenVertexArrays); RESOLVE(glBindVertexArray); RESOLVE(glEnableVertexAttribArray);
    RESOLVE(glVertexAttribPointer); RESOLVE(glGenTextures); RESOLVE(glBindTexture);
    RESOLVE(glActiveTexture); RESOLVE(glTexImage2D); RESOLVE(glTexSubImage2D);
    RESOLVE(glTexParameteri); RESOLVE(glGenerateMipmap); RESOLVE(glCreateShader);
    RESOLVE(glPixelStorei); RESOLVE(glGetIntegerv);
    RESOLVE(glShaderSource); RESOLVE(glCompileShader); RESOLVE(glGetShaderiv);
    RESOLVE(glGetShaderInfoLog); RESOLVE(glCreateProgram); RESOLVE(glAttachShader);
    RESOLVE(glLinkProgram); RESOLVE(glGetProgramiv); RESOLVE(glUseProgram);
    RESOLVE(glGetUniformLocation); RESOLVE(glUniform1i); RESOLVE(glUniform3f);
    RESOLVE(glUniformMatrix4fv); RESOLVE(glDrawElements); RESOLVE(glBindAttribLocation);
    RESOLVE(glUniform3fv); RESOLVE(glDrawArrays); RESOLVE(glDrawElementsBaseVertex);
    RESOLVE(glBindBufferRange); RESOLVE(glBindBufferBase);
    RESOLVE(glGetUniformBlockIndex); RESOLVE(glUniformBlockBinding);
    RESOLVE(glGenSamplers); RESOLVE(glBindSampler); RESOLVE(glSamplerParameteri);
    RESOLVE(glGenFramebuffers); RESOLVE(glBindFramebuffer); RESOLVE(glGenRenderbuffers);
    RESOLVE(glBindRenderbuffer); RESOLVE(glRenderbufferStorage); RESOLVE(glFramebufferRenderbuffer);
    RESOLVE(glCheckFramebufferStatus);
    // Optional: end_frame_wait() falls back to glFinish when a stack has no
    // sync objects, so resolve without failing the run.
    *(void**)&glFenceSync = g_eglGetProcAddress("glFenceSync");
    if (!glFenceSync) *(void**)&glFenceSync = dlsym(g_provider, "glFenceSync");
    *(void**)&glClientWaitSync = g_eglGetProcAddress("glClientWaitSync");
    if (!glClientWaitSync) *(void**)&glClientWaitSync = dlsym(g_provider, "glClientWaitSync");
    *(void**)&glDeleteSync = g_eglGetProcAddress("glDeleteSync");
    if (!glDeleteSync) *(void**)&glDeleteSync = dlsym(g_provider, "glDeleteSync");
    // Desktop-only: GLES 3.2 has DrawElementsBaseVertex but no multi-draw form,
    // so bench_multi_draw_elements_base_vertex() emulates it when this is null.
    *(void**)&glMultiDrawElementsBaseVertex = g_eglGetProcAddress("glMultiDrawElementsBaseVertex");
    if (!glMultiDrawElementsBaseVertex)
        *(void**)&glMultiDrawElementsBaseVertex = dlsym(g_provider, "glMultiDrawElementsBaseVertex");

    fprintf(stderr, "renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "version:  %s\n", glGetString(GL_VERSION));
    return 0;
}

int main(int argc, char** argv) {
    long draws = 2048;
    if (getenv("DRIVERBENCH_DRAWS")) draws = atol(getenv("DRIVERBENCH_DRAWS"));
    if (getenv("DRIVERBENCH_FRAMES")) g_frames = atoi(getenv("DRIVERBENCH_FRAMES"));
    if (getenv("DRIVERBENCH_SPRITES")) g_mixSprites = atol(getenv("DRIVERBENCH_SPRITES"));

    if (boot_egl()) return 1;
    build_resources();

    printf("case,frames,ops_per_frame,median_frame_ms,ns_per_op,fps\n");
    for (int i = 0; i < kBenchCaseCount; ++i) {
        const BenchCaseDesc* c = &kBenchCases[i];
        if (argc > 1) {
            int wanted = 0;
            for (int j = 1; j < argc; ++j)
                if (strcmp(argv[j], c->name) == 0) wanted = 1;
            if (!wanted) continue;
        }
        // The generic cases scale with DRIVERBENCH_DRAWS; the mc_* rates are
        // measured and must not move, or the numbers stop being comparable.
        long a = c->a, ops = c->opsPerFrame;
        if (strncmp(c->name, "mc_", 3) != 0 && a > 100) {
            a = draws * a / 2048;
            ops = c->opsPerFrame * draws / 2048;
        }
        run_case(c->name, c->fn, a, c->b, ops);
    }
    return 0;
}
