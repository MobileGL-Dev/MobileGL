// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SnormAttachmentScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - SIGNED-NORMALIZED COLOUR ATTACHMENTS, on a live driver.
//
// The bug: a GLES driver without GL_EXT_render_snorm treats every signed-normalized format as
// texture-only. DirectGLES had a colour-renderable substitute for exactly one of the eight
// (GL_RGB16_SNORM, through the three-channel widening), so an R8_SNORM or R16_SNORM attachment got
// no storage the driver would render into: the ES framebuffer was incomplete, the draw landed
// nowhere, and glGetTexImage fell through to the CPU shadow - all zeroes for a texture created with
// no data. KHR-GL4x.texture_swizzle renders into a SINGLE-CHANNEL SNORM output for every one of its
// SNORM source formats, which is why all 46 of its GL43 SNORM cases failed on Mali.
//
// THE OTHER HALF, and the reason this scenario asserts VALUES rather than only completeness: the
// substitute has to be exact. A half float's 11-bit mantissa cannot represent a 16-bit SNORM
// channel - 23451/32767 quantizes about six SNORM steps away, against a conformance window of one -
// so the 16-bit formats must land on a 32-bit float even though the 8-bit ones are fine in a half.
// Trading 46 visible failures for silent precision loss in Iris' SNORM normal buffers would be the
// worse outcome, so the round trip below is pinned tightly enough to fail on a half-float substitute
// (tolerance two SNORM steps, half-float error six).
//
// WHAT THIS GATE CAN AND CANNOT SEE. Both CI drivers (Mesa llvmpipe) and Adreno expose
// GL_EXT_render_snorm, so they take the NATIVE path here and the substitution stays dead. That is
// precisely why the assertions are written as invariants of the format rather than of the fallback:
// "a signed-normalized colour attachment is complete and round-trips its channel values" has to
// hold whichever path answers it, so the scenario fails if anyone ever routes these formats to a
// lossy storage on a driver where it IS live. The substitution itself can only be observed on a
// device without EXT_render_snorm (Mali Immortalis-G925).
//
// DirectGLES only, like the three-channel scenario next door: DirectVulkan resolves SNORM formats
// on its own terms and asserting Espryt's answers there would pin a coincidence.

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
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        // A uniform rather than a literal so nothing can constant-fold the value into a different
        // precision than the one the attachment stores.
        constexpr const char* kFS = R"(#version 330 core
out vec4 oColor;
uniform float uValue;
void main() { oColor = vec4(uValue, 0.0, 0.0, 1.0); }
)";

        constexpr int kSize = 8;

        // The two channel values the round trip is pinned on. Both are positive on purpose:
        // glReadPixels applies GL_CLAMP_READ_COLOR (GL_FIXED_ONLY by default) to a fixed-point
        // colour buffer, so the negative half of a SNORM attachment reads back as 0 and would
        // measure the clamp instead of the storage.
        constexpr int kSnorm8Value = 99;
        constexpr int kSnorm16Value = 23451;

        class SnormAttachmentScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (Gl().BackendName() != "DirectGLES") {
                    GTEST_SKIP() << "the signed-normalized substitution is a DirectGLES fallback; backend is "
                                 << Gl().BackendName();
                }
            }

            // A single-level 2D texture in `internalFormat`, or 0 when the driver rejects the
            // storage outright (which is a different failure from rejecting the ATTACHMENT).
            static GLuint MakeTexture(GLenum internalFormat) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, kSize, kSize);
                if (glGetError() != GL_NO_ERROR) {
                    glDeleteTextures(1, &texture);
                    return 0;
                }
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                return texture;
            }

            static GLenum SingleAttachmentStatus(GLenum internalFormat) {
                const GLuint texture = MakeTexture(internalFormat);
                if (texture == 0) return GL_NONE;
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
                const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &texture);
                return status;
            }

            // Renders `value` into the red channel of a fresh `internalFormat` attachment and hands
            // back what glReadPixels sees. Returns false when the framebuffer never came up, which
            // is the failure mode this scenario exists for - a draw into an incomplete framebuffer
            // is dropped by the driver and leaves the caller reading the cleared texture.
            bool RenderAndReadRed(GLenum internalFormat, float value, float* outRed) {
                std::string error;
                const GLuint program = CompileProgram(kVS, kFS, &error);
                EXPECT_NE(program, 0u) << error;
                if (program == 0) return false;
                const GLint valueLocation = glGetUniformLocation(program, "uValue");
                EXPECT_GE(valueLocation, 0);

                const GLuint texture = MakeTexture(internalFormat);
                EXPECT_NE(texture, 0u) << "the driver refused the texture storage itself";
                if (texture == 0) {
                    glDeleteProgram(program);
                    return false;
                }

                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
                const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

                if (complete) {
                    const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
                    GLuint vao = 0;
                    GLuint vbo = 0;
                    glGenVertexArrays(1, &vao);
                    glBindVertexArray(vao);
                    glGenBuffers(1, &vbo);
                    glBindBuffer(GL_ARRAY_BUFFER, vbo);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
                    glUseProgram(program);
                    glUniform1f(valueLocation, value);
                    glViewport(0, 0, kSize, kSize);
                    // Cleared to zero so a dropped draw cannot be mistaken for a correct one.
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                    std::vector<float> pixels(static_cast<std::size_t>(kSize) * kSize * 4, -1.0f);
                    glReadBuffer(GL_COLOR_ATTACHMENT0);
                    glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_FLOAT, pixels.data());
                    if (outRed) *outRed = pixels[0];

                    glDeleteBuffers(1, &vbo);
                    glDeleteVertexArrays(1, &vao);
                }

                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &texture);
                glDeleteProgram(program);
                return complete;
            }
        };

        // THE regression gate for the frontend's answer. Every one of these used to be
        // GL_FRAMEBUFFER_UNSUPPORTED on a driver without EXT_render_snorm, and nothing in the CTS
        // (or in Iris) checks the status before drawing, so the failure was silent all the way to a
        // readback of zeroes.
        TEST_F(SnormAttachmentScenario, SignedNormalizedColorAttachmentsReportComplete) {
            if (!Ready() || IsSkipped()) return;

            // GL_R8 is the control: colour-renderable in ES core, so it must pass with or without
            // any substitution. If it ever fails, nothing below means anything.
            EXPECT_EQ(SingleAttachmentStatus(GL_R8), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "GL_R8 is ES-core colour-renderable";

            // The single-channel pair KHR-GL4x.texture_swizzle renders into for every SNORM source
            // format - the whole 46-case failure.
            EXPECT_EQ(SingleAttachmentStatus(GL_R8_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            EXPECT_EQ(SingleAttachmentStatus(GL_R16_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            // ...and the two- and four-channel siblings, which are what a shaderpack actually
            // declares (Iris colortex buffers in RGBA16_SNORM).
            EXPECT_EQ(SingleAttachmentStatus(GL_RG8_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            EXPECT_EQ(SingleAttachmentStatus(GL_RG16_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            EXPECT_EQ(SingleAttachmentStatus(GL_RGBA8_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            EXPECT_EQ(SingleAttachmentStatus(GL_RGBA16_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // The other half: whatever storage answers for the attachment has to hold the channel value
        // to the format's own precision. This is the assertion that fails if the 16-bit formats are
        // ever routed to a half float - the substitute an implementer naturally reaches for, because
        // it is what the 8-bit ones correctly use.
        TEST_F(SnormAttachmentScenario, SignedNormalizedAttachmentsRoundTripTheirChannelValues) {
            if (!Ready() || IsSkipped()) return;

            const float snorm8Expected = static_cast<float>(kSnorm8Value) / 127.0f;
            float red8 = -1.0f;
            ASSERT_TRUE(RenderAndReadRed(GL_R8_SNORM, snorm8Expected, &red8))
                << "an R8_SNORM colour attachment must be complete before any value can be asserted";
            // Two 8-bit SNORM steps. A half float is exact here (worst case 0.03 of a step), so this
            // only has to catch a storage that quantizes harder than the format itself.
            EXPECT_NEAR(red8, snorm8Expected, 2.0f / 127.0f)
                << "R8_SNORM attachment lost its channel value";
            EXPECT_GT(red8, 0.5f) << "the draw never landed - this is the cleared texture, not the rendered one";

            const float snorm16Expected = static_cast<float>(kSnorm16Value) / 32767.0f;
            float red16 = -1.0f;
            ASSERT_TRUE(RenderAndReadRed(GL_R16_SNORM, snorm16Expected, &red16))
                << "an R16_SNORM colour attachment must be complete before any value can be asserted";
            // Two 16-bit SNORM steps (6.1e-5). A half float would land 1.9e-4 away - three times
            // this window - which is exactly the failure this bound exists to catch.
            EXPECT_NEAR(red16, snorm16Expected, 2.0f / 32767.0f)
                << "R16_SNORM attachment was stored in something that cannot hold 16 signed bits";
            EXPECT_GT(red16, 0.5f) << "the draw never landed - this is the cleared texture, not the rendered one";

            float red16x4 = -1.0f;
            ASSERT_TRUE(RenderAndReadRed(GL_RGBA16_SNORM, snorm16Expected, &red16x4))
                << "an RGBA16_SNORM colour attachment must be complete before any value can be asserted";
            EXPECT_NEAR(red16x4, snorm16Expected, 2.0f / 32767.0f)
                << "RGBA16_SNORM attachment was stored in something that cannot hold 16 signed bits";

            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

    } // namespace
} // namespace MGITest
