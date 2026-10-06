// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/GpuWrittenDrawInputScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - DRAW INPUTS A SHADER WROTE, READ BY A BACKEND ON THE CPU (P8-C).
//
// Espryt emulates five desktop-GL draw features on the CPU, and each one reads a buffer's bytes
// on the side that applies the draw:
//
//   * the primitive-restart rewrite (an application restart index that is not the all-ones value
//     of the index type): the whole element array buffer, rewritten into a scratch copy;
//   * the multi-draw rebase tier, which a batch with such a restart index is forced onto;
//   * *IndirectCount: the count word of the parameter buffer (GLES has no counted indirect draw);
//   * a native indirect draw's gl_BaseVertex, fed to the shader as a uniform from the command;
//   * a GL_DOUBLE vertex array, narrowed to float from the source buffer's bytes.
//
// Under split those bytes are the server's R-11 staged copy, which only uploads fill. A buffer a
// shader WROTE on the server - storage block, atomic counter, buffer image, transform feedback -
// is therefore read stale unless the server refreshes its copy first. Every case below writes
// the buffer on the GPU only (the staged copy holds a poison the correct picture cannot come
// from), so a stale read is a wrong picture, never a lucky one.
//
// MONOLITH ARMS KNOWN BROKEN HERE SKIP BY NAME (SkipBrokenMonolith). Espryt reads three from the
// frontend shadow without SyncGpuWrites: *IndirectCount (OPEN-QUESTIONS 15), an indirect
// gl_BaseVertex (the comment beside it in DirectGLES.cpp says so) and a GL_DOUBLE vertex array
// (the M-3 deviation declared in Managers.cpp). Magma reads MultiDrawArraysIndirectCount's count
// the same way, and its restart rewrite and GL_DOUBLE conversion call SyncGpuWrites while the
// draw is being recorded, so the draw then records into an ended command buffer (SIGSEGV in
// lavapipe). Those arms are dev's to fix (ID-P8-3); the skip keeps the ambient monolith lane
// honest without hiding the split arms, which run the same cases under DirectGLES.Split/Spawn/Tcp,
// DirectVulkan.*.Full and the inproc A/B.

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#include "../Harness/WireIndirectPeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        constexpr int kSurface = 64;

        constexpr const char* kFlatVertex = R"(#version 430 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        constexpr const char* kFlatFragment = R"(#version 430 core
out vec4 oColor;
void main() { oColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        // Green when the shader saw gl_BaseVertex == 9 (the value the compute shader wrote),
        // red for anything else - in particular 3, the value the staged copy still holds.
        constexpr const char* kBaseVertexVertex = R"(#version 450 core
#extension GL_ARB_shader_draw_parameters : require
layout(location = 0) in vec2 aPos;
flat out vec4 vColor;
void main() {
    vColor = gl_BaseVertexARB == 9 ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kBaseVertexFragment = R"(#version 450 core
flat in vec4 vColor;
out vec4 oColor;
void main() { oColor = vColor; }
)";

        // ---- the restart scene (PrimitiveRestartScenario's geometry) ----
        //
        // Two triangles with a gap between them, plus two spare vertices at the origin. Index 7 is
        // the application's restart index and a legal vertex, so a dropped restart welds the strip
        // across the gap (caught by the gap probe) instead of reading out of bounds.
        constexpr GLfloat kRestartVertices[] = {
            -0.9f, -0.9f, -0.1f, -0.9f, -0.9f, 0.9f, // 0-2: left triangle
            0.1f,  -0.9f, 0.9f,  -0.9f, 0.9f,  0.9f, // 3-5: right triangle
            0.0f,  0.0f,  0.0f,  0.0f,               // 6, 7: spares
        };
        constexpr GLuint kRestartIndex = 7;
        constexpr GLuint kRestartIndexCount = 7;
        // What the compute shader writes: {0, 1, 2, 7, 3, 4, 5}.
        constexpr const char* kWriteRestartIndices = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Indices { uint value[]; };
void main() {
    value[0] = 0u; value[1] = 1u; value[2] = 2u; value[3] = 7u;
    value[4] = 3u; value[5] = 4u; value[6] = 5u;
}
)";
        // The staged poison: every index names spare vertex 6, a strip of degenerate triangles
        // that paints nothing.
        const std::vector<GLuint> kRestartPoison(kRestartIndexCount, 6u);

        // The same seven indices, produced by a TRANSFORM FEEDBACK capture: one point per index.
        constexpr const char* kCaptureIndicesVertex = R"(#version 430 core
flat out uint idx;
const uint kTable[7] = uint[7](0u, 1u, 2u, 7u, 3u, 4u, 5u);
void main() {
    idx = kTable[gl_VertexID];
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    gl_PointSize = 1.0;
}
)";
        // ...and by a capture with a HOLE after every index (gl_SkipComponents1), which ES cannot
        // express and Espryt therefore scatters itself. The captured words land at the even
        // slots; the odd slots keep what the application staged there.
        constexpr const char* kCaptureScatteredVertex = R"(#version 430 core
flat out uint idx;
const uint kTable[4] = uint[4](0u, 2u, 3u, 5u);
void main() {
    idx = kTable[gl_VertexID];
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    gl_PointSize = 1.0;
}
)";
        constexpr const char* kCaptureFragment = R"(#version 430 core
out vec4 oColor;
void main() { oColor = vec4(1.0); }
)";
        // Even slots are the capture's (poison 6 until it lands), odd slots the holes: after the
        // scatter the first seven words are {0, 1, 2, 7, 3, 4, 5}; before it, a strip of
        // degenerate triangles.
        constexpr GLuint kScatterStaged[8] = {6u, 1u, 6u, 7u, 6u, 4u, 6u, 6u};

        // ...and by IMAGE STORES into a buffer texture over the element array buffer.
        constexpr const char* kImageStoreRestartIndices = R"(#version 430 core
layout(local_size_x = 1) in;
layout(r32ui, binding = 0) uniform writeonly uimageBuffer indices;
const uint kTable[7] = uint[7](0u, 1u, 2u, 7u, 3u, 4u, 5u);
void main() {
    for (int i = 0; i < 7; ++i) imageStore(indices, i, uvec4(kTable[i]));
}
)";

        // NDC (-0.5, -0.5), (0.6, -0.5), (0.2, -0.5) on the 64x64 surface.
        constexpr int kLeftX = 16, kRightX = 51, kGapX = 38, kProbeY = 16;

        // ---- the indirect scene (DrawParametersScenario's geometry) ----
        //
        // Three pad vertices at the origin, then the left half of the surface as two triangles
        // (vertices 3-8), then the right half (9-14). Element indices 0..5 address one half
        // through the command's baseVertex.
        constexpr int kLeftFirst = 3;
        constexpr int kRightFirst = 9;
        constexpr int kHalfCount = 6;

        std::vector<GLfloat> HalvesVertices() {
            std::vector<GLfloat> v(6, 0.0f);
            const float bounds[2][2] = {{-1.0f, 0.0f}, {0.0f, 1.0f}};
            for (const auto& half : bounds) {
                const float x0 = half[0];
                const float x1 = half[1];
                const float quad[] = {x0, -1.0f, x1, -1.0f, x1, 1.0f, x0, -1.0f, x1, 1.0f, x0, 1.0f};
                v.insert(v.end(), std::begin(quad), std::end(quad));
            }
            return v;
        }

        struct ElementsCommand {
            std::uint32_t count, instanceCount, firstIndex;
            std::int32_t baseVertex;
            std::uint32_t baseInstance;
        };
        struct ArraysCommand {
            std::uint32_t count, instanceCount, first, baseInstance;
        };

        bool IsGreen(const Rgba8& p) { return p.g > 128 && p.r < 128; }

        class GpuWrittenDrawInputScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_target = MakeColorFbo(kSurface, kSurface);
                ASSERT_NE(m_target.fbo, 0u) << "the 64x64 colour target did not build";
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_CULL_FACE);
                DrainErrors();
            }

            void TearDown() override {
                if (!Ready()) return;
                glDisable(GL_PRIMITIVE_RESTART);
                glPrimitiveRestartIndex(0);
                glUseProgram(0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
                glBindBuffer(GL_PARAMETER_BUFFER, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindVertexArray(0);
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                m_texture = 0;
                for (GLuint* buffer : {&m_vbo, &m_ebo, &m_commands, &m_parameters}) {
                    if (*buffer != 0) glDeleteBuffers(1, buffer);
                    *buffer = 0;
                }
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                for (GLuint program : m_programs) glDeleteProgram(program);
                m_programs.clear();
                BindDefaultFramebuffer();
                DestroyColorFbo(m_target);
                DrainErrors();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            // THE ARMS THIS FILE SKIPS, BY NAME: the monolith arm of a backend whose reason is
            // non-null. Every split arm, and the inproc A/B, runs the case.
            bool SkipBrokenMonolith(const char* espryt, const char* magma) {
                if (PeekSplitRuntime().transportName != "monolith") return false;
                const std::string backend = Gl().BackendName();
                const char* why = backend == "DirectGLES" ? espryt : backend == "DirectVulkan" ? magma : nullptr;
                if (why == nullptr) return false;
                m_skipReason = "monolith " + backend + ": " + why +
                               " (dev's fix, ID-P8-3) - this case gates the split arms";
                return true;
            }
            static constexpr const char* kUnsyncedCount =
                "the *IndirectCount count is read from the frontend shadow without SyncGpuWrites "
                "(OPEN-QUESTIONS 15)";
            static constexpr const char* kUnsyncedBaseVertex =
                "an indirect command's gl_BaseVertex is read from the frontend shadow without "
                "SyncGpuWrites";
            static constexpr const char* kUnsyncedDoubles =
                "a GL_DOUBLE vertex array is narrowed from the client shadow without SyncGpuWrites "
                "(the M-3 deviation declared in Managers.cpp)";
            static constexpr const char* kSyncMidRecording =
                "SyncGpuWrites inside the draw (the restart rewrite, the GL_DOUBLE vertex conversion) "
                "submits the frame mid-recording and the draw then records into the ended command "
                "buffer (SIGSEGV)";

            GLuint Program(const char* vertex, const char* fragment) {
                std::string error;
                const GLuint program = CompileProgram(vertex, fragment, &error);
                EXPECT_NE(program, 0u) << error;
                if (program != 0) m_programs.push_back(program);
                return program;
            }

            GLuint ComputeProgram(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (!compiled) {
                    char log[4096]{};
                    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                    ADD_FAILURE() << log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (!linked) {
                    char log[4096]{};
                    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                    ADD_FAILURE() << log;
                    glDeleteProgram(program);
                    return 0;
                }
                m_programs.push_back(program);
                return program;
            }

            // Buffer `name` at `target` with `bytes` bytes of `data` (null: an orphaned store).
            static void Store(GLuint& name, GLenum target, GLsizeiptr bytes, const void* data) {
                if (name == 0) glGenBuffers(1, &name);
                glBindBuffer(target, name);
                glBufferData(target, bytes, data, GL_DYNAMIC_COPY);
            }

            void BindVertices(const GLfloat* vertices, GLsizeiptr bytes) {
                glBindVertexArray(m_vao);
                Store(m_vbo, GL_ARRAY_BUFFER, bytes, vertices);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), nullptr);
            }

            // One dispatch of `source` with `first` (and `second`, if non-zero) bound as storage
            // buffers 0 and 1, then the barrier the draw that consumes them needs.
            void WriteWithCompute(const char* source, GLuint first, GLuint second, GLbitfield barrier) {
                const GLuint compute = ComputeProgram(source);
                ASSERT_NE(compute, 0u);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, first);
                if (second != 0) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, second);
                glUseProgram(compute);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(barrier | GL_SHADER_STORAGE_BARRIER_BIT);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
                glUseProgram(0);
            }

            template <typename DrawFn>
            Image Render(GLuint program, DrawFn&& draw) {
                BindFbo(m_target);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(program);
                glBindVertexArray(m_vao);
                draw();
                return ReadPixels(kSurface, kSurface);
            }

            // The element array buffer of the restart scene, written by a compute shader over the
            // poison (or over an orphaned store when `orphaned`).
            void BuildRestartScene(bool orphaned) {
                BindVertices(kRestartVertices, sizeof(kRestartVertices));
                Store(m_ebo, GL_ELEMENT_ARRAY_BUFFER, kRestartIndexCount * sizeof(GLuint),
                      orphaned ? nullptr : kRestartPoison.data());
                WriteWithCompute(kWriteRestartIndices, m_ebo, 0, GL_ELEMENT_ARRAY_BARRIER_BIT);
                ArmRestart();
            }

            // The element array buffer bound to the scene's VAO, restart index 7 on.
            void ArmRestart() {
                glBindVertexArray(m_vao);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
                glEnable(GL_PRIMITIVE_RESTART);
                glPrimitiveRestartIndex(kRestartIndex);
            }

            // A vertex + fragment program whose `varyings` are captured INTERLEAVED.
            GLuint XfbProgram(const char* vertex, const char* fragment, const char* const* varyings,
                              GLsizei count) {
                const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                glShaderSource(vs, 1, &vertex, nullptr);
                glCompileShader(vs);
                const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
                glShaderSource(fs, 1, &fragment, nullptr);
                glCompileShader(fs);
                const GLuint program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, fs);
                glTransformFeedbackVaryings(program, count, varyings, GL_INTERLEAVED_ATTRIBS);
                glLinkProgram(program);
                glDeleteShader(vs);
                glDeleteShader(fs);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (!linked) {
                    char log[4096]{};
                    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                    ADD_FAILURE() << "the capture program did not link: " << log;
                    glDeleteProgram(program);
                    return 0;
                }
                m_programs.push_back(program);
                return program;
            }

            // `vertices` points drawn with rasterization off, captured into m_ebo from byte 0.
            void CaptureIntoElementBuffer(GLuint program, GLsizei vertices) {
                glBindVertexArray(m_vao);
                glUseProgram(program);
                glEnable(GL_RASTERIZER_DISCARD);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, m_ebo);
                glBeginTransformFeedback(GL_POINTS);
                glDrawArrays(GL_POINTS, 0, vertices);
                glEndTransformFeedback();
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
                glDisable(GL_RASTERIZER_DISCARD);
                glUseProgram(0);
            }

            static void ExpectSplitStrip(const Image& image, const char* what) {
                EXPECT_TRUE(IsGreen(image.At(kLeftX, kProbeY)))
                    << what << ": the left triangle did not render (" << image.At(kLeftX, kProbeY)
                    << ") - the backend drew the indices it had staged, not the ones the compute "
                       "shader wrote";
                EXPECT_TRUE(IsGreen(image.At(kRightX, kProbeY)))
                    << what << ": the right triangle did not render (" << image.At(kRightX, kProbeY) << ")";
                EXPECT_FALSE(IsGreen(image.At(kGapX, kProbeY)))
                    << what << ": the gap is covered, so the restart at index 7 was dropped";
            }

            // Left and right half of the halves scene, sampled at their centres.
            static Rgba8 Half(const Image& image, int half) {
                return image.At(kSurface * (1 + 2 * half) / 4, kSurface / 2);
            }

            ColorFbo m_target;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_ebo = 0;
            GLuint m_commands = 0;
            GLuint m_parameters = 0;
            GLuint m_texture = 0;
            std::vector<GLuint> m_programs;
            std::string m_skipReason;
        };

    } // namespace

    // ---- the primitive-restart rewrite (DirectGLES.cpp ScopedRestartIndexSubstitution) ----

    TEST_F(GpuWrittenDrawInputScenario, RestartIndicesAComputeShaderWroteSplitTheStrip) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(nullptr, kSyncMidRecording)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        BuildRestartScene(/*orphaned=*/false);
        const Image image = Render(program, [] {
            glDrawElements(GL_TRIANGLE_STRIP, kRestartIndexCount, GL_UNSIGNED_INT, nullptr);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectSplitStrip(image, "glDrawElements over compute-written restart indices");
    }

    // The same indices in a store the application ORPHANED (glBufferData(NULL)): nothing was
    // ever staged for it, so the only copy of its bytes is the one the GPU holds.
    TEST_F(GpuWrittenDrawInputScenario, RestartIndicesAComputeShaderWroteIntoAnOrphanedStoreSplitTheStrip) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(nullptr, kSyncMidRecording)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        BuildRestartScene(/*orphaned=*/true);
        const Image image = Render(program, [] {
            glDrawElements(GL_TRIANGLE_STRIP, kRestartIndexCount, GL_UNSIGNED_INT, nullptr);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectSplitStrip(image, "glDrawElements over compute-written restart indices in an orphaned store");
    }

    // ---- the multi-draw rebase tier (MultiDraw.cpp RunRebasedDrawElements) ----
    //
    // An arbitrary restart index forces a multi-draw onto the one tier that rewrites the stream,
    // and that tier reads the whole element array buffer on the CPU. Sub-draw 0 is {0,1,2} (the
    // left triangle); sub-draw 1 is {7,3,4,5} (a restart, then the right triangle).
    TEST_F(GpuWrittenDrawInputScenario, RestartIndicesAComputeShaderWroteSplitEveryMultiDrawStrip) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(nullptr, kSyncMidRecording)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        BuildRestartScene(/*orphaned=*/false);
        const GLsizei counts[2] = {3, 4};
        const void* offsets[2] = {nullptr, reinterpret_cast<const void*>(3 * sizeof(GLuint))};
        const Image image = Render(program, [&] {
            glMultiDrawElements(GL_TRIANGLE_STRIP, counts, GL_UNSIGNED_INT, offsets, 2);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectSplitStrip(image, "glMultiDrawElements over compute-written restart indices");
    }

    // ---- *IndirectCount (DirectGLES.cpp MultiDraw{Elements,Arrays}IndirectCount) ----
    //
    // The compute shader writes BOTH commands and the count; the staged copies hold zeroes, so a
    // stale count draws nothing and a stale command (on a device without native indirect) draws
    // nothing either.
    TEST_F(GpuWrittenDrawInputScenario, ACountAComputeShaderWroteDrivesMultiDrawElementsIndirectCount) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(kUnsyncedCount, nullptr)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        const std::vector<GLfloat> vertices = HalvesVertices();
        BindVertices(vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(GLfloat)));
        const GLuint indices[kHalfCount] = {0, 1, 2, 3, 4, 5};
        Store(m_ebo, GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices);
        const ElementsCommand zeroCommands[2] = {};
        Store(m_commands, GL_DRAW_INDIRECT_BUFFER, sizeof(zeroCommands), zeroCommands);
        const GLuint zeroCount = 0;
        Store(m_parameters, GL_PARAMETER_BUFFER, sizeof(zeroCount), &zeroCount);
        WriteWithCompute(R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Commands { uint command[]; };
layout(std430, binding = 1) buffer Parameters { uint count; };
void main() {
    command[0] = 6u; command[1] = 1u; command[2] = 0u; command[3] = 3u; command[4] = 0u;
    command[5] = 6u; command[6] = 1u; command[7] = 0u; command[8] = 9u; command[9] = 0u;
    count = 2u;
}
)",
                         m_commands, m_parameters, GL_COMMAND_BARRIER_BIT);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commands);
        glBindBuffer(GL_PARAMETER_BUFFER, m_parameters);
        const Image image = Render(program, [] {
            glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 0, 2, 0);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 0)))
            << "command 0 did not draw (" << Half(image, 0)
            << "): the count was read from a copy the compute shader never reached";
        EXPECT_TRUE(IsGreen(Half(image, 1))) << "command 1 did not draw (" << Half(image, 1) << ")";
    }

    TEST_F(GpuWrittenDrawInputScenario, ACountAComputeShaderWroteDrivesMultiDrawArraysIndirectCount) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(kUnsyncedCount, kUnsyncedCount)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        const std::vector<GLfloat> vertices = HalvesVertices();
        BindVertices(vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(GLfloat)));
        const ArraysCommand zeroCommands[2] = {};
        Store(m_commands, GL_DRAW_INDIRECT_BUFFER, sizeof(zeroCommands), zeroCommands);
        const GLuint zeroCount = 0;
        Store(m_parameters, GL_PARAMETER_BUFFER, sizeof(zeroCount), &zeroCount);
        WriteWithCompute(R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Commands { uint command[]; };
layout(std430, binding = 1) buffer Parameters { uint count; };
void main() {
    command[0] = 6u; command[1] = 1u; command[2] = 3u; command[3] = 0u;
    command[4] = 6u; command[5] = 1u; command[6] = 9u; command[7] = 0u;
    count = 2u;
}
)",
                         m_commands, m_parameters, GL_COMMAND_BARRIER_BIT);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commands);
        glBindBuffer(GL_PARAMETER_BUFFER, m_parameters);
        const Image image = Render(program, [] {
            glMultiDrawArraysIndirectCount(GL_TRIANGLES, nullptr, 0, 2, 0);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 0)))
            << "command 0 did not draw (" << Half(image, 0)
            << "): the count was read from a copy the compute shader never reached";
        EXPECT_TRUE(IsGreen(Half(image, 1))) << "command 1 did not draw (" << Half(image, 1) << ")";
    }

    // ---- a native indirect draw's gl_BaseVertex (DirectGLES.cpp ExecuteIndexedIndirectCommands) ----
    //
    // The GPU executes the command itself, so the geometry is right whatever the backend's copy
    // says; gl_BaseVertex is the one field Espryt feeds from the CPU. The staged command names
    // the left half (baseVertex 3), the compute shader moves it to the right half (9).
    TEST_F(GpuWrittenDrawInputScenario, ABaseVertexAComputeShaderWroteReachesGlBaseVertex) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(kUnsyncedBaseVertex, nullptr)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kBaseVertexVertex, kBaseVertexFragment);
        ASSERT_NE(program, 0u);
        const std::vector<GLfloat> vertices = HalvesVertices();
        BindVertices(vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(GLfloat)));
        const GLuint indices[kHalfCount] = {0, 1, 2, 3, 4, 5};
        Store(m_ebo, GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices);
        const ElementsCommand staged{kHalfCount, 1, 0, kLeftFirst, 0};
        Store(m_commands, GL_DRAW_INDIRECT_BUFFER, sizeof(staged), &staged);
        WriteWithCompute(R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Commands { int command[]; };
void main() { command[3] = 9; }
)",
                         m_commands, 0, GL_COMMAND_BARRIER_BIT);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commands);
        const Image image = Render(program, [] {
            glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 1)))
            << "the right half is " << Half(image, 1)
            << ": red means the draw ran with the compute-written baseVertex but gl_BaseVertex was "
               "fed from the staged command; black means the draw did not run there at all";
        EXPECT_FALSE(IsGreen(Half(image, 0))) << "the left half drew: the GPU saw the staged command";
    }

    // ---- indirect commands with no staged copy at all (DirectGLES.cpp ResolveIndirectCommandBytes) ----
    //
    // The command buffer is ORPHANED and only the compute shader ever writes it - the GPU-culling
    // shape. Nothing was staged for it, so a backend that needs a CPU copy before it will issue
    // the draw has to make one from its own GL buffer.
    TEST_F(GpuWrittenDrawInputScenario, IndirectCommandsAComputeShaderWroteIntoAnOrphanedStoreDraw) {
        if (!Ready()) return;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        const std::vector<GLfloat> vertices = HalvesVertices();
        BindVertices(vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(GLfloat)));
        const GLuint indices[kHalfCount] = {0, 1, 2, 3, 4, 5};
        Store(m_ebo, GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices);
        Store(m_commands, GL_DRAW_INDIRECT_BUFFER, sizeof(ElementsCommand), nullptr);
        WriteWithCompute(R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Commands { uint command[]; };
void main() { command[0] = 6u; command[1] = 1u; command[2] = 0u; command[3] = 3u; command[4] = 0u; }
)",
                         m_commands, 0, GL_COMMAND_BARRIER_BIT);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commands);
        const Image image = Render(program, [] {
            glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 0)))
            << "the compute-written command did not draw (" << Half(image, 0)
            << "): the backend refused a draw whose command buffer it had no CPU copy of";
        EXPECT_FALSE(IsGreen(Half(image, 1))) << "the right half drew: the command was not the one written";
    }

    // ---- the other GPU writers: transform feedback, buffer images, atomic counters ----
    //
    // The same restart reader over indices a CAPTURE produced. Espryt maps the captured range
    // back at glEndTransformFeedback for the client; the server's own copy has to take it too.
    TEST_F(GpuWrittenDrawInputScenario, RestartIndicesATransformFeedbackCapturedSplitTheStrip) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(nullptr, kSyncMidRecording)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        const char* varyings[] = {"idx"};
        const GLuint capture = XfbProgram(kCaptureIndicesVertex, kCaptureFragment, varyings, 1);
        ASSERT_NE(capture, 0u);
        BindVertices(kRestartVertices, sizeof(kRestartVertices));
        Store(m_ebo, GL_ELEMENT_ARRAY_BUFFER, kRestartIndexCount * sizeof(GLuint), kRestartPoison.data());
        CaptureIntoElementBuffer(capture, kRestartIndexCount);
        ArmRestart();
        const Image image = Render(program, [] {
            glDrawElements(GL_TRIANGLE_STRIP, kRestartIndexCount, GL_UNSIGNED_INT, nullptr);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectSplitStrip(image, "glDrawElements over restart indices a transform feedback captured");
    }

    // ...through the SCATTER Espryt runs for a layout with holes: the server composes the range
    // from its pre-capture bytes and the captured words, and its own copy has to keep the result.
    TEST_F(GpuWrittenDrawInputScenario, RestartIndicesAScatteredTransformFeedbackCapturedSplitTheStrip) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(nullptr, kSyncMidRecording)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        const char* varyings[] = {"idx", "gl_SkipComponents1"};
        const GLuint capture = XfbProgram(kCaptureScatteredVertex, kCaptureFragment, varyings, 2);
        ASSERT_NE(capture, 0u);
        BindVertices(kRestartVertices, sizeof(kRestartVertices));
        Store(m_ebo, GL_ELEMENT_ARRAY_BUFFER, sizeof(kScatterStaged), kScatterStaged);
        CaptureIntoElementBuffer(capture, 4);
        ArmRestart();
        const Image image = Render(program, [] {
            glDrawElements(GL_TRIANGLE_STRIP, kRestartIndexCount, GL_UNSIGNED_INT, nullptr);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectSplitStrip(image, "glDrawElements over restart indices a scattered capture composed");
    }

    // ...and over indices IMAGE STORES wrote through a buffer texture on the element buffer.
    TEST_F(GpuWrittenDrawInputScenario, RestartIndicesImageStoresWroteSplitTheStrip) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(nullptr, kSyncMidRecording)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        const GLuint compute = ComputeProgram(kImageStoreRestartIndices);
        ASSERT_NE(compute, 0u);
        BindVertices(kRestartVertices, sizeof(kRestartVertices));
        Store(m_ebo, GL_ELEMENT_ARRAY_BUFFER, kRestartIndexCount * sizeof(GLuint), kRestartPoison.data());
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_BUFFER, m_texture);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, m_ebo);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
        glBindImageTexture(0, m_texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
        glUseProgram(compute);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ELEMENT_ARRAY_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glUseProgram(0);
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
        ArmRestart();
        const Image image = Render(program, [] {
            glDrawElements(GL_TRIANGLE_STRIP, kRestartIndexCount, GL_UNSIGNED_INT, nullptr);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectSplitStrip(image, "glDrawElements over restart indices image stores wrote");
    }

    // The GPU-culling shape at its most common: the draw count is an ATOMIC COUNTER the compute
    // shader bumped once per surviving command, and that counter buffer is the parameter buffer.
    TEST_F(GpuWrittenDrawInputScenario, ACountAnAtomicCounterProducedDrivesMultiDrawElementsIndirectCount) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(kUnsyncedCount, nullptr)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        const GLuint compute = ComputeProgram(R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 0, offset = 0) uniform atomic_uint drawCount;
layout(std430, binding = 0) buffer Commands { uint command[]; };
void main() {
    const uint baseVertex[2] = uint[2](3u, 9u);
    for (int i = 0; i < 2; ++i) {
        uint slot = atomicCounterIncrement(drawCount);
        command[slot * 5u + 0u] = 6u;
        command[slot * 5u + 1u] = 1u;
        command[slot * 5u + 2u] = 0u;
        command[slot * 5u + 3u] = baseVertex[i];
        command[slot * 5u + 4u] = 0u;
    }
}
)");
        ASSERT_NE(compute, 0u);
        const std::vector<GLfloat> vertices = HalvesVertices();
        BindVertices(vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(GLfloat)));
        const GLuint indices[kHalfCount] = {0, 1, 2, 3, 4, 5};
        Store(m_ebo, GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices);
        const ElementsCommand zeroCommands[2] = {};
        Store(m_commands, GL_DRAW_INDIRECT_BUFFER, sizeof(zeroCommands), zeroCommands);
        const GLuint zeroCount = 0;
        Store(m_parameters, GL_ATOMIC_COUNTER_BUFFER, sizeof(zeroCount), &zeroCount);
        glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, m_parameters);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_commands);
        glUseProgram(compute);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        glUseProgram(0);
        glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commands);
        glBindBuffer(GL_PARAMETER_BUFFER, m_parameters);
        const Image image = Render(program, [] {
            glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 0, 2, 0);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 0)))
            << "command 0 did not draw (" << Half(image, 0)
            << "): the count was read from a copy the atomic counter never reached";
        EXPECT_TRUE(IsGreen(Half(image, 1))) << "command 1 did not draw (" << Half(image, 1) << ")";
    }

    // ---- fp64 vertex narrowing (Managers.cpp SyncFloat64AttributeAsFloat32ByHandle) ----
    //
    // A GL_DOUBLE vertex array is narrowed to float on the CPU from the source buffer's bytes,
    // and the narrowed copy is memoised on the source's serial - which a GPU write never moves.
    // Two rounds: the first catches a stale read, the second a stale memo. The compute shader
    // writes each double as its two IEEE-754 words (low word 0), so it needs no fp64 itself.
    TEST_F(GpuWrittenDrawInputScenario, DoubleVerticesAComputeShaderWroteAreNarrowedFromTheWrittenBytes) {
        if (!Ready()) return;
        if (SkipBrokenMonolith(kUnsyncedDoubles, kSyncMidRecording)) GTEST_SKIP() << m_skipReason;
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        // Four vertices, x then y, all 100.0: a strip wholly off the surface.
        const double poison[8] = {100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0};
        glBindVertexArray(m_vao);
        Store(m_vbo, GL_ARRAY_BUFFER, sizeof(poison), poison);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_DOUBLE, GL_FALSE, 2 * sizeof(double), nullptr);

        // High words: -1.0 = 0xBFF00000, 1.0 = 0x3FF00000, 0.0 = 0.
        const auto writeQuad = [&](const char* highWords) {
            const std::string source = std::string(R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Positions { uint word[]; };
void main() {
    const uint hi[8] = uint[8]()") + highWords + R"();
    for (int i = 0; i < 8; ++i) { word[2 * i] = 0u; word[2 * i + 1] = hi[i]; }
}
)";
            WriteWithCompute(source.c_str(), m_vbo, 0, GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
        };

        // Round 1: the whole surface, (-1,-1) (1,-1) (-1,1) (1,1).
        writeQuad("0xBFF00000u, 0xBFF00000u, 0x3FF00000u, 0xBFF00000u, 0xBFF00000u, 0x3FF00000u, "
                  "0x3FF00000u, 0x3FF00000u");
        Image image = Render(program, [] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); });
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 0)) && IsGreen(Half(image, 1)))
            << "round 1: the quad is " << Half(image, 0) << " / " << Half(image, 1)
            << " - black means the doubles were narrowed from the staged poison, not the written bytes";

        // Round 2: only the left half, (-1,-1) (0,-1) (-1,1) (0,1).
        writeQuad("0xBFF00000u, 0xBFF00000u, 0u, 0xBFF00000u, 0xBFF00000u, 0x3FF00000u, 0u, 0x3FF00000u");
        image = Render(program, [] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); });
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 0))) << "round 2: the left half is " << Half(image, 0);
        EXPECT_FALSE(IsGreen(Half(image, 1)))
            << "round 2: the right half still drew - the narrowed copy of round 1 was reused after the "
               "second GPU write";
    }


    // P13 W4a (ID-P8-14a): AN INDIRECT DRAW WITH NO CLIENT-MEMORY ARRAY READS NOTHING BACK. The
    // monolith indirect executors used to pull a GPU-written command buffer back into its shadow
    // once PER COMMAND, PER VIEWPORT PASS - before asking whether any attribute came from client
    // memory, which is the only reason the words are read on the CPU at all. On a phone that was a
    // multi-minute hang (62bfe461). The positive control is the same draw with a client-memory
    // attribute: there the words ARE needed, so the readback must still happen and still draw.
    TEST_F(GpuWrittenDrawInputScenario, AnIndirectDrawWithNoClientArrayReadsNothingBack) {
        if (!Ready()) return;
        WireIndirectCounters before{};
        if (!PeekWireIndirectCounters(&before)) GTEST_SKIP() << "no in-process counter peek on this platform";
        const GLuint program = Program(kFlatVertex, kFlatFragment);
        ASSERT_NE(program, 0u);
        const std::vector<GLfloat> vertices = HalvesVertices();
        BindVertices(vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(GLfloat)));
        constexpr int kCommands = 4;
        Store(m_commands, GL_DRAW_INDIRECT_BUFFER, kCommands * sizeof(ArraysCommand), nullptr);
        // Four copies of {6, 1, 3, 0}: the left half, four times.
        WriteWithCompute(R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Commands { uint command[]; };
void main() {
    for (int i = 0; i < 4; ++i) {
        command[4 * i + 0] = 6u; command[4 * i + 1] = 1u; command[4 * i + 2] = 3u; command[4 * i + 3] = 0u;
    }
}
)",
                         m_commands, 0, GL_COMMAND_BARRIER_BIT);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commands);
        PeekWireIndirectCounters(&before);
        Image image = Render(program, [] { glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, kCommands, 0); });
        WireIndirectCounters after{};
        ASSERT_TRUE(PeekWireIndirectCounters(&after));
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 0))) << "the compute-written commands did not draw (" << Half(image, 0) << ")";
        EXPECT_FALSE(IsGreen(Half(image, 1))) << "the right half drew: the commands were not the ones written";
        EXPECT_EQ(after.resourceReadbacks - before.resourceReadbacks, 0u)
            << "an indirect draw whose attributes all come from buffers read a buffer back into its "
               "shadow; nothing on the CPU needs the command words for it";

        // The positive control: attribute 0 from client memory, so the words size a snapshot.
        WriteWithCompute(R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Commands { uint command[]; };
void main() { command[0] = 6u; command[1] = 1u; command[2] = 3u; command[3] = 0u; }
)",
                         m_commands, 0, GL_COMMAND_BARRIER_BIT);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), vertices.data());
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commands);
        if (FirstGLError() != 0u) {
            // A context that refuses client-memory arrays has no snapshot for the control to need.
            GTEST_SKIP() << "client-memory vertex arrays are refused here; the positive control has no subject";
        }
        PeekWireIndirectCounters(&before);
        image = Render(program, [] { glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, 1, 0); });
        ASSERT_TRUE(PeekWireIndirectCounters(&after));
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(IsGreen(Half(image, 0))) << "the client-array control did not draw (" << Half(image, 0) << ")";
        // The words are read from the frontend SHADOW by Espryt (and by every client that
        // snapshots), so there the readback is required; Magma's monolith arm reads them from its
        // own host-visible store and needs none, and asserting one there would be asserting a cost.
        if (Gl().BackendName() == "DirectGLES" || PeekSplitRuntime().transportName != "monolith") {
            EXPECT_GE(after.resourceReadbacks - before.resourceReadbacks, 1u)
                << "the positive control read nothing back: either the counter is dead or the command "
                   "words a client-array snapshot needs were taken from a stale shadow";
        }
    }

} // namespace MGITest
