// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PostLinkAttachScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A PROGRAM'S LIVE ATTACH LIST IS NOT ITS EXECUTABLE, AND THE BACKENDS MAY NOT
// INDEX ONE BY THE OTHER.
//
// GL 4.6 core 7.3: glAttachShader adds to the program's attach list immediately and affects
// what the program RUNS only at the next link (glDetachShader defers its removal the same
// way). So between an attach and the relink the two lists differ - the attach list is
// strictly longer - and the program stays perfectly drawable throughout, with the executable
// its last link produced.
//
// Both backends walked the attach list while indexing the LAST LINK's generated SPIR-V by
// the same running index:
//
//   DirectGLES   BackendProgramObjectImpl::SyncToBackend - `shaderSpirvs[index]` over
//                `attachedShaders.size()`
//   DirectVulkan ProgramFactory::GetOrCreateProgram      - `spirv[i]` and `moduleSpirvs[i]`
//                over `shaders.size()`
//
// One post-link attach therefore read one Vector past the end of the module array and
// copied it, which is the SIGSEGV this scenario is the regression test for (the source
// vector reported a capacity of 35177040171136). DirectGLES additionally derived
// "does this program tessellate" from the same wrong list, which would synthesize a
// pass-through tessellation control stage for an executable that does not tessellate.
//
// The repro needs the attach to land BEFORE the program's first backend build: the ES
// twin's rebuild is gated on the link version (which an attach does not move), so a program
// that was already drawn once keeps its built driver program and never re-reads the list.
// Every case below therefore attaches first and draws second.
//
// Deliberately pinned with a PIXEL and not just with glGetError. "Reject the draw earlier"
// would silence the crash while breaking the spec - GL requires this draw to execute - so
// the assertion has to be that the frame really came out, not merely that nothing complained.
//
// Needs a real context: the crash is in a backend program build, which the GPU-free suites
// never reach.

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

        constexpr int kFboWidth = 64;
        constexpr int kFboHeight = 64;

        // A full-viewport triangle from gl_VertexID alone, so the scenario needs no vertex
        // buffer and every pixel of the target is covered by the one draw.
        const char* const kVertexSource = R"(#version 330 core
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

        const char* const kFragmentSource = R"(#version 330 core
out vec4 fragColor;
void main()
{
    fragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        // The replacement fragment stage of the last case. A different colour, so "which
        // executable did this draw run" is answerable from the frame alone.
        const char* const kBlueFragmentSource = R"(#version 330 core
out vec4 fragColor;
void main()
{
    fragColor = vec4(0.0, 0.0, 1.0, 1.0);
}
)";

        constexpr Rgba8 kGreen{0, 255, 0, 255};
        constexpr Rgba8 kBlue{0, 0, 255, 255};

        // The extra attaches. Each declares a stage the executable ALREADY has and no main(),
        // which is what a real shader library looks like and what makes the relink at the end
        // of the second case legal. Their whole job here is to make the attach list longer
        // than the module array.
        const char* const kVertexHelperSource = R"(#version 330 core
vec4 mgPostLinkAttachVertexHelper()
{
    return vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        const char* const kFragmentHelperSource = R"(#version 330 core
vec4 mgPostLinkAttachFragmentHelper()
{
    return vec4(1.0, 0.0, 1.0, 1.0);
}
)";

        // A pass-through, so that once it IS linked in the same full-viewport triangle still
        // reaches the rasterizer and the final frame is still comparable to the first one.
        const char* const kGeometrySource = R"(#version 330 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
void main()
{
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
}
)";

        class PostLinkAttachScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                m_target = MakeColorFbo(kFboWidth, kFboHeight);
                ASSERT_NE(m_target.fbo, 0u) << "could not create the scenario's colour target";
                BindFbo(m_target);
                DrainErrors();
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                for (const GLuint program : m_programs) glDeleteProgram(program);
                m_programs.clear();
                for (const GLuint shader : m_shaders) glDeleteShader(shader);
                m_shaders.clear();
                BindDefaultFramebuffer();
                DestroyColorFbo(m_target);
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_vao = 0;
                DrainErrors();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            static bool BackendHostsGeometry() {
                GLint maxGeometryOutputVertices = 0;
                glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES, &maxGeometryOutputVertices);
                DrainErrors();
                return maxGeometryOutputVertices >= 4;
            }

            // Kept alive until TearDown rather than flagged for deletion at attach time: a
            // deleted-but-attached shader is a second, unrelated lifetime rule, and this
            // scenario is about which LIST the backend reads.
            GLuint MakeShader(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                if (shader == 0) return 0;
                m_shaders.push_back(shader);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                return shader;
            }

            // Vertex + fragment, linked. This is the executable every case draws with.
            // `outFragmentShader` is the stage that paints green, which the last case needs a
            // name for in order to detach it.
            GLuint LinkBaseProgram(GLuint* outFragmentShader = nullptr) {
                const GLuint program = glCreateProgram();
                m_programs.push_back(program);
                const GLuint fragment = MakeShader(GL_FRAGMENT_SHADER, kFragmentSource);
                glAttachShader(program, MakeShader(GL_VERTEX_SHADER, kVertexSource));
                glAttachShader(program, fragment);
                glLinkProgram(program);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (!linked) return 0;
                if (outFragmentShader != nullptr) *outFragmentShader = fragment;
                return program;
            }

            // Clears to red, draws the full-viewport triangle, and hands back the frame. Red
            // is deliberately the clear colour: a draw that silently did not execute leaves a
            // red target, which is a different failure message from a draw that executed and
            // painted the wrong thing.
            //
            // `outDrawError` is sampled between the draw and the readback, so a rejected draw
            // is never confused with a readback that went wrong afterwards.
            Image DrawFullViewportTriangle(GLuint program, GLenum mode, GLenum* outDrawError = nullptr) {
                glUseProgram(program);
                ClearTo(1.0f, 0.0f, 0.0f, 1.0f);
                DrainErrors();
                glDrawArrays(mode, 0, 3);
                if (outDrawError != nullptr) *outDrawError = glGetError();
                return ReadPixels(kFboWidth, kFboHeight);
            }

            // The clear colour is red and no shader here ever writes red, so "still red" reads
            // as "the draw did not execute" and any other wrong colour as "it executed against
            // the wrong modules" - two failures worth telling apart.
            static void ExpectFullyColored(const Image& frame, const Rgba8& expected, const char* what) {
                ASSERT_FALSE(frame.Empty()) << what << ": nothing was read back";
                for (const int y : {0, kFboHeight / 2, kFboHeight - 1}) {
                    for (const int x : {0, kFboWidth / 2, kFboWidth - 1}) {
                        EXPECT_EQ(frame.At(x, y), expected)
                            << what << ": pixel (" << x << ", " << y << ") is " << frame.ColorName(x, y);
                    }
                }
            }

            GLuint m_vao = 0;
            ColorFbo m_target{};
            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_shaders;
        };

        // THE REGRESSION. Up to four shaders attached after the link (the geometry one only
        // where the backend has that stage), two of them duplicating a stage the executable
        // already carries - so the attach list runs to five or six while the last link produced
        // two modules, and the old loops read indices 2..5 of a 2-element array.
        //
        // Duplicating a stage is the sharp case on purpose: it is the one shape under which a
        // "look the stage up in the attach list instead" repair still returns a valid-looking
        // index for a module that does not exist.
        TEST_F(PostLinkAttachScenario, DrawingAfterPostLinkAttachesStaysInsideTheGeneratedModules) {
            if (!Ready()) GTEST_SKIP();

            const GLuint program = LinkBaseProgram();
            ASSERT_NE(program, 0u) << "the vertex+fragment program did not link";

            // Not drawn yet: the ES backend's rebuild is gated on the link version, so a draw
            // here would build the driver program from the 2-module executable and the attaches
            // below would never be re-read. The repro is the FIRST build seeing the long list.
            glAttachShader(program, MakeShader(GL_VERTEX_SHADER, kVertexHelperSource));
            glAttachShader(program, MakeShader(GL_FRAGMENT_SHADER, kFragmentHelperSource));
            if (BackendHostsGeometry()) {
                glAttachShader(program, MakeShader(GL_GEOMETRY_SHADER, kGeometrySource));
            }
            // The stage that made DirectGLES synthesize a pass-through control stage for a
            // program whose executable does not tessellate. Attached whether or not this
            // backend can tessellate - an attach needs no support and no successful compile.
            const GLuint tessEval = MakeShader(GL_TESS_EVALUATION_SHADER, R"(#version 420 core
layout(triangles, equal_spacing, ccw) in;
void main()
{
    gl_Position = gl_in[0].gl_Position;
}
)");
            if (tessEval != 0) glAttachShader(program, tessEval);
            DrainErrors();

            GLint attachedCount = 0;
            glGetProgramiv(program, GL_ATTACHED_SHADERS, &attachedCount);
            DrainErrors();
            ASSERT_GT(attachedCount, 2) << "the attaches did not land, so this case is not testing anything";

            // Still the two-stage executable of three lines ago, and GL says it draws.
            GLenum drawError = GL_NO_ERROR;
            const Image frame = DrawFullViewportTriangle(program, GL_TRIANGLES, &drawError);
            EXPECT_EQ(drawError, static_cast<GLenum>(GL_NO_ERROR))
                << "the attaches have not been linked in, so nothing about them may reject this draw";
            ExpectFullyColored(frame, kGreen, "the post-attach draw");
            DrainErrors();
        }

        // The same window, asked to prove something stronger than "it did not crash": WHICH
        // modules the draw in that window ran. Between the detach+attach and the relink the
        // program has three attached shaders and two modules, and GL 4.6 core 7.3 says the
        // executable is still the one the last link produced - so the frame must come out in
        // the OLD fragment shader's colour, not the newly attached one's and not garbage.
        //
        // This is also the other direction of the fix, so it cannot be "freeze the backend on
        // the first link": the relink really does swap the executable, and the very next draw
        // has to be rebuilt from it.
        TEST_F(PostLinkAttachScenario, TheWindowKeepsTheOldExecutableAndTheRelinkSwapsIt) {
            if (!Ready()) GTEST_SKIP();

            GLuint greenFragment = 0;
            const GLuint program = LinkBaseProgram(&greenFragment);
            ASSERT_NE(program, 0u) << "the vertex+fragment program did not link";

            // Both of these are deferred to the next link, in opposite directions: the green
            // stage stays in the executable until then, and the blue one stays out of it.
            const GLuint blueFragment = MakeShader(GL_FRAGMENT_SHADER, kBlueFragmentSource);
            glDetachShader(program, greenFragment);
            glAttachShader(program, blueFragment);
            DrainErrors();

            GLenum windowDrawError = GL_NO_ERROR;
            const Image inTheWindow = DrawFullViewportTriangle(program, GL_TRIANGLES, &windowDrawError);
            EXPECT_EQ(windowDrawError, static_cast<GLenum>(GL_NO_ERROR))
                << "neither the detach nor the attach has been linked in, so the draw must execute";
            ExpectFullyColored(inTheWindow, kGreen, "the draw inside the attach window");
            DrainErrors();

            glLinkProgram(program);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            ASSERT_EQ(linked, GL_TRUE) << "the relink onto the blue fragment stage failed";
            DrainErrors();

            GLenum relinkedDrawError = GL_NO_ERROR;
            const Image afterRelink = DrawFullViewportTriangle(program, GL_TRIANGLES, &relinkedDrawError);
            EXPECT_EQ(relinkedDrawError, static_cast<GLenum>(GL_NO_ERROR)) << "the relinked program must draw";
            ExpectFullyColored(afterRelink, kBlue, "the draw after the relink");
            DrainErrors();
        }

    } // namespace
} // namespace MGITest
