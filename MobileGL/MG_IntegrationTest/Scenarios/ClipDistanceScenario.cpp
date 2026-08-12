// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ClipDistanceScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - gl_ClipDistance ACTUALLY CLIPS, AND ONLY WHERE IT IS ENABLED.
//
// CapabilityInput::ClipDistance0..7 existed end to end - the GL enum converted to it, the
// string converter named it, glEnable(GL_CLIP_DISTANCE0 + i) raised no error - and then
// RenderState::SetCapability had no case for it and dropped it into `default: break`. Nothing
// was stored, no version was bumped, and neither backend ever heard about it. The shader half
// worked all along (SPIRV-Cross emits gl_ClipDistance with a
// `#extension GL_EXT_clip_cull_distance : require` that Adreno accepts), so the distances were
// computed and then ignored: no clipping ever happened on DirectGLES, which is the whole of
// KHR-GLxx.clip_distance.functional. glIsEnabled lied about it too - it returned GL_FALSE
// immediately after a successful glEnable.
//
// The assertions are behavioural, not query-shaped, because a query-only test passes against a
// backend that stores the bit and never forwards it. Each case draws one full-viewport triangle
// whose clip distance is positive on one side of the viewport and negative on the other, then
// checks BOTH sides: the kept side proves the draw happened at all, and the clipped side is the
// actual claim. The disabled case is the negative control - the identical shader with the
// identical distances and the enable turned off must leave both sides painted, which is what
// says the pixels below are being removed by clipping and not by something else.
//
// HONEST LIMIT OF THIS FILE IN CI. Of the four cases, only EnableIsObservableThroughIsEnabled is
// falsifiable on the software rasterizers every automated lane runs on. llvmpipe and lavapipe
// clip by EVERY declared gl_ClipDistance regardless of the enables, so
// AnEnabledClipDistanceRemovesTheNegativeHalf goes green there against the broken tree as well,
// and the two cases that need real per-distance semantics skip (see
// DriverHonoursPerDistanceEnables). What actually pins the behaviour is Adreno, through
// KHR-GLxx.clip_distance.functional - whose "without dynamic redeclaration" variants declare all
// gl_MaxClipDistances slots and enable only the first N, i.e. exactly the subset semantics these
// skipped cases assert. Read a green CI run here as "the state survives the frontend", not as
// "clipping is correct"; the second claim is a device claim.
//
// Every case disables all eight distances on entry rather than assuming they start off:
// XfbAfterClipDistanceScenario deliberately leaves one enabled for the rest of the process, and
// forwarding the enables is what turned that leftover from inert bookkeeping into live driver
// state.

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

#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif
#ifndef GL_CLIP_DISTANCE1
#define GL_CLIP_DISTANCE1 0x3001
#endif

namespace MGITest {
    namespace {

        // One clip distance per half of the viewport: distance 0 is positive on the right half
        // (x > 0 in clip space) and distance 1 is positive on the top half. A vertex shader
        // producing a full-screen triangle from gl_VertexID, so no buffers are needed.
        const char* const kVertexSource = R"(#version 400 core
out float gl_ClipDistance[2];
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = positions[gl_VertexID];
    gl_Position = vec4(p, 0.0, 1.0);
    gl_ClipDistance[0] = p.x;
    gl_ClipDistance[1] = p.y;
}
)";

        const char* const kFragmentSource = R"(#version 400 core
out vec4 fragColor;
void main() { fragColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        class ClipDistanceScenario : public ScenarioTest {
        protected:
            GLuint BuildProgram() {
                const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                glShaderSource(vs, 1, &kVertexSource, nullptr);
                glCompileShader(vs);
                GLint compiled = 0;
                glGetShaderiv(vs, GL_COMPILE_STATUS, &compiled);
                if (!compiled) {
                    m_buildLog = ShaderLog(vs);
                    glDeleteShader(vs);
                    return 0;
                }
                const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
                glShaderSource(fs, 1, &kFragmentSource, nullptr);
                glCompileShader(fs);
                glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
                if (!compiled) {
                    m_buildLog = ShaderLog(fs);
                    glDeleteShader(vs);
                    glDeleteShader(fs);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, fs);
                glLinkProgram(program);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                glDeleteShader(vs);
                glDeleteShader(fs);
                if (!linked) {
                    GLint length = 0;
                    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                    std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
                    glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
                    m_buildLog = log.data();
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            const std::string& BuildLog() const { return m_buildLog; }

            // Paints the whole viewport red, then draws the clipped triangle in green.
            void DrawClippedTriangle(GLuint program, GLuint vao) const {
                glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(program);
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }

            static bool IsGreen(const unsigned char* px) {
                return px[0] < 64 && px[1] > 192;
            }

            static bool IsRed(const unsigned char* px) {
                return px[0] > 192 && px[1] < 64;
            }

            void PixelAt(int x, int y, unsigned char* out) const {
                glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
            }

            // Never assume the eight start disabled - see the header note about
            // XfbAfterClipDistanceScenario leaving one on for the rest of the process.
            static void DisableEveryClipDistance() {
                for (int i = 0; i < 8; ++i) {
                    glDisable(static_cast<GLenum>(GL_CLIP_DISTANCE0 + i));
                }
            }

            // True when the driver under this backend actually implements PER-DISTANCE enable
            // state, i.e. when a written-but-disabled gl_ClipDistance leaves its fragments
            // alone. Not every stack does, and the difference is not MobileGL's to hide:
            //
            //   - Adreno's ES driver honours GL_CLIP_DISTANCE0_EXT..7_EXT, which is what makes
            //     KHR-GLxx.clip_distance.functional pass on the device once the enables are
            //     forwarded at all.
            //   - Vulkan has no such state: every clip distance a shader declares is active,
            //     always. DirectVulkan therefore clips by a disabled distance.
            //   - Mesa's llvmpipe ES driver behaves like Vulkan here.
            //
            // Emulating GL's semantics on those two would mean forcing the disabled slots to a
            // non-negative value inside the shader, which makes the enable mask part of the
            // pipeline key - a feature, not a fix, and deliberately not attempted here. The
            // cases that need the real semantics gate on this probe and say so when they skip,
            // rather than being deleted or silently weakened.
            bool DriverHonoursPerDistanceEnables(GLuint program, GLuint vao) const {
                for (int i = 0; i < 8; ++i) {
                    glDisable(static_cast<GLenum>(GL_CLIP_DISTANCE0 + i));
                }
                DrawClippedTriangle(program, vao);
                unsigned char negativeSide[4] = {0, 0, 0, 0};
                glReadPixels(Gl().Width() / 4, Gl().Height() / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, negativeSide);
                return IsGreen(negativeSide);
            }

        private:
            static std::string ShaderLog(GLuint shader) {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
                glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
                return log.data();
            }

            std::string m_buildLog;
        };

    } // namespace

    // The state itself: glEnable must be observable through glIsEnabled. This is the cheap half
    // of the bug - SetCapability's missing case made the query answer GL_FALSE for a capability
    // that had just been enabled without error.
    TEST_F(ClipDistanceScenario, EnableIsObservableThroughIsEnabled) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();

        DisableEveryClipDistance();
        EXPECT_EQ(glIsEnabled(GL_CLIP_DISTANCE0), GL_FALSE)
            << "glDisable(GL_CLIP_DISTANCE0) is not observable through glIsEnabled";
        glEnable(GL_CLIP_DISTANCE0);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(glIsEnabled(GL_CLIP_DISTANCE0), GL_TRUE)
            << "glEnable(GL_CLIP_DISTANCE0) raised no error but glIsEnabled still reports it disabled";
        EXPECT_EQ(glIsEnabled(GL_CLIP_DISTANCE1), GL_FALSE)
            << "enabling distance 0 must not enable distance 1 - the eight are independent";

        glEnable(GL_CLIP_DISTANCE1);
        glDisable(GL_CLIP_DISTANCE0);
        EXPECT_EQ(glIsEnabled(GL_CLIP_DISTANCE0), GL_FALSE);
        EXPECT_EQ(glIsEnabled(GL_CLIP_DISTANCE1), GL_TRUE);

        glDisable(GL_CLIP_DISTANCE1);
        EXPECT_EQ(FirstGLError(), 0u);
        gl.EndFrame();
    }

    // The claim: an enabled clip distance removes the fragments where it is negative.
    TEST_F(ClipDistanceScenario, AnEnabledClipDistanceRemovesTheNegativeHalf) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 8);
        ASSERT_GE(height, 8);

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        const GLuint program = BuildProgram();
        ASSERT_NE(program, 0u) << "the gl_ClipDistance program did not build: " << BuildLog();

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        // Distance 1 is positive by a single pixel at the sampled row, so a stray enable on it
        // would put the "kept" probe right on the clip boundary.
        DisableEveryClipDistance();
        glEnable(GL_CLIP_DISTANCE0);
        DrawClippedTriangle(program, vao);
        EXPECT_EQ(FirstGLError(), 0u);

        unsigned char right[4] = {0, 0, 0, 0};
        unsigned char left[4] = {0, 0, 0, 0};
        PixelAt(width - 1 - width / 4, height / 2, right);
        PixelAt(width / 4, height / 2, left);
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(IsGreen(right)) << "the kept half is not painted (" << int(right[0]) << "," << int(right[1])
                                    << "," << int(right[2]) << ") - the draw itself did not happen, so the clipped "
                                       "half below proves nothing";
        EXPECT_TRUE(IsRed(left)) << "gl_ClipDistance[0] is negative on the left half and GL_CLIP_DISTANCE0 is "
                                    "enabled, so those fragments must be clipped away; found ("
                                 << int(left[0]) << "," << int(left[1]) << "," << int(left[2]) << ")";

        glDisable(GL_CLIP_DISTANCE0);
        glUseProgram(0);
        glBindVertexArray(0);
        glDeleteProgram(program);
        glDeleteVertexArrays(1, &vao);
        gl.EndFrame();
    }

    // The negative control: the same shader writing the same distances, with the enable off,
    // must paint both halves. Without this a backend that clipped everything - or one whose
    // draw simply failed - would pass the case above.
    TEST_F(ClipDistanceScenario, ADisabledClipDistanceRemovesNothing) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 8);
        ASSERT_GE(height, 8);

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        const GLuint program = BuildProgram();
        ASSERT_NE(program, 0u) << "the gl_ClipDistance program did not build: " << BuildLog();

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        DisableEveryClipDistance();

        DrawClippedTriangle(program, vao);
        EXPECT_EQ(FirstGLError(), 0u);

        unsigned char right[4] = {0, 0, 0, 0};
        unsigned char left[4] = {0, 0, 0, 0};
        PixelAt(width - 1 - width / 4, height / 2, right);
        PixelAt(width / 4, height / 2, left);
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(IsGreen(right)) << "with every clip distance disabled the whole triangle must survive";
        const bool driverHonoursEnables = IsGreen(left);

        glUseProgram(0);
        glBindVertexArray(0);
        glDeleteProgram(program);
        glDeleteVertexArrays(1, &vao);
        gl.EndFrame();
        if (!driverHonoursEnables) {
            GTEST_SKIP() << "renderer " << gl.RendererString()
                         << " clips by a DISABLED gl_ClipDistance - it does not implement per-distance enable state "
                            "(see DriverHonoursPerDistanceEnables). Emulating GL's semantics there needs shader-side "
                            "masking keyed on the enable mask, which is a separate feature";
        }
    }

    // The eight enables are independent: enabling only distance 1 must clip by distance 1 and
    // leave distance 0 alone. A backend that forwarded "any clip distance enabled" as a single
    // bit, or that always enables every declared distance (which is what Vulkan does natively),
    // passes both cases above and fails this one.
    TEST_F(ClipDistanceScenario, TheEnablesAreIndependentPerDistance) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 8);
        ASSERT_GE(height, 8);

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        const GLuint program = BuildProgram();
        ASSERT_NE(program, 0u) << "the gl_ClipDistance program did not build: " << BuildLog();

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        if (!DriverHonoursPerDistanceEnables(program, vao)) {
            glUseProgram(0);
            glBindVertexArray(0);
            glDeleteProgram(program);
            glDeleteVertexArrays(1, &vao);
            DisableEveryClipDistance();
            gl.EndFrame();
            GTEST_SKIP() << "renderer " << gl.RendererString()
                         << " clips by every declared gl_ClipDistance regardless of the enables, so per-distance "
                            "independence is not observable here";
        }

        DisableEveryClipDistance();
        glEnable(GL_CLIP_DISTANCE1);

        DrawClippedTriangle(program, vao);
        EXPECT_EQ(FirstGLError(), 0u);

        // Distance 1 is negative on the bottom half, distance 0 on the left half. With only
        // distance 1 enabled, the bottom-left must survive (distance 0 is off) and the bottom
        // must not.
        unsigned char topLeft[4] = {0, 0, 0, 0};
        unsigned char bottomRight[4] = {0, 0, 0, 0};
        PixelAt(width / 4, height - 1 - height / 4, topLeft);
        PixelAt(width - 1 - width / 4, height / 4, bottomRight);
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(IsGreen(topLeft)) << "gl_ClipDistance[0] is negative here but GL_CLIP_DISTANCE0 is disabled, so "
                                         "this fragment must survive";
        EXPECT_TRUE(IsRed(bottomRight)) << "gl_ClipDistance[1] is negative here and GL_CLIP_DISTANCE1 is enabled, so "
                                           "this fragment must be clipped";

        glDisable(GL_CLIP_DISTANCE1);
        glUseProgram(0);
        glBindVertexArray(0);
        glDeleteProgram(program);
        glDeleteVertexArrays(1, &vao);
        gl.EndFrame();
    }

} // namespace MGITest
