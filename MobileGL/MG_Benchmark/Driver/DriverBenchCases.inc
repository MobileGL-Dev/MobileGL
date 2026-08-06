/* MobileGL - MobileGL/MG_Benchmark/Driver/DriverBenchCases.inc
 * Copyright (c) 2025-2026 MobileGL-Dev
 * Licensed under the GNU Lesser General Public License v3.0:
 *   https://www.gnu.org/licenses/gpl-3.0.txt
 *   https://www.gnu.org/licenses/lgpl-3.0.txt
 * SPDX-License-Identifier: LGPL-3.0-only
 * End of Source File Header
 *
 * The benchmark scene and its cases, with no harness and no GL loader: the
 * includer supplies both. DriverBench.c drives it through function pointers
 * resolved from one EGL provider; MG_Util/SelfTest/DriverBenchJni.cpp drives
 * it through MobileGL's own frontend entry points inside the Android plugin.
 * Sharing the bodies is the point - a number from the phone and a number from
 * the desktop have to describe the same work.
 *
 * The includer must have declared, before including this file: the GL types
 * and enums used below, and callable gl* entry points with the standard
 * signatures. bench_gl_failed() is called (and must be defined) when shader
 * compilation or linking fails, so a caller can report the failure instead of
 * dying inside a benchmark.
 */

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
/* Two small offscreen targets for the 26.2-style render-pass churn case. */
static GLuint g_passFbo[2];
static GLuint g_passColor[2];
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
        bench_gl_failed("shader compile", log);
        return 0;
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
        bench_gl_failed("program link", "");
        return 0;
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

// ESSL 3.20 twins of the four shaders above. The bodies are identical; only the
// version line and the precision qualifiers differ, so the two paths compile the
// same work. Needed because this bench also runs against a device's native GLES
// driver as the baseline MobileGL is measured against, and that driver rejects
// desktop GLSL - while MobileGL is fed desktop GLSL on purpose, since translating
// it is the thing under test.
static const char* kChunkVsEs =
    "#version 320 es\n"
    "precision highp float;\n"
    "in vec3 aPos; in vec4 aColor; in vec2 aUv; in vec2 aLight;\n"
    "uniform mat4 uMvp; uniform vec3 uOffset;\n"
    "out vec4 vColor; out vec2 vUv; out vec2 vLight;\n"
    "void main(){ gl_Position = uMvp * vec4(aPos + uOffset, 1.0);\n"
    "  vColor = aColor; vUv = aUv; vLight = aLight * (1.0/256.0); }\n";
static const char* kChunkFsEs =
    "#version 320 es\n"
    "precision mediump float;\n"
    "in vec4 vColor; in vec2 vUv; in vec2 vLight; out vec4 o;\n"
    "uniform sampler2D uAtlas; uniform sampler2D uLight;\n"
    "void main(){ o = texture(uAtlas, vUv) * vColor * texture(uLight, vLight); }\n";
static const char* kEntityVsEs =
    "#version 320 es\n"
    "precision highp float;\n"
    "in vec3 aPos; in vec4 aColor; in vec2 aUv; in vec2 aLight;\n"
    "uniform mat4 uMvp; uniform mat4 uModel;\n"
    "out vec4 vColor; out vec2 vUv;\n"
    "void main(){ gl_Position = uMvp * uModel * vec4(aPos, 1.0); vColor = aColor; vUv = aUv; }\n";
static const char* kEntityFsEs =
    "#version 320 es\n"
    "precision mediump float;\n"
    "in vec4 vColor; in vec2 vUv; out vec4 o; uniform sampler2D uTex;\n"
    "void main(){ o = texture(uTex, vUv) * vColor; }\n";

// True once build_resources() has seen a GL_VERSION beginning with "OpenGL ES".
static int g_isGlesContext = 0;

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

static GLuint g_mainFbo;

static void build_resources(void) {
    /* offscreen render target: 1280x720 RBO FBO, like CTS fbo surface mode */
    GLuint fbo, rboColor, rboDepth;
    glGenFramebuffers(1, &fbo);
    g_mainFbo = fbo;
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
        bench_gl_failed("FBO incomplete", "");
        return;
    }

    const char* versionString = (const char*)glGetString(GL_VERSION);
    g_isGlesContext = versionString != NULL && strncmp(versionString, "OpenGL ES", 9) == 0;
    g_progChunk = g_isGlesContext ? make_program(kChunkVsEs, kChunkFsEs) : make_program(kChunkVs, kChunkFs);
    g_progEntity = g_isGlesContext ? make_program(kEntityVsEs, kEntityFsEs) : make_program(kEntityVs, kEntityFs);
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
    unsigned* idx = (unsigned*)malloc((size_t)maxQuads * 6 * 4);
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

    g_scratch = (unsigned char*)malloc(4 * 1024 * 1024);
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

    for (int i = 0; i < 2; ++i) {
        glGenFramebuffers(1, &g_passFbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_passFbo[i]);
        glGenRenderbuffers(1, &g_passColor[i]);
        glBindRenderbuffer(GL_RENDERBUFFER, g_passColor[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 256, 256);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_passColor[i]);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            bench_gl_failed("pass FBO incomplete", "");
            return;
        }
    }
    /* back to the main offscreen target the harness set up */
    glBindFramebuffer(GL_FRAMEBUFFER, g_mainFbo);

    glGenSamplers(1, &g_sampler);
    glSamplerParameteri(g_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(g_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.3f, 0.5f, 0.9f, 1.0f);
    glViewport(0, 0, 1280, 720);
    const GLenum setupError = glGetError();
    if (setupError != GL_NO_ERROR) {
        char message[64];
        snprintf(message, sizeof message, "0x%04x", setupError);
        bench_gl_failed("GL error during resource setup", message);
    }
}

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
        // Routed through the includer: GLES has no multi-draw-with-base-vertex, so
        // a native-driver harness emulates it with the loop the extension folds up.
        bench_multi_draw_elements_base_vertex(GL_TRIANGLES, counts, GL_UNSIGNED_INT, offsets,
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


/* 26.2 switches render targets constantly: 132 glBindFramebuffer and 198
 * glDrawBuffers per frame. Pass switching is where a Vulkan backend pays for
 * render-pass breaks, so this case is the one to watch on Magma. a = passes. */
static void case_mc_pass_switch(int frame, long a, long b) {
    (void)frame; (void)b;
    static const GLenum kColor0[1] = {GL_COLOR_ATTACHMENT0};
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_passFbo[i & 1]);
        glDrawBuffers(1, kColor0);
        glViewport(0, 0, 256, 256);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_mainFbo);
    glViewport(0, 0, 1280, 720);
}

/* Blaze3D toggles blend around batches: 46 glEnable/glDisable pairs and 28
 * glBlendFuncSeparate per vanilla frame. a = toggle pairs. */
static void case_mc_state_toggle(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
        glDisable(GL_BLEND);
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
}

/* 26.2 re-sets texture parameters relentlessly - 612 glTexParameteri per frame,
 * almost always to the value already in place. Measures redundant-param
 * filtering. a = parameter writes. */
static void case_mc_tex_param(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    glBindTexture(GL_TEXTURE_2D, g_texAtlas);
    for (long i = 0; i < a; i += 4) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
}

/* Sodium switches programs mid-frame far more than vanilla: 62 glUseProgram and
 * 60 mat4 uploads per frame. a = program switches. */
static void case_mc_use_program(int frame, long a, long b) {
    (void)frame; (void)b;
    glBindVertexArray(g_vao[0]);
    for (long i = 0; i < a; ++i) {
        if (i & 1) {
            glUseProgram(g_progEntity);
            glUniformMatrix4fv(g_uMvpEntity, 1, 0, g_mvp);
        } else {
            glUseProgram(g_progChunk);
            glUniformMatrix4fv(g_uMvpChunk, 1, 0, g_mvp);
        }
        glDrawElements(GL_TRIANGLES, g_quadsPerSection * 6, GL_UNSIGNED_INT, 0);
    }
    glUseProgram(g_progChunk);
}

/* ---- the case table both harnesses iterate --------------------------------
 * a/b are the case's own knobs; opsPerFrame is what one bench frame is
 * normalised by, so ns_per_op compares across renderers. The mc_* rates are
 * the per-frame call counts measured from the captured traces.
 */
typedef void (*bench_case_fn)(int frame, long a, long b);

typedef struct {
    const char* name;
    bench_case_fn fn;
    long a, b, opsPerFrame;
} BenchCaseDesc;

static const BenchCaseDesc kBenchCases[] = {
    {"mc_vanilla_draw", case_mc_vanilla_draw, 5495, 0, 5495},
    {"mc_sodium_multidraw", case_mc_sodium_multidraw, 132, 32, 132},
    {"mc_ubo_range", case_mc_ubo_range, 3401, 0, 3401},
    {"mc_tex_stream", case_mc_tex_stream, 95, 0, 95},
    {"mc_uniform_lookup", case_mc_uniform_lookup, 41, 0, 41},
    {"mc_sampler_churn", case_mc_sampler_churn, 306, 0, 306},
    {"mc_pass_switch", case_mc_pass_switch, 132, 0, 132},
    {"mc_state_toggle", case_mc_state_toggle, 46, 0, 46},
    {"mc_tex_param", case_mc_tex_param, 612, 0, 612},
    {"mc_use_program", case_mc_use_program, 62, 0, 62},
    {"draw_tiny", case_draw_tiny, 2048, 0, 2048},
    {"draw_uniform", case_draw_uniform, 2048, 0, 2048},
    {"draw_multi_vao", case_draw_multi_vao, 2048, 0, 2048},
    {"tex_pingpong", case_tex_pingpong, 1024, 0, 1024},
    {"program_pingpong", case_program_pingpong, 512, 0, 512},
    {"chunk_upload", case_chunk_upload, 24, 0, 24},
    {"atlas_sprite", case_atlas_sprite, 32, 0, 32},
    {"lightmap", case_lightmap, 4, 0, 4},
    {"scene_mix", case_scene_mix, 2048, 12, 2048},
};
static const int kBenchCaseCount = (int)(sizeof kBenchCases / sizeof kBenchCases[0]);
