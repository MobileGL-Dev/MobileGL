// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/LargeArenaAdoptionScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - MESH-ARENA-SIZED BUFFERS, END TO END.
//
// A buffer store of at least 16MiB is adopted into the backend's persistently and
// coherently mapped GPU storage the moment it is defined (BufferObject::
// TryAdoptLargeStorage): the CPU shadow is dropped and every later write lands
// directly in GPU-visible memory with no per-write driver call. Minecraft 26.3
// streams chunk meshes into 128MB vertex arenas with plain glNamedBufferSubData -
// on Mali, every driver-mediated route for that write into a busy mutable store
// either parks the calling thread or ghost-copies the whole arena on a driver
// worker (~167ms per touched arena: the recurring in-world hiccup this adoption
// removed). Every existing buffer scenario uses stores far below the threshold,
// so without this file the adopted path would have zero coverage.
//
// What is pinned, deliberately through the same API mix Minecraft uses:
//   * a glBufferSubData written AFTER the arena was drawn (in flight) reaches the
//     next draw - the write-visibility contract adoption must not weaken;
//   * GetBufferSubData reads back the latest CPU write - the shadow IS the map;
//   * a compute-shader write through an SSBO binding of the same arena is read
//     back - the GPU-written path for adopted stores (glFinish + direct read).

#include <array>
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

        // Comfortably past the 16MiB adoption threshold, and the vertex payload sits
        // deep inside the store so an implementation that quietly clamped or aliased
        // the adopted range would miss it.
        constexpr GLsizeiptr kArenaBytes = GLsizeiptr(24) * 1024 * 1024;
        constexpr GLintptr kVertexOffset = GLintptr(20) * 1024 * 1024;

        constexpr const char* kVertexSource = R"(#version 430 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec3 a_color;
out vec3 v_color;
void main() {
    v_color = a_color;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 430 core
in vec3 v_color;
out vec4 o_color;
void main() { o_color = vec4(v_color, 1.0); }
)";

        constexpr const char* kMarkerComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Arena { uint word; };
void main() { word = 0xC0FFEEu; }
)";

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        // A full-viewport quad, colored uniformly so one center readback speaks for
        // the whole draw.
        std::vector<Vertex> QuadVertices(float r, float g, float b) {
            return {
                {-1.f, -1.f, r, g, b}, {1.f, -1.f, r, g, b}, {1.f, 1.f, r, g, b},
                {-1.f, -1.f, r, g, b}, {1.f, 1.f, r, g, b},  {-1.f, 1.f, r, g, b},
            };
        }

        class LargeArenaAdoptionScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_program = LinkProgram(kVertexSource, kFragmentSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_arena);
                glBindBuffer(GL_ARRAY_BUFFER, m_arena);
                // The NULL-data definition is the adoption point (and Minecraft's
                // arena-creation idiom).
                glBufferData(GL_ARRAY_BUFFER, kArenaBytes, nullptr, GL_DYNAMIC_DRAW);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<void*>(kVertexOffset));
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<void*>(kVertexOffset + 2 * sizeof(float)));
                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_arena != 0) glDeleteBuffers(1, &m_arena);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_compute != 0) glDeleteProgram(m_compute);
                m_vao = 0;
                m_arena = 0;
                m_program = 0;
                m_compute = 0;
            }

            unsigned int CompileStage(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[2048] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                return shader;
            }

            unsigned int LinkProgram(const char* vs, const char* fs) {
                const GLuint v = CompileStage(GL_VERTEX_SHADER, vs);
                if (v == 0) return 0;
                const GLuint f = CompileStage(GL_FRAGMENT_SHADER, fs);
                if (f == 0) {
                    glDeleteShader(v);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, v);
                glAttachShader(program, f);
                glLinkProgram(program);
                glDeleteShader(v);
                glDeleteShader(f);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[2048] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            void UploadQuad(float r, float g, float b) {
                const auto vertices = QuadVertices(r, g, b);
                glBindBuffer(GL_ARRAY_BUFFER, m_arena);
                glBufferSubData(GL_ARRAY_BUFFER, kVertexOffset,
                                GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data());
            }

            void DrawQuad() {
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glClearColor(0.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }

            std::array<unsigned char, 4> CenterPixel() {
                std::array<unsigned char, 4> px = {0, 0, 0, 0};
                glReadPixels(Gl().Width() / 2, Gl().Height() / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                             px.data());
                return px;
            }

            unsigned int m_program = 0;
            unsigned int m_compute = 0;
            unsigned int m_vao = 0;
            unsigned int m_arena = 0;
            std::string m_buildLog;
        };

    } // namespace

    // The Minecraft shape: the arena is drawn, the frame retires, and a
    // glBufferSubData rewrites the SAME vertex bytes while the previous frame's
    // draw may still be in flight. The next draw must show the NEW bytes.
    TEST_F(LargeArenaAdoptionScenario, SubDataAfterAnInFlightDrawReachesTheNextDraw) {
        if (!Ready() || IsSkipped()) return;

        UploadQuad(1.f, 0.f, 0.f);
        DrawQuad();
        auto px = CenterPixel();
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_GT(px[0], 200) << "the first draw from the adopted arena never landed";
        EXPECT_LT(px[1], 50);

        Gl().EndFrame();

        UploadQuad(0.f, 1.f, 0.f);
        DrawQuad();
        px = CenterPixel();
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_GT(px[1], 200) << "the cross-frame rewrite of the adopted arena did not reach the draw; "
                                 "the old color means the write went to bytes the draw no longer reads";
        EXPECT_LT(px[0], 50) << "the draw still shows the previous frame's bytes";
    }

    // The shadow IS the mapping: a readback straight after a CPU write must hand
    // back exactly those bytes.
    TEST_F(LargeArenaAdoptionScenario, ReadbackSeesTheLatestCpuWrite) {
        if (!Ready() || IsSkipped()) return;

        const auto vertices = QuadVertices(0.25f, 0.5f, 0.75f);
        glBindBuffer(GL_ARRAY_BUFFER, m_arena);
        glBufferSubData(GL_ARRAY_BUFFER, kVertexOffset,
                        GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data());
        std::vector<Vertex> read(vertices.size());
        glGetBufferSubData(GL_ARRAY_BUFFER, kVertexOffset,
                           GLsizeiptr(read.size() * sizeof(Vertex)), read.data());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(0, std::memcmp(read.data(), vertices.data(), read.size() * sizeof(Vertex)))
            << "GetBufferSubData of the adopted arena returned different bytes than the SubData wrote";
    }

    // A GPU write through an SSBO binding of the adopted arena must be visible to
    // a CPU readback - the path that waits out the GPU and reads the coherent
    // mapping directly.
    TEST_F(LargeArenaAdoptionScenario, GpuWriteIntoTheArenaIsReadBack) {
        if (!Ready() || IsSkipped()) return;

        GLint maxComputeStorageBlocks = 0;
        glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &maxComputeStorageBlocks);
        if (maxComputeStorageBlocks < 1) {
            GTEST_SKIP() << "no compute shader storage blocks on this driver";
        }
        const GLuint compute = CompileStage(GL_COMPUTE_SHADER, kMarkerComputeSource);
        ASSERT_NE(compute, 0u) << m_buildLog;
        m_compute = glCreateProgram();
        glAttachShader(m_compute, compute);
        glLinkProgram(m_compute);
        glDeleteShader(compute);
        GLint linked = 0;
        glGetProgramiv(m_compute, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_TRUE);

        const unsigned int seed = 0u;
        glBindBuffer(GL_ARRAY_BUFFER, m_arena);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(seed), &seed);
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, m_arena, 0, sizeof(unsigned int));
        glUseProgram(m_compute);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        unsigned int marker = 0;
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(marker), &marker);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(marker, 0xC0FFEEu)
            << "the compute write into the adopted arena did not reach the CPU readback";
    }

} // namespace MGITest
