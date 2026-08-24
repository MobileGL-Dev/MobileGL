// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/GuiBatchScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A GUI QUAD DRAWN THE WAY AcceleratedRendering DRAWS ONE.
//
// The mod replaces every GUI blit with a compute pass: the application writes unit-space
// vertices into a persistently mapped SSBO, a compute shader multiplies them by a shared
// transform into a second buffer, and that second buffer is then bound as GL_ARRAY_BUFFER of a
// DSA vertex array and drawn with glDrawElementsBaseVertex through vanilla's position_tex_color
// program. Every element of that is replayed here, one axis per test, so a failure names the
// element that killed the quad rather than "the GUI is broken".
//
// The axis that mattered is MeshesBlockLeftUnbound. The mod's vertex-transform compute shader
// declares SIX storage blocks, and the last of them - `Meshes`, the cache of pre-uploaded model
// geometry - is read only when a vertex says it comes from a cached mesh. A batch of plain GUI
// blits has no cached meshes, so the mod binds nothing at that point and the shader never reads
// it. GL 4.6 core 7.8 is explicit that this is legal: a storage block with no buffer at its
// binding point simply has no store.
//
// DirectVulkan used to refuse the whole descriptor set over it, and both SetupDraw and
// DispatchCompute skip their work on that refusal - so the transform dispatch never ran, the
// output vertex buffer kept whatever was in it, and every hotbar and container-screen background
// quad came out degenerate. Items were unaffected because item geometry DOES come from cached
// meshes, which is what made the bug look like "only the backgrounds disappear".
//
// The assertions are whole-region, not centre-pixel: a quad that survives with three stale
// vertices still paints its centre.
//
// Reproduces on DirectVulkan only. DirectGLES forwards the unbound binding to the GLES driver,
// which does what GL says, so it is the control - every test here must stay green on both.

#include <cstring>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        constexpr const char* kTransformComputeSource = R"(#version 460 core

struct Vertex {
    float   x;
    float   y;
    float   z;
    float   u0;
    float   v0;
    uint    color;
};

struct VaryingData {
    int     offset;
    int     sharing;
    int     mesh;
    int     shouldCull;
};

struct SharingData {
    mat4    transform;
    mat3    normal;
};

layout(local_size_x = 128) in;

layout(binding=0, std430) restrict readonly buffer VerticesIn {
    Vertex verticesIn[];
};

layout(binding=1, std430) restrict writeonly buffer VerticesOut {
    Vertex verticesOut[];
};

layout(binding=2, std430) restrict readonly buffer Sharings {
    SharingData sharings[];
};

layout(binding=3, std430) restrict readonly buffer VaryingsIn {
    VaryingData varyingsIn[];
};

layout(binding=4, std430) restrict writeonly buffer VaryingsOut {
    VaryingData varyingsOut[];
};

layout(binding=5, std430) restrict readonly buffer Meshes {
    Vertex meshVertices[];
};

layout(location=0) uniform uint vertexCount;
layout(location=1) uniform uint vertexOffset;
layout(location=2) uniform uint varyingOffset;

void main() {
    uint    indexIn     = gl_GlobalInvocationID.x;
    uint    vertexOut   = indexIn + vertexOffset;
    uint    varyingOut  = indexIn + varyingOffset;

    if (indexIn >= vertexCount) {
        return;
    }

    int     offset      = varyingsIn[indexIn]   .offset;
    uint    reference   = indexIn - offset;
    int     sharing     = varyingsIn[reference] .sharing;
    int     mesh        = varyingsIn[reference] .mesh;

    mat4    transformMatrix;

    if (sharing != -1) {
        transformMatrix = sharings[sharing].transform;
    } else {
        transformMatrix = mat4(1.0);
    }

    Vertex  vertexIn;
    vec4    colorMesh;

    if (mesh != -1) {
        vertexIn    = meshVertices[mesh + offset];
        colorMesh   = unpackUnorm4x8    (vertexIn.color);
    } else {
        vertexIn    = verticesIn[indexIn];
        colorMesh   = vec4  (1.0);
    }

    vec4    colorIn = unpackUnorm4x8    (verticesIn[reference].color);

    vec4    posOut      = transformMatrix   * vec4          (vertexIn.x, vertexIn.y, vertexIn.z, 1.0);
    vec4    colorOut    = colorMesh         * colorIn;

    verticesOut[vertexOut].x        = posOut.x;
    verticesOut[vertexOut].y        = posOut.y;
    verticesOut[vertexOut].z        = posOut.z;

    verticesOut[vertexOut].u0       = vertexIn.u0;
    verticesOut[vertexOut].v0       = vertexIn.v0;

    verticesOut[vertexOut].color    = packUnorm4x8  (colorOut);

    varyingsOut[varyingOut].offset      = offset;
    varyingsOut[varyingOut].shouldCull  = varyingsIn[reference].shouldCull;
}
)";

        // Vanilla position_tex_color, spelled the way MC ships it: #version 150, no explicit
        // attribute locations (they come from glBindAttribLocation in format order) and the
        // ProjMat/ModelViewMat pair the mod re-uploads through setDefaultUniforms.
        constexpr const char* kBlitVertexSource = R"(#version 150
in vec3 Position;
in vec2 UV0;
in vec4 Color;
uniform mat4 ModelViewMat;
uniform mat4 ProjMat;
out vec2 texCoord0;
out vec4 vertexColor;
void main() {
    gl_Position = ProjMat * ModelViewMat * vec4(Position, 1.0);
    texCoord0 = UV0;
    vertexColor = Color;
}
)";

        constexpr const char* kBlitFragmentSource = R"(#version 150
uniform sampler2D Sampler0;
in vec2 texCoord0;
in vec4 vertexColor;
out vec4 fragColor;
void main() {
    fragColor = texture(Sampler0, texCoord0) * vertexColor;
}
)";

        constexpr int kFboSize = 64;
        constexpr int kShaderStorageRestoreRange = 9;
        constexpr int kAtomicCounterRestoreRange = 1;
        constexpr int kBuilders = 2;

        struct GuiVertex {
            float x, y, z;
            float u, v;
            std::uint32_t color;
        };
        static_assert(sizeof(GuiVertex) == 24, "POSITION_TEX_COLOR is 24 bytes");

        struct VaryingData {
            std::int32_t offset;
            std::int32_t sharing;
            std::int32_t mesh;
            std::int32_t shouldCull;
        };

        struct IndexedBinding {
            GLint buffer = 0;
            GLint start = 0;
            GLint size = 0;
        };

        // Which parts of the mod's real frame this replay reproduces. Each test flips exactly
        // one on top of the baseline so a failure names the element that killed the quad.
        struct Fidelity {
            bool shortIndices = false;   // MC's AutoStorageIndexBuffer is USHORT at small counts
            bool baseVertex = false;     // ...and the second builder draws at a base vertex
            bool twoBuilders = false;    // two render types share one output buffer
            bool blendAndDepth = false;  // TRANSLUCENT_TRANSPARENCY + LEQUAL_DEPTH_TEST
            bool regrow = false;         // MutableBuffer.doExpand replaces the GL name
            bool rewriteMapEachFrame = false;
            bool skipRelayout = false;   // bindDrawBuffers() only re-lays-out when resized
            bool leaveMeshesUnbound = false; // a batch with no server meshes never binds binding 5
        };

        class GuiBatchScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                m_transform = CompileComputeProgram(kTransformComputeSource);
                ASSERT_NE(m_transform, 0u) << m_buildLog;
                m_blit = CompileBlitProgram();
                ASSERT_NE(m_blit, 0u) << m_buildLog;

                m_target = MakeColorFbo(kFboSize, kFboSize);
                ASSERT_NE(m_target.fbo, 0u);

                MakeTexture();
                MakeIndexBuffers();
                MakeAcceleratedBuffers();
                m_laidOut = false;
                ASSERT_EQ(FirstGLError(), 0u) << "setup raised a GL error";
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glDisable(GL_BLEND);
                glDisable(GL_DEPTH_TEST);
                DestroyColorFbo(m_target);
                if (m_transform) glDeleteProgram(m_transform);
                if (m_blit) glDeleteProgram(m_blit);
                if (m_texture) glDeleteTextures(1, &m_texture);
                if (!m_buffers.empty()) glDeleteBuffers((GLsizei)m_buffers.size(), m_buffers.data());
                if (!m_vaos.empty()) glDeleteVertexArrays((GLsizei)m_vaos.size(), m_vaos.data());
                m_buffers.clear();
                m_vaos.clear();
            }

            unsigned int CompileComputeProgram(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[4096] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("compute shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("compute program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            GLuint CompileOne(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[4096] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                return shader;
            }

            // glBindAttribLocation in format order, exactly as ShaderInstance does it.
            unsigned int CompileBlitProgram() {
                const GLuint vs = CompileOne(GL_VERTEX_SHADER, kBlitVertexSource);
                if (!vs) return 0;
                const GLuint fs = CompileOne(GL_FRAGMENT_SHADER, kBlitFragmentSource);
                if (!fs) return 0;
                const GLuint program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, fs);
                glBindAttribLocation(program, 0, "Position");
                glBindAttribLocation(program, 1, "UV0");
                glBindAttribLocation(program, 2, "Color");
                glLinkProgram(program);
                glDeleteShader(vs);
                glDeleteShader(fs);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("blit program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            void MakeTexture() {
                std::vector<std::uint8_t> pixels(4 * 4 * 4, 0);
                for (int i = 0; i < 16; ++i) {
                    pixels[i * 4 + 2] = 255;
                    pixels[i * 4 + 3] = 255;
                }
                glGenTextures(1, &m_texture);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            }

            GLuint NewBuffer() {
                GLuint b = 0;
                glCreateBuffers(1, &b);
                m_buffers.push_back(b);
                return b;
            }

            GLuint NewVao() {
                GLuint v = 0;
                glCreateVertexArrays(1, &v);
                m_vaos.push_back(v);
                return v;
            }

            void MakeIndexBuffers() {
                std::uint32_t wide[12];
                std::uint16_t narrow[12];
                for (int quad = 0; quad < 2; ++quad) {
                    const std::uint32_t base = (std::uint32_t)(quad * 4);
                    const std::uint32_t pattern[6] = {base, base + 1, base + 2, base + 2, base + 3, base};
                    for (int i = 0; i < 6; ++i) {
                        wide[quad * 6 + i] = pattern[i];
                        narrow[quad * 6 + i] = (std::uint16_t)pattern[i];
                    }
                }
                m_wideIndices = NewBuffer();
                glNamedBufferStorage(m_wideIndices, sizeof(wide), wide, 0);
                m_narrowIndices = NewBuffer();
                glNamedBufferStorage(m_narrowIndices, sizeof(narrow), narrow, 0);
            }

            static void SetupAttributes() {
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (const void*)0);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 24, (const void*)12);
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 24, (const void*)20);
                glEnableVertexAttribArray(2);
            }

            void WriteInputs() {
                const GuiVertex unit[4] = {
                    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu},
                    {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu},
                    {1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu},
                    {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0xFFFFFFFFu},
                };
                for (int b = 0; b < kBuilders; ++b) {
                    std::memcpy(m_inVertexMap[b], unit, sizeof(unit));
                    VaryingData varyings[4];
                    for (int i = 0; i < 4; ++i) {
                        varyings[i].offset = i;
                        varyings[i].sharing = b;
                        varyings[i].mesh = -1;
                        varyings[i].shouldCull = 0;
                    }
                    std::memcpy(m_inVaryingMap[b], varyings, sizeof(varyings));
                }
            }

            void MakeAcceleratedBuffers() {
                const GLbitfield persistent = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                for (int b = 0; b < kBuilders; ++b) {
                    m_inVertices[b] = NewBuffer();
                    glNamedBufferStorage(m_inVertices[b], 4 * (GLsizeiptr)sizeof(GuiVertex), nullptr, persistent);
                    m_inVertexMap[b] =
                        glMapNamedBufferRange(m_inVertices[b], 0, 4 * (GLsizeiptr)sizeof(GuiVertex), persistent);
                    m_inVaryings[b] = NewBuffer();
                    glNamedBufferStorage(m_inVaryings[b], 4 * (GLsizeiptr)sizeof(VaryingData), nullptr, persistent);
                    m_inVaryingMap[b] =
                        glMapNamedBufferRange(m_inVaryings[b], 0, 4 * (GLsizeiptr)sizeof(VaryingData), persistent);
                }

                // Two SharingData entries: builder 0 lands left of centre, builder 1 right.
                float sharing[56] = {};
                const float tx[2] = {-0.9f, 0.1f};
                for (int b = 0; b < 2; ++b) {
                    float* m = sharing + b * 28;
                    m[0] = 0.8f;
                    m[5] = 1.0f;
                    m[10] = 1.0f;
                    m[15] = 1.0f;
                    m[12] = tx[b];
                    m[13] = -0.5f;
                    m[16] = 1.0f;
                    m[20] = 1.0f;
                    m[24] = 1.0f;
                }
                m_sharings = NewBuffer();
                glNamedBufferStorage(m_sharings, sizeof(sharing), nullptr, persistent);
                void* r = glMapNamedBufferRange(m_sharings, 0, sizeof(sharing), persistent);
                std::memcpy(r, sharing, sizeof(sharing));

                m_outSize = 8 * (GLsizeiptr)sizeof(GuiVertex);
                m_outVertices = NewBuffer();
                glNamedBufferStorage(m_outVertices, m_outSize, nullptr, GL_DYNAMIC_STORAGE_BIT);
                m_outVaryings = NewBuffer();
                glNamedBufferStorage(m_outVaryings, 8 * (GLsizeiptr)sizeof(VaryingData), nullptr,
                                     GL_DYNAMIC_STORAGE_BIT);
                m_meshes = NewBuffer();
                glNamedBufferStorage(m_meshes, 4 * (GLsizeiptr)sizeof(GuiVertex), nullptr, GL_DYNAMIC_STORAGE_BIT);

                m_vao = NewVao();
                WriteInputs();
            }

            std::vector<IndexedBinding> Record(GLenum b, GLenum s, GLenum z, int range) {
                std::vector<IndexedBinding> saved((std::size_t)range);
                for (int i = 0; i < range; ++i) {
                    glGetIntegeri_v(b, (GLuint)i, &saved[(std::size_t)i].buffer);
                    glGetIntegeri_v(s, (GLuint)i, &saved[(std::size_t)i].start);
                    glGetIntegeri_v(z, (GLuint)i, &saved[(std::size_t)i].size);
                }
                return saved;
            }

            void Restore(GLenum target, const std::vector<IndexedBinding>& saved) {
                for (std::size_t i = 0; i < saved.size(); ++i) {
                    if (saved[i].start == 0 && saved[i].size == 0) {
                        glBindBufferBase(target, (GLuint)i, (GLuint)saved[i].buffer);
                    } else {
                        glBindBufferRange(target, (GLuint)i, (GLuint)saved[i].buffer, saved[i].start,
                                          saved[i].size);
                    }
                }
            }

            // MutableBuffer.doExpand: a NEW immutable store, copied from the old one, old one gone.
            void RegrowOutputBuffer() {
                const GLsizeiptr newSize = m_outSize * 2;
                GLuint grown = 0;
                glCreateBuffers(1, &grown);
                glNamedBufferStorage(grown, newSize, nullptr, GL_DYNAMIC_STORAGE_BIT);
                glCopyNamedBufferSubData(m_outVertices, grown, 0, 0, m_outSize);
                glDeleteBuffers(1, &m_outVertices);
                for (auto& b : m_buffers) {
                    if (b == m_outVertices) b = grown;
                }
                m_outVertices = grown;
                m_outSize = newSize;
            }

            void Frame(const Fidelity& f, int builders) {
                if (f.rewriteMapEachFrame) WriteInputs();

                // --- prepareBuffers() -------------------------------------------------
                const std::vector<IndexedBinding> ssbo =
                    Record(GL_SHADER_STORAGE_BUFFER_BINDING, GL_SHADER_STORAGE_BUFFER_START,
                           GL_SHADER_STORAGE_BUFFER_SIZE, kShaderStorageRestoreRange);
                const std::vector<IndexedBinding> counters =
                    Record(GL_ATOMIC_COUNTER_BUFFER_BINDING, GL_ATOMIC_COUNTER_BUFFER_START,
                           GL_ATOMIC_COUNTER_BUFFER_SIZE, kAtomicCounterRestoreRange);
                GLint currentProgram = 0;
                glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);

                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_outVertices);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_sharings);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_outVaryings);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, f.leaveMeshesUnbound ? 0 : m_meshes);
                glUseProgram(m_transform);
                for (int b = 0; b < builders; ++b) {
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_inVertices[b]);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_inVaryings[b]);
                    glProgramUniform1ui(m_transform, glGetUniformLocation(m_transform, "vertexCount"), 4u);
                    glProgramUniform1ui(m_transform, glGetUniformLocation(m_transform, "vertexOffset"),
                                        (GLuint)(4 * b));
                    glProgramUniform1ui(m_transform, glGetUniformLocation(m_transform, "varyingOffset"),
                                        (GLuint)(4 * b));
                    glDispatchCompute(1, 1, 1);
                }
                glUseProgram(0);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                glUseProgram((GLuint)currentProgram);
                Restore(GL_SHADER_STORAGE_BUFFER, ssbo);
                Restore(GL_ATOMIC_COUNTER_BUFFER, counters);

                if (f.regrow) {
                    RegrowOutputBuffer();
                    m_laidOut = false; // isResized() forces the relayout
                }

                // --- drawBuffers() ----------------------------------------------------
                glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT |
                                GL_COMMAND_BARRIER_BIT);
                glBindVertexArray(m_vao);
                if (!m_laidOut || !f.skipRelayout) {
                    glBindBuffer(GL_ARRAY_BUFFER, m_outVertices);
                    SetupAttributes();
                    m_laidOut = true;
                }

                if (f.blendAndDepth) {
                    glEnable(GL_BLEND);
                    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(GL_LEQUAL);
                }

                const GLenum indexType = f.shortIndices ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
                const GLuint indexBuffer = f.shortIndices ? m_narrowIndices : m_wideIndices;
                const GLsizei indexStride = f.shortIndices ? 2 : 4;

                for (int b = 0; b < builders; ++b) {
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer); // AutoStorageIndexBuffer.bind
                    glUseProgram(m_blit);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, m_texture);
                    glUniform1i(glGetUniformLocation(m_blit, "Sampler0"), 0);
                    UploadIdentityMatrices();
                    if (f.baseVertex) {
                        // The mod's BASEVERTEX path: every builder reads the SAME first six
                        // indices and offsets the vertices with a base vertex.
                        glDrawElementsBaseVertex(GL_TRIANGLES, 6, indexType, (const void*)0, 4 * b);
                    } else {
                        glDrawElements(GL_TRIANGLES, 6, indexType, (const void*)(intptr_t)(b * 6 * indexStride));
                    }
                    glUseProgram(0);
                }

                if (f.blendAndDepth) {
                    glDisable(GL_BLEND);
                    glDisable(GL_DEPTH_TEST);
                }
                glBindVertexArray(0);
            }

            void UploadIdentityMatrices() {
                static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                glUniformMatrix4fv(glGetUniformLocation(m_blit, "ModelViewMat"), 1, GL_FALSE, identity);
                glUniformMatrix4fv(glGetUniformLocation(m_blit, "ProjMat"), 1, GL_FALSE, identity);
            }

            void ExpectQuads(const Image& image, int builders, const char* when) {
                EXPECT_TRUE(RegionIsMostly(image, 6, 26, 19, 45, "blue", 0.0,
                                           std::string("left accelerated quad, ") + when));
                if (builders > 1) {
                    EXPECT_TRUE(RegionIsMostly(image, 38, 58, 19, 45, "blue", 0.0,
                                               std::string("right accelerated quad, ") + when));
                }
            }

            void RunFrames(const Fidelity& f, int builders, int frames, const char* when) {
                for (int i = 0; i < frames; ++i) {
                    BindFbo(m_target);
                    ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                    Frame(f, builders);
                    ASSERT_EQ(FirstGLError(), 0u) << "frame " << i << " raised a GL error (" << when << ")";
                    const Image image = ReadPixels(kFboSize, kFboSize);
                    ExpectQuads(image, builders, (std::string(when) + ", frame " + std::to_string(i)).c_str());
                    Gl().EndFrame();
                }
            }

            unsigned int m_transform = 0;
            unsigned int m_blit = 0;
            ColorFbo m_target{};
            GLuint m_texture = 0;
            GLuint m_wideIndices = 0;
            GLuint m_narrowIndices = 0;
            GLuint m_vao = 0;
            GLuint m_inVertices[kBuilders] = {};
            GLuint m_inVaryings[kBuilders] = {};
            void* m_inVertexMap[kBuilders] = {};
            void* m_inVaryingMap[kBuilders] = {};
            GLuint m_sharings = 0;
            GLuint m_outVertices = 0;
            GLuint m_outVaryings = 0;
            GLuint m_meshes = 0;
            GLsizeiptr m_outSize = 0;
            bool m_laidOut = false;
            std::vector<GLuint> m_buffers;
            std::vector<GLuint> m_vaos;
            std::string m_buildLog;
        };

    } // namespace

    TEST_F(GuiBatchScenario, Baseline) {
        if (!Ready() || IsSkipped()) return;
        RunFrames(Fidelity{}, 1, 1, "baseline");
    }

    TEST_F(GuiBatchScenario, ShortIndices) {
        if (!Ready() || IsSkipped()) return;
        Fidelity f;
        f.shortIndices = true;
        RunFrames(f, 1, 1, "short indices");
    }

    TEST_F(GuiBatchScenario, TwoBuildersWideIndices) {
        if (!Ready() || IsSkipped()) return;
        RunFrames(Fidelity{}, 2, 1, "two builders, wide indices");
    }

    TEST_F(GuiBatchScenario, TwoBuildersBaseVertex) {
        if (!Ready() || IsSkipped()) return;
        Fidelity f;
        f.baseVertex = true;
        RunFrames(f, 2, 1, "two builders, base vertex");
    }

    TEST_F(GuiBatchScenario, TwoBuildersShortIndicesBaseVertex) {
        if (!Ready() || IsSkipped()) return;
        Fidelity f;
        f.baseVertex = true;
        f.shortIndices = true;
        RunFrames(f, 2, 1, "two builders, short indices, base vertex");
    }

    TEST_F(GuiBatchScenario, BlendAndDepth) {
        if (!Ready() || IsSkipped()) return;
        Fidelity f;
        f.baseVertex = true;
        f.shortIndices = true;
        f.blendAndDepth = true;
        RunFrames(f, 2, 1, "blend and depth");
    }

    TEST_F(GuiBatchScenario, ThreeFramesWithoutRelayout) {
        if (!Ready() || IsSkipped()) return;
        Fidelity f;
        f.baseVertex = true;
        f.shortIndices = true;
        f.blendAndDepth = true;
        f.skipRelayout = true;
        f.rewriteMapEachFrame = true;
        RunFrames(f, 2, 3, "three frames without relayout");
    }

    TEST_F(GuiBatchScenario, MeshesBlockLeftUnbound) {
        if (!Ready() || IsSkipped()) return;
        Fidelity f;
        f.baseVertex = true;
        f.shortIndices = true;
        f.blendAndDepth = true;
        f.leaveMeshesUnbound = true;
        RunFrames(f, 2, 1, "Meshes block left unbound");
    }

    TEST_F(GuiBatchScenario, FullFidelityWithRegrowth) {
        if (!Ready() || IsSkipped()) return;
        Fidelity f;
        f.baseVertex = true;
        f.shortIndices = true;
        f.blendAndDepth = true;
        f.skipRelayout = true;
        f.rewriteMapEachFrame = true;
        RunFrames(f, 2, 1, "full fidelity, pre-growth");
        f.regrow = true;
        RunFrames(f, 2, 1, "full fidelity, growth frame");
        f.regrow = false;
        RunFrames(f, 2, 2, "full fidelity, post-growth");
    }

    namespace {

        // Atomic counter blocks resolve through the SAME descriptor path as shader storage
        // blocks (glslang rewrites every atomic_uint into a synthesized storage block), so an
        // unbound GL_ATOMIC_COUNTER_BUFFER point loses the dispatch for exactly the same reason
        // an unbound SSBO did. The mod reaches this with its INDIRECT draw method, whose culling
        // shaders carry a counter the BASEVERTEX default never binds.
        //
        // The counter is INCREMENTED, not merely declared: an unreferenced one is optimised out
        // before it ever reaches a descriptor, so a shader that only declares it proves nothing.
        constexpr const char* kCounterComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 0, offset = 0) uniform atomic_uint g_unbound;
layout(std430, binding = 0) buffer Output { uint g_data[]; };
void main() {
    atomicCounterIncrement(g_unbound);
    g_data[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x + 1u;
}
)";

        class UnboundCounterBlockScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                GLint counters = 0;
                glGetIntegerv(GL_MAX_COMPUTE_ATOMIC_COUNTERS, &counters);
                if (counters < 1) {
                    GTEST_SKIP() << "GL_MAX_COMPUTE_ATOMIC_COUNTERS is " << counters;
                }
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &kCounterComputeSource, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                ASSERT_NE(compiled, GL_FALSE);
                m_program = glCreateProgram();
                glAttachShader(m_program, shader);
                glLinkProgram(m_program);
                glDeleteShader(shader);
                GLint linked = 0;
                glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
                ASSERT_NE(linked, GL_FALSE);
                glGenBuffers(1, &m_buffer);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                if (m_buffer) glDeleteBuffers(1, &m_buffer);
                if (m_program) glDeleteProgram(m_program);
            }

            unsigned int m_program = 0;
            GLuint m_buffer = 0;
        };

    } // namespace

    TEST_F(UnboundCounterBlockScenario, ADeclaredButUnboundCounterDoesNotLoseTheDispatch) {
        if (!Ready() || IsSkipped()) return;

        constexpr int kElements = 4;
        const std::vector<unsigned int> zeros((std::size_t)kElements, 0u);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(zeros.size() * sizeof(unsigned int)), zeros.data(),
                     GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_buffer);
        // Nothing is bound at GL_ATOMIC_COUNTER_BUFFER point 0 on purpose.
        glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, 0);
        ASSERT_EQ(FirstGLError(), 0u);

        glUseProgram(m_program);
        glDispatchCompute(kElements, 1, 1);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        EXPECT_EQ(FirstGLError(), 0u) << "the dispatch raised a GL error";

        std::vector<unsigned int> values((std::size_t)kElements, 0xDEADBEEFu);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_buffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(values.size() * sizeof(unsigned int)),
                           values.data());
        for (int i = 0; i < kElements; ++i) {
            EXPECT_EQ(values[(std::size_t)i], (unsigned int)(i + 1))
                << "element " << i << " came back as " << values[(std::size_t)i]
                << "; zero everywhere means the whole dispatch was dropped over the unbound counter block";
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    }
} // namespace MGITest
