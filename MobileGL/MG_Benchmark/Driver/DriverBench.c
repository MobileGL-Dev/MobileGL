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
#define EGL_DEPTH_SIZE 0x3025
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NONE 0x3038
#define EGL_OPENGL_API 0x30A2
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

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

/* ---- shared scene resources (Minecraft-shaped) ---- */
#define MAX_SECTIONS 512
static GLuint g_progChunk, g_progEntity;
static GLint g_uOffsetChunk, g_uMvpChunk, g_uMvpEntity;
static GLuint g_vao[MAX_SECTIONS], g_vbo[MAX_SECTIONS];
static GLuint g_sharedIbo;
static GLuint g_texAtlas, g_texLight, g_texEntity;
static int g_quadsPerSection = 128; /* 128 quads = 512 verts, 768 indices */
static unsigned char* g_scratch;
/* Uniform ring + sampler for the 26.2-shaped cases (see the case block below). */
static GLuint g_uboRing;
static GLint g_uboAlign = 256;
static size_t g_uboSlot = 256;
static GLuint g_sampler;
static float g_mvp[16] = {0.002f, 0, 0, 0, 0, 0.002f, 0, 0, 0, 0, -0.001f, 0, -1.f, -1.f, 0.f, 1.f};

/* Minecraft chunk vertex: pos 3f, color 4ub, uv 2f, packed light 2s -> 32 B */
#define VERT_STRIDE 32
static void fill_section_vertices(unsigned char* dst, int quads, unsigned seed) {
    for (int q = 0; q < quads * 4; ++q) {
        float* f = (float*)(dst + q * VERT_STRIDE);
        unsigned r = seed = seed * 1664525u + 1013904223u;
        f[0] = (float)(q & 31) * 8.0f + (float)(r & 7);
        f[1] = (float)((q >> 5) & 31) * 8.0f;
        f[2] = (float)(q % 7) * 0.1f;
        dst[q * VERT_STRIDE + 12] = (unsigned char)r;
        dst[q * VERT_STRIDE + 13] = (unsigned char)(r >> 8);
        dst[q * VERT_STRIDE + 14] = (unsigned char)(r >> 16);
        dst[q * VERT_STRIDE + 15] = 255;
        f[4] = (float)(r & 1023) / 1024.0f;
        f[5] = (float)((r >> 10) & 511) / 512.0f;
        ((short*)(dst + q * VERT_STRIDE + 24))[0] = 15 << 4;
        ((short*)(dst + q * VERT_STRIDE + 24))[1] = 15 << 4;
    }
}

static GLuint make_shader(GLenum kind, const char* src) {
    GLuint sh = glCreateShader(kind);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "FAIL: shader compile: %s\n", log);
        exit(1);
    }
    return sh;
}

static GLuint make_program(const char* vs_src, const char* fs_src) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, make_shader(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, make_shader(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aColor");
    glBindAttribLocation(prog, 2, "aUv");
    glBindAttribLocation(prog, 3, "aLight");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "FAIL: program link\n");
        exit(1);
    }
    return prog;
}

static const char* kChunkVs =
    "#version 150 core\n"
    "in vec3 aPos; in vec4 aColor; in vec2 aUv; in vec2 aLight;\n"
    "uniform mat4 uMvp; uniform vec3 uOffset;\n"
    "out vec4 vColor; out vec2 vUv; out vec2 vLight;\n"
    "void main(){ gl_Position = uMvp * vec4(aPos + uOffset, 1.0);\n"
    "  vColor = aColor; vUv = aUv; vLight = aLight * (1.0/256.0); }\n";
static const char* kChunkFs =
    "#version 150 core\n"
    "in vec4 vColor; in vec2 vUv; in vec2 vLight; out vec4 o;\n"
    "uniform sampler2D uAtlas; uniform sampler2D uLight;\n"
    "void main(){ o = texture(uAtlas, vUv) * vColor * texture(uLight, vLight); }\n";
static const char* kEntityVs =
    "#version 150 core\n"
    "in vec3 aPos; in vec4 aColor; in vec2 aUv; in vec2 aLight;\n"
    "uniform mat4 uMvp; uniform mat4 uModel;\n"
    "out vec4 vColor; out vec2 vUv;\n"
    "void main(){ gl_Position = uMvp * uModel * vec4(aPos, 1.0); vColor = aColor; vUv = aUv; }\n";
static const char* kEntityFs =
    "#version 150 core\n"
    "in vec4 vColor; in vec2 vUv; out vec4 o; uniform sampler2D uTex;\n"
    "void main(){ o = texture(uTex, vUv) * vColor; }\n";

static void setup_vao(GLuint vao, GLuint vbo, GLuint ibo) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, VERT_STRIDE, (void*)0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 1, VERT_STRIDE, (void*)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, 0, VERT_STRIDE, (void*)16);
    glVertexAttribPointer(3, 2, GL_SHORT, 0, VERT_STRIDE, (void*)24);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
}

static void build_resources(void) {
    /* offscreen render target: 1280x720 RBO FBO, like CTS fbo surface mode */
    GLuint fbo, rboColor, rboDepth;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rboColor);
    glBindRenderbuffer(GL_RENDERBUFFER, rboColor);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1280, 720);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboColor);
    glGenRenderbuffers(1, &rboDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, rboDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 1280, 720);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboDepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FAIL: FBO incomplete\n");
        exit(1);
    }

    g_progChunk = make_program(kChunkVs, kChunkFs);
    g_progEntity = make_program(kEntityVs, kEntityFs);
    glUseProgram(g_progChunk);
    g_uMvpChunk = glGetUniformLocation(g_progChunk, "uMvp");
    g_uOffsetChunk = glGetUniformLocation(g_progChunk, "uOffset");
    glUniform1i(glGetUniformLocation(g_progChunk, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(g_progChunk, "uLight"), 2);
    glUniformMatrix4fv(g_uMvpChunk, 1, 0, g_mvp);
    glUseProgram(g_progEntity);
    g_uMvpEntity = glGetUniformLocation(g_progEntity, "uMvp");
    glUniform1i(glGetUniformLocation(g_progEntity, "uTex"), 0);
    glUniformMatrix4fv(g_uMvpEntity, 1, 0, g_mvp);
    glUseProgram(g_progChunk);

    /* shared quad index buffer, like Blaze3D's RenderSystem shared sequences */
    int maxQuads = 4096;
    unsigned* idx = malloc((size_t)maxQuads * 6 * 4);
    for (int q = 0; q < maxQuads; ++q) {
        unsigned base = q * 4;
        unsigned* p = idx + q * 6;
        p[0] = base; p[1] = base + 1; p[2] = base + 2;
        p[3] = base + 2; p[4] = base + 3; p[5] = base;
    }
    glGenBuffers(1, &g_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_sharedIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, maxQuads * 6 * 4, idx, GL_STATIC_DRAW);
    free(idx);

    g_scratch = malloc(4 * 1024 * 1024);
    memset(g_scratch, 0x5a, 4 * 1024 * 1024);

    glGenVertexArrays(MAX_SECTIONS, g_vao);
    glGenBuffers(MAX_SECTIONS, g_vbo);
    int bytes = g_quadsPerSection * 4 * VERT_STRIDE;
    for (int i = 0; i < MAX_SECTIONS; ++i) {
        fill_section_vertices(g_scratch, g_quadsPerSection, i * 7919u + 1);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, bytes, g_scratch, GL_STATIC_DRAW);
        setup_vao(g_vao[i], g_vbo[i], g_sharedIbo);
    }

    glGenTextures(1, &g_texAtlas);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texAtlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_scratch);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenerateMipmap(GL_TEXTURE_2D);

    glGenTextures(1, &g_texLight);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, g_texLight);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_scratch);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &g_texEntity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texEntity);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_scratch);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, g_texAtlas);

    // Uniform ring the 26.2-style case sub-ranges into, sized like a real
    // frame's worth of per-draw uniform slots.
    GLint align = 256;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    g_uboAlign = align > 0 ? align : 256;
    g_uboSlot = (size_t)g_uboAlign;
    glGenBuffers(1, &g_uboRing);
    glBindBuffer(GL_UNIFORM_BUFFER, g_uboRing);
    glBufferData(GL_UNIFORM_BUFFER, 4 * 1024 * 1024, g_scratch, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glGenSamplers(1, &g_sampler);
    glSamplerParameteri(g_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(g_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.3f, 0.5f, 0.9f, 1.0f);
    glViewport(0, 0, 1280, 720);
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "FAIL: GL error during resource setup\n");
        exit(1);
    }
}

/* ---- bench driver: glFinish-paced frames on the offscreen FBO ---- */
typedef void (*case_fn)(int frame, long a, long b);
static int g_warmup = 30, g_frames = 120;

static void run_case(const char* name, case_fn body, long a, long b, long opsPerFrame) {
    static uint64_t samples[4096];
    if (g_frames > 4096) g_frames = 4096;
    glFinish();
    for (int i = 0; i < g_warmup; ++i) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        body(i, a, b);
        glFinish();
    }
    for (int i = 0; i < g_frames; ++i) {
        uint64_t t0 = now_ns();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        body(i, a, b);
        glFinish();
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
static void case_draw_tiny(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
}

static void case_draw_uniform(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) {
        glUniform3f(g_uOffsetChunk, (float)(i & 15), (float)((i >> 4) & 15), 0.0f);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
}

static void case_draw_multi_vao(int frame, long a, long b) {
    (void)frame; (void)b;
    for (long i = 0; i < a; ++i) {
        glBindVertexArray(g_vao[i % MAX_SECTIONS]);
        glUniform3f(g_uOffsetChunk, (float)(i & 15), (float)((i >> 4) & 15), 0.0f);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
}

static void case_tex_pingpong(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) {
        glBindTexture(GL_TEXTURE_2D, (i & 1) ? g_texEntity : g_texAtlas);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
    glBindTexture(GL_TEXTURE_2D, g_texAtlas);
}

static void case_program_pingpong(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) {
        if (i & 1) {
            glUseProgram(g_progEntity);
            glUniformMatrix4fv(g_uMvpEntity, 1, 0, g_mvp);
        } else {
            glUseProgram(g_progChunk);
            glUniform3f(g_uOffsetChunk, (float)(i & 15), 0.0f, 0.0f);
        }
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
    glUseProgram(g_progChunk);
}

/* a = uploads per frame, b = bytes per upload (0 => section size) */
static void case_chunk_upload(int frame, long a, long b) {
    if (b <= 0) b = g_quadsPerSection * 4 * VERT_STRIDE;
    if (b > 4 * 1024 * 1024) b = 4 * 1024 * 1024;
    for (long i = 0; i < a; ++i) {
        int slot = (int)(((long)frame * a + i) % MAX_SECTIONS);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo[slot]);
        glBufferData(GL_ARRAY_BUFFER, b, NULL, GL_STATIC_DRAW); /* orphan */
        glBufferSubData(GL_ARRAY_BUFFER, 0, b, g_scratch);
        glBindVertexArray(g_vao[slot]);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
}

/* a = sprite updates per frame */
static void case_atlas_sprite(int frame, long a, long b) {
    (void)b;
    glBindVertexArray(g_vao[0]);
    glBindTexture(GL_TEXTURE_2D, g_texAtlas);
    for (long i = 0; i < a; ++i) {
        int x = (int)((frame * 13 + i * 17) % (1024 - 16));
        int y = (int)((frame * 7 + i * 29) % (512 - 16));
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, g_scratch);
    }
    glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
}

/* a = lightmap updates (+draw) per frame */
static void case_lightmap(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) {
        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, g_texLight);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, g_scratch);
        glActiveTexture(GL_TEXTURE0);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
}

/* Composite: a = total draws, b = uploads per frame. Mix modeled on trace
 * analysis: chunk draws with per-draw offset uniform across sections, 10%
 * entity-style program flips, per-frame lightmap + sprite updates, b chunk
 * re-uploads. */
static long g_mixSprites = 8;
static void case_scene_mix(int frame, long a, long b) {
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, g_texLight);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, g_scratch);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texAtlas);
    for (long i = 0; i < g_mixSprites; ++i) {
        int x = (int)((frame * 13 + i * 17) % (1024 - 16));
        int y = (int)((frame * 7 + i * 29) % (512 - 16));
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, g_scratch);
    }
    for (long i = 0; i < b; ++i) {
        int slot = (int)(((long)frame * b + i) % MAX_SECTIONS);
        long bytes = g_quadsPerSection * 4 * VERT_STRIDE;
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo[slot]);
        glBufferData(GL_ARRAY_BUFFER, bytes, NULL, GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, g_scratch);
    }
    long entityEvery = 10;
    for (long i = 0; i < a; ++i) {
        if (i % entityEvery == entityEvery - 1) {
            glUseProgram(g_progEntity);
            glUniformMatrix4fv(g_uMvpEntity, 1, 0, g_mvp);
            glBindTexture(GL_TEXTURE_2D, g_texEntity);
            glBindVertexArray(g_vao[i % MAX_SECTIONS]);
            glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
            glUseProgram(g_progChunk);
            glBindTexture(GL_TEXTURE_2D, g_texAtlas);
        } else {
            glBindVertexArray(g_vao[i % MAX_SECTIONS]);
            glUniform3f(g_uOffsetChunk, (float)(i & 15), (float)((i >> 4) & 15), 0.0f);
            glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
        }
    }
}

/* ---- Trace-derived cases -------------------------------------------------
 * Per-frame call mixes measured from the three captured Minecraft traces
 * (render distance 32, 1280x720, hovering in-world). Each case reproduces one
 * renderer's dominant per-draw sequence at its measured rate, so the number a
 * backend posts here is directly comparable to what that game version asks of
 * the driver every frame.
 *
 *   vanilla 1.21.1 : 5495 glDrawElements, 5490 glBindVertexArray,
 *                    5487 glUniform3fv, 95 glTexSubImage2D (+382 glPixelStorei,
 *                    247 glTexParameteri), 23 glBufferData per frame
 *   fabric+sodium  : 132 glMultiDrawElementsBaseVertex, 279 glBindVertexArray,
 *                    132 glUniform3f, 32 glBufferData per frame
 *   26.2 snapshot  : 3401 glDrawElementsBaseVertex, each preceded by
 *                    glBindBufferRange + glBindBuffer (3639/3412 per frame)
 */
/* vanilla: bind VAO, push the chunk offset, draw. a = draws per frame. */
static void case_mc_vanilla_draw(int frame, long a, long b) {
    (void)frame; (void)b;
    float offset[3];
    for (long i = 0; i < a; ++i) {
        glBindVertexArray(g_vao[i % MAX_SECTIONS]);
        offset[0] = (float)(i & 15);
        offset[1] = (float)((i >> 4) & 15);
        offset[2] = 0.0f;
        glUniform3fv(g_uOffsetChunk, 1, offset);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
}

/* sodium: one multi-draw covers many chunk sections out of a shared buffer.
 * a = multi-draws per frame, b = sub-draws inside each. */
static void case_mc_sodium_multidraw(int frame, long a, long b) {
    (void)frame;
    enum { kMaxSub = 64 };
    if (b <= 0 || b > kMaxSub) b = 32;
    GLsizei counts[kMaxSub];
    const void* offsets[kMaxSub];
    GLint baseVertices[kMaxSub];
    for (long s = 0; s < b; ++s) {
        counts[s] = (GLsizei)(g_quadsPerSection * 6 / b);
        offsets[s] = (const void*)(uintptr_t)(s * (g_quadsPerSection * 6 / b) * 4);
        baseVertices[s] = 0;
    }
    for (long i = 0; i < a; ++i) {
        glBindVertexArray(g_vao[i % MAX_SECTIONS]);
        glBindVertexArray(g_vao[i % MAX_SECTIONS]); /* sodium rebinds ~2x per draw */
        glUniform3f(g_uOffsetChunk, (float)(i & 15), (float)((i >> 4) & 15), 0.0f);
        glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_INT, offsets,
                                      (GLsizei)b, baseVertices);
    }
}

/* 26.2: every draw rebinds a fresh uniform-buffer range out of a ring.
 * a = draws per frame. */
static void case_mc_ubo_range(int frame, long a, long b) {
    (void)b;
    const size_t slots = (4u * 1024u * 1024u) / g_uboSlot;
    for (long i = 0; i < a; ++i) {
        const size_t slot = (size_t)(((long)frame * a + i) % (long)slots);
        glBindBufferRange(GL_UNIFORM_BUFFER, 0, g_uboRing, (GLintptr)(slot * g_uboSlot),
                          (GLsizeiptr)g_uboSlot);
        glBindBuffer(GL_UNIFORM_BUFFER, g_uboRing);
        glDrawElementsBaseVertex(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0, 0);
    }
}

/* vanilla's animated-sprite path: every upload is wrapped in the pixel-store
 * and filter state Blaze3D re-sets around it. a = uploads per frame. */
static void case_mc_tex_stream(int frame, long a, long b) {
    (void)b;
    glBindVertexArray(g_vao[0]);
    glBindTexture(GL_TEXTURE_2D, g_texAtlas);
    for (long i = 0; i < a; ++i) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        int x = (int)((frame * 13 + i * 17) % (1024 - 16));
        int y = (int)((frame * 7 + i * 29) % (512 - 16));
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, g_scratch);
    }
    glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
}

/* Blaze3D re-resolves uniform locations by name every frame. a = lookups. */
static void case_mc_uniform_lookup(int frame, long a, long b) {
    (void)frame; (void)b;
    static const char* names[4] = {"uMvp", "uOffset", "uAtlas", "uLight"};
    volatile GLint sink = 0;
    for (long i = 0; i < a; ++i) sink += glGetUniformLocation(g_progChunk, names[i & 3]);
    (void)sink;
    glBindVertexArray(g_vao[0]);
    glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
}

/* 26.2 rebinds a sampler object per texture unit switch. a = switches. */
static void case_mc_sampler_churn(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)(i & 3));
        glBindTexture(GL_TEXTURE_2D, (i & 1) ? g_texEntity : g_texAtlas);
        glBindSampler((GLuint)(i & 3), g_sampler);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
    glActiveTexture(GL_TEXTURE0);
}

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

    ((EGLBoolean(*)(EGLenum))p_eglBindAPI)(EGL_OPENGL_API);

    const EGLint cfgAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                                 EGL_DEPTH_SIZE, 24, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    const EGLint cfgAttribsRelaxed[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_NONE};
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) =
        (EGLBoolean(*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))p_eglChooseConfig;
    if (!chooseConfig(dpy, cfgAttribs, &cfg, 1, &ncfg) || ncfg < 1) {
        if (!chooseConfig(dpy, cfgAttribsRelaxed, &cfg, 1, &ncfg) || ncfg < 1) {
            fprintf(stderr, "FAIL: eglChooseConfig\n");
            return 1;
        }
    }

    const EGLint ctxAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) =
        (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))p_eglCreateContext;
    EGLContext ctx = createContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) ctx = createContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
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
    RESOLVE(glMultiDrawElementsBaseVertex); RESOLVE(glBindBufferRange); RESOLVE(glBindBufferBase);
    RESOLVE(glGetUniformBlockIndex); RESOLVE(glUniformBlockBinding);
    RESOLVE(glGenSamplers); RESOLVE(glBindSampler); RESOLVE(glSamplerParameteri);
    RESOLVE(glGenFramebuffers); RESOLVE(glBindFramebuffer); RESOLVE(glGenRenderbuffers);
    RESOLVE(glBindRenderbuffer); RESOLVE(glRenderbufferStorage); RESOLVE(glFramebufferRenderbuffer);
    RESOLVE(glCheckFramebufferStatus);

    fprintf(stderr, "renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "version:  %s\n", glGetString(GL_VERSION));
    return 0;
}

typedef struct {
    const char* name;
    case_fn fn;
    long a, b, opsPerFrame;
} BenchCase;

int main(int argc, char** argv) {
    long draws = 2048;
    if (getenv("DRIVERBENCH_DRAWS")) draws = atol(getenv("DRIVERBENCH_DRAWS"));
    if (getenv("DRIVERBENCH_FRAMES")) g_frames = atoi(getenv("DRIVERBENCH_FRAMES"));
    if (getenv("DRIVERBENCH_SPRITES")) g_mixSprites = atol(getenv("DRIVERBENCH_SPRITES"));

    if (boot_egl()) return 1;
    build_resources();

    // Rates are the measured per-frame call counts of each trace, so one
    // bench frame costs what one real frame of that game version costs.
    BenchCase cases[] = {
        {"mc_vanilla_draw", case_mc_vanilla_draw, 5495, 0, 5495},
        {"mc_sodium_multidraw", case_mc_sodium_multidraw, 132, 32, 132},
        {"mc_ubo_range", case_mc_ubo_range, 3401, 0, 3401},
        {"mc_tex_stream", case_mc_tex_stream, 95, 0, 95},
        {"mc_uniform_lookup", case_mc_uniform_lookup, 41, 0, 41},
        {"mc_sampler_churn", case_mc_sampler_churn, 306, 0, 306},
        {"draw_tiny", case_draw_tiny, draws, 0, draws},
        {"draw_uniform", case_draw_uniform, draws, 0, draws},
        {"draw_multi_vao", case_draw_multi_vao, draws, 0, draws},
        {"tex_pingpong", case_tex_pingpong, draws / 2, 0, draws / 2},
        {"program_pingpong", case_program_pingpong, draws / 4, 0, draws / 4},
        {"chunk_upload", case_chunk_upload, 24, 0, 24},
        {"atlas_sprite", case_atlas_sprite, 32, 0, 32},
        {"lightmap", case_lightmap, 4, 0, 4},
        {"scene_mix", case_scene_mix, draws, 12, draws},
    };
    int ncases = (int)(sizeof cases / sizeof cases[0]);

    printf("case,frames,ops_per_frame,median_frame_ms,ns_per_op,fps\n");
    for (int i = 0; i < ncases; ++i) {
        if (argc > 1) {
            int wanted = 0;
            for (int j = 1; j < argc; ++j)
                if (strcmp(argv[j], cases[i].name) == 0) wanted = 1;
            if (!wanted) continue;
        }
        run_case(cases[i].name, cases[i].fn, cases[i].a, cases[i].b, cases[i].opsPerFrame);
    }
    return 0;
}
