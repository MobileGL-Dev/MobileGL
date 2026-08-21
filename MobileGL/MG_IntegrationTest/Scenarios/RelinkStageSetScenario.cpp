// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/RelinkStageSetScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A RELINK MAY CHANGE WHICH STAGES A PROGRAM HAS, AND EVERY DRAW AFTER IT RUNS
// THE NEW STAGE SET.
//
// GL 4.6 core 7.3: glLinkProgram builds an executable out of whatever is attached at that
// moment, so the stage set is a property of a LINK and not of a program. A program that
// linked vertex+fragment, drew, then had a geometry shader attached and was relinked runs
// three stages from that point on.
//
// DirectGLES rebuilds its driver program in place - same GL name, new executable - and the
// per-draw bind dedupes on that name, so a relink that changed the stage set installed
// nothing and the following draws rendered NOTHING at all: no GL error, LINK_STATUS true,
// and a framebuffer that kept its clear colour. See the note at the glLinkProgram in
// BackendProgramObjectImpl::SyncToBackend for what the driver does with such a relink.
//
// PostLinkAttachScenario pins the other half of the same rule - that the executable does
// NOT move until the relink. This one pins what happens when it does, in all three
// directions: a stage added, a stage removed, and a stage added that the ES backend has to
// synthesize a partner for.
//
// Every case asserts on a SHAPE and not merely on "something came out". The geometry and
// tessellation stages here halve the triangle, so a full-viewport green frame and a
// half-size one say which executable ran - "still drew" and "drew the right stages" are
// different claims and only the second one is worth pinning.
//
// Needs a real context: what is asserted is a rendered pixel out of a backend program build.

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

        // A full-viewport triangle out of gl_VertexID alone, so no case here needs a vertex
        // buffer and one draw covers every pixel of the target.
        const char* const kVertexSource = R"(#version 420 core
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

        const char* const kFragmentSource = R"(#version 420 core
out vec4 fragColor;
void main()
{
    fragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        // Halves the triangle instead of passing it through: the centre pixel stays covered
        // and all four corners fall outside, so the frame alone says whether this stage ran.
        const char* const kGeometrySource = R"(#version 420 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
void main()
{
    for (int i = 0; i < 3; ++i) {
        gl_Position = vec4(gl_in[i].gl_Position.xy * 0.5, gl_in[i].gl_Position.zw);
        EmitVertex();
    }
    EndPrimitive();
}
)";

        // No control stage on purpose: OpenGL ES rejects that shape outright, so DirectGLES
        // synthesizes a pass-through one (AttachPassthroughTessControlStage) and DirectVulkan
        // does the same. Reading only gl_in[].gl_Position keeps this inside what such a
        // pass-through may forward. At the tessellation levels it sets (all 1.0) the patch
        // comes back out as one triangle whose gl_TessCoord values are the three corners, so
        // the barycentric sum reproduces the vertex stage's triangle - halved, for the same
        // reason the geometry stage above halves it.
        const char* const kTessEvalSource = R"(#version 420 core
layout(triangles, equal_spacing, ccw) in;
void main()
{
    vec4 p = gl_TessCoord.x * gl_in[0].gl_Position +
             gl_TessCoord.y * gl_in[1].gl_Position +
             gl_TessCoord.z * gl_in[2].gl_Position;
    gl_Position = vec4(p.xy * 0.5, p.zw);
}
)";

        constexpr Rgba8 kGreen{0, 255, 0, 255};
        constexpr Rgba8 kRed{255, 0, 0, 255};

        class RelinkStageSetScenario : public ScenarioTest {
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

            // The same real-backend probes the other stage-gated scenarios use: 0 on a
            // DirectGLES driver without the extension and on a DirectVulkan device without
            // the feature.
            static bool BackendHostsGeometry() {
                GLint maxGeometryOutputVertices = 0;
                glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES, &maxGeometryOutputVertices);
                DrainErrors();
                return maxGeometryOutputVertices >= 4;
            }

            static bool BackendHostsTessellation() {
                GLint maxTessGenLevel = 0;
                glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTessGenLevel);
                DrainErrors();
                return maxTessGenLevel >= 1;
            }

            static std::string InfoLog(GLuint object, bool isShader) {
                GLint length = 0;
                if (isShader) {
                    glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
                } else {
                    glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
                }
                if (length <= 0) return {};
                std::string log(static_cast<size_t>(length), '\0');
                if (isShader) {
                    glGetShaderInfoLog(object, length, nullptr, log.data());
                } else {
                    glGetProgramInfoLog(object, length, nullptr, log.data());
                }
                log.resize(std::char_traits<char>::length(log.c_str()));
                return log;
            }

            GLuint MakeShader(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                if (shader == 0) return 0;
                m_shaders.push_back(shader);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                EXPECT_EQ(compiled, GL_TRUE) << "a scenario shader did not compile: " << InfoLog(shader, true);
                return shader;
            }

            GLuint MakeProgram() {
                const GLuint program = glCreateProgram();
                m_programs.push_back(program);
                return program;
            }

            bool Link(GLuint program) {
                glLinkProgram(program);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked != GL_TRUE) {
                    ADD_FAILURE() << "the link failed: " << InfoLog(program, false);
                    return false;
                }
                return true;
            }

            // Clears to red and draws. Red is the clear colour deliberately: nothing here ever
            // paints red inside the triangle, so a frame that is red where it should be green
            // says "this draw did not execute" while a frame that is green where it should be
            // red says "it executed against the wrong executable" - two failures worth telling
            // apart. The error is sampled between the draw and the readback so a rejected draw
            // is never confused with a readback that went wrong afterwards.
            Image DrawTriangle(GLuint program, GLenum mode, GLenum* outDrawError = nullptr) {
                glUseProgram(program);
                ClearTo(1.0f, 0.0f, 0.0f, 1.0f);
                DrainErrors();
                glDrawArrays(mode, 0, 3);
                if (outDrawError != nullptr) *outDrawError = glGetError();
                return ReadPixels(kFboWidth, kFboHeight);
            }

            // The vertex stage's triangle covers the whole target, corners included.
            static void ExpectFullTriangle(const Image& frame, const char* what) {
                ASSERT_FALSE(frame.Empty()) << what << ": nothing was read back";
                ExpectPixel(frame, kFboWidth / 2, kFboHeight / 2, kGreen, what, "centre");
                for (const int y : {0, kFboHeight - 1}) {
                    for (const int x : {0, kFboWidth - 1}) {
                        ExpectPixel(frame, x, y, kGreen, what, "corner");
                    }
                }
            }

            // ...and halved by a geometry or tessellation stage it no longer reaches any of
            // them, which is what makes the shape readable as "that stage ran".
            static void ExpectHalvedTriangle(const Image& frame, const char* what) {
                ASSERT_FALSE(frame.Empty()) << what << ": nothing was read back";
                ExpectPixel(frame, kFboWidth / 2, kFboHeight / 2, kGreen, what, "centre");
                for (const int y : {0, kFboHeight - 1}) {
                    for (const int x : {0, kFboWidth - 1}) {
                        ExpectPixel(frame, x, y, kRed, what, "corner");
                    }
                }
            }

            static void ExpectPixel(const Image& frame, int x, int y, const Rgba8& expected, const char* what,
                                    const char* where) {
                EXPECT_EQ(frame.At(x, y), expected)
                    << what << ": " << where << " pixel (" << x << ", " << y << ") is " << frame.ColorName(x, y);
            }

            GLuint m_vao = 0;
            ColorFbo m_target{};
            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_shaders;
        };

        // THE REGRESSION. Vertex+fragment, linked and DRAWN - which is what puts a built driver
        // program on the backend twin - then a geometry shader attached and the program
        // relinked. The halved frame is the assertion: the three-stage executable really is
        // what the next draw ran.
        TEST_F(RelinkStageSetScenario, RelinkingToAddAGeometryStageRunsTheNewExecutable) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsGeometry()) {
                GTEST_SKIP() << "no geometry stage on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << "); there is no stage to add";
            }

            const GLuint program = MakeProgram();
            glAttachShader(program, MakeShader(GL_VERTEX_SHADER, kVertexSource));
            glAttachShader(program, MakeShader(GL_FRAGMENT_SHADER, kFragmentSource));
            ASSERT_TRUE(Link(program));
            DrainErrors();

            GLenum beforeError = GL_NO_ERROR;
            const Image before = DrawTriangle(program, GL_TRIANGLES, &beforeError);
            EXPECT_EQ(beforeError, static_cast<GLenum>(GL_NO_ERROR)) << "the vertex+fragment draw must execute";
            ExpectFullTriangle(before, "the draw before the relink");
            DrainErrors();

            glAttachShader(program, MakeShader(GL_GEOMETRY_SHADER, kGeometrySource));
            ASSERT_TRUE(Link(program));
            DrainErrors();

            GLenum afterError = GL_NO_ERROR;
            const Image after = DrawTriangle(program, GL_TRIANGLES, &afterError);
            EXPECT_EQ(afterError, static_cast<GLenum>(GL_NO_ERROR)) << "the relinked three-stage program must draw";
            ExpectHalvedTriangle(after, "the draw after the geometry stage was linked in");
            DrainErrors();
        }

        // The same move in the other direction, which no repair may confuse with "the stage
        // set did not change": the geometry stage leaves the executable, so the halving has to
        // stop with it.
        TEST_F(RelinkStageSetScenario, RelinkingToRemoveAGeometryStageRunsTheNewExecutable) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsGeometry()) {
                GTEST_SKIP() << "no geometry stage on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << "); there is no stage to remove";
            }

            const GLuint program = MakeProgram();
            glAttachShader(program, MakeShader(GL_VERTEX_SHADER, kVertexSource));
            const GLuint geometry = MakeShader(GL_GEOMETRY_SHADER, kGeometrySource);
            glAttachShader(program, geometry);
            glAttachShader(program, MakeShader(GL_FRAGMENT_SHADER, kFragmentSource));
            ASSERT_TRUE(Link(program));
            DrainErrors();

            // Also the control for the case above: a three-stage program linked in ONE go and
            // never relinked draws its halved triangle.
            GLenum beforeError = GL_NO_ERROR;
            const Image before = DrawTriangle(program, GL_TRIANGLES, &beforeError);
            EXPECT_EQ(beforeError, static_cast<GLenum>(GL_NO_ERROR)) << "the three-stage draw must execute";
            ExpectHalvedTriangle(before, "the draw before the geometry stage was dropped");
            DrainErrors();

            glDetachShader(program, geometry);
            ASSERT_TRUE(Link(program));
            DrainErrors();

            GLenum afterError = GL_NO_ERROR;
            const Image after = DrawTriangle(program, GL_TRIANGLES, &afterError);
            EXPECT_EQ(afterError, static_cast<GLenum>(GL_NO_ERROR)) << "the relinked vertex+fragment program must draw";
            ExpectFullTriangle(after, "the draw after the geometry stage was dropped");
            DrainErrors();
        }

        // The third direction, and the one that asks the most of the rebuild: the added stage
        // is a tessellation evaluation shader with no control stage, so the ES backend has to
        // synthesize a pass-through control stage for an executable that had neither a moment
        // ago. GL_PATCHES becomes the only legal mode with it, which is also the only draw-mode
        // change any case here makes.
        TEST_F(RelinkStageSetScenario, RelinkingToAddATessEvalStageRunsTheNewExecutable) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << "); there is no stage to add";
            }

            const GLuint program = MakeProgram();
            glAttachShader(program, MakeShader(GL_VERTEX_SHADER, kVertexSource));
            glAttachShader(program, MakeShader(GL_FRAGMENT_SHADER, kFragmentSource));
            ASSERT_TRUE(Link(program));
            DrainErrors();

            GLenum beforeError = GL_NO_ERROR;
            const Image before = DrawTriangle(program, GL_TRIANGLES, &beforeError);
            EXPECT_EQ(beforeError, static_cast<GLenum>(GL_NO_ERROR)) << "the vertex+fragment draw must execute";
            ExpectFullTriangle(before, "the draw before the relink");
            DrainErrors();

            glAttachShader(program, MakeShader(GL_TESS_EVALUATION_SHADER, kTessEvalSource));
            ASSERT_TRUE(Link(program));
            // Three, which is already the default; spelled out because the synthesized control
            // stage's output patch size is compiled from it.
            glPatchParameteri(GL_PATCH_VERTICES, 3);
            DrainErrors();

            GLenum afterError = GL_NO_ERROR;
            const Image after = DrawTriangle(program, GL_PATCHES, &afterError);
            EXPECT_EQ(afterError, static_cast<GLenum>(GL_NO_ERROR)) << "the relinked tessellating program must draw";
            ExpectHalvedTriangle(after, "the draw after the tessellation stage was linked in");
            DrainErrors();
        }

    } // namespace
} // namespace MGITest
