// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ClientVertexArrayScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A VERTEX ARRAY THAT LIVES IN THE APPLICATION'S OWN MEMORY, DRAWN OVER THE WIRE.
//
// WHY THIS FILE EXISTS AT ALL (P5e vi, ID-82 / BRIEF-P5E §6 open item (a)). The brief asked
// whether any existing scenario exercises a client-memory vertex array under split, and named
// DoublePrecisionScenario as the candidate. IT DOES NOT, and nothing else does either: every
// glVertexAttribPointer in this directory is issued with a buffer bound to GL_ARRAY_BUFFER, so
// its last argument is a byte OFFSET and not a host pointer (DoublePrecisionScenario uses the
// binding-model calls against real buffer objects, DoublePrecisionScenario.cpp:944-959). So the
// one draw shape whose bytes have no wire form was, until this file, never drawn over the wire
// in any lane.
//
// Under a transport the client snapshots referenced elements into owned buffer
// resources before returning. Both lockstep and run-ahead must render the pixels;
// neither may borrow the application's addresses on the apply thread. The added
// cases also cover host overwrites, instancing and GPU-produced fetch ranges.
//
// Both draw entry points that carry the upload are driven, because they are two arms and not
// one: DrawArrays uploads the single range, MultiDrawArrays uploads one range per sub-draw.

#include <cstdint>
#include <algorithm>
#include <array>
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

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        class ClientVertexArrayScenario : public ScenarioTest {};

        // THE WHOLE POINT IS THE ABSENCE OF A BUFFER. glBindBuffer(GL_ARRAY_BUFFER, 0) before
        // glVertexAttribPointer makes the last argument a HOST POINTER rather than an offset,
        // which is the one vertex source EmitVertexBuffers publishes as Res ==
        // kMGPipeNullHandle - "not a hole: it is exactly how the server learns this attribute
        // is client-sourced, upload it yourself" (VertexInputEmit.h).
        //
        // A VAO is still bound, because ES core requires one and because the server's upload
        // hangs off the VAO's twin.
        struct ClientArrayQuad {
            GLuint vao = 0;
            const float* vertices = nullptr;

            void Bind(const float* quad) {
                vertices = quad;
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), vertices);
            }
            void Release() {
                glBindVertexArray(0);
                if (vao != 0) glDeleteVertexArrays(1, &vao);
                vao = 0;
            }
        };

        // Full-viewport strip, so "did the draw arrive" is one pixel read rather than a shape
        // comparison - the claim is about the vertex SOURCE, not about rasterisation.
        const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};

        Rgba8 CentreAfter(int width, int height) {
            const Image image = ReadPixelsRect(0, 0, width, height);
            return image.At(width / 2, height / 2);
        }

        GLuint ClientInputComputeProgram(const char* source) {
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
            return program;
        }

    } // namespace

    TEST_F(ClientVertexArrayScenario, AClientMemoryVertexArrayReachesTheDrawItFeeds) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);

        ClientArrayQuad quad;
        quad.Bind(kQuad);
        glUseProgram(program);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        const Rgba8 centre = CentreAfter(width, height);
        EXPECT_GT(static_cast<int>(centre.g), 200)
            << "the client-memory vertex array never reached the draw: the centre is " << centre
            << ", i.e. still the clear colour. The draw did not consume its owned client-vertex snapshot";
        EXPECT_LT(static_cast<int>(centre.b), 60)
            << "the quad drew, but the clear colour is still showing through: " << centre;

        quad.Release();
        glUseProgram(0);
        glDeleteProgram(program);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(ClientVertexArrayScenario, EverySubDrawOfAMultiDrawArraysGetsItsOwnClientRange) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);

        // Two strips out of one client array: the left half and the right half, so a
        // MultiDrawArrays that uploaded only the FIRST sub-draw's range leaves one side blue.
        static const float kTwoHalves[] = {
            -1.0f, -1.0f, 0.0f, -1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
            0.0f,  -1.0f, 1.0f, -1.0f, 0.0f,  1.0f, 1.0f, 1.0f,
        };
        ClientArrayQuad quad;
        quad.Bind(kTwoHalves);
        glUseProgram(program);

        const GLint firsts[2] = {0, 4};
        const GLsizei counts[2] = {4, 4};
        glMultiDrawArrays(GL_TRIANGLE_STRIP, firsts, counts, 2);

        const Image image = ReadPixelsRect(0, 0, width, height);
        const Rgba8 left = image.At(width / 4, height / 2);
        const Rgba8 right = image.At((3 * width) / 4, height / 2);
        EXPECT_GT(static_cast<int>(left.g), 200)
            << "the FIRST sub-draw's client range did not reach the draw: " << left;
        EXPECT_GT(static_cast<int>(right.g), 200)
            << "the SECOND sub-draw's client range did not reach the draw (" << right
            << "): an upload that ran once for the whole call instead of once per sub-draw "
               "leaves exactly this half unpainted";

        quad.Release();
        glUseProgram(0);
        glDeleteProgram(program);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(ClientVertexArrayScenario, ClientVerticesAndIndicesKeepTheirBytesAfterTheDrawReturns) {
        if (!Ready()) return;
        std::string error;
        const GLuint program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;
        BindDefaultFramebuffer();
        glViewport(0, 0, Gl().Width(), Gl().Height());
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        ClearTo(0, 0, 1, 1);
        std::array<float, 8> positions{-1, -1, 0, -1, -1, 1, 0, 1};
        std::array<GLushort, 6> indices{0, 1, 2, 2, 1, 3};
        ClientArrayQuad quad;
        quad.Bind(positions.data());
        glUseProgram(program);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices.data());
        for (size_t i = 0; i < positions.size(); i += 2) positions[i] += 1;
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices.data());
        // No readback or Finish between the draws and the host writes.
        std::fill(positions.begin(), positions.end(), -100.0f);
        std::fill(indices.begin(), indices.end(), 0);
        const Image image = ReadPixelsRect(0, 0, Gl().Width(), Gl().Height());
        EXPECT_GT(image.At(Gl().Width() / 4, Gl().Height() / 2).g, 200);
        EXPECT_GT(image.At(3 * Gl().Width() / 4, Gl().Height() / 2).g, 200);
        quad.Release();
        glUseProgram(0);
        glDeleteProgram(program);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(ClientVertexArrayScenario, ClientAttributesKeepBaseVertexAndDividedBaseInstanceFetches) {
        if (!Ready()) return;
        const char* vertex = R"(#version 430 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aOffset;
void main() { gl_Position = vec4(aPos + aOffset, 0.0, 1.0); }
)";
        std::string error;
        const GLuint program = CompileProgram(vertex, kFS, &error);
        ASSERT_NE(program, 0u) << error;
        BindDefaultFramebuffer();
        glViewport(0, 0, Gl().Width(), Gl().Height());
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        ClearTo(0, 0, 1, 1);
        const float positions[]{-0.4f, -0.8f, 0.4f, -0.8f, -0.4f, 0.8f, 0.4f, 0.8f};
        const float offsets[]{100, 100, -0.5f, 0, 0.5f, 0};
        const GLushort indices[]{1, 2, 3, 3, 2, 4};
        ClientArrayQuad quad;
        quad.Bind(positions);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), offsets);
        glVertexAttribDivisor(1, 2);
        glUseProgram(program);
        glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices, 3, -1, 1);
        const Image image = ReadPixelsRect(0, 0, Gl().Width(), Gl().Height());
        EXPECT_GT(image.At(Gl().Width() / 4, Gl().Height() / 2).g, 200);
        EXPECT_GT(image.At(3 * Gl().Width() / 4, Gl().Height() / 2).g, 200);
        quad.Release();
        glUseProgram(0);
        glDeleteProgram(program);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(ClientVertexArrayScenario, GpuWrittenIndicesSelectTheClientVerticesBeforeTheyAreCopied) {
        if (!Ready()) return;
        const GLuint compute = ClientInputComputeProgram(R"(#version 430 core
layout(local_size_x=1) in;
layout(std430, binding=0) buffer Indices { uint value[]; };
void main() {
    value[0]=4u; value[1]=5u; value[2]=6u;
    value[3]=6u; value[4]=5u; value[5]=7u;
}
)");
        ASSERT_NE(compute, 0u);
        std::string error;
        const GLuint program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;
        BindDefaultFramebuffer();
        glViewport(0, 0, Gl().Width(), Gl().Height());
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        ClearTo(0, 0, 1, 1);
        const float positions[]{100, 100, 100, 100, 100, 100, 100, 100,
                                -1, -1, 1, -1, -1, 1, 1, 1};
        ClientArrayQuad quad;
        quad.Bind(positions);
        GLuint ebo = 0;
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        const GLuint poison[6]{};
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(poison), poison, GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ebo);
        glUseProgram(compute);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ELEMENT_ARRAY_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glUseProgram(program);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        EXPECT_GT(CentreAfter(Gl().Width(), Gl().Height()).g, 200);
        quad.Release();
        glUseProgram(0);
        glDeleteBuffers(1, &ebo);
        glDeleteProgram(program);
        glDeleteProgram(compute);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(ClientVertexArrayScenario, GpuWrittenIndirectCommandsDefineTheClientAttributeSnapshot) {
        if (!Ready()) return;
        const GLuint compute = ClientInputComputeProgram(R"(#version 430 core
layout(local_size_x=1) in;
layout(std430, binding=0) buffer Command { uint value[]; };
void main() { value[0]=4u; value[1]=1u; value[2]=4u; value[3]=1u; }
)");
        ASSERT_NE(compute, 0u);
        const char* vertex = R"(#version 430 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aOffset;
void main() { gl_Position = vec4(aPos + aOffset, 0.0, 1.0); }
)";
        std::string error;
        const GLuint program = CompileProgram(vertex, kFS, &error);
        ASSERT_NE(program, 0u) << error;
        BindDefaultFramebuffer();
        glViewport(0, 0, Gl().Width(), Gl().Height());
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        ClearTo(0, 0, 1, 1);
        const float positions[]{100, 100, 100, 100, 100, 100, 100, 100,
                                -1, -1, 1, -1, -1, 1, 1, 1};
        const float offsets[]{100, 100, 0, 0};
        ClientArrayQuad quad;
        quad.Bind(positions);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), offsets);
        glVertexAttribDivisor(1, 1);
        GLuint commands = 0;
        glGenBuffers(1, &commands);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, commands);
        const GLuint poison[4]{};
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(poison), poison, GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, commands);
        glUseProgram(compute);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glUseProgram(program);
        glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
        EXPECT_GT(CentreAfter(Gl().Width(), Gl().Height()).g, 200);
        quad.Release();
        glUseProgram(0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        glDeleteBuffers(1, &commands);
        glDeleteProgram(program);
        glDeleteProgram(compute);
        EXPECT_EQ(FirstGLError(), 0u);
    }

} // namespace MGITest
