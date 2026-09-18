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
// WHAT IT PINS, in the two arms it can be in:
//
//   TODAY (lockstep, and the monolith control) the draw is NOT refused - ruling 4: the client
//   is parked in WaitForApplied for exactly this record, so the server's per-draw upload of the
//   application's bytes (Managers.cpp's SyncClientSideAttributesForDrawArrays, the kimi audit's
//   row 14) reads memory that is not moving. The assertion is therefore about PIXELS: the quad
//   has to arrive. That is what makes vi's "monolith-only would have been wrong here" concrete
//   rather than an argument in a report - delete the split arm of the upload and this goes red.
//
//   UNDER RUN-AHEAD it is REFUSED BY NAME on the GL thread, Fatal{UnmigratedVerb,
//   "DrawArrays+CLIENT_ARRAYS"} (CONTRACT-P5E §5.1), because the client is a frame ahead and
//   those bytes are moving under the reader. This file is the red-once for that refusal: it is
//   the only lane entry that can reach it. Staging the bytes as a per-attribute {BindingIndex,
//   MGHostSpan} tail - the shape kDrawHasUserIndices already has for client INDICES - is P8's,
//   and retires the refusal along with this scenario's second half.
//
// Both draw entry points that carry the upload are driven, because they are two arms and not
// one: DrawArrays uploads the single range, MultiDrawArrays uploads one range per sub-draw.

#include <cstdint>
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
            << ", i.e. still the clear colour. Under split this means the per-draw upload of "
               "the application's own bytes did not run - the arm CONTRACT-P5E §5.1 keeps alive "
               "for lockstep, and refuses only under run-ahead";
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

} // namespace MGITest
