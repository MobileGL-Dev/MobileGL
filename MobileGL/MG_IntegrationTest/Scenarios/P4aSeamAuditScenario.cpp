// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/P4aSeamAuditScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE FOUR SEAMS THE P4a FABLE SEAM AUDIT PROVED, each pinned by the public-GL sequence
// (or the white-box reading) that was red on the tree the audit read and is green with its fix.
//
// The audit's rule, which every case here is an instance of: EVERY FIELD OF EVERY EMITTED RECORD
// NAMES THE FRONTEND SETTER THAT CHANGES IT, AND THAT SETTER MOVES A COUNTER THE EMITTING BIT'S
// SHUTTER READS. A record whose field has a setter no shutter sees is a stale record with nothing
// to refuse - no census line, no Fatal, a wrong picture or a permanent silent fallback - which is
// why none of the 80 scenarios before this file caught any of the four (Tracker.h carries the
// record-field -> setter -> shutter table this file is the gate for).
//
//   F-3  set_framebuffer_state INLINES an attachment's format (D-C1) and a storage redefinition of
//        an ATTACHED texture or renderbuffer moved nothing bit 11 read: Espryt's handle arm then
//        answered its alpha-widening / snorm-clamp / integer masks from the stale copy. AND THE
//        PRE-HANDLE ARM WAS NOT FRESH EITHER, which these cases found on the 0x1ff / 0 / pull
//        lanes: a redefinition that keeps the driver id (mutable texture storage regenerated in
//        place, a renderbuffer re-storaged in place) moves neither the framebuffer's frontend
//        versions nor the backend-id generation the FBO memo reads, so SyncToBackend never
//        re-ran and the masks stayed on both arms. The texture half is fixed on both arms (an
//        in-place regeneration now takes the same generation a re-mint takes); the renderbuffer
//        half only on the handle arm, where the resource record carries the re-storage - on the
//        pre-handle arm a renderbuffer's twin is only ever reached from inside the FBO walk the
//        memo skips (D-D2's documented hole, pre-P4a code), so that case asserts on the handle
//        arm and declines by name elsewhere. Three cases, both directions, texture and
//        renderbuffer. DirectGLES only: the masks are Espryt's substitution machinery.
//   F-1  set_sampler_views is resolved for the PROGRAM IN USE and bit 12's shutter read no program
//        input, so a glUseProgram alone never re-emitted it; E's record epoch (the two set serials)
//        then kept the program-independent texture sync list from ever rebuilding, and a texture
//        bound to an EMPTY slot under one program was never synced for the next. One case, both
//        backends, red as a black quad.
//   F-2  bit 14's plain-program arm mixed a per-program COUNTER two programs routinely share, so a
//        program switch never re-emitted set_shader_images and the window stayed the previous
//        program's - and E's SD-4 (a buffer image never reaching the record at all) is the same
//        bit through the null -> program transition. One case, white-box, both backends run it.
//   F-4  BindCurrentUnitSamplers' record arm looked a CONTENT-addressed CSO handle up in the
//        IDENTITY-keyed twin registry: a miss on every draw, hidden because the pre-handle program
//        pass bound the same values. One case, white-box: the unit's driver sampler must be the
//        CSO's own twin.
//
// A WHITE-BOX READING THAT CANNOT BE TAKEN IS DECLINED BY NAME AND THE CASE CONTINUES with its
// public-GL half (the shape TextureParamsWithoutASamplerViewScenario.cpp argues for): a pull
// build, Magma, or a lane whose mask leaves Espryt's sampler family on its legacy arm has no
// record arm to assert about, and skipping the whole case there would delete the public-GL
// verdict those lanes carry. Every decline is printed and RecordProperty'd.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/P4aSeamPeek.h"
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

        constexpr int kSize = 16;
        constexpr int kInset = 2;

        // No attributes: the quad's corners come from gl_VertexID, so a bare VAO is all a draw
        // needs and no vertex-input state can enter any of the sequences below.
        constexpr const char* kQuadVS = R"(#version 330 core
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,
                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    gl_Position = vec4(corner, 0.0, 1.0);
}
)";

        constexpr const char* kColorFS = R"(#version 330 core
uniform vec4 uColor;
out vec4 oColor;
void main() { oColor = uColor; }
)";

        // texelFetch, so WHICH image the unit holds is the whole answer and no filter, wrap or
        // completeness rule can explain a colour away.
        constexpr const char* kFetchFS = R"(#version 330 core
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texelFetch(uTex, ivec2(0, 0), 0); }
)";

        // texture() at (1.5, 1.5): outside the image on both axes, so the WRAP mode of whichever
        // sampler applies - the unit's sampler object or the texture's built-in one - decides
        // whether the texel or the border colour comes back.
        constexpr const char* kOutsideSampleFS = R"(#version 330 core
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vec2(1.5, 1.5)); }
)";

        // F-2 / SD-4: two compute programs over BUFFER images (the SD-4 shape - the kind E's I2
        // flip found never reached the record at all), the second naming one unit more than the
        // first, and both image-unit counters equal (layout(binding) assigns the unit at link, so
        // neither program ever moves it through glUniform1i).
        constexpr const char* kOneBufferImageCS = R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 0, r32ui) writeonly uniform uimageBuffer i0;
void main() { imageStore(i0, 0, uvec4(7u, 0u, 0u, 0u)); }
)";

        constexpr const char* kTwoBufferImagesCS = R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 0, r32ui) readonly uniform uimageBuffer i0;
layout(binding = 1, r32ui) writeonly uniform uimageBuffer i1;
void main() { imageStore(i1, 0, imageLoad(i0, 0) + uvec4(2u, 0u, 0u, 0u)); }
)";

        class P4aSeamAuditScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glDisable(GL_BLEND);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            // The F-3 cases are about Espryt's four cross-object masks, which are its own
            // substitution machinery (three-channel widening, SNORM/UNORM clamp, integer outputs);
            // Magma answers the same GL questions on its own terms, so a verdict there would pin
            // a coincidence - the same reason SnormAttachment and ThreeChannelAttachment skip.
            // Marks the case skipped; the caller tests IsSkipped() and returns (GTEST_SKIP is a
            // void statement, so it cannot return the verdict itself).
            void SkipUnlessEspryt(const char* what) {
                if (Gl().BackendName() == "DirectGLES") return;
                GTEST_SKIP() << what << " is a DirectGLES handle-arm seam; backend is " << Gl().BackendName();
            }

            static void DrawQuad() { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }

            // One pixel's RGBA as floats, from the currently bound READ framebuffer.
            static void ReadPixelFloat(int x, int y, float out[4]) {
                out[0] = out[1] = out[2] = out[3] = -1.0f;
                glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, out);
            }

            // The white-box gate shared by F-2 and F-4: true when Espryt's sampler family is on
            // its handle arm in this process, so the applier's unit sets are consumed and an
            // assertion about them can only be red for its own reason. Prints the decline.
            bool SamplerHandleArmIsLive(const char* what) {
                bool live = false;
                std::string why;
                if (!PeekEsprytSamplerHandleArmIsLive(&live)) {
                    why = "the reading cannot be taken here (a pull build, Android, or a backend that "
                          "is not DirectGLES)";
                } else if (!live) {
                    why = "Espryt's sampler family runs its legacy arm in this process "
                          "(MOBILEGL_PIPE_PUSH leaves bit 11 clear or refuses it)";
                }
                if (why.empty()) return true;
                std::cout << "[ P4aSeamAudit ] white-box reading DECLINED for " << what << ": " << why
                          << "; the public-GL half of the case still runs" << std::endl;
                RecordProperty("p4a_seam_white_box", "declined");
                RecordProperty("p4a_seam_white_box_reason", why);
                return false;
            }

            // A 2x2 RGBA8 texture filled with one colour, NEAREST, single level - complete under
            // every rule, so nothing about completeness can enter the F-1 and F-4 sequences.
            static GLuint MakeSolidTexture2D(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                std::uint8_t texels[2 * 2 * 4];
                for (int i = 0; i < 4; ++i) {
                    texels[i * 4 + 0] = r;
                    texels[i * 4 + 1] = g;
                    texels[i * 4 + 2] = b;
                    texels[i * 4 + 3] = 255;
                }
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                return texture;
            }

            bool ComputeImagesAreUsable() const {
                GLint maxImageUnits = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                GLint maxComputeImageUniforms = 0;
                glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &maxComputeImageUniforms);
                GLint maxBufferSize = 0;
                glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxBufferSize);
                while (glGetError() != GL_NO_ERROR) {
                }
                return maxImageUnits >= 2 && maxComputeImageUniforms >= 2 && maxBufferSize >= 4;
            }

            static GLuint MakeComputeProgram(const char* source, std::string* outError) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[2048] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    *outError = std::string("compute shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[2048] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    *outError = std::string("compute program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            // An R32UI buffer texture over a fresh 4-texel buffer, every texel `fill`.
            static GLuint MakeBufferTexture(GLuint* outBuffer, GLuint fill) {
                const GLuint texels[4] = {fill, fill, fill, fill};
                glGenBuffers(1, outBuffer);
                glBindBuffer(GL_TEXTURE_BUFFER, *outBuffer);
                glBufferData(GL_TEXTURE_BUFFER, sizeof(texels), texels, GL_DYNAMIC_COPY);
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_BUFFER, texture);
                glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, *outBuffer);
                return texture;
            }

            static GLuint ReadBufferTexel0(GLuint buffer) {
                GLuint value = 0xFFFFFFFFu;
                glBindBuffer(GL_TEXTURE_BUFFER, buffer);
                glGetBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(value), &value);
                return value;
            }

            GLuint m_vao = 0;
        };

        // -----------------------------------------------------------------------------------
        // F-3: a storage redefinition WHILE ATTACHED reaches the framebuffer record
        // -----------------------------------------------------------------------------------
        //
        // GL_SRGB8 is a format Espryt can only render into through its three-channel widening
        // (llvmpipe reports INCOMPLETE_ATTACHMENT for it natively - ThreeChannelAttachmentScenario
        // measured the table), so its draw buffer carries the alpha-widened mask: every draw has its
        // alpha masked off so the stored alpha stays at the 1.0 a three-channel format implies.
        // Redefine the same attached texture as GL_SRGB8_ALPHA8 and the application owns alpha
        // again - the mask must clear. On the tree the audit read the record still said SRGB8, the
        // handle arm kept masking, and the 0.25 this case draws never reached the storage; on the
        // pre-handle arm the twin regenerated the (mutable) storage on the same driver id, nothing
        // the FBO memo reads moved, and the masks stayed the same way.

        TEST_F(P4aSeamAuditScenario, ATextureRespecifiedWhileAttachedReachesTheFramebufferRecord) {
            if (!Ready()) return;
            SkipUnlessEspryt("F-3");
            if (IsSkipped()) return;

            std::string error;
            const GLuint program = CompileProgram(kQuadVS, kColorFS, &error);
            ASSERT_NE(program, 0u) << error;
            const GLint colorLocation = glGetUniformLocation(program, "uColor");
            ASSERT_GE(colorLocation, 0);

            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8, kSize, kSize, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ASSERT_EQ(FirstGLError(), 0u) << "the SRGB8 texture was refused";

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "an SRGB8 colour attachment must be complete (natively or through the widening)";
            glViewport(0, 0, kSize, kSize);
            glUseProgram(program);
            glUniform4f(colorLocation, 0.0f, 1.0f, 0.0f, 0.25f);

            // Phase 1: the three-channel format. Whatever the draw writes, GL reports alpha 1.0.
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            DrawQuad();
            float pixel[4];
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            ReadPixelFloat(kSize / 2, kSize / 2, pixel);
            EXPECT_NEAR(pixel[3], 1.0f, 0.02f) << "a three-channel attachment reports alpha 1.0";

            // Phase 2: THE RESPECIFY, while attached, with no re-attach and no rebind of the FBO.
            // The only thing that moves between the two draws is the texture's storage.
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            ASSERT_EQ(FirstGLError(), 0u) << "the respecify to SRGB8_ALPHA8 was refused";
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            DrawQuad();
            ReadPixelFloat(kSize / 2, kSize / 2, pixel);
            EXPECT_NEAR(pixel[1], 1.0f, 0.05f) << "the draw did not land at all";
            EXPECT_NEAR(pixel[3], 0.25f, 0.02f)
                << "the draw's alpha never reached a four-channel attachment: the framebuffer record "
                   "(handle arm) or the FBO twin's memo (pre-handle arm) still describes the "
                   "three-channel storage the texture was attached with, so alpha stayed masked off (F-3)";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &texture);
            glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // The renderbuffer twin. A renderbuffer's three storage setters bump no version at all:
        // D-D2 closed the RESOURCE record by emitting from the entry point and left the framebuffer
        // record - and with it the masks - describing the storage it was attached with.
        TEST_F(P4aSeamAuditScenario, ARenderbufferRestoragedWhileAttachedReachesTheFramebufferRecord) {
            if (!Ready()) return;
            SkipUnlessEspryt("F-3");
            if (IsSkipped()) return;

            std::string error;
            const GLuint program = CompileProgram(kQuadVS, kColorFS, &error);
            ASSERT_NE(program, 0u) << error;
            const GLint colorLocation = glGetUniformLocation(program, "uColor");
            ASSERT_GE(colorLocation, 0);

            GLuint renderbuffer = 0;
            glGenRenderbuffers(1, &renderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_SRGB8, kSize, kSize);
            ASSERT_EQ(FirstGLError(), 0u) << "the SRGB8 renderbuffer was refused";

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "an SRGB8 renderbuffer attachment must be complete (natively or through the widening)";
            glViewport(0, 0, kSize, kSize);
            glUseProgram(program);
            glUniform4f(colorLocation, 0.0f, 1.0f, 0.0f, 0.25f);

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            DrawQuad();
            float pixel[4];
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            ReadPixelFloat(kSize / 2, kSize / 2, pixel);
            EXPECT_NEAR(pixel[3], 1.0f, 0.02f) << "a three-channel attachment reports alpha 1.0";

            // THE RE-STORAGE, while attached.
            glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_SRGB8_ALPHA8, kSize, kSize);
            ASSERT_EQ(FirstGLError(), 0u) << "the re-storage to SRGB8_ALPHA8 was refused";
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            DrawQuad();
            ReadPixelFloat(kSize / 2, kSize / 2, pixel);
            EXPECT_NEAR(pixel[1], 1.0f, 0.05f) << "the draw did not land at all";
            // THE HANDLE ARM ONLY. On the pre-handle arm a renderbuffer's twin is reached only from
            // inside the FBO walk, and nothing that walk's memo reads moves on glRenderbufferStorage
            // - the frontend setters bump no version (D-D2), no framebuffer version sees them, and
            // the twin that would bump the backend generation is exactly what the memo skips. That
            // is pre-P4a code and D-D2's documented hole; the resource record is what closes it,
            // so the verdict is taken where the record is consumed and declined by name elsewhere
            // (measured: alpha 1.0 on the pull build and at 0x1ff / 0, the mask of the storage the
            // renderbuffer was attached with).
            bool framebufferArmLive = false;
            if (PeekEsprytFramebufferHandleArmIsLive(&framebufferArmLive) && framebufferArmLive) {
                EXPECT_NEAR(pixel[3], 0.25f, 0.02f)
                    << "the draw's alpha never reached the four-channel renderbuffer: the framebuffer "
                       "record still describes the storage it was attached with (F-3)";
            } else {
                std::cout << "[ P4aSeamAudit ] renderbuffer re-storage verdict DECLINED on the pre-handle arm "
                             "(D-D2's documented hole: no frontend version and no backend generation moves on a "
                             "renderbuffer re-storage until the FBO walk the memo skips); alpha read "
                          << pixel[3] << std::endl;
                RecordProperty("p4a_seam_white_box", "declined");
                RecordProperty("p4a_seam_white_box_reason", "renderbuffer re-storage: pre-handle arm (D-D2)");
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteRenderbuffers(1, &renderbuffer);
            glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // The mirror direction, four channels -> three, and it needs the driver to READ the stored
        // alpha because the readback fixup (which consults the frontend) would hide it: after the
        // respecify to SRGB8 the widening discipline has to hold - the clear puts 1.0 into the
        // carrier's alpha and the draw is masked away from it - so a GL_DST_ALPHA blend of white sees
        // 1.0. On a stale record the draw wrote its 0.25 into the carrier and the blend saw that.
        TEST_F(P4aSeamAuditScenario, ATextureRespecifiedToThreeChannelsWhileAttachedReachesTheFramebufferRecord) {
            if (!Ready()) return;
            SkipUnlessEspryt("F-3");
            if (IsSkipped()) return;

            std::string error;
            const GLuint program = CompileProgram(kQuadVS, kColorFS, &error);
            ASSERT_NE(program, 0u) << error;
            const GLint colorLocation = glGetUniformLocation(program, "uColor");
            ASSERT_GE(colorLocation, 0);

            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ASSERT_EQ(FirstGLError(), 0u) << "the SRGB8_ALPHA8 texture was refused";

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            glViewport(0, 0, kSize, kSize);
            glUseProgram(program);

            // Phase 1: four channels, the application owns alpha.
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform4f(colorLocation, 0.0f, 1.0f, 0.0f, 0.25f);
            DrawQuad();
            float pixel[4];
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            ReadPixelFloat(kSize / 2, kSize / 2, pixel);
            EXPECT_NEAR(pixel[3], 0.25f, 0.02f) << "a four-channel attachment stores the draw's alpha";

            // Phase 2: THE RESPECIFY to three channels, while attached.
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8, kSize, kSize, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            ASSERT_EQ(FirstGLError(), 0u) << "the respecify to SRGB8 was refused";
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

            glDisable(GL_BLEND);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform4f(colorLocation, 0.0f, 1.0f, 0.0f, 0.25f);
            DrawQuad();
            // dst = stored alpha; src factor GL_DST_ALPHA, dst factor GL_ZERO, source white =>
            // the colour becomes (storedAlpha, storedAlpha, storedAlpha) - ThreeChannelAttachment's
            // own probe, which nothing on the readback path can doctor.
            glEnable(GL_BLEND);
            glBlendFunc(GL_DST_ALPHA, GL_ZERO);
            glUniform4f(colorLocation, 1.0f, 1.0f, 1.0f, 1.0f);
            DrawQuad();
            glDisable(GL_BLEND);
            ReadPixelFloat(kSize / 2, kSize / 2, pixel);
            EXPECT_NEAR(pixel[0], 1.0f, 0.05f)
                << "GL_DST_ALPHA read the stored alpha of a three-channel attachment and it was not "
                   "1.0: the framebuffer record (handle arm) or the FBO twin's memo (pre-handle arm) "
                   "still describes the four-channel storage the texture was attached with, so the "
                   "draw was let write alpha (F-3, mirror)";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &texture);
            glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // -----------------------------------------------------------------------------------
        // F-1 / F-1b: a program switch re-resolves the view set, and the texture sync list with it
        // -----------------------------------------------------------------------------------
        //
        // The sequence the audit named, and every step of it is ordinary: two programs sampling two
        // different units, a texture bound to a unit's EMPTY 2D slot - the unit was already touched
        // through another target, so the high-water mark does not move - while a program that does
        // not sample it is in use, then the switch to the one that does. Nothing between the two
        // draws touches a parameter, a level or a populated slot, which is exactly what leaves the
        // record epoch - and the program-independent texture sync list keyed on it - unmoved on
        // the tree the audit read: the second program sampled an unbound unit and drew black.
        TEST_F(P4aSeamAuditScenario, ATextureBoundToAnEmptySlotUnderOneProgramIsSampledByTheNext) {
            if (!Ready()) return;

            std::string error;
            const GLuint first = CompileProgram(kQuadVS, kFetchFS, &error);
            ASSERT_NE(first, 0u) << error;
            const GLuint second = CompileProgram(kQuadVS, kFetchFS, &error);
            ASSERT_NE(second, 0u) << error;
            glUseProgram(first);
            glUniform1i(glGetUniformLocation(first, "uTex"), 0);
            glUseProgram(second);
            glUniform1i(glGetUniformLocation(second, "uTex"), 1);
            glUseProgram(0);

            // Every texture exists, complete, with its parameters set, BEFORE the first draw: a
            // parameter or a level defined between the two draws would move the sampling-resolution
            // generation and rescue the list by accident.
            const GLuint red = MakeSolidTexture2D(255, 0, 0);
            const GLuint green = MakeSolidTexture2D(0, 255, 0);
            GLuint touch3D = 0;
            glGenTextures(1, &touch3D);
            glBindTexture(GL_TEXTURE_3D, touch3D);
            const std::uint8_t blue[2 * 2 * 2 * 4] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
                                                     0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
            glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_3D, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            ASSERT_EQ(FirstGLError(), 0u) << "texture setup left a GL error behind";

            ColorFbo target = MakeColorFbo(kSize, kSize);
            ASSERT_NE(target.fbo, 0u);
            BindFbo(target);
            glBindVertexArray(m_vao);

            // Unit 1 is TOUCHED through its 3D slot; its 2D slot stays empty. Unit 0 holds red.
            // The first program is in use BEFORE the first verb (the clear), so the very first
            // view set that goes out is already resolved for it - measured: with no program in
            // use at the clear the first set is [null, null], and the bind below then re-resolves
            // to [red, null], a DIFFERENT set that moves the serial and rescues the case by
            // accident.
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_3D, touch3D);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, red);
            glUseProgram(first);
            ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
            DrawQuad();

            // THE BIND ONTO THE EMPTY SLOT, under a program that does not sample unit 1, and a
            // draw with THAT program so the bind's own re-resolution of the view set happens under
            // it (the bind generation fires bit 12 at the next verb; a switch inside the same verb
            // gap would let that fire resolve under the second program by accident) ...
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, green);
            glActiveTexture(GL_TEXTURE0);
            DrawQuad();
            // ... and THE SWITCH to the one that does sample it. No other state moves.
            glUseProgram(second);
            DrawQuad();
            EXPECT_EQ(FirstGLError(), 0u) << "the two draws left a GL error behind";

            const Image image = ReadPixels(kSize, kSize);
            ASSERT_FALSE(image.Empty());
            EXPECT_TRUE(RegionIsMostly(image, kInset, kSize - 1 - kInset, kInset, kSize - 1 - kInset, "green", 0.0,
                                       "the draw after the program switch"))
                << "black means the second program sampled an unbound unit: the texture bound to the "
                   "empty slot was never synced because the view set - and E's record epoch with it - "
                   "did not move on the program switch (F-1 / F-1b); red means the first program's "
                   "set was still in force";

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_3D, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(0);
            DestroyColorFbo(target);
            glDeleteTextures(1, &red);
            glDeleteTextures(1, &green);
            glDeleteTextures(1, &touch3D);
            glDeleteProgram(first);
            glDeleteProgram(second);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // -----------------------------------------------------------------------------------
        // F-2 / SD-4: the image window follows the program, through buffer images
        // -----------------------------------------------------------------------------------
        //
        // Public-GL half: both dispatches store what they should (every arm passes this - the
        // server's window/high-water union takes the pre-handle bind for a unit the record does
        // not cover, which is exactly why the seam was silent). White-box half, on Espryt's handle
        // arm: after the first dispatch set_shader_images must have arrived with a window of ONE
        // unit (SD-4: on the tree the audit read a buffer image never reached the record at all -
        // the null -> program transition moved nothing bit 14 read), and after the switch to the
        // program naming two units the window must be TWO (F-2: the two programs' image-unit
        // counters are equal, so the switch alone moved nothing either).
        TEST_F(P4aSeamAuditScenario, AProgramSwitchWithEqualImageUnitCountersMovesTheImageWindow) {
            if (!Ready()) return;
            if (!ComputeImagesAreUsable()) GTEST_SKIP() << "no compute image units / buffer textures on this host";

            std::string error;
            const GLuint one = MakeComputeProgram(kOneBufferImageCS, &error);
            ASSERT_NE(one, 0u) << error;
            const GLuint two = MakeComputeProgram(kTwoBufferImagesCS, &error);
            ASSERT_NE(two, 0u) << error;

            GLuint buffer0 = 0;
            GLuint buffer1 = 0;
            const GLuint image0 = MakeBufferTexture(&buffer0, 0u);
            const GLuint image1 = MakeBufferTexture(&buffer1, 0u);
            ASSERT_EQ(FirstGLError(), 0u) << "buffer texture setup left a GL error behind";

            // Both units bound BEFORE any dispatch, so the bind generation does not move between
            // the two dispatches and the only thing that changes is the program in use.
            glBindImageTexture(0, image0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
            glBindImageTexture(1, image1, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
            ASSERT_EQ(FirstGLError(), 0u) << "binding the buffer images left a GL error behind";

            const bool whiteBox = SamplerHandleArmIsLive("F-2 / SD-4");

            glUseProgram(one);
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            EXPECT_EQ(FirstGLError(), 0u) << "the first dispatch leaked a GL error";
            if (whiteBox) {
                PipeShaderImageWindowPeek window{};
                ASSERT_TRUE(PeekPipeShaderImageWindow(&window));
                EXPECT_EQ(window.Start, 0u);
                EXPECT_EQ(window.Count, 1u)
                    << "set_shader_images never arrived for a program whose only image is a BUFFER "
                       "image (SD-4): the null -> program transition moved nothing bit 14 read";
            }

            glUseProgram(two);
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            EXPECT_EQ(FirstGLError(), 0u) << "the second dispatch leaked a GL error";
            if (whiteBox) {
                PipeShaderImageWindowPeek window{};
                ASSERT_TRUE(PeekPipeShaderImageWindow(&window));
                EXPECT_EQ(window.Start, 0u);
                EXPECT_EQ(window.Count, 2u)
                    << "the image window did not follow the program switch: two programs with equal "
                       "image-unit counters, and bit 14 mixed only the counter (F-2)";
            }

            EXPECT_EQ(ReadBufferTexel0(buffer0), 7u) << "the first program's store did not land";
            EXPECT_EQ(ReadBufferTexel0(buffer1), 9u) << "the second program's store did not land";

            glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
            glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
            glBindBuffer(GL_TEXTURE_BUFFER, 0);
            glBindTexture(GL_TEXTURE_BUFFER, 0);
            glUseProgram(0);
            glDeleteProgram(one);
            glDeleteProgram(two);
            glDeleteTextures(1, &image0);
            glDeleteTextures(1, &image1);
            glDeleteBuffers(1, &buffer0);
            glDeleteBuffers(1, &buffer1);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // -----------------------------------------------------------------------------------
        // F-4: the unit's driver sampler is the CSO's own twin on the handle arm
        // -----------------------------------------------------------------------------------
        //
        // Public-GL half: a glBindSampler'd object whose wrap differs from the texture's built-in
        // sampler wins (GL 4.6 core 8.10) - every arm passes this, because the pre-handle program
        // pass bound the object through its identity twin. White-box half, on Espryt's handle arm:
        // the sampler the unit carries on the driver must be the twin Espryt holds AT THE CSO
        // HANDLE bind_sampler_states named for the unit. On the tree the audit read that twin did
        // not exist - the handle is content-addressed, the registry's twins were minted off
        // lifetime ids - so the record arm bound nothing on every draw.
        TEST_F(P4aSeamAuditScenario, ABoundSamplerObjectIsDrivenThroughItsCsoTwinOnTheHandleArm) {
            if (!Ready()) return;

            std::string error;
            const GLuint program = CompileProgram(kQuadVS, kOutsideSampleFS, &error);
            ASSERT_NE(program, 0u) << error;

            // The texture's built-in sampler REPEATS, so (1.5, 1.5) reads the red texel through it;
            // the sampler object CLAMPS TO A WHITE BORDER, so the same coordinate reads white
            // through it. White is a Vulkan palette border colour, so Magma needs no extension.
            const GLuint red = MakeSolidTexture2D(255, 0, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            GLuint sampler = 0;
            glGenSamplers(1, &sampler);
            glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            const GLfloat white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, white);
            ASSERT_EQ(FirstGLError(), 0u) << "sampler setup left a GL error behind";

            ColorFbo target = MakeColorFbo(kSize, kSize);
            ASSERT_NE(target.fbo, 0u);
            BindFbo(target);
            glBindVertexArray(m_vao);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, red);
            glBindSampler(0, sampler);
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "uTex"), 0);
            ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
            DrawQuad();
            EXPECT_EQ(FirstGLError(), 0u) << "the draw left a GL error behind";

            const Image image = ReadPixels(kSize, kSize);
            ASSERT_FALSE(image.Empty());
            EXPECT_TRUE(RegionIsMostly(image, kInset, kSize - 1 - kInset, kInset, kSize - 1 - kInset, "white", 0.0,
                                       "the draw through the bound sampler object"))
                << "red means the texture's own REPEAT sampler applied instead of the bound object's "
                   "CLAMP_TO_BORDER";

            if (SamplerHandleArmIsLive("F-4")) {
                EsprytUnitSamplerPeek peek{};
                ASSERT_TRUE(PeekEsprytUnitSampler(0, sampler, &peek));
                std::cout << "[ P4aSeamAudit ] white-box: unit 0 driver sampler " << peek.BoundSamplerId
                          << ", bind_sampler_states handle {" << peek.CsoHandleSlot << ", " << peek.CsoHandleGen
                          << "} inside window " << (peek.UnitInsideWindow ? "yes" : "no") << ", CSO twin "
                          << peek.CsoTwinSamplerId << ", identity twin " << peek.IdentityTwinSamplerId << std::endl;
                EXPECT_TRUE(peek.UnitInsideWindow) << "bind_sampler_states did not describe unit 0";
                EXPECT_NE(peek.CsoHandleSlot, 0u) << "bind_sampler_states names no CSO for a unit that carries "
                                                     "a sampler object";
                EXPECT_NE(peek.CsoTwinSamplerId, 0u)
                    << "Espryt holds no twin at the CSO handle bind_sampler_states named: the record arm's "
                       "lookup went to the identity-keyed registry with a content-addressed handle and "
                       "could never hit (F-4)";
                EXPECT_EQ(peek.BoundSamplerId, peek.CsoTwinSamplerId)
                    << "the driver sampler on unit 0 is not the CSO's twin, so it was put there by the "
                       "pre-handle program pass and not by the record arm (F-4)";
                EXPECT_EQ(peek.IdentityTwinSamplerId, 0u)
                    << "an identity-keyed twin was minted for the sampler object on the handle arm: the "
                       "pre-handle pass is still the one doing the binding";
            }

            glBindSampler(0, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(0);
            DestroyColorFbo(target);
            glDeleteSamplers(1, &sampler);
            glDeleteTextures(1, &red);
            glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

    } // namespace
} // namespace MGITest
