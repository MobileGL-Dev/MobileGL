// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/MagmaVertexLayoutScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P7 wave 2 package C, CONTRACT-P7 §3.2's `vertex-layout` row: THE ATTRIBUTE VULKAN CANNOT MAP.
//
// WHAT THIS EXISTS TO CATCH, and it is not a picture. Until this package
// VertexInputStateFactory::BuildWireVertexInput answered SEVEN different device/format
// conditions and one protocol condition with the same `false`, and the single caller answered
// that `false` with a P7-marked MagmaWireFatal - which, before wave 0 funnelled it, was a bare
// std::abort(). So a Magma split session that met a GL_FIXED array, a dvec on a device with
// native fp64, or any format without VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT did not decline the
// draw: it KILLED THE PROCESS. The monolith arm on the very same device has always masked the
// attribute out of the vertex input state and then declined the draw loudly and once, which is
// the answer GL asks for. These cases pin that the two arms now agree.
//
// WHY GL_FIXED IS THE VEHICLE. It is the one unmappable shape that needs no device to cooperate:
// DataType::Fixed32 has no arm in ToVkVertexFormat at all, so the format map answers
// VK_FORMAT_UNDEFINED on every ICD including the lavapipe the host lanes run on. That matters
// because CONTRACT-P7 §2.5 records that host lavapipe reaches no other P7-marked refusal -
// the readback matrices and odd-offset uniform ranges that reach the rest need real hardware -
// so without this shape the whole `vertex-layout` retirement would have no host gate at all.
//
// DIRECTVULKAN ONLY, deliberately. The claim is about what Magma does with a format Vulkan
// cannot express; Espryt hands GL_FIXED straight to a driver that supports it and renders, so
// registering these on DirectGLES would assert a different fact under the same name.

#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"

#include <array>
#include <vector>

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
namespace {

    constexpr int kWidth = 16, kHeight = 16;

    // Reads BOTH attributes, so an array masked out of the vertex input state is one the
    // program actually consumes and the draw must decline rather than silently substitute the
    // current attribute value.
    constexpr const char* kVertexReadsBoth = R"(#version 430 core
layout(location=0) in vec2 inPos;
layout(location=1) in vec3 inTint;
out vec3 vTint;
void main() {
    vTint = inTint;
    gl_Position = vec4(inPos, 0.0, 1.0);
})";

    // Reads ONLY location 0. An enabled-but-unread array is not part of this program's vertex
    // input at all, so neither arm has anything to mask and the draw must land.
    constexpr const char* kVertexReadsPositionOnly = R"(#version 430 core
layout(location=0) in vec2 inPos;
out vec3 vTint;
void main() {
    vTint = vec3(0.0, 1.0, 0.0);
    gl_Position = vec4(inPos, 0.0, 1.0);
})";

    constexpr const char* kFragment = R"(#version 430 core
in vec3 vTint;
layout(location=0) out vec4 color;
void main() { color = vec4(vTint, 1.0); })";

    class MagmaVertexLayoutScenario : public ScenarioTest {
    protected:
        GLuint m_vao = 0, m_positions = 0, m_fixedAttrib = 0, m_program = 0;

        void SetUp() override {
            ScenarioTest::SetUp();
            if (!Ready()) return;
            if (Gl().BackendName() != "DirectVulkan") {
                GTEST_SKIP() << "the format map under test is Magma's; Espryt hands GL_FIXED to a "
                                "driver that supports it";
            }
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glViewport(0, 0, kWidth, kHeight);
        }

        void TearDown() override {
            if (m_program) glDeleteProgram(m_program);
            if (m_vao) glDeleteVertexArrays(1, &m_vao);
            if (m_positions) glDeleteBuffers(1, &m_positions);
            if (m_fixedAttrib) glDeleteBuffers(1, &m_fixedAttrib);
            ScenarioTest::TearDown();
        }

        GLuint MakeProgram(const char* vertexSource) {
            const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vs, 1, &vertexSource, nullptr);
            glCompileShader(vs);
            GLint ok = GL_FALSE;
            glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
            EXPECT_EQ(ok, GL_TRUE) << "vertex shader did not compile";
            const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(fs, 1, &kFragment, nullptr);
            glCompileShader(fs);
            glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
            EXPECT_EQ(ok, GL_TRUE) << "fragment shader did not compile";
            const GLuint program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            EXPECT_EQ(ok, GL_TRUE) << "program did not link";
            glDeleteShader(vs);
            glDeleteShader(fs);
            return program;
        }

        // A full-viewport triangle in location 0 and a GL_FIXED size-3 array in location 1.
        // GL_FIXED is 16.16 fixed point; the bytes are never fetched on the arm under test
        // (the attribute is masked out before any binding is built), so their VALUE matters
        // only for the unread case, where nothing reads them either.
        void BuildVaoWithAFixedAttribute() {
            const float positions[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
            const GLint fixedTint[9] = {0, 1 << 16, 0, 0, 1 << 16, 0, 0, 1 << 16, 0};

            glGenVertexArrays(1, &m_vao);
            glBindVertexArray(m_vao);

            glGenBuffers(1, &m_positions);
            glBindBuffer(GL_ARRAY_BUFFER, m_positions);
            glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glEnableVertexAttribArray(0);

            glGenBuffers(1, &m_fixedAttrib);
            glBindBuffer(GL_ARRAY_BUFFER, m_fixedAttrib);
            glBufferData(GL_ARRAY_BUFFER, sizeof(fixedTint), fixedTint, GL_STATIC_DRAW);
            // THE SHAPE UNDER TEST: size 3, GL_FIXED. ToVkVertexFormat has no Fixed32 arm, so
            // this resolves to VK_FORMAT_UNDEFINED and the wire builder's `format-map` reason
            // fires - the reason that used to abort the session.
            glVertexAttribPointer(1, 3, GL_FIXED, GL_FALSE, 0, nullptr);
            glEnableVertexAttribArray(1);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        std::array<GLubyte, 4> ClearDrawRead(GLuint program) {
            glClearColor(0.0f, 0.0f, 1.0f, 1.0f);  // blue: the "the draw did not land" colour
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(program);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            std::array<GLubyte, 4> pixel{};
            glReadPixels(kWidth / 2, kHeight / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            glBindVertexArray(0);
            glUseProgram(0);
            return pixel;
        }
    };

    // ------------------------------------------------------------------------------------
    // THE RETIREMENT ITSELF. The program READS the unmappable array, so the attribute is
    // masked out of the vertex input state and the draw is then declined - loudly, once, and
    // without touching the session. Before this package the same GL killed the process.
    //
    // THE ASSERTION IS THAT THE PROCESS IS STILL HERE and that the framebuffer still holds the
    // clear, which together say "declined" rather than "rendered wrong pixels". A gtest case
    // cannot assert the absence of an abort any more directly than by reaching its next
    // statement: the old behaviour took the whole binary down, so every case after this one in
    // the lane was equally the control.
    TEST_F(MagmaVertexLayoutScenario, AnUnmappableVertexFormatDeclinesTheDrawInsteadOfEndingTheSession) {
        if (!Ready() || IsSkipped()) return;
        m_program = MakeProgram(kVertexReadsBoth);
        BuildVaoWithAFixedAttribute();
        const auto pixel = ClearDrawRead(m_program);

        EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR))
            << "a declined draw must not invent a GL error: GL has no error for 'this "
               "implementation cannot fetch that array', and the monolith arm raises none";
        const int blue[4] = {0, 0, 255, 255};
        for (int i = 0; i < 4; ++i) {
            EXPECT_NEAR(pixel[i], blue[i], 1)
                << "component " << i << ": the draw was expected to DECLINE (leaving the clear "
                   "colour) because location 1 is an enabled GL_FIXED array the program reads "
                   "and Vulkan has no format for it. A non-clear pixel means the attribute was "
                   "silently replaced by the current vertex attribute value, which is the exact "
                   "wrong-pixels outcome the monolith arm's pre-flight refuses.";
        }
    }

    // ------------------------------------------------------------------------------------
    // THE OTHER HALF, and it is what keeps the case above from being satisfied by "decline
    // every draw that mentions GL_FIXED". The array is still enabled and still unmappable; the
    // program simply does not read it, so it is not part of this program's vertex input on
    // either arm and the draw must land normally.
    TEST_F(MagmaVertexLayoutScenario, AnUnreadUnmappableArrayDoesNotStopTheRestOfTheDraw) {
        if (!Ready() || IsSkipped()) return;
        m_program = MakeProgram(kVertexReadsPositionOnly);
        BuildVaoWithAFixedAttribute();
        const auto pixel = ClearDrawRead(m_program);

        EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the draw should have been ordinary";
        const int green[4] = {0, 255, 0, 255};
        for (int i = 0; i < 4; ++i) {
            EXPECT_NEAR(pixel[i], green[i], 1)
                << "component " << i << ": an enabled but UNREAD GL_FIXED array must not cost "
                   "the draw. Blue here means the whole draw was declined for an attribute "
                   "that is not part of this program's vertex input at all.";
        }
    }

} // namespace
} // namespace MGITest
