// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/WireIndirectDrawScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P8-D: THE MAGMA WIRE ARM ISSUES INDIRECT DRAWS FROM THE WIRE STORE, AS THE MONOLITH ARM DOES.
//
// Before P8-D every wire indirect draw read its commands (and its count word) through
// ReadWireBuffer and expanded them on the CPU, and a store a shader had written turned that read
// into a whole-GPU wait per draw (the create-indirect trace, now
// minecraft-1.21.1-neoforge-create-indirect-in-world-align1024: 321 expansions, 327 waits, ~4-8 s of
// waiting per replay on lavapipe). The cases pin both halves of the change:
//   * PIXELS, on every arm and both backends where the backend's own semantics allow it - each
//     indirect form draws exactly the strips its commands name, and GPU-written commands are the
//     ones the draw sees (the poison the buffers start with names the OTHER strips);
//   * THE SERVER'S OWN COUNTERS (Harness/WireIndirectPeek.h), where they can be read - Magma on
//     the inproc arm, whose server is this process: every call issued natively, no CPU
//     expansion, no host wait, and the INDIRECT_COMMAND_READ barrier exactly once per write.
//
// Four strips across the viewport, one per quarter; a command draws one strip. Green = drawn,
// red = the clear. The VBO opens with three padding vertices so a command that loses its `first`
// or `firstIndex` draws a degenerate triangle rather than the right strip.

#include <array>
#include <cstdint>
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
#include <GL/glext.h>

namespace MGITest {
    namespace {

        constexpr int kStrips = 4;
        constexpr std::uint32_t kPad = 3;
        constexpr std::uint32_t kStripVertices = 6;

        constexpr const char* kVertexSource = R"(#version 430 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";
        constexpr const char* kFragmentSource = R"(#version 430 core
out vec4 oColor;
void main() { oColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";
        // Copies `source` into `words`, one word per invocation: the compute write under test.
        constexpr const char* kCopySource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Words { uint words[]; };
layout(std430, binding = 1) readonly buffer Source { uint source[]; };
void main() { words[gl_GlobalInvocationID.x] = source[gl_GlobalInvocationID.x]; }
)";

        struct ArraysCommand {
            std::uint32_t count, instanceCount, first, baseInstance;
        };
        struct ElementsCommand {
            std::uint32_t count, instanceCount, firstIndex;
            std::int32_t baseVertex;
            std::uint32_t baseInstance;
        };
        // A padded elements command, for the stride case: 32 bytes, the tail unread.
        struct PaddedElementsCommand {
            ElementsCommand command;
            std::uint32_t padding[3];
        };

        ArraysCommand Arrays(int strip) { return {kStripVertices, 1, kPad + kStripVertices * strip, 0}; }
        ElementsCommand Elements(int strip) { return {kStripVertices, 1, kStripVertices * strip, 0, 0}; }

        using Strips = std::array<bool, kStrips>;

        class WireIndirectDrawScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_magma = Gl().BackendName() == "DirectVulkan";
                m_transport = PeekSplitRuntime().transportName;
                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;
                m_copy = CompileCompute(kCopySource);
                ASSERT_NE(m_copy, 0u);

                std::vector<float> vertices(2 * kPad, 0.0f);
                for (int strip = 0; strip < kStrips; ++strip) {
                    const float x0 = -1.0f + 0.5f * static_cast<float>(strip);
                    const float x1 = x0 + 0.5f;
                    const float quad[12] = {x0, -1.0f, x1, -1.0f, x1, 1.0f, x0, -1.0f, x1, 1.0f, x0, 1.0f};
                    vertices.insert(vertices.end(), quad, quad + 12);
                }
                std::vector<std::uint32_t> indices;
                for (std::uint32_t i = 0; i < kStripVertices * kStrips; ++i) indices.push_back(kPad + i);

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                             vertices.data(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
                glGenBuffers(1, &m_ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
                             indices.data(), GL_STATIC_DRAW);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";

                // The counters are armed by the first peek, so it has to come before any draw.
                if (CountersReadable()) {
                    ASSERT_TRUE(PeekWireIndirectCounters(&m_before))
                        << "Magma on the inproc arm must be able to read the wire arm's own counters";
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindVertexArray(0);
                for (GLuint* buffer : {&m_vbo, &m_ebo, &m_indirect, &m_parameter, &m_source}) {
                    if (*buffer != 0) glDeleteBuffers(1, buffer);
                    *buffer = 0;
                }
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_copy != 0) glDeleteProgram(m_copy);
            }

            static GLuint CompileCompute(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint ok = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
                if (ok != GL_TRUE) {
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                glGetProgramiv(program, GL_LINK_STATUS, &ok);
                if (ok != GL_TRUE) {
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            template <typename T>
            static void Fill(GLuint& name, GLenum target, const std::vector<T>& data) {
                if (name == 0) glGenBuffers(1, &name);
                glBindBuffer(target, name);
                glBufferData(target, static_cast<GLsizeiptr>(data.size() * sizeof(T)), data.data(), GL_DYNAMIC_COPY);
            }

            // The compute write: `words` (already holding its poison) receives `data` from the GPU.
            template <typename T>
            void ComputeWrite(GLuint words, const std::vector<T>& data) {
                Fill(m_source, GL_SHADER_STORAGE_BUFFER, data);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, words);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_source);
                glUseProgram(m_copy);
                glDispatchCompute(static_cast<GLuint>(data.size() * sizeof(T) / sizeof(std::uint32_t)), 1, 1);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
            }

            template <typename DrawFn>
            Image Render(DrawFn&& draw) {
                BindDefaultFramebuffer();
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glDisable(GL_DEPTH_TEST);
                ClearTo(1.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirect);
                draw();
                return ReadPixels(Gl().Width(), Gl().Height());
            }

            static void ExpectStrips(const Image& image, const Strips& drawn, const std::string& what) {
                for (int strip = 0; strip < kStrips; ++strip) {
                    const int x = image.Width() * (2 * strip + 1) / (2 * kStrips);
                    EXPECT_STREQ(image.ColorName(x, image.Height() / 2), drawn[strip] ? "green" : "red")
                        << what << ": strip " << strip;
                }
            }

            // Magma's server in THIS process: the inproc arm. Spawn and tcp servers are other
            // processes, and the monolith arm never takes the wire path at all.
            bool CountersReadable() const { return m_magma && m_transport == "inproc"; }

            WireIndirectCounters Delta() const {
                WireIndirectCounters now{};
                EXPECT_TRUE(PeekWireIndirectCounters(&now));
                return {now.nativeDraws - m_before.nativeDraws, now.cpuExpansions - m_before.cpuExpansions,
                        now.barriers - m_before.barriers, now.hostWaits - m_before.hostWaits,
                        now.indirectWaits - m_before.indirectWaits};
            }

            bool m_magma = false;
            std::string m_transport;
            WireIndirectCounters m_before{};
            GLuint m_program = 0, m_copy = 0, m_vao = 0, m_vbo = 0, m_ebo = 0;
            GLuint m_indirect = 0, m_parameter = 0, m_source = 0;
        };

        // Every indirect form from CPU-written commands. On Magma/inproc the server must have
        // issued all six natively, expanded none, waited for nothing and recorded no barrier (no
        // shader ever wrote these stores).
        TEST_F(WireIndirectDrawScenario, EveryIndirectFormIsIssuedNativelyFromItsStore) {
            if (!Ready() || IsSkipped()) return;
            // Arrays commands at 0.., elements commands after them: the offsets are byte offsets
            // into one store, as an application packs them.
            const std::vector<ArraysCommand> arrays = {Arrays(0), Arrays(1), Arrays(3), Arrays(2)};
            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, arrays);
            Image image = Render([&] { glDrawArraysIndirect(GL_TRIANGLES, reinterpret_cast<const void*>(16)); });
            ExpectStrips(image, {false, true, false, false}, "glDrawArraysIndirect at byte 16");
            image = Render([&] { glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, 3, 0); });
            ExpectStrips(image, {true, true, false, true}, "glMultiDrawArraysIndirect, 3 commands");

            const std::vector<PaddedElementsCommand> padded = {{Elements(3), {}}, {Elements(0), {}}};
            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, padded);
            image = Render([&] {
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 2, sizeof(PaddedElementsCommand));
            });
            ExpectStrips(image, {true, false, false, true}, "glMultiDrawElementsIndirect, stride 32");
            image = Render([&] {
                glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                       reinterpret_cast<const void*>(sizeof(PaddedElementsCommand)));
            });
            ExpectStrips(image, {true, false, false, false}, "glDrawElementsIndirect at byte 32");

            // The count forms: three commands each, a count word that allows fewer.
            Fill(m_parameter, GL_PARAMETER_BUFFER, std::vector<std::uint32_t>{7, 2, 1});
            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, std::vector<ArraysCommand>{Arrays(0), Arrays(1), Arrays(2)});
            image = Render([&] {
                glMultiDrawArraysIndirectCount(GL_TRIANGLES, nullptr, 4, 3, 0);
            });
            ExpectStrips(image, {true, true, false, false}, "glMultiDrawArraysIndirectCount, count word 2 of 3");
            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, std::vector<ElementsCommand>{Elements(3), Elements(2), Elements(1)});
            image = Render([&] {
                glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 8, 3, 0);
            });
            ExpectStrips(image, {false, false, false, true}, "glMultiDrawElementsIndirectCount, count word 1 of 3");
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            if (!CountersReadable()) return;
            const WireIndirectCounters delta = Delta();
            EXPECT_EQ(delta.nativeDraws, 6u) << "every indirect call must be issued as vkCmdDraw*Indirect[Count]";
            EXPECT_EQ(delta.cpuExpansions, 0u) << "no indirect call may read its words on the CPU";
            EXPECT_EQ(delta.indirectWaits, 0u) << "an indirect call took a host wait";
            EXPECT_EQ(delta.hostWaits, 0u) << "a host wait was taken with no GPU write anywhere";
            EXPECT_EQ(delta.barriers, 0u) << "no shader wrote these stores, so no barrier is due";
        }

        // Commands a compute shader wrote, behind the application's own GL_COMMAND_BARRIER_BIT.
        // The buffers start with commands for the OTHER strips, so a read of stale words shows.
        // On Magma/inproc: issued natively, and - the point of the package - NO host wait.
        TEST_F(WireIndirectDrawScenario, ComputeWrittenCommandsDrawWithoutAHostWait) {
            if (!Ready() || IsSkipped()) return;
            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, std::vector<ArraysCommand>{Arrays(2), Arrays(3)});
            ComputeWrite(m_indirect, std::vector<ArraysCommand>{Arrays(0), Arrays(1)});
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
            Image image = Render([&] { glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, 2, 0); });
            ExpectStrips(image, {true, true, false, false}, "glMultiDrawArraysIndirect of compute-written commands");

            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, std::vector<ElementsCommand>{Elements(0), Elements(1)});
            ComputeWrite(m_indirect, std::vector<ElementsCommand>{Elements(3), Elements(2)});
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
            image = Render([&] { glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 2, 0); });
            ExpectStrips(image, {false, false, true, true}, "glMultiDrawElementsIndirect of compute-written commands");
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            if (!CountersReadable()) return;
            const WireIndirectCounters delta = Delta();
            EXPECT_EQ(delta.nativeDraws, 2u) << "both draws must be issued natively";
            EXPECT_EQ(delta.cpuExpansions, 0u) << "no indirect call may read its words on the CPU";
            EXPECT_EQ(delta.indirectWaits, 0u)
                << "an indirect draw of shader-written commands waited for the GPU on the host";
            EXPECT_EQ(delta.hostWaits, 0u) << "a host wait was taken";
            EXPECT_EQ(delta.barriers, 2u) << "one INDIRECT_COMMAND_READ barrier per freshly written store";
        }

        // The COUNT word a compute shader wrote. Espryt reads that word on the CPU from the
        // server's staged copy of the parameter buffer; P8-C refreshes that copy after a shader
        // write (SplitHostBytesForCpuRead), so its split arms answer here too (P8-SE). Espryt's
        // MONOLITH arm still reads the frontend shadow without a sync (OPEN-QUESTIONS 15, C.md's
        // monolith defects), left to P13 (ID-P8-13).
        TEST_F(WireIndirectDrawScenario, ComputeWrittenCountWordIsTheOneTheGpuReads) {
            if (!Ready() || IsSkipped()) return;
            if (!m_magma && m_transport == "monolith" && !PeekSplitRuntime().dataArmIsRecord) {
                GTEST_SKIP() << "Espryt's monolith arm reads the count word from the frontend shadow without "
                                "a sync (OQ15; recorded for P13, ID-P8-13)";
            }
            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, std::vector<ElementsCommand>{Elements(0), Elements(3), Elements(2)});
            Fill(m_parameter, GL_PARAMETER_BUFFER, std::vector<std::uint32_t>{3});
            ComputeWrite(m_parameter, std::vector<std::uint32_t>{1});
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
            Image image = Render([&] {
                glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 0, 3, 0);
            });
            ExpectStrips(image, {true, false, false, false}, "glMultiDrawElementsIndirectCount, shader-written count 1");
            if (m_transport == "monolith") {
                // DirectVulkan.cpp's monolith MultiDrawArraysIndirectCount reads this word from the
                // frontend shadow without SyncGpuWrites: recorded for dev (ID-P8-3), not this lane's.
                return;
            }
            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, std::vector<ArraysCommand>{Arrays(1), Arrays(2), Arrays(3)});
            Fill(m_parameter, GL_PARAMETER_BUFFER, std::vector<std::uint32_t>{0});
            ComputeWrite(m_parameter, std::vector<std::uint32_t>{2});
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
            image = Render([&] { glMultiDrawArraysIndirectCount(GL_TRIANGLES, nullptr, 0, 3, 0); });
            ExpectStrips(image, {false, true, true, false}, "glMultiDrawArraysIndirectCount, shader-written count 2");
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            if (!CountersReadable()) return;
            const WireIndirectCounters delta = Delta();
            EXPECT_EQ(delta.nativeDraws, 2u);
            EXPECT_EQ(delta.cpuExpansions, 0u) << "a count word was read on the CPU";
            EXPECT_EQ(delta.hostWaits, 0u) << "a host wait was taken";
            EXPECT_EQ(delta.barriers, 2u) << "one barrier per draw whose count store a shader wrote";
        }

        // Flywheel's shape (minecraft-1.21.1-neoforge-create-indirect-in-world-align1024: 322
        // cull-then-draw rounds, no glMemoryBarrier between them). GL leaves it undefined; the
        // wire arm orders it anyway, once per write,
        // because the CPU path it replaced did (by waiting). Magma's split arms only: the monolith
        // arm records no dependency here (recorded for dev), and Espryt's is its driver's.
        TEST_F(WireIndirectDrawScenario, ShaderWrittenCommandsWithoutAnApplicationBarrierAreOrdered) {
            if (!Ready() || IsSkipped()) return;
            if (!m_magma || m_transport == "monolith") {
                GTEST_SKIP() << "the implicit INDIRECT_COMMAND_READ barrier is the Magma wire arm's (P8-D)";
            }
            Fill(m_indirect, GL_DRAW_INDIRECT_BUFFER, std::vector<ArraysCommand>{Arrays(3), Arrays(2)});
            ComputeWrite(m_indirect, std::vector<ArraysCommand>{Arrays(0), Arrays(1)});
            Image image = Render([&] { glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, 2, 0); });
            ExpectStrips(image, {true, true, false, false}, "the first draw after the write");
            image = Render([&] { glDrawArraysIndirect(GL_TRIANGLES, reinterpret_cast<const void*>(16)); });
            ExpectStrips(image, {false, true, false, false}, "a second draw, no write between");
            ComputeWrite(m_indirect, std::vector<ArraysCommand>{Arrays(2), Arrays(3)});
            image = Render([&] { glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, 2, 0); });
            ExpectStrips(image, {false, false, true, true}, "the draw after a second write");
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            if (!CountersReadable()) return;
            const WireIndirectCounters delta = Delta();
            EXPECT_EQ(delta.nativeDraws, 3u);
            EXPECT_EQ(delta.cpuExpansions, 0u);
            EXPECT_EQ(delta.hostWaits, 0u) << "a host wait was taken";
            EXPECT_EQ(delta.barriers, 2u) << "exactly one barrier per write: the second draw reads no new write";
        }

    } // namespace
} // namespace MGITest
