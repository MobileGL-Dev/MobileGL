// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DeletedBoundBufferScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A BUFFER DELETED WHILE IT IS STILL BOUND TO AN INDEXED BINDING POINT.
//
// glDeleteBuffers unbinds the buffer from every binding point of the current context (GL 4.6
// core 5.1.2, ES 3.2 6.1.1), indexed ones included, and the frontend does (BufferState::
// MarkBufferObjectForDeletion). Under a transport that is not enough: the SERVER holds its own
// copy of each indexed window (set_shader_buffers / the uniform window), re-sent only when the
// target's bind-point GENERATION moves (Tracker.h's bits 15/16/17). A delete that cleared the
// points without moving the generation left the server naming the dead buffer's handle; once
// the client recycled that handle's slot at a newer generation, the next draw or dispatch that
// walked the window asked the backend for the old one:
//   Espryt  Fatal{ProtocolCorruption, "BackendSlotTable.Generation"} (the whole session dies);
//   Magma   the handle resolves to nothing and the draw / dispatch that declares the point is
//           dropped.
// (P11 B2 addendum; first seen as LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack
// followed by TheArenaLandsInTheTierItsLaneDeclares in one process.)
//
// Each case, for one indexed target: bind a buffer at point 0 and use it (so the window naming it
// reaches the server), delete it WHILE BOUND, create buffers until its slot has been reused at a
// newer generation, then draw and dispatch with a program that DECLARES point 0 but never
// touches it (a uniform-guarded branch: reading an empty point would be undefined, declaring it
// is not). Both must run - no Fatal, a marker written, the frame drawn - and a fresh buffer bound
// at the same point afterwards must be read correctly.

#include <array>
#include <initializer_list>
#include <sstream>
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

        // More than enough for any free-list order: the dead buffer's slot is reused well before.
        constexpr int kRecycledBuffers = 16;
        constexpr unsigned int kMarker = 0xC0FFEEu;
        constexpr unsigned int kFreshWord = 0x1234u;
        constexpr GLuint kResultBinding = 7;

        constexpr const char* kQuadVertexSource = R"(#version 430 core
layout(location = 0) in vec2 a_pos;
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }
)";
        constexpr const char* kQuadFragmentSource = R"(#version 430 core
out vec4 o_color;
void main() { o_color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        // The target's declaration and the one read of it; the read is only ever taken with
        // u_read = 1 (a fresh buffer bound). Result: the marker, unconditional, and what was read.
        std::string ComputeSource(const char* declaration, const char* read) {
            std::ostringstream os;
            os << "#version 430 core\n"
                  "layout(local_size_x = 1) in;\n"
                  "layout(std430, binding = 7) buffer Result { uint marker; uint value; };\n"
                  "uniform uint u_read;\n"
               << declaration << "\n"
               << "void main() {\n"
                  "    uint v = 0u;\n"
                  "    if (u_read != 0u) { v = " << read << "; }\n"
                  "    marker = 0xC0FFEEu;\n"
                  "    value = v;\n"
                  "}\n";
            return os.str();
        }

        constexpr const char* kCaptureVertexSource = R"(#version 430 core
layout(location = 0) in vec2 a_pos;
out float vs_value;
void main() {
    vs_value = float(gl_VertexID) * 2.0 + 1.0;
    gl_Position = vec4(a_pos, 0.0, 1.0);
    gl_PointSize = 1.0;
}
)";

        class DeletedBoundBufferScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_quad = Link(kQuadVertexSource, kQuadFragmentSource);
                ASSERT_NE(m_quad, 0u) << m_log;
                const float quad[] = {-1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f};
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
                glEnableVertexAttribArray(0);
                // The result block lives at its own point, bound ONCE here: nothing later rebinds
                // a storage point, so the storage window the server holds is only ever re-sent
                // by what the case itself does.
                const unsigned int zero[2] = {0u, 0u};
                glGenBuffers(1, &m_result);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_result);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), zero, GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kResultBinding, m_result);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                if (!m_owned.empty()) glDeleteBuffers(static_cast<GLsizei>(m_owned.size()), m_owned.data());
                if (m_result != 0) glDeleteBuffers(1, &m_result);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                for (GLuint p : {m_quad, m_compute, m_capture}) {
                    if (p != 0) glDeleteProgram(p);
                }
                m_owned.clear();
                m_result = m_vbo = m_vao = m_quad = m_compute = m_capture = 0;
            }

            GLuint Compile(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint ok = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
                if (!ok) {
                    char log[2048] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    m_log = std::string("shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                return shader;
            }

            GLuint LinkShaders(std::initializer_list<GLuint> shaders, const char* varying = nullptr) {
                const GLuint program = glCreateProgram();
                for (GLuint s : shaders) glAttachShader(program, s);
                if (varying != nullptr) glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
                glLinkProgram(program);
                for (GLuint s : shaders) glDeleteShader(s);
                GLint ok = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &ok);
                if (!ok) {
                    char log[2048] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    m_log = std::string("program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            GLuint Link(const char* vs, const char* fs, const char* varying = nullptr) {
                const GLuint v = Compile(GL_VERTEX_SHADER, vs);
                if (v == 0) return 0;
                const GLuint f = Compile(GL_FRAGMENT_SHADER, fs);
                if (f == 0) {
                    glDeleteShader(v);
                    return 0;
                }
                return LinkShaders({v, f}, varying);
            }

            GLuint LinkCompute(const std::string& source) {
                const GLuint c = Compile(GL_COMPUTE_SHADER, source.c_str());
                return c == 0 ? 0 : LinkShaders({c});
            }

            GLuint MakeBuffer(GLenum target, const void* data, GLsizeiptr size) {
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(target, buffer);
                glBufferData(target, size, data, GL_DYNAMIC_DRAW);
                glBindBuffer(target, 0);
                return buffer;
            }

            // Delete the buffer still bound at `target` point 0, then take the dead buffer's slot
            // back at a newer generation: every new buffer is defined with content, which is what
            // makes the server twin it (and so move its live generation) at once.
            void DeleteWhileBoundAndRecycle(GLuint& bound) {
                glDeleteBuffers(1, &bound);
                bound = 0;
                std::vector<unsigned int> payload(64);
                for (int i = 0; i < kRecycledBuffers; ++i) {
                    for (auto& word : payload) word = static_cast<unsigned int>(i);
                    m_owned.push_back(MakeBuffer(GL_ARRAY_BUFFER, payload.data(),
                                                 static_cast<GLsizeiptr>(payload.size() * sizeof(unsigned int))));
                }
            }

            // The draw path first (Espryt walks the whole storage window at every draw), then the
            // frame must show the quad.
            void DrawTheQuadAndExpectIt(const char* when) {
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glClearColor(0.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(m_quad);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                unsigned char px[4] = {0, 0, 0, 0};
                glReadPixels(Gl().Width() / 2, Gl().Height() / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                EXPECT_GT(px[1], 200) << "the quad was not drawn " << when;
                EXPECT_LT(px[0], 50) << when;
            }

            // Dispatch the target's program once; returns {marker, value}.
            std::array<unsigned int, 2> Dispatch(unsigned int read) {
                const unsigned int zero[2] = {0u, 0u};
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_result);
                glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero), zero);
                glUseProgram(m_compute);
                glUniform1ui(glGetUniformLocation(m_compute, "u_read"), read);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT |
                                GL_ATOMIC_COUNTER_BARRIER_BIT);
                std::array<unsigned int, 2> out = {0xDEADBEEFu, 0xDEADBEEFu};
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_result);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(out), out.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                return out;
            }

            // The three shader-buffer targets share one shape.
            void RunShaderBufferTarget(GLenum target, const char* declaration, const char* read,
                                       unsigned int freshSeed, unsigned int expectedRead) {
                m_compute = LinkCompute(ComputeSource(declaration, read));
                ASSERT_NE(m_compute, 0u) << m_log;

                // 1. A buffer at point 0, used once so the window naming it crosses.
                const unsigned int seed[4] = {freshSeed, 0u, 0u, 0u};
                GLuint doomed = MakeBuffer(target, seed, sizeof(seed));
                glBindBufferBase(target, 0, doomed);
                auto first = Dispatch(0);
                EXPECT_EQ(first[0], kMarker) << "the dispatch with the buffer bound did not run";
                ASSERT_EQ(FirstGLError(), 0u);

                // 2. Deleted while bound; its slot recycled at a newer generation.
                DeleteWhileBoundAndRecycle(doomed);
                GLint stillBound = -1;
                glGetIntegeri_v(target == GL_SHADER_STORAGE_BUFFER ? GL_SHADER_STORAGE_BUFFER_BINDING
                                : target == GL_UNIFORM_BUFFER      ? GL_UNIFORM_BUFFER_BINDING
                                                                   : GL_ATOMIC_COUNTER_BUFFER_BINDING,
                                0, &stillBound);
                EXPECT_EQ(stillBound, 0) << "glDeleteBuffers did not unbind indexed point 0";

                // 3. The SAME program dispatched again - no glUseProgram of another program in
                //    between, because the uniform window's shutter mixes the program identity in and a
                //    program change would re-send the window anyway - then a draw. Both must run.
                auto after = Dispatch(0);
                EXPECT_EQ(after[0], kMarker)
                    << "the dispatch that declares the deleted buffer's point did not run: its window still named "
                       "the dead handle (Magma drops the dispatch; Espryt would have died with "
                       "Fatal{ProtocolCorruption, \"BackendSlotTable.Generation\"} before this line)";
                DrawTheQuadAndExpectIt("after the bound buffer was deleted and its slot reused");
                EXPECT_EQ(FirstGLError(), 0u);

                // 4. A fresh buffer at the same point reads correctly.
                const unsigned int fresh[4] = {freshSeed, 0u, 0u, 0u};
                const GLuint replacement = MakeBuffer(target, fresh, sizeof(fresh));
                m_owned.push_back(replacement);
                glBindBufferBase(target, 0, replacement);
                auto read1 = Dispatch(1);
                EXPECT_EQ(read1[0], kMarker);
                EXPECT_EQ(read1[1], expectedRead) << "the buffer bound after the delete was not the one read";
                EXPECT_EQ(FirstGLError(), 0u);
                glBindBufferBase(target, 0, 0);
            }

            GLuint m_quad = 0;
            GLuint m_compute = 0;
            GLuint m_capture = 0;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_result = 0;
            std::vector<GLuint> m_owned;
            std::string m_log;
        };

    } // namespace

    TEST_F(DeletedBoundBufferScenario, AStorageBufferDeletedWhileBoundLeavesNoStaleBinding) {
        if (!Ready() || IsSkipped()) return;
        RunShaderBufferTarget(GL_SHADER_STORAGE_BUFFER,
                              "layout(std430, binding = 0) buffer Target { uint word; };", "word",
                              kFreshWord, kFreshWord);
    }

    TEST_F(DeletedBoundBufferScenario, AUniformBufferDeletedWhileBoundLeavesNoStaleBinding) {
        if (!Ready() || IsSkipped()) return;
        RunShaderBufferTarget(GL_UNIFORM_BUFFER, "layout(std140, binding = 0) uniform Target { uint word; };",
                              "word", kFreshWord, kFreshWord);
    }

    // The counter's pre-increment value is what the shader reads, and the buffer then holds seed + 1.
    TEST_F(DeletedBoundBufferScenario, AnAtomicCounterBufferDeletedWhileBoundLeavesNoStaleBinding) {
        if (!Ready() || IsSkipped()) return;
        GLint counters = 0;
        glGetIntegerv(GL_MAX_COMPUTE_ATOMIC_COUNTERS, &counters);
        if (counters < 1) GTEST_SKIP() << "GL_MAX_COMPUTE_ATOMIC_COUNTERS is " << counters;
        RunShaderBufferTarget(GL_ATOMIC_COUNTER_BUFFER, "layout(binding = 0, offset = 0) uniform atomic_uint counter;",
                              "atomicCounterIncrement(counter)", 41u, 41u);
    }

    // Transform feedback: nothing re-sends a capture window at a draw today (bit 17 is computed and
    // not emitted), so the stale-handle failure cannot reach the backend through this target; the
    // case pins the behaviour anyway - the delete unbinds, a draw after the slot is reused runs,
    // and a fresh capture buffer at the same point receives exactly the captured values.
    TEST_F(DeletedBoundBufferScenario, ATransformFeedbackBufferDeletedWhileBoundLeavesNoStaleBinding) {
        if (!Ready() || IsSkipped()) return;
        m_capture = Link(kCaptureVertexSource, kQuadFragmentSource, "vs_value");
        ASSERT_NE(m_capture, 0u) << m_log;
        constexpr int kPoints = 4;
        const float poison[kPoints + 2] = {-7.f, -7.f, -7.f, -7.f, -7.f, -7.f};

        auto capture = [&](GLuint into) {
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, into);
            glUseProgram(m_capture);
            glBindVertexArray(m_vao);
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, kPoints);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);
        };

        GLuint doomed = MakeBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, poison, sizeof(poison));
        capture(doomed);
        ASSERT_EQ(FirstGLError(), 0u);

        DeleteWhileBoundAndRecycle(doomed);
        GLint stillBound = -1;
        glGetIntegeri_v(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, 0, &stillBound);
        EXPECT_EQ(stillBound, 0) << "glDeleteBuffers did not unbind transform feedback point 0";
        DrawTheQuadAndExpectIt("after the bound capture buffer was deleted and its slot reused");
        EXPECT_EQ(FirstGLError(), 0u);

        const GLuint fresh = MakeBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, poison, sizeof(poison));
        m_owned.push_back(fresh);
        capture(fresh);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        float captured[kPoints + 2] = {};
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, fresh);
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(captured), captured);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
        EXPECT_EQ(FirstGLError(), 0u);
        for (int i = 0; i < kPoints + 2; ++i) {
            const float expected = i < kPoints ? static_cast<float>(i) * 2.f + 1.f : -7.f;
            EXPECT_EQ(captured[i], expected) << "capture record " << i;
        }
    }

} // namespace MGITest
